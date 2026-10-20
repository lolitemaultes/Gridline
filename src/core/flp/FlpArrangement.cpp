// Gridline — FLP import: arrangement. Playlist clips (pattern, audio and
// automation), audio-clip and automation channels, automation targets,
// playlist tracks, time markers.
//
// Layouts (observed on the shipped demo projects, see analysis/demos/notes_alpha.md):
//   233 playlist items, 32/60/80-byte records: u32 position, u16 base (0x5000),
//       u16 item (> base: pattern item − base, else rack channel), u32 length,
//       u16 499 − track, u16 group, u16 ?, u16 flags (0x2000 = muted), …,
//       @24/@28 start/end offset: i32 ticks for pattern items, f32 for channel
//       items (audio: milliseconds of the sample; automation: beats); −1 = none.
//       80-byte records add, for audio clips: f32 fade in (ms) @36, f32 fade-in
//       tension @40, f32 fade out (ms) @44, f32 pitch (cents) @48, f32 gain @52,
//       f64 stretch factor @64.
//   234 automation points: see FlpAutomation.cpp.
//   227 automation link: u16 source channel @2, u32 target @8.
//       Target: 0x40000005 tempo; < 0x2000 << 16: channel (high word) +
//       parameter (low word: 0 volume, 1 pan, 4 pitch, 0x10 pan offset,
//       0x11 volume offset, 0x8000 + n generator parameter n);
//       ≥ 0x2000 << 16: mixer, high word − base (0x2000, FL 2025: 0x7000) =
//       insert × 64 + slot; low word 0x8000 + n effect parameter n, 0x1F01
//       slot mix, 0x1FC0 / 0x1FC1 / 0x1FC2 insert volume / pan / separation.
//   238 track: u32 index (1-based), u32 colour, u32 icon, u8 enabled, f32 height;
//       239 name follows its track record.
//   148 marker position (high byte: kind, 8 = time signature), 33/34 time
//       signature, 205 name.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <set>
#include "../Params.h"
#include "../PluginParams.h"
#include "Flp.h"
#include "FlpFx.h"

namespace gl::flp {

namespace {
Rgb flColor(uint32_t v) { return ((v & 0xFF) << 16) | (v & 0xFF00) | ((v >> 16) & 0xFF); }

// Playlist item record size by FL version (80 / 60 / 32 bytes).
uint32_t itemStride(const Event& e) {
  for (uint32_t k : {80u, 60u, 32u}) {
    if (e.size % k) continue;
    bool ok = true;
    for (uint32_t o = 0; o + k <= e.size && ok; o += k) ok = e.u16(o + 4) == 0x5000;
    if (ok) return k;
  }
  return 0;
}

std::string channelName(const std::vector<Event>& evs, int idx) {
  std::string n;
  if (const Event* e = find(evs, 203)) n = e->text();
  if (n.empty())
    if (const Event* e = find(evs, 192)) n = e->text();
  return n.empty() ? "Channel " + std::to_string(idx + 1) : n;
}

// A resolved automation target: the Gridline parameter, the clip range
// (normalized), and an optional per-value conversion from the FL value.
struct Target {
  ParamId id;
  float min = 0, max = 1;
  std::function<double(double)> convert;  // FL value (0..1) → Gridline normalized value
  std::string what;                       // description for the report
};

class Importer {
 public:
  explicit Importer(Context& c) : c_(c), s_(c.song) {}
  void run();
  bool resolveTarget(uint32_t target, Target& t);

 private:
  Context& c_;
  Song& s_;
  int arr_ = 0;
  std::map<int, ObjectId> audioChannels_;  // FL channel → Gridline channel (audio clip channels)
  std::map<int, ObjectId> automations_;    // FL channel → AutomationClip
  std::map<int, std::string> autoNames_;
  std::map<ObjectId, int> channelCents_;   // audio clip channel → pitch knob (cents), applied per clip

  void importAudioChannels();
  void importAutomationChannels();
  void importTargets();
  void importRemoteLinks();
  void importTracksAndClips();
  void importMarkers();
  ObjectId channelId(int flIndex) const {
    if (auto it = audioChannels_.find(flIndex); it != audioChannels_.end()) return it->second;
    if (auto it = c_.channelIds.find(flIndex); it != c_.channelIds.end()) return it->second;
    return kNoId;
  }
};

void Importer::run() {
  importAudioChannels();
  importAutomationChannels();
  importTargets();
  importRemoteLinks();
  if (c_.sec.arrangements.empty()) return;
  // Every arrangement is imported; the current one (event 100, else the
  // first) is imported last so the report counts describe it.
  int current = c_.sec.arrangements.begin()->first;
  if (const Event* e = find(c_.sec.project, 100); e && c_.sec.arrangements.count(static_cast<int>(e->value())))
    current = static_cast<int>(e->value());
  std::map<int, Arrangement> done;
  auto importOne = [&](int idx) {
    arr_ = idx;
    s_.playlist.clear();
    s_.markers.clear();
    s_.live = LiveSettings{};
    importTracksAndClips();
    importMarkers();
    Arrangement a;
    if (const Event* n = find(c_.sec.arrangements[idx], 241); n && !n->text().empty()) a.name = n->text();
    a.playlist = std::move(s_.playlist);
    a.tracks = std::move(s_.tracks);
    a.markers = std::move(s_.markers);
    a.live = s_.live;
    done[idx] = std::move(a);
  };
  for (const auto& [idx, evs] : c_.sec.arrangements)
    if (idx != current) importOne(idx);
  importOne(current);
  s_.arrangements.clear();
  for (auto& [idx, a] : done) {
    if (idx == current) {
      s_.currentArrangement = static_cast<int>(s_.arrangements.size());
      s_.playlist = std::move(a.playlist);
      s_.tracks = std::move(a.tracks);
      s_.markers = std::move(a.markers);
      s_.live = a.live;
      a.playlist.clear();
      a.tracks.clear();
      a.markers.clear();
      a.live = LiveSettings{};
    }
    s_.arrangements.push_back(std::move(a));
  }
  if (s_.arrangements.size() == 1 && s_.arrangements[0].name == "Arrangement") s_.arrangements.clear();  // the default single arrangement
  if (done.size() > 1) c_.report.counts["arrangements"] = static_cast<int>(done.size());
}

// FL "audio clip" channels (type 4) → sampler channels whose sample the
// playlist's audio clips play, through the channel strip and its insert.
void Importer::importAudioChannels() {
  int made = 0;
  for (const auto& [idx, evs] : c_.sec.channels) {
    auto ty = c_.channelTypes.find(idx);
    if (ty == c_.channelTypes.end() || ty->second != 4) continue;
    ObjectId id = kNoId;
    if (auto it = c_.channelIds.find(idx); it != c_.channelIds.end()) id = it->second;  // already in the rack
    if (id == kNoId) {
      Channel ch;
      ch.id = s_.allocateId();
      ch.name = channelName(evs, idx);
      if (const Event* e = find(evs, 128)) ch.color = flColor(e->value());
      ch.plugin = defaultPluginState("gl.sampler");
      if (const Event* e = find(evs, 196)) ch.plugin.data = c_.resolvePath(e->text());
      const Event* ins = find(evs, 22);
      if (!ins) ins = find(evs, 104);  // FL 2025 stores the insert here
      ch.mixerTrack = s_.masterId();
      if (ins)
        if (auto it = c_.insertIds.find(static_cast<int>(ins->value())); it != c_.insertIds.end()) ch.mixerTrack = it->second;
      if (const Event* e = find(evs, 219); e && e->size >= 12) {  // levels: pan, volume, pitch (cents)
        ch.pan = std::clamp(static_cast<float>(e->i32(0) - 6400) / 6400.0f, -1.0f, 1.0f);
        ch.gain = std::clamp(static_cast<float>(e->i32(4)) / 12800.0f, 0.0f, 1.25f);
      }
      // Rack order: before the first channel with a higher FL index.
      size_t pos = s_.channels.size();
      for (const auto& [fi, gid] : c_.channelIds)
        if (fi > idx) {
          int k = s_.channelIndex(gid);
          if (k >= 0) pos = std::min(pos, static_cast<size_t>(k));
        }
      s_.channels.insert(s_.channels.begin() + static_cast<std::ptrdiff_t>(pos), ch);
      id = ch.id;
      ++made;
    }
    Channel* ch = s_.channel(id);
    if (!ch) continue;
    if (ch->plugin.type != "gl.sampler") ch->plugin = defaultPluginState("gl.sampler");
    if (ch->plugin.data.empty())
      if (const Event* e = find(evs, 196)) ch->plugin.data = c_.resolvePath(e->text());
    // Sampler: pitch shift (cents), stretch mode, time multiplier, reverse.
    auto& prm = ch->plugin.params;
    auto set = [&](int k, float v) {
      if (static_cast<size_t>(k) < prm.size()) prm[static_cast<size_t>(k)] = v;
    };
    if (const Event* e = find(evs, 215); e && e->size >= 112) {
      int cents = e->i32(100), mul = e->i32(104), mode = e->i32(108);
      set(pp::SmpPitchShift, static_cast<float>(std::clamp(cents, -1200, 1200)));
      set(pp::SmpStretchMode, mode != 0 ? 1.f : 0.f);  // 0 = resample; others keep the pitch apart from the speed
      if (mul != 0) set(pp::SmpTimeStretch, static_cast<float>(std::clamp(std::pow(2.0, mul / 10000.0), 0.25, 4.0)));
    }
    if (const Event* e = find(evs, 70)) set(pp::SmpReverse, (e->value() & 2) ? 1.f : 0.f);
    if (const Event* e = find(evs, 219); e && e->size >= 12 && e->i32(8) != 0) channelCents_[id] = e->i32(8);
    audioChannels_[idx] = id;
    c_.channelIds[idx] = id;
  }
  if (made) c_.report.counts["audio clip channels"] = made;
}

// FL automation channels (type 5) → automation clips.
void Importer::importAutomationChannels() {
  for (const auto& [idx, evs] : c_.sec.channels) {
    auto ty = c_.channelTypes.find(idx);
    if (ty == c_.channelTypes.end() || ty->second != 5) continue;
    const Event* e = find(evs, 234);
    AutomationClip a;
    a.id = s_.allocateId();
    a.name = channelName(evs, idx);
    if (const Event* col = find(evs, 128)) a.color = flColor(col->value());
    if (e) decodeAutomationPoints(*e, s_.ppq, a.points);
    if (a.points.empty()) a.points.push_back(AutoPoint{});
    automations_[idx] = a.id;
    c_.automationIds[idx] = a.id;
    autoNames_[idx] = a.name;
    s_.automations.push_back(std::move(a));
  }
  if (!automations_.empty()) c_.report.counts["automation clips (contents)"] = static_cast<int>(automations_.size());
}

bool Importer::resolveTarget(uint32_t target, Target& t) {
  const uint32_t hi = target >> 16, lo = target & 0xFFFF;
  if (target == 0x40000005u) {  // tempo; FL's tempo clips span 60 … 180 BPM
    t.id = songParam(SongParam::Tempo);
    auto d = describeParam(s_, t.id);
    t.min = static_cast<float>(d->toNormalized(60.0));
    t.max = static_cast<float>(d->toNormalized(180.0));
    t.what = "tempo";
    return true;
  }
  if (hi < 0x2000) {  // channel parameter
    int fl = static_cast<int>(hi);
    ObjectId ch = channelId(fl);
    t.what = "channel " + std::to_string(fl + 1);
    if (ch == kNoId) return false;
    if (lo >= 0x8000) {  // generator parameter
      auto pm = c_.channelPlugins.find(fl);
      t.what += " plugin parameter " + std::to_string(lo - 0x8000);
      if (pm == c_.channelPlugins.end() || !pm->second.param) return false;
      int k = pm->second.param(static_cast<int>(lo - 0x8000));
      if (k < 0) return false;
      t.id = channelPluginParam(ch, k);
      auto d = describeParam(s_, t.id);
      if (!d) return false;
      if (pm->second.value) {
        auto conv = pm->second.value;
        int flp = static_cast<int>(lo - 0x8000);
        ParamDesc desc = *d;
        t.convert = [conv, flp, desc](double v) { return desc.toNormalized(desc.clamp(conv(flp, v))); };
      }
      return true;
    }
    switch (lo) {
      case 0x00: case 0x11: t.id = channelParam(ch, ChannelParam::Volume), t.max = 0.8f; return true;  // FL 0..1 → gain 0..1 (of 1.25)
      case 0x01: case 0x10: t.id = channelParam(ch, ChannelParam::Pan); return true;
      case 0x04: {  // pitch knob, ±2 semitones
        t.id = channelParam(ch, ChannelParam::Pitch);
        auto d = describeParam(s_, t.id);
        t.min = static_cast<float>(d->toNormalized(-2.0));
        t.max = static_cast<float>(d->toNormalized(2.0));
        return true;
      }
      default: t.what += " setting 0x" + [&] { char b[8]; std::snprintf(b, sizeof b, "%x", lo); return std::string(b); }(); return false;
    }
  }
  // Mixer: insert × 64 + slot above a version-dependent base.
  uint32_t rel = hi - (hi >= 0x7000 ? 0x7000 : 0x2000);
  int ins = static_cast<int>(rel >> 6), slot = static_cast<int>(rel & 0x3F);
  auto tid = c_.insertIds.find(ins);
  t.what = "insert " + std::to_string(ins);
  if (tid == c_.insertIds.end()) return false;
  ObjectId track = tid->second;
  if (lo >= 0x8000) {  // effect parameter
    auto pm = c_.slotPlugins.find({ins, slot});
    t.what += " slot " + std::to_string(slot + 1) + " parameter " + std::to_string(lo - 0x8000);
    if (pm == c_.slotPlugins.end() || !pm->second.param) {
      if (pm != c_.slotPlugins.end()) t.what += " (" + pm->second.flName + ")";
      return false;
    }
    int k = pm->second.param(static_cast<int>(lo - 0x8000));
    if (k < 0) return false;
    t.id = slotParam(track, slot, k);
    auto d = describeParam(s_, t.id);
    if (!d) return false;
    if (pm->second.value) {
      auto conv = pm->second.value;
      int flp = static_cast<int>(lo - 0x8000);
      ParamDesc desc = *d;
      t.convert = [conv, flp, desc](double v) { return desc.toNormalized(desc.clamp(conv(flp, v))); };
    }
    return true;
  }
  switch (lo) {
    case 0x1F01: t.id = slotMixParam(track, slot); return describeParam(s_, t.id).has_value();
    case 0x1FC0: t.id = mixerParam(track, MixerParam::Volume), t.max = 0.8f; return true;  // FL 0..1 → 0..1 (of 1.25)
    case 0x1FC1: t.id = mixerParam(track, MixerParam::Pan); return true;
    case 0x1FC2: t.id = mixerParam(track, MixerParam::StereoSep); return true;
    case 0x1F00: t.id = slotEnableParam(track, slot); return describeParam(s_, t.id).has_value();
    default: break;
  }
  {
    // Strip EQ: gain 0x1FD0-2 (±18 dB, centre = 0), frequency 0x1FD8-A and
    // bandwidth 0x1FE0-2 (16-bit words, see FlpFx.h).
    auto eqTarget = [&](uint32_t first, uint32_t base, std::function<double(double)> toUnits) {
      t.id = mixerParam(track, static_cast<MixerParam>(first + (lo - base)));
      auto d = describeParam(s_, t.id);
      if (!d) return false;
      ParamDesc desc = *d;
      t.convert = [desc, toUnits](double v) { return desc.toNormalized(desc.clamp(toUnits(v))); };
      return true;
    };
    if (lo >= 0x1FD0 && lo <= 0x1FD2)
      return eqTarget(static_cast<uint32_t>(MixerParam::EqLowGain), 0x1FD0, [](double v) { return (v - 0.5) * 36.0; });
    if (lo >= 0x1FD8 && lo <= 0x1FDA)
      return eqTarget(static_cast<uint32_t>(MixerParam::EqLowFreq), 0x1FD8, [](double v) { return fx::flEqFreqHz(v * 65536.0); });
    if (lo >= 0x1FE0 && lo <= 0x1FE2)
      return eqTarget(static_cast<uint32_t>(MixerParam::EqLowQ), 0x1FE0, [](double v) { return fx::flInsertEqQ(v * 65536.0); });
    // Send (route) level to insert k: 0x1F40 + k (FL ≤ 21 layout); 0..1 = 0..100 %.
    if (lo >= 0x1F40 && lo < 0x1FC0) {
      t.what += " send to insert " + std::to_string(lo - 0x1F40);
      auto dest = c_.insertIds.find(static_cast<int>(lo - 0x1F40));
      int di = dest == c_.insertIds.end() ? -1 : s_.mixerIndex(dest->second);
      if (di < 0) return false;
      t.id = sendParam(track, di);
      auto d = describeParam(s_, t.id);
      if (!d) return false;
      ParamDesc desc = *d;
      t.convert = [desc](double v) { return desc.toNormalized(desc.clamp(v)); };
      return true;
    }
    t.what += " setting";
    return false;
  }
}

// 227 records link automation channels to their targets.
void Importer::importTargets() {
  int ok = 0, bad = 0;
  std::set<int> linked;
  int ctlOk = 0, ctlBad = 0;
  for (const auto& e : c_.file.events) {
    if (e.id != 227 || e.size < 12) continue;
    int src = e.u16(2);
    if (e.u16(0) & 0x8000) {  // internal controller output (0x8000 + output) → parameter
      ModLink ml;
      ml.sourceOutput = std::clamp(e.u16(0) & 0x7FFF, 0, kMaxControllerOutputs - 1);
      if (src < 0x2000) {
        ml.sourceOwner = channelId(src);
      } else {
        uint32_t rel = static_cast<uint32_t>(src) - (src >= 0x7000 ? 0x7000u : 0x2000u);
        auto tid = c_.insertIds.find(static_cast<int>(rel >> 6));
        ml.sourceOwner = tid == c_.insertIds.end() ? kNoId : tid->second;
        ml.sourceSlot = static_cast<int>(rel & 0x3F);
      }
      Target t;
      if (ml.sourceOwner == kNoId || !resolveTarget(e.u32(8), t) || t.id.owner == kSongId || findModLink(s_, t.id.owner, t.id.index)) {
        ++ctlBad;
        continue;
      }
      ml.owner = t.id.owner;
      ml.index = t.id.index;
      ml.min = t.min;
      ml.max = t.max;
      s_.modLinks.push_back(ml);  // kept even if the source plugin has no Gridline controller yet (ignored then)
      ++ctlOk;
      continue;
    }
    auto it = automations_.find(src);
    if (it == automations_.end()) continue;
    AutomationClip* a = s_.automation(it->second);
    if (!a || linked.count(src)) continue;  // one target per clip (FL can link more; first wins)
    linked.insert(src);
    c_.automationTargets[src] = e.u32(8);
    Target t;
    if (!resolveTarget(e.u32(8), t)) {
      ++bad;
      c_.report.warn("automation \"" + a->name + "\": target not mapped (" + (t.what.empty() ? "unknown" : t.what) + ")");
      continue;
    }
    a->owner = t.id.owner;
    a->index = t.id.index;
    a->min = std::clamp(t.min, 0.0f, 1.0f);
    a->max = std::clamp(t.max, 0.0f, 1.0f);
    if (t.convert) {
      for (auto& p : a->points) p.value = static_cast<float>(std::clamp(t.convert(p.value), 0.0, 1.0));
      a->min = 0, a->max = 1;
    }
    ++ok;
  }
  for (const auto& [fl, id] : automations_)
    if (!linked.count(fl)) {
      ++bad;
      if (const AutomationClip* a = s_.automation(id)) c_.report.warn("automation \"" + a->name + "\" has no target");
    }
  if (ok) c_.report.counts["automation targets resolved"] = ok;
  if (bad) c_.report.counts["automation targets not mapped"] = bad;
  if (ctlOk) c_.report.counts["controller links"] = ctlOk;
  if (ctlBad) c_.report.counts["controller links not mapped"] = ctlBad;
}

// 226 records: MIDI remote-control links → Song::links.
void Importer::importRemoteLinks() {
  int ok = 0, focused = 0, bad = 0;
  for (const auto& e : c_.file.events) {
    FlRemoteLink r;
    if (e.id != 226 || !decodeRemoteLink(e, r) || !r.midi()) continue;
    if (r.focused()) {  // follows the focused channel / track: Gridline links have fixed targets
      ++focused;
      continue;
    }
    Target t;
    FlTarget ft;
    ControllerLink l;
    if (!resolveTarget(r.target, t)) {
      ++bad;
      continue;
    }
    ft.id = t.id, ft.min = t.min, ft.max = t.max, ft.convert = t.convert, ft.what = t.what;
    if (!remoteLinkToController(r, ft, l)) {
      ++bad;
      continue;
    }
    bool dup = false;
    for (const auto& x : s_.links) dup = dup || x == l;
    if (!dup) s_.links.push_back(l), ++ok;
  }
  if (ok) c_.report.counts["MIDI remote links"] = ok;
  if (focused) c_.report.counts["MIDI remote links to the focused channel (not imported)"] = focused;
  if (bad) c_.report.counts["MIDI remote links not mapped"] = bad;
}

void Importer::importTracksAndClips() {
  const auto& evs = c_.sec.arrangements[arr_];
  // Tracks: 238 record (+ 239 name until the next record).
  std::vector<PlaylistTrack> tracks(static_cast<size_t>(kMaxPlaylistTracks));
  std::map<int, bool> grouped;  // track → grouped under the nearest ungrouped track above
  int lastNamed = -1, cur = -1, liveTracks = 0;
  for (const auto& e : evs) {
    if (e.id == 238 && e.size >= 17) {
      int idx = static_cast<int>(e.u32(0)) - 1;
      cur = idx >= 0 && idx < kMaxPlaylistTracks ? idx : -1;
      if (cur < 0) continue;
      PlaylistTrack& t = tracks[static_cast<size_t>(cur)];
      t.color = flColor(e.u32(4));
      t.muted = e.u8(12) == 0;
      float h = e.f32(13);
      t.height = std::isfinite(h) ? std::clamp(h, 0.25f, 4.0f) : 1.0f;
      if (t.muted || t.height != 1.0f) lastNamed = std::max(lastNamed, cur);
      // Live (performance) settings @22: u32 motion, press, trigger sync,
      // queue, tolerant, position sync; @46 u8 "grouped under the track above".
      if (e.size >= 46) {
        static const FollowMode kFollow[7] = {FollowMode::Loop, FollowMode::Once, FollowMode::Next, FollowMode::NextHold,
                                              FollowMode::NextStop, FollowMode::Random, FollowMode::Shuffle};
        static const PadMode kPad[4] = {PadMode::Trigger, PadMode::Gate, PadMode::Gate, PadMode::Toggle};
        static const LaunchGrid kGrid[7] = {LaunchGrid::Now, LaunchGrid::Step, LaunchGrid::HalfBeat, LaunchGrid::Beat,
                                            LaunchGrid::TwoBeats, LaunchGrid::FourBeats, LaunchGrid::ClipLength};
        t.follow = kFollow[std::min<uint32_t>(e.u32(22), 6)];
        t.pad = kPad[std::min<uint32_t>(e.u32(26), 3)];
        t.launch = kGrid[std::min<uint32_t>(e.u32(30), 6)];
        t.syncStart = kGrid[std::min<uint32_t>(e.u32(42), 6)];
        if (!t.liveDefaults()) lastNamed = std::max(lastNamed, cur), ++liveTracks;
      }
      if (e.size > 46 && e.u8(46) && cur > 0) grouped[cur] = true;
    } else if (e.id == 239 && cur >= 0) {
      tracks[static_cast<size_t>(cur)].name = e.text();
      lastNamed = std::max(lastNamed, cur);
    }
  }
  // Grouped tracks: the nearest ungrouped track above holds the group.
  int groups = 0;
  for (int t = 0; t < kMaxPlaylistTracks; ++t) {
    if (grouped.count(t) || !grouped.count(t + 1)) continue;
    int n = 0;
    while (grouped.count(t + 1 + n)) ++n;
    tracks[static_cast<size_t>(t)].groupSize = n;
    lastNamed = std::max(lastNamed, t + n);
    ++groups;
  }
  // Track field = last track index − track. FL before 12.9 had 199 tracks.
  int trackBase = 499;
  if (c_.sec.legacyArrangement) {
    trackBase = 198;
    for (const auto& e : evs)
      if (uint32_t k = e.id == 233 ? itemStride(e) : 0)
        for (uint32_t o = 0; o + k <= e.size; o += k)
          if (e.u16(o + 12) > 198) trackBase = 499;
  }
  int lastUsed = -1;
  int patClips = 0, audioClips = 0, autoClips = 0, muted = 0, offsets = 0, other = 0;
  for (const auto& e : evs) {
    if (e.id != 233) continue;
    uint32_t k = itemStride(e);
    if (!k) {
      if (e.size) c_.report.warn("unknown playlist record size");
      continue;
    }
    for (uint32_t o = 0; o + k <= e.size; o += k) {
      Clip cl;
      cl.start = c_.tick(e.u32(o));
      cl.length = c_.tick(e.u32(o + 8));
      int base = e.u16(o + 4), item = e.u16(o + 6);
      cl.track = std::clamp(trackBase - static_cast<int>(e.u16(o + 12)), 0, kMaxPlaylistTracks - 1);
      cl.muted = (e.u16(o + 18) & 0x2000) != 0;
      if (item > base) {  // pattern item; offsets in ticks
        auto it = c_.patternIds.find(item - base);
        if (it == c_.patternIds.end()) continue;
        cl.kind = ClipKind::Pattern;
        cl.pattern = it->second;
        int32_t so = e.i32(o + 24);
        if (so > 0) cl.offset = c_.tick(so), ++offsets;
        ++patClips;
      } else {
        float so = e.f32(o + 24), eo = e.f32(o + 28);
        if (auto a = automations_.find(item); a != automations_.end()) {
          cl.kind = ClipKind::Automation;
          cl.source = a->second;
          if (std::isfinite(so) && so > 0) cl.offset = static_cast<Tick>(std::llround(static_cast<double>(so) * s_.ppq)), ++offsets;
          ++autoClips;
        } else if (auto ch = audioChannels_.find(item); ch != audioChannels_.end()) {
          cl.kind = ClipKind::Audio;
          cl.source = ch->second;
          if (std::isfinite(so) && so > 0) cl.audioStart = so / 1000.0;
          if (std::isfinite(eo) && eo > 0 && eo > so) cl.audioEnd = eo / 1000.0;
          if (k >= 80) {
            float fi = e.f32(o + 36), fo = e.f32(o + 44), pc = e.f32(o + 48), g = e.f32(o + 52);
            double st = e.f64(o + 64);
            if (std::isfinite(fi) && fi > 0) cl.fadeIn = fi / 1000.0;
            if (std::isfinite(fo) && fo > 0) cl.fadeOut = fo / 1000.0;
            if (std::isfinite(pc)) cl.pitch = std::clamp(pc, -4800.0f, 4800.0f);
            if (std::isfinite(g) && g >= 0) cl.gain = std::min(g, 16.0f);
            if (std::isfinite(st) && st > 0.0625 && st < 16) cl.stretch = st;
          }
          if (auto cc = channelCents_.find(ch->second); cc != channelCents_.end()) cl.pitch += static_cast<float>(cc->second);
          ++audioClips;
        } else {
          ++other;
          continue;
        }
      }
      if (cl.length <= 0) continue;
      cl.id = s_.allocateId();
      muted += cl.muted;
      s_.playlist.push_back(cl);
      lastUsed = std::max(lastUsed, cl.track);
    }
  }
  int nTracks = std::clamp(std::max({lastUsed + 1, lastNamed + 1, 8}) + 2, 1, kMaxPlaylistTracks);
  s_.tracks.assign(tracks.begin(), tracks.begin() + nTracks);
  c_.report.counts["pattern clips"] = patClips;
  if (audioClips) c_.report.counts["audio clips"] = audioClips;
  if (autoClips) c_.report.counts["automation clips"] = autoClips;
  if (muted) c_.report.counts["muted clips"] = muted;
  if (offsets) c_.report.counts["clips with a start offset"] = offsets;
  if (other) c_.report.counts["channel clips not imported"] = other;
  c_.report.counts["playlist tracks"] = nTracks;
  if (groups) c_.report.counts["playlist track groups"] = groups;
  if (liveTracks) c_.report.counts["tracks with live settings"] = liveTracks;
}

void Importer::importMarkers() {
  const auto& evs = c_.sec.arrangements[arr_];
  Marker* m = nullptr;
  for (const auto& e : evs) {
    switch (e.id) {
      case 148: {
        uint32_t v = e.value();
        Marker mk;
        mk.pos = c_.tick(v & 0x00FFFFFFu);
        if ((v >> 24) == 8) mk.timeSigNum = mk.timeSigDen = 4;  // time signature marker (values follow)
        // Kind 5 = song start: clips placed before it are launched by hand
        // (FL's performance zone) → Gridline's live area.
        if ((v >> 24) == 5 && mk.pos > 0) s_.live.areaEnd = mk.pos;
        s_.markers.push_back(mk);
        m = &s_.markers.back();
        break;
      }
      case 33:
        if (m && m->timeSigNum) m->timeSigNum = std::clamp(static_cast<int>(e.value()), 1, 32);
        break;
      case 34:
        if (m && m->timeSigNum) m->timeSigDen = std::clamp(static_cast<int>(e.value()), 1, 32);
        break;
      case 205:
        if (m) m->name = e.text();
        break;
      default: break;
    }
  }
  std::stable_sort(s_.markers.begin(), s_.markers.end(), [](const Marker& a, const Marker& b) { return a.pos < b.pos; });
  if (s_.live.areaEnd > 0) {
    bool any = false;
    for (const auto& cl : s_.playlist) any = any || cl.start < s_.live.areaEnd;
    // FL marks performance clips that are not playing as muted; in the live
    // area that is launch state, not a mute: the unmuted ones were playing
    // when the project was saved (offline renders launch them).
    for (auto& cl : s_.playlist)
      if (cl.start < s_.live.areaEnd) {
        cl.launched = !cl.muted;
        cl.muted = false;
      }
    if (!any) s_.live.areaEnd = 0;  // a start marker with nothing before it: plain song
    else c_.report.counts["live area clips"] = static_cast<int>(std::count_if(s_.playlist.begin(), s_.playlist.end(), [&](const Clip& cl) { return cl.start < s_.live.areaEnd; }));
  }
  if (!s_.markers.empty()) c_.report.counts["markers"] = static_cast<int>(s_.markers.size());
}

}  // namespace

void importArrangement(Context& c) {
  Importer imp(c);
  imp.run();
}

bool decodeRemoteLink(const Event& e, FlRemoteLink& out) {
  if (e.id != 226 || e.size < 20) return false;
  const uint32_t src = e.u32(0);
  out.port = static_cast<int>(src >> 22) - 1;
  out.channel = static_cast<int>((src >> 16) & 0xF);
  out.control = static_cast<int>(src & 0xFFFF);
  out.target = e.u32(8);
  out.flags = e.u32(12);
  out.noteMode = static_cast<int>((out.flags >> 8) & 0xFF);
  out.smoothing = e.i32(16);
  return true;
}

bool remoteLinkToController(const FlRemoteLink& r, const FlTarget& t, ControllerLink& out) {
  ControllerLink l;
  if (r.control >= 0 && r.control < 128) {
    l.source = RemoteSource::CC, l.cc = r.control;
  } else if (r.control == 253) {
    l.source = RemoteSource::ChannelPressure, l.cc = 0;
  } else if (r.control == 254) {
    l.source = RemoteSource::KeyPressure, l.cc = 0;
  } else if (r.control == 255) {
    l.source = RemoteSource::PitchBend, l.cc = 0;
  } else if (r.control >= 256 && r.control < 256 + 128) {
    l.source = RemoteSource::Note, l.cc = r.control - 256;
  } else {
    return false;
  }
  l.midiChannel = std::clamp(r.channel, 0, 15);
  l.port = r.port >= 0 ? r.port : -1;
  l.owner = t.id.owner;
  l.index = t.id.index;
  if (t.convert) {  // map the FL range ends through the target's conversion
    l.min = static_cast<float>(std::clamp(t.convert(0.0), 0.0, 1.0));
    l.max = static_cast<float>(std::clamp(t.convert(1.0), 0.0, 1.0));
  } else {
    l.min = t.min, l.max = t.max;
  }
  static const RemoteButton kMode[5] = {RemoteButton::Value, RemoteButton::Momentary, RemoteButton::Toggle, RemoteButton::Increment,
                                        RemoteButton::Value};
  l.button = kMode[std::clamp(r.noteMode, 0, 4)];
  out = l;
  return true;
}

bool resolveFlTarget(Context& c, uint32_t target, FlTarget& out) {
  Importer imp(c);
  Target t;
  bool ok = imp.resolveTarget(target, t);
  out.id = t.id;
  out.min = t.min;
  out.max = t.max;
  out.convert = t.convert;
  out.what = t.what;
  return ok;
}

}  // namespace gl::flp
