// Gridline — "plasma" pack: the distortion module's static shapers. Each mode is a closed-form
// function of the input and the drive (0..1), written in units of the module's full scale. The
// module halves its output. Allocation free, finite for any input and any drive.
#include <algorithm>
#include <cmath>
#include "plasma_dsp.h"

namespace gl::packs::plasma {
namespace {

constexpr double kFull = 1.6572;  // the level the clipping modes stop at, in internal units
constexpr double kHalfPi = 1.5707963267948966;
constexpr double kInputLimit = 1e6;

inline double sgn(double v) { return v < 0 ? -1.0 : 1.0; }
inline double clip1(double v) { return std::clamp(v, -1.0, 1.0); }
inline double cube(double v) { return v * v * v; }

// Hyperbolic saturator that reaches a hard ceiling, its knee sharpening with the drive
double tube(double d, double u) { return clip1(u / (1.0 + 0.9889 * d * (std::fabs(u) - 1.0))); }

// Linear up to a knee, then a hyperbola with slope 1 at the knee that settles one unit above it
double softClip(double d, double u) {
  constexpr double kKnee = 0.5;
  const double v = (1.0 + 2.939 * cube(d)) * u, a = std::fabs(v);
  return a > kKnee ? sgn(v) * (kKnee + (a - kKnee) / (a - kKnee + 1.0)) : v;
}

double hardClip(double d, double u) { return clip1((1.0 + 3.98 * cube(d)) * u); }

// A rational limiter that fades in from the straight line over the first eighth of the drive
double diode1(double d, double u) {
  constexpr double kFadeIn = 0.125;
  const double a = std::fabs(u);
  const auto limiter = [&](double dd) {
    const double t = (dd - kFadeIn) / (1.0 - kFadeIn), k = a + 20.89 + 99.4 * cube(t);
    return u * k / ((k + 1.39) * a + 1.0);
  };
  return d >= kFadeIn ? limiter(d) : u + (d / kFadeIn) * (limiter(kFadeIn) - u);
}

// One arc of an offset sine, then a flat tail at a different height on each side (a short ramp joins them)
double diode2Shape(double d, double u) {
  constexpr double kGain = 21.68, kOffset = 0.0753, kEnd = 2.5, kRamp = 0.1, kTailUp = 0.5701, kTailDown = -0.68;
  const double v = kGain * d * u;
  const double arc = std::sin(v + kOffset) - std::sin(kOffset);
  return arc + ((v > 0 ? kTailUp : kTailDown) - arc) * std::clamp((std::fabs(v) - kEnd) / kRamp, 0.0, 1.0);
}
// silent at drive 0: the first tenth of the drive raises the level instead of the gain
double diode2(double d, double u) { return d < 0.1 ? 10.0 * d * diode2Shape(0.1, u) : diode2Shape(d, u); }

// Reflects v back and forth at +-1, each reflection scaling it by k1. The reflections run
// s[n+1] = k1 s[n] - (1 + k1) on the magnitude, which sums to a geometric series, so the count and the
// result come out directly whatever the size of v.
struct Folded {
  double value;
  double rate;  // the running slope scale k1^n with a continuous n; 1 before the first reflection
  bool reflected;
};
Folded reflect(double v, double k1, double logK1) {
  const double s0 = std::fabs(v);
  if (s0 <= 1.0) return {v, 1.0, false};
  const double pole = -(1.0 + k1) / (1.0 - k1), rate = (1.0 - pole) / (s0 - pole);
  const double n = std::ceil(std::log(rate) / logK1);
  const double s = std::clamp(pole + std::exp(n * logK1) * (s0 - pole), -1.0, 1.0);
  return {(std::fmod(n, 2.0) == 0.0) == (v >= 0) ? s : -s, rate, true};
}
// The folds lose height as they crowd together: r is how quickly the shaper swings at this input
inline double crowding(double r, double q) { return std::pow(1.0 + r * r, -q); }

double linearFold(double d, double u) {
  const double g = 1.0 + 2128.0 * cube(d);
  const Folded f = reflect(g * u, 0.925, -0.0779615415);
  return f.value * crowding(0.01328 * std::sqrt(std::fabs(u)) * g * f.rate, 1.151);
}

double sineFold(double d, double u) {
  const double g = 1.0 + 2125.0 * cube(d);
  const Folded f = reflect(g * u, 0.8, -0.2231435513);
  const double y = f.reflected ? std::sin(kHalfPi * f.value) : f.value;
  return y * crowding(0.0389 * std::sqrt(std::fabs(u)) * g * f.rate, 0.1046);
}

// A steep arctangent S that jumps away from zero by the drive (a very short ramp keeps it continuous)
double zeroSquare(double d, double u) {
  constexpr double kRamp = 0.004;
  const double a = std::fabs(u), w = a + 1.001 * d * std::min(a / kRamp, 1.0);
  return sgn(u) * 0.9 * std::pow(std::atan(std::pow(1.0987 * w, 5.0)), 0.2);
}

// A square-root limiter whose denominator is a parabola of the input: the positive side droops
// back down at high levels, the negative side clips first
double asymmetric(double d, double u) {
  const double v = (1.0 + 19.93 * d * d) * u, w = (0.05165 * v + 1.0386) * v;
  return clip1(v / std::sqrt(w * w + 0.6235));
}

double rectify(double d, double u) { return -std::min(1.0, (1.0 + 19.92 * d * d) * std::fabs(u)); }

// A sine of the driven input through three bending stages of y (2 |y| - 4), the last one halved
double sineShaper(double d, double u) {
  const double g = d * d + 0.04955, h = 0.9577 * g, w = 0.5096 + 14.46 * g;
  double y = -h * std::sin(w * u);
  y *= 2.0 * std::fabs(y) - 4.0;
  y *= 2.0 * std::fabs(y) - 4.0;
  y *= std::fabs(y) - 2.0;
  return y * crowding(0.8686 * std::sqrt(std::fabs(u)) * h * w, 0.079);
}

// Two straight lines: the driven gain up to a knee, then a shallow line through (1, 1)
double stompBox(double d, double u) {
  constexpr double kKnee = 0.8005;
  const double g = 1.0 + 6.956 * d, a = std::fabs(u);
  return g * a < kKnee ? g * u : sgn(u) * (1.0 + (a - 1.0) * (1.0 - kKnee) / (1.0 - kKnee / g));
}

// The driven gain up to a knee, then a cosine crest at (1, 1) that rolls back down and wraps around
double tapeShape(double d, double u) {
  constexpr double kKnee = 0.6682, kArc = 0.8387;  // kArc = acos(kKnee), so the two parts meet
  const double g = 1.0 + 3.046 * d, a = std::fabs(u);
  return g * a < kKnee ? g * u : sgn(u) * std::cos(kArc * (a - 1.0) / (1.0 - kKnee / g));
}
// straight at drive 0: the first tenth of the drive crossfades into the shape
double tapeSat(double d, double u) { return d < 0.1 ? u + 10.0 * d * (tapeShape(0.1, u) - u) : tapeShape(d, u); }

// Linear to a knee, a parabola to the ceiling
double softSat(double d, double u) {
  constexpr double kTop = 1.4994;
  const double v = (1.0 + 15.9 * d * d) * u, a = std::min(std::fabs(v), kTop);
  return sgn(v) * (a > kTop - 1.0 ? 1.0 - 0.5 * (kTop - a) * (kTop - a) : a);
}

using Shape = double (*)(double, double);

// The fold pattern races past a fixed input as the drive turns. These modes move between ten drive
// steps by crossfading, which keeps a sweep from zippering and the level from jumping.
double stepped(Shape shape, double d, double u) {
  const double pos = d * 10.0;
  const int r = std::min(9, static_cast<int>(pos));
  const double f = pos - r, a = shape(r * 0.1, u);
  return f > 0 ? a + (shape((r + 1) * 0.1, u) - a) * f : a;
}
}  // namespace

// y for input x (internal units) at drive 0..1, the module's output half included
float fxDistTable(int mode, float drive, float x) {
  const double d = drive > 0 ? std::min(static_cast<double>(drive), 1.0) : 0.0;  // NaN reads as 0
  const double in = x == x ? std::clamp(static_cast<double>(x), -kInputLimit, kInputLimit) : 0.0;
  const double u = in / kFull;
  double y;
  switch (mode) {
    case 0: y = tube(d, u); break;
    case 1: y = softClip(d, u); break;
    case 2: y = hardClip(d, u); break;
    case 3: y = diode1(d, u); break;
    case 4: y = diode2(d, u); break;
    case 5: y = stepped(linearFold, d, u); break;
    case 6: y = stepped(sineFold, d, u); break;
    case 7: y = zeroSquare(d, u); break;
    case 9: y = asymmetric(d, u); break;
    case 10: y = rectify(d, u); break;
    case 13: y = stepped(sineShaper, d, u); break;
    case 14: y = stompBox(d, u); break;
    case 15: y = tapeSat(d, u); break;
    case 17: y = softSat(d, u); break;
    default: return static_cast<float>(0.5 * in);
  }
  return static_cast<float>(0.5 * kFull * y);
}

}  // namespace gl::packs::plasma
