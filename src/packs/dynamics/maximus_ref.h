// Gridline — "dynamics" pack, J-35: reference engine of the mastering
// dynamics (gl.master MsRefModel = 1, gl.enhance EnCustom = 3).
//
// Low cut (four high-pass biquads) → Linkwitz–Riley style low / mid / high
// split (IIR, polarity-corrected 12 dB bands) → per band: stereo separation,
// peak (sliding maximum) or RMS window detector, 1–8 instant-attack release
// stages on the level, a gain computer reading the band's drawn transfer curve
// (tabulated with the reference's tension law), 1–8 gain-smoothing stages
// (attack / second release), look-ahead delay, gain, saturation (atan family
// or exponential knee) → band mix and sum → the master band with the same
// chain. Muting, merged (off) bands, solo and the mid-only master mode follow
// the reference. Settings are the plugin's stored integers (RefState); the
// tables are rebuilt only when their inputs change. Buffers are sized in the
// constructor; process never allocates.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <complex>
#include <cstring>
#include <vector>
#include <atomic>
#include "../../engine/MEnv.h"

namespace gl::packs::dynamics::maxref {

inline constexpr int kBands = 4;
inline constexpr int kPts = 64;  // K-S: a drawn curve holds what the reference's state can hold
inline constexpr int kRaw = 59;  // 7 globals + 4 × 13 band ints
enum BandTag { TMode = 0, TPre, TPost, TSep, TSatThr, TSatCeil, TAtt, TRel2, TAttCurve, TRel, TRelCurve, TWindow, TSusMode, TCount };
inline int rawIndex(int band, int tag) { return 7 + band * TCount + tag; }

// ---------------------------------------------------------------- laws
inline double timeMs(double v) { return 2.0 * (std::exp(std::log(501.0) * v * 1e-4) - 1.0); }
inline double gain7(double v) { return v == 1000 ? 1.0 : (std::pow(7.0, v / 1000.0) - 1.0) / 6.0; }
inline double splitHz(double v) { return 30.0 * std::pow(2.0, v * 0.000138227748); }
inline double lowCutHz(double v) { return 5.0 * std::pow(2.0, v / 16384.0); }

struct Curve {
  int n = 3;
  double x[kPts] = {0, 0.5, 1}, y[kPts] = {0, 0.5, 0.5}, t[kPts] = {0, 0, 0};
  int type[kPts] = {0, 0, 0};
  // K-S: tangents of "smooth" segments (ends at i), monotone cubic as the envelope model; see prepareTangents().
  float tanS[kPts] = {}, tanE[kPts] = {};
  bool hasSmooth = false;
  void prepareTangents() {
    hasSmooth = false;
    for (int i = 1; i < n; ++i) {
      tanS[i] = tanE[i] = 0;
      if (type[i] != 10) continue;
      hasSmooth = true;
      const double h = x[i] - x[i - 1], dy = static_cast<double>(static_cast<float>(y[i])) - static_cast<float>(y[i - 1]);
      if (h <= 0 || std::fabs(dy) < 6e-8) continue;
      const double d = dy / h;
      auto tangent = [&](int j0, int j1, int j2) {
        const double h0 = x[j1] - x[j0], h1 = x[j2] - x[j1];
        if (h0 <= 0 || h1 <= 0) return 0.0;
        const double d0 = (static_cast<double>(static_cast<float>(y[j1])) - static_cast<float>(y[j0])) / h0;
        const double d1 = (static_cast<double>(static_cast<float>(y[j2])) - static_cast<float>(y[j1])) / h1;
        if (d0 * d1 <= 0) return 0.0;
        return 3 * d0 * d1 * (h0 + h1) / ((2 * h0 + h1) * d0 + (h0 + 2 * h1) * d1);  // measured on the oracle (4 segments, within 1e-3)
      };
      const double m0 = (i >= 2 && type[i - 1] == 10) ? tangent(i - 2, i - 1, i) : 0.0;
      const double m1 = (i + 1 < n && type[i + 1] == 10) ? tangent(i - 1, i, i + 1) : 0.0;
      tanS[i] = static_cast<float>(m0 / d), tanE[i] = static_cast<float>(m1 / d);
    }
  }
  bool operator==(const Curve& o) const {
    if (n != o.n) return false;
    for (int i = 0; i < n; ++i)
      if (x[i] != o.x[i] || y[i] != o.y[i] || t[i] != o.t[i] || type[i] != o.type[i]) return false;
    return true;
  }
};

// Reference envelope segment shape: phase t (0..1), tension τ, point type.
inline double segShape(double t, double tau, int type, int rev = 1, double tanS = 0, double tanE = 0) {
  if (type == 2) return 0.0;  // hold
  if (type == 12 && rev >= 1) {  // K-S: two mirrored rational bends, measured on the oracle (menv's coefficient halves them twice)
    const auto c = menv::coef(menv::SingleCurve3, static_cast<float>(tau));
    return t < 0.5 ? 0.5 * menv::shape(menv::SingleCurve3, c, 2 * t) : 1.0 - 0.5 * menv::shape(menv::SingleCurve3, c, 2 * (1 - t));
  }
  if (type >= 3 && type <= 12) {  // J-42: the other point shapes of the envelope model
    const auto m = static_cast<uint8_t>(type);
    if (m == menv::Smooth && rev >= 1) return menv::shape(m, menv::coef(m, static_cast<float>(tau)), t, tanS, tanE);
    return menv::shape(m, menv::coef(m, static_cast<float>(tau)), t);
  }
  auto f = [tau](double u) {
    if (tau == 0) return u;
    const double k = (tau > 0 ? 1.0 : -1.0) * (std::pow(10.0, 6.0 * std::fabs(tau)) - 1.0);
    const double A = 1.0 / std::fabs(k), K = std::log(std::fabs(k) + 1.0);
    return tau > 0 ? 1.0 - A * (std::exp(K * (1.0 - u)) - 1.0) : A * (std::exp(K * u) - 1.0);
  };
  if (type == 1) return t < 0.5 ? 0.5 * f(2 * t) : 1.0 - 0.5 * f(2 * (1 - t));
  return f(t);
}
inline double curveAt(const Curve& c, double X, int rev = 1) {
  if (c.n <= 0) return 0;
  if (X <= c.x[0]) return static_cast<float>(c.y[0]);
  for (int i = 1; i < c.n; ++i) {
    if (X < c.x[i]) {
      const double dx = c.x[i] - c.x[i - 1];
      const double y0 = static_cast<float>(c.y[i - 1]), y1 = static_cast<float>(c.y[i]);
      if (dx <= 0) return y1;
      const double t = (X - c.x[i - 1]) / dx;
      return y0 + segShape(t, c.t[i], c.type[i], rev, c.tanS[i], c.tanE[i]) * (y1 - y0);
    }
  }
  return static_cast<float>(c.y[c.n - 1]);
}

// Everything the engine reads (the plugin's stored values).
struct RefState {
  int raw[kRaw] = {};
  bool solo = false, masterMid = false, safetyOff = false;
  int target = 0;
  Curve curve[kBands];
  // J-42: linear-phase crossovers — legacy FIR (byte 0x335) and the FFT filter bank (flags bit 0, wins).
  bool fir = false, lin = false;
  int linQuality = 2, linSpeed = 0;
  int os = 0;  // K-S: oversampling stages of the low / mid / high section (2^os; the master band stays at the base rate)
  // K-S: 0 = the laws before the K-8 oracle round (states saved with them keep their sound), 1 = current.
  int rev = 1;
  // K-46: controls moved while running follow the reference (see the "moving" notes below). Static sound is the same
  // either way; states saved before this keep the moves they had (masterHasMoveLaw, dynamics.h).
  bool moveLaw = false;
};

// ---------------------------------------------------------------- J-42 linear-phase splits
// Legacy FIR: N = round(4 ms) | 1 taps (≥ 5) per band; split kernels of length
// round((slope + 1)·os·27500 / f) | 1 (5 … N), windowed sinc (4-term Blackman–Harris),
// centred; mid = δ − (LP_low + HP_high), so the three bands sum to a pure delay.
inline int firTaps(double F) { return std::max(5, static_cast<int>(std::nearbyint(0.004 * F)) | 1); }
inline int firLen(int slope, double f, int N, int osFactor = 1) {
  int L = static_cast<int>(std::nearbyint(static_cast<float>((slope + 1) * osFactor * 27500.0 / f))) | 1;
  if (L < 6) L = 5;
  return std::min(L, N);
}
// Adds a ×sign windowed-sinc low pass (hp: δ − sinc) of length L centred in an N buffer.
inline void firAdd(std::vector<double>& buf, int N, double f, double F, int L, bool hp) {
  const double c = std::min(f / F, 0.5), pi = 3.141592653589793;
  const int m = (L - 1) / 2, off = (N - L) / 2;
  for (int i = 0; i < L; ++i) {
    const int t = i - m;
    const double lp = t == 0 ? 2 * c : std::sin(2 * pi * c * t) / (pi * t);
    const double a = 2 * pi * i / (L - 1);
    const double w = 0.35875 - 0.48829 * std::cos(a) + 0.14128 * std::cos(2 * a) - 0.01168 * std::cos(3 * a);
    buf[static_cast<size_t>(off + i)] += (hp ? (t == 0 ? 1.0 : 0.0) - lp : lp) * w;
  }
}
struct FirBand {
  int N = 0, pos = 0;
  std::vector<float> taps, line[2];  // line: double-written ring of N
  void init(int n) {
    N = n, pos = 0;
    taps.assign(static_cast<size_t>(N), 0.f);
    for (auto& l : line) l.assign(static_cast<size_t>(2 * N), 0.f);
  }
  void clear() {
    for (auto& l : line) std::fill(l.begin(), l.end(), 0.f);
    pos = 0;
  }
  void push(const float* x) {
    for (int c = 0; c < 2; ++c) line[c][static_cast<size_t>(pos)] = line[c][static_cast<size_t>(pos + N)] = x[c];
    pos = pos + 1 == N ? 0 : pos + 1;
  }
  float out(int c) const {  // y = Σ taps[k]·x[n − k]; oldest sample first at line[pos]
    const float* h = line[c].data() + pos;
    double acc = 0;
    for (int k = 0; k < N; ++k) acc += static_cast<double>(taps[static_cast<size_t>(N - 1 - k)]) * h[k];
    return static_cast<float>(acc);
  }
};

// LIN: FFT filter bank (one object per band). N = (512 << quality) scaled to the rate
// (halved below 44.1 kHz, ≤ 65536); zero-phase masks from fast log / pow approximations
// with −6 dB at the split and S dB per "6 dB octave" (LP + HP = 1); the low cut folded into
// every mask; kernels of N/2 taps (sine window) run by overlap-add (hop N/2, latency 3N/4);
// mask changes cross-fade between two engines (0.1 / 0.25 / 0.01 / 0.02 s), starting from
// silence after a load.
inline int linSize(double F, int quality) {
  const int base = 512 << std::clamp(quality, 0, 5);
  if (F < 44100) return base / 2;
  const int sh = static_cast<int>(std::log2(F / 44100.0) + 0.5);
  return std::min(base << sh, 65536);
}
inline float log2a(float r) {
  uint32_t b;
  std::memcpy(&b, &r, 4);
  const int e = static_cast<int>((b >> 23) & 0xFF) - 127;
  const float m = static_cast<float>(b & 0x7FFFFF) / 8388608.0f;
  const float u = 1.0f - m;
  return static_cast<float>(e + 1) - 0.739f * u - 0.1063157f * u * u - 0.1556501f * u * u * u;
}
inline float pow2a(float x) {
  if (x < 0) return 1.0f / pow2a(-x);
  const int i = static_cast<int>(x);
  const float f = x - static_cast<float>(i);
  return std::ldexp(0.999862219597f + 0.69659850257f * f + 0.224328091473f * f * f + 0.079064658211f * f * f * f, i);
}
inline float maskA(float f, float fc, float S, bool hp) {
  float x = log2a(f / fc);
  if (hp) x = -x;
  const float d = -6.0f - S * std::fabs(x);
  const float a = d < -100.0f ? 0.0f : pow2a(d / 6.0f);
  return x < 0 ? 1.0f - a : a;
}
struct FftD {  // radix-2 complex FFT (double)
  int n = 0;
  std::vector<std::complex<double>> w;
  std::vector<int> rev;
  void init(int size) {
    if (size == n) return;
    n = size;
    w.resize(static_cast<size_t>(n / 2));
    for (int k = 0; k < n / 2; ++k) w[static_cast<size_t>(k)] = std::polar(1.0, -2 * 3.141592653589793 * k / n);
    rev.assign(static_cast<size_t>(n), 0);
    int bits = 0;
    while ((1 << bits) < n) ++bits;
    for (int i = 0; i < n; ++i) {
      int r = 0;
      for (int b = 0; b < bits; ++b)
        if (i & (1 << b)) r |= 1 << (bits - 1 - b);
      rev[static_cast<size_t>(i)] = r;
    }
  }
  void run(std::complex<double>* a, bool inv) const {
    for (int i = 0; i < n; ++i)
      if (i < rev[static_cast<size_t>(i)]) std::swap(a[i], a[rev[static_cast<size_t>(i)]]);
    for (int len = 2; len <= n; len <<= 1) {
      const int step = n / len;
      for (int i = 0; i < n; i += len)
        for (int j = 0; j < len / 2; ++j) {
          std::complex<double> tw = w[static_cast<size_t>(j * step)];
          if (inv) tw = std::conj(tw);
          const std::complex<double> u = a[i + j], v = a[i + j + len / 2] * tw;
          a[i + j] = u + v, a[i + j + len / 2] = u - v;
        }
    }
  }
};
struct LinEngine {
  int N = 0, pos = 0;
  std::vector<std::complex<double>> H, X;
  std::vector<float> blk[2];
  std::vector<double> acc[2];
  void init(int n) {
    N = n;
    H.assign(static_cast<size_t>(N), {0, 0});
    X.assign(static_cast<size_t>(N), {0, 0});
    for (int c = 0; c < 2; ++c) blk[c].assign(static_cast<size_t>(N / 2), 0.f), acc[c].assign(static_cast<size_t>(N), 0.0);
    pos = 0;
  }
  void clear() {
    for (int c = 0; c < 2; ++c) std::fill(blk[c].begin(), blk[c].end(), 0.f), std::fill(acc[c].begin(), acc[c].end(), 0.0);
    pos = 0;
  }
  void design(const std::vector<float>& M, const FftD& f) {  // M: N/2 + 1 magnitudes
    for (int k = 0; k < N; ++k) {
      const int kk = k <= N / 2 ? k : N - k;
      X[static_cast<size_t>(k)] = {static_cast<double>(M[static_cast<size_t>(kk)]) * ((k & 1) ? -1.0 : 1.0), 0.0};
    }
    f.run(X.data(), true);
    const int B = N / 2;
    std::fill(H.begin(), H.end(), std::complex<double>(0, 0));
    for (int i = 0; i < B; ++i)
      H[static_cast<size_t>(i)] = X[static_cast<size_t>(N / 4 + i)].real() / N * std::sin(3.141592653589793 * i / (B - 1));
    f.run(H.data(), false);
  }
  void run(const float* in, float* out, const FftD& f) {
    for (int c = 0; c < 2; ++c) {
      out[c] = static_cast<float>(acc[c][static_cast<size_t>(pos)]);
      blk[c][static_cast<size_t>(pos)] = in[c];
    }
    if (++pos < N / 2) return;
    pos = 0;
    const int B = N / 2;
    for (int i = 0; i < N; ++i) X[static_cast<size_t>(i)] = i < B ? std::complex<double>(blk[0][static_cast<size_t>(i)], blk[1][static_cast<size_t>(i)]) : 0.0;
    f.run(X.data(), false);
    for (int i = 0; i < N; ++i) X[static_cast<size_t>(i)] *= H[static_cast<size_t>(i)];
    f.run(X.data(), true);
    for (int c = 0; c < 2; ++c) {
      std::copy(acc[c].begin() + B, acc[c].end(), acc[c].begin());
      std::fill(acc[c].begin() + B, acc[c].end(), 0.0);
      for (int i = 0; i < N; ++i) acc[c][static_cast<size_t>(i)] += (c == 0 ? X[static_cast<size_t>(i)].real() : X[static_cast<size_t>(i)].imag()) / N;
    }
  }
};
struct LinBand {
  LinEngine e[2];
  int active = 0, N = 0, hpos = 0, fadeN = 0, fadePos = 0;
  bool fading = false, pending = false;
  std::vector<float> mask, hist[2];
  void init(int n) {
    N = n;
    for (auto& x : e) x.init(n);
    for (auto& h : hist) h.assign(static_cast<size_t>(N), 0.f);
    mask.assign(static_cast<size_t>(N / 2 + 1), 0.f);
    active = 0, hpos = 0, fading = false, pending = false;
  }
  void clear() {
    for (auto& x : e) x.clear();
    for (auto& h : hist) std::fill(h.begin(), h.end(), 0.f);
    hpos = 0, fading = false;
  }
  void run(const float* in, float* out, const FftD& f, int xfade) {
    if (pending && !fading) {
      LinEngine& nw = e[1 - active];
      nw.clear();
      nw.design(mask, f);
      float tmp[2], frame[2];
      for (int k = 0; k < N; ++k) {
        const size_t q = static_cast<size_t>((hpos + k) % N);
        frame[0] = hist[0][q], frame[1] = hist[1][q];
        nw.run(frame, tmp, f);
      }
      pending = false;
      fadeN = xfade, fadePos = 0;
      fading = fadeN > 0;
      if (!fading) active = 1 - active;
    }
    hist[0][static_cast<size_t>(hpos)] = in[0], hist[1][static_cast<size_t>(hpos)] = in[1];
    hpos = hpos + 1 == N ? 0 : hpos + 1;
    if (fading) {
      float a[2], b[2];
      e[active].run(in, a, f);
      e[1 - active].run(in, b, f);
      const float t = static_cast<float>(fadePos) / static_cast<float>(fadeN);
      for (int c = 0; c < 2; ++c) out[c] = t * b[c] + (1 - t) * a[c];
      if (++fadePos >= fadeN) active = 1 - active, fading = false;
    } else {
      e[active].run(in, out, f);
    }
  }
};

// Effective band modes (solo, mid-only master) and the reported latency.
inline void effectiveModes(const RefState& s, int* mode) {
  for (int k = 0; k < kBands; ++k) mode[k] = std::clamp(s.raw[rawIndex(k, TMode)], 0, 3);
  if (s.solo && s.target != 3) {
    for (int k = 0; k < 3; ++k)
      if (k != s.target) mode[k] = std::max(mode[k], 2);
    mode[3] = std::max(mode[3], 1);
  }
  if (s.masterMid) mode[0] = std::max(mode[0], 2), mode[2] = std::max(mode[2], 2);
}
inline int lmhDelay(const RefState& s, const int* mode, double fs, int k) {
  bool anyComp = false;
  for (int b = 0; b < 3; ++b) anyComp = anyComp || mode[b] == 0;
  if (k < 3) return (mode[k] < 2 && anyComp) ? static_cast<int>(std::lround(fs * timeMs(s.raw[4]) / 1000.0)) : 0;
  return mode[3] < 1 ? static_cast<int>(std::lround(fs * timeMs(s.raw[rawIndex(3, TAtt)]) / 1000.0)) : 0;
}
// Delay of the oversampling filters at the base rate (their group delay at low
// frequencies, rounded): reported on top, so delay compensation stays exact.
inline int osDelay(int stages) {
  static const int d[4] = {0, 3, 5, 6};
  return d[std::clamp(stages, 0, 3)];
}
inline int latencyOf(const RefState& s, double fs) {
  int mode[kBands];
  effectiveModes(s, mode);
  const int f = 1 << std::clamp(s.os, 0, 3);
  const int split = s.lin ? 3 * linSize(fs * f, s.linQuality) / 4 / f : s.fir ? (firTaps(fs * f) - 1) / 2 / f : 0;
  return split + std::max({lmhDelay(s, mode, fs, 0), lmhDelay(s, mode, fs, 1), lmhDelay(s, mode, fs, 2)}) + lmhDelay(s, mode, fs, 3) + osDelay(s.os);
}

// ---------------------------------------------------------------- K-S oversampling
// One 2x stage: polyphase half-band of two branches of three cascaded first-order
// all-passes, y = x[-1] + a (x - y[-1]), state in double, float between the stages.
struct HalfBand {
  static constexpr double kSlow[3] = {0.0391515977345, 0.302646848328, 0.674615918547};
  static constexpr double kFast[3] = {0.147377113601, 0.48246854277, 0.883005025769};
  struct Ap {
    double x1[2] = {0, 0}, y1[2] = {0, 0};
    double run(int c, double x, double a) {
      const double y = x1[c] + a * (x - y1[c]);
      x1[c] = x, y1[c] = y;
      return y;
    }
  };
  Ap slow[3], fast[3];
  void clear() {
    for (auto& a : slow) a = Ap{};
    for (auto& a : fast) a = Ap{};
  }
  double runSlow(int c, double x) {
    for (int i = 0; i < 3; ++i) x = slow[i].run(c, x, kSlow[i]);
    return x;
  }
  double runFast(int c, double x) {
    for (int i = 0; i < 3; ++i) x = fast[i].run(c, x, kFast[i]);
    return x;
  }
  void up(const float* in, float* a, float* b) {  // one stereo frame -> two
    for (int c = 0; c < 2; ++c) {
      a[c] = static_cast<float>(runSlow(c, in[c]));
      b[c] = static_cast<float>(runFast(c, in[c]));
    }
  }
  void down(const float* a, const float* b, float* out) {  // two stereo frames -> one
    for (int c = 0; c < 2; ++c) out[c] = static_cast<float>((runFast(c, a[c]) + runSlow(c, b[c])) * 0.5);
  }
};

// ---------------------------------------------------------------- filters
struct Biquad {  // direct form I, double coefficients and state
  double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  double x1[2] = {0, 0}, x2[2] = {0, 0}, y1[2] = {0, 0}, y2[2] = {0, 0};
  void lp(double w, double alpha) {
    const double cs = std::cos(w), a0 = 1 + alpha;
    b1 = (1 - cs) / a0, b0 = b2 = b1 / 2, a1 = -2 * cs / a0, a2 = (1 - alpha) / a0;
  }
  void hp(double w, double alpha) {
    const double cs = std::cos(w), a0 = 1 + alpha;
    b1 = -(1 + cs) / a0, b0 = b2 = (1 + cs) / (2 * a0), a1 = -2 * cs / a0, a2 = (1 - alpha) / a0;
  }
  void ap(double w, double alpha) {
    const double cs = std::cos(w), a0 = 1 + alpha;
    a1 = -2 * cs / a0, a2 = (1 - alpha) / a0, b0 = a2, b1 = a1, b2 = 1;
  }
  float run(int c, float xf) {
    const double x = xf;
    const double y = b0 * x + b1 * x1[c] + b2 * x2[c] - a1 * y1[c] - a2 * y2[c];
    x2[c] = x1[c], x1[c] = x, y2[c] = y1[c], y1[c] = y;
    return static_cast<float>(y);
  }
  void clear() {
    for (int c = 0; c < 2; ++c) x1[c] = x2[c] = y1[c] = y2[c] = 0;
  }
};
struct Chain {
  Biquad s[2];
  int n = 0;
  float run(int c, float x) {
    for (int i = 0; i < n; ++i) x = s[i].run(c, x);
    return x;
  }
  void clear() {
    for (auto& b : s) b.clear();
  }
};

// ---------------------------------------------------------------- engine
class Engine {
 public:
  Engine() = default;
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;
  ~Engine() {
    delete pending_.exchange(nullptr);
    freeRetired();
  }
  // K-S: the audio thread never allocates once the host has prepared the engine.
  // prepare() (UI thread, every Program build; the engine may be running) builds what
  // this state needs into a Pool and publishes it; configure() swaps the storage in
  // without allocating and hands the old storage back to be freed on the UI thread.
  void prepare(const RefState& s, double fs) {
    freeRetired();
    if (Pool* un = pending_.exchange(nullptr, std::memory_order_acq_rel)) {  // not taken yet: its needs count again
      uiFs_ = un->prevFs, uiStages_ = un->prevStages, uiLinN_ = un->prevLinN;
      delete un;
    }
    freeRetired();
    const int st = std::clamp(s.os, 0, 3);
    const bool needRing = fs != uiFs_ || st > uiStages_;
    const int stages = needRing ? std::max(st, fs == uiFs_ ? uiStages_ : 1) : uiStages_;
    const int linN = s.lin ? linSize(fs * (1 << st), s.linQuality) : 0;
    const bool needLin = linN > uiLinN_;
    if (needRing || needLin) {
      auto* p = new Pool;
      p->prevFs = uiFs_, p->prevStages = uiStages_, p->prevLinN = uiLinN_;
      if (needRing) fillRing(*p, fs, stages), uiFs_ = fs, uiStages_ = stages;
      if (needLin) fillLin(*p, linN), uiLinN_ = linN;
      pending_.store(p, std::memory_order_release);
    }
    hostPrepared_.store(true, std::memory_order_release);
  }
  // A host runs this engine but has nothing to prepare yet: never size buffers on its audio thread.
  void hostOnly() { hostPrepared_.store(true, std::memory_order_release); }
  int latency() const { return lat_; }
  // Gain reduction of a band (dB, ≤ 0) for meters.
  float bandGr(int k) const { return band_[k].grDb; }
  // K-S: monitor values of a band since the last call; zeroes them.
  void takeScope(int k, float& in, float& out, float& env) {
    Band& B = band_[k];
    in = B.scopeIn, out = B.scopeOut, env = B.scopeEnv;
    B.scopeIn = B.scopeOut = B.scopeEnv = 0;
  }

  // Rebuilds what changed; call before process with the current state.
  void configure(const RefState& s, double fs) {
    if (Pool* p = pending_.exchange(nullptr, std::memory_order_acq_rel)) {  // no allocation, nothing freed here
      adopt(*p);
      retire(p);
    }
    const bool hosted = hostPrepared_.load(std::memory_order_acquire);
    if (!hosted && (fs != fs_ || band_[0].table.empty() || std::clamp(s.os, 0, 3) > capStages_ ||
                    (s.lin && linSize(fs * (1 << std::clamp(s.os, 0, 3)), s.linQuality) > linCap_))) {
      // Offline use without a host (no Program build): sized here, on the caller's thread.
      Pool p;
      const int st = std::clamp(s.os, 0, 3);
      const bool ring = fs != fs_ || band_[0].table.empty() || st > capStages_;
      if (ring) fillRing(p, fs, std::max(st, 1));
      if (s.lin) fillLin(p, std::max(linCap_, linSize(fs * (1 << st), s.linQuality)));
      adopt(p);
    }
    if (band_[0].table.empty()) return;  // a host's buffers not there yet
    if (cfgValid_ && std::memcmp(s.raw, st_.raw, sizeof s.raw) == 0 && s.solo == st_.solo && s.masterMid == st_.masterMid &&
        s.safetyOff == st_.safetyOff && s.target == st_.target && s.curve[0] == st_.curve[0] && s.curve[1] == st_.curve[1] &&
        s.curve[2] == st_.curve[2] && s.curve[3] == st_.curve[3] && s.fir == st_.fir && s.lin == st_.lin && s.linQuality == st_.linQuality &&
        s.linSpeed == st_.linSpeed && s.os == st_.os && s.rev == st_.rev && s.moveLaw == st_.moveLaw)
      return;
    const RefState old = st_;
    const bool first = !cfgValid_;
    st_ = s;
    cfgValid_ = true;
    const int* r = s.raw;
    // Effective band modes.
    effectiveModes(s, mode_);
    // Automation can ask for more than was prepared only without a Program build: it waits for the next one.
    os_ = std::min(std::clamp(s.os, 0, 3), capStages_);
    const int osf = 1 << os_;
    const double fo = fs_ * osf;  // rate of the low / mid / high section
    if (first || os_ != old.os) {
      for (auto& h : hbUp_) h.clear();
      for (auto& h : hbDown_) h.clear();
    }
    // Crossover.
    int slL = r[1] > 0 ? 1 : 0, slH = r[3] > 0 ? 1 : 0;
    pol_[0] = slL == 0 ? -1.f : 1.f, pol_[2] = slH == 0 ? -1.f : 1.f, pol_[1] = 1.f;
    const bool useLin = s.lin && linCap_ > 0;
    if (s.fir || useLin) pol_[0] = pol_[2] = 1.f;
    const int sL0 = slL, sH0 = slH;
    if (r[rawIndex(0, TMode)] == 3) slL = -1;
    if (r[rawIndex(2, TMode)] == 3) slH = -1;
    if (s.moveLaw && !first && (slL != chainL_ || slH != chainH_)) {  // moving: another filter layout starts from clean filters
      lowLp_.clear(), midHp_.clear(), midLp_.clear(), highHp_.clear(), lowAp_.clear();
    }
    chainL_ = slL, chainH_ = slH;
    double fL = splitHz(r[0]), fH = splitHz(r[2]);
    const double fmax = 0.49 * fo;
    fL = std::min(fL, fmax), fH = std::min(fH, fmax);
    if (slL >= 0 && slH >= 0 && fH < fL) std::swap(fL, fH);
    auto design = [&](Chain& ch, int slope, double f, bool hp) {
      ch.n = slope + 1;
      const double w = 2 * 3.141592653589793 * f / fo, al = slope == 0 ? std::sin(w) : std::sin(w) / std::sqrt(2.0);
      for (auto& b : ch.s) hp ? b.hp(w, al) : b.lp(w, al);
    };
    design(lowLp_, slL, fL, false);
    design(midHp_, slL, fL, true);
    design(midLp_, slH, fH, false);
    design(highHp_, slH, fH, true);
    apOn_ = slH == 1 && slL >= 0;
    {
      const double w = 2 * 3.141592653589793 * fH / fo;
      lowAp_.ap(w, s.rev >= 1 ? std::sin(w) : std::sin(w) / std::sqrt(2.0));  // K-S: Q 0.5 (measured on the oracle)
    }
    // J-42: linear-phase splits.
    splitMode_ = useLin ? 2 : s.fir ? 1 : 0;
    if (splitMode_ == 1) {
      const int N = firTaps(fo);
      std::vector<double>& lo = firTmp_[0];  // K-S: within the capacity prepared (no allocation)
      std::vector<double>& mi = firTmp_[1];
      std::vector<double>& hi = firTmp_[2];
      for (auto& v : firTmp_) v.assign(static_cast<size_t>(N), 0.0);
      if (slL >= 0) firAdd(lo, N, fL, fo, firLen(slL, fL, N, osf), false);
      if (slH >= 0) firAdd(hi, N, fH, fo, firLen(slH, fH, N, osf), true);
      if (slL >= 0 && slH >= 0) {
        for (int i = 0; i < N; ++i) mi[static_cast<size_t>(i)] = -(lo[static_cast<size_t>(i)] + hi[static_cast<size_t>(i)]);
        mi[static_cast<size_t>((N - 1) / 2)] += 1.0;
      } else if (slH >= 0) {
        firAdd(mi, N, fH, fo, firLen(slH, fH, N, osf), false);
      } else if (slL >= 0) {
        firAdd(mi, N, fL, fo, firLen(slL, fL, N, osf), true);
      }
      midCopy_ = slL < 0 && slH < 0;
      std::vector<double>* k3[3] = {&lo, &mi, &hi};
      for (int b = 0; b < 3; ++b) {
        if (fir_[b].N != N) fir_[b].init(N);
        for (int i = 0; i < N; ++i) fir_[b].taps[static_cast<size_t>(i)] = static_cast<float>((*k3[b])[static_cast<size_t>(i)]);
      }
    }
    if (splitMode_ == 2) {
      const int N = std::min(linSize(fo, s.linQuality), linCap_);
      static const float kSpeed[4] = {0.1f, 0.25f, 0.01f, 0.02f};
      xfade_ = static_cast<int>(static_cast<float>(static_cast<int>(fo)) * kSpeed[std::clamp(s.linSpeed, 0, 3)]);
      if (lin_[0].N != N) {
        fft_.init(N);
        for (auto& b : lin_) b.init(N);
      }
      // Masks (no frequency swap; split frequencies from the stored values).
      const float FL = static_cast<float>(splitHz(r[0])), FH = static_cast<float>(splitHz(r[2]));
      const float SL = sL0 > 0 ? 24.f : 12.f, SH = sH0 > 0 ? 24.f : 12.f;
      const bool lowOff = r[rawIndex(0, TMode)] == 3, highOff = r[rawIndex(2, TMode)] == 3;
      float lcFc = 0;
      const bool lc = r[6] > 0;
      if (lc) {
        const double fc = lowCutHz(r[6]);
        lcFc = static_cast<float>(fc - std::nearbyint(std::log(std::max(fc - 5.0, 1.0)) / std::log(1.3)));
      }
      bool lcOne = false;
      for (int i = 0; i <= N / 2; ++i) {
        const float f = std::max(static_cast<float>(i) * static_cast<float>(fo / N), 0.1f);
        float m3[3];
        if (f > 0.4999f * static_cast<float>(fo)) {
          m3[0] = m3[1] = m3[2] = 1.f;
        } else {
          m3[0] = maskA(f, FL, SL, false);
          if (lowOff && highOff) m3[1] = pow2a(1.0f / 6.0f);
          else if (highOff) m3[1] = maskA(f, FL, SL, true);
          else if (lowOff) m3[1] = maskA(f, FH, SH, false);
          else m3[1] = maskA(f, FL, SL, true) * maskA(f, FH, SH, false);
          m3[2] = maskA(f, FH, SH, true);
          if (lc && !lcOne) {
            const float g = maskA(f, lcFc, 24.f, true);
            if (g == 1.0f) lcOne = true;
            else
              for (float& v : m3) v *= g;
          }
        }
        for (int b = 0; b < 3; ++b) lin_[b].mask[static_cast<size_t>(i)] = m3[b];
      }
      for (int b = 0; b < 3; ++b) lin_[b].mask[static_cast<size_t>(N / 2)] = lin_[b].mask[static_cast<size_t>(N / 2 - 1)];
      for (auto& b : lin_) b.pending = true;
    }
    // Low cut.
    lcOn_ = r[6] >= 0 && splitMode_ != 2;
    if (lcOn_) {
      static const double kk[4] = {0.19771184, 0.61932772, 0.67684454, 1.07935691};
      const double w = 2 * 3.141592653589793 * 0.8 * lowCutHz(r[6]) / fs_;
      for (int i = 0; i < 4; ++i) lc_[i].hp(w, kk[i] * std::sin(w));
    }
    mix_ = r[5] * 0.001f;
    mixActive_ = mix_ != 1.f;
    // Look-ahead delays.
    for (int k = 0; k < kBands; ++k) {
      Band& B = band_[k];
      const int D = std::min(lmhDelay(s, mode_, fs_, k) * (k < 3 ? osf : 1), B.mask - 1);
      const bool moved = D != B.D;
      B.D = D;
      if (moved && s.moveLaw && !first) clearDelay(B);  // moving: a new length starts from an empty line
    }
    lat_ = latencyOf(s, fs_);
    // Bands.
    for (int k = 0; k < kBands; ++k) {
      Band& B = band_[k];
      auto R = [&](int tag) { return r[rawIndex(k, tag)]; };
      const double fb = k < 3 ? fo : fs_;  // the band's own rate
      B.pre = gain7(R(TPre)), B.post = gain7(R(TPost));
      const double sv = std::clamp(R(TSep), -1000, 1000) / 1000.0;
      const double sq = sv > 0 ? (std::pow(5.0, sv) - 1) / 4 : sv;
      B.hTarget = static_cast<float>(0.5 * std::min(sq, 1.0));
      if (first || s.rev < 1) B.h = B.hTarget;  // later changes glide (K-S, see processBand)
      B.sepTarget[0] = B.sepTarget[1] = 1.f - B.hTarget, B.sepTarget[2] = B.sepTarget[3] = B.hTarget;
      const bool wasPeak = B.peakMode;
      const int wasN = B.N;
      B.peakMode = R(TSusMode) == 0;
      B.N = std::clamp(std::max(B.D, static_cast<int>(std::lround(fb * timeMs(R(TWindow)) / 1000.0))), 1, B.mask - 1);
      if (s.moveLaw && !first && (B.N != wasN || B.peakMode != wasPeak)) clearDetector(B);  // moving: a new window starts empty
      {  // K-S: added smoothing stages join at the last stage's value (no step in the level)
        const int stages = R(TRel) == 0 ? 0 : std::clamp(R(TRelCurve), 1, 8);
        if (!first && s.moveLaw) {  // moving: another stage count restarts the level from nothing
          if (stages != B.relStages)
            for (int i = 0; i < stages; ++i) B.rel[i] = 0;
        } else if (!first && s.rev >= 1 && stages > B.relStages && B.relStages > 0) {
          for (int i = B.relStages; i < stages; ++i) B.rel[i] = B.rel[B.relStages - 1];
        }
        B.relStages = stages;
      }
      {
        double Nr = (2 * timeMs(R(TRel)) * fb / 1000.0) * (s.rev >= 1 ? 1 : 2) / std::sqrt(std::max(1, B.relStages));  // K-S: measured on the oracle
        Nr = std::max(1.0, Nr);
        B.cRel = std::exp(std::log(0.01) / Nr);
      }
      {
        const int stages = std::clamp(R(TAttCurve), 1, 8);
        if (!first && s.moveLaw) {  // moving: another stage count restarts the gain from unity
          if (stages != B.gainStages)
            for (int i = 0; i < stages; ++i) B.gs[i] = 1.0;
        } else if (!first && s.rev >= 1 && stages > B.gainStages) {
          for (int i = B.gainStages; i < stages; ++i) B.gs[i] = B.gs[B.gainStages - 1];
        }
        B.gainStages = stages;
      }
      {
        const double sA = std::sqrt(static_cast<double>(B.gainStages));
        double Na = R(TAtt) == 0 ? B.D : fb * timeMs(R(TAtt)) / 1000.0;
        const double nAtt = Na * 0.8 / sA;
        const double nRel2 = R(TRel2) == 0 ? nAtt : fb * 2 * timeMs(R(TRel2)) / 1000.0 / sA;
        auto co = [](double n) { return n <= 0 ? 0.0 : std::exp(std::log(0.01) / std::max(1.0, n)); };
        B.cAtt = co(nAtt), B.cRel2 = co(nRel2);
      }
      const bool curveChanged = first || !(s.curve[k] == old.curve[k]) || s.safetyOff != old.safetyOff || s.rev != old.rev;
      if (curveChanged) buildTable(B, s.curve[k], s.safetyOff ? 1.0 : 0.975, s.rev);
      const int satV = std::clamp(R(TSatThr), -1000, 1000);
      B.satMode = satV == 0 ? 0 : satV < 0 ? 1 : 2;
      B.ceil = gain7(R(TSatCeil));
      if (B.satMode == 1) {
        const double L = (B.peakMode && mode_[k] == 0) ? B.ratioMax : 4.0;
        const double Lmax = std::max(B.post * L, B.ceil);
        const double a = -satV / 10.0, p = 20 - 19 * std::log(1 + a) / std::log(101.0);
        if (curveChanged || std::max(Lmax, 1e-9) != B.lmax || p != B.p) buildSat(B, Lmax, p);
      } else if (B.satMode == 2) {
        const double b = 1 - satV / 1000.0;
        B.th = b * B.ceil, B.wd = std::max(B.ceil - B.th, 1.5259e-5);
      }
    }
    if (first) reset();
    // Moving: a new slope restarts the engine (filters, delays, envelopes); the detectors keep their windows.
    else if (s.moveLaw && (r[1] != old.raw[1] || r[3] != old.raw[3])) restart(true);
  }

  void reset() { restart(false); }

  void restart(bool keepDetectors) {
    if (fs_ <= 0) return;
    for (int k = 0; k < kBands; ++k) {
      Band& B = band_[k];
      for (double& v : B.rel) v = 0;
      const double g0 = B.pre * B.post * B.table[0];
      for (double& v : B.gs) v = g0;
      if (!keepDetectors) {
        B.head = B.tail = 0, B.t = 0, B.rmsSum = 0;
        std::fill(B.rms.begin(), B.rms.end(), 0.f);
      }
      B.h = B.hTarget;
      for (int c = 0; c < 4; ++c) B.sepNow[c] = B.sepEnd[c] = B.sepGoal[c] = B.sepTarget[c], B.sepStep[c] = 0;
      std::fill(B.dl[0].begin(), B.dl[0].end(), 0.f), std::fill(B.dl[1].begin(), B.dl[1].end(), 0.f);
      B.w = 0;
    }
    for (auto& b : lc_) b.clear();
    lowLp_.clear(), midHp_.clear(), midLp_.clear(), highHp_.clear(), lowAp_.clear();
    for (auto& b : fir_) b.clear();
    for (auto& b : lin_) b.clear();
    for (auto& h : hbUp_) h.clear();
    for (auto& h : hbDown_) h.clear();
  }

  // Moving (K-46): the separation matrix runs in straight lines per host block, 44.1 per second at most (at the band's
  // own rate: 0.001 a sample at 44.1 kHz, half that when oversampled twice; measured on the oracle). run() can
  // then be called in several parts (the editor's monitor hop) without changing the audio.
  // K-46: the separation coefficients move in straight lines of kSepLine host samples from the
  // moment their target changes, so a move sounds the same at every host block size.
  void beginBlock() {
    if (fs_ <= 0 || !st_.moveLaw) return;
    for (int k = 0; k < kBands; ++k) {
      if (mode_[k] >= 2) continue;
      const Band& B = band_[k];
      bool moved = false;
      for (int c = 0; c < 4; ++c) moved = moved || B.sepTarget[c] != B.sepGoal[c];
      if (moved) sepLine(k, true);
    }
  }
  void process(float* L, float* R, int n) {
    beginBlock();
    run(L, R, n);
  }
  void run(float* L, float* R, int n) {
    if (fs_ <= 0) return;
    const double silenceN = 1.5 * fs_ + lat_;
    for (int i = 0; i < n; ++i) {
      if (st_.moveLaw)
        for (int k = 0; k < kBands; ++k) {
          if (mode_[k] >= 2) continue;
          if (band_[k].sepLeft == 0) sepLine(k, false);
          --band_[k].sepLeft;
        }
      float x[2] = {L[i], R[i]};
      if (std::fabs(x[0]) < 1e-38f && std::fabs(x[1]) < 1e-38f) {
        if (++quiet_ >= silenceN) {
          if (!stopped_) reset(), stopped_ = true;
          L[i] = R[i] = 0;
          continue;
        }
      } else {
        quiet_ = 0, stopped_ = false;
      }
      if (lcOn_)
        for (int c = 0; c < 2; ++c)
          for (auto& b : lc_) x[c] = b.run(c, x[c]);
      float sum[2] = {0, 0};
      if (os_ == 0) {
        lmh(x, sum);
      } else {  // K-S: the low / mid / high section runs oversampled
        float fr[8][2];
        fr[0][0] = x[0], fr[0][1] = x[1];
        int cnt = 1;
        for (int st = 0; st < os_; ++st) {
          float tmp[8][2];
          for (int q = 0; q < cnt; ++q) hbUp_[st].up(fr[q], tmp[2 * q], tmp[2 * q + 1]);
          cnt *= 2;
          for (int q = 0; q < cnt; ++q) fr[q][0] = tmp[q][0], fr[q][1] = tmp[q][1];
        }
        for (int q = 0; q < cnt; ++q) {
          float o[2] = {0, 0};
          lmh(fr[q], o);
          fr[q][0] = o[0], fr[q][1] = o[1];
        }
        for (int st = os_ - 1; st >= 0; --st) {
          cnt /= 2;
          for (int q = 0; q < cnt; ++q) {
            float o[2];
            hbDown_[st].down(fr[2 * q], fr[2 * q + 1], o);
            fr[q][0] = o[0], fr[q][1] = o[1];
          }
        }
        sum[0] = fr[0][0], sum[1] = fr[0][1];
      }
      if (mode_[3] == 2) {
        L[i] = R[i] = 0;
        continue;
      }
      float dryM[2];
      processBand(3, sum, dryM, nullptr);
      L[i] = sum[0], R[i] = sum[1];
    }
    for (auto& B : band_) {
      const double mn = std::min(1.0, B.gs[B.gainStages - 1] / std::max(1e-9, B.pre * B.post * B.table[0]));
      B.grDb = static_cast<float>(20 * std::log10(std::max(1e-6, mn)));
    }
  }

 private:
  struct Band {
    int mask = 0;  // ring size - 1 of this band's buffers
    float scopeIn = 0, scopeOut = 0, scopeEnv = 0;  // K-S: monitor: peaks since the last takeScope (pre-gain input, output, analysis level)
    // settings
    double pre = 1, post = 1, cRel = 0, cAtt = 0, cRel2 = 0, ceil = 1, th = 1, wd = 1, lmax = -1, p = -1, ratioMax = 1;
    float h = 0, hTarget = 0;
    float sepTarget[4] = {1, 1, 0, 0}, sepNow[4] = {1, 1, 0, 0}, sepStep[4] = {0, 0, 0, 0}, sepEnd[4] = {1, 1, 0, 0}, sepGoal[4] = {1, 1, 0, 0};  // K-46: LL, RR, LR, RL
    int sepLeft = 0;  // host samples left in the current line
    bool peakMode = true;
    int D = 0, N = 1, relStages = 3, gainStages = 2, satMode = 0, lastIdx = 0;
    double outLast = 1;
    std::vector<float> table, sat;
    // state
    std::vector<float> dqV, rms, dl[2];
    std::vector<int32_t> dqI;
    int head = 0, tail = 0, w = 0;
    int32_t t = 0;
    float rmsSum = 0;
    double rel[8] = {}, gs[8] = {};
    float grDb = 0;
  };

  // Moving (K-46): only what the new length reads before it is written again; no allocation.
  static void clearDelay(Band& B) {
    for (int i = 1; i <= B.D; ++i)
      for (auto& d : B.dl) d[static_cast<size_t>((B.w - i) & B.mask)] = 0.f;
  }
  static void clearDetector(Band& B) {
    B.head = B.tail = 0, B.rmsSum = 0;
    for (int i = 1; i <= B.N; ++i) B.rms[static_cast<size_t>((B.t - i) & B.mask)] = 0.f;
  }
  static void buildTable(Band& B, const Curve& c, double M, int rev) {
    double maxOut = 0, out = M;
    int lastIdx = 0;
    for (int i = 0; i <= 32768; ++i) {
      const double a = std::max(i, 1) / 8192.0;
      const double X = std::log(2 * a + 1) / std::log(9.0);
      const double y = curveAt(c, X, rev);
      out = (std::pow(9.0, y) - 1) / 2 * M;
      if (std::fabs(out - M) < 5.9604644775390625e-08) out = M;
      else lastIdx = i;
      maxOut = std::max(maxOut, out);
      B.table[static_cast<size_t>(i)] = static_cast<float>(out / a);
    }
    B.table[static_cast<size_t>(lastIdx + 1)] = B.table[static_cast<size_t>(lastIdx)];
    B.lastIdx = lastIdx;
    B.outLast = out;
    B.ratioMax = maxOut / M;
  }
  static void buildSat(Band& B, double Lmax, double p) {
    // J-92: post gain and ceiling at 0 (silence) gave Lmax = ceiling = 0: a
    // NaN table and a NaN read index (crash). Floors keep both finite.
    B.lmax = std::max(Lmax, 1e-9), B.p = p;
    const double top = B.lmax / std::max(B.ceil, 1e-9);
    const double norm = std::pow(std::atan(std::pow(top, p)), 1.0 / p);
    for (int j = 0; j <= 65536; ++j) {
      const double u = j / 65536.0 * top;
      double y = std::pow(std::atan(std::pow(u, p)), 1.0 / p) / norm;
      if (y < 6e-8) y = 0;
      B.sat[static_cast<size_t>(j)] = static_cast<float>(y);
    }
  }
  float saturate(const Band& B, float x) const {
    const float ax = std::fabs(x);
    float y;
    if (B.satMode == 1) {
      double pos = ax * 65536.0 / B.lmax;
      if (!(pos < 65536)) y = B.sat[65536];  // also NaN
      else {
        const int i = static_cast<int>(pos);
        const float f = static_cast<float>(pos - i);
        y = B.sat[static_cast<size_t>(i)] + f * (B.sat[static_cast<size_t>(i + 1)] - B.sat[static_cast<size_t>(i)]);
      }
      y *= static_cast<float>(B.ceil);
    } else {
      if (ax <= B.th) return x;
      y = static_cast<float>(B.th + B.wd * (1 - std::exp(-(ax - B.th) / B.wd)));
    }
    return x < 0 ? -y : y;
  }
  // K-S: split + low / mid / high bands + their sum, at the section's rate.
  void lmh(const float* x, float* sum) {
    // Split.
    float lo[2], mid[2], hi[2], full[2] = {x[0], x[1]};
    if (splitMode_ == 1) {
      for (auto& b : fir_) b.push(x);
      for (int c = 0; c < 2; ++c) lo[c] = fir_[0].out(c), mid[c] = midCopy_ ? x[c] : fir_[1].out(c), hi[c] = fir_[2].out(c);
    } else if (splitMode_ == 2) {
      lin_[0].run(x, lo, fft_, xfade_);
      lin_[1].run(x, mid, fft_, xfade_);
      lin_[2].run(x, hi, fft_, xfade_);
    }
    for (int c = 0; c < 2 && splitMode_ == 0; ++c) {
      float X = x[c];
      lo[c] = lowLp_.run(c, X);
      if (apOn_) lo[c] = lowAp_.run(c, lo[c]);
      if (!st_.masterMid) {
        X = midHp_.run(c, X);
        mid[c] = midLp_.run(c, X);
      } else {  // K-S: the whole input stays whole; the detector copy is still filtered both ways
        mid[c] = midLp_.run(c, st_.rev >= 1 ? midHp_.run(c, X) : X);
      }
      hi[c] = highHp_.run(c, X);
    }
    float* bs[3] = {lo, mid, hi};
    for (int k = 0; k < 3; ++k) {
      if (mode_[k] >= 2) continue;
      float s[2] = {bs[k][0], bs[k][1]}, dry[2];
      if (k == 1 && st_.masterMid) {
        float det[2] = {s[0], s[1]};
        processBand(k, full, dry, det);
        s[0] = full[0], s[1] = full[1];
      } else {
        processBand(k, s, dry, nullptr);
      }
      const float g = pol_[k];
      for (int c = 0; c < 2; ++c) {
        float v = s[c];
        if (mixActive_) v = mix_ * g * v + (1 - mix_) * g * dry[c];
        else if (g == -1.f) v = -v;
        sum[c] += v;
      }
    }
  }

  // s: band signal in/out (stereo); dry: delayed pre-gain copy; det: detector input (null = s).
  void processBand(int k, float* s, float* dry, const float* det) {
    Band& B = band_[k];
    const int md = mode_[k];
    const bool moveLaw = st_.moveLaw;
    if (!moveLaw && B.h != B.hTarget) {  // K-S: separation changes glide (at most 0.001 a sample: no clicks, any block size)
      const float dh = B.hTarget - B.h;
      B.h = std::fabs(dh) <= 0.001f ? B.hTarget : B.h + (dh > 0 ? 0.001f : -0.001f);
    }
    const float ll = moveLaw ? B.sepNow[0] : 1 - B.h, rr = moveLaw ? B.sepNow[1] : 1 - B.h;
    const float lr = moveLaw ? B.sepNow[2] : B.h, rl = moveLaw ? B.sepNow[3] : B.h;
    // Mid on the whole input: the matrix belongs to the detector's copy; the whole input stays whole (with the moving law).
    if (md < 2 && (lr != 0 || rl != 0 || ll != 1 || rr != 1) && !(moveLaw && det)) {
      const float l = s[0], r = s[1];
      s[0] = ll * l + lr * r, s[1] = rr * r + rl * l;
    }
    if (moveLaw && md < 2)
      for (int c = 0; c < 4; ++c) B.sepNow[c] += B.sepStep[c];
    double G = B.post;
    if (md == 0) {
      float dl = s[0], dr = s[1];
      if (det) {  // mid-only master mode: the filtered band drives the detector
        dl = ll * det[0] + lr * det[1], dr = rr * det[1] + rl * det[0];
      }
      float e;
      B.scopeIn = std::max({B.scopeIn, std::fabs(dl), std::fabs(dr)});
      const int32_t t = B.t++;
      if (B.peakMode) {
        const float p = std::max(std::fabs(dl), std::fabs(dr));
        while (B.tail != B.head && B.dqV[static_cast<size_t>((B.tail - 1) & B.mask)] <= p) --B.tail;
        B.dqV[static_cast<size_t>(B.tail & B.mask)] = p, B.dqI[static_cast<size_t>(B.tail & B.mask)] = t;
        ++B.tail;
        while (B.dqI[static_cast<size_t>(B.head & B.mask)] <= t - B.N) ++B.head;
        e = B.dqV[static_cast<size_t>(B.head & B.mask)];
      } else {
        const float q = dl * dl + dr * dr;
        const size_t idx = static_cast<size_t>(t & B.mask);
        B.rmsSum += q - B.rms[static_cast<size_t>((t - B.N) & B.mask)];
        B.rms[idx] = q;
        e = std::sqrt(std::max(0.f, B.rmsSum / (2.f * static_cast<float>(B.N))));
      }
      double lv = e;
      for (int i = 0; i < B.relStages; ++i) {
        double& y = B.rel[i];
        y = lv >= y ? lv : lv + B.cRel * (y - lv);
        if (std::fabs(y) < 5.9604644775390625e-08) y = 0;
        lv = y;
      }
      const float ev = static_cast<float>(lv);
      B.scopeEnv = std::max(B.scopeEnv, ev);
      const double u = B.pre * ev * 8192.25;
      int i = u > 0 ? static_cast<int>(u) : 0;
      const double f = u - i;
      double Gc;
      if (i <= B.lastIdx) {
        const double t0 = B.table[static_cast<size_t>(i)], t1 = B.table[static_cast<size_t>(i + 1)];
        Gc = (t0 + f * (t1 - t0)) * B.pre * B.post;
      } else {
        Gc = B.post * B.outLast / ev;
      }
      double x = Gc;
      for (int j = 0; j < B.gainStages; ++j) {
        double& y = B.gs[j];
        y = x + (x < y ? B.cAtt : B.cRel2) * (y - x);
        x = y;
      }
      G = x;
    }
    // look-ahead delay
    for (int c = 0; c < 2; ++c) {
      B.dl[c][static_cast<size_t>(B.w & B.mask)] = s[c];
      s[c] = B.dl[c][static_cast<size_t>((B.w - B.D) & B.mask)];
      dry[c] = s[c];
    }
    ++B.w;
    const float g = static_cast<float>(G);
    for (int c = 0; c < 2; ++c) s[c] *= g;
    if (md < 2 && B.satMode != 0)
      for (int c = 0; c < 2; ++c) s[c] = saturate(B, s[c]);
    if (md != 0) B.scopeIn = std::max({B.scopeIn, std::fabs(dry[0]), std::fabs(dry[1])});
    B.scopeOut = std::max({B.scopeOut, std::fabs(s[0]), std::fabs(s[1])});
  }

  // K-S: storage built on the UI thread and swapped in by configure().
  struct Pool {
    Pool* next = nullptr;
    double prevFs = 0;
    int prevStages = -1, prevLinN = 0;
    bool hasRing = false;
    double fs = 0;
    int stages = 0;
    struct Ring {
      std::vector<float> table, sat, dqV, rms, dl[2];
      std::vector<int32_t> dqI;
      int mask = 0;
    } ring[kBands];
    FirBand fir[3];
    std::vector<double> firTmp[3];
    int linN = 0;
    FftD fft;
    LinBand lin[3];
  };
  static void fillRing(Pool& p, double fs, int stages) {
    int cap = 1;
    while (cap < static_cast<int>(fs * (1 << stages) * 1.05) + 8) cap <<= 1;
    int capM = 1;
    while (capM < static_cast<int>(fs * 1.05) + 8) capM <<= 1;
    p.hasRing = true, p.fs = fs, p.stages = stages;
    for (int k = 0; k < kBands; ++k) {
      auto& r = p.ring[k];
      const size_t c = static_cast<size_t>(k == 3 ? capM : cap);  // the master band always runs at the base rate
      r.mask = static_cast<int>(c) - 1;
      r.table.assign(32770, 1.f);
      r.sat.assign(65537, 0.f);
      r.dqV.assign(c, 0.f);
      r.dqI.assign(c, 0);
      r.rms.assign(c, 0.f);
      r.dl[0].assign(c, 0.f), r.dl[1].assign(c, 0.f);
    }
    // The older linear-phase split at any oversampling: automation of the split only rewrites taps.
    const size_t firMax = static_cast<size_t>(firTaps(fs * 8.0));
    for (auto& v : p.firTmp) v.reserve(firMax);
    for (auto& f : p.fir) {
      f.taps.reserve(firMax);
      for (auto& l : f.line) l.reserve(2 * firMax);
    }
  }
  static void fillLin(Pool& p, int N) {
    p.linN = N;
    p.fft.init(N);
    for (auto& b : p.lin) b.init(N);
  }
  void adopt(Pool& p) {  // swaps only: the old storage stays in the pool
    if (p.hasRing) {
      for (int k = 0; k < kBands; ++k) {
        Band& b = band_[k];
        auto& r = p.ring[k];
        b.mask = r.mask;
        b.table.swap(r.table), b.sat.swap(r.sat), b.dqV.swap(r.dqV), b.dqI.swap(r.dqI), b.rms.swap(r.rms);
        b.dl[0].swap(r.dl[0]), b.dl[1].swap(r.dl[1]);
      }
      for (int b = 0; b < 3; ++b) {
        fir_[b].taps.swap(p.fir[b].taps);
        fir_[b].line[0].swap(p.fir[b].line[0]), fir_[b].line[1].swap(p.fir[b].line[1]);
        fir_[b].N = 0, fir_[b].pos = 0;
        firTmp_[b].swap(p.firTmp[b]);
      }
      fs_ = p.fs, capStages_ = p.stages;
    }
    if (p.linN > 0) {
      std::swap(fft_, p.fft);
      for (int b = 0; b < 3; ++b) std::swap(lin_[b], p.lin[b]);
      linCap_ = p.linN;
    }
    cfgValid_ = false;
  }
  void retire(Pool* p) {
    Pool* head = retired_.load(std::memory_order_relaxed);
    do p->next = head;
    while (!retired_.compare_exchange_weak(head, p, std::memory_order_release, std::memory_order_relaxed));
  }
  void freeRetired() {
    Pool* p = retired_.exchange(nullptr, std::memory_order_acquire);
    while (p) {
      Pool* n = p->next;
      delete p;
      p = n;
    }
  }
  std::atomic<Pool*> pending_{nullptr}, retired_{nullptr};
  std::atomic<bool> hostPrepared_{false};
  double uiFs_ = 0;  // what prepare() has already published (UI thread only)
  int uiStages_ = -1, uiLinN_ = 0;
  std::vector<double> firTmp_[3];
  int linCap_ = 0, capStages_ = 0, os_ = 0;
  HalfBand hbUp_[3], hbDown_[3];
  double fs_ = 0;
  static constexpr int kSepLine = 512;  // host samples in one separation line
  void sepLine(int k, bool moved) {
    Band& B = band_[k];
    const int frames = k < 3 ? kSepLine << os_ : kSepLine;
    const float limit = static_cast<float>(44.1 / (k < 3 ? fs_ * (1 << os_) : fs_));
    for (int c = 0; c < 4; ++c) {
      const float from = stopped_ ? B.sepTarget[c] : moved ? B.sepNow[c] : B.sepEnd[c];
      float end = from, step = 0;
      if (B.sepTarget[c] != from) {
        end = B.sepTarget[c];
        step = (end - from) / static_cast<float>(frames);
        if (std::fabs(step) > limit) {
          step = step > 0 ? limit : -limit;
          end = static_cast<float>(frames) * step + from;
        }
      }
      B.sepNow[c] = from, B.sepStep[c] = step, B.sepEnd[c] = end, B.sepGoal[c] = B.sepTarget[c];
    }
    B.sepLeft = kSepLine;
  }
  bool cfgValid_ = false, lcOn_ = false, apOn_ = false, mixActive_ = false, stopped_ = false;
  RefState st_;
  int mode_[kBands] = {0, 0, 0, 0};
  float pol_[3] = {1, 1, 1};
  float mix_ = 1;
  int lat_ = 0;
  double quiet_ = 0;
  Biquad lc_[4], lowAp_;
  Chain lowLp_, midHp_, midLp_, highHp_;
  Band band_[kBands];
  int splitMode_ = 0, xfade_ = 0;  // 0 IIR, 1 legacy FIR, 2 FFT bank
  int chainL_ = 0, chainH_ = 0;     // K-46: the filter layout now running (sections - 1, -1 = split removed)
  bool midCopy_ = false;
  FirBand fir_[3];
  LinBand lin_[3];
  FftD fft_;
};

}  // namespace gl::packs::dynamics::maxref

namespace gl {
struct PluginState;
}
namespace gl::packs::dynamics {
maxref::Curve masterRefCurve(const PluginState& s, int band);  // K-S: the curve a band runs (long curves included)
}
