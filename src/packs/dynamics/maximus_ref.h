// Gridline — "dynamics" pack, J-35: reference engine of the mastering
// dynamics (gl.master MsRefModel = 1, gl.enhance EnCustom = 3; study
// analysis/demos/specs/j35_maximus.md).
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
#include "../../engine/MEnv.h"

namespace gl::packs::dynamics::maxref {

inline constexpr int kBands = 4;
inline constexpr int kPts = 8;
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
  bool operator==(const Curve& o) const {
    if (n != o.n) return false;
    for (int i = 0; i < n; ++i)
      if (x[i] != o.x[i] || y[i] != o.y[i] || t[i] != o.t[i] || type[i] != o.type[i]) return false;
    return true;
  }
};

// Reference envelope segment shape: phase t (0..1), tension τ, point type.
inline double segShape(double t, double tau, int type) {
  if (type == 2) return 0.0;  // hold
  if (type >= 3 && type <= 12) {  // J-42: the other point shapes of the envelope model
    const auto m = static_cast<uint8_t>(type);
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
inline double curveAt(const Curve& c, double X) {
  if (c.n <= 0) return 0;
  if (X <= c.x[0]) return static_cast<float>(c.y[0]);
  for (int i = 1; i < c.n; ++i) {
    if (X < c.x[i]) {
      const double dx = c.x[i] - c.x[i - 1];
      const double y0 = static_cast<float>(c.y[i - 1]), y1 = static_cast<float>(c.y[i]);
      if (dx <= 0) return y1;
      const double t = (X - c.x[i - 1]) / dx;
      return y0 + segShape(t, c.t[i], c.type[i]) * (y1 - y0);
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
};

// ---------------------------------------------------------------- J-42 linear-phase splits
// Legacy FIR: N = round(4 ms) | 1 taps (≥ 5) per band; split kernels of length
// round((slope + 1)·os·27500 / f) | 1 (5 … N), windowed sinc (4-term Blackman–Harris),
// centred; mid = δ − (LP_low + HP_high), so the three bands sum to a pure delay.
inline int firTaps(double F) { return std::max(5, static_cast<int>(std::nearbyint(0.004 * F)) | 1); }
inline int firLen(int slope, double f, int N) {
  int L = static_cast<int>(std::nearbyint(static_cast<float>((slope + 1) * 27500.0 / f))) | 1;
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
inline int latencyOf(const RefState& s, double fs) {
  int mode[kBands];
  effectiveModes(s, mode);
  const int split = s.lin ? 3 * linSize(fs, s.linQuality) / 4 : s.fir ? (firTaps(fs) - 1) / 2 : 0;
  return split + std::max({lmhDelay(s, mode, fs, 0), lmhDelay(s, mode, fs, 1), lmhDelay(s, mode, fs, 2)}) + lmhDelay(s, mode, fs, 3);
}

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
  // Buffers are sized when the sample rate is first seen (like the other
  // reference engines): windows and look-ahead up to 1 s plus margin.
  void allocate(double fs) {
    int cap = 1;
    while (cap < static_cast<int>(fs * 1.05) + 8) cap <<= 1;
    kCap = cap;
    for (auto& b : band_) {
      b.table.assign(32770, 1.f);
      b.sat.assign(65537, 0.f);
      b.dqV.assign(static_cast<size_t>(kCap), 0.f);
      b.dqI.assign(static_cast<size_t>(kCap), 0);
      b.rms.assign(static_cast<size_t>(kCap), 0.f);
      b.dl[0].assign(static_cast<size_t>(kCap), 0.f), b.dl[1].assign(static_cast<size_t>(kCap), 0.f);
    }
  }
  int latency() const { return lat_; }
  // Gain reduction of a band (dB, ≤ 0) for meters.
  float bandGr(int k) const { return band_[k].grDb; }

  // Rebuilds what changed; call before process with the current state.
  void configure(const RefState& s, double fs) {
    const bool rateChanged = fs != fs_;
    if (rateChanged) fs_ = fs, cfgValid_ = false, allocate(fs);
    if (cfgValid_ && std::memcmp(s.raw, st_.raw, sizeof s.raw) == 0 && s.solo == st_.solo && s.masterMid == st_.masterMid &&
        s.safetyOff == st_.safetyOff && s.target == st_.target && s.curve[0] == st_.curve[0] && s.curve[1] == st_.curve[1] &&
        s.curve[2] == st_.curve[2] && s.curve[3] == st_.curve[3] && s.fir == st_.fir && s.lin == st_.lin && s.linQuality == st_.linQuality &&
        s.linSpeed == st_.linSpeed)
      return;
    const RefState old = st_;
    const bool first = !cfgValid_;
    st_ = s;
    cfgValid_ = true;
    const int* r = s.raw;
    // Effective band modes.
    effectiveModes(s, mode_);
    // Crossover.
    int slL = r[1] > 0 ? 1 : 0, slH = r[3] > 0 ? 1 : 0;
    pol_[0] = slL == 0 ? -1.f : 1.f, pol_[2] = slH == 0 ? -1.f : 1.f, pol_[1] = 1.f;
    if (s.fir || s.lin) pol_[0] = pol_[2] = 1.f;
    const int sL0 = slL, sH0 = slH;
    if (r[rawIndex(0, TMode)] == 3) slL = -1;
    if (r[rawIndex(2, TMode)] == 3) slH = -1;
    double fL = splitHz(r[0]), fH = splitHz(r[2]);
    const double fmax = 0.49 * fs_;
    fL = std::min(fL, fmax), fH = std::min(fH, fmax);
    if (slL >= 0 && slH >= 0 && fH < fL) std::swap(fL, fH);
    auto design = [&](Chain& ch, int slope, double f, bool hp) {
      ch.n = slope + 1;
      const double w = 2 * 3.141592653589793 * f / fs_, al = slope == 0 ? std::sin(w) : std::sin(w) / std::sqrt(2.0);
      for (auto& b : ch.s) hp ? b.hp(w, al) : b.lp(w, al);
    };
    design(lowLp_, slL, fL, false);
    design(midHp_, slL, fL, true);
    design(midLp_, slH, fH, false);
    design(highHp_, slH, fH, true);
    apOn_ = slH == 1 && slL >= 0;
    {
      const double w = 2 * 3.141592653589793 * fH / fs_;
      lowAp_.ap(w, std::sin(w) / std::sqrt(2.0));
    }
    // J-42: linear-phase splits.
    splitMode_ = s.lin ? 2 : s.fir ? 1 : 0;
    if (splitMode_ == 1) {
      const int N = firTaps(fs_);
      std::vector<double> lo(static_cast<size_t>(N), 0.0), mi(static_cast<size_t>(N), 0.0), hi(static_cast<size_t>(N), 0.0);
      if (slL >= 0) firAdd(lo, N, fL, fs_, firLen(slL, fL, N), false);
      if (slH >= 0) firAdd(hi, N, fH, fs_, firLen(slH, fH, N), true);
      if (slL >= 0 && slH >= 0) {
        for (int i = 0; i < N; ++i) mi[static_cast<size_t>(i)] = -(lo[static_cast<size_t>(i)] + hi[static_cast<size_t>(i)]);
        mi[static_cast<size_t>((N - 1) / 2)] += 1.0;
      } else if (slH >= 0) {
        firAdd(mi, N, fH, fs_, firLen(slH, fH, N), false);
      } else if (slL >= 0) {
        firAdd(mi, N, fL, fs_, firLen(slL, fL, N), true);
      }
      midCopy_ = slL < 0 && slH < 0;
      std::vector<double>* k3[3] = {&lo, &mi, &hi};
      for (int b = 0; b < 3; ++b) {
        if (fir_[b].N != N) fir_[b].init(N);
        for (int i = 0; i < N; ++i) fir_[b].taps[static_cast<size_t>(i)] = static_cast<float>((*k3[b])[static_cast<size_t>(i)]);
      }
    }
    if (splitMode_ == 2) {
      const int N = linSize(fs_, s.linQuality);
      static const float kSpeed[4] = {0.1f, 0.25f, 0.01f, 0.02f};
      xfade_ = static_cast<int>(static_cast<float>(static_cast<int>(fs_)) * kSpeed[std::clamp(s.linSpeed, 0, 3)]);
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
        const float f = std::max(static_cast<float>(i) * static_cast<float>(fs_ / N), 0.1f);
        float m3[3];
        if (f > 0.4999f * static_cast<float>(fs_)) {
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
    for (int k = 0; k < kBands; ++k) band_[k].D = std::min(lmhDelay(s, mode_, fs_, k), kCap - 2);
    lat_ = std::max({band_[0].D, band_[1].D, band_[2].D}) + band_[3].D +
           (splitMode_ == 2 ? 3 * lin_[0].N / 4 : splitMode_ == 1 ? (fir_[0].N - 1) / 2 : 0);
    // Bands.
    for (int k = 0; k < kBands; ++k) {
      Band& B = band_[k];
      auto R = [&](int tag) { return r[rawIndex(k, tag)]; };
      B.pre = gain7(R(TPre)), B.post = gain7(R(TPost));
      const double sv = std::clamp(R(TSep), -1000, 1000) / 1000.0;
      const double sq = sv > 0 ? (std::pow(5.0, sv) - 1) / 4 : sv;
      B.h = static_cast<float>(0.5 * std::min(sq, 1.0));
      B.peakMode = R(TSusMode) == 0;
      B.N = std::clamp(std::max(B.D, static_cast<int>(std::lround(fs_ * timeMs(R(TWindow)) / 1000.0))), 1, kCap - 2);
      B.relStages = R(TRel) == 0 ? 0 : std::clamp(R(TRelCurve), 1, 8);
      {
        double Nr = (2 * timeMs(R(TRel)) * fs_ / 1000.0) * 2 / std::sqrt(std::max(1, B.relStages));
        Nr = std::max(1.0, Nr);
        B.cRel = std::exp(std::log(0.01) / Nr);
      }
      B.gainStages = std::clamp(R(TAttCurve), 1, 8);
      {
        const double sA = std::sqrt(static_cast<double>(B.gainStages));
        double Na = R(TAtt) == 0 ? B.D : fs_ * timeMs(R(TAtt)) / 1000.0;
        const double nAtt = Na * 0.8 / sA;
        const double nRel2 = R(TRel2) == 0 ? nAtt : fs_ * 2 * timeMs(R(TRel2)) / 1000.0 / sA;
        auto co = [](double n) { return n <= 0 ? 0.0 : std::exp(std::log(0.01) / std::max(1.0, n)); };
        B.cAtt = co(nAtt), B.cRel2 = co(nRel2);
      }
      const bool curveChanged = first || !(s.curve[k] == old.curve[k]) || s.safetyOff != old.safetyOff;
      if (curveChanged) buildTable(B, s.curve[k], s.safetyOff ? 1.0 : 0.975);
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
    if (first || rateChanged) reset();
  }

  void reset() {
    if (fs_ <= 0) return;
    for (int k = 0; k < kBands; ++k) {
      Band& B = band_[k];
      for (double& v : B.rel) v = 0;
      const double g0 = B.pre * B.post * B.table[0];
      for (double& v : B.gs) v = g0;
      B.head = B.tail = 0, B.t = 0, B.rmsSum = 0;
      std::fill(B.rms.begin(), B.rms.end(), 0.f);
      std::fill(B.dl[0].begin(), B.dl[0].end(), 0.f), std::fill(B.dl[1].begin(), B.dl[1].end(), 0.f);
      B.w = 0;
    }
    for (auto& b : lc_) b.clear();
    lowLp_.clear(), midHp_.clear(), midLp_.clear(), highHp_.clear(), lowAp_.clear();
    for (auto& b : fir_) b.clear();
    for (auto& b : lin_) b.clear();
  }

  void process(float* L, float* R, int n) {
    if (fs_ <= 0) return;
    const double silenceN = 1.5 * fs_ + lat_;
    for (int i = 0; i < n; ++i) {
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
        if (!st_.masterMid) X = midHp_.run(c, X);
        mid[c] = midLp_.run(c, X);
        hi[c] = highHp_.run(c, X);
      }
      float sum[2] = {0, 0};
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
    // settings
    double pre = 1, post = 1, cRel = 0, cAtt = 0, cRel2 = 0, ceil = 1, th = 1, wd = 1, lmax = -1, p = -1, ratioMax = 1;
    float h = 0;
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

  static void buildTable(Band& B, const Curve& c, double M) {
    double maxOut = 0, out = M;
    int lastIdx = 0;
    for (int i = 0; i <= 32768; ++i) {
      const double a = std::max(i, 1) / 8192.0;
      const double X = std::log(2 * a + 1) / std::log(9.0);
      const double y = curveAt(c, X);
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
  // s: band signal in/out (stereo); dry: delayed pre-gain copy; det: detector input (null = s).
  void processBand(int k, float* s, float* dry, const float* det) {
    Band& B = band_[k];
    const int md = mode_[k];
    if (md < 2 && B.h != 0) {
      const float l = s[0], r = s[1];
      s[0] = (1 - B.h) * l + B.h * r, s[1] = (1 - B.h) * r + B.h * l;
    }
    double G = B.post;
    if (md == 0) {
      float dl = s[0], dr = s[1];
      if (det) {  // mid-only master mode: the filtered band drives the detector
        dl = (1 - B.h) * det[0] + B.h * det[1], dr = (1 - B.h) * det[1] + B.h * det[0];
      }
      float e;
      const int32_t t = B.t++;
      if (B.peakMode) {
        const float p = std::max(std::fabs(dl), std::fabs(dr));
        while (B.tail != B.head && B.dqV[static_cast<size_t>((B.tail - 1) & (kCap - 1))] <= p) --B.tail;
        B.dqV[static_cast<size_t>(B.tail & (kCap - 1))] = p, B.dqI[static_cast<size_t>(B.tail & (kCap - 1))] = t;
        ++B.tail;
        while (B.dqI[static_cast<size_t>(B.head & (kCap - 1))] <= t - B.N) ++B.head;
        e = B.dqV[static_cast<size_t>(B.head & (kCap - 1))];
      } else {
        const float q = dl * dl + dr * dr;
        const size_t idx = static_cast<size_t>(t & (kCap - 1));
        B.rmsSum += q - B.rms[static_cast<size_t>((t - B.N) & (kCap - 1))];
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
      B.dl[c][static_cast<size_t>(B.w & (kCap - 1))] = s[c];
      s[c] = B.dl[c][static_cast<size_t>((B.w - B.D) & (kCap - 1))];
      dry[c] = s[c];
    }
    ++B.w;
    const float g = static_cast<float>(G);
    for (int c = 0; c < 2; ++c) s[c] *= g;
    if (md < 2 && B.satMode != 0)
      for (int c = 0; c < 2; ++c) s[c] = saturate(B, s[c]);
  }

  int kCap = 1;
  double fs_ = 0;
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
  bool midCopy_ = false;
  FirBand fir_[3];
  LinBand lin_[3];
  FftD fft_;
};

}  // namespace gl::packs::dynamics::maxref
