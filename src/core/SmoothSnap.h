// Gridline — smooth snapping (J-37): what an editor *draws* while a snapped
// drag moves things from grid cell to grid cell. The logical (committed)
// position still jumps cell by cell; the drawn position follows it with a
// short exponential ease-out, so a one-cell move glides into place in ~140 ms
// instead of teleporting, without lagging far behind the pointer.
//
// The ease is the exact solution of dx/dt = (target - x) / tau, so it is
// frame-rate independent (n frames of dt/n land where one frame of dt does),
// never overshoots, starts moving at once (snappy) and stays continuous when
// the target changes mid-flight (the next cell is reached from wherever the
// clip is drawn now). Pure: no UI dependencies, unit tested.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <unordered_map>

namespace gl::smooth {

// Time constant of the ease. 30 ms: ~40 % of a step is covered in the first
// 60 Hz frame, 1 % is left after ~140 ms (the "settle" time). J-188: J-37's
// 12 ms covered 75 % in the first frame and was done in three, which read as
// a jump: the glide was on but could not be seen.
constexpr double kTau = 0.030;
// Q-10: event-lane bars (piano roll property lane, pattern automation lane)
// glide to a new value in ~75 ms (1 % left): quick, but visibly continuous.
constexpr double kBarTau = 0.016;
// Remaining fraction of a step that counts as settled.
constexpr double kSettleResidual = 0.01;

// Seconds until only `residual` of a step is left.
inline double settleTime(double tau = kTau, double residual = kSettleResidual) { return -tau * std::log(residual); }

// One step of the ease: where a value at `cur` is `dt` seconds later.
inline double approach(double cur, double target, double dt, double tau = kTau) {
  if (!(dt > 0)) return cur;
  if (!(tau > 0)) return target;
  return target + (cur - target) * std::exp(-dt / tau);
}

// J-189: the time constant for a value `dist` from its target, where the latest
// snapped step was `step` long and the steps arrive `interval` seconds apart
// (`sinceStep` since the latest one). A single step glides with `tau`; when
// the target runs ahead (a fast drag: steps arrive before the last one has
// landed) the ease speeds up so the drawn value stays about one step behind
// instead of trailing by speed x tau, and continuously (no jump).
inline double catchUpTau(double dist, double step, double interval, double sinceStep, double tau = kTau) {
  double te = tau;
  if (step > 0 && dist > step) te = tau * step / dist;
  if (interval > 0 && sinceStep < 2 * interval) te = std::min(te, std::max(tau * 0.25, 0.5 * interval));
  return te;
}

// Drawn positions of keyed objects (clip ids, ...), N values each (e.g.
// start, end, row, content offset). Only seeded keys animate: everything else
// is drawn where it logically is, and an empty tracker costs nothing.
template <size_t N>
class Tracker {
 public:
  using Vec = std::array<double, N>;

  bool empty() const { return map_.empty(); }
  size_t size() const { return map_.size(); }
  bool contains(uint64_t key) const { return map_.count(key) > 0; }
  void clear() { map_.clear(); }

  // Starts animating `key` from `at` (its current drawn position). A key that
  // is still settling keeps its drawn position, so a new drag continues
  // smoothly from where the last one is drawn.
  void seed(uint64_t key, const Vec& at, double now) {
    auto it = map_.find(key);
    if (it != map_.end()) {
      it->second.settled = false;
      return;
    }
    Entry e;
    e.pos = e.target = at;
    e.time = e.stepAt = now;
    map_[key] = e;
  }

  // Drawn position of `key` at time `now`, easing towards `target`.
  // `animate` false (smooth snapping off, free grid) jumps to the target: the
  // object then follows the pointer directly. `eps` per value: closer than
  // this counts as arrived (the value lands exactly on the target). Keys
  // never seeded are drawn at their target.
  Vec follow(uint64_t key, const Vec& target, double now, bool animate, const Vec& eps, double tau = kTau) {
    auto it = map_.find(key);
    if (it == map_.end()) return target;
    Entry& e = it->second;
    const double dt = now - e.time;
    e.time = now;
    if (target != e.target) {  // a new snapped step
      for (size_t i = 0; i < N; ++i)
        if (target[i] != e.target[i]) e.step[i] = std::fabs(target[i] - e.target[i]);
      e.interval = e.stepped ? now - e.stepAt : 0;  // the first step after the seed has no pace yet
      e.stepped = true;
      e.stepAt = now;
      e.target = target;
    }
    bool settled = true;
    for (size_t i = 0; i < N; ++i) {
      const double te = catchUpTau(std::fabs(target[i] - e.pos[i]), e.step[i], e.interval, now - e.stepAt, tau);
      double v = animate ? approach(e.pos[i], target[i], dt, te) : target[i];
      if (std::fabs(v - target[i]) <= eps[i]) v = target[i];
      settled = settled && v == target[i];
      e.pos[i] = v;
    }
    e.settled = settled;
    return e.pos;
  }

  // Forgets settled keys unless `holding` (a drag is still under way and may
  // move them again), and keys not followed for `staleAfter` seconds (gone,
  // scrolled away) once nothing holds them.
  void prune(double now, bool holding, double staleAfter = 0.5) {
    if (holding) return;
    for (auto it = map_.begin(); it != map_.end();)
      it = (it->second.settled || now - it->second.time > staleAfter) ? map_.erase(it) : std::next(it);
  }

 private:
  struct Entry {
    Vec pos{}, target{}, step{};  // drawn, latest target, size of the latest step
    double time = 0, stepAt = 0, interval = 0;  // last follow, latest step, time between the last two steps
    bool stepped = false;                       // a step was seen since the seed
    bool settled = false;
  };
  std::unordered_map<uint64_t, Entry> map_;
};

// J-67: single snapped values that glide while their drag is under way
// (time-selection edges, automation points, slice and marker lines). Unlike
// Tracker, a key remembers where it is drawn while it rests, so the next
// snapped step glides from there; keys not followed for `staleAfter`
// seconds are forgotten. `animate` false (smooth snapping off, snap off, no
// drag) draws the value exactly where it is: no lag.
class Glides {
 public:
  double follow(uint64_t key, double target, double now, bool animate, double eps, double tau = kTau) {
    auto it = map_.find(key);
    if (!animate || it == map_.end()) {
      if (animate) map_[key] = Entry{target, now, target, 0, now, 0, false};
      else if (it != map_.end()) map_.erase(it);
      return target;
    }
    Entry& e = it->second;
    if (target != e.target) {
      e.step = std::fabs(target - e.target);
      e.interval = e.stepped ? now - e.stepAt : 0;
      e.stepped = true;
      e.stepAt = now;
      e.target = target;
    }
    double v = approach(e.pos, target, now - e.time, catchUpTau(std::fabs(target - e.pos), e.step, e.interval, now - e.stepAt, tau));
    if (std::fabs(v - target) <= eps) v = target;
    e.pos = v;
    e.time = now;
    return v;
  }
  void prune(double now, double staleAfter = 0.5) {
    for (auto it = map_.begin(); it != map_.end();) it = now - it->second.time > staleAfter ? map_.erase(it) : std::next(it);
  }
  size_t size() const { return map_.size(); }

 private:
  struct Entry {
    double pos = 0, time = 0;
    double target = 0, step = 0, stepAt = 0, interval = 0;  // J-189: see catchUpTau
    bool stepped = false;
  };
  std::unordered_map<uint64_t, Entry> map_;
};

}  // namespace gl::smooth
