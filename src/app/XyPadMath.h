// Gridline - J-91: the one mapping between an X-Y pad's values and the
// screen, for drawing its handle and for reading the pointer (Auto Pan's pan x
// volume pad, the X-Y controller, the skin kit's xyPad, ...). Values are
// normalised 0..1 with y pointing up. `inset` keeps a handle of that radius
// fully inside the pad: the usable area is the pad shrunk by `inset` on every
// side, in both directions, so a dragged handle stays exactly under the
// pointer. Positions outside 0..1 (a value beyond the pad's range) are drawn
// on the pad's edge. No ImGui here (unit tested in test_j91).
#pragma once
#include <algorithm>

namespace gl::xypad {

struct Rect {
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // screen, y down
};

inline float clamp01(float v) { return v != v ? 0.0f : std::clamp(v, 0.0f, 1.0f); }  // NaN -> 0

// Normalised (nx, ny) -> screen point (sx, sy), always inside the pad.
inline void toScreen(const Rect& r, float nx, float ny, float inset, float& sx, float& sy) {
  const float w = std::max(0.0f, r.x1 - r.x0 - 2 * inset), h = std::max(0.0f, r.y1 - r.y0 - 2 * inset);
  sx = r.x0 + inset + clamp01(nx) * w;
  sy = r.y1 - inset - clamp01(ny) * h;
}

// Screen point -> normalised values (clamped 0..1).
inline void fromScreen(const Rect& r, float sx, float sy, float inset, float& nx, float& ny) {
  const float w = r.x1 - r.x0 - 2 * inset, h = r.y1 - r.y0 - 2 * inset;
  nx = w > 0 ? clamp01((sx - r.x0 - inset) / w) : 0.5f;
  ny = h > 0 ? clamp01((r.y1 - inset - sy) / h) : 0.5f;
}

}  // namespace gl::xypad
