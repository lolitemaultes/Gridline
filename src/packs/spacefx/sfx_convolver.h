// Gridline — "spacefx" pack: Convolver (included by spacefx_dsp.cpp only).
//
// Impulse response pipeline (UI thread, cached): source (file or a procedurally
// generated space) → start trim → reverse → stretch → length → fade in / fade
// out / tighten envelope → self-convolution → low/high cut → normalize →
// resample to the engine rate → partitioned spectra (the kernel).
//
// Runtime (audio thread, zero latency): the first 64 IR samples run as a
// direct FIR; IR samples 64..2047 run as 64-sample uniformly partitioned
// overlap-save blocks; the rest as 1024-sample partitions. Each stage's FFT
// latency is hidden because its first partition starts at least one block
// into the IR. Every computation is scheduled on an internal per-sample
// counter, so output is independent of the host block size.
#pragma once
#include <atomic>
#include <cstdio>
#include <list>
#include <map>
#include <mutex>
#include "../../engine/MEnv.h"
#include "../../engine/Resample.h"
#include "sfx_common.h"

namespace gl::packs::spacefx::detail {

inline std::atomic<double> g_convRate{0.0};

constexpr int kHeadLen = 64;     // direct FIR taps
constexpr int kBA = 64;          // stage A block
constexpr int kPartsA = 31;      // stage A covers IR samples 64 .. 2047
constexpr int kBB = 1024;        // stage B block
constexpr int kStartB = 2;       // stage B starts at IR sample 2048
constexpr int kMaxPartsB = 14;   // stage B covers IR samples 2048 .. 16383
constexpr int kBC = 8192;        // stage C block (multiply-adds spread over the block)
constexpr int kStartC = 2;       // stage C starts at IR sample 16384
constexpr int kDefPartsC = 64;   // stage C capacity a new instance starts with (≈11 s @ 48 kHz)
constexpr double kMaxIrSeconds = 60.0;  // longest IR the kernel holds
inline int maxPartsC(double rate) { return static_cast<int>(std::ceil((kMaxIrSeconds * rate - kBC * kStartC) / kBC)) + 1; }
constexpr const char* kIrTag = "\x01gl.convolver.kernel";

// Paths: 0 = L→L, 1 = R→R, 2 = L→R, 3 = R→L.
struct ConvKernel {
  double rate = 0;
  int paths = 2;
  int len = 0;
  float head[4][kHeadLen] = {};
  int partsA = 0, partsB = 0, partsC = 0;
  std::vector<float> specA[4], specB[4], specC[4];  // parts × (B+1) complex (interleaved)
};

// The prepared IR travels in ChannelAux::samples[0] as a Sample subclass
// (left/right = display channels at the native rate). Identified by path tag.
struct IrData : Sample {
  ConvKernel kernel;
};

// ------------------------------------------------------------ generation
struct IrBuf {
  double rate = 48000;
  int paths = 2;
  std::vector<float> p[4];
  size_t len() const { return p[0].size(); }
  void resize(size_t n) {
    for (int k = 0; k < 4; ++k) p[k].assign(k < paths ? n : 0, 0.f);
  }
};

// Diffuse noise tail with three frequency bands decaying at their own rates.
inline void addTail(std::vector<float>& out, double sr, uint32_t seed, double start, double rtLow, double rtMid, double rtHigh,
                    double onset, double gain, double xLow = 350, double xHigh = 3500) {
  dsp::Noise n;
  n.seed(seed);
  OnePole lp1, lp2;
  lp1.setHz(sr, xLow);
  lp2.setHz(sr, xHigh);
  size_t s0 = static_cast<size_t>(start * sr);
  double kl = std::log(1000.0) / (rtLow * sr), km = std::log(1000.0) / (rtMid * sr), kh = std::log(1000.0) / (rtHigh * sr);
  for (size_t i = s0; i < out.size(); ++i) {
    double t = static_cast<double>(i - s0);
    float w = n.next();
    float lo = lp1.lp(w);
    float mid = lp2.lp(w) - lo;
    float hi = w - lo - mid;
    double env = onset > 0 ? 1.0 - std::exp(-t / (onset * sr)) : 1.0;
    double v = lo * std::exp(-kl * t) * 1.6 + mid * std::exp(-km * t) + hi * std::exp(-kh * t);
    out[i] += static_cast<float>(v * env * gain);
  }
}

// Sparse early reflections: n taps between t0 and t1 seconds.
inline void addEarly(std::vector<float>& out, double sr, uint32_t seed, int n, double t0, double t1, double gain) {
  dsp::Noise r;
  r.seed(seed * 7919u + 13u);
  for (int k = 0; k < n; ++k) {
    double u = (k + 0.5 + 0.4 * r.next()) / n;
    double t = t0 + (t1 - t0) * u * u;
    size_t i = static_cast<size_t>(t * sr);
    if (i >= out.size()) continue;
    double g = gain * (1.0 - 0.6 * u) * (r.next() < 0 ? -1 : 1);
    out[i] += static_cast<float>(g);
  }
}

inline void biquadRun(std::vector<float>& x, dsp::Biquad::Type t, double sr, double hz, double q, double gainDb) {
  dsp::Biquad b;
  b.set(t, sr, hz, q, gainDb);
  for (float& v : x) v = static_cast<float>(b.process(v));
}

inline IrBuf generateSpace(int src, double size, double tone, bool trueStereo) {
  IrBuf ir;
  ir.rate = 48000;
  ir.paths = trueStereo ? 4 : 2;
  const double sr = ir.rate;
  size = clampd(size, 0, 1);
  tone = clampd(tone, 0, 1);
  double secs = 1.0;
  switch (src) {
    case SrcRoom: secs = 0.4 + size * 1.4; break;
    case SrcHall: secs = 2.0 + size * 5.0; break;
    case SrcPlate: secs = 1.4 + size * 3.6; break;
    case SrcSpring: secs = 1.8 + size * 2.8; break;
    case SrcChamber: secs = 1.0 + size * 2.4; break;
    case SrcCabinet: secs = 0.09; break;
    case SrcGated: secs = 0.2 + size * 0.55; break;
    case SrcReverse: secs = 0.6 + size * 2.6; break;
    default: break;
  }
  ir.resize(static_cast<size_t>(secs * sr));
  for (int p = 0; p < ir.paths; ++p) {
    std::vector<float>& h = ir.p[p];
    uint32_t seed = 0x9E3779B9u * static_cast<uint32_t>(p + 1) + static_cast<uint32_t>(src) * 101u;
    double cross = p >= 2 ? 0.55 : 1.0;  // cross paths of true stereo are weaker
    double hiRatio = 0.25 + 0.6 * tone;
    switch (src) {
      case SrcRoom: {
        double rt = 0.25 + size * 0.9;
        addEarly(h, sr, seed, 14, 0.002, 0.012 + 0.03 * size, 0.55 * cross);
        addTail(h, sr, seed, 0.004, rt * 1.1, rt, rt * hiRatio, 0.004, 0.32 * cross);
        break;
      }
      case SrcHall: {
        double rt = 1.2 + size * 3.6;
        addEarly(h, sr, seed, 10, 0.012, 0.035 + 0.07 * size, 0.38 * cross);
        addTail(h, sr, seed, 0.01, rt * 1.3, rt, rt * hiRatio, 0.03 + 0.04 * size, 0.22 * cross);
        break;
      }
      case SrcPlate: {
        double rt = 0.8 + size * 3.0;
        addTail(h, sr, seed, 0.0005, rt * 0.8, rt, rt * (0.55 + 0.45 * tone), 0.002, 0.3 * cross, 500, 5000);
        break;
      }
      case SrcChamber: {
        double rt = 0.6 + size * 1.8;
        addEarly(h, sr, seed, 8, 0.005, 0.02 + 0.04 * size, 0.45 * cross);
        addTail(h, sr, seed, 0.006, rt * 0.9, rt, rt * hiRatio * 0.9, 0.008, 0.28 * cross, 400, 2800);
        break;
      }
      case SrcGated: {
        dsp::Noise n;
        n.seed(seed);
        double gate = 0.12 + size * 0.42;
        OnePole lp;
        lp.setHz(sr, 2500 + tone * 9000);
        for (size_t i = 0; i < h.size(); ++i) {
          double t = static_cast<double>(i) / sr;
          double env = t < 0.004 ? t / 0.004 : (t < gate ? 1.0 - 0.15 * t / gate : std::max(0.0, 0.85 * (1.0 - (t - gate) / 0.012)));
          h[i] = static_cast<float>(lp.lp(n.next()) * env * 0.35 * cross);
        }
        addEarly(h, sr, seed, 6, 0.002, 0.02, 0.4 * cross);
        break;
      }
      case SrcReverse: {
        double rt = 0.6 + size * 2.4;
        std::vector<float> t(h.size(), 0.f);
        addTail(t, sr, seed, 0.0, rt * 1.2, rt, rt * hiRatio, 0.002, 0.3 * cross);
        for (size_t i = 0; i < h.size(); ++i) h[i] = t[h.size() - 1 - i];
        size_t fade = static_cast<size_t>(0.004 * sr);
        for (size_t i = 0; i < fade && i < h.size(); ++i) h[h.size() - 1 - i] *= static_cast<float>(i) / static_cast<float>(fade);
        break;
      }
      case SrcSpring: {
        // Dispersive "chirp" echoes that smear further on each round trip.
        double period = 0.022 + 0.035 * size, rt = 1.4 + size * 2.4;
        dsp::Noise n;
        n.seed(seed);
        for (int e = 0; e < 200; ++e) {
          double t0 = 0.0015 + e * period * (1.0 + 0.03 * (p & 1));
          if (t0 >= secs) break;
          double amp = std::pow(10.0, -3.0 * t0 / rt) * (e % 2 ? -0.55 : 0.75) * cross;
          double dur = 0.012 + 0.006 * e;  // chirp length grows each pass
          double f0 = 3800 + 1500 * tone, f1 = 260;
          size_t i0 = static_cast<size_t>(t0 * sr), m = static_cast<size_t>(dur * sr);
          double ph = 0;
          for (size_t i = 0; i < m && i0 + i < h.size(); ++i) {
            double u = static_cast<double>(i) / static_cast<double>(m);
            double f = f0 * std::pow(f1 / f0, std::sqrt(u));
            ph += kTwoPi * f / sr;
            double w = std::sin(kPi * u);
            h[i0 + i] += static_cast<float>(amp * w * std::sin(ph) / std::sqrt(1.0 + e * 0.6));
          }
        }
        addTail(h, sr, seed, 0.004, rt * 0.7, rt * 0.8, rt * 0.3, 0.03, 0.05 * cross, 300, 3000);
        biquadRun(h, dsp::Biquad::HighPass, sr, 110, 0.7, 0);
        biquadRun(h, dsp::Biquad::LowPass, sr, 4200 + 2500 * tone, 0.7, 0);
        break;
      }
      case SrcCabinet: {
        if (p >= 2) break;  // cabinets have no cross paths
        h[0] = 1.f;
        double scale = 1.25 - 0.5 * size;  // bigger box → lower resonances
        biquadRun(h, dsp::Biquad::HighPass, sr, 65 * scale, 0.8, 0);
        biquadRun(h, dsp::Biquad::Peak, sr, 105 * scale, 2.2, 6);
        biquadRun(h, dsp::Biquad::Peak, sr, 1700 * scale * (1 + 0.04 * p), 1.3, 4);
        biquadRun(h, dsp::Biquad::Peak, sr, 3100 * scale, 2.0, -5);
        double lp = 3200 + 3500 * tone;
        biquadRun(h, dsp::Biquad::LowPass, sr, lp, 0.9, 0);
        biquadRun(h, dsp::Biquad::LowPass, sr, lp * 1.15, 0.6, 0);
        break;
      }
      default: break;
    }
  }
  return ir;
}

// Cubic-interpolated resample of every path by `ratio` (output length = len * ratio).
inline void resampleIr(IrBuf& ir, double ratio) {
  if (std::fabs(ratio - 1.0) < 1e-9 || ir.len() == 0) return;
  size_t n = static_cast<size_t>(static_cast<double>(ir.len()) * ratio);
  for (int p = 0; p < ir.paths; ++p) {
    const std::vector<float>& x = ir.p[p];
    std::vector<float> y(n);
    // Low-pass before decimating to limit aliasing.
    std::vector<float> src = x;
    if (ratio < 1.0) {
      biquadRun(src, dsp::Biquad::LowPass, ir.rate, 0.45 * ir.rate * ratio, 0.6, 0);
      biquadRun(src, dsp::Biquad::LowPass, ir.rate, 0.45 * ir.rate * ratio, 0.6, 0);
    }
    auto at = [&](long i) { return i >= 0 && static_cast<size_t>(i) < src.size() ? src[static_cast<size_t>(i)] : 0.f; };
    for (size_t i = 0; i < n; ++i) {
      double pos = static_cast<double>(i) / ratio;
      long k = static_cast<long>(pos);
      float f = static_cast<float>(pos - static_cast<double>(k));
      float xm1 = at(k - 1), x0 = at(k), x1 = at(k + 1), x2 = at(k + 2);
      float c1 = 0.5f * (x1 - xm1), c2 = xm1 - 2.5f * x0 + 2.f * x1 - 0.5f * x2, c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
      y[i] = ((c3 * f + c2) * f + c1) * f + x0;
    }
    ir.p[p] = std::move(y);
  }
}

// Linear convolution of a signal with itself (FFT), truncated to its length.
inline std::vector<float> selfConvolve(const std::vector<float>& x) {
  size_t L = x.size();
  int n = 2;
  while (static_cast<size_t>(n) < 2 * L) n <<= 1;
  Fft f;
  f.init(n);
  std::vector<float> a(static_cast<size_t>(2 * n), 0.f);
  for (size_t i = 0; i < L; ++i) a[2 * i] = x[i];
  f.run(a.data(), false);
  for (int k = 0; k < n; ++k) {
    float r = a[static_cast<size_t>(2 * k)], im = a[static_cast<size_t>(2 * k + 1)];
    a[static_cast<size_t>(2 * k)] = r * r - im * im;
    a[static_cast<size_t>(2 * k + 1)] = 2 * r * im;
  }
  f.run(a.data(), true);
  std::vector<float> y(L);
  for (size_t i = 0; i < L; ++i) y[i] = a[2 * i] / static_cast<float>(n);
  return y;
}

inline double energy(const std::vector<float>& x) {
  double e = 0;
  for (float v : x) e += static_cast<double>(v) * v;
  return e;
}

// Time stretch that keeps the spectrum (phase vocoder, 2048-point frames,
// 4x overlap): output frame m reads the input at m·hop/ratio and advances
// each bin's phase by the input's own phase step over one hop.
inline void stretchKeepPitch(IrBuf& ir, double ratio) {
  if (std::fabs(ratio - 1.0) < 1e-9 || ir.len() < 16) return;
  constexpr int N = 2048, H = N / 4;
  Fft f;
  f.init(N);
  std::vector<float> win(N);
  for (int i = 0; i < N; ++i) win[static_cast<size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(kTwoPi * i / N));
  const size_t inLen = ir.len(), outLen = static_cast<size_t>(static_cast<double>(inLen) * ratio);
  std::vector<float> a(2 * N), b(2 * N);
  std::vector<double> phase(N / 2 + 1);
  for (int p = 0; p < ir.paths; ++p) {
    const std::vector<float>& x = ir.p[p];
    std::vector<float> y(outLen + N, 0.f);
    std::fill(phase.begin(), phase.end(), 0.0);
    auto frame = [&](double pos, std::vector<float>& buf) {
      long s0 = static_cast<long>(std::floor(pos)) - N / 2;
      for (int i = 0; i < N; ++i) {
        long k = s0 + i;
        float v = k >= 0 && static_cast<size_t>(k) < inLen ? x[static_cast<size_t>(k)] : 0.f;
        buf[static_cast<size_t>(2 * i)] = v * win[static_cast<size_t>(i)];
        buf[static_cast<size_t>(2 * i + 1)] = 0.f;
      }
      f.run(buf.data(), false);
    };
    for (size_t m = 0; m * H < outLen + N / 2; ++m) {
      double pos = static_cast<double>(m * H) / ratio;
      frame(pos, a);
      frame(pos + H, b);
      for (int k = 0; k <= N / 2; ++k) {
        double ar = a[static_cast<size_t>(2 * k)], ai = a[static_cast<size_t>(2 * k + 1)];
        double br = b[static_cast<size_t>(2 * k)], bi = b[static_cast<size_t>(2 * k + 1)];
        double mag = std::hypot(ar, ai);
        if (m == 0)
          phase[static_cast<size_t>(k)] = std::atan2(ai, ar);
        double re = mag * std::cos(phase[static_cast<size_t>(k)]), im = mag * std::sin(phase[static_cast<size_t>(k)]);
        phase[static_cast<size_t>(k)] += std::atan2(bi, br) - std::atan2(ai, ar);
        a[static_cast<size_t>(2 * k)] = static_cast<float>(re);
        a[static_cast<size_t>(2 * k + 1)] = static_cast<float>(im);
        if (k > 0 && k < N / 2) {
          a[static_cast<size_t>(2 * (N - k))] = static_cast<float>(re);
          a[static_cast<size_t>(2 * (N - k) + 1)] = static_cast<float>(-im);
        }
      }
      f.run(a.data(), true);
      // Hann analysis × Hann synthesis at 4x overlap sums to 1.5.
      const float g = 1.f / (static_cast<float>(N) * 1.5f);
      long o0 = static_cast<long>(m * H) - N / 2;
      for (int i = 0; i < N; ++i) {
        long k = o0 + i;
        if (k >= 0 && static_cast<size_t>(k) < y.size()) y[static_cast<size_t>(k)] += a[static_cast<size_t>(2 * i)] * win[static_cast<size_t>(i)] * g;
      }
    }
    y.resize(outLen);
    ir.p[p] = std::move(y);
  }
}

// Zero-phase spectral gain from the EQ curve, blended with the dry IR by `mix`.
inline void applyEqCurve(IrBuf& ir, const std::vector<EnvPoint>& eq, double mix) {
  if (eq.empty() || mix <= 0 || ir.len() == 0) return;
  bool flat = true;
  for (const EnvPoint& q : eq) flat = flat && std::fabs(q.v - 0.5) < 1e-6;
  if (flat) return;
  size_t L = ir.len();
  int n = 2;
  while (static_cast<size_t>(n) < L + 4096) n <<= 1;
  Fft f;
  f.init(n);
  std::vector<float> gains(static_cast<size_t>(n / 2 + 1));
  for (int k = 0; k <= n / 2; ++k) {
    double hz = static_cast<double>(k) * ir.rate / n;
    gains[static_cast<size_t>(k)] = static_cast<float>(std::pow(10.0, convEqDb(eq, std::max(hz, 1.0)) / 20.0));
  }
  std::vector<float> a(static_cast<size_t>(2 * n));
  for (int p = 0; p < ir.paths; ++p) {
    std::fill(a.begin(), a.end(), 0.f);
    for (size_t i = 0; i < L; ++i) a[2 * i] = ir.p[p][i];
    f.run(a.data(), false);
    for (int k = 0; k < n; ++k) {
      float g = gains[static_cast<size_t>(k <= n / 2 ? k : n - k)];
      a[static_cast<size_t>(2 * k)] *= g;
      a[static_cast<size_t>(2 * k + 1)] *= g;
    }
    f.run(a.data(), true);
    const float inv = 1.f / static_cast<float>(n);
    for (size_t i = 0; i < L; ++i) ir.p[p][i] = static_cast<float>((1.0 - mix) * ir.p[p][i] + mix * a[2 * i] * inv);
  }
}

// ------------------------------------------------- reference IR build (CvModel = 1)
// J-42, spec analysis/demos/specs/j35_convolver.md: normalise (300 ms RMS peak
// to 0.05) → pan / volume / separation envelopes → self-convolution, all at the
// file rate; then per engine rate: stretch + rate conversion to stretch × rate,
// EQ curve (zero-phase FIR from the smoothed curve, or a plain gain when the
// curve is flat), trailing-silence trim. Pre-delay runs as an integer delay.
namespace ref {

// Point envelope with FL's segment shapes (shape and tension of the point ending a segment).
struct EnvEval {
  const std::vector<EnvPoint>* p = nullptr;
  std::vector<menv::Coef> c;
  explicit EnvEval(const std::vector<EnvPoint>& pts) : p(&pts) {
    for (const EnvPoint& q : pts) c.push_back(menv::coef(static_cast<uint8_t>(std::clamp(q.c, 0, 12)), static_cast<float>(q.k)));
  }
  double at(double t) const {
    const auto& v = *p;
    if (v.empty()) return 0.5;
    if (t < v.front().t) return v.front().v;
    if (t >= v.back().t) return v.back().v;
    size_t i = static_cast<size_t>(std::upper_bound(v.begin(), v.end(), t, [](double x, const EnvPoint& q) { return x < q.t; }) - v.begin());
    const EnvPoint& a = v[i - 1];
    const EnvPoint& b = v[i];
    const double span = b.t - a.t;
    const double f = span > 0 ? (t - a.t) / span : 1.0;
    const uint8_t mode = static_cast<uint8_t>(std::clamp(b.c, 0, 12));
    return a.v + (b.v - a.v) * menv::shape(mode, c[i], f);
  }
};

inline bool flatAt(const std::vector<EnvPoint>& e, double v) {
  for (const EnvPoint& q : e)
    if (static_cast<float>(q.v) != static_cast<float>(v)) return false;
  return true;
}

// Pan / gain / separation → 2×2 matrix [LL, RR, RL, LR] (constant power ×√2 with a fold,
// separation cross-feed), the same law as the Delay Bank feedback matrix.
inline void sepPan(double p, double g, double sp, float m[4]) {
  if (p == 0 && sp == 0) {
    m[0] = m[1] = static_cast<float>(g), m[2] = m[3] = 0;
    return;
  }
  if (g == 0) {
    m[0] = m[1] = m[2] = m[3] = 0;
    return;
  }
  const float sq2 = static_cast<float>(1.0 / std::sin(kPi / 4));
  const float th = static_cast<float>((1.0 + p) * kPi / 4);
  const float L = static_cast<float>(std::cos(th) * g * sq2), R = static_cast<float>(std::sin(th) * g * sq2);
  float cL = 0, cR = 0;
  if (p > 0) cR = static_cast<float>(1.0 - L / g);
  else cL = static_cast<float>(1.0 - R / g);
  if (sp != 0) {
    const float t = sp > 0 ? static_cast<float>(std::exp(sp * std::log(5.0)) - 1.0) * 0.25f : static_cast<float>(sp);
    cL = std::min(cL + t, 1.f), cR = std::min(cR + t, 1.f);
  }
  m[2] = L * (cL / 2), m[0] = L * (1 - cL / 2);
  m[3] = R * (cR / 2), m[1] = R * (1 - cR / 2);
}

// Linear convolution y = a ∗ b (full length a + b − 1), float FFT.
inline std::vector<float> convolve(const std::vector<float>& a, const std::vector<float>& b) {
  if (a.empty() || b.empty()) return {};
  const size_t full = a.size() + b.size() - 1;
  int n = 2;
  while (static_cast<size_t>(n) < full) n <<= 1;
  Fft f;
  f.init(n);
  std::vector<float> x(static_cast<size_t>(2 * n), 0.f), y(static_cast<size_t>(2 * n), 0.f);
  for (size_t i = 0; i < a.size(); ++i) x[2 * i] = a[i];
  for (size_t i = 0; i < b.size(); ++i) y[2 * i] = b[i];
  f.run(x.data(), false);
  f.run(y.data(), false);
  for (int k = 0; k < n; ++k) {
    const size_t r = static_cast<size_t>(2 * k), im = r + 1;
    const double xr = x[r], xi = x[im], yr = y[r], yi = y[im];
    x[r] = static_cast<float>(xr * yr - xi * yi);
    x[im] = static_cast<float>(xr * yi + xi * yr);
  }
  f.run(x.data(), true);
  std::vector<float> out(full);
  const double inv = 1.0 / n;
  for (size_t i = 0; i < full; ++i) out[i] = static_cast<float>(x[2 * i] * inv);
  return out;
}

// Normalise: peak of a causal 300 ms RMS (both channels) brought to 0.05.
inline void normalise(IrBuf& ir) {
  const size_t L = ir.len();
  if (L == 0) return;
  const long w = std::max(1L, static_cast<long>(std::nearbyint(300.0 * ir.rate * 0.001)));
  double sum = 0, peak = 0;
  const std::vector<float>& l = ir.p[0];
  const std::vector<float>& r = ir.p[1];
  for (size_t i = 0; i < L; ++i) {
    sum += static_cast<double>(l[i]) * l[i] + static_cast<double>(r[i]) * r[i];
    if (static_cast<long>(i) >= w) {
      const size_t o = i - static_cast<size_t>(w);
      sum -= static_cast<double>(l[o]) * l[o] + static_cast<double>(r[o]) * r[o];
    }
    peak = std::max(peak, std::sqrt(std::max(0.0, sum / (2.0 * static_cast<double>(w)))));
  }
  if (peak <= 0) return;
  const float g = static_cast<float>(0.05 / peak);
  for (int p = 0; p < 2; ++p)
    for (float& v : ir.p[p]) v *= g;
}

// Pan (0), volume (1) and separation (2) envelopes, time = seconds of the source file.
inline void envelopes(IrBuf& ir, const ConvData& cd) {
  const bool pan = !cd.pan.empty() && !flatAt(cd.pan, 0.5), vol = !cd.env.empty() && !flatAt(cd.env, 0.5),
             sep = !cd.sep.empty() && !flatAt(cd.sep, 0.5);
  if (!pan && !vol && !sep) return;
  const EnvEval ep(cd.pan), ev(cd.env), es(cd.sep);
  float m[4];
  for (size_t i = 0; i < ir.len(); ++i) {
    const double t = static_cast<double>(i) / ir.rate;
    const double p = pan ? 2.0 * (static_cast<float>(ep.at(t)) - 0.5) : 0.0;
    const double g = vol ? 2.0 * static_cast<float>(ev.at(t)) : 1.0;
    const double sp = sep ? 2.0 * (static_cast<float>(es.at(t)) - 0.5) : 0.0;
    sepPan(p, g, sp, m);
    const float l = ir.p[0][i], r = ir.p[1][i];
    ir.p[0][i] = m[0] * l + m[2] * r;
    ir.p[1][i] = m[1] * r + m[3] * l;
  }
}

// IR' = (1 − s/2)·IR + (s/2)·(IR ∗ IR)/‖IR‖ (largest channel L2 norm), cropped to the IR length.
inline void selfConvolution(IrBuf& ir, double s) {
  if (s == 0 || ir.len() == 0) return;
  double n2 = 0;
  for (int p = 0; p < 2; ++p) n2 = std::max(n2, energy(ir.p[p]));
  const double g = n2 > 0 ? 1.0 / std::sqrt(n2) : 1.0;
  const float dry = static_cast<float>(1.0 - 0.5 * s), wet = static_cast<float>(0.5 * s * g);
  for (int p = 0; p < 2; ++p) {
    std::vector<float> y = convolve(ir.p[p], ir.p[p]);
    for (size_t i = 0; i < ir.len(); ++i) ir.p[p][i] = dry * ir.p[p][i] + wet * y[i];
  }
}

// EQ-curve gain law: (3^(2v) − 1) / 2 (0.5 → 0 dB).
inline float eqGain(double v) {
  if (2.0 * v == 1.0) return 1.f;
  const float ln3 = 1.0986123f;
  return static_cast<float>((std::exp(static_cast<double>(static_cast<float>(2.0 * v) * ln3)) - 1.0) * 0.5);
}
// Frequency (Hz) → curve position 0..1 (log above 130.81 Hz, linear below).
inline double eqX(double f) {
  const double f0 = 440.0 * std::pow(2.0, (36.0 - 57.0) / 12.0), f1 = 22050.0;
  const double x0 = std::log(2.0) / std::log(2.0 * f1 / f0), K = f1 / f0;
  if (f < f0) return f * x0 / f0;
  return x0 + (1.0 - x0) * std::log(1.0 + (f - f0) * K / (f1 - f0)) / std::log(1.0 + K);
}

// Zero-phase FIR (length M, centre M/2) from a magnitude curve G[0..M/2] (Hann over M + 2 points).
inline std::vector<float> firFromCurve(const std::vector<double>& G) {
  const int N = static_cast<int>(G.size()) - 1, M = 2 * N;
  Fft f;
  f.init(M);
  std::vector<float> a(static_cast<size_t>(2 * M), 0.f);
  for (int k = 0; k <= N; ++k) a[static_cast<size_t>(2 * k)] = static_cast<float>(G[static_cast<size_t>(k)]);
  for (int k = 1; k < N; ++k) a[static_cast<size_t>(2 * (M - k))] = static_cast<float>(G[static_cast<size_t>(k)]);
  f.run(a.data(), true);
  std::vector<float> h(static_cast<size_t>(M));
  for (int n = 0; n < M; ++n) {
    const double v = a[static_cast<size_t>(2 * ((n + M / 2) % M))] / static_cast<double>(M);
    const double w = 0.5 - 0.5 * std::cos(kTwoPi * (n + 1) / (M + 1.0));
    h[static_cast<size_t>(n)] = static_cast<float>(v * w);
  }
  return h;
}

// Spectral smoothing kernel of the EQ curve: Gaussian response exp(−½(k/σ)²), σ = 0.25·65537/128,
// designed as a 131072-point zero-phase FIR (taps below 1e-12 of the peak dropped).
inline const std::vector<double>& smoothKernel() {
  static const std::vector<double> k = [] {
    const int N = 65536;
    const double sk = 0.25 * 65537.0 / 128.0;
    std::vector<double> G(static_cast<size_t>(N + 1));
    for (int i = 0; i <= N; ++i) G[static_cast<size_t>(i)] = std::exp(-0.5 * (i / sk) * (i / sk));
    std::vector<float> h = firFromCurve(G);
    const int c = N;
    double mx = h[static_cast<size_t>(c)];
    int r = 0;
    while (r + 1 < N && std::fabs(h[static_cast<size_t>(c + r + 1)]) > 1e-12 * mx) ++r;
    std::vector<double> out(static_cast<size_t>(2 * r + 1));
    for (int i = -r; i <= r; ++i) out[static_cast<size_t>(i + r)] = h[static_cast<size_t>(c + i)];
    return out;
  }();
  return k;
}

// EQ curve on an IR at rate R' (= stretch × engine rate), r = stretch.
inline void eqCurve(IrBuf& ir, const std::vector<EnvPoint>& eq, double mix, double rp, double r) {
  if (eq.empty() || ir.len() == 0) return;
  if (flatAt(eq, eq.front().v)) {
    if (2.0 * static_cast<float>(eq.front().v) == 1.0) return;
    const float g = eqGain(static_cast<float>(eq.front().v));
    for (int p = 0; p < 2; ++p)
      for (float& v : ir.p[p]) v *= g;
    return;
  }
  if (mix <= 0) return;
  int lg = 1;
  const double n2 = std::nearbyint(2.0 * rp);
  while (static_cast<double>(1L << lg) < n2) ++lg;
  lg = std::clamp(lg, 4, 20);
  const int N = 1 << lg;
  const EnvEval ev(eq);
  std::vector<double> G(static_cast<size_t>(N + 1));
  for (int i = 0; i <= N; ++i) {
    const double f = static_cast<double>(static_cast<float>(i * rp / (N * 22050.0 * r * 2.0))) * 22050.0;
    G[static_cast<size_t>(i)] = eqGain(static_cast<float>(ev.at(eqX(f))));
  }
  // Spectral smoothing (curve padded with its end values).
  const std::vector<double>& sk = smoothKernel();
  const int R = static_cast<int>(sk.size() / 2);
  std::vector<double> Gs(G.size());
  for (int i = 0; i <= N; ++i) {
    double acc = 0;
    for (int j = -R; j <= R; ++j) acc += sk[static_cast<size_t>(j + R)] * G[static_cast<size_t>(std::clamp(i - j, 0, N))];
    Gs[static_cast<size_t>(i)] = acc;
  }
  const std::vector<float> h = firFromCurve(Gs);
  const size_t half = static_cast<size_t>(N);  // M / 2
  const float dry = static_cast<float>(1.0 - mix), wet = static_cast<float>(mix);
  for (int p = 0; p < 2; ++p) {
    std::vector<float> y = convolve(ir.p[p], h);
    for (size_t j = 0; j < ir.len(); ++j) ir.p[p][j] = dry * ir.p[p][j] + wet * y[j + half];
  }
}

// Drops trailing frames below 2^-24 on both channels.
inline void trimTail(IrBuf& ir) {
  const float eps = 1.0f / 16777216.0f;
  size_t n = ir.len();
  while (n > 0 && std::fabs(ir.p[0][n - 1]) < eps && std::fabs(ir.p[1][n - 1]) < eps) --n;
  for (int p = 0; p < 2; ++p) ir.p[p].resize(n);
}

// File-rate part of the chain.
inline void buildSource(IrBuf& ir, const PluginState& s) {
  ir.paths = 2;
  for (int p = 2; p < 4; ++p) ir.p[p].clear();
  const ConvData cd = parseConvData(s.data);
  if (pb(s, CvNormalize)) normalise(ir);
  envelopes(ir, cd);
  selfConvolution(ir, clampd(pv(s, CvSelfConv, 0), 0, 1));
}

// Engine-rate part: stretch + conversion, EQ, tail trim.
inline void buildForRate(IrBuf& ir, const PluginState& s, double rate) {
  const double r = clampd(pv(s, CvStretch, 1), 0.5, 2.0), rp = r * rate;
  if (const size_t len = ir.len(); len > 0 && std::fabs(rp - ir.rate) > 1e-9)  // len: read before p[0] changes
    for (int p = 0; p < 2; ++p) ir.p[p] = resampleChannel(ir.p[p].data(), len, ir.rate, rp, 64);
  ir.rate = rp;
  const ConvData cd = parseConvData(s.data);
  eqCurve(ir, cd.eq, clampd(pv(s, CvEqMix, 1), 0, 1), rp, r);
  trimTail(ir);
  ir.rate = rate;  // played at the engine rate
}

}  // namespace ref

// Applies the editing pipeline to a raw IR (see header comment).
inline void editIr(IrBuf& ir, const PluginState& s, double maxSeconds) {
  if (pi(s, CvModel, 0) == 1) {
    ref::buildSource(ir, s);
    return;
  }
  const ConvData cd = parseConvData(s.data);
  // Start trim.
  size_t start = static_cast<size_t>(pv(s, CvStart, 0) * 0.001 * ir.rate);
  if (start > 0)
    for (int p = 0; p < ir.paths; ++p) {
      auto& v = ir.p[p];
      v.erase(v.begin(), v.begin() + static_cast<long>(std::min(start, v.size())));
    }
  if (pb(s, CvReverse))
    for (int p = 0; p < ir.paths; ++p) std::reverse(ir.p[p].begin(), ir.p[p].end());
  if (pi(s, CvStretchMode, 0) == 1)
    stretchKeepPitch(ir, clampd(pv(s, CvStretch, 1), 0.5, 2.0));
  else
    resampleIr(ir, clampd(pv(s, CvStretch, 1), 0.5, 2.0));
  // Length.
  double lenS = std::min(static_cast<double>(pv(s, CvLength, 12)), maxSeconds);
  size_t len = std::min(ir.len(), static_cast<size_t>(std::max(1.0, lenS * ir.rate)));
  for (int p = 0; p < ir.paths; ++p) ir.p[p].resize(len);
  // Envelope.
  double fin = pv(s, CvFadeIn, 0) * 0.001 * ir.rate, fout = clampd(pv(s, CvFadeOut, 0.1), 0, 1) * static_cast<double>(len);
  double tight = clampd(pv(s, CvTighten, 0), 0, 1) * 9.0;
  for (size_t i = 0; i < len; ++i) {
    double t = static_cast<double>(i);
    double g = 1.0;
    if (fin > 1 && t < fin) g *= 0.5 - 0.5 * std::cos(kPi * t / fin);
    double rem = static_cast<double>(len) - t;
    if (fout > 1 && rem < fout) g *= 0.5 - 0.5 * std::cos(kPi * rem / fout);
    if (tight > 0) g *= std::exp(-tight * t / static_cast<double>(len));
    for (int p = 0; p < ir.paths; ++p) ir.p[p][i] *= static_cast<float>(g);
  }
  // Self-convolution blend (energy-matched).
  double sc = clampd(pv(s, CvSelfConv, 0), 0, 1);
  if (sc > 0.001 && len > 1)
    for (int p = 0; p < ir.paths; ++p) {
      std::vector<float> c = selfConvolve(ir.p[p]);
      double e0 = energy(ir.p[p]), e1 = energy(c);
      float g = e1 > 1e-20 ? static_cast<float>(std::sqrt(e0 / e1)) : 0.f;
      for (size_t i = 0; i < len; ++i) ir.p[p][i] = static_cast<float>((1 - sc) * ir.p[p][i] + sc * c[i] * g);
    }
  // IR amplitude envelope (0.5 = unity, 1 = ×2), then the EQ curve.
  if (pb(s, CvEnvOn) && cd.env.size() >= 1)
    for (size_t i = 0; i < len; ++i) {
      float g = static_cast<float>(2.0 * clampd(envAt(cd.env, static_cast<double>(i) / ir.rate), 0, 1));
      for (int p = 0; p < ir.paths; ++p) ir.p[p][i] *= g;
    }
  applyEqCurve(ir, cd.eq, clampd(pv(s, CvEqMix, 1), 0, 1));
  // Low / high cut (12 dB/oct each).
  double lc = pv(s, CvLowCut, 20), hc = pv(s, CvHighCut, 20000);
  for (int p = 0; p < ir.paths; ++p) {
    if (lc > 21) biquadRun(ir.p[p], dsp::Biquad::HighPass, ir.rate, lc, 0.707, 0);
    if (hc < 19900 && hc < ir.rate * 0.45) biquadRun(ir.p[p], dsp::Biquad::LowPass, ir.rate, hc, 0.707, 0);
  }
  // Stereo mode: mono folds everything to one IR used for both sides.
  int mode = pi(s, CvStereo, 1);
  if (mode == 2) {
    std::vector<float> m(len, 0.f);
    for (int p = 0; p < ir.paths; ++p)
      for (size_t i = 0; i < len; ++i) m[i] += ir.p[p][i] / static_cast<float>(ir.paths);
    ir.paths = 2;
    ir.p[0] = m;
    ir.p[1] = m;
    ir.p[2].clear();
    ir.p[3].clear();
  } else if (mode == 0 && ir.paths == 4) {
    ir.paths = 2;
    ir.p[2].clear();
    ir.p[3].clear();
  }
  // Normalize: unit energy into each output (white noise keeps its level).
  if (pb(s, CvNormalize)) {
    double eL = energy(ir.p[0]) + (ir.paths == 4 ? energy(ir.p[3]) : 0.0);
    double eR = energy(ir.p[1]) + (ir.paths == 4 ? energy(ir.p[2]) : 0.0);
    double e = 0.5 * (eL + eR);
    if (e > 1e-20) {
      float g = static_cast<float>(1.0 / std::sqrt(e));
      for (int p = 0; p < ir.paths; ++p)
        for (float& v : ir.p[p]) v *= g;
    }
  }
}

inline void buildKernel(const IrBuf& ir, ConvKernel& k) {
  k.rate = ir.rate;
  k.paths = ir.paths;
  const int capC = maxPartsC(ir.rate);
  k.len = static_cast<int>(std::min(ir.len(), static_cast<size_t>(kBC) * static_cast<size_t>(kStartC + capC)));
  auto h = [&](int p, long i) { return i < k.len ? ir.p[p][static_cast<size_t>(i)] : 0.f; };
  k.partsA = std::clamp((k.len - kHeadLen + kBA - 1) / kBA, 0, kPartsA);
  k.partsB = std::clamp((k.len - kBB * kStartB + kBB - 1) / kBB, 0, kMaxPartsB);
  k.partsC = std::clamp((k.len - kBC * kStartC + kBC - 1) / kBC, 0, capC);
  Fft fa, fb, fc;
  fa.init(2 * kBA);
  fb.init(2 * kBB);
  if (k.partsC > 0) fc.init(2 * kBC);
  std::vector<float> buf;
  for (int p = 0; p < k.paths; ++p) {
    for (int i = 0; i < kHeadLen; ++i) k.head[p][i] = h(p, i);
    auto part = [&](const Fft& f, int B, int kIdx, std::vector<float>& dst, int slot) {
      buf.assign(static_cast<size_t>(4 * B), 0.f);
      for (int i = 0; i < B; ++i) buf[static_cast<size_t>(2 * i)] = h(p, static_cast<long>(kIdx) * B + i);
      f.run(buf.data(), false);
      std::copy(buf.begin(), buf.begin() + 2 * (B + 1), dst.begin() + static_cast<long>(slot) * 2 * (B + 1));
    };
    k.specA[p].assign(static_cast<size_t>(k.partsA * 2 * (kBA + 1)), 0.f);
    for (int j = 0; j < k.partsA; ++j) part(fa, kBA, j + 1, k.specA[p], j);
    k.specB[p].assign(static_cast<size_t>(k.partsB * 2 * (kBB + 1)), 0.f);
    for (int j = 0; j < k.partsB; ++j) part(fb, kBB, j + kStartB, k.specB[p], j);
    k.specC[p].assign(static_cast<size_t>(k.partsC) * 2 * (kBC + 1), 0.f);
    for (int j = 0; j < k.partsC; ++j) part(fc, kBC, j + kStartC, k.specC[p], j);
  }
}

// ---------------------------------------------------------------- caching
struct IrCacheEntry {
  std::string key;
  bool reference = false;  // CvModel 1: buildForRate per engine rate
  PluginState state;       // the state the entry was built from (reference build)
  IrView view;
  IrBuf edited;  // at native rate
  std::map<long, std::shared_ptr<const IrData>> kernels;  // by rounded rate
};

inline std::mutex& irMutex() {
  static std::mutex m;
  return m;
}
inline std::list<IrCacheEntry>& irCache() {
  static std::list<IrCacheEntry> c;
  return c;
}

inline std::string irKey(const PluginState& s) {
  char buf[512];
  std::snprintf(buf, sizeof buf, "%d|%.4f|%.4f|%.3f|%.4f|%.3f|%.4f|%.4f|%d|%.5f|%.2f|%.2f|%.4f|%d|%d", pi(s, CvSource, SrcHall), pv(s, CvSize, 0.5f),
                pv(s, CvTone, 0.6f), pv(s, CvStart), pv(s, CvLength, 12), pv(s, CvFadeIn), pv(s, CvFadeOut, 0.1f), pv(s, CvTighten),
                pb(s, CvReverse) ? 1 : 0, pv(s, CvStretch, 1), pv(s, CvLowCut, 20), pv(s, CvHighCut, 20000), pv(s, CvSelfConv),
                pb(s, CvNormalize) ? 1 : 0, pi(s, CvStereo, 1));
  char ext[96];
  std::snprintf(ext, sizeof ext, "|%.6f|%d|%d|%d|%.7f", pv(s, CvEqMix, 1), pb(s, CvEnvOn) ? 1 : 0, pi(s, CvStretchMode, 0), pi(s, CvModel, 0),
                pv(s, CvStretch, 1));
  std::string d = s.data;
  if (pi(s, CvSource, SrcHall) != SrcFile) {  // generated spaces ignore the path line
    size_t nl = d.find('\n');
    d = nl == std::string::npos ? std::string() : d.substr(nl);
  }
  return std::string(buf) + ext + "|" + d;
}

// Finds or builds the cache entry for a state (caller holds irMutex()).
inline IrCacheEntry* irEntry(const PluginState& s) {
  std::string key = irKey(s);
  auto& c = irCache();
  for (auto it = c.begin(); it != c.end(); ++it)
    if (it->key == key) {
      c.splice(c.begin(), c, it);  // most recently used first
      return &c.front();
    }
  IrCacheEntry e;
  e.key = key;
  e.reference = pi(s, CvModel, 0) == 1;
  if (e.reference) e.state = s;
  int src = std::clamp(pi(s, CvSource, SrcHall), 0, SrcCount - 1);
  bool trueStereo = pi(s, CvStereo, 1) == 1;
  IrBuf raw;
  if (src == SrcFile) {
    const std::string path = parseConvData(s.data).path;
    SamplePtr smp = path.empty() ? nullptr : sampleCache().get(path);
    if (!smp || smp->frames() == 0) {
      e.view.error = path.empty() ? "No impulse file loaded" : "Could not read the impulse file";
    } else {
      raw.rate = smp->sampleRate;
      raw.paths = 2;
      raw.p[0] = smp->left;
      raw.p[1] = smp->right;
      size_t cap = static_cast<size_t>(kMaxIrSeconds * raw.rate);
      for (int p = 0; p < 2; ++p)
        if (raw.p[p].size() > cap) raw.p[p].resize(cap);
    }
  } else {
    // Generated spaces are cached separately so envelope / EQ edits stay cheap.
    char rk[96];
    std::snprintf(rk, sizeof rk, "%d|%.4f|%.4f|%d", src, pv(s, CvSize, 0.5f), pv(s, CvTone, 0.6f), trueStereo ? 1 : 0);
    static std::list<std::pair<std::string, IrBuf>> rawCache;
    auto it = std::find_if(rawCache.begin(), rawCache.end(), [&](const auto& x) { return x.first == rk; });
    if (it == rawCache.end()) {
      rawCache.emplace_front(rk, generateSpace(src, pv(s, CvSize, 0.5f), pv(s, CvTone, 0.6f), trueStereo));
      while (rawCache.size() > 4) rawCache.pop_back();
      it = rawCache.begin();
    }
    raw = it->second;
  }
  if (raw.len() > 0) {
    e.view.rawSeconds = static_cast<double>(raw.len()) / raw.rate;
    editIr(raw, s, kMaxIrSeconds);
    auto disp = std::make_shared<Sample>();
    disp->path = "IR";
    disp->sampleRate = raw.rate;
    disp->left = raw.p[0];
    disp->right = raw.p[1];
    e.view.ir = disp;
    e.view.paths = raw.paths;
    e.edited = std::move(raw);
  }
  c.push_front(std::move(e));
  while (c.size() > 12) c.pop_back();
  return &c.front();
}

inline std::shared_ptr<const IrData> irKernel(IrCacheEntry& e, double rate) {
  if (!e.view.ir) return nullptr;
  long rk = std::lround(rate);
  auto it = e.kernels.find(rk);
  if (it != e.kernels.end()) return it->second;
  IrBuf r = e.edited;
  if (e.reference) {
    ref::buildForRate(r, e.state, rate);
  } else {
    resampleIr(r, rate / r.rate);
    r.rate = rate;
  }
  auto d = std::make_shared<IrData>();
  d->path = kIrTag;
  d->sampleRate = rate;
  buildKernel(r, d->kernel);
  e.kernels[rk] = d;
  return d;
}

// ------------------------------------------------------------------ runtime
// One uniformly partitioned overlap-save stage over both input channels.
struct ConvStage {
  int B = 0, N = 0, kStart = 1, cap = 0;
  Fft fft;
  std::vector<float> in;   // last N input frames: L in re, R in im
  std::vector<float> fdl;  // cap slots × 2 channels × (B+1) complex
  std::vector<float> acc;  // 2 channels × (B+1) complex
  std::vector<float> work; // N complex
  std::vector<float> out;  // B frames × 2 (L, R) for the current block
  std::vector<float> gains;  // stage C: partition gains (tempo-synced length)
  int pos = 0, head = 0;   // sample position in block, newest FDL slot

  void init(int blk, int start, int maxParts) {
    B = blk;
    N = 2 * blk;
    kStart = start;
    cap = start - 1 + maxParts;
    fft.init(N);
    in.assign(static_cast<size_t>(2 * N), 0.f);
    fdl.assign(static_cast<size_t>(cap * 2 * 2 * (B + 1)), 0.f);
    acc.assign(static_cast<size_t>(2 * 2 * (B + 1)), 0.f);
    work.assign(static_cast<size_t>(2 * N), 0.f);
    out.assign(static_cast<size_t>(2 * B), 0.f);
    pos = head = 0;
  }
  void clear() {
    std::fill(in.begin(), in.end(), 0.f);
    std::fill(fdl.begin(), fdl.end(), 0.f);
    std::fill(out.begin(), out.end(), 0.f);
    std::fill(acc.begin(), acc.end(), 0.f);
    pos = head = 0;
  }
  float* slot(int s, int ch) { return fdl.data() + (static_cast<size_t>(s) * 2 + static_cast<size_t>(ch)) * 2 * static_cast<size_t>(B + 1); }
  int maxParts() const { return cap - kStart + 1; }

  // Spread mode (stage C): the multiply-adds for the next output block run a
  // slice per sample; the transforms run at the block boundary. Partition j
  // uses the input spectrum `kStart + j - 1` blocks older than the block that
  // completes at the next boundary, which is already available (kStart >= 2).
  void spreadMac(const std::vector<float>* spec, int paths, int parts, const float* gains, int j0, int j1) {
    float* al = acc.data();
    float* ar = acc.data() + 2 * (B + 1);
    for (int j = j0; j < j1 && j < parts; ++j) {
      float g = gains ? gains[j] : 1.f;
      if (g <= 0.f) continue;
      int age = kStart + j - 1;
      int sIdx = ((head + 1 - age) % cap + cap) % cap;
      const float* XL = slot(sIdx, 0);
      const float* XR = slot(sIdx, 1);
      const size_t off = static_cast<size_t>(j) * 2 * static_cast<size_t>(B + 1);
      auto mac = [&](float* dst, const float* X, const std::vector<float>& H) {
        const float* h = H.data() + off;
        for (int k = 0; k <= B; ++k) {
          float xr_ = X[2 * k], xi = X[2 * k + 1], hr = h[2 * k] * g, hi = h[2 * k + 1] * g;
          dst[2 * k] += xr_ * hr - xi * hi;
          dst[2 * k + 1] += xr_ * hi + xi * hr;
        }
      };
      mac(al, XL, spec[0]);
      mac(ar, XR, spec[1]);
      if (paths == 4) {
        mac(ar, XL, spec[2]);
        mac(al, XR, spec[3]);
      }
    }
  }
  void feedSpread(float il, float ir, const std::vector<float>* spec, int paths, int parts, const float* gains) {
    size_t w = static_cast<size_t>(2 * (B + pos));
    in[w] = il;
    in[w + 1] = ir;
    parts = spec ? std::min(parts, maxParts()) : 0;
    if (parts > 0) {
      int j0 = static_cast<int>(static_cast<long>(pos) * parts / B), j1 = static_cast<int>(static_cast<long>(pos + 1) * parts / B);
      spreadMac(spec, paths, parts, gains, j0, j1);
    }
    if (++pos < B) return;
    pos = 0;
    // Output block from the accumulated spectra.
    if (parts > 0) {
      const float* al = acc.data();
      const float* ar = acc.data() + 2 * (B + 1);
      for (int k = 0; k <= B; ++k) {
        float lr = al[2 * k], li = al[2 * k + 1], rr = ar[2 * k], ri = ar[2 * k + 1];
        work[static_cast<size_t>(2 * k)] = lr - ri;
        work[static_cast<size_t>(2 * k + 1)] = li + rr;
        if (k > 0 && k < B) {
          int nk = N - k;
          work[static_cast<size_t>(2 * nk)] = lr + ri;
          work[static_cast<size_t>(2 * nk + 1)] = -li + rr;
        }
      }
      fft.run(work.data(), true);
      const float inv = 1.f / static_cast<float>(N);
      for (int i = 0; i < B; ++i) {
        out[static_cast<size_t>(2 * i)] = flush(work[static_cast<size_t>(2 * (B + i))] * inv);
        out[static_cast<size_t>(2 * i + 1)] = flush(work[static_cast<size_t>(2 * (B + i) + 1)] * inv);
      }
    } else {
      std::fill(out.begin(), out.end(), 0.f);
    }
    std::fill(acc.begin(), acc.end(), 0.f);
    // Spectrum of the newest input window.
    std::copy(in.begin(), in.end(), work.begin());
    fft.run(work.data(), false);
    head = (head + 1) % cap;
    float* xl = slot(head, 0);
    float* xr = slot(head, 1);
    for (int k = 0; k <= B; ++k) {
      int nk = (N - k) % N;
      float zr = work[static_cast<size_t>(2 * k)], zi = work[static_cast<size_t>(2 * k + 1)];
      float cr = work[static_cast<size_t>(2 * nk)], ci = -work[static_cast<size_t>(2 * nk + 1)];
      xl[2 * k] = 0.5f * (zr + cr);
      xl[2 * k + 1] = 0.5f * (zi + ci);
      xr[2 * k] = 0.5f * (zi - ci);
      xr[2 * k + 1] = -0.5f * (zr - cr);
    }
    std::copy(in.begin() + 2 * B, in.end(), in.begin());
  }

  // Called when a block of B input samples is complete. `spec[p]` holds
  // `parts` partitions for partition indices kStart.. ; gains fade them.
  void block(const std::vector<float>* spec, int paths, int parts, const float* gains) {
    // Transform the last N inputs (two real signals packed in one complex FFT).
    std::copy(in.begin(), in.end(), work.begin());
    fft.run(work.data(), false);
    head = (head + 1) % cap;
    float* xl = slot(head, 0);
    float* xr = slot(head, 1);
    for (int k = 0; k <= B; ++k) {
      int nk = (N - k) % N;
      float zr = work[static_cast<size_t>(2 * k)], zi = work[static_cast<size_t>(2 * k + 1)];
      float cr = work[static_cast<size_t>(2 * nk)], ci = -work[static_cast<size_t>(2 * nk + 1)];
      xl[2 * k] = 0.5f * (zr + cr);
      xl[2 * k + 1] = 0.5f * (zi + ci);
      xr[2 * k] = 0.5f * (zi - ci);
      xr[2 * k + 1] = -0.5f * (zr - cr);
    }
    // Shift the input window by B.
    std::copy(in.begin() + 2 * B, in.end(), in.begin());
    // Multiply-accumulate.
    std::fill(acc.begin(), acc.end(), 0.f);
    float* al = acc.data();
    float* ar = acc.data() + 2 * (B + 1);
    parts = std::min(parts, cap - kStart + 1);
    for (int j = 0; j < parts; ++j) {
      float g = gains ? gains[j] : 1.f;
      if (g <= 0.f) continue;
      int age = kStart + j - 1;  // X_{c+1-k}
      int sIdx = ((head - age) % cap + cap) % cap;
      const float* XL = slot(sIdx, 0);
      const float* XR = slot(sIdx, 1);
      const size_t off = static_cast<size_t>(j) * 2 * static_cast<size_t>(B + 1);
      auto mac = [&](float* dst, const float* X, const std::vector<float>& H) {
        const float* h = H.data() + off;
        for (int k = 0; k <= B; ++k) {
          float xr_ = X[2 * k], xi = X[2 * k + 1], hr = h[2 * k] * g, hi = h[2 * k + 1] * g;
          dst[2 * k] += xr_ * hr - xi * hi;
          dst[2 * k + 1] += xr_ * hi + xi * hr;
        }
      };
      mac(al, XL, spec[0]);
      mac(ar, XR, spec[1]);
      if (paths == 4) {
        mac(ar, XL, spec[2]);
        mac(al, XR, spec[3]);
      }
    }
    // Pack Y = YL + i·YR over the full spectrum and invert.
    for (int k = 0; k <= B; ++k) {
      float lr = al[2 * k], li = al[2 * k + 1], rr = ar[2 * k], ri = ar[2 * k + 1];
      work[static_cast<size_t>(2 * k)] = lr - ri;
      work[static_cast<size_t>(2 * k + 1)] = li + rr;
      if (k > 0 && k < B) {
        int nk = N - k;  // conj(YL) + i·conj(YR)
        work[static_cast<size_t>(2 * nk)] = lr + ri;
        work[static_cast<size_t>(2 * nk + 1)] = -li + rr;
      }
    }
    fft.run(work.data(), true);
    const float inv = 1.f / static_cast<float>(N);
    for (int i = 0; i < B; ++i) {
      out[static_cast<size_t>(2 * i)] = flush(work[static_cast<size_t>(2 * (B + i))] * inv);
      out[static_cast<size_t>(2 * i + 1)] = flush(work[static_cast<size_t>(2 * (B + i) + 1)] * inv);
    }
  }
};

class ConvolverFx final : public Effect {
 public:
  ConvolverFx() {
    a_.init(kBA, 1, kPartsA);
    b_.init(kBB, kStartB, kMaxPartsB);
    c_ = new ConvStage();
    c_->init(kBC, kStartC, kDefPartsC);
    c_->gains.assign(static_cast<size_t>(kDefPartsC), 1.f);
    capC_.store(kDefPartsC);
    pre_[0].init(static_cast<size_t>(0.5 * kMaxRate) + 8);
    pre_[1].init(static_cast<size_t>(0.5 * kMaxRate) + 8);
  }
  ~ConvolverFx() override {
    delete c_;
    delete pending_.exchange(nullptr);
    delete retired_.exchange(nullptr);
  }
  ConvolverFx(const ConvolverFx&) = delete;
  ConvolverFx& operator=(const ConvolverFx&) = delete;

  // UI thread (Program build): grows stage C when the prepared kernel holds
  // more partitions than it fits. The new, empty stage is handed to the audio
  // thread lock-free; the replaced one comes back here to be freed.
  void prepareHost(const PluginState& s, double sampleRate) override {
    delete retired_.exchange(nullptr);
    int need = 0;
    {
      std::lock_guard<std::mutex> lk(irMutex());
      IrCacheEntry* e = irEntry(s);
      if (auto k = irKernel(*e, sampleRate)) need = k->kernel.partsC;
    }
    if (need <= capC_.load()) return;
    auto* st = new ConvStage();
    st->init(kBC, kStartC, need);
    st->gains.assign(static_cast<size_t>(need), 1.f);
    capC_.store(need);
    delete pending_.exchange(st);
  }
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    ChannelAux none;
    processAux(s, none, ctx, L, R, n);
  }
  void processAux(const PluginState& s, const ChannelAux& aux, const ProcessContext& ctx, float* L, float* R, int n) override {
    if (g_convRate.load(std::memory_order_relaxed) != ctx.sampleRate) g_convRate.store(ctx.sampleRate, std::memory_order_relaxed);
    // Adopt a grown stage C once the previous hand-back has been collected.
    if (pending_.load(std::memory_order_acquire) && !retired_.load(std::memory_order_acquire)) {
      if (ConvStage* p = pending_.exchange(nullptr, std::memory_order_acq_rel)) {
        retired_.store(c_, std::memory_order_release);
        c_ = p;
      }
    }
    const ConvKernel* k = nullptr;
    if (SamplePtr sp = aux.sample(0); sp && sp->path == kIrTag) k = &static_cast<const IrData*>(sp.get())->kernel;
    const double sr = ctx.sampleRate;
    const float dry = pv(s, CvDry, 1), wet = pv(s, CvWet, 0.35f);
    const float inW = pv(s, CvInWidth, 1), outW = pv(s, CvWidth, 1);
    const bool mono = pi(s, CvStereo, 1) == 2;
    double preD = std::max(0.0, pv(s, CvPreDelay, 0) * 0.001 * sr);
    if (pi(s, CvModel, 0) == 1) preD = std::nearbyint(preD);  // reference: whole samples
    // Tempo-synced length: fade the tail out partition by partition.
    const bool sync = computeGains(s, ctx, k);
    ConvStage& c = *c_;
    const float* gC = sync ? c.gains.data() : nullptr;
    for (int i = 0; i < n; ++i) {
      float xl = L[i], xr = R[i];
      float m = 0.5f * (xl + xr), sd = 0.5f * (xl - xr) * inW;
      float il = mono ? m : m + sd, ir = mono ? m : m - sd;
      if (preD >= 1.0) {
        float dl = pre_[0].read(preD), dr = pre_[1].read(preD);
        pre_[0].push(il);
        pre_[1].push(ir);
        il = dl;
        ir = dr;
      } else {
        pre_[0].push(il);
        pre_[1].push(ir);
      }
      // Head FIR over the input history (double-written ring for contiguous reads).
      hist_[0][hp_] = hist_[0][hp_ + kHeadLen] = il;
      hist_[1][hp_] = hist_[1][hp_ + kHeadLen] = ir;
      float yl = 0, yr = 0;
      if (k) {
        const float* hl = &hist_[0][hp_ + 1];  // oldest .. newest
        const float* hr = &hist_[1][hp_ + 1];
        for (int t = 0; t < kHeadLen; ++t) {
          int age = kHeadLen - 1 - t;
          yl += k->head[0][age] * hl[t];
          yr += k->head[1][age] * hr[t];
        }
        if (k->paths == 4)
          for (int t = 0; t < kHeadLen; ++t) {
            int age = kHeadLen - 1 - t;
            yr += k->head[2][age] * hl[t];
            yl += k->head[3][age] * hr[t];
          }
      }
      hp_ = (hp_ + 1) % kHeadLen;
      yl += a_.out[static_cast<size_t>(2 * a_.pos)] + b_.out[static_cast<size_t>(2 * b_.pos)] + c.out[static_cast<size_t>(2 * c.pos)];
      yr += a_.out[static_cast<size_t>(2 * a_.pos + 1)] + b_.out[static_cast<size_t>(2 * b_.pos + 1)] + c.out[static_cast<size_t>(2 * c.pos + 1)];
      feed(a_, il, ir, k ? k->specA : nullptr, k ? k->paths : 2, k ? k->partsA : 0, sync ? gainsA_ : nullptr);
      feed(b_, il, ir, k ? k->specB : nullptr, k ? k->paths : 2, k ? k->partsB : 0, sync ? gainsB_ : nullptr);
      c.feedSpread(il, ir, k ? k->specC : nullptr, k ? k->paths : 2, k ? k->partsC : 0, gC);
      float wm = 0.5f * (yl + yr), ws = 0.5f * (yl - yr) * outW;
      L[i] = flush(xl * dry + (wm + ws) * wet);
      R[i] = flush(xr * dry + (wm - ws) * wet);
    }
  }
  void reset() override {
    a_.clear();
    b_.clear();
    c_->clear();
    pre_[0].clear();
    pre_[1].clear();
    std::memset(hist_, 0, sizeof hist_);
    hp_ = 0;
  }
  // Partitions stage C currently holds (tests).
  int capacityC() const { return c_->maxParts(); }

 private:
  static void feed(ConvStage& st, float il, float ir, const std::vector<float>* spec, int paths, int parts, const float* gains) {
    size_t w = static_cast<size_t>(2 * (st.B + st.pos));
    st.in[w] = il;
    st.in[w + 1] = ir;
    if (++st.pos == st.B) {
      st.pos = 0;
      if (spec && parts > 0)
        st.block(spec, paths, parts, gains);
      else {
        std::copy(st.in.begin() + 2 * st.B, st.in.end(), st.in.begin());
        std::fill(st.out.begin(), st.out.end(), 0.f);
      }
    }
  }
  // Partition gains for the tempo-synced length; false when not synced (all 1).
  bool computeGains(const PluginState& s, const ProcessContext& ctx, const ConvKernel* k) {
    if (!pb(s, CvSync)) return false;
    int c = std::clamp(pi(s, CvSyncLen, 4), 0, 6);
    double lenSec = kConvSyncBeats[c] * 60.0 / std::max(1.0, ctx.tempo);
    double len = lenSec * (k ? k->rate : ctx.sampleRate), fade0 = len * 0.7;
    auto g = [&](double a, double b) -> float {
      double mid = 0.5 * (a + b);
      if (mid <= fade0) return 1.f;
      if (mid >= len) return 0.f;
      return static_cast<float>(0.5 + 0.5 * std::cos(kPi * (mid - fade0) / (len - fade0)));
    };
    for (int j = 0; j < kPartsA; ++j) gainsA_[j] = g((j + 1.0) * kBA, (j + 2.0) * kBA);
    for (int j = 0; j < kMaxPartsB; ++j) gainsB_[j] = g((j + kStartB) * 1.0 * kBB, (j + kStartB + 1.0) * kBB);
    std::vector<float>& gc = c_->gains;
    const int pc = std::min(static_cast<int>(gc.size()), k ? k->partsC : 0);
    for (int j = 0; j < pc; ++j) gc[static_cast<size_t>(j)] = g((j + kStartC) * 1.0 * kBC, (j + kStartC + 1.0) * kBC);
    return true;
  }

  ConvStage a_, b_;
  ConvStage* c_ = nullptr;
  std::atomic<ConvStage*> pending_{nullptr}, retired_{nullptr};
  std::atomic<int> capC_{0};
  Ring pre_[2];
  float hist_[2][2 * kHeadLen] = {};
  int hp_ = 0;
  float gainsA_[kPartsA] = {};
  float gainsB_[kMaxPartsB] = {};
};

}  // namespace gl::packs::spacefx::detail
