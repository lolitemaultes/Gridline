// Gridline - K-276: blocks on a time grid (playlist clips, piano roll notes, live takes) on whole
// device pixels, so touching blocks always show the same gap and their edges meet the grid lines.
//
// The interface is laid out in logical px; the screen has `dpp` device px per logical px (the
// viewport's framebuffer scale: 1, 2, but also 1.25, 2.5 ... with an interface scale). Rounding a
// position to a whole *logical* px only lands on a device pixel when dpp is a whole number; else
// each edge falls somewhere inside a device pixel and is blurred by its own amount, so the gaps
// between touching clips read 1, 2 or 3 px at random. Rule, for every block drawn on a time grid:
//
//   1. map the time to x unrounded (the editor's xOf), then snap that x ONCE with edge(): a time
//      always gives the same device-pixel edge, so two blocks that touch in time share it;
//   2. only then inset the block (block()), by a whole number of device px: every gap is equal;
//   3. draw the grid lines with line() at the same snapped edge: block edges meet the lines.
//
// Pure geometry (no drawing), so tests check it headless (tests/test_pixel_grid.cpp). Works on
// either axis (x for time, y for rows of tracks or keys).
#pragma once
#include <algorithm>
#include <cmath>

#include "imgui.h"

namespace gl::pixelgrid {

// Device px per logical px of the window being drawn (frameScale() reads it from ImGui).
inline float frameScale() {
  const ImGuiViewport* vp = ImGui::GetWindowViewport();
  const float s = vp && vp->FramebufferScale.x > 0 ? vp->FramebufferScale.x : ImGui::GetIO().DisplayFramebufferScale.x;
  return s > 0 ? s : 1.0f;
}

// `x` (logical px) moved to the nearest device-pixel edge.
inline float edge(float x, float dpp) {
  if (!(dpp > 0)) dpp = 1.0f;
  return std::round(x * dpp) / dpp;
}

// A length of `px` logical px as a whole number of device px (at least `minDevice` of them), in logical px.
inline float whole(float px, float dpp, float minDevice = 1.0f) {
  if (!(dpp > 0)) dpp = 1.0f;
  return std::max(minDevice, std::round(px * dpp)) / dpp;
}

struct Span {
  float x0 = 0, x1 = 0;
  float w() const { return x1 - x0; }
};

// The block drawn for a span whose unsnapped edges are a..b: both edges snapped (edge()), then
// inset by `insetA` at the start and `insetB` at the end (logical px, made whole device px, 0 for
// none). Two blocks that touch (one's b is the other's a) are always insetA + insetB apart. A block
// too short for its insets grows towards `minW` (whole device px) into its own end inset, never past
// its snapped end, and always keeps one device px, so a tiny clip still shows without covering the next.
inline Span block(float a, float b, float dpp, float insetA = 1.0f, float insetB = 1.0f, float minW = 1.0f) {
  const float ia = insetA > 0 ? whole(insetA, dpp) : 0.0f, ib = insetB > 0 ? whole(insetB, dpp) : 0.0f;
  const float eb = edge(b, dpp);
  Span s{edge(a, dpp) + ia, eb - ib};
  // Short: grow to minW, but not past its own end (the edge it shares with the next block); one device px always.
  const float one = 1.0f / (dpp > 0 ? dpp : 1.0f);
  s.x1 = std::max(s.x1, std::max(std::min(s.x0 + whole(minW, dpp), eb), s.x0 + one));
  return s;
}

// How far to move a box's corners in before ImDrawList::AddRect strokes it `thick` px wide, so the
// stroke lies wholly inside the box (AddRect centres its stroke half a px in from the corners given:
// a 2 px rim stroked on the box itself reaches half a px out, into the gap, and a selected clip's
// gap reads narrower). 0 for a 1 px rim. Pass it to both corners: AddRect(p0 + in, p1 - in, ...).
inline float strokeIn(float thick) { return std::max(0.0f, thick * 0.5f - 0.5f); }

// A grid line for position `x`: `thick` logical px (whole device px) starting at x's snapped edge,
// to fill as a rectangle (no anti-aliasing, so it stays one crisp column at every scale).
inline Span line(float x, float dpp, float thick = 1.0f) {
  const float e = edge(x, dpp);
  return Span{e, e + whole(thick, dpp)};
}

}  // namespace gl::pixelgrid
