// Gridline — multi-mode envelope curves (the point model of the time/volume
// slot editors, the filter-unit envelopes and the waveshaper curve).
//
// An envelope is a list of points (x, y, tension, mode). Each point carries
// the shape of the segment that ENDS at it (from the previous point). Inside
// a segment the value is y0 + (y1 - y0) * r(f) with f in [0,1) the position
// in the segment and r the mode's curve; equal x values make vertical jumps.
// Before the first point the value is y[0], after the last y[last].
//
// Curve modes (tension t in -1..1; negative = slow start / fast end):
//   0 single curve   exponential, k = ln(1e6) * |t|
//   1 double curve   two mirrored single curves (S shape)
//   2 hold           keeps the previous value until the next point
//   3 stairs         n steps, n from |t|
//   4 smooth stairs  steps joined by cubic ramps
//   5 pulse          n on/off pulses between the two values
//   6 wave           n triangle (t >= 0) or sine (t < 0) cycles
//   7 single curve 2 polynomial bend f - t (g^6 - g)
//   8 double curve 2
//   9 half sine      quarter-sine bend blended by |t|
//  10 smooth         monotone cubic (tangents from neighbouring smooth points)
//  11 single curve 3 rational bend
//  12 double curve 3
// Audio-thread safe: fixed arrays, no allocation; prepare() precomputes the
// per-point coefficients (call after editing).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace gl::menv {

enum Mode : uint8_t {
  SingleCurve, DoubleCurve, Hold, Stairs, SmoothStairs, Pulse, Wave, SingleCurve2, DoubleCurve2, HalfSine, Smooth, SingleCurve3, DoubleCurve3,
  ModeCount
};

inline constexpr double kLn1e6 = 13.815511557963774;
inline constexpr double kPi = 3.14159265358979323846;

// Step / pulse / cycle count of the stepped modes from |t|.
inline int stepCount(float t) {
  float a = std::fabs(t);
  double v = (std::exp(kLn1e6 * (1.0 - 0.6 * static_cast<double>(a))) - 1.0) * 0.001;
  return static_cast<int>(std::nearbyint(v));  // round half to even (default FP mode)
}

struct Coef {
  int8_t sign = 0;  // -1, 0, +1, or 2 = linear
  float A = 0, B = 0, C = 0;
};

// Coefficients of one segment (mode + tension). `prevSmooth`/`nextSmooth`
// tangents are only used by mode 10 and filled by the envelope.
inline Coef coef(uint8_t mode, float t) {
  Coef c;
  c.sign = t > 0 ? 1 : t < 0 ? -1 : 0;
  switch (mode) {
    case SingleCurve:
    case DoubleCurve: {
      double a = std::fabs(static_cast<double>(t));
      double v = (c.sign < 0 ? -1.0 : 1.0) * (std::exp(kLn1e6 * a) - 1.0);
      if (c.sign == 0) v = 0;
      c.A = static_cast<float>(v);
      c.B = v != 0 ? static_cast<float>(1.0 / std::fabs(v)) : 0.f;
      c.C = static_cast<float>(kLn1e6 * a);
      break;
    }
    case Stairs:
    case Pulse: {
      int n = stepCount(t);
      if (mode == Stairs && t >= 0) ++n;
      double s = t < 0 ? -1.0 : 1.0;
      double N = n + 1;
      if (mode == Stairs) {
        c.A = static_cast<float>(N);
        c.C = static_cast<float>(0.5 * s);
        c.B = static_cast<float>(1.0 / (N - s));
      } else {
        c.C = static_cast<float>(0.5 * s);
        c.A = static_cast<float>(2.0 * N);
      }
      break;
    }
    case SmoothStairs: {
      int n = stepCount(t);
      c.A = static_cast<float>(n + 1);
      c.B = 1.f / c.A;
      break;
    }
    case Wave: {
      int n = stepCount(t);
      c.A = static_cast<float>(n + 0.5);
      c.B = static_cast<float>(2 * kPi);
      if (n == 0 && t > 0) c.sign = 2;
      break;
    }
    case SingleCurve2:
    case DoubleCurve2: c.A = t; break;
    case HalfSine: c.A = std::fabs(t); break;
    case Smooth: c.C = 1.f - std::fabs(t); break;
    case SingleCurve3:
    case DoubleCurve3: {
      double x = std::max(0.9 * std::fabs(static_cast<double>(t)), 1.0 / 65536.0);
      double A = 1.0 - (1.0 - x) * (1.0 - x) * (1.0 - x);
      double B = (1.0 - A) / A, C = B;
      if (t > 0) {
        double b0 = B;
        B = -B;
        C = -b0 - 1.0;
      }
      if (mode == DoubleCurve3) B *= 0.5, C *= 0.5;
      c.A = static_cast<float>(A), c.B = static_cast<float>(B), c.C = static_cast<float>(C);
      break;
    }
    default: break;
  }
  return c;
}

namespace detail {
inline double r0(const Coef& c, double f) {
  if (c.A == 0) return f;
  if (c.A > 0) return 1.0 - c.B * (std::exp(c.C * (1.0 - f)) - 1.0);
  return c.B * (std::exp(c.C * f) - 1.0);
}
inline double r7(const Coef& c, double f) {
  double g = c.sign > 0 ? 1.0 - f : f;
  double g2 = g * g, g6 = g2 * g2 * g2;
  return f - c.A * (g6 - g);
}
inline double r11(const Coef& c, double f) {
  if (c.sign > 0) return c.B / (1.0 - c.A * (1.0 - f)) - c.C;
  return c.B / (1.0 - c.A * f) - c.C;
}
}  // namespace detail

// Shape r(f) of a segment, f in [0,1). Smooth (mode 10) needs the tangents.
inline double shape(uint8_t mode, const Coef& c, double f, double tanStart = 0, double tanEnd = 0) {
  using namespace detail;
  if (c.sign == 2) return f;
  switch (mode) {
    case SingleCurve: return r0(c, f);
    case DoubleCurve: return f < 0.5 ? 0.5 * r0(c, 2 * f) : 1.0 - 0.5 * r0(c, 2 * (1 - f));
    case Hold: return 0;
    case Stairs: return std::nearbyint(c.A * f - c.C) * c.B;
    case SmoothStairs: {
      double u = c.A * f - 0.5;
      double k = std::nearbyint(u - 0.5);
      double q = 2 * (u - k) - 1;
      double s = c.sign < 0 ? (q < 0 ? -1.0 : 1.0) * (1.0 - std::pow(1.0 - std::fabs(q), 3.0)) : q * q * q;
      return c.B * (k + 1 + 0.5 * s);
    }
    case Pulse: return static_cast<double>(static_cast<long long>(std::nearbyint(c.A * f - c.C)) & 1);
    case Wave: {
      if (c.sign < 0) return (std::sin(c.B * (c.A * f + 0.75)) + 1) * 0.5;
      double u = c.A * f - 0.5;
      double k = std::nearbyint(u - 0.5);
      return std::fabs(2 * (u - k) - 1);
    }
    case SingleCurve2: return r7(c, f);
    case DoubleCurve2: return f < 0.5 ? 0.5 * r7(c, 2 * f) : 1.0 - 0.5 * r7(c, 2 * (1 - f));
    case HalfSine: {
      double s = c.sign > 0 ? std::sin(kPi * f * 0.5) : 1.0 - std::cos(kPi * f * 0.5);
      return f - c.A * (f - s);
    }
    case Smooth: {
      double f2 = f * f, f3 = f2 * f;
      return f + c.C * (3 * f2 - 2 * f3 + tanEnd * f2 * (f - 1) + tanStart * f * (f - 1) * (f - 1) - f);
    }
    case SingleCurve3: return r11(c, f);
    case DoubleCurve3: return f < 0.5 ? 0.5 * r11(c, 2 * f) : 1.0 - 0.5 * r11(c, 2 * (1 - f));
    default: return f;
  }
}

// Fixed-capacity envelope.
template <int N>
struct Env {
  static constexpr int kMax = N;
  int n = 0;
  double x[N] = {};
  float y[N] = {}, t[N] = {};
  uint8_t mode[N] = {};
  Coef c[N];
  float tanS[N] = {}, tanE[N] = {};  // smooth-mode tangents (start / end of the segment ending at i)

  void clear() { n = 0; }
  bool add(double px, float py, float pt = 0, uint8_t pm = SingleCurve) {
    if (n >= N) return false;
    x[n] = px, y[n] = py, t[n] = pt, mode[n] = pm < ModeCount ? pm : static_cast<uint8_t>(SingleCurve);
    ++n;
    return true;
  }
  void insert(int at, double px, float py, float pt, uint8_t pm) {
    if (n >= N || at < 0 || at > n) return;
    for (int i = n; i > at; --i) x[i] = x[i - 1], y[i] = y[i - 1], t[i] = t[i - 1], mode[i] = mode[i - 1];
    x[at] = px, y[at] = py, t[at] = pt, mode[at] = pm;
    ++n;
  }
  void erase(int at) {
    if (at < 0 || at >= n) return;
    for (int i = at; i + 1 < n; ++i) x[i] = x[i + 1], y[i] = y[i + 1], t[i] = t[i + 1], mode[i] = mode[i + 1];
    --n;
  }
  double length() const { return n ? x[n - 1] : 0.0; }

  // Precomputes the curve coefficients (after any edit).
  void prepare() {
    for (int i = 0; i < n; ++i) c[i] = coef(mode[i], t[i]);
    // Smooth mode: monotone cubic tangents (in units of the segment's normalised slope).
    for (int i = 1; i < n; ++i) {
      tanS[i] = tanE[i] = 0;
      if (mode[i] != Smooth) continue;
      double h = x[i] - x[i - 1], dy = static_cast<double>(y[i]) - y[i - 1];
      if (h <= 0 || std::fabs(dy) < 6e-8) {
        c[i].sign = 2;
        continue;
      }
      double d = dy / h;
      auto tangent = [&](int j0, int j1, int j2) {  // tangent at j1 between segments j0-j1, j1-j2 (Fritsch–Butland)
        double h0 = x[j1] - x[j0], h1 = x[j2] - x[j1];
        if (h0 <= 0 || h1 <= 0) return 0.0;
        double d0 = (static_cast<double>(y[j1]) - y[j0]) / h0, d1 = (static_cast<double>(y[j2]) - y[j1]) / h1;
        if (d0 * d1 <= 0) return 0.0;
        return 3 * d0 * d1 * (h0 + h1) / ((2 * h1 + h0) * d0 + (h1 + 2 * h0) * d1);
      };
      double m0 = (i >= 2 && mode[i - 1] == Smooth) ? tangent(i - 2, i - 1, i) : d;
      double m1 = (i + 1 < n && mode[i + 1] == Smooth) ? tangent(i - 1, i, i + 1) : d;
      // Express as f-domain derivatives of the normalised curve (value 0..1 over the segment).
      tanS[i] = static_cast<float>(m0 / d);
      tanE[i] = static_cast<float>(m1 / d);
    }
  }
  // Index of the segment end point for x (first point with x > xq), n when past the end.
  int segmentAt(double xq) const {
    int lo = 0, hi = n;
    while (lo < hi) {
      int mid = (lo + hi) / 2;
      if (x[mid] > xq) hi = mid;
      else lo = mid + 1;
    }
    return lo;
  }
  double eval(double xq) const {
    if (n <= 0) return 0;
    if (xq <= x[0]) return y[0];
    int i = segmentAt(xq);
    if (i >= n) return y[n - 1];
    double w = x[i] - x[i - 1];
    if (w <= 0) return y[i];
    double f = (xq - x[i - 1]) / w;
    // Smooth tangents are normalised to the segment (straight line = 1).
    double r = mode[i] == Smooth ? shape(Smooth, c[i], f, tanS[i], tanE[i]) : shape(mode[i], c[i], f);
    return y[i - 1] + (static_cast<double>(y[i]) - y[i - 1]) * r;
  }
};

}  // namespace gl::menv
