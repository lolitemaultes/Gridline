#include "MenuOps.h"
#include "ClipLoop.h"
#include "ChordTools.h"
#include "TrackModes.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>

namespace gl::ops {

namespace {

template <typename T>
void eraseIf(std::vector<T>& v, const std::function<bool(const T&)>& pred) {
  v.erase(std::remove_if(v.begin(), v.end(), pred), v.end());
}

std::vector<uint8_t> stepRow(const Pattern& p, ObjectId ch) {
  std::vector<uint8_t> row;
  if (auto it = p.steps.find(ch); it != p.steps.end()) row = it->second;
  row.resize(static_cast<size_t>(std::max(0, p.bars * p.stepsPerBar)), 0);
  return row;
}

bool hasData(const Pattern& p, ObjectId ch) {
  if (auto it = p.steps.find(ch); it != p.steps.end())
    for (uint8_t v : it->second)
      if (v) return true;
  if (auto it = p.notes.find(ch); it != p.notes.end() && !it->second.empty()) return true;
  return false;
}

Tick roundTo(Tick t, Tick grid) {
  if (grid <= 1) return t;
  Tick q = (t + grid / 2) / grid;
  if (t < 0) q = -((-t + grid / 2) / grid);
  return q * grid;
}

Rgb lerpColor(Rgb a, Rgb b, double t) {
  auto ch = [&](int shift) {
    double x = ((a >> shift) & 0xFF) * (1 - t) + ((b >> shift) & 0xFF) * t;
    return static_cast<Rgb>(std::clamp(static_cast<int>(std::lround(x)), 0, 255)) << shift;
  };
  return ch(16) | ch(8) | ch(0);
}

// Targeted notes: the selection, or everything when nothing is selected.
std::vector<bool> targets(const std::vector<Note>& notes, NoteSel& sel) {
  sel.resize(notes.size(), false);
  bool any = std::find(sel.begin(), sel.end(), true) != sel.end();
  std::vector<bool> t(notes.size(), !any);
  if (any) t = sel;
  return t;
}

uint32_t lcg(uint32_t& state) {
  state = state * 1664525u + 1013904223u;
  return state >> 8;
}

}  // namespace

// ---------------------------------------------------------------- patterns ----

bool patternIsEmpty(const Pattern& p) {
  for (const auto& [ch, row] : p.steps)
    for (uint8_t v : row)
      if (v) return false;
  for (const auto& [ch, ns] : p.notes)
    if (!ns.empty()) return false;
  return true;
}

ObjectId firstEmptyPattern(const Song& s) {
  for (const auto& p : s.patterns)
    if (patternIsEmpty(p)) return p.id;
  return kNoId;
}

ObjectId nextEmptyPattern(const Song& s, ObjectId after) {
  int n = static_cast<int>(s.patterns.size());
  int start = s.patternIndex(after);
  for (int k = 1; k <= n; ++k) {
    const Pattern& p = s.patterns[static_cast<size_t>((start + k + n) % n)];
    if (p.id != after && patternIsEmpty(p)) return p.id;
  }
  return kNoId;
}

std::string nextPatternName(const Song& s) {
  for (size_t n = s.patterns.size() + 1;; ++n) {
    std::string name = "Pattern " + std::to_string(n);
    bool used = false;
    for (const auto& p : s.patterns) used = used || p.name == name;
    if (!used) return name;
  }
}

ObjectId nextEmptyOrNewPattern(Document& d, ObjectId after) {
  ObjectId id = nextEmptyPattern(d.song(), after);
  if (id != kNoId) return id;
  return d.addPattern(nextPatternName(d.song()));
}

std::vector<ObjectId> unusedPatterns(const Song& s) {
  std::vector<ObjectId> out;
  for (const auto& p : s.patterns) {
    bool used = false;
    for (const auto& c : s.playlist) used = used || c.pattern == p.id;
    if (!used) out.push_back(p.id);
  }
  return out;
}

int deleteUnusedPatterns(Document& d) {
  auto unused = unusedPatterns(d.song());
  if (unused.size() == d.song().patterns.size() && !unused.empty()) unused.erase(unused.begin());  // keep one
  if (unused.empty()) return 0;
  std::set<ObjectId> del(unused.begin(), unused.end());
  d.editSong("delete unused patterns",
             [&](Song& s) { eraseIf<Pattern>(s.patterns, [&](const Pattern& p) { return del.count(p.id) > 0; }); });
  return static_cast<int>(del.size());
}

ObjectId insertPattern(Document& d, ObjectId before, const std::string& name) {
  int idx = d.song().patternIndex(before);
  return d.addPattern(name, 1, 16, idx);
}

bool movePattern(Document& d, ObjectId id, int newIndex) {
  const Song& s = d.song();
  int from = s.patternIndex(id);
  if (from < 0 || newIndex < 0 || newIndex >= static_cast<int>(s.patterns.size())) return false;
  if (from == newIndex) return true;
  d.editSong("move pattern", [&](Song& x) {
    Pattern p = x.patterns[static_cast<size_t>(from)];
    x.patterns.erase(x.patterns.begin() + from);
    x.patterns.insert(x.patterns.begin() + newIndex, p);
  });
  return true;
}

bool setPatternColor(Document& d, ObjectId id, Rgb color) {
  return d.editPattern(id, "change pattern colour", [&](Pattern& p) { p.color = color; });
}

bool transposePattern(Document& d, ObjectId id, int semitones) {
  return d.editPattern(id, "transpose pattern", [&](Pattern& p) {
    for (auto& [ch, ns] : p.notes)
      for (auto& n : ns) n.pitch = std::clamp(n.pitch + semitones, 0, 127);
  });
}

std::vector<ObjectId> splitPatternByChannel(Document& d, ObjectId id) {
  const Song& cur = d.song();
  const Pattern* src = cur.pattern(id);
  if (!src) return {};
  std::vector<ObjectId> chans;
  for (const auto& c : cur.channels)
    if (hasData(*src, c.id)) chans.push_back(c.id);
  if (chans.empty() || static_cast<int>(cur.patterns.size() + chans.size()) > kMaxPatterns) return {};
  std::vector<ObjectId> out;
  d.editSong("split pattern by channel", [&](Song& s) {
    Pattern base = *s.pattern(id);
    int idx = s.patternIndex(id);
    for (size_t k = 0; k < chans.size(); ++k) {
      Pattern np = base;
      np.id = s.allocateId();
      np.name = base.name + " - " + s.channel(chans[k])->name;
      np.steps.clear();
      np.stepProps.clear();
      np.notes.clear();
      if (auto it = base.steps.find(chans[k]); it != base.steps.end()) np.steps[chans[k]] = it->second;
      if (auto it = base.stepProps.find(chans[k]); it != base.stepProps.end()) np.stepProps[chans[k]] = it->second;
      if (auto it = base.notes.find(chans[k]); it != base.notes.end()) np.notes[chans[k]] = it->second;
      s.patterns.insert(s.patterns.begin() + idx + 1 + static_cast<int>(k), np);
      out.push_back(np.id);
    }
    std::vector<Clip> old;
    for (const auto& c : s.playlist)
      if (c.pattern == id) old.push_back(c);
    eraseIf<Clip>(s.playlist, [&](const Clip& c) { return c.pattern == id; });
    for (const auto& c : old) {
      Tick len = s.clipLength(c);  // pattern still present: keeps the clip's timing
      for (size_t k = 0; k < out.size(); ++k) {
        Clip nc;
        nc.id = s.allocateId();
        nc.pattern = out[k];
        nc.track = std::min(c.track + static_cast<int>(k), kMaxPlaylistTracks - 1);
        nc.start = c.start;
        nc.length = len;
        nc.offset = c.offset;
        nc.muted = c.muted;
        if (static_cast<size_t>(nc.track) >= s.tracks.size()) s.tracks.resize(static_cast<size_t>(nc.track) + 1);
        s.playlist.push_back(nc);
      }
    }
  });
  return out;
}

// ---------------------------------------------------------------- channels ----

bool fillSteps(Document& d, ObjectId pattern, ObjectId channel, int every, int offset, uint8_t velocity) {
  const Pattern* p = d.song().pattern(pattern);
  if (!p || !d.song().channel(channel) || every < 1) return false;
  auto row = stepRow(*p, channel);
  for (size_t k = 0; k < row.size(); ++k) {
    int i = static_cast<int>(k) - offset;
    row[k] = i >= 0 && i % every == 0 ? velocity : 0;
  }
  return d.setSteps(pattern, channel, row);
}

bool rotateChannel(Document& d, ObjectId pattern, ObjectId channel, int dir) {
  const Pattern* p = d.song().pattern(pattern);
  if (!p || !d.song().channel(channel) || dir == 0) return false;
  auto row = stepRow(*p, channel);
  if (!row.empty()) {
    if (dir < 0)
      std::rotate(row.begin(), row.begin() + 1, row.end());
    else
      std::rotate(row.rbegin(), row.rbegin() + 1, row.rend());
  }
  std::vector<Note> ns;
  if (auto it = p->notes.find(channel); it != p->notes.end()) ns = it->second;
  Tick len = d.song().patternLength(*p), step = d.song().ticksPerStep(*p);
  if (len > 0)
    for (auto& n : ns) n.start = ((n.start + (dir < 0 ? -step : step)) % len + len) % len;
  d.beginGroup(dir < 0 ? "rotate left" : "rotate right");
  d.setSteps(pattern, channel, row);
  if (p->stepProps.count(channel))  // per-step parameters move with their steps
    d.editPattern(pattern, "rotate step parameters", [&](Pattern& q) {
      auto& pr = q.stepProps[channel];
      pr.resize(row.size());
      if (pr.empty()) return;
      if (dir < 0)
        std::rotate(pr.begin(), pr.begin() + 1, pr.end());
      else
        std::rotate(pr.rbegin(), pr.rbegin() + 1, pr.rend());
    });
  d.setNotes(pattern, channel, ns, "rotate notes");
  d.endGroup();
  return true;
}

bool clearChannel(Document& d, ObjectId pattern, ObjectId channel) {
  if (!d.song().pattern(pattern) || !d.song().channel(channel)) return false;
  d.beginGroup("clear channel in pattern");
  d.setSteps(pattern, channel, {});
  if (d.song().pattern(pattern)->stepProps.count(channel))
    d.editPattern(pattern, "clear step parameters", [&](Pattern& q) { q.stepProps.erase(channel); });
  d.setNotes(pattern, channel, {});
  d.endGroup();
  return true;
}

bool setChannelsColor(Document& d, const std::vector<ObjectId>& channels, Rgb color) {
  bool ok = false;
  d.beginGroup("change channel colour");
  for (ObjectId c : channels) ok |= d.editChannel(c, "change channel colour", [&](Channel& ch) { ch.color = color; });
  d.endGroup();
  return ok;
}

bool gradientColor(Document& d, const std::vector<ObjectId>& channels) {
  std::vector<ObjectId> order;
  for (const auto& c : d.song().channels)
    if (std::find(channels.begin(), channels.end(), c.id) != channels.end()) order.push_back(c.id);
  if (order.size() < 3) return false;
  Rgb a = d.song().channel(order.front())->color, b = d.song().channel(order.back())->color;
  d.beginGroup("gradient colour");
  for (size_t i = 0; i < order.size(); ++i) {
    Rgb c = lerpColor(a, b, static_cast<double>(i) / static_cast<double>(order.size() - 1));
    d.editChannel(order[i], "gradient colour", [&](Channel& ch) { ch.color = c; });
  }
  d.endGroup();
  return true;
}

bool setChannelsMuted(Document& d, const std::vector<ObjectId>& channels, bool muted) {
  bool ok = false;
  d.beginGroup(muted ? "mute channels" : "unmute channels");
  for (ObjectId c : channels) ok |= d.editChannel(c, "mute channel", [&](Channel& ch) { ch.muted = muted; });
  d.endGroup();
  return ok;
}

bool transposeChannels(Document& d, ObjectId pattern, const std::vector<ObjectId>& channels, int semitones) {
  const Pattern* p = d.song().pattern(pattern);
  if (!p) return false;
  d.beginGroup("transpose channels");
  for (ObjectId c : channels) {
    auto it = p->notes.find(c);
    if (it == p->notes.end()) continue;
    auto ns = it->second;
    for (auto& n : ns) n.pitch = std::clamp(n.pitch + semitones, 0, 127);
    d.setNotes(pattern, c, ns, "transpose channels");
    p = d.song().pattern(pattern);
  }
  d.endGroup();
  return true;
}

bool sortChannels(Document& d, ChannelSort by) {
  d.editSong("sort channels", [by](Song& s) {
    auto key = [&](const Channel& c) -> std::string {
      switch (by) {
        case ChannelSort::Name: {
          std::string n = c.name;
          for (char& ch : n) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
          return n;
        }
        case ChannelSort::Color: {
          // Hue-ish order: by the dominant component, then value.
          char buf[16];
          std::snprintf(buf, sizeof buf, "%06X", c.color);
          return buf;
        }
        case ChannelSort::MixerTrack: {
          char buf[16];
          std::snprintf(buf, sizeof buf, "%05d", s.mixerIndex(c.mixerTrack));
          return buf;
        }
        case ChannelSort::Instrument: {
          const PluginInfo* p = findPlugin(c.plugin.type);
          return p ? p->name : c.plugin.type;
        }
      }
      return c.name;
    };
    std::stable_sort(s.channels.begin(), s.channels.end(), [&](const Channel& a, const Channel& b) { return key(a) < key(b); });
  });
  return true;
}

bool moveChannels(Document& d, const std::vector<ObjectId>& channels, int dir) {
  const Song& s = d.song();
  std::vector<int> idx;
  for (ObjectId c : channels)
    if (int i = s.channelIndex(c); i >= 0) idx.push_back(i);
  if (idx.empty() || dir == 0) return false;
  std::sort(idx.begin(), idx.end());
  if (dir < 0 && idx.front() == 0) return false;
  if (dir > 0 && idx.back() + 1 >= static_cast<int>(s.channels.size())) return false;
  d.editSong("move channels", [&](Song& x) {
    if (dir < 0)
      for (int i : idx) std::swap(x.channels[static_cast<size_t>(i)], x.channels[static_cast<size_t>(i - 1)]);
    else
      for (auto it = idx.rbegin(); it != idx.rend(); ++it)
        std::swap(x.channels[static_cast<size_t>(*it)], x.channels[static_cast<size_t>(*it + 1)]);
  });
  return true;
}

std::vector<ObjectId> cloneChannels(Document& d, const std::vector<ObjectId>& channels) {
  std::vector<ObjectId> out;
  d.beginGroup("clone channels");
  for (ObjectId c : channels)
    if (ObjectId n = d.cloneChannel(c); n != kNoId) out.push_back(n);
  d.endGroup();
  return out;
}

bool removeChannels(Document& d, const std::vector<ObjectId>& channels) {
  bool ok = false;
  d.beginGroup("delete channels");
  for (ObjectId c : channels) ok |= d.removeChannel(c);
  d.endGroup();
  return ok;
}

std::vector<ObjectId> unusedChannels(const Song& s) {
  std::vector<ObjectId> out;
  for (const auto& c : s.channels) {
    bool used = false;
    for (const auto& p : s.patterns) used = used || hasData(p, c.id);
    if (!used) out.push_back(c.id);
  }
  return out;
}

bool mixerTrackIsFree(const Song& s, ObjectId track) {
  const MixerTrack* t = s.mixerTrack(track);
  if (!t || track == s.masterId() || !t->name.empty()) return false;
  for (const auto& c : s.channels)
    if (c.mixerTrack == track) return false;
  for (const auto& sl : t->slots)
    if (!sl.plugin.empty()) return false;
  for (const auto& m : s.mixer)
    for (const auto& snd : m.sends)
      if (snd.to == track) return false;
  return true;
}

std::vector<ObjectId> routeToFreeMixerTracks(Document& d, const std::vector<ObjectId>& channels) {
  std::vector<ObjectId> order;
  for (const auto& c : d.song().channels)
    if (std::find(channels.begin(), channels.end(), c.id) != channels.end()) order.push_back(c.id);
  if (order.empty()) return {};
  std::vector<ObjectId> used;
  d.editSong("route to free mixer tracks", [&](Song& s) {
    ensureMixer(s, 0);
    for (ObjectId cid : order) {
      Channel* c = s.channel(cid);
      ObjectId target = kNoId;
      for (size_t i = 1; i < s.mixer.size() && target == kNoId; ++i)
        if (mixerTrackIsFree(s, s.mixer[i].id)) target = s.mixer[i].id;
      if (target == kNoId) {
        if (static_cast<int>(s.mixer.size()) > kMaxMixerInserts) break;
        MixerTrack t;
        t.id = s.allocateId();
        t.sends.push_back({s.masterId(), 1.0f});
        s.mixer.push_back(t);
        target = t.id;
      }
      c->mixerTrack = target;
      MixerTrack* t = s.mixerTrack(target);
      t->name = c->name;
      t->color = c->color;
      used.push_back(target);
    }
  });
  return used;
}

bool routeChannels(Document& d, const std::vector<ObjectId>& channels, ObjectId track, bool consecutive) {
  const Song& s = d.song();
  int base = s.mixerIndex(track);
  if (base < 0) return false;
  std::vector<ObjectId> order;
  for (const auto& c : s.channels)
    if (std::find(channels.begin(), channels.end(), c.id) != channels.end()) order.push_back(c.id);
  d.beginGroup("route channels");
  for (size_t k = 0; k < order.size(); ++k) {
    size_t mi = consecutive ? std::min(static_cast<size_t>(base) + k, d.song().mixer.size() - 1) : static_cast<size_t>(base);
    d.routeChannel(order[k], d.song().mixer[mi].id);
  }
  d.endGroup();
  return !order.empty();
}

// ----------------------------------------------------------------- playlist ----

bool insertPlaylistTrack(Document& d, int index) {
  const Song& s = d.song();
  if (index < 0 || index > static_cast<int>(s.tracks.size()) || static_cast<int>(s.tracks.size()) >= kMaxPlaylistTracks) return false;
  d.editSong("insert playlist track", [&](Song& x) {
    x.tracks.insert(x.tracks.begin() + index, PlaylistTrack{});
    for (auto& c : x.playlist)
      if (c.track >= index) ++c.track;
  });
  return true;
}

bool deletePlaylistTrack(Document& d, int index) {
  const Song& s = d.song();
  if (index < 0 || index >= static_cast<int>(s.tracks.size()) || s.tracks.size() <= 1) return false;
  d.editSong("delete playlist track", [&](Song& x) {
    eraseIf<Clip>(x.playlist, [&](const Clip& c) { return c.track == index; });
    for (auto& c : x.playlist)
      if (c.track > index) --c.track;
    x.tracks.erase(x.tracks.begin() + index);
  });
  return true;
}

bool movePlaylistTrack(Document& d, int index, int dir) {
  const Song& s = d.song();
  int j = index + dir;
  int n = static_cast<int>(s.tracks.size());
  if (index < 0 || index >= n || j < 0 || j >= n || dir == 0) return false;
  d.editSong(dir < 0 ? "move playlist track up" : "move playlist track down", [&](Song& x) {
    std::swap(x.tracks[static_cast<size_t>(index)], x.tracks[static_cast<size_t>(j)]);
    for (auto& c : x.playlist) {
      if (c.track == index)
        c.track = j;
      else if (c.track == j)
        c.track = index;
    }
  });
  return true;
}

bool clonePlaylistTrack(Document& d, int index) {
  const Song& s = d.song();
  if (index < 0 || index >= static_cast<int>(s.tracks.size()) || static_cast<int>(s.tracks.size()) >= kMaxPlaylistTracks) return false;
  d.editSong("clone playlist track", [&](Song& x) {
    x.tracks.insert(x.tracks.begin() + index + 1, x.tracks[static_cast<size_t>(index)]);
    {  // PL-08: a mixer track or channel belongs to one track; a cloned audio track gets a free mixer track
      PlaylistTrack& k = x.tracks[static_cast<size_t>(index) + 1];
      const bool audio = k.mode == TrackMode::Audio;
      k.mode = TrackMode::Unassigned;
      k.mixer = k.channel = kNoId;
      if (audio) trackmodes::makeAudioTrack(x, index + 1);
    }
    std::vector<Clip> copies;
    for (auto& c : x.playlist) {
      if (c.track > index) ++c.track;
      if (c.track == index) {
        Clip k = c;
        k.id = 0;
        k.track = index + 1;
        copies.push_back(k);
      }
    }
    for (auto& k : copies) {
      k.id = x.allocateId();
      x.playlist.push_back(k);
    }
  });
  return true;
}

bool clearPlaylistTrack(Document& d, int index) {
  bool any = false;
  for (const auto& c : d.song().playlist) any = any || c.track == index;
  if (!any) return false;
  d.editSong("clear playlist track", [&](Song& x) { eraseIf<Clip>(x.playlist, [&](const Clip& c) { return c.track == index; }); });
  return true;
}

bool removeClips(Document& d, const std::set<ObjectId>& clips) {
  bool any = false;
  for (ObjectId c : clips) any = any || d.song().clip(c);
  if (!any) return false;
  d.editSong(clips.size() == 1 ? "delete clip" : "delete clips",
             [&](Song& x) { eraseIf<Clip>(x.playlist, [&](const Clip& c) { return clips.count(c.id) > 0; }); });
  return true;
}

std::vector<ObjectId> duplicateClips(Document& d, const std::set<ObjectId>& clips) {
  const Song& s = d.song();
  Tick lo = -1, hi = 0;
  for (const auto& c : s.playlist)
    if (clips.count(c.id)) {
      lo = lo < 0 ? c.start : std::min(lo, c.start);
      hi = std::max(hi, c.start + s.clipLength(c));
    }
  if (lo < 0) return {};
  Tick shift = hi - lo;
  std::vector<ObjectId> out;
  d.editSong("duplicate clips", [&](Song& x) {
    std::vector<Clip> add;
    for (const auto& c : x.playlist)
      if (clips.count(c.id)) {
        Clip k = c;
        k.start += shift;
        add.push_back(k);
      }
    std::map<ObjectId, ObjectId> groups;  // Q-3: grouped copies form new groups
    for (auto& k : add) {
      if (k.group != kNoId) {
        auto [it, fresh] = groups.emplace(k.group, kNoId);
        if (fresh) it->second = x.allocateId();
        k.group = it->second;
      }
      k.takeGroup = kNoId;
      k.id = x.allocateId();
      out.push_back(k.id);
      x.playlist.push_back(k);
    }
  });
  return out;
}

ObjectId makeClipUnique(Document& d, ObjectId clip) {
  const Song& s = d.song();
  const Clip* c = s.clip(clip);
  if (!c || !s.pattern(c->pattern) || static_cast<int>(s.patterns.size()) >= kMaxPatterns) return kNoId;  // pattern clips only
  ObjectId out = kNoId;
  d.editSong("make clip unique", [&](Song& x) {
    Clip* k = x.clip(clip);
    int idx = x.patternIndex(k->pattern);
    Pattern np = x.patterns[static_cast<size_t>(idx)];
    np.id = x.allocateId();
    np.name = np.name + " #2";
    x.patterns.insert(x.patterns.begin() + idx + 1, np);
    k = x.clip(clip);
    k->pattern = np.id;
    out = np.id;
  });
  return out;
}

ObjectId mergeClips(Document& d, const std::set<ObjectId>& clips) {
  const Song& s = d.song();
  std::vector<Clip> sel;
  for (const auto& c : s.playlist)
    if (clips.count(c.id) && s.pattern(c.pattern)) sel.push_back(c);  // pattern clips only (audio / automation stay)
  if (sel.empty() || static_cast<int>(s.patterns.size()) >= kMaxPatterns) return kNoId;
  Tick lo = sel[0].start, hi = 0;
  int track = sel[0].track;
  for (const auto& c : sel) {
    lo = std::min(lo, c.start);
    hi = std::max(hi, c.start + s.clipLength(c));
    track = std::min(track, c.track);
  }
  ObjectId out = kNoId;
  d.editSong("merge pattern clips", [&](Song& x) {
    Pattern np;
    np.id = x.allocateId();
    np.name = x.pattern(sel[0].pattern)->name + " (merged)";
    np.color = x.pattern(sel[0].pattern)->color;
    np.stepsPerBar = 16;
    Tick bar = std::max<Tick>(1, x.barTicks());
    np.bars = static_cast<int>(std::clamp<Tick>((hi - lo + bar - 1) / bar, 1, kMaxPatternBars));  // longer content still plays
    for (const auto& c : sel) {
      const Pattern& p = *x.pattern(c.pattern);
      Tick clipLen = x.clipLength(c), patLen = std::max<Tick>(1, x.patternContentLength(p)), tps = x.ticksPerStep(p);
      for (Tick rep = 0; rep < clipLen; rep += patLen) {
        Tick off = c.start - lo + rep;
        for (const auto& [ch, ns] : p.notes)
          for (const auto& n : ns)
            if (rep + n.start < clipLen) {
              Note c = n;  // keeps the note's properties
              c.start = off + n.start;
              c.length = std::min(n.length, clipLen - rep - n.start);
              np.notes[ch].push_back(c);
            }
        if (tps > 0)
          for (const auto& [ch, row] : p.steps) {
            std::vector<StepProps> props;
            if (auto pt = p.stepProps.find(ch); pt != p.stepProps.end()) props = pt->second;
            for (size_t k = 0; k < row.size(); ++k) {
              Tick t = static_cast<Tick>(k) * tps;
              if (!row[k] || rep + t >= clipLen) continue;
              const StepProps sp = k < props.size() ? props[k] : StepProps{};
              Note c(off + t + tps * sp.shift / kStepShiftMax, tps, sp.pitch < 0 ? 60 : sp.pitch, row[k]);
              c.pan = sp.pan, c.release = sp.release, c.fine = sp.fine, c.modX = sp.modX, c.modY = sp.modY;
              np.notes[ch].push_back(c);
            }
          }
      }
    }
    for (auto& [ch, ns] : np.notes)
      std::stable_sort(ns.begin(), ns.end(), [](const Note& a, const Note& b) {
        return a.start != b.start ? a.start < b.start : a.pitch < b.pitch;
      });
    x.patterns.push_back(np);
    eraseIf<Clip>(x.playlist, [&](const Clip& c) { return clips.count(c.id) > 0 && c.kind == ClipKind::Pattern; });
    Clip nc;
    nc.id = x.allocateId();
    nc.pattern = np.id;
    nc.track = track;
    nc.start = lo;
    nc.length = hi - lo;
    x.playlist.push_back(nc);
    out = nc.id;
  });
  return out;
}

std::vector<ObjectId> chopClip(Document& d, ObjectId clip, Tick every) {
  const Song& s = d.song();
  const Clip* c = s.clip(clip);
  if (!c || !s.pattern(c->pattern)) return {};  // pattern clips only
  Tick len = s.clipLength(*c), patLen = s.patternContentLength(*s.pattern(c->pattern));
  // Q-1: a looping clip splits at its loop points (each piece one pass, its
  // loop start kept).
  if (const Tick loop = clipLoopLength(s, *c); loop > 0 && every == loop) {
    const std::vector<ClipPass> passes = clipPasses(s, *c);
    if (passes.size() < 2) return {};
    std::vector<ObjectId> out;
    d.editSong("chop clip", [&](Song& x) {
      Clip base = *x.clip(clip);
      eraseIf<Clip>(x.playlist, [&](const Clip& k) { return k.id == clip; });
      for (const ClipPass& p : passes) {
        Clip k = base;
        k.id = p.index == 0 ? base.id : x.allocateId();
        k.start = base.start + p.at;
        k.length = p.len;
        k.offset = p.content;
        x.playlist.push_back(k);
        out.push_back(k.id);
      }
    });
    return out;
  }
  // Pieces must start on a pattern repeat (they keep the clip's offset).
  if (every <= 0 || patLen <= 0 || every % patLen != 0 || every >= len) return {};
  std::vector<ObjectId> out;
  d.editSong("chop clip", [&](Song& x) {
    Clip base = *x.clip(clip);
    eraseIf<Clip>(x.playlist, [&](const Clip& k) { return k.id == clip; });
    for (Tick t = 0; t < len; t += every) {
      Clip k = base;
      k.id = t == 0 ? base.id : x.allocateId();
      k.start = base.start + t;
      k.length = std::min(every, len - t);
      x.playlist.push_back(k);
      out.push_back(k.id);
    }
  });
  return out;
}

// -------------------------------------------------------------------- mixer ----

bool moveMixerTrack(Document& d, ObjectId id, int dir) {
  const Song& s = d.song();
  int i = s.mixerIndex(id), j = i + dir;
  if (i < 1 || j < 1 || j >= static_cast<int>(s.mixer.size()) || dir == 0) return false;
  d.editSong("move mixer track", [&](Song& x) { std::swap(x.mixer[static_cast<size_t>(i)], x.mixer[static_cast<size_t>(j)]); });
  return true;
}

int trimUnusedMixerTracks(Document& d) {
  int n = 0;
  const Song& s = d.song();
  for (size_t i = s.mixer.size(); i > 1 && mixerTrackIsFree(s, s.mixer[i - 1].id); --i) ++n;
  if (!n) return 0;
  d.editSong("trim unused mixer tracks", [&](Song& x) { x.mixer.resize(x.mixer.size() - static_cast<size_t>(n)); });
  return n;
}

bool setSlotEnabled(Document& d, ObjectId track, int slot, bool enabled) {
  if (slot < 0 || slot >= kEffectSlots) return false;
  return d.editMixerTrack(track, enabled ? "enable effect" : "disable effect",
                          [&](MixerTrack& m) { m.slots[static_cast<size_t>(slot)].enabled = enabled; });
}

bool soloMixerTrack(Document& d, ObjectId id) {
  const Song& s = d.song();
  if (!s.mixerTrack(id)) return false;
  bool only = s.mixerTrack(id)->solo;
  for (const auto& m : s.mixer) only = only && (m.id == id || !m.solo);
  d.editSong(only ? "unsolo mixer track" : "solo mixer track", [&](Song& x) {
    for (auto& m : x.mixer) m.solo = !only && m.id == id, m.soloAlone = false;  // J-31: a plain solo
  });
  return true;
}

// -------------------------------------------------------------------- notes ----

void sortNotes(std::vector<Note>& notes, NoteSel& sel) {
  sel.resize(notes.size(), false);
  std::vector<size_t> idx(notes.size());
  for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
  std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
    return notes[a].start != notes[b].start ? notes[a].start < notes[b].start : notes[a].pitch < notes[b].pitch;
  });
  std::vector<Note> n2;
  NoteSel s2;
  for (size_t i : idx) {
    n2.push_back(notes[i]);
    s2.push_back(sel[i]);
  }
  notes.swap(n2);
  sel.swap(s2);
}

void quantize(std::vector<Note>& notes, NoteSel& sel, Tick grid, bool ends) {
  auto t = targets(notes, sel);
  for (size_t i = 0; i < notes.size(); ++i) {
    if (!t[i]) continue;
    Note& n = notes[i];
    Tick end = n.start + n.length;
    n.start = std::max<Tick>(0, roundTo(n.start, grid));
    if (ends) {
      Tick e = roundTo(end, grid);
      n.length = std::max<Tick>(std::max<Tick>(1, grid), e - n.start);
    }
  }
  sortNotes(notes, sel);
}

void legato(std::vector<Note>& notes, NoteSel& sel) {
  sortNotes(notes, sel);
  auto t = targets(notes, sel);
  for (size_t i = 0; i < notes.size(); ++i) {
    if (!t[i]) continue;
    for (size_t j = i + 1; j < notes.size(); ++j)
      if (t[j] && notes[j].start > notes[i].start) {
        notes[i].length = notes[j].start - notes[i].start;
        break;
      }
  }
}

void chop(std::vector<Note>& notes, NoteSel& sel, Tick len) {
  if (len <= 0) return;
  auto t = targets(notes, sel);
  std::vector<Note> out;
  NoteSel os;
  for (size_t i = 0; i < notes.size(); ++i) {
    const Note& n = notes[i];
    if (!t[i] || n.length <= len) {
      out.push_back(n);
      os.push_back(sel[i]);
      continue;
    }
    for (Tick s = 0; s < n.length; s += len) {
      Note c = n;  // pieces keep the note's properties
      c.start = n.start + s;
      c.length = std::min(len, n.length - s);
      out.push_back(c);
      os.push_back(sel[i]);
    }
  }
  notes.swap(out);
  sel.swap(os);
  sortNotes(notes, sel);
}

void glue(std::vector<Note>& notes, NoteSel& sel) {
  sortNotes(notes, sel);
  auto t = targets(notes, sel);
  std::vector<bool> gone(notes.size(), false);
  for (size_t i = 0; i < notes.size(); ++i) {
    if (!t[i] || gone[i]) continue;
    for (size_t j = i + 1; j < notes.size(); ++j) {
      if (!t[j] || gone[j] || notes[j].pitch != notes[i].pitch) continue;
      Tick end = notes[i].start + notes[i].length;
      if (notes[j].start > end) break;
      notes[i].length = std::max(end, notes[j].start + notes[j].length) - notes[i].start;
      gone[j] = true;
    }
  }
  std::vector<Note> out;
  NoteSel os;
  for (size_t i = 0; i < notes.size(); ++i)
    if (!gone[i]) out.push_back(notes[i]), os.push_back(sel[i]);
  notes.swap(out);
  sel.swap(os);
}

void transpose(std::vector<Note>& notes, NoteSel& sel, int semitones) {
  auto t = targets(notes, sel);
  for (size_t i = 0; i < notes.size(); ++i)
    if (t[i]) notes[i].pitch = std::clamp(notes[i].pitch + semitones, 0, 127);
  sortNotes(notes, sel);
}

void nudge(std::vector<Note>& notes, NoteSel& sel, Tick delta) {
  auto t = targets(notes, sel);
  for (size_t i = 0; i < notes.size(); ++i)
    if (t[i]) notes[i].start = std::max<Tick>(0, notes[i].start + delta);
  sortNotes(notes, sel);
}

void flipVertical(std::vector<Note>& notes, NoteSel& sel) {
  auto t = targets(notes, sel);
  int lo = 127, hi = 0;
  for (size_t i = 0; i < notes.size(); ++i)
    if (t[i]) lo = std::min(lo, notes[i].pitch), hi = std::max(hi, notes[i].pitch);
  for (size_t i = 0; i < notes.size(); ++i)
    if (t[i]) notes[i].pitch = std::clamp(lo + hi - notes[i].pitch, 0, 127);
  sortNotes(notes, sel);
}

void flipHorizontal(std::vector<Note>& notes, NoteSel& sel) {
  auto t = targets(notes, sel);
  Tick lo = -1, hi = 0;
  for (size_t i = 0; i < notes.size(); ++i)
    if (t[i]) {
      lo = lo < 0 ? notes[i].start : std::min(lo, notes[i].start);
      hi = std::max(hi, notes[i].start + notes[i].length);
    }
  if (lo < 0) return;
  for (size_t i = 0; i < notes.size(); ++i)
    if (t[i]) notes[i].start = lo + hi - (notes[i].start + notes[i].length);
  sortNotes(notes, sel);
}

void scaleVelocity(std::vector<Note>& notes, NoteSel& sel, double factor) {
  auto t = targets(notes, sel);
  for (size_t i = 0; i < notes.size(); ++i)
    if (t[i]) notes[i].velocity = std::clamp(static_cast<int>(std::lround(notes[i].velocity * factor)), 1, 127);
}

void humanize(std::vector<Note>& notes, NoteSel& sel, int velRange, Tick timeRange, uint32_t seed) {
  auto t = targets(notes, sel);
  uint32_t st = seed ? seed : 1;
  for (size_t i = 0; i < notes.size(); ++i) {
    if (!t[i]) continue;
    if (velRange > 0) {
      int dv = static_cast<int>(lcg(st) % static_cast<uint32_t>(2 * velRange + 1)) - velRange;
      notes[i].velocity = std::clamp(notes[i].velocity + dv, 1, 127);
    }
    if (timeRange > 0) {
      Tick dt = static_cast<Tick>(lcg(st) % static_cast<uint32_t>(2 * timeRange + 1)) - timeRange;
      notes[i].start = std::max<Tick>(0, notes[i].start + dt);
    }
  }
  sortNotes(notes, sel);
}

void strum(std::vector<Note>& notes, NoteSel& sel, Tick step) {
  sortNotes(notes, sel);  // same start: ascending pitch
  auto t = targets(notes, sel);
  std::map<Tick, int> count;
  for (size_t i = 0; i < notes.size(); ++i) {
    if (!t[i]) continue;
    int k = count[notes[i].start]++;
    Tick off = step * k;
    notes[i].start += off;
    notes[i].length = std::max<Tick>(1, notes[i].length - off);
  }
  sortNotes(notes, sel);
}

void arpeggiate(std::vector<Note>& notes, NoteSel& sel, Tick step) {
  if (step <= 0) return;
  sortNotes(notes, sel);
  auto t = targets(notes, sel);
  std::map<Tick, std::vector<size_t>> chords;
  for (size_t i = 0; i < notes.size(); ++i)
    if (t[i]) chords[notes[i].start].push_back(i);
  std::vector<bool> gone(notes.size(), false);
  std::vector<Note> add;
  NoteSel addSel;
  for (auto& [start, idx] : chords) {
    if (idx.size() < 2) continue;
    Tick len = 0;
    for (size_t i : idx) len = std::max(len, notes[i].length), gone[i] = true;
    int k = 0;
    for (Tick at = 0; at < len; at += step, ++k) {
      const Note& src = notes[idx[static_cast<size_t>(k) % idx.size()]];
      Note c = src;
      c.start = start + at;
      c.length = std::min(step, len - at);
      add.push_back(c);
      addSel.push_back(sel[idx[0]]);
    }
  }
  std::vector<Note> out;
  NoteSel os;
  for (size_t i = 0; i < notes.size(); ++i)
    if (!gone[i]) out.push_back(notes[i]), os.push_back(sel[i]);
  out.insert(out.end(), add.begin(), add.end());
  os.insert(os.end(), addSel.begin(), addSel.end());
  notes.swap(out);
  sel.swap(os);
  sortNotes(notes, sel);
}

void duplicate(std::vector<Note>& notes, NoteSel& sel, Tick grid) {
  auto t = targets(notes, sel);
  Tick lo = -1, hi = 0;
  for (size_t i = 0; i < notes.size(); ++i)
    if (t[i]) {
      lo = lo < 0 ? notes[i].start : std::min(lo, notes[i].start);
      hi = std::max(hi, notes[i].start + notes[i].length);
    }
  if (lo < 0) return;
  Tick span = hi - lo;
  if (grid > 1) span = (span + grid - 1) / grid * grid;
  size_t n = notes.size();
  NoteSel ns(n, false);
  for (size_t i = 0; i < n; ++i)
    if (t[i]) {
      Note c = notes[i];
      c.start += span;
      notes.push_back(c);
      ns.push_back(true);
    }
  sel.swap(ns);
  sortNotes(notes, sel);
}

void deleteNotes(std::vector<Note>& notes, NoteSel& sel) {
  auto t = targets(notes, sel);
  std::vector<Note> out;
  for (size_t i = 0; i < notes.size(); ++i)
    if (!t[i]) out.push_back(notes[i]);
  notes.swap(out);
  sel.assign(notes.size(), false);
}

void selectAll(const std::vector<Note>& notes, NoteSel& sel) { sel.assign(notes.size(), true); }
void selectNone(NoteSel& sel) { std::fill(sel.begin(), sel.end(), false); }
void invertSelection(NoteSel& sel) { sel.flip(); }

void selectOdd(const std::vector<Note>& notes, NoteSel& sel) {
  sel.assign(notes.size(), false);
  for (size_t i = 0; i < notes.size(); i += 2) sel[i] = true;
}

void selectSamePitch(const std::vector<Note>& notes, NoteSel& sel) {
  sel.resize(notes.size(), false);
  std::set<int> pitches;
  for (size_t i = 0; i < notes.size(); ++i)
    if (sel[i]) pitches.insert(notes[i].pitch);
  for (size_t i = 0; i < notes.size(); ++i) sel[i] = pitches.count(notes[i].pitch) > 0;
}

void selectOverlapping(const std::vector<Note>& notes, NoteSel& sel) {
  sel.assign(notes.size(), false);
  for (size_t i = 0; i < notes.size(); ++i)
    for (size_t j = i + 1; j < notes.size(); ++j) {
      const Note &a = notes[i], &b = notes[j];
      if (a.pitch == b.pitch && b.start < a.start + a.length && a.start < b.start + b.length) sel[i] = sel[j] = true;
    }
}

void selectRandom(const std::vector<Note>& notes, NoteSel& sel, uint32_t seed) {
  uint32_t st = seed ? seed : 1;
  sel.assign(notes.size(), false);
  for (size_t i = 0; i < notes.size(); ++i) sel[i] = (lcg(st) & 1u) != 0;
}

// ---------------------------------------------------------------- progressions ----

const std::vector<Progression>& progressions() {
  static const std::vector<Progression> p = {
      {"Anthem  (I V vi IV)", {1, 5, 6, 4}, false},
      {"Fifties  (I vi IV V)", {1, 6, 4, 5}, false},
      {"Ballad  (vi IV I V)", {6, 4, 1, 5}, false},
      {"Turnaround  (ii V I I)", {2, 5, 1, 1}, false},
      {"Twelve-bar seed  (I IV I V)", {1, 4, 1, 5}, false},
      {"Night drive  (i VI III VII)", {1, 6, 3, 7}, true},
      {"Lament  (i iv VI v)", {1, 4, 6, 5}, true},
      {"Pulse  (i VII VI VII)", {1, 7, 6, 7}, true},
  };
  return p;
}

namespace {
// Diatonic triad on a scale degree (J-31: from ChordTools), ascending from `root`.
std::vector<int> triad(int degree, int root, bool minor) {
  chord::ChordProgression p;
  p.key = ((root % 12) + 12) % 12;
  p.scale = minor ? 2 : 1;  // tools::scales(): 1 major, 2 natural minor
  p.octave = (root - p.key) / 12;
  p.chords.push_back(chord::diatonicChord(p.key, p.scale, degree));
  return chord::voice(p, 0).main;
}
}  // namespace

std::vector<Note> progressionNotes(const Progression& p, int root, Tick barTicks, int bars, int velocity) {
  std::vector<Note> out;
  if (p.degrees.empty() || barTicks <= 0) return out;
  for (int b = 0; b < bars; ++b) {
    auto ch = triad(p.degrees[static_cast<size_t>(b) % p.degrees.size()], root, p.minor);
    Tick t = barTicks * b;
    out.push_back({t, barTicks, std::clamp(ch[0] - 12, 0, 127), velocity});  // bass
    for (int n : ch) out.push_back({t, barTicks, std::clamp(n, 0, 127), velocity});
  }
  std::stable_sort(out.begin(), out.end(), [](const Note& a, const Note& b) {
    return a.start != b.start ? a.start < b.start : a.pitch < b.pitch;
  });
  return out;
}

std::vector<Note> progressionRiff(const Progression& p, int root, Tick barTicks, Tick stepTicks, int bars, int velocity) {
  std::vector<Note> out;
  if (p.degrees.empty() || barTicks <= 0 || stepTicks <= 0) return out;
  for (int b = 0; b < bars; ++b) {
    auto ch = triad(p.degrees[static_cast<size_t>(b) % p.degrees.size()], root, p.minor);
    ch.push_back(ch[0] + 12);
    ch.push_back(ch[2]);
    ch.push_back(ch[1]);
    int k = 0;
    for (Tick t = 0; t < barTicks; t += stepTicks, ++k)
      out.push_back({barTicks * b + t, std::min(stepTicks, barTicks - t), std::clamp(ch[static_cast<size_t>(k) % ch.size()], 0, 127),
                     k % 4 == 0 ? velocity : std::max(1, velocity - 20)});
  }
  return out;
}

}  // namespace gl::ops
