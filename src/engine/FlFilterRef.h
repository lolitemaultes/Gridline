// Gridline — J-26: reference kernels of the classic filter family (studies
// analysis/demos/specs/j26_filter.md, j26_fastlp.md, j26_bassboost.md,
// j26_freefilter.md): a 4 Hz two-pole DC remover, a per-sample coefficient
// ramp, the multi-tap state-variable filter, the one-knob resonant low-pass,
// the bass booster and the fixed-design biquad. Coefficients that the
// originals define per sample (independent of the sample rate) stay so.
#pragma once
#include <algorithm>
#include <cmath>

namespace gl::flref {

inline constexpr double kPi = 3.14159265358979323846;

// Two-pole DC remover (≈ 4 Hz): v += (x - y)·k1; y += v; v ·= 0.96; out = x - y.
struct DcRemover {
  double y = 0, v = 0, k1 = 0;
  void set(double sr) { k1 = 1.0070235 / sr, y = v = 0; }
  float tick(float x) {
    v += (x - y) * k1;
    y += v;
    v *= 0.96;
    return static_cast<float>(x - y);
  }
  void clear() { y = v = 0; }
};

// Per-sample linear ramp toward a target over 128 samples (a ramped value
// below 5.96e-8 snaps to 0).
struct Ramp128 {
  float v = 0, step = 0;
  int left = 0;
  bool primed = false;
  float target = 0;
  void set(float t) {
    if (!primed) {
      v = target = t, left = 0, primed = true;
      return;
    }
    if (t == target) return;
    target = t;
    step = (t - v) / 128.0f;
    left = 128;
  }
  float tick() {
    if (left > 0) {
      v += step;
      if (--left == 0) v = target;
      if (std::fabs(v) < 5.96e-8f) v = 0;
    }
    return v;
  }
};

// Multi-tap state-variable filter: knob positions n (0..1).
inline float filterCutCoef(double n) { return static_cast<float>((std::pow(26.0, std::clamp(n, 0.0, 1.0)) - 1) / 25.0); }
inline float filterDamping(double n) { return static_cast<float>(1.0 - 0.8 * std::clamp(n, 0.0, 1.0)); }
inline float filterLevel(double n) { return static_cast<float>((std::pow(11.0, std::clamp(n, 0.0, 1.0)) - 1) / 10.0); }
// Equivalent analogue cutoff of the coefficient at a sample rate (display only).
inline double filterCutHz(double n, double sr) { return sr / kPi * std::asin(std::min(1.0, filterCutCoef(n) / 2.0)); }

struct MultiSvf {
  Ramp128 f, d, lo, bp, hi;
  float s[2][4] = {};  // per channel: lp, bp (stage 1), lp, bp (stage 2)
  DcRemover dc[2];
  double sr = 0;
  bool twoStage = false;
  void clear() {
    for (auto& c : s)
      for (float& v : c) v = 0;
    dc[0].clear(), dc[1].clear();
  }
  void process(double cutN, double resN, double loN, double bpN, double hiN, bool x2, bool center, double rate, float* L, float* R, int n) {
    if (rate != sr) sr = rate, dc[0].set(rate), dc[1].set(rate);
    if (x2 != twoStage) {
      twoStage = x2;
      for (auto& c : s)
        for (float& v : c) v = 0;
    }
    f.set(filterCutCoef(cutN)), d.set(filterDamping(resN)), lo.set(filterLevel(loN)), bp.set(filterLevel(bpN)), hi.set(filterLevel(hiN));
    for (int i = 0; i < n; ++i) {
      const float cf = f.tick(), cd = d.tick(), L1 = lo.tick(), B1 = bp.tick(), H1 = hi.tick();
      float* io[2] = {L + i, R + i};
      for (int c = 0; c < 2; ++c) {
        float* st = s[c];
        float x = *io[c];
        st[0] += cf * st[1];
        float h = x - st[0] - cd * st[1];
        st[1] += cf * h;
        float y = H1 * h + B1 * st[1] + L1 * st[0];
        if (x2) {
          st[2] += cf * st[3];
          float h2 = y - st[2] - st[3];
          st[3] += cf * h2;
          y = H1 * h2 + B1 * st[3] + L1 * st[2];
        }
        *io[c] = center ? dc[c].tick(y) : y;
      }
    }
    for (auto& c : s)
      for (float& v : c)
        if (std::fabs(v) < 1e-30f) v = 0;
  }
};

// One-knob resonant low-pass: v += a(x - y); y += v; out = y; v ·= b.
inline float quickLpCoef(double cutN) {
  double x = (std::clamp(cutN, 0.0, 1.0) + 1.0 / 63.0) * (63.0 / 64.0);
  if (x >= 1) return 1.0f;
  if (x <= 0) return 0.0f;
  return static_cast<float>((std::exp(x * std::log(601.0)) - 1) / 600.0);
}
inline float quickLpRes(double resN) {
  double r = std::min(0.625, 0.625 * std::clamp(resN, 0.0, 1.0));
  if (r <= 0) return 0.0f;
  return static_cast<float>(1 - (std::exp((1 - r) * std::log(601.0)) - 1) / 600.0);
}
struct QuickLp {
  Ramp128 a;
  double y[2] = {}, v[2] = {};
  void clear() { y[0] = y[1] = v[0] = v[1] = 0; }
  void process(double cutN, double resN, float* L, float* R, int n) {
    a.set(quickLpCoef(cutN));
    const double b = quickLpRes(resN);
    for (int i = 0; i < n; ++i) {
      const double ca = a.tick();
      float* io[2] = {L + i, R + i};
      for (int c = 0; c < 2; ++c) {
        v[c] += ca * (*io[c] - y[c]);
        y[c] += v[c];
        *io[c] = static_cast<float>(y[c]);
        v[c] *= b;
      }
    }
    for (int c = 0; c < 2; ++c) {
      if (std::fabs(y[c]) < 1e-30) y[c] = 0;
      if (std::fabs(v[c]) < 1e-30) v[c] = 0;
    }
  }
};

// Bass booster: dry + G · (two-pole low-pass at an integer frequency in Hz).
inline int bassBoostHz(double n) { return static_cast<int>(std::nearbyint(80.0 * std::clamp(n, 0.0, 1.0) + 20.0)); }
inline double bassBoostGain(double n) { return (std::pow(11.0, std::clamp(n, 0.0, 1.0)) - 1) * 0.5; }
struct BassBooster {
  double s[2] = {}, v[2] = {}, sr = 0, hz = -1;
  float k1 = 0, k2 = 0;
  void clear() { s[0] = s[1] = v[0] = v[1] = 0; }
  void process(double freqHz, double gainG, double rate, float* L, float* R, int n) {
    if (rate != sr || freqHz != hz) {
      sr = rate, hz = freqHz;
      double c = std::cos(2 * kPi * freqHz / rate), cm = c - 1;
      k1 = static_cast<float>(2 - 2 * c);
      k2 = static_cast<float>((cm / 2 + std::sqrt(2.0) * std::sqrt(std::max(0.0, -cm * cm * cm))) / (cm / 2));
    }
    for (int i = 0; i < n; ++i) {
      float* io[2] = {L + i, R + i};
      for (int c = 0; c < 2; ++c) {
        const double x = *io[c];
        v[c] += (x - s[c]) * k1;
        s[c] += v[c];
        v[c] *= k2;
        *io[c] = static_cast<float>(x + gainG * s[c]);
      }
    }
    for (int c = 0; c < 2; ++c) {
      if (std::fabs(s[c]) < 1e-30) s[c] = 0;
      if (std::fabs(v[c]) < 1e-30) v[c] = 0;
    }
  }
};

// Fixed-design biquad (types 0 LP, 1 BP, 2 HP, 3 notch, 4 low shelf, 5 peak,
// 6 high shelf). The design assumes 44.1 kHz whatever the host rate.
inline double freeFilterHz(double n) { return 10.0 + 159.9 * (std::pow(101.0, std::clamp(n, 0.0, 1.0)) - 1); }
inline double freeFilterDb(double n) { return 36.0 * std::clamp(n, 0.0, 1.0) - 18.0; }
inline int freeFilterType(double n) { return std::clamp(static_cast<int>(std::nearbyint(7.0 * std::clamp(n, 0.0, 1.0) - 0.5)) + 1, 1, 7) - 1; }
struct FixedBiquad {
  float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  float x1[2] = {}, x2[2] = {}, y1[2] = {}, y2[2] = {};
  void clear() {
    for (int c = 0; c < 2; ++c) x1[c] = x2[c] = y1[c] = y2[c] = 0;
  }
  void design(int type, double hz, double qN, double gainDb) {
    double w = std::max(0.0004, std::min(hz, 22000.0) * 2 * kPi / 44100.0), sn = std::sin(w), cs = std::cos(w);
    qN = std::clamp(qN, 0.0, 1.0);
    double B0, B1, B2, A0, A1, A2;
    if (type <= 3) {
      double q = 0.1 + 18.9 * qN, al = sn / (2 * q);
      A0 = 1 + al, A1 = -2 * cs, A2 = 1 - al;
      if (type == 0) B1 = 1 - cs, B0 = B2 = B1 / 2;
      else if (type == 1) B0 = al, B1 = 0, B2 = -al;
      else if (type == 2) B0 = B2 = (1 + cs) / 2, B1 = -(1 + cs);
      else B0 = B2 = 1, B1 = -2 * cs;
    } else if (type == 5) {
      double A = std::pow(10.0, gainDb / 40), q = 0.1 + 0.0999 * (std::pow(1001.0, qN) - 1), al = sn / (2 * q);
      A0 = 1 + al / A, B0 = 1 + al * A, B1 = -2 * cs, B2 = 1 - al * A, A1 = -2 * cs, A2 = 1 - al / A;
    } else {
      double A = std::pow(10.0, gainDb / 40), S = 0.1 + 1.9 * qN;
      double beta = std::sqrt(std::max(0.0, (A * A + 1) / S - (A - 1) * (A - 1))), bs = beta * sn;
      if (type == 4) {
        A0 = (A + 1) + (A - 1) * cs + bs, B0 = A * ((A + 1) - (A - 1) * cs + bs), B1 = 2 * A * ((A - 1) - (A + 1) * cs);
        B2 = A * ((A + 1) - (A - 1) * cs - bs), A1 = -2 * ((A - 1) + (A + 1) * cs), A2 = (A + 1) + (A - 1) * cs - bs;
      } else {
        A0 = (A + 1) - (A - 1) * cs + bs, B0 = A * ((A + 1) + (A - 1) * cs + bs), B1 = -2 * A * ((A - 1) + (A + 1) * cs);
        B2 = A * ((A + 1) + (A - 1) * cs - bs), A1 = 2 * ((A - 1) - (A + 1) * cs), A2 = (A + 1) - (A - 1) * cs - bs;
      }
    }
    b0 = static_cast<float>(B0 / A0), b1 = static_cast<float>(B1 / A0), b2 = static_cast<float>(B2 / A0);
    a1 = static_cast<float>(A1 / A0), a2 = static_cast<float>(A2 / A0);
  }
  float tick(int c, float x) {
    float y = b0 * x + b1 * x1[c] + b2 * x2[c] - a1 * y1[c] - a2 * y2[c];
    x2[c] = x1[c], x1[c] = x, y2[c] = y1[c], y1[c] = std::fabs(y) < 1e-30f ? 0.0f : y;
    return y;
  }
};

}  // namespace gl::flref
