// Gridline — smooth snapping (J-37): what an editor *draws* while a snapped
// drag moves things from grid cell to grid cell. The logical (committed)
// position still jumps cell by cell; the drawn position follows it with a
// short exponential ease-out, so a one-cell move glides into place in ~55 ms
// instead of teleporting, without lagging behind the pointer.
//
// The ease is the exact solution of dx/dt = (target - x) / tau, so it is
// frame-rate independent (n frames of dt/n land where one frame of dt does),
// never overshoots, starts moving at once (snappy) and stays continuous when
// the target changes mid-flight (the next cell is reached from wherever the
// clip is drawn now). Pure: no UI dependencies, unit tested.
#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <unordered_map>

namespace gl::smooth {

// Time constant of the ease. 12 ms: 75 % of a step is covered in the first
// 60 Hz frame, 1 % is left after ~55 ms (the "settle" time).
constexpr double kTau = 0.012;
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
    map_[key] = Entry{at, now, false};
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
    bool settled = true;
    for (size_t i = 0; i < N; ++i) {
      double v = animate ? approach(e.pos[i], target[i], dt, tau) : target[i];
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
    Vec pos{};
    double time = 0;
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
      if (animate) map_[key] = Entry{target, now};
      else if (it != map_.end()) map_.erase(it);
      return target;
    }
    Entry& e = it->second;
    double v = approach(e.pos, target, now - e.time, tau);
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
  };
  std::unordered_map<uint64_t, Entry> map_;
};

}  // namespace gl::smooth
