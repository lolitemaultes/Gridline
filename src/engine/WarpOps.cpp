// Gridline — K-85: editing an audio clip's warp markers. See WarpOps.h.
#include "WarpOps.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include "../core/ClipRate.h"
#include "../core/Timeline.h"
#include "../packs/edison/edison.h"
#include "ClipSource.h"
#include "TimeStretch.h"

namespace gl {

namespace ed = packs::edison;

namespace {

// A marker in the clip's direction: source `s`, warped time `w`.
struct DirMarker {
  uint32_t id = 0;
  double s = 0, w = 0;
};

// Everything an edit needs about one clip.
struct Ctx {
  ClipSource src;
  bool rev = false;
  double last = 0;          // the last frame's position (seconds)
  double a = 0, b = 0;      // the clip's window, its direction
  double speed = 1, secPerTick = 0, startSec = 0, start = 0;
  std::vector<DirMarker> m;  // sorted by s
  const TempoMap* map = nullptr;

  std::vector<WarpPoint> points() const {
    std::vector<WarpPoint> p;
    p.reserve(m.size());
    for (const DirMarker& k : m) p.push_back({k.w, k.s});
    return p;
  }
  // Unwarped position (source seconds at the clip's speed) at song tick t, and back.
  double pAt(double t) const { return secPerTick > 0 ? (t - start) * secPerTick : (map->secondsAt(t) - startSec) * speed; }
  double tickOf(double p) const { return secPerTick > 0 ? start + p / secPerTick : map->tickAtSeconds(startSec + p / std::max(1e-9, speed)); }
  double w0() const { return warpTimeAt(points(), a); }  // warped time of the clip's first sample
  double sourceAt(double t) const {
    const std::vector<WarpPoint> p = points();
    return warpSourceAt(p, warpTimeAt(p, a) + pAt(t));
  }
  int find(uint32_t id) const {
    for (size_t i = 0; i < m.size(); ++i)
      if (m[i].id == id) return static_cast<int>(i);
    return -1;
  }
};

bool context(const Song& song, const Clip& c, const TempoMap& map, Ctx& x) {
  const Channel* ch = c.kind == ClipKind::Audio ? song.channel(c.source) : nullptr;
  if (!ch) return false;
  x.src = clipSource(*ch);
  if (!x.src.ok()) return false;
  x.rev = clipPlaysReversed(*ch, c);
  x.last = static_cast<double>(x.src.sample->frames() - 1) / x.src.sample->sampleRate;
  if (!clipWindow(x.src, x.rev, c, x.a, x.b)) return false;
  const ClipRate r = clipRate(*ch, c);
  x.start = static_cast<double>(c.start);
  if (c.syncBpm > 0) x.secPerTick = 60.0 / (c.syncBpm * song.ppq) / r.timeFactor;
  else x.speed = r.speed;
  x.map = &map;
  x.startSec = map.secondsAt(x.start);
  x.m.clear();
  for (const WarpMarker& k : c.warp) x.m.push_back({k.id, x.rev ? x.last - k.at : k.at, x.rev ? -k.to : k.to});
  std::sort(x.m.begin(), x.m.end(), [](const DirMarker& p, const DirMarker& q) { return p.s < q.s; });
  return true;
}

bool store(Clip& c, const Ctx& x) {
  std::vector<WarpMarker> v;
  v.reserve(x.m.size());
  for (const DirMarker& k : x.m) v.push_back({k.id, x.rev ? x.last - k.s : k.s, x.rev ? -k.w : k.w});
  normalizeWarp(v, x.src.seconds());
  if (v == c.warp) return false;
  c.warp = std::move(v);
  return true;
}

uint32_t freshId(const Ctx& x) {
  uint32_t id = 1;
  for (const DirMarker& k : x.m) id = std::max(id, k.id + 1);
  return id;
}

// Inserts a marker at source s keeping the current timing; returns its id (0: too close to another).
uint32_t insertAt(Ctx& x, double s) {
  for (const DirMarker& k : x.m)
    if (std::fabs(k.s - s) < 0.001) return 0;
  if (x.m.size() >= kMaxWarpMarkers) return 0;
  const DirMarker n{freshId(x), s, warpTimeAt(x.points(), s)};
  x.m.insert(std::upper_bound(x.m.begin(), x.m.end(), n, [](const DirMarker& p, const DirMarker& q) { return p.s < q.s; }), n);
  return n.id;
}

// The clip's start is pinned by a marker at or before its first sample.
void pinStart(Ctx& x) {
  for (const DirMarker& k : x.m)
    if (k.s <= x.a + 1e-9) return;
  for (const DirMarker& k : x.m)
    if (std::fabs(k.s - x.a) < 0.001) return;  // (one right at the start pins it closely enough)
  insertAt(x, x.a);
}

// Onsets of the clip's window, as source seconds in its direction.
std::vector<double> onsetsOf(const Ctx& x, double lo, double hi, double sensitivity) {
  const Sample& smp = *x.src.sample;
  const double fa = x.rev ? x.last - hi : lo, fb = x.rev ? x.last - lo : hi;
  ed::Audio au = ed::fromSample(smp);
  const auto r0 = std::clamp<int64_t>(static_cast<int64_t>(std::llround(std::max(0.0, fa) * smp.sampleRate)), 0, au.frames());
  const auto r1 = std::clamp<int64_t>(static_cast<int64_t>(std::llround(fb * smp.sampleRate)), r0, au.frames());
  std::vector<double> out;
  if (r1 - r0 < 2) return out;
  for (int64_t f : ed::detectOnsets(au, {r0, r1}, sensitivity)) {
    const double s = static_cast<double>(f) / smp.sampleRate;
    out.push_back(x.rev ? x.last - s : s);
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace

double clipTickOfSource(const Song& song, const Clip& c, const TempoMap& map, double s) {
  Ctx x;
  if (!context(song, c, map, x)) return std::numeric_limits<double>::quiet_NaN();
  const std::vector<WarpPoint> p = x.points();
  return x.tickOf(warpTimeAt(p, s) - warpTimeAt(p, x.a));
}

std::vector<WarpMarkerView> warpMarkerViews(const Song& song, const Clip& c, const TempoMap& map) {
  std::vector<WarpMarkerView> out;
  if (c.warp.empty()) return out;
  Ctx x;
  if (!context(song, c, map, x)) return out;
  const double w0 = x.w0(), end = static_cast<double>(c.start + song.clipLength(c));
  for (size_t i = 0; i < x.m.size(); ++i) {
    const DirMarker& k = x.m[i];
    if (k.s < x.a - 1e-9 || k.s > x.b + 1e-9) continue;
    const double t = x.tickOf(k.w - w0);
    if (!(t >= x.start - 1e-6) || t > end + 1e-6) continue;
    WarpMarkerView v;
    v.id = k.id;
    v.tick = t;
    if (i + 1 < x.m.size()) {
      const double dw = x.m[i + 1].w - k.w;
      v.speed = dw > 0 ? (x.m[i + 1].s - k.s) / dw : 1.0;
    }
    out.push_back(v);
  }
  return out;
}

uint32_t addWarpMarker(const Song& song, Clip& clip, const TempoMap& map, double tick, bool snap) {
  Clip* c = &clip;
  Ctx x;
  if (!context(song, *c, map, x)) return 0;
  if (tick < x.start || tick > static_cast<double>(c->start + song.clipLength(*c))) return 0;
  double s = x.sourceAt(tick);
  if (snap) {
    double best = std::numeric_limits<double>::infinity();
    for (double o : onsetsOf(x, std::max(x.a, s - 0.04), std::min(x.b, s + 0.04), 0.5))
      if (std::fabs(o - s) < std::fabs(best - s)) best = o;
    if (std::isfinite(best)) s = best;
  }
  if (s < x.a - 1e-9 || s > x.b + 1e-9) return 0;
  const uint32_t id = insertAt(x, s);
  if (!id) return 0;
  store(*c, x);
  return id;
}

bool moveWarpMarker(const Song& song, Clip& clip, const TempoMap& map, uint32_t id, double tick) {
  Clip* c = &clip;
  Ctx x;
  if (!context(song, *c, map, x)) return false;
  if (x.find(id) < 0 || x.m[static_cast<size_t>(x.find(id))].s <= x.a + 1e-9) return false;  // (the start's own marker stays)
  pinStart(x);
  const int i = x.find(id);
  const double target = x.pAt(tick);
  // The warped time the marker may take: both neighbouring sections within the limits.
  const double slack = 1e-7;
  double lo = x.m[static_cast<size_t>(i)].w - 1e5, hi = x.m[static_cast<size_t>(i)].w + 1e5;
  if (i > 0) {
    const DirMarker& p = x.m[static_cast<size_t>(i - 1)];
    const double ds = x.m[static_cast<size_t>(i)].s - p.s;
    lo = std::max(lo, p.w + std::max(kWarpMinSection, ds / kWarpMaxSpeed) * (1 + slack));
    hi = std::min(hi, p.w + ds / kWarpMinSpeed * (1 - slack));
  }
  if (i + 1 < static_cast<int>(x.m.size())) {
    const DirMarker& n = x.m[static_cast<size_t>(i + 1)];
    const double ds = n.s - x.m[static_cast<size_t>(i)].s;
    lo = std::max(lo, n.w - ds / kWarpMinSpeed * (1 - slack));
    hi = std::min(hi, n.w - std::max(kWarpMinSection, ds / kWarpMaxSpeed) * (1 + slack));
  }
  if (!(lo <= hi)) return false;
  // The marker plays `target` after the clip's first sample (monotonic in w: bisection).
  auto offsetFor = [&](double w) {
    x.m[static_cast<size_t>(i)].w = w;
    return w - x.w0();
  };
  double a = lo, b = hi;
  if (offsetFor(a) >= target) {
    b = a;
  } else if (offsetFor(b) <= target) {
    a = b;
  } else {
    for (int k = 0; k < 100 && b - a > 1e-12; ++k) {
      const double mid = 0.5 * (a + b);
      (offsetFor(mid) < target ? a : b) = mid;
    }
  }
  x.m[static_cast<size_t>(i)].w = 0.5 * (a + b);
  return store(*c, x);
}

bool slideWarpMarker(const Song& song, Clip& clip, const TempoMap& map, uint32_t id, double tick) {
  Clip* c = &clip;
  Ctx x;
  if (!context(song, *c, map, x) || x.find(id) < 0) return false;
  pinStart(x);
  const int i = x.find(id);
  const DirMarker cur = x.m[static_cast<size_t>(i)];
  if (cur.s <= x.a + 1e-9) return false;
  const double slack = 1e-7;
  double lo = x.a + 0.001, hi = x.b;
  if (i > 0) {
    const DirMarker& p = x.m[static_cast<size_t>(i - 1)];
    const double dw = cur.w - p.w;
    lo = std::max(lo, p.s + dw * kWarpMinSpeed * (1 + slack));
    hi = std::min(hi, p.s + dw * kWarpMaxSpeed * (1 - slack));
  }
  if (i + 1 < static_cast<int>(x.m.size())) {
    const DirMarker& n = x.m[static_cast<size_t>(i + 1)];
    const double dw = n.w - cur.w;
    lo = std::max(lo, n.s - dw * kWarpMaxSpeed * (1 - slack));
    hi = std::min(hi, n.s - dw * kWarpMinSpeed * (1 + slack));
  }
  if (!(lo <= hi)) return false;
  x.m[static_cast<size_t>(i)].s = std::clamp(x.sourceAt(tick), lo, hi);
  return store(*c, x);
}

bool deleteWarpMarker(Clip& clip, uint32_t id) {
  Clip* c = &clip;
  const size_t n = c->warp.size();
  c->warp.erase(std::remove_if(c->warp.begin(), c->warp.end(), [&](const WarpMarker& m) { return m.id == id; }), c->warp.end());
  return c->warp.size() != n;
}

bool clearWarp(Clip& clip) {
  Clip* c = &clip;
  if (c->warp.empty()) return false;
  c->warp.clear();
  return true;
}

int warpMarkersAtTransients(const Song& song, Clip& clip, const TempoMap& map, double sensitivity) {
  Clip* c = &clip;
  Ctx x;
  if (!context(song, *c, map, x)) return 0;
  const double end = static_cast<double>(c->start + song.clipLength(*c));
  const double hi = std::min(x.b, x.sourceAt(end));
  int added = 0;
  for (double s : onsetsOf(x, x.a, hi, sensitivity)) {
    if (s <= x.a + 0.001) continue;
    bool near = false;
    for (const DirMarker& k : x.m) near = near || std::fabs(k.s - s) < 0.01;
    if (!near && insertAt(x, s)) ++added;
  }
  if (added) store(*c, x);
  return added;
}

int quantizeWarpMarkers(const Song& song, Clip& clip, const TempoMap& map, Tick grid, double strength, const std::set<uint32_t>* only) {
  const Clip* c = &clip;
  if (grid <= 0 || !(strength > 0)) return 0;
  strength = std::min(1.0, strength);
  int moved = 0;
  const std::vector<WarpMarkerView> views = warpMarkerViews(song, *c, map);
  for (const WarpMarkerView& v : views) {
    if (only && !only->count(v.id)) continue;
    const double g = static_cast<double>(grid);
    const double target = std::round(v.tick / g) * g;
    const double to = v.tick + strength * (target - v.tick);
    if (std::fabs(to - v.tick) < 1e-6) continue;
    if (moveWarpMarker(song, clip, map, v.id, to)) ++moved;  // (the clip passed in)
  }
  return moved;
}

int quantizeAudioClip(const Song& song, Clip& clip, const TempoMap& map, Tick grid, double strength, double sensitivity) {
  warpMarkersAtTransients(song, clip, map, sensitivity);
  return quantizeWarpMarkers(song, clip, map, grid, strength, nullptr);
}

double warpDragTick(double markerTick, Tick grabTick, Tick pointerTick, Tick grid) {
  const double raw = std::max(0.0, markerTick + static_cast<double>(pointerTick - grabTick));
  if (grid <= 1) return raw;
  return static_cast<double>(timeline::snapTick(static_cast<Tick>(std::llround(raw)), grid, Snap::Line));
}

// ---- by clip id ----

uint32_t addWarpMarker(Song& song, ObjectId clip, const TempoMap& map, double tick, bool snap) {
  Clip* c = song.clip(clip);
  return c ? addWarpMarker(static_cast<const Song&>(song), *c, map, tick, snap) : 0;
}
bool moveWarpMarker(Song& song, ObjectId clip, const TempoMap& map, uint32_t id, double tick) {
  Clip* c = song.clip(clip);
  return c && moveWarpMarker(static_cast<const Song&>(song), *c, map, id, tick);
}
bool slideWarpMarker(Song& song, ObjectId clip, const TempoMap& map, uint32_t id, double tick) {
  Clip* c = song.clip(clip);
  return c && slideWarpMarker(static_cast<const Song&>(song), *c, map, id, tick);
}
bool deleteWarpMarker(Song& song, ObjectId clip, uint32_t id) {
  Clip* c = song.clip(clip);
  return c && deleteWarpMarker(*c, id);
}
bool clearWarp(Song& song, ObjectId clip) {
  Clip* c = song.clip(clip);
  return c && clearWarp(*c);
}
int warpMarkersAtTransients(Song& song, ObjectId clip, const TempoMap& map, double sensitivity) {
  Clip* c = song.clip(clip);
  return c ? warpMarkersAtTransients(static_cast<const Song&>(song), *c, map, sensitivity) : 0;
}
int quantizeWarpMarkers(Song& song, ObjectId clip, const TempoMap& map, Tick grid, double strength, const std::set<uint32_t>* only) {
  Clip* c = song.clip(clip);
  return c ? quantizeWarpMarkers(static_cast<const Song&>(song), *c, map, grid, strength, only) : 0;
}
int quantizeAudioClip(Song& song, ObjectId clip, const TempoMap& map, Tick grid, double strength, double sensitivity) {
  Clip* c = song.clip(clip);
  return c ? quantizeAudioClip(static_cast<const Song&>(song), *c, map, grid, strength, sensitivity) : 0;
}

}  // namespace gl
