// Gridline — J-117: the elastic ("rubber") boundary of windows floating inside the main window.
//
// A floating window (the channel rack, the mixer or piano roll undocked, an editor, a dialog)
// stays inside the main window's work area, between the top bar and the status bar:
//  * while it is dragged it can be pulled past an edge, with growing resistance: the part of the
//    drag past the edge is damped by J-67's rubber band (core/SmoothScroll.h), so the window
//    never gets further out than the elastic bound (approached, never reached);
//  * once it is let go (or when the work area shrinks under it) it springs back, with J-67's
//    exponential spring: the distance left shrinks by exp(-dt / backTau) each frame, the same
//    for any frame rate, and the last half pixel snaps;
//  * a window larger than the work area keeps its top-left corner in (its title bar and its
//    left part stay reachable) instead.
// With elastic edges off (Settings > Interface, or the system asks for reduced motion) the
// boundary is a hard one: no stretch, no spring.
//
// Pure maths (no UI), header only apart from J-67's rubber(): tests/test_j117.cpp. The app side
// (which windows, the drag, the per-window spring state) is ui::ClampTrackedWindows in
// app/DialogLayout.cpp.
#pragma once
#include <algorithm>
#include <cmath>
#include "SmoothScroll.h"

namespace gl::rubber {

// J-67's elastic edge (maxOver 48 px, resist 0.55, backTau 0.055 s), elastic on/off.
using Params = smoothscroll::Params;
// Closer than this to its place counts as arrived (px): the spring snaps.
constexpr float kArrive = 0.5f;

// Where the top-left of a window `size` long may rest on one axis of an area [a0, a1]: fully
// inside, or for a window longer than the area its start edge on the area's start.
struct Range {
  float lo = 0, hi = 0;
};
inline Range range(float a0, float a1, float size) { return Range{a0, std::max(a0, a1 - size)}; }

// Its resting place: `v` clamped into the range.
inline float rest(float v, Range r) { return std::clamp(v, r.lo, r.hi); }

// Where a window dragged to `raw` is drawn: in range 1:1, past an edge by the damped stretch
// (|result - edge| < maxOver). Hard edges when elastic is off.
inline float drag(float raw, Range r, const Params& p) {
  if (raw >= r.lo && raw <= r.hi) return raw;
  const float edge = raw < r.lo ? r.lo : r.hi;
  if (!p.elastic) return edge;
  return edge + static_cast<float>(smoothscroll::rubber(static_cast<double>(raw - edge), p));
}

// One frame (`dt` s) of the spring back from `cur` towards `target`: exp(-dt / backTau) of the
// distance is left (frame-rate independent), the last kArrive px snap. Hard edges (elastic off)
// or no time constant: there at once.
inline float spring(float cur, float target, double dt, const Params& p) {
  if (!p.elastic || !(p.backTau > 0)) return target;
  if (!(dt > 0)) return cur;
  const float next = target + (cur - target) * static_cast<float>(std::exp(-dt / p.backTau));
  return std::fabs(next - target) < kArrive ? target : next;
}

// Seconds the spring takes from `distance` px out until it snaps (for tests and docs).
inline double settleTime(float distance, const Params& p) {
  if (!p.elastic || !(p.backTau > 0) || std::fabs(distance) < kArrive) return 0;
  return p.backTau * std::log(std::fabs(distance) / kArrive);
}

}  // namespace gl::rubber
