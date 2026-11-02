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

}  // namespace gl::loopnotch
