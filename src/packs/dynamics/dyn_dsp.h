// Gridline — "dynamics" pack: shared realtime building blocks (header-only).
// Everything here is allocation-free after construction and processes one
// sample at a time, so results never depend on the host block size.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>
#include "../../engine/Dsp.h"
#include "../../engine/Effects.h"
#include "dynamics.h"

namespace gl::packs::dynamics {

using dsp::kPi;
using dsp::kTwoPi;

inline double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline double dbToLin(double db) { return std::pow(10.0, db * 0.05); }
inline double linToDb(double g) { return g > 1e-9 ? 20.0 * std::log10(g) : -180.0; }
inline double flushDenormal(double v) { return std::fabs(v) < 1e-25 ? 0.0 : v; }
inline bool finite(double v) { return std::isfinite(v); }
// Exponential smoothing coefficient for a time constant in seconds.
inline double timeCoef(double seconds, double sr) { return seconds <= 0 ? 0.0 : std::exp(-1.0 / std::max(1.0, seconds * sr)); }

// --------------------------------------------------------------- base class
// Effect with parameter access that tolerates short / old parameter vectors.
class DynFx : public Effect {
 public:
  explicit DynFx(const std::string& type) {
    PluginState d = defaultPluginState(type);
    defaults_ = d.params;
  }
  float meter() const override { return meter_; }
  float inputLevel() const override { return input_; }

 protected:
  double p(const PluginState& s, int k) const {
    size_t i = static_cast<size_t>(k);
    if (i < s.params.size()) return s.params[i];
    return i < defaults_.size() ? defaults_[i] : 0.0;
  }
  int pi(const PluginState& s, int k) const { return static_cast<int>(std::lround(p(s, k))); }
  bool pb(const PluginState& s, int k) const { return p(s, k) >= 0.5; }
  std::vector<float> defaults_;
  float meter_ = 0, input_ = -120;
};

// --------------------------------------------------------------- one-pole TPT
struct OnePoleTpt {
  double g = 0, s = 0;
  void set(double sr, double hz) {
    double w = std::tan(kPi * clampd(hz, 1.0, sr * 0.49) / sr);
    g = w / (1.0 + w);
  }
  double lp(double x) {
    double v = (x - s) * g;
    double y = v + s;
    s = flushDenormal(y + v);
    return y;
  }
  double hp(double x) { return x - lp(x); }
  double ap(double x) { return 2.0 * lp(x) - x; }
  void reset() { s = 0; }
};

// --------------------------------------------------------------- SVF TPT
// Zavalishin state-variable filter with direct damping (k = 1/Q).
struct SvfTpt {
  double g = 0, k = 1.41421356, a1 = 0, a2 = 0, a3 = 0, ic1 = 0, ic2 = 0;
  double lp = 0, bp = 0, hp = 0;
  void set(double sr, double hz, double q) {
    g = std::tan(kPi * clampd(hz, 5.0, sr * 0.49) / sr);
    k = 1.0 / std::max(0.05, q);
    a1 = 1.0 / (1.0 + g * (g + k));
    a2 = g * a1;
    a3 = g * a2;
  }
  void tick(double x) {
    double v3 = x - ic2;
    double v1 = a1 * ic1 + a2 * v3;
    double v2 = ic2 + a2 * ic1 + a3 * v3;
    ic1 = flushDenormal(2.0 * v1 - ic1);
    ic2 = flushDenormal(2.0 * v2 - ic2);
    lp = v2;
    bp = v1;
    hp = x - k * v1 - v2;
  }
  double allpass(double x) {
    tick(x);
    return x - 2.0 * k * bp;
  }
  void reset() { ic1 = ic2 = lp = bp = hp = 0; }
};

// --------------------------------------------------------------- crossover
// One split point with 6 (complementary), 12 (LR2) or 24 dB/oct (LR4) slopes.
// low + high is flat in magnitude (an allpass) for every slope.
struct Split {
  int slope = 2;  // 0 = 6 dB, 1 = 12 dB, 2 = 24 dB
  OnePoleTpt a, b, c, d;
  SvfTpt s1, s2, s3, s4, apf;
  OnePoleTpt ap1;
  void set(double sr, double hz, int sl) {
    slope = sl;
    a.set(sr, hz); b.set(sr, hz); c.set(sr, hz); d.set(sr, hz); ap1.set(sr, hz);
    const double q = 0.70710678;
    s1.set(sr, hz, q); s2.set(sr, hz, q); s3.set(sr, hz, q); s4.set(sr, hz, q); apf.set(sr, hz, q);
  }
  void process(double x, double& lo, double& hi) {
    switch (slope) {
      case 0:
        lo = a.lp(x);
        hi = x - lo;
        break;
      case 1:
        lo = b.lp(a.lp(x));
        hi = -d.hp(c.hp(x));  // inverted so that lo + hi = first-order allpass
        break;
      default:
        s1.tick(x);
        s2.tick(s1.lp);
        lo = s2.lp;
        s3.tick(x);
        s4.tick(s3.hp);
        hi = s4.hp;
        break;
    }
  }
  // The phase response of lo + hi, applied to bands that skip this split.
  double compensate(double x) {
    switch (slope) {
      case 0: return x;
      case 1: return ap1.ap(x);
      default: return apf.allpass(x);
    }
  }
  void reset() {
    a.reset(); b.reset(); c.reset(); d.reset(); ap1.reset();
    s1.reset(); s2.reset(); s3.reset(); s4.reset(); apf.reset();
  }
};

// Three bands; low + mid + high is an allpass of the input.
struct Crossover3 {
  Split lo, hi;
  void set(double sr, double f1, int sl1, double f2, int sl2) {
    lo.set(sr, f1, sl1);
    hi.set(sr, f2, sl2);
  }
  void process(double x, double& l, double& m, double& h) {
    double rest;
    lo.process(x, l, rest);
    hi.process(rest, m, h);
    l = hi.compensate(l);
  }
  void reset() {
    lo.reset();
    hi.reset();
  }
};

// --------------------------------------------------------------- oversampling
// Half-band FIR stages (windowed sinc, 31 taps) for 2x / 4x processing of
// memoryless nonlinearities.
struct HalfbandKernel {
  static constexpr int N = 31;
  double h[N];
  HalfbandKernel() {
    const double fc = 0.22;  // of the high rate
    double sum = 0;
    for (int i = 0; i < N; ++i) {
      double m = i - (N - 1) / 2.0;
      double sinc = m == 0 ? 2.0 * fc : std::sin(kTwoPi * fc * m) / (kPi * m);
      double w = 0.42 - 0.5 * std::cos(kTwoPi * i / (N - 1)) + 0.08 * std::cos(2 * kTwoPi * i / (N - 1));
      h[i] = sinc * w;
      sum += h[i];
    }
    for (double& v : h) v /= sum;
  }
  static const HalfbandKernel& get() {
    static const HalfbandKernel k;
    return k;
  }
};

// The histories are circular (each sample written twice, at i and i + size,
// so a window is always contiguous): no shifting per sample (J-25). The sums
// run in the same order as a shifted history, so results are identical.
struct Up2 {
  static constexpr int M = (HalfbandKernel::N + 1) / 2;
  double xb[2 * M] = {};
  int p = 0;
  const double* h = HalfbandKernel::get().h;
  void process(double in, double& y0, double& y1) {
    p = p == 0 ? M - 1 : p - 1;
    xb[p] = xb[p + M] = in;
    const double* x = xb + p;  // x[0] newest
    double a = 0, b = 0;
    for (int k = 0; k < M; ++k) {
      a += h[2 * k] * x[k];
      if (2 * k + 1 < HalfbandKernel::N) b += h[2 * k + 1] * x[k];
    }
    // High-rate output at the input's time uses the even taps, the sample
    // after it (between inputs) the odd taps.
    y0 = 2.0 * a;
    y1 = 2.0 * b;
  }
  void reset() {
    std::fill(xb, xb + 2 * M, 0.0);
    p = 0;
  }
};

struct Down2 {
  static constexpr int N = HalfbandKernel::N;
  double xb[2 * N] = {};
  int p = 0;
  const double* h = HalfbandKernel::get().h;
  double process(double a, double b) {
    p = (p + N - 2) % N;
    const int q = p + 1 == N ? 0 : p + 1;
    xb[p] = xb[p + N] = b;  // x[0]
    xb[q] = xb[q + N] = a;  // x[1]
    const double* x = xb + p;
    double y = 0;
    for (int i = 0; i < N; ++i) y += h[i] * x[i];
    return y;
  }
  void reset() {
    std::fill(xb, xb + 2 * N, 0.0);
    p = 0;
  }
};

struct Oversampler {
  Up2 u1, u2a, u2b;
  Down2 d1, d2a, d2b;
  Oversampler() { (void)HalfbandKernel::get(); }  // build the kernel off the audio thread
  template <class F>
  double process(double x, int factor, F&& f) {
    if (factor <= 1) return f(x);
    double a, b;
    u1.process(x, a, b);
    if (factor == 2) return d1.process(f(a), f(b));
    double a0, a1, b0, b1;
    u2a.process(a, a0, a1);
    u2b.process(b, b0, b1);
    double A = d2a.process(f(a0), f(a1));
    double B = d2b.process(f(b0), f(b1));
    return d1.process(A, B);
  }
  void reset() {
    u1.reset(); u2a.reset(); u2b.reset();
    d1.reset(); d2a.reset(); d2b.reset();
  }
};
inline int osFactor(int choice) { return choice <= 0 ? 1 : (choice == 1 ? 2 : 4); }
// Delay (base-rate samples, rounded) of Oversampler::process at a factor:
// 2x = up + down half-band (15 + 15 high-rate samples, 14.5 base); 4x adds
// the inner up/down pair at the 2x rate (measured 29).
inline int osLatency(int factor) { return factor <= 1 ? 0 : (factor == 2 ? 15 : 29); }

// --------------------------------------------------------------- delay line
struct DelayLine {
  std::vector<float> buf;
  size_t mask = 0, w = 0;
  explicit DelayLine(size_t minSize = 8192) {
    size_t n = 1;
    while (n < minSize) n <<= 1;
    buf.assign(n, 0.0f);
    mask = n - 1;
  }
  void push(double x) {
    buf[w & mask] = static_cast<float>(x);
    ++w;
  }
  // Integer delay (1 = the sample pushed last).
  double at(size_t d) const {
    d = std::min(std::max<size_t>(d, 1), mask);
    return buf[(w - d) & mask];
  }
  // Fractional delay in samples (>= 1), linear interpolation.
  double frac(double d) const {
    d = clampd(d, 1.0, static_cast<double>(mask - 2));
    size_t i = static_cast<size_t>(d);
    double f = d - static_cast<double>(i);
    double a = buf[(w - i) & mask], b = buf[(w - i - 1) & mask];
    return a + (b - a) * f;
  }
  size_t capacity() const { return mask; }
  void reset() {
    std::fill(buf.begin(), buf.end(), 0.0f);
    w = 0;
  }
};

// --------------------------------------------------------------- misc
struct DcBlock {
  double x1 = 0, y1 = 0, r = 0.995;
  void set(double sr) { r = std::exp(-kTwoPi * 10.0 / sr); }
  double process(double x) {
    double y = x - x1 + r * y1;
    x1 = x;
    y1 = flushDenormal(y);
    return y;
  }
  void reset() { x1 = y1 = 0; }
};

// Soft saturation: linear below `thr`, then a smooth knee that approaches
// `ceil` (both linear amplitudes); hard clip when ceil <= thr.
inline double softSat(double x, double thr, double ceil) {
  double a = std::fabs(x);
  if (a <= thr) return x;
  double y;
  if (ceil <= thr + 1e-9)
    y = std::min(a, ceil);
  else {
    double span = ceil - thr;
    y = thr + span * std::tanh((a - thr) / span);
  }
  return x < 0 ? -y : y;
}

// Small 8-line feedback-delay-network reverb (mono in, stereo out).
struct FdnReverb {
  static constexpr int K = 8;
  std::vector<DelayLine> lines;
  DelayLine pre;
  double maxLen;
  explicit FdnReverb(size_t lineSize = 16384) : lines(K, DelayLine(lineSize)), pre(lineSize), maxLen(static_cast<double>(lineSize) - 8) {}
  double lpState[K] = {};
  double len[K] = {};
  double fb[K] = {};
  double damp = 0.3;
  double predelay = 0;
  void set(double sr, double size, double decaySec, double damping, double preMs) {
    static const double base[K] = {1031, 1327, 1523, 1801, 2063, 2297, 2539, 2803};
    double scale = sr / 44100.0 * (0.35 + 1.15 * clampd(size, 0, 1));
    for (int i = 0; i < K; ++i) {
      len[i] = std::min(base[i] * scale, maxLen);
      // gain per pass for a -60 dB decay in decaySec
      fb[i] = std::pow(10.0, -3.0 * len[i] / std::max(1.0, decaySec * sr));
    }
    damp = clampd(damping, 0, 0.95);
    predelay = clampd(preMs * 0.001 * sr, 1.0, maxLen);
  }
  void process(double in, double& outL, double& outR) {
    pre.push(in);
    double x = pre.frac(predelay);
    double v[K];
    for (int i = 0; i < K; ++i) {
      double d = lines[i].frac(len[i]);
      lpState[i] = flushDenormal(d + (lpState[i] - d) * damp);
      v[i] = lpState[i];
    }
    // Hadamard mix (fast Walsh–Hadamard, normalised).
    for (int h = 1; h < K; h <<= 1)
      for (int i = 0; i < K; i += h << 1)
        for (int j = i; j < i + h; ++j) {
          double a = v[j], b = v[j + h];
          v[j] = a + b;
          v[j + h] = a - b;
        }
    const double norm = 1.0 / std::sqrt(static_cast<double>(K));
    double l = 0, r = 0;
    for (int i = 0; i < K; ++i) {
      double y = v[i] * norm;
      lines[i].push(x * 0.35 + y * fb[i]);
      if (i & 1)
        r += lpState[i];
      else
        l += lpState[i];
    }
    outL = l * 0.5;
    outR = r * 0.5;
  }
  void reset() {
    for (auto& l : lines) l.reset();
    pre.reset();
    std::fill(lpState, lpState + K, 0.0);
  }
};

// Chain of first-order allpasses with a shared, modulated coefficient.
struct PhaserChain {
  static constexpr int kMax = 12;
  double z[kMax] = {};
  double last = 0;
  double process(double x, double coef, int stages, double feedback) {
    double y = x + last * feedback;
    for (int i = 0; i < stages && i < kMax; ++i) {
      double o = coef * y + z[i];
      z[i] = flushDenormal(y - coef * o);
      y = o;
    }
    last = flushDenormal(y);
    return y;
  }
  void reset() {
    std::fill(z, z + kMax, 0.0);
    last = 0;
  }
};
// First-order allpass coefficient for a break frequency.
inline double allpassCoef(double hz, double sr) {
  double t = std::tan(kPi * clampd(hz, 20.0, sr * 0.45) / sr);
  return (t - 1.0) / (t + 1.0);
}

// Peak envelope follower (attack / release coefficients).
struct Follower {
  double v = 0;
  double process(double x, double atk, double rel) {
    double c = x > v ? atk : rel;
    v = flushDenormal(x + (v - x) * c);
    return v;
  }
};

// Tempo helper: seconds per beat.
inline double beatSeconds(double tempo) { return 60.0 / std::max(10.0, tempo); }

}  // namespace gl::packs::dynamics
