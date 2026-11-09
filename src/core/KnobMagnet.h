// Gridline - magnetic knob detents (soft magnets at chosen values, e.g. the musical
// divisions of a tempo-synced delay time).
//
// The knob stays continuous: every value between two magnets can still be reached and
// kept. Only a drag is shaped. The drag's own position (normalised 0..1 along the knob's
// drag travel, before any magnet) is mapped to the value through a smooth curve that
// flattens around each magnet: within a capture zone of `zonePx` drag pixels on either
// side of a magnet the value eases onto it, holds there through the zone's inner part
// (a plateau, so it sits still while the drag goes on) and eases off again past it. The
// curve is continuous with a continuous slope at the zone's edges, so the value never
// jumps, and it depends on the drag position alone, so a pointer that wobbles at the edge
// of a zone cannot make the value flicker in and out (the plateau is the hysteresis: it
// takes `zonePx * kHold` px of drag to leave a magnet once on it). Magnets closer together
// than two zones share the room between them evenly.
//
// Fine drags (Shift), typed values, automation and playback never snap: only the
// interactive coarse drag and the wheel (which steps from magnet to magnet) use this.
// Pure functions (tests/test_k147.cpp).
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace gl::magnet {

constexpr float kZonePx = 7.0f;   // capture zone on each side of a magnet, in drag pixels
constexpr double kHold = 0.45;    // inner share of the zone that holds the value on the magnet

// Ease of the zone's outer part: t = distance from the magnet / zone (0..1) to the
// shaped distance (0..1): 0 through the plateau, then rising to meet the straight
// mapping at t = 1 with the same slope (so the edge is seamless).
inline double ease(double t, double hold = kHold) {
  t = std::clamp(t, 0.0, 1.0);
  if (t <= hold) return 0.0;
  const double w = (t - hold) / (1.0 - hold);
  return (2.0 + hold) * w * w - (1.0 + hold) * w * w * w;  // q(1) = 1, q'(1) = 1 (in t)
}

// The value position (normalised) for drag position `s` (normalised), with magnets at
// `at` (normalised, any order) and a zone of `zone` (normalised drag distance).
inline double shape(double s, const std::vector<double>& at, double zone) {
  if (at.empty() || !(zone > 0)) return std::clamp(s, 0.0, 1.0);
  std::vector<double> m(at);
  std::sort(m.begin(), m.end());
  s = std::clamp(s, 0.0, 1.0);
  // nearest magnet and its zone (half the gap to a neighbour at most)
  const auto it = std::lower_bound(m.begin(), m.end(), s);
  size_t k = it == m.end() ? m.size() - 1 : static_cast<size_t>(it - m.begin());
  if (it != m.begin() && (it == m.end() || s - *(it - 1) < *it - s)) k = static_cast<size_t>(it - m.begin()) - 1;
  double r = zone;
  if (k > 0) r = std::min(r, (m[k] - m[k - 1]) * 0.5);
  if (k + 1 < m.size()) r = std::min(r, (m[k + 1] - m[k]) * 0.5);
  const double d = s - m[k];
  if (!(r > 1e-9) || std::fabs(d) >= r) return s;
  const double q = ease(std::fabs(d) / r) * r;
  return std::clamp(m[k] + (d < 0 ? -q : q), 0.0, 1.0);
}

// A drag position that shows value position `v` (the inverse of shape(): the curve
// is monotone, so bisection; on a magnet the middle of its plateau).
inline double unshape(double v, const std::vector<double>& at, double zone) {
  v = std::clamp(v, 0.0, 1.0);
  if (at.empty() || !(zone > 0)) return v;
  for (double m : at)
    if (std::fabs(m - v) < 1e-12) return v;
  double lo = 0, hi = 1;
  for (int i = 0; i < 60; ++i) {
    const double mid = (lo + hi) * 0.5;
    (shape(mid, at, zone) < v ? lo : hi) = mid;
  }
  return (lo + hi) * 0.5;
}

// The next magnet from value position `v` in direction `dir` (+1 / -1); `v` itself
// when there is none that way.
inline double step(double v, const std::vector<double>& at, int dir) {
  double best = v;
  bool found = false;
  for (double m : at) {
    if (dir > 0 && m > v + 1e-9 && (!found || m < best)) best = m, found = true;
    if (dir < 0 && m < v - 1e-9 && (!found || m > best)) best = m, found = true;
  }
  return best;
}

// The musical divisions between `lo` and `hi`, in ticks of `ppq` per beat (a 4/4 bar is
// 4 beats): straight 1/64 to 8 bars, dotted 1/16 to 1/2 and triplets 1/16 to 1/2.
inline std::vector<double> syncDivisions(double lo, double hi, double ppq = 192) {
  std::vector<double> v;
  const double whole = ppq * 4;
  for (double d : {1 / 64.0, 1 / 32.0, 1 / 16.0, 1 / 8.0, 1 / 4.0, 1 / 2.0, 1.0, 2.0, 4.0, 8.0}) v.push_back(whole * d);
  for (double d : {1 / 16.0, 1 / 8.0, 1 / 4.0, 1 / 2.0}) {
    v.push_back(whole * d * 1.5);
    v.push_back(whole * d * 2.0 / 3.0);
  }
  std::vector<double> out;
  for (double x : v)
    if (x >= lo - 1e-9 && x <= hi + 1e-9) out.push_back(x);
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace gl::magnet
