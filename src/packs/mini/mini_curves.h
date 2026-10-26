// Gridline — "mini" pack: transfer curves shared by the DSP and the editors
// (Burn, Bend, Shear, Press) so a drawn curve is exactly what is heard.
#pragma once
#include <algorithm>
#include <cmath>

namespace gl::packs::mini::curve {

inline constexpr double kPi = 3.14159265358979323846;

// Burn: one saturation per type (input already driven and biased).
inline double burn(int type, double x) {
  switch (type) {
    case 1:  // Tube: asymmetric, the negative side clips harder
      return x >= 0 ? std::tanh(x) : std::tanh(1.6 * x) / 1.6 * 1.25;
    case 2:  // Hard
      return std::clamp(x, -1.0, 1.0);
    case 3: {  // Fold: triangle fold back into [-1, 1]
      double t = std::fmod(std::fabs(x) + 1.0, 4.0);
      double y = t < 2.0 ? t - 1.0 : 3.0 - t;
      return x >= 0 ? y : -y;
    }
    case 4:  // Sine
      return std::sin(x);
    case 5: {  // Step: saturated, then 9 levels
      double s = std::tanh(x);
      return std::round(s * 4.0) / 4.0;
    }
    default:  // Warm
      return std::tanh(x);
  }
}

// Bend: shape of x in [-1, 1] (amount 0..1), then the edge rule outside.
inline double bendEdge(int edge, double x) {
  if (x >= -1.0 && x <= 1.0) return x;
  if (edge == 1) {  // Mirror
    double t = std::fmod(std::fabs(x) + 1.0, 4.0);
    double y = t < 2.0 ? t - 1.0 : 3.0 - t;
    return x >= 0 ? y : -y;
  }
  if (edge == 2) {  // Wrap
    double t = std::fmod(x + 1.0, 2.0);
    if (t < 0) t += 2.0;
    return t - 1.0;
  }
  return std::clamp(x, -1.0, 1.0);
}
inline double bendShape(int shape, double amount, double x) {
  const double a = std::clamp(amount, 0.0, 1.0);
  const double s = x < 0 ? -1.0 : 1.0, m = std::min(1.0, std::fabs(x));
  switch (shape) {
    case 1:  // Steep S: expands small values
      return s * std::pow(m, 1.0 / (1.0 + 4.0 * a));
    case 2: {  // Steps
      double n = 2.0 + (1.0 - a) * 14.0;
      return (1.0 - a * 0.999) * x + a * 0.999 * std::round(x * n) / n;
    }
    case 3:  // Ripple
      return std::clamp(x + a * 0.3 * std::sin(6.0 * kPi * x), -1.0, 1.0);
    case 4:  // Lean: even harmonics (the DSP removes the offset)
      return std::clamp(x + a * 0.5 * (x * x), -1.0, 1.0);
    case 5:  // Bounce: blends towards a full-wave rectifier
      return (1.0 - a) * x + a * (2.0 * m - 1.0);
    default:  // Smooth S
      return s * (1.0 - std::pow(1.0 - m, 1.0 + 3.0 * a));
  }
}
inline double bend(int shape, double amount, int edge, double x) {
  double e = bendEdge(edge, x);
  return bendShape(shape, amount, e);
}

// Shear: clip at c (linear) with a soft knee of `soft` (0..1) below it.
inline double shear(double x, double c, double soft) {
  const double m = std::fabs(x);
  const double t = c * (1.0 - std::clamp(soft, 0.0, 0.999));
  if (m <= t) return x;
  double y = soft <= 0.0 ? c : t + (c - t) * std::tanh((m - t) / std::max(1e-9, c - t));
  return x < 0 ? -y : y;
}

// Press: static gain change (dB) for a detector level (dB).
inline double pressGain(double db, double th, double ratio, double knee) {
  const double over = db - th;
  const double slope = 1.0 / std::max(1.0, ratio) - 1.0;
  if (knee > 0 && 2 * std::fabs(over) <= knee) return slope * (over + knee / 2) * (over + knee / 2) / (2 * knee);
  if (2 * over < -knee) return 0;
  return slope * over;
}

}  // namespace gl::packs::mini::curve
