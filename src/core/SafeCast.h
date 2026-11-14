// Gridline: float to int conversions that cannot overflow.
#pragma once
#include <algorithm>

namespace gl {

// v truncated toward zero into [lo, hi]. The clamp happens on the floating value, before the conversion,
// so an off-screen mouse (ImGui reports -FLT_MAX) or a huge ratio never converts out of range; NaN gives lo.
inline int toIntClamped(double v, int lo, int hi) {
  if (!(v == v)) return lo;
  return static_cast<int>(std::clamp(v, static_cast<double>(lo), static_cast<double>(hi)));
}

}  // namespace gl
