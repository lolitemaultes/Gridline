// Gridline - the Live layout's session grid geometry (K-112c): where a title, slot or stop cell
// lies. Every edge is an unsnapped position (the grid's origin plus whole steps) snapped once to a
// device pixel, then inset by a whole number of device pixels (K-276, app/PixelGrid.h), so the gap
// between neighbouring cells is the same across columns and rows, under the titles and above the
// stop row, at any window size and interface scale. Pure geometry, so a test can measure it.
#pragma once
#include "PixelGrid.h"

namespace gl::livegrid {

constexpr float kPad = 8, kTitleH = 20, kGap = 2, kCellH = 18, kSlotH = kCellH + kGap, kColW = 96;

struct Cell {
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

// A position on the nearest device-pixel edge (`dpp`: device px per logical px).
inline float snap(float v, float dpp = 1.0f) { return pixelgrid::edge(v, dpp); }

// Column t's drawn span: its own edge snapped, the gap taken off its right end.
inline pixelgrid::Span column(float originX, int t, float dpp = 1.0f) {
  const float a = snap(originX, dpp) + kPad + static_cast<float>(t) * kColW;  // the origin snapped first: one rounding for every column
  return pixelgrid::block(a, a + kColW, dpp, 0.0f, kGap);
}
inline float columnX(float originX, int t, float dpp = 1.0f) { return column(originX, t, dpp).x0; }
// The title row's top and bottom.
inline float titleY(float originY, float dpp = 1.0f) { return snap(snap(originY, dpp) + kPad, dpp); }
inline float titleBottom(float originY, float dpp = 1.0f) { return snap(snap(originY, dpp) + kPad + kTitleH, dpp); }
// Slot row k's drawn span (k == rows: the stop row): the gap taken off its top, so the first row
// sits one gap under the titles.
inline pixelgrid::Span row(float originY, int k, float dpp = 1.0f) {
  const float a = snap(originY, dpp) + kPad + kTitleH + static_cast<float>(k) * kSlotH;
  return pixelgrid::block(a, a + kSlotH, dpp, kGap, 0.0f);
}
inline float rowY(float originY, int k, float dpp = 1.0f) { return row(originY, k, dpp).x0; }

inline Cell title(float ox, float oy, int t, float dpp = 1.0f) {
  const pixelgrid::Span c = column(ox, t, dpp);
  return {c.x0, titleY(oy, dpp), c.x1, titleBottom(oy, dpp)};
}
inline Cell slot(float ox, float oy, int t, int k, float dpp = 1.0f) {
  const pixelgrid::Span c = column(ox, t, dpp), r = row(oy, k, dpp);
  return {c.x0, r.x0, c.x1, r.x1};
}

}  // namespace gl::livegrid
