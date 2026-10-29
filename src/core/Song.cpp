#include "Song.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <set>

namespace gl {

namespace {
template <typename V>
int indexOf(const V& v, ObjectId id) {
  for (size_t i = 0; i < v.size(); ++i)
    if (v[i].id == id) return static_cast<int>(i);
  return -1;
}

// Visits every tick value of the song (positions and lengths).
template <typename S, typename F>
void forEachTick(S& song, F&& fn) {
  for (auto& p : song.patterns)
    for (auto& [ch, ns] : p.notes)
      for (auto& n : ns) {
        fn(n.start);
        fn(n.length);
      }
  for (auto& c : song.playlist) {
    fn(c.start);
    fn(c.length);
    fn(c.offset);  // arrangement: content offset
  }
  // arrangement: automation points and markers
  for (auto& a : song.automations) {
    for (auto& pt : a.points) fn(pt.pos);
    fn(a.loop);
  }
  for (auto& m : song.markers) fn(m.pos);
  // pattern automation, parked arrangements, live area
  for (auto& p : song.patterns)
    for (auto& lane : p.automation) {
      for (auto& pt : lane.points) fn(pt.pos);
      fn(lane.loop);  // J-173: pattern automation clips loop too
    }
  for (size_t i = 0; i < song.arrangements.size(); ++i) {
    if (static_cast<int>(i) == song.currentArrangement) continue;
    auto& a = song.arrangements[i];
    for (auto& c : a.playlist) {
      fn(c.start);
      fn(c.length);
      fn(c.offset);
    }
    for (auto& m : a.markers) fn(m.pos);
    fn(a.live.areaEnd);
  }
  fn(song.live.areaEnd);
}
}  // namespace

Tick Song::patternContentLength(const Pattern& p) const {
  Tick len = patternLength(p);
  Tick bar = barTicks();
  Tick end = 0;
  for (const auto& [ch, ns] : p.notes)
    for (const auto& n : ns) end = std::max(end, n.start + n.length);
  if (end > len && bar > 0) len = (end + bar - 1) / bar * bar;  // round up to whole bars
  return len;
}

Tick Song::clipLength(const Clip& c) const {
  if (c.length > 0) return c.length;
  if (c.kind == ClipKind::Automation) {  // arrangement: up to the last point (at least one beat)
    const AutomationClip* a = automation(c.source);
    Tick end = a && !a->points.empty() ? a->points.back().pos - c.offset : 0;
    return std::max<Tick>(end, beatTicks());
  }
  if (c.kind == ClipKind::Audio) return barTicks();  // audio clips always store their length
  const Pattern* p = pattern(c.pattern);
  return p ? patternContentLength(*p) : 0;
}

Tick Song::lengthTicks() const {
  Tick end = 0;
  for (const auto& c : playlist) end = std::max(end, c.start + clipLength(c));
  return end;
}

Channel* Song::channel(ObjectId id) {
  int i = indexOf(channels, id);
  return i < 0 ? nullptr : &channels[static_cast<size_t>(i)];
}
const Channel* Song::channel(ObjectId id) const {
  int i = indexOf(channels, id);
  return i < 0 ? nullptr : &channels[static_cast<size_t>(i)];
}
Pattern* Song::pattern(ObjectId id) {
  int i = indexOf(patterns, id);
  return i < 0 ? nullptr : &patterns[static_cast<size_t>(i)];
}
const Pattern* Song::pattern(ObjectId id) const {
  int i = indexOf(patterns, id);
  return i < 0 ? nullptr : &patterns[static_cast<size_t>(i)];
}
Clip* Song::clip(ObjectId id) {
  int i = indexOf(playlist, id);
  return i < 0 ? nullptr : &playlist[static_cast<size_t>(i)];
}
const Clip* Song::clip(ObjectId id) const {
  int i = indexOf(playlist, id);
  return i < 0 ? nullptr : &playlist[static_cast<size_t>(i)];
}
MixerTrack* Song::mixerTrack(ObjectId id) {
  int i = indexOf(mixer, id);
  return i < 0 ? nullptr : &mixer[static_cast<size_t>(i)];
}
const MixerTrack* Song::mixerTrack(ObjectId id) const {
  int i = indexOf(mixer, id);
  return i < 0 ? nullptr : &mixer[static_cast<size_t>(i)];
}
AutomationClip* Song::automation(ObjectId id) {
  int i = indexOf(automations, id);
  return i < 0 ? nullptr : &automations[static_cast<size_t>(i)];
}
const AutomationClip* Song::automation(ObjectId id) const {
  int i = indexOf(automations, id);
  return i < 0 ? nullptr : &automations[static_cast<size_t>(i)];
}
int Song::channelIndex(ObjectId id) const { return indexOf(channels, id); }
int Song::patternIndex(ObjectId id) const { return indexOf(patterns, id); }
int Song::clipIndex(ObjectId id) const { return indexOf(playlist, id); }
int Song::mixerIndex(ObjectId id) const { return indexOf(mixer, id); }

std::vector<int> flRouteOutputs(const Song& song, int mixerIndex) {
  std::vector<int> r;
  if (mixerIndex < 0 || static_cast<size_t>(mixerIndex) >= song.mixer.size()) return r;
  for (const Send& s : song.mixer[static_cast<size_t>(mixerIndex)].sends) {
    const int to = song.mixerIndex(s.to);
    if (to >= 0 && to != mixerIndex) r.push_back(to);
  }
  std::sort(r.begin(), r.end());
  r.erase(std::unique(r.begin(), r.end()), r.end());
  r.insert(r.begin(), mixerIndex);
  return r;
}

std::vector<int> flRouteInputs(const Song& song, int mixerIndex) {
  std::vector<int> r;
  if (mixerIndex < 0 || static_cast<size_t>(mixerIndex) >= song.mixer.size()) return r;
  const ObjectId id = song.mixer[static_cast<size_t>(mixerIndex)].id;
  r.push_back(mixerIndex);
  for (size_t i = 0; i < song.mixer.size(); ++i) {
    if (static_cast<int>(i) == mixerIndex) continue;
    for (const Send& s : song.mixer[i].sends)
      if (s.to == id) {
        r.push_back(static_cast<int>(i));
        break;
      }
  }
  return r;
}

int flRouteOutput(const Song& song, int mixerIndex, int n) {
  if (n <= 0) return -1;
  const std::vector<int> r = flRouteOutputs(song, mixerIndex);
  if (r.empty()) return -1;
  const int to = r[std::min(static_cast<size_t>(n), r.size() - 1)];
  return to == mixerIndex ? -1 : to;
}

int flRouteInput(const Song& song, int mixerIndex, int n) {
  if (n <= 0) return -1;
  const std::vector<int> r = flRouteInputs(song, mixerIndex);
  return static_cast<size_t>(n) < r.size() ? r[static_cast<size_t>(n)] : -1;
}

void ensureMixer(Song& song, int inserts) {
  if (!song.mixer.empty()) return;
  MixerTrack master;
  master.id = song.allocateId();
  master.name = "Master";
  song.mixer.push_back(master);
  for (int i = 0; i < inserts; ++i) {
    MixerTrack t;
    t.id = song.allocateId();
    t.sends.push_back({master.id, 1.0f, false});
    song.mixer.push_back(t);
  }
}

Song makeEmptySong() {
  Song s;
  ensureMixer(s);
  Pattern p;
  p.id = s.allocateId();
  p.name = "Pattern 1";
  s.patterns.push_back(p);
  return s;
}

bool canRoute(const Song& song, ObjectId from, ObjectId to) {
  if (from == to || !song.mixerTrack(from) || !song.mixerTrack(to)) return false;
  if (to != song.masterId() && from == song.masterId()) return false;  // master feeds the output only
  // Cycle if `from` is reachable from `to`.
  std::set<ObjectId> seen;
  std::function<bool(ObjectId)> reaches = [&](ObjectId x) {
    if (x == from) return true;
    if (!seen.insert(x).second) return false;
    const MixerTrack* t = song.mixerTrack(x);
    if (!t) return false;
    for (const auto& s : t->sends)
      if (reaches(s.to)) return true;
    return false;
  };
  return !reaches(to);
}

std::vector<ObjectId> mixerOrder(const Song& song) {
  // Depth-first post-order over the "sends to" graph from every track,
  // reversed, gives sources before destinations.
  std::vector<ObjectId> out;
  std::set<ObjectId> done, active;
  std::function<void(ObjectId)> visit = [&](ObjectId id) {
    if (done.count(id) || active.count(id)) return;  // `active` guards against bad (cyclic) data
    active.insert(id);
    if (const MixerTrack* t = song.mixerTrack(id))
      for (const auto& s : t->sends) visit(s.to);
    active.erase(id);
    done.insert(id);
    out.push_back(id);
  };
  for (const auto& t : song.mixer) visit(t.id);
  std::reverse(out.begin(), out.end());
  // Keep the master last even if nothing routes to it.
  ObjectId m = song.masterId();
  auto it = std::find(out.begin(), out.end(), m);
  if (it != out.end()) {
    out.erase(it);
    out.push_back(m);
  }
  return out;
}

int countPpqRoundings(const Song& song, int newPpq) {
  int n = 0;
  forEachTick(song, [&](Tick t) {
    if ((static_cast<long double>(t) * newPpq / song.ppq) != rescaleTick(t, song.ppq, newPpq)) ++n;
  });
  return n;
}

int changePpq(Song& song, int newPpq) {
  int n = countPpqRoundings(song, newPpq);
  int old = song.ppq;
  std::vector<bool> explicitLen;
  for (const auto& c : song.playlist) explicitLen.push_back(c.length > 0);
  forEachTick(song, [&](Tick& t) { t = rescaleTick(t, old, newPpq); });
  // Notes and explicit clip lengths must stay at least one tick long
  // (a clip length of 0 means "pattern length").
  for (auto& p : song.patterns)
    for (auto& [ch, ns] : p.notes)
      for (auto& note : ns) note.length = std::max<Tick>(note.length, 1);
  for (size_t i = 0; i < song.playlist.size(); ++i)
    if (explicitLen[i]) song.playlist[i].length = std::max<Tick>(song.playlist[i].length, 1);
  song.ppq = newPpq;
  return n;
}

const ModLink* findModLink(const Song& song, ObjectId owner, uint32_t index) {
  for (const auto& l : song.modLinks)
    if (l.owner == owner && l.index == index) return &l;
  return nullptr;
}

bool isControllerSource(const Song& song, ObjectId owner, int slot) {
  const PluginState* ps = nullptr;
  if (slot < 0) {
    if (const Channel* c = song.channel(owner)) ps = &c->plugin;
  } else if (const MixerTrack* m = song.mixerTrack(owner)) {
    if (static_cast<size_t>(slot) < m->slots.size()) ps = &m->slots[static_cast<size_t>(slot)].plugin;
  }
  if (!ps || ps->empty()) return false;
  const PluginInfo* info = findPlugin(ps->type);
  return info && !info->outputs.empty();
}

int pruneModLinks(Song& song) {
  size_t before = song.modLinks.size();
  auto targetOk = [&](const ModLink& l) {
    if (l.owner == kSongId) return true;
    return song.channel(l.owner) != nullptr || song.mixerTrack(l.owner) != nullptr;
  };
  song.modLinks.erase(std::remove_if(song.modLinks.begin(), song.modLinks.end(),
                                     [&](const ModLink& l) {
                                       return !isControllerSource(song, l.sourceOwner, l.sourceSlot) || !targetOk(l);
                                     }),
                      song.modLinks.end());
  return static_cast<int>(before - song.modLinks.size());
}

double applyModCurve(ModCurve c, double x) {
  x = x < 0 ? 0 : (x > 1 ? 1 : x);
  switch (c) {
    case ModCurve::Linear: return x;
    case ModCurve::Inverted: return 1.0 - x;
    case ModCurve::Log: return std::log1p(x * 99.0) / std::log(100.0);           // fast rise
    case ModCurve::InvLog: return (std::pow(100.0, x) - 1.0) / 99.0;             // slow rise
    case ModCurve::Switch: return x >= 0.5 ? 1.0 : 0.0;
    case ModCurve::Steps: return std::min(1.0, std::floor(x * 8.0) / 7.0);
    case ModCurve::FirstHalf: return std::min(1.0, x * 2.0);
    case ModCurve::LastHalf: return std::max(0.0, x * 2.0 - 1.0);
    case ModCurve::DeadZone: {
      constexpr double z = 0.1;  // centre dead zone
      if (std::fabs(x - 0.5) < z * 0.5) return 0.5;
      return x < 0.5 ? x / (1.0 - z) : 0.5 + (x - 0.5 - z * 0.5) / (1.0 - z);
    }
    default: return x;
  }
}

const char* modCurveName(ModCurve c) {
  static const char* names[] = {"Linear", "Inverted", "Fast rise", "Slow rise", "Switch", "8 steps",
                                "First half", "Second half", "Centre dead zone", "Formula"};
  int i = static_cast<int>(c);
  return i >= 0 && i < static_cast<int>(ModCurve::Count) ? names[i] : "?";
}

std::string mixerTrackName(const Song& song, size_t index) {
  if (index < song.mixer.size() && !song.mixer[index].name.empty()) return song.mixer[index].name;
  return index == 0 ? "Master" : "Insert " + std::to_string(index);
}

std::string playlistTrackName(const Song& song, size_t index) {
  if (index < song.tracks.size() && !song.tracks[index].name.empty()) return song.tracks[index].name;
  return "Track " + std::to_string(index + 1);
}

}  // namespace gl
