// Gridline — "plasma" pack: the distortion module's shapers (J-203d). The static curves
// are transfer tables measured on the Serum 2 oracle with a held sine through each mode at
// drive 0, 10, .. 100 % (input levels from -12 to +24 dB re full scale), in Plasma's internal
// units; between the measured drives the curves are blended linearly, past the measured input
// range they continue along their last segment. The module halves its output (measured: the
// linear part of every static mode is -6.0 dB at drive 0). Allocation free.
#include <algorithm>
#include <cmath>
#include "plasma_dsp.h"

namespace gl::packs::plasma {
namespace {
#include "plasma_fx_dist_tables.inc"

constexpr int kK = 40;          // grid points per side
constexpr float kXMax = 10.5f;  // input at the last grid point

struct Tab {
  const float (*rows)[kK + 1];       // odd curves: k = 0 .. kK
  const float (*rowsFull)[2 * kK + 1];  // other curves: k = -kK .. kK
};

Tab tabOf(int mode) {
  switch (mode) {
    case 0: return {kDist0, nullptr};
    case 1: return {kDist1, nullptr};
    case 2: return {kDist2, nullptr};
    case 3: return {kDist3, nullptr};
    case 4: return {nullptr, kDist4};
    case 5: return {kDist5, nullptr};
    case 6: return {kDist6, nullptr};
    case 7: return {kDist7, nullptr};
    case 9: return {nullptr, kDist9};
    case 10: return {nullptr, kDist10};
    case 13: return {kDist13, nullptr};
    case 14: return {kDist14, nullptr};
    case 15: return {kDist15, nullptr};
    case 17: return {kDist17, nullptr};
    default: return {nullptr, nullptr};
  }
}

// one side of a table row at |x| = a (grid X_k = kXMax (k / kK)^2); s = +1 / -1 picks the side of a full row
float sideAt(const float* row, int sgn, int center, float a) {
  const float t = std::sqrt(a / kXMax) * static_cast<float>(kK);
  int k = static_cast<int>(t);
  if (k >= kK) k = kK - 1;  // past the grid: continue the last segment
  const float x0 = kXMax * static_cast<float>(k * k) / (kK * kK), x1 = kXMax * static_cast<float>((k + 1) * (k + 1)) / (kK * kK);
  const float y0 = row[center + sgn * k], y1 = row[center + sgn * (k + 1)];
  return y0 + (y1 - y0) * (a - x0) / (x1 - x0);
}

float rowAt(const Tab& t, int r, float x) {
  if (t.rows) {
    const float y = sideAt(t.rows[r], 1, 0, std::fabs(x));
    return x < 0 ? -y : y;
  }
  return sideAt(t.rowsFull[r], x < 0 ? -1 : 1, kK, std::fabs(x));
}
}  // namespace

// y for input x (internal units) at drive 0..1, the module's output half included
float fxDistTable(int mode, float drive, float x) {
  const Tab t = tabOf(mode);
  if (!t.rows && !t.rowsFull) return 0.5f * x;
  const float d = std::clamp(drive, 0.f, 1.f) * 10.f;
  const int r = std::min(9, static_cast<int>(d));
  const float f = d - static_cast<float>(r);
  const float a = rowAt(t, r, x), b = f > 0 ? rowAt(t, r + 1, x) : a;
  return 0.5f * (a + (b - a) * f);
}

}  // namespace gl::packs::plasma
