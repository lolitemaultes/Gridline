// Gridline — K-11: loop notches on repeating clips (playlist).
//
// Each loop point of a clip that repeats its content (a looping pattern clip,
// Q-1) is marked by a small V cut into the clip's top edge and a matching one
// into its bottom edge, at the same x, in the colour of the grid behind the
// clip; the body runs on unbroken. The notch grows a little with the width of
// one pass (zoom), stays within 5 .. 8 logical px wide and about 0.6 of that
// deep (6 x 4 px at a bar of 64 px, 8 x 5 px from a bar of 130 px), and is
// left out when a pass is too narrow to read or the clip too short. Logical
// px: the UI scale multiplies them like every other size.
#pragma once
#include <algorithm>
#include <cmath>

#include "../core/ClipLoop.h"

namespace gl::loopnotch {

struct Geom {
  bool show = false;  // false: no notches (and no divider) for this clip
  float width = 0;    // V width at the clip edge, logical px
  float depth = 0;    // how far the V reaches into the clip, logical px
};

// `passPx`: the on-screen width of one pass of the content; `clipH`: the
// clip's height (title bar included).
inline Geom geom(float passPx, float clipH) {
  Geom g;
  if (passPx < 16.0f || clipH < 14.0f) return g;
  g.width = std::clamp(4.0f + passPx * 0.03f, 5.0f, 8.0f);
  g.depth = std::min(g.width * 0.66f, clipH * 0.16f);
  g.show = g.depth >= 2.5f;
  return g;
}

// K-388: Smooth's divots. A looped region in Smooth marks each repeat start with a V cut into its top
// and bottom edges (the repeats are washed lighter, as before). 8 .. 11 px wide (growing with the zoom),
// about 0.6 of that deep and at most 0.2 of the region's height: as clear at 1x as the K-11 notch on
// Studio's clips. When repeats get narrow (zoomed out, short loops) every 2nd loop point shows (counted
// from the first, which always shows), so two divots are always at least `kSmoothMinGap` px and 3 widths
// apart; narrower still, none show (as in Studio), so a short loop never reads as a longer one.
struct SmoothGeom {
  bool show = false;
  float width = 0, depth = 0;
  int stride = 1;
};
inline constexpr float kSmoothMinGap = 20.0f;

inline SmoothGeom smoothGeom(float passPx, float regionH) {
  SmoothGeom g;
  if (!(passPx > 0) || regionH < 12.0f) return g;
  g.width = std::clamp(6.0f + passPx * 0.04f, 8.0f, 11.0f);
  g.depth = std::min(g.width * 0.6f, regionH * 0.2f);
  const float gap = std::max(kSmoothMinGap, g.width * 3.0f);
  if (passPx < gap) g.stride = 2;
  g.show = g.depth >= 2.0f && passPx * static_cast<float>(g.stride) >= gap;
  return g;
}

// The first loop point of a window [offset, offset + len) over content repeating [from, from + loop):
// ticks from the region's start (the end of its first, possibly partial, pass).
inline Tick firstLoopPoint(Tick offset, Tick loop, Tick from = 0) { return from + loop - wrapLoopOffset(offset, loop, from); }

// Calls fn(at, k) for the loop points k = 1, 2, ... (at = ticks from the region start, inside (0, len))
// that show with `stride` (k - 1 a multiple of it) and lie in [visFrom, visTo] (ticks from the region
// start): O(points visited), no allocation, each one's tick exact (no summed drift).
template <class F>
inline void forEachLoopPoint(Tick offset, Tick len, Tick loop, Tick from, int stride, double visFrom, double visTo, F&& fn) {
  if (loop <= 0 || len <= 0 || stride < 1) return;
  const Tick first = firstLoopPoint(offset, loop, from);
  Tick k = 1;
  if (visFrom > static_cast<double>(first)) k = 1 + static_cast<Tick>(std::floor((visFrom - static_cast<double>(first)) / static_cast<double>(loop)));
  k = 1 + ((k - 1 + stride - 1) / stride) * stride;  // up to the next shown one
  for (Tick at = first + (k - 1) * loop; at < len && static_cast<double>(at) <= visTo; at += loop * stride, k += stride) fn(at, static_cast<int>(k));
}

}  // namespace gl::loopnotch
