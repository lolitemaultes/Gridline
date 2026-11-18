// Gridline - tracks from the playlist on the Stage (see StagePick.h).
#include "StagePick.h"
#include <algorithm>
#include <cmath>
#include <set>
#include "Meter.h"
#include "NoteTools.h"
#include "Params.h"
#include "StageRoomOps.h"

namespace gl::stagepick {

namespace {

bool laneMuted(const Song& s, int lane) { return lane >= 0 && lane < static_cast<int>(s.tracks.size()) && s.tracks[static_cast<size_t>(lane)].muted; }

// The tracks one channel plays into (its mixer track and its extra outputs). False: it plays straight
// into the master (or it is gone).
bool channelTracks(const Song& s, ObjectId channel, std::set<ObjectId>& out) {
  const Channel* c = s.channel(channel);
  if (!c) {
    return false;
  }
  bool any = false;
  auto add = [&](ObjectId t) {
    if (t == kNoId || t == s.masterId() || !s.mixerTrack(t) || roomops::isSpaceTrack(s, t)) {
      return;
    }
    out.insert(t);
    any = true;
  };
  add(c->mixerTrack);
  for (ObjectId a : c->auxOuts) {
    add(a);
  }
  return any;
}

// What one clip plays through. False: nothing of its own (an automation clip, an empty pattern, or
// channels that play straight into the master).
bool clipTracks(const Song& s, const Clip& c, std::set<ObjectId>& out, bool& toMaster) {
  toMaster = false;
  if (c.kind == ClipKind::Audio) {
    const bool any = channelTracks(s, c.source, out);
    toMaster = !any && s.channel(c.source) != nullptr;
    return any;
  }
  if (c.kind != ClipKind::Pattern) {
    return false;
  }
  const Pattern* p = s.pattern(c.pattern);
  if (!p) {
    return false;
  }
  bool any = false, master = false;
  auto one = [&](ObjectId ch) {
    if (channelTracks(s, ch, out)) {
      any = true;
    } else if (s.channel(ch)) {
      master = true;
    }
  };
  for (const auto& [ch, ns] : p->notes) {
    if (!ns.empty()) {
      one(ch);
    }
  }
  for (const auto& [ch, st] : p->steps) {
    bool on = false;
    for (uint8_t v : st) {
      on = on || v != 0;
    }
    if (on) {
      one(ch);
    }
  }
  toMaster = !any && master;
  return any;
}

// `direct` plus, with buses, every track they send into (never the master or a space), mixer order.
Found finish(const Song& s, const std::set<ObjectId>& direct, const Options& o, Found f) {
  std::set<ObjectId> all = direct, bus;
  if (o.buses) {
    std::vector<ObjectId> todo(direct.begin(), direct.end());
    while (!todo.empty()) {
      const MixerTrack* m = s.mixerTrack(todo.back());
      todo.pop_back();
      if (!m) {
        continue;
      }
      for (const auto& sd : m->sends) {
        if (sd.to == s.masterId() || sd.stageRoom || sd.sidechain || roomops::isSpaceTrack(s, sd.to) || !s.mixerTrack(sd.to)) {
          continue;
        }
        if (all.insert(sd.to).second) {
          bus.insert(sd.to);
          todo.push_back(sd.to);
        }
      }
    }
  }
  for (size_t k = 1; k < s.mixer.size(); ++k) {
    const ObjectId id = s.mixer[k].id;
    if (!all.count(id)) {
      continue;
    }
    f.tracks.push_back(id);
    if (bus.count(id)) {
      f.buses.push_back(id);
    }
  }
  return f;
}

// Adds one clip to the search.
void take(const Song& s, const Clip& c, const Options& o, std::set<ObjectId>& direct, Found& f) {
  if (c.kind == ClipKind::Automation) {
    return;
  }
  if (!o.muted && (c.muted || laneMuted(s, c.track))) {
    ++f.mutedLeftOut;
    return;
  }
  bool master = false;
  if (clipTracks(s, c, direct, master)) {
    ++f.clips;
  } else if (master) {
    ++f.toMaster;
  }
}

}  // namespace

Found fromClips(const Song& s, const std::vector<ObjectId>& clips, const Options& o) {
  Found f;
  std::set<ObjectId> direct;
  for (ObjectId id : clips) {
    if (const Clip* c = s.clip(id)) {
      take(s, *c, o, direct, f);
    }
  }
  return finish(s, direct, o, f);
}

Found fromTracks(const Song& s, const std::vector<int>& lanes, const Options& o) {
  Found f;
  std::set<ObjectId> direct;
  const std::set<int> want(lanes.begin(), lanes.end());
  for (const Clip& c : s.playlist) {
    if (want.count(c.track)) {
      take(s, c, o, direct, f);
    }
  }
  for (int lane : want) {
    if (lane < 0 || lane >= static_cast<int>(s.tracks.size())) {
      continue;
    }
    if (!o.muted && laneMuted(s, lane)) {
      continue;
    }
    const PlaylistTrack& t = s.tracks[static_cast<size_t>(lane)];
    if (t.mode == TrackMode::Audio && t.mixer != kNoId && t.mixer != s.masterId() && s.mixerTrack(t.mixer) && !roomops::isSpaceTrack(s, t.mixer)) {
      direct.insert(t.mixer);
    }
    if (t.mode == TrackMode::Instrument) {
      if (t.mixer != kNoId && t.mixer != s.masterId() && s.mixerTrack(t.mixer) && !roomops::isSpaceTrack(s, t.mixer)) {
        direct.insert(t.mixer);
      } else if (t.channel != kNoId) {
        channelTracks(s, t.channel, direct);
      }
    }
  }
  return finish(s, direct, o, f);
}

Found fromRange(const Song& s, Tick start, Tick end, const Options& o) {
  Found f;
  std::set<ObjectId> direct;
  if (end > start) {
    for (const Clip& c : s.playlist) {
      const Tick c0 = c.start, c1 = c.start + std::max<Tick>(1, s.clipLength(c));
      if (c1 > start && c0 < end) {
        take(s, c, o, direct, f);
      }
    }
  }
  return finish(s, direct, o, f);
}

// ---------------------------------------------------------------- marker areas
std::vector<Area> markerAreas(const Song& s) {
  std::vector<Area> out;
  for (size_t i = 0; i < s.markers.size(); ++i) {
    out.push_back(betweenMarkers(s, static_cast<int>(i), -1));
  }
  return out;
}

Area betweenMarkers(const Song& s, int from, int to) {
  Area a;
  const int n = static_cast<int>(s.markers.size());
  if (from < 0 || from >= n) {
    return a;
  }
  a.marker = from;
  a.name = s.markers[static_cast<size_t>(from)].name;
  a.start = s.markers[static_cast<size_t>(from)].pos;
  if (to >= 0 && to < n && s.markers[static_cast<size_t>(to)].pos > a.start) {
    a.markerTo = to;
    a.end = s.markers[static_cast<size_t>(to)].pos;
    return a;
  }
  const auto r = ops::markerLoopRange(s, from);  // to the next marker, or the song end (at least a bar)
  a.end = r.second;
  for (int k = 0; k < n; ++k) {
    if (s.markers[static_cast<size_t>(k)].pos == a.end && a.end > a.start) {
      a.markerTo = k;
      break;
    }
  }
  return a;
}

std::string areaName(const Song& s, const Area& a) {
  if (!a.name.empty()) {
    return a.name;
  }
  if (a.marker >= 0) {
    return "Marker " + std::to_string(a.marker + 1);
  }
  const MeterMap mm(s);
  return "Bars " + std::to_string(mm.barIndex(a.start) + 1) + " to " + std::to_string(std::max<int64_t>(mm.barIndex(a.start), mm.barIndex(a.end - 1)) + 1);
}

// ---------------------------------------------------------------- places
std::vector<stage::Pos> frontPlaces(const std::vector<stage::Pos>& taken, size_t n, bool avoidCentre) {
  std::vector<stage::Pos> out;
  constexpr double kStep = 0.2, kClear = 0.15, kRow = 0.12;
  for (int row = 0; row <= 4 && out.size() < n; ++row) {
    const double d = row * 0.25;
    for (int k = 0; k <= 10 && out.size() < n; ++k) {  // 0, -0.2, 0.2, -0.4, ...
      const double pan = (k == 0 ? 0.0 : ((k + 1) / 2) * kStep * (k % 2 == 1 ? -1.0 : 1.0));
      if (std::fabs(pan) > 1.0 + 1e-9) {
        continue;
      }
      if (avoidCentre && row == 0 && k == 0) {
        continue;
      }
      bool free = true;
      for (const stage::Pos& t : taken) {
        free = free && !(std::fabs(t.depth - d) < kRow && std::fabs(t.pan - pan) < kClear);
      }
      for (const stage::Pos& t : out) {
        free = free && !(std::fabs(t.depth - d) < kRow && std::fabs(t.pan - pan) < kClear);
      }
      if (free) {
        out.push_back({kNoId, pan, d});
      }
    }
  }
  while (out.size() < n) {  // a full stage: the back, where they can be dragged from
    out.push_back({kNoId, 0.0, 1.0});
  }
  std::stable_sort(out.begin(), out.end(), [](const stage::Pos& a, const stage::Pos& b) {
    if (std::fabs(a.depth - b.depth) > 1e-9) {
      return a.depth < b.depth;
    }
    return a.pan < b.pan;
  });
  return out;
}

std::vector<stage::Pos> arrange(const std::vector<stage::Pos>& in, Arrange how) {
  std::vector<stage::Pos> out = in;
  if (in.empty()) {
    return out;
  }
  double lo = 1e9, hi = -1e9, near = 1e9, far = -1e9, sum = 0;
  for (const stage::Pos& p : in) {
    lo = std::min(lo, p.pan), hi = std::max(hi, p.pan);
    near = std::min(near, p.depth), far = std::max(far, p.depth);
    sum += p.pan;
  }
  auto spread = [&](bool front) {
    std::vector<size_t> order(in.size());
    for (size_t k = 0; k < order.size(); ++k) {
      order[k] = k;
    }
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return in[a].pan < in[b].pan; });
    const size_t n = in.size();
    const double w = std::min(0.8, 0.2 * static_cast<double>(n - 1));  // a step of 0.2 each, at most 80 % to each side
    for (size_t k = 0; k < n; ++k) {
      stage::Pos& p = out[order[k]];
      p.pan = n == 1 ? 0.0 : -w + 2.0 * w * static_cast<double>(k) / static_cast<double>(n - 1);
      if (front) {
        p.depth = 0;
      }
    }
  };
  auto shift = [&](double dPan, double dDepth) {
    dPan = std::clamp(dPan, -1.0 - lo, 1.0 - hi);
    dDepth = std::clamp(dDepth, -near, 1.0 - far);
    for (stage::Pos& p : out) {
      p.pan = std::clamp(p.pan + dPan, -1.0, 1.0);
      p.depth = std::clamp(p.depth + dDepth, 0.0, 1.0);
    }
  };
  switch (how) {
    case Arrange::SpreadLeftRight: spread(false); break;
    case Arrange::LineUpFront: spread(true); break;
    case Arrange::Forward: shift(0, -0.25); break;
    case Arrange::Back: shift(0, 0.25); break;
    case Arrange::Centre: shift(-sum / static_cast<double>(in.size()), 0); break;
    case Arrange::Left: shift(-0.2, 0); break;
    case Arrange::Right: shift(0.2, 0); break;
    case Arrange::Count: break;
  }
  return out;
}

// ---------------------------------------------------------------- edits
namespace {
bool shownIn(const Song& s, const MixerTrack& m, int room, bool everyTrack) {
  if (m.id == s.masterId() || roomops::isSpaceTrack(s, m.id) || roomops::trackRoom(s, m) != room) {
    return false;
  }
  return everyTrack || roomops::trackInUse(s, m);
}

void place(Document& d, ObjectId id, double pan, double depth) {
  const MixerTrack* m = d.song().mixerTrack(id);
  if (!m || m->locked) {
    return;
  }
  if (std::fabs(m->pan - pan) > 1e-9) {
    d.setParam(mixerParam(id, MixerParam::Pan), pan, false);
  }
  if (std::fabs(m->depth - depth) > 1e-9) {
    d.setParam(mixerParam(id, MixerParam::Depth), depth, false);
  }
}
}  // namespace

int addToStage(Document& d, const std::vector<ObjectId>& ids, int room, bool everyTrack) {
  const Song& s0 = d.song();
  if (room < 0 || room >= roomops::roomCount(s0)) {
    return 0;
  }
  std::vector<ObjectId> add, elsewhere;
  bool unused = false;
  for (ObjectId id : ids) {
    const MixerTrack* m = s0.mixerTrack(id);
    if (!m || id == s0.masterId() || roomops::isSpaceTrack(s0, id) || m->locked || shownIn(s0, *m, room, everyTrack)) {
      continue;
    }
    if (std::find(add.begin(), add.end(), id) != add.end()) {
      continue;
    }
    add.push_back(id);
    if (roomops::trackRoom(s0, *m) != room) {
      elsewhere.push_back(id);
    }
    if (!everyTrack && !roomops::trackInUse(s0, *m)) {
      unused = true;
    }
  }
  if (add.empty()) {
    return 0;
  }
  std::vector<stage::Pos> taken;
  for (size_t k = 1; k < s0.mixer.size(); ++k) {
    if (shownIn(s0, s0.mixer[k], room, everyTrack)) {
      taken.push_back({s0.mixer[k].id, s0.mixer[k].pan, s0.mixer[k].depth});
    }
  }
  const std::vector<stage::Pos> to = frontPlaces(taken, add.size(), unused);
  // Mixer order, left to right.
  std::stable_sort(add.begin(), add.end(), [&](ObjectId a, ObjectId b) { return s0.mixerIndex(a) < s0.mixerIndex(b); });
  d.endGesture();
  d.beginGroup("add tracks to the stage");
  if (!elsewhere.empty()) {
    roomops::moveTracksToRoom(d, elsewhere, room);
  }
  for (size_t k = 0; k < add.size(); ++k) {
    place(d, add[k], to[k].pan, to[k].depth);
  }
  d.endGroup(false);
  d.endGesture();
  return static_cast<int>(add.size());
}

int moveToRoom(Document& d, const std::vector<ObjectId>& ids, int room, const std::string& newName, int copyFrom, int* outRoom) {
  if (outRoom) {
    *outRoom = room;
  }
  if (ids.empty()) {
    return 0;
  }
  if (room >= roomops::roomCount(d.song())) {
    return 0;
  }
  d.endGesture();
  d.beginGroup("move tracks to room");
  if (room < 0) {
    room = roomops::addRoom(d, std::max(0, copyFrom), nullptr, newName);
  }
  const int n = room >= 0 ? roomops::moveTracksToRoom(d, ids, room) : 0;
  d.endGroup(false);
  d.endGesture();
  if (outRoom) {
    *outRoom = room;
  }
  return n;
}

int arrangeTracks(Document& d, const std::vector<ObjectId>& ids, Arrange how) {
  const Song& s = d.song();
  std::vector<stage::Pos> in;
  for (size_t k = 1; k < s.mixer.size(); ++k) {  // mixer order
    const MixerTrack& m = s.mixer[k];
    if (m.locked || std::find(ids.begin(), ids.end(), m.id) == ids.end()) {
      continue;
    }
    in.push_back({m.id, m.pan, m.depth});
  }
  if (in.empty()) {
    return 0;
  }
  const std::vector<stage::Pos> to = arrange(in, how);
  int moved = 0;
  d.endGesture();
  d.beginGroup("move on stage");
  for (size_t k = 0; k < in.size(); ++k) {
    if (std::fabs(to[k].pan - in[k].pan) > 1e-9 || std::fabs(to[k].depth - in[k].depth) > 1e-9) {
      ++moved;
    }
    place(d, in[k].id, to[k].pan, to[k].depth);
  }
  d.endGroup(false);
  d.endGesture();
  return moved;
}

}  // namespace gl::stagepick
