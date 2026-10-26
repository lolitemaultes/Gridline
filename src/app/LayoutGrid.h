// Gridline — J-128 layout layer: the geometry of rows, columns and cells that
// plugin editors and panels place their controls on. Pure maths (no ImGui, no
// drawing): how a knob, fader or button LOOKS is the element layer's business
// (PluginSkin / Widgets); where it SITS is decided here, so a row of controls
// always shares one centre line and one label line, cells share one width and
// gaps are equal. Units are whatever the caller uses (form units or logical px).
#pragma once
#include <algorithm>
#include <cmath>

namespace gl::grid {

// A row of controls of mixed sizes: one centre line through every control and
// one line for the labels under them (under the tallest control + gap).
struct Row {
  float cy = 0;      // centre line
  float labelY = 0;  // top of the labels
  // Top edge of a control of height `h` on the row.
  float top(float h) const { return cy - h * 0.5f; }
};
// Row whose tallest control (height `tallest`) starts at `top`.
inline Row row(float top, float tallest, float labelGap = 1.5f) { return Row{top + tallest * 0.5f, top + tallest + labelGap}; }
// Row through a given centre line.
inline Row rowAt(float cy, float tallest, float labelGap = 1.5f) { return Row{cy, cy + tallest * 0.5f + labelGap}; }

// `n` equal cells of width `w` spread over [x0, x1] with equal gaps (and half
// gaps at both ends): left edge of cell i.
inline float spread(float x0, float x1, int n, float w, int i) {
  if (n <= 0) return x0;
  const float gap = ((x1 - x0) - w * static_cast<float>(n)) / static_cast<float>(n);
  return x0 + gap * 0.5f + (w + gap) * static_cast<float>(i);
}
// Left edge of cell i of `n` cells of width `w` with gap `gap`, the group centred on cx.
inline float centred(float cx, int n, float w, float gap, int i) {
  const float total = w * static_cast<float>(n) + gap * static_cast<float>(std::max(0, n - 1));
  return cx - total * 0.5f + (w + gap) * static_cast<float>(i);
}
// Top-left of a box of size (w, h) centred in the cell [x0, x0 + cw] x [y0, y0 + ch].
inline float centreIn(float x0, float cw, float w) { return x0 + (cw - w) * 0.5f; }

// Snaps a logical coordinate to the pixel grid of the interface scale (1, 1.5,
// 2, ...): controls on one row land on the same device pixel at every scale.
inline float snap(float v, float scale = 1.0f) { return scale > 0 ? std::round(v * scale) / scale : v; }

}  // namespace gl::grid
