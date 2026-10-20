// Gridline — chop, stutter and time-region editing (PL-13). See Chop.h.
#include "Chop.h"
#include <algorithm>
#include <cmath>
#include <map>
#include "../core/Automation.h"
#include "../core/I18n.h"
#include "../core/NoteTools.h"
#include "../core/PluginInfo.h"
#include "../core/PluginParams.h"
#include "ClipOps.h"
#include "Sample.h"
#include "Slicing.h"

namespace gl::chop {

using i18n::N_;

namespace {

constexpr size_t kMaxSteps = 4096;  // safety limit for one stutter region

Tick endOf(const Song& s, const Clip& c) { return c.start + s.clipLength(c); }

// Splits clip `id` at each tick of `cuts` (ascending); returns all pieces.
std::vector<ObjectId> splitAt(Song& s, ObjectId id, const std::vector<Tick>& cuts) {
  std::vector<ObjectId> out{id};
  ObjectId cur = id;
  for (Tick t : cuts) {
    ObjectId r = splitClipAt(s, cur, t);
    if (r == kNoId) continue;
    out.push_back(r);
    cur = r;
  }
  return out;
}

// The listed clips in playlist order (stable ids before any edit).
std::vector<ObjectId> ordered(const Song& s, const std::set<ObjectId>& clips) {
  std::vector<ObjectId> v;
  for (const auto& c : s.playlist)
    if (clips.count(c.id)) v.push_back(c.id);
  return v;
}

// A pattern's notes per channel with its step rows as notes (C5 by default).
std::map<ObjectId, std::vector<Note>> contentNotes(const Song& s, const Pattern& p) {
  std::map<ObjectId, std::vector<Note>> out;
  for (const auto& [ch, ns] : p.notes) out[ch] = ns;
  const Tick tps = s.ticksPerStep(p);
  if (tps > 0)
    for (const auto& [ch, row] : p.steps) {
      tools::StepPropRow props;
      if (auto it = p.stepProps.find(ch); it != p.stepProps.end()) props = it->second;
      for (const Note& n : tools::stepsToNotes(row, props, tps, 60)) out[ch].push_back(n);
    }
  for (auto& [ch, ns] : out)
    std::stable_sort(ns.begin(), ns.end(), [](const Note& x, const Note& y) { return x.start < y.start; });
  return out;
}

bool tracksMatch(const std::set<int>& tracks, int t) { return tracks.empty() || tracks.count(t) > 0; }

// First track at or below `from` with no clip over [a, b) (and not `except`).
int freeTrack(const Song& s, int from, Tick a, Tick b, const std::set<int>& except) {
  for (int t = std::max(0, from); t < kMaxPlaylistTracks; ++t) {
    if (except.count(t)) continue;
    bool used = false;
    for (const auto& c : s.playlist) used = used || (c.track == t && c.start < b && endOf(s, c) > a);
    if (!used) return t;
  }
  return std::max(0, from);
}

void ensureTrack(Song& s, int t) {
  if (t >= 0 && static_cast<int>(s.tracks.size()) <= t) s.tracks.resize(static_cast<size_t>(t) + 1);
}

std::string rangeName(const Song& s, const std::set<ObjectId>& ids) {
  for (const auto& c : s.playlist) {
    if (!ids.count(c.id)) continue;
    if (c.kind == ClipKind::Audio && s.channel(c.source)) return s.channel(c.source)->name;
    if (c.kind == ClipKind::Pattern && s.pattern(c.pattern)) return s.pattern(c.pattern)->name;
  }
  return "Selection";
}

}  // namespace

// ------------------------------------------------------------------ slicing ----

std::vector<ObjectId> sliceAtGrid(Song& s, const std::set<ObjectId>& clips, Tick grid) {
  std::vector<ObjectId> out;
  if (grid <= 0) return out;
  for (ObjectId id : ordered(s, clips)) {
    const Clip* c = s.clip(id);
    if (!c) continue;
    std::vector<Tick> cuts;
    const Tick e = endOf(s, *c);
    for (Tick t = (c->start / grid + 1) * grid; t < e; t += grid) cuts.push_back(t);
    for (ObjectId k : splitAt(s, id, cuts)) out.push_back(k);
  }
  return out;
}

std::vector<ObjectId> sliceEqual(Song& s, const std::set<ObjectId>& clips, int parts) {
  std::vector<ObjectId> out;
  if (parts < 2) return ordered(s, clips);
  for (ObjectId id : ordered(s, clips)) {
    const Clip* c = s.clip(id);
    if (!c) continue;
    const Tick len = s.clipLength(*c);
    std::vector<Tick> cuts;
    for (int k = 1; k < parts; ++k) {
      const Tick t = c->start + (len * k + parts / 2) / parts;
      if (t > c->start && (cuts.empty() || t > cuts.back())) cuts.push_back(t);
    }
    for (ObjectId k : splitAt(s, id, cuts)) out.push_back(k);
  }
  return out;
}

std::vector<Tick> patternOnsets(const Song& s, const Clip& c) {
  std::vector<Tick> out;
  const Pattern* p = c.kind == ClipKind::Pattern ? s.pattern(c.pattern) : nullptr;
  if (!p) return out;
  const Tick len = s.clipLength(c);
  for (const auto& [ch, ns] : contentNotes(s, *p))
    for (const Note& n : ns)
      if (!n.muted && n.start > c.offset && n.start < c.offset + len) out.push_back(c.start + n.start - c.offset);
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

std::vector<ObjectId> sliceAtTransients(Song& s, const std::set<ObjectId>& clips, double sensitivity) {
  std::vector<ObjectId> out;
  for (ObjectId id : ordered(s, clips)) {
    const Clip* c = s.clip(id);
    if (!c) continue;
    std::vector<ObjectId> pieces;
    if (c->kind == ClipKind::Audio)
      pieces = sliceClipAtTransients(s, id, std::clamp(sensitivity, 0.0, 1.0));
    else if (c->kind == ClipKind::Pattern)
      pieces = splitAt(s, id, patternOnsets(s, *c));
    if (pieces.empty()) pieces.push_back(id);
    out.insert(out.end(), pieces.begin(), pieces.end());
  }
  return out;
}

// ------------------------------------------------------------------ stutter ----

bool StutterParams::operator==(const StutterParams& o) const {
  return rate == o.rate && triplet == o.triplet && endRate == o.endRate && decay == o.decay && pitchStart == o.pitchStart &&
         pitchEnd == o.pitchEnd && reverse == o.reverse && gate == o.gate && speedStart == o.speedStart &&
         speedEnd == o.speedEnd && follow == o.follow && fadeMs == o.fadeMs;
}

Tick rateTicks(int rate, bool triplet, int ppq) {
  rate = std::clamp(rate, 1, 256);
  const double t = 4.0 * ppq / rate * (triplet ? 2.0 / 3.0 : 1.0);
  return std::max<Tick>(1, static_cast<Tick>(std::llround(t)));
}

std::vector<Step> plan(const StutterParams& p, Tick length, int ppq) {
  std::vector<Step> out;
  if (length <= 0) return out;
  const double s0 = static_cast<double>(rateTicks(p.rate, p.triplet, ppq));
  const double s1 = p.endRate > 0 ? static_cast<double>(rateTicks(p.endRate, p.triplet, ppq)) : s0;
  for (Tick t = 0; t < length && out.size() < kMaxSteps;) {
    const double f = static_cast<double>(t) / static_cast<double>(length);
    Tick slot = std::max<Tick>(1, static_cast<Tick>(std::llround(s0 * std::pow(s1 / s0, f))));
    if (out.size() + 1 == kMaxSteps || t + slot > length) slot = length - t;  // the last slot ends at the region end
    Step st;
    st.at = t;
    st.slot = slot;
    out.push_back(st);
    t += slot;
  }
  const size_t n = out.size();
  const double gate = std::clamp(p.gate, 0.05, 1.0), decay = std::clamp(p.decay, -1.0, 1.0);
  double src = 0;
  for (size_t i = 0; i < n; ++i) {
    Step& st = out[i];
    const double fi = n > 1 ? static_cast<double>(i) / static_cast<double>(n - 1) : 0.0;
    const double mid = (static_cast<double>(st.at) + 0.5 * static_cast<double>(st.slot)) / static_cast<double>(length);
    st.play = std::clamp<Tick>(static_cast<Tick>(std::llround(static_cast<double>(st.slot) * gate)), 1, st.slot);
    st.speed = std::clamp(p.speedStart + (p.speedEnd - p.speedStart) * mid, 0.05, 4.0);
    st.src = p.follow ? src : 0.0;
    src += static_cast<double>(st.slot) * st.speed;
    st.semis = p.pitchStart + (p.pitchEnd - p.pitchStart) * fi;
    st.gain = decay >= 0 ? std::pow(1.0 - decay, static_cast<double>(i)) : std::pow(1.0 + decay, static_cast<double>(n - 1 - i));
    st.reverse = p.reverse == 1 || (p.reverse == 2 && i % 2 == 1);
  }
  return out;
}

namespace {

// Audio: the region clip `base` becomes one clip per step.
std::vector<ObjectId> stutterAudio(Song& s, const Clip& base, const std::vector<Step>& steps, const StutterParams& p) {
  std::vector<ObjectId> out;
  const Channel* ch = s.channel(base.source);
  if (!ch) return out;
  SamplePtr smp = sampleCache().get(ch->plugin.data);
  TempoMap map;
  map.build(s);
  const Tick ra = base.start;
  s.playlist.erase(std::remove_if(s.playlist.begin(), s.playlist.end(), [&](const Clip& c) { return c.id == base.id; }),
                   s.playlist.end());
  for (const Step& st : steps) {
    Clip k = base;
    k.id = s.allocateId();
    k.start = ra + st.at;
    k.length = st.play;
    k.audioStart = std::max(0.0, clipSourceAt(s, base, map, static_cast<double>(ra) + st.src));
    k.stretch = base.stretch / st.speed;
    k.pitch = base.pitch + static_cast<float>(st.semis * 100.0);
    k.gain = base.gain * static_cast<float>(st.gain);
    k.takeGroup = kNoId, k.take = 0;
    const double secs = map.secondsAt(static_cast<double>(k.start + k.length)) - map.secondsAt(static_cast<double>(k.start));
    const double f = std::min(std::max(0.0, p.fadeMs) / 1000.0, 0.5 * secs);
    k.fadeIn = k.fadeOut = f;
    k.fadeInManual = k.fadeOutManual = f > 0;
    k.fadeInShape = k.fadeOutShape = 0;
    k.fadeInTension = k.fadeOutTension = 0;
    if (st.reverse && smp && smp->frames() > 1 && smp->sampleRate > 0) {
      // Same source window, read backwards: the engine reads a reversed clip
      // at (last frame − position), so the window [s0, s1) maps to [D − s1, D − s0).
      const double s0 = k.audioStart, s1 = clipSourceAt(s, k, map, static_cast<double>(k.start + k.length));
      const double D = static_cast<double>(smp->frames() - 1) / smp->sampleRate;
      k.reverse = !k.reverse;
      k.audioStart = std::max(0.0, D - s1);
      k.audioEnd = std::max(k.audioStart + 1e-6, D - s0);
    }
    s.playlist.push_back(k);
    out.push_back(k.id);
  }
  return out;
}

// Pattern: the region clip `base` plays a new pattern with the repeated notes.
std::vector<ObjectId> stutterPattern(Song& s, const Clip& base, Tick length, const std::vector<Step>& steps) {
  const Pattern* bp = s.pattern(base.pattern);
  if (!bp) return {};
  const auto notes = contentNotes(s, *bp);
  Pattern np;
  np.name = bp->name + " stutter";
  np.color = bp->color;
  np.stepsPerBar = bp->stepsPerBar;
  const Tick bar = std::max<Tick>(1, s.barTicks());
  np.bars = static_cast<int>(std::clamp<Tick>((length + bar - 1) / bar, 1, kMaxPatternBars));
  for (const auto& lane : bp->automation) {  // parameter lanes follow the region
    PatternAutomation l = lane;
    l.points.clear();
    for (AutoPoint q : lane.points)
      if (q.pos >= base.offset && q.pos <= base.offset + length) q.pos -= base.offset, l.points.push_back(q);
    if (!l.points.empty()) np.automation.push_back(l);
  }
  for (const Step& st : steps) {
    const double w0 = static_cast<double>(base.offset) + st.src;
    const double W = static_cast<double>(st.play) * st.speed;  // content heard in this slice
    const Tick end = st.at + st.play;
    for (const auto& [ch, ns] : notes)
      for (const Note& n : ns) {
        const double ns0 = static_cast<double>(n.start);
        if (ns0 < w0 - 1e-9 || ns0 >= w0 + W - 1e-9) continue;
        const double heard = std::min(static_cast<double>(n.length), w0 + W - ns0);
        double rel = (ns0 - w0) / st.speed;
        if (st.reverse) rel = (W - (ns0 - w0) - heard) / st.speed;
        Note m = n;
        m.start = st.at + std::max<Tick>(0, static_cast<Tick>(std::llround(rel)));
        if (m.start >= end) continue;
        m.length = std::clamp<Tick>(static_cast<Tick>(std::llround(heard / st.speed)), 1, end - m.start);
        const long whole = std::lround(st.semis);
        m.pitch = std::clamp(n.pitch + static_cast<int>(whole), 0, 127);
        m.fine = std::clamp(n.fine + static_cast<int>(std::lround((st.semis - static_cast<double>(whole)) * 100.0)), -kNoteFineMax,
                            kNoteFineMax);
        m.velocity = static_cast<int>(std::lround(n.velocity * st.gain));
        if (m.velocity < 1) continue;
        m.velocity = std::min(m.velocity, 127);
        np.notes[ch].push_back(m);
      }
  }
  for (auto& [ch, ns] : np.notes)
    std::stable_sort(ns.begin(), ns.end(), [](const Note& x, const Note& y) { return x.start != y.start ? x.start < y.start : x.pitch < y.pitch; });
  np.id = s.allocateId();
  s.patterns.push_back(np);
  Clip* c = s.clip(base.id);
  c->pattern = np.id;
  c->offset = 0;
  c->length = length;
  return {base.id};
}

}  // namespace

std::vector<ObjectId> stutter(Song& s, const std::set<ObjectId>& clips, Tick a, Tick b, const StutterParams& p) {
  std::vector<ObjectId> out;
  if (b <= a) return out;
  for (ObjectId id : ordered(s, clips)) {
    const Clip* c = s.clip(id);
    if (!c || c->kind == ClipKind::Automation) continue;
    const Tick ra = std::max(a, c->start), rb = std::min(b, endOf(s, *c));
    if (rb <= ra) continue;
    ObjectId mid = id;
    if (ra > c->start) mid = splitClipAt(s, id, ra);
    if (mid == kNoId) continue;
    if (rb < endOf(s, *s.clip(mid))) splitClipAt(s, mid, rb);
    const Clip base = *s.clip(mid);
    const auto steps = plan(p, rb - ra, s.ppq);
    const auto ids = base.kind == ClipKind::Audio ? stutterAudio(s, base, steps, p) : stutterPattern(s, base, rb - ra, steps);
    out.insert(out.end(), ids.begin(), ids.end());
  }
  return out;
}

// --------------------------------------------------------------- chop presets ----

const char* presetName(Preset p) {
  switch (p) {
    case Preset::BeatRepeat: return N_("Beat repeat");
    case Preset::TapeStop: return N_("Tape stop");
    case Preset::HalfTime: return N_("Half-time");
    case Preset::TripletStutter: return N_("Triplet stutter");
    case Preset::ReverseBuild: return N_("Reverse build");
    default: return "";
  }
}

const char* presetHint(Preset p) {
  switch (p) {
    case Preset::BeatRepeat: return N_("Repeats the first eighth of the region");
    case Preset::TapeStop: return N_("Plays on while slowing down to a halt");
    case Preset::HalfTime: return N_("Plays the region at half speed");
    case Preset::TripletStutter: return N_("Short gated triplet repeats that fade away");
    case Preset::ReverseBuild: return N_("Reversed repeats that speed up, rise and get louder");
    default: return "";
  }
}

StutterParams presetParams(Preset p) {
  StutterParams q;
  switch (p) {
    case Preset::BeatRepeat: q.rate = 8; break;
    case Preset::TapeStop:
      q.rate = 32, q.follow = true, q.speedStart = 1.0, q.speedEnd = 0.1, q.fadeMs = 1.0;
      break;
    case Preset::HalfTime: q.rate = 16, q.follow = true, q.speedStart = q.speedEnd = 0.5; break;
    case Preset::TripletStutter: q.rate = 16, q.triplet = true, q.gate = 0.7, q.decay = 0.12; break;
    case Preset::ReverseBuild:
      q.rate = 4, q.endRate = 32, q.reverse = 1, q.decay = -0.08, q.pitchStart = 0, q.pitchEnd = 7;
      break;
    default: break;
  }
  return q;
}

// ----------------------------------------------------------- time selection ----

std::set<ObjectId> clipsInRange(const Song& s, Tick a, Tick b, const std::set<int>& tracks) {
  std::set<ObjectId> out;
  for (const auto& c : s.playlist)
    if (tracksMatch(tracks, c.track) && c.start < b && endOf(s, c) > a) out.insert(c.id);
  return out;
}

std::set<ObjectId> isolateRange(Song& s, Tick a, Tick b, const std::set<int>& tracks) {
  std::set<ObjectId> out;
  if (b <= a) return out;
  for (ObjectId id : ordered(s, clipsInRange(s, a, b, tracks))) {
    ObjectId mid = id;
    const Clip* c = s.clip(id);
    if (a > c->start) mid = splitClipAt(s, id, a);
    if (mid == kNoId) continue;
    if (b < endOf(s, *s.clip(mid))) splitClipAt(s, mid, b);
    out.insert(mid);
  }
  return out;
}

std::vector<ObjectId> duplicateRange(Song& s, Tick a, Tick b, const std::set<int>& tracks) {
  std::vector<ObjectId> out;
  if (b <= a) return out;
  const Tick len = b - a;
  // Which tracks take part: the listed ones, or the ones with content in the range.
  std::set<int> tr = tracks;
  if (tr.empty())
    for (const auto& c : s.playlist)
      if (c.start < b && endOf(s, c) > a) tr.insert(c.track);
  if (tr.empty()) return out;
  const std::set<ObjectId> src = isolateRange(s, a, b, tr);
  // The destination is cleared first (clips there are cut at its edges).
  const std::set<ObjectId> dst = isolateRange(s, b, b + len, tr);
  s.playlist.erase(std::remove_if(s.playlist.begin(), s.playlist.end(), [&](const Clip& c) { return dst.count(c.id) > 0; }),
                   s.playlist.end());
  std::vector<Clip> copies;
  for (const auto& c : s.playlist)
    if (src.count(c.id)) copies.push_back(c);
  for (Clip c : copies) {
    c.id = s.allocateId();
    c.start += len;
    c.takeGroup = kNoId, c.take = 0;
    s.playlist.push_back(c);
    out.push_back(c.id);
  }
  return out;
}

bool insertSpace(Song& s, Tick at, Tick length, const std::set<int>& tracks) {
  if (at < 0 || length <= 0) return false;
  for (ObjectId id : ordered(s, clipsInRange(s, at, at + 1, tracks)))
    if (const Clip* c = s.clip(id); c && c->start < at) splitClipAt(s, id, at);
  for (auto& c : s.playlist)
    if (tracksMatch(tracks, c.track) && c.start >= at) c.start += length;
  if (tracks.empty()) {
    for (auto& m : s.markers)
      if (m.pos >= at) m.pos += length;
    if (s.live.areaEnd > at) s.live.areaEnd += length;
  }
  return true;
}

bool deleteSpace(Song& s, Tick a, Tick b, const std::set<int>& tracks) {
  if (a < 0 || b <= a) return false;
  const Tick len = b - a;
  const std::set<ObjectId> inside = isolateRange(s, a, b, tracks);
  s.playlist.erase(std::remove_if(s.playlist.begin(), s.playlist.end(), [&](const Clip& c) { return inside.count(c.id) > 0; }),
                   s.playlist.end());
  for (auto& c : s.playlist)
    if (tracksMatch(tracks, c.track) && c.start >= b) c.start -= len;
  if (tracks.empty()) {
    s.markers.erase(std::remove_if(s.markers.begin(), s.markers.end(), [&](const Marker& m) { return m.pos >= a && m.pos < b; }),
                    s.markers.end());
    for (auto& m : s.markers)
      if (m.pos >= b) m.pos -= len;
    if (s.live.areaEnd >= b) s.live.areaEnd -= len;
    else if (s.live.areaEnd > a) s.live.areaEnd = a;
  }
  return true;
}

namespace {
// Renders [a, b) of `tracks` into a WAV (cuts made on a copy: the song is unchanged).
std::string renderRange(const Song& s, Tick a, Tick b, const std::set<int>& tracks, const std::string& dir, double sr,
                        std::set<int>* used, std::string* name) {
  Song tmp = s;
  std::set<ObjectId> ids = isolateRange(tmp, a, b, tracks);
  for (auto it = ids.begin(); it != ids.end();) {  // automation clips are not audio
    const Clip* c = tmp.clip(*it);
    it = c && c->kind != ClipKind::Automation && !c->muted ? std::next(it) : ids.erase(it);
  }
  if (ids.empty()) return "";
  if (used)
    for (ObjectId id : ids) used->insert(tmp.clip(id)->track);
  const std::string n = rangeName(tmp, ids);
  if (name) *name = n;
  return renderClipsToWav(tmp, ids, a, b, sr, dir, n + " consolidated");
}

ObjectId addChannelFor(Song& s, const std::string& type, const std::string& name, const std::string& path) {
  Channel ch;
  ch.id = s.allocateId();
  ch.name = name;
  ch.color = 0x8A9BA8;
  ch.plugin = defaultPluginState(type);
  ch.plugin.data = path;
  ch.mixerTrack = s.masterId();
  s.channels.push_back(ch);
  return ch.id;
}
}  // namespace

ObjectId consolidateRange(Song& s, Tick a, Tick b, const std::set<int>& tracks, const std::string& dir, double sr, int destTrack) {
  if (b <= a) return kNoId;
  std::set<int> used;
  std::string name;
  const std::string path = renderRange(s, a, b, tracks, dir, sr, &used, &name);
  if (path.empty()) return kNoId;
  const ObjectId ch = addChannelFor(s, "gl.sampler", name + " (rendered)", path);
  Clip c;
  c.id = s.allocateId();
  c.kind = ClipKind::Audio;
  c.source = ch;
  c.track = destTrack >= 0 ? destTrack : freeTrack(s, used.empty() ? 0 : *used.rbegin() + 1, a, b, used);
  c.start = a;
  c.length = b - a;
  ensureTrack(s, c.track);
  s.playlist.push_back(c);
  return c.id;
}

ChopResult chopRangeToSlicer(Song& s, Tick a, Tick b, const std::set<int>& tracks, Tick grid, double sensitivity,
                             const std::string& dir, double sr, int destTrack) {
  ChopResult r;
  if (b <= a) return r;
  std::set<int> used;
  std::string name;
  const std::string path = renderRange(s, a, b, tracks, dir, sr, &used, &name);
  if (path.empty()) return r;
  SamplePtr smp = sampleCache().get(path);
  if (!smp || smp->frames() < 2) return r;
  // Slice starts (ticks from a): the grid, or the transients of the rendered audio.
  const Tick len = b - a;
  std::vector<Tick> starts{0};
  if (grid > 0) {
    for (Tick t = grid; t < len; t += grid) starts.push_back(t);
  } else {
    TempoMap map;
    map.build(s);
    const double sec0 = map.secondsAt(static_cast<double>(a));
    for (int64_t f : computeSlices(*smp, 0, std::clamp(sensitivity, 0.0, 1.0))) {
      const Tick t = static_cast<Tick>(std::llround(map.tickAtSeconds(sec0 + static_cast<double>(f) / smp->sampleRate))) - a;
      if (t > starts.back() && t < len) starts.push_back(t);
    }
  }
  if (static_cast<int>(starts.size()) > pp::kMaxSlices) starts.resize(static_cast<size_t>(pp::kMaxSlices));
  TempoMap map;
  map.build(s);
  const double sec0 = map.secondsAt(static_cast<double>(a)), secs = map.secondsAt(static_cast<double>(b)) - sec0;
  const ObjectId ch = addChannelFor(s, "gl.slicer", name + " (chopped)", path);
  Channel* c = s.channel(ch);
  auto& prm = c->plugin.params;
  if (prm.size() < static_cast<size_t>(pp::SlCount)) prm.resize(static_cast<size_t>(pp::SlCount), 0.0f);
  prm[pp::SlMode] = static_cast<float>(pp::SlModeManual);
  for (int k = 0; k < pp::kMaxSlices; ++k) prm[static_cast<size_t>(pp::slMarker(k))] = -1.0f;
  const int baseNote = static_cast<int>(std::lround(prm[pp::SlBaseNote]));
  Pattern p;
  p.id = s.allocateId();
  p.name = name + " chops";
  p.color = 0x8A9BA8;
  const Tick bar = std::max<Tick>(1, s.barTicks());
  p.bars = static_cast<int>(std::clamp<Tick>((len + bar - 1) / bar, 1, kMaxPatternBars));
  for (size_t k = 0; k < starts.size(); ++k) {
    const double pos = (map.secondsAt(static_cast<double>(a + starts[k])) - sec0) / std::max(1e-9, secs);
    prm[static_cast<size_t>(pp::slMarker(static_cast<int>(k)))] = static_cast<float>(std::clamp(pos, 0.0, 1.0));
    const Tick e = k + 1 < starts.size() ? starts[k + 1] : len;
    p.notes[ch].push_back(Note(starts[k], e - starts[k], std::clamp(baseNote + static_cast<int>(k), 0, 127), 100));
  }
  s.patterns.push_back(p);
  Clip pc;
  pc.id = s.allocateId();
  pc.pattern = p.id;
  pc.track = destTrack >= 0 ? destTrack : freeTrack(s, used.empty() ? 0 : *used.rbegin() + 1, a, b, used);
  pc.start = a;
  pc.length = len;
  ensureTrack(s, pc.track);
  s.playlist.push_back(pc);
  r.channel = ch, r.pattern = p.id, r.clip = pc.id, r.slices = static_cast<int>(starts.size());
  return r;
}

}  // namespace gl::chop
