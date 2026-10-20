// Gridline — FL-style point shapes (see FlCurves.h).
#include "FlCurves.h"
#include <algorithm>
#include <cmath>

namespace gl::flcurve {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kLn10 = 2.302585092994045684;

inline double sgn1(double t) { return t < 0 ? -1.0 : 1.0; }  // sign with t = 0 → +1
// Round half to even (the FPU's default rounding mode).
inline double rint(double v) { return std::nearbyint(v); }

// "D" form of a kernel: two halves, the second mirrored.
template <typename F>
double twoHalves(F f, double x) {
  return x < 0.5 ? 0.5 * f(2.0 * x) : 1.0 - 0.5 * f(2.0 * (1.0 - x));
}

double curve2(double t, double x) {  // shape 7
  const double u = t > 0 ? 1.0 - x : x;
  const double u2 = u * u;
  return x - t * (u2 * u2 * u2 - u);
}

double curve3(double t, double x) {  // shape 11
  const double a = std::max(0.9 * std::fabs(t), 1.0 / 65536.0);
  const double b = (1.0 - a) * (1.0 - a) * (1.0 - a);
  const double P = 1.0 - b, Q = b / P;
  return t < 0 ? Q / (1.0 - P * x) - Q : 1.0 - Q / (1.0 - P * (1.0 - x)) + Q;
}

double halfSine(double t, double x) {  // shape 9
  const double q = t > 0 ? std::sin(0.5 * kPi * x) : 1.0 - std::cos(0.5 * kPi * x);
  return x - std::fabs(t) * (x - q);
}

}  // namespace

double curveKernel(double c, double x) {
  if (c == 0) return x;
  const double ac = std::fabs(c), k = std::log1p(ac);
  return c < 0 ? std::expm1(k * x) / ac : 1.0 - std::expm1(k * (1.0 - x)) / ac;
}

double singleCurve(double t, double x) {
  if (t == 0) return x;
  const double at = std::fabs(t);
  const double ac = std::pow(10.0, 6.0 * at) - 1.0, k = 6.0 * at * kLn10;
  return t < 0 ? std::expm1(k * x) / ac : 1.0 - std::expm1(k * (1.0 - x)) / ac;
}

int stepCount(double t) {
  const double v = (std::pow(10.0, 6.0 * (1.0 - 0.6 * std::fabs(t))) - 1.0) / 1000.0;
  return static_cast<int>(rint(v));
}

double smoothTangent(double sigma, double r) {
  if (!(sigma > 0) || !(r > 0) || !std::isfinite(sigma) || !std::isfinite(r)) return 0.0;
  return 3.0 * (r + 1.0) * sigma / ((r + 2.0) + (2.0 * r + 1.0) * sigma);
}

double smooth(double t, double x, double ps, double pe) {
  const double x1 = x - 1.0;
  const double h = ps * x * x1 * x1 + pe * x * x * x1 + 3.0 * x * x - 2.0 * x * x * x - x;
  return x + (1.0 - std::fabs(t)) * h;
}

double shape(int sh, double t, double x) {
  x = std::clamp(x, 0.0, 1.0);
  if (t == 0 && (sh == 0 || sh == 1 || sh == 7 || sh == 8 || sh == 9 || sh == 11 || sh == 12)) return x;
  const double s = sgn1(t);
  switch (sh) {
    case 0: return singleCurve(t, x);
    case 1: return twoHalves([t](double u) { return singleCurve(t, u); }, x);
    case 2: return 0.0;
    case 3: {
      const double m = stepCount(t) + (t >= 0 ? 1 : 0) + 1;
      const double h = s * 0.5;
      return rint(m * x - h) / (m - s);
    }
    case 4: {
      const double m = stepCount(t) + 1;
      const double d = m * x - 0.5, i = std::floor(d), f = 2.0 * (d - i) - 1.0;
      double g;
      if (t >= 0) {
        g = f * f * f;
      } else {
        const double a = 1.0 - std::fabs(f);
        g = (f < 0 ? -1.0 : f > 0 ? 1.0 : 0.0) * (1.0 - a * a * a);
      }
      return (0.5 * g + i + 1.0) / m;
    }
    case 5: {
      const double m = 2.0 * (stepCount(t) + 1);
      const double v = rint(m * x - s * 0.5);
      const double r = std::fmod(v, 2.0);
      return r < 0 ? r + 2.0 : r;
    }
    case 6: {
      const int n = stepCount(t);
      const double m = n + 0.5;
      if (t < 0) return (std::sin(2.0 * kPi * (m * x + 0.75)) + 1.0) * 0.5;
      if (n == 0 && t > 0) return x;
      const double u = m * x - 0.5;
      return std::fabs(2.0 * (u - std::floor(u)) - 1.0);
    }
    case 7: return curve2(t, x);
    case 8: return twoHalves([t](double u) { return curve2(t, u); }, x);
    case 9: return halfSine(t, x);
    case 10: return smooth(t, x, 0.0, 0.0);
    case 11: return curve3(t, x);
    case 12: return twoHalves([t](double u) { return curve3(t, u); }, x);
    default: return 0.0;
  }
}

}  // namespace gl::flcurve
