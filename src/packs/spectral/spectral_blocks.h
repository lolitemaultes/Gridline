// Gridline — "spectral" pack: DSP building blocks used by the pack's
// effects (FFT, biquads with all-pass/crossover designs, interpolated ring
// buffers). Header-only; nothing here allocates after construction.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace gl::packs::spectral::blk {

inline constexpr double kPi = 3.14159265358979323846;

// In-place iterative radix-2 complex FFT with precomputed tables.
class Fft {
 public:
  explicit Fft(int n) : n_(n), rev_(static_cast<size_t>(n)), cs_(static_cast<size_t>(n / 2)), sn_(static_cast<size_t>(n / 2)) {
    int bits = 0;
    while ((1 << bits) < n) ++bits;
    for (int i = 0; i < n; ++i) {
      int r = 0;
      for (int b = 0; b < bits; ++b)
        if (i & (1 << b)) r |= 1 << (bits - 1 - b);
      rev_[static_cast<size_t>(i)] = r;
    }
    for (int i = 0; i < n / 2; ++i) {
      cs_[static_cast<size_t>(i)] = static_cast<float>(std::cos(2.0 * kPi * i / n));
      sn_[static_cast<size_t>(i)] = static_cast<float>(std::sin(2.0 * kPi * i / n));
    }
  }
  int size() const { return n_; }
  // inverse = false: X[k] = sum x[n] e^{-i 2pi kn/N}; inverse = true: no 1/N scaling.
  void run(float* re, float* im, bool inverse) const {
    for (int i = 0; i < n_; ++i) {
      int j = rev_[static_cast<size_t>(i)];
      if (i < j) {
        std::swap(re[i], re[j]);
        std::swap(im[i], im[j]);
      }
    }
    const float sgn = inverse ? 1.0f : -1.0f;
    for (int len = 2; len <= n_; len <<= 1) {
      int half = len >> 1, step = n_ / len;
      for (int i = 0; i < n_; i += len) {
        for (int j = 0; j < half; ++j) {
          float wr = cs_[static_cast<size_t>(j * step)], wi = sgn * sn_[static_cast<size_t>(j * step)];
          int a = i + j, b = a + half;
          float xr = re[b] * wr - im[b] * wi, xi = re[b] * wi + im[b] * wr;
          re[b] = re[a] - xr;
          im[b] = im[a] - xi;
          re[a] += xr;
          im[a] += xi;
        }
      }
    }
  }

 private:
  int n_;
  std::vector<int> rev_;
  std::vector<float> cs_, sn_;
};

// Transposed direct form II biquad (double state), with the designs the
// crossovers and vocoder bands need.
struct Bq {
  double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  double z1 = 0, z2 = 0;
  void clear() { z1 = z2 = 0; }
  double process(double x) {
    double y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
  // |H(f)| of the current coefficients (for displays).
  double magnitude(double hz, double sr) const {
    double w = 2.0 * kPi * hz / sr;
    double c1 = std::cos(w), s1 = std::sin(w), c2 = std::cos(2 * w), s2 = std::sin(2 * w);
    double nr = b0 + b1 * c1 + b2 * c2, ni = -(b1 * s1 + b2 * s2);
    double dr = 1 + a1 * c1 + a2 * c2, di = -(a1 * s1 + a2 * s2);
    return std::sqrt((nr * nr + ni * ni) / std::max(1e-30, dr * dr + di * di));
  }
  void norm(double nb0, double nb1, double nb2, double na0, double na1, double na2) {
    b0 = nb0 / na0;
    b1 = nb1 / na0;
    b2 = nb2 / na0;
    a1 = na1 / na0;
    a2 = na2 / na0;
  }
  static double w0(double sr, double hz) { return 2.0 * kPi * std::clamp(hz, 5.0, sr * 0.495) / sr; }
  void lowpass(double sr, double hz, double q) {
    double w = w0(sr, hz), c = std::cos(w), al = std::sin(w) / (2 * q);
    norm((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
  }
  void highpass(double sr, double hz, double q) {
    double w = w0(sr, hz), c = std::cos(w), al = std::sin(w) / (2 * q);
    norm((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
  }
  void allpass(double sr, double hz, double q) {
    double w = w0(sr, hz), c = std::cos(w), al = std::sin(w) / (2 * q);
    norm(1 - al, -2 * c, 1 + al, 1 + al, -2 * c, 1 - al);
  }
  // Constant 0 dB peak band-pass.
  void bandpass(double sr, double hz, double q) {
    double w = w0(sr, hz), c = std::cos(w), al = std::sin(w) / (2 * q);
    norm(al, 0, -al, 1 + al, -2 * c, 1 - al);
  }
  // First-order all-pass (1 - s/wc) / (1 + s/wc), as a degenerate biquad.
  void allpass1(double sr, double hz) {
    double k = std::tan(w0(sr, hz) / 2), a = (k - 1) / (k + 1);
    b0 = a;
    b1 = 1;
    b2 = 0;
    a1 = a;
    a2 = 0;
  }
  void lowpass1(double sr, double hz) {
    double k = std::tan(w0(sr, hz) / 2);
    b0 = b1 = k / (1 + k);
    b2 = 0;
    a1 = (k - 1) / (k + 1);
    a2 = 0;
  }
  void highpass1(double sr, double hz) {
    double k = std::tan(w0(sr, hz) / 2);
    b0 = 1 / (1 + k);
    b1 = -b0;
    b2 = 0;
    a1 = (k - 1) / (k + 1);
    a2 = 0;
  }
};

// Linkwitz-Riley crossover of order 2, 4 or 8 (slope 12/24/48 dB per octave).
// lowpass + highpass sums to an all-pass (allpassOut) for every order; the
// 2nd-order high band is returned with inverted polarity so the sum stays flat.
struct LrCrossover {
  int sections = 0;  // biquads per side
  Bq lp[4], hp[4];
  void design(double sr, double hz, int order) {  // order 0: LR2, 1: LR4, 2: LR8
    if (order <= 0) {
      sections = 1;
      lp[0].lowpass(sr, hz, 0.5);
      hp[0].highpass(sr, hz, 0.5);
    } else if (order == 1) {
      sections = 2;
      for (int i = 0; i < 2; ++i) {
        lp[i].lowpass(sr, hz, std::sqrt(0.5));
        hp[i].highpass(sr, hz, std::sqrt(0.5));
      }
    } else {
      sections = 4;
      const double q[2] = {0.54119610014619698, 1.3065629648763766};
      for (int i = 0; i < 4; ++i) {
        lp[i].lowpass(sr, hz, q[i & 1]);
        hp[i].highpass(sr, hz, q[i & 1]);
      }
    }
    invertHigh = order <= 0;
  }
  void clear() {
    for (auto& b : lp) b.clear();
    for (auto& b : hp) b.clear();
  }
  void split(double x, double& lo, double& hi) {
    lo = x;
    hi = x;
    for (int i = 0; i < sections; ++i) {
      lo = lp[i].process(lo);
      hi = hp[i].process(hi);
    }
    if (invertHigh) hi = -hi;
  }
  bool invertHigh = false;
};

// The all-pass a Linkwitz-Riley crossover sums to (phase compensation for the
// bands that do not pass through that crossover).
struct LrAllpass {
  int sections = 0;
  Bq ap[2];
  void design(double sr, double hz, int order) {
    if (order <= 0) {
      sections = 1;
      ap[0].allpass1(sr, hz);
    } else if (order == 1) {
      sections = 1;
      ap[0].allpass(sr, hz, std::sqrt(0.5));
    } else {
      sections = 2;
      ap[0].allpass(sr, hz, 0.54119610014619698);
      ap[1].allpass(sr, hz, 1.3065629648763766);
    }
  }
  void clear() {
    for (auto& b : ap) b.clear();
  }
  double process(double x) {
    for (int i = 0; i < sections; ++i) x = ap[i].process(x);
    return x;
  }
};

// Power-of-two ring buffer with fractional reads (absolute sample indices).
struct Ring {
  std::vector<float> d;
  size_t mask = 0;
  void init(size_t minSize) {
    size_t n = 1;
    while (n < minSize) n <<= 1;
    d.assign(n, 0.0f);
    mask = n - 1;
  }
  void clear() { std::fill(d.begin(), d.end(), 0.0f); }
  size_t size() const { return d.size(); }
  float& at(int64_t i) { return d[static_cast<size_t>(i) & mask]; }
  float at(int64_t i) const { return d[static_cast<size_t>(i) & mask]; }
  float linear(double pos) const {
    double f = std::floor(pos);
    int64_t i = static_cast<int64_t>(f);
    float t = static_cast<float>(pos - f);
    float a = at(i), b = at(i + 1);
    return a + (b - a) * t;
  }
  // 4-point Hermite.
  float cubic(double pos) const {
    double f = std::floor(pos);
    int64_t i = static_cast<int64_t>(f);
    float t = static_cast<float>(pos - f);
    float xm1 = at(i - 1), x0 = at(i), x1 = at(i + 1), x2 = at(i + 2);
    float c1 = 0.5f * (x1 - xm1);
    float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
  }
};

inline float flush(float x) { return std::fabs(x) < 1e-20f ? 0.0f : x; }
inline double dbGain(double db) { return db <= -59.9 ? 0.0 : std::pow(10.0, db / 20.0); }
inline float toDb(double g) { return g > 1e-6 ? static_cast<float>(20.0 * std::log10(g)) : -120.0f; }

}  // namespace gl::packs::spectral::blk

// ------------------------------------------------------------------ J-1 additions
// (band splitter / vocoder reference models)
namespace gl::packs::spectral::blk {

// Butterworth section Q (k-th biquad of an order-n prototype).
inline double bwQ(int n, int k) { return 1.0 / (2.0 * std::sin((2 * k + 1) * kPi / (2.0 * n))); }

// Reference crossover slopes 6..96 dB: products of Butterworth prototypes.
struct SlopeDesign {
  int factors[2] = {1, 0};
  int nf = 1;
  bool invertHigh = false;
};
inline SlopeDesign slopeDesign(int idx) {
  static const SlopeDesign t[8] = {
      {{1, 0}, 1, false},  // 6 dB: first order (sums to 1)
      {{1, 1}, 2, true},   // 12 dB: LR2
      {{3, 0}, 1, false},  // 18 dB: Butterworth 3 (sums to an all-pass)
      {{2, 2}, 2, false},  // 24 dB: LR4
      {{4, 2}, 2, true},   // 36 dB: Butterworth 4 x Butterworth 2
      {{4, 4}, 2, false},  // 48 dB: LR8
      {{6, 6}, 2, false},  // 72 dB: LR12
      {{8, 8}, 2, false},  // 96 dB: LR16
  };
  return t[idx < 0 ? 0 : (idx > 7 ? 7 : idx)];
}
// Closed-form magnitudes of a slope's low / high parts at warped frequency
// ratio r = tan(w/2) / tan(wc/2) (identical to the IIR sections below).
inline void slopeMag(int idx, double r, double& lp, double& hp) {
  SlopeDesign d = slopeDesign(idx);
  double den = 1.0;
  int order = 0;
  for (int i = 0; i < d.nf; ++i) {
    den *= 1.0 + std::pow(r, 2.0 * d.factors[i]);
    order += d.factors[i];
  }
  lp = 1.0 / std::sqrt(den);
  hp = std::pow(r, order) * lp;
  if (!std::isfinite(hp)) hp = 1.0;
}

// Crossover of one reference slope: low / high outputs; lo + hi is all-pass
// (flat) for 6, 12, 18, 24, 48, 72 and 96 dB.
struct XoverRef {
  int sections = 0;
  Bq lp[8], hp[8];
  bool invertHigh = false;
  void design(double sr, double hz, int slope) {
    SlopeDesign d = slopeDesign(slope);
    invertHigh = d.invertHigh;
    sections = 0;
    for (int f = 0; f < d.nf; ++f) {
      int n = d.factors[f];
      if (n & 1) {
        lp[sections].lowpass1(sr, hz);
        hp[sections].highpass1(sr, hz);
        ++sections;
      }
      for (int k = 0; k < n / 2; ++k) {
        lp[sections].lowpass(sr, hz, bwQ(n, k));
        hp[sections].highpass(sr, hz, bwQ(n, k));
        ++sections;
      }
    }
  }
  void clear() {
    for (auto& b : lp) b.clear();
    for (auto& b : hp) b.clear();
  }
  void split(double x, double& lo, double& hi) {
    lo = x;
    hi = x;
    for (int i = 0; i < sections; ++i) {
      lo = lp[i].process(lo);
      hi = hp[i].process(hi);
    }
    if (invertHigh) hi = -hi;
  }
  // lo + hi (phase compensation for bands that bypass this crossover).
  double sum(double x) {
    double l, h;
    split(x, l, h);
    return l + h;
  }
};

// Radix-2 complex FFT of any power-of-two size up to a capacity fixed at
// construction (tables shared by all sizes). No allocation in run().
class FftVar {
 public:
  explicit FftVar(int cap = 2) : cap_(cap), cs_(static_cast<size_t>(cap / 2)), sn_(static_cast<size_t>(cap / 2)) {
    for (int i = 0; i < cap / 2; ++i) {
      cs_[static_cast<size_t>(i)] = std::cos(2.0 * kPi * i / cap);
      sn_[static_cast<size_t>(i)] = std::sin(2.0 * kPi * i / cap);
    }
  }
  int capacity() const { return cap_; }
  // inverse = false: X[k] = sum x[n] e^{-i 2pi kn/N}; no scaling either way.
  void run(float* re, float* im, int n, bool inverse) const {
    if (n < 2 || n > cap_) return;
    for (int i = 1, j = 0; i < n; ++i) {
      int bit = n >> 1;
      for (; j & bit; bit >>= 1) j ^= bit;
      j ^= bit;
      if (i < j) {
        std::swap(re[i], re[j]);
        std::swap(im[i], im[j]);
      }
    }
    const double sgn = inverse ? 1.0 : -1.0;
    for (int len = 2; len <= n; len <<= 1) {
      int half = len >> 1, step = cap_ / len;
      for (int i = 0; i < n; i += len)
        for (int j = 0; j < half; ++j) {
          float wr = static_cast<float>(cs_[static_cast<size_t>(j * step)]);
          float wi = static_cast<float>(sgn * sn_[static_cast<size_t>(j * step)]);
          int a = i + j, b = a + half;
          float xr = re[b] * wr - im[b] * wi, xi = re[b] * wi + im[b] * wr;
          re[b] = re[a] - xr;
          im[b] = im[a] - xi;
          re[a] += xr;
          im[a] += xi;
        }
    }
  }

 private:
  int cap_;
  std::vector<double> cs_, sn_;
};

}  // namespace gl::packs::spectral::blk

// ============================================================================
// J-1 reference blocks for the pitch corrector, pitch shifter and spreader.
// All buffers are sized in the constructors (up to 192 kHz); nothing below
// allocates while processing.
namespace gl::packs::spectral::blk {

// Deterministic 0..1 random numbers (linear congruential).
struct Lcg {
  uint32_t s = 0x2545F491u;
  double next() {
    s = s * 1664525u + 1013904223u;
    return static_cast<double>(s >> 8) * (1.0 / 16777216.0);
  }
};

// Filtered AMDF pitch detector. The input is smoothed by a one-pole and eight
// cascaded one-pole lowpasses (corners matching coefficients 0.4 / 0.125 at
// 44.1 kHz), decimated, and every N decimated samples (N = one period of the
// lowest accepted pitch) the average magnitude difference over N samples is
// evaluated for every lag. Normalised by its maximum, the first local dip
// under 0.3 (shortest lag first) is the period; no dip, or a pitch outside
// fmin..fmax, means unvoiced.
class AmdfDetector {
 public:
  static constexpr int kRing = 1 << 13, kMaxLag = 2048;
  AmdfDetector() : ring_(kRing, 0.f), d_(kMaxLag + 2, 0.f) {}
  void configure(double sr, double fmin, double fmax = 1000.0) {
    sr_ = sr;
    decim_ = std::max(1, static_cast<int>(sr / 16000.0));
    dsr_ = sr / decim_;
    fmin_ = fmin;
    fmax_ = fmax;
    n_ = std::clamp(static_cast<int>(std::ceil(dsr_ / fmin)) + 1, 8, kMaxLag - 2);
    minLag_ = std::max(2, static_cast<int>(std::floor(dsr_ / fmax)));
    preA_ = std::exp(-2.0 * kPi * 3585.0 / sr);
    lpc_ = 1.0 - std::exp(-2.0 * kPi * 937.0 / sr);
    clear();
  }
  void clear() {
    std::fill(ring_.begin(), ring_.end(), 0.f);
    w_ = 0;
    phase_ = 0;
    hop_ = 0;
    pre_ = 0;
    for (double& s : lp_) s = 0;
    voiced_ = false;
    period_ = 0;
    conf_ = 0;
  }
  // Feeds one input sample; true when a new estimate was made.
  bool push(float x) {
    pre_ = x + (pre_ - x) * preA_;
    double y = pre_;
    for (double& s : lp_) {
      s += (y - s) * lpc_;
      y = s;
    }
    if (++phase_ < decim_) return false;
    phase_ = 0;
    ring_[static_cast<size_t>(w_ & (kRing - 1))] = static_cast<float>(y);
    ++w_;
    if (++hop_ < n_ || w_ < 2 * n_ + 4) return false;
    hop_ = 0;
    analyse();
    return true;
  }
  bool voiced() const { return voiced_; }
  double period() const { return period_; }  // full-rate samples
  double hz() const { return voiced_ && period_ > 0 ? sr_ / period_ : 0.0; }
  double confidence() const { return conf_; }
  int window() const { return n_ * decim_; }  // full-rate samples between estimates

 private:
  float at(int64_t i) const { return ring_[static_cast<size_t>(i & (kRing - 1))]; }
  void analyse() {
    const int64_t t = w_ - 1;
    double e = 0;
    for (int j = 0; j < n_; ++j) e += static_cast<double>(at(t - j)) * at(t - j);
    if (std::sqrt(e / n_) < 2e-5) {
      voiced_ = false;
      conf_ = 0;
      return;
    }
    double mx = 1e-30;
    const int lo = minLag_ - 1, hi = n_ + 1;
    for (int tau = lo; tau <= hi; ++tau) {
      double s = 0;
      for (int j = 0; j < n_; ++j) s += std::fabs(at(t - j) - at(t - j - tau));
      d_[static_cast<size_t>(tau)] = static_cast<float>(s);
      mx = std::max(mx, s);
    }
    const float inv = static_cast<float>(1.0 / mx);
    for (int tau = lo; tau <= hi; ++tau) d_[static_cast<size_t>(tau)] *= inv;
    int best = -1;
    for (int tau = minLag_; tau <= n_; ++tau) {
      float v = d_[static_cast<size_t>(tau)];
      if (v < 0.3f && v <= d_[static_cast<size_t>(tau - 1)] && v < d_[static_cast<size_t>(tau + 1)]) {
        best = tau;
        break;
      }
    }
    if (best < 0) {
      voiced_ = false;
      conf_ = 0;
      return;
    }
    // V-shaped (AMDF) interpolation of the dip.
    double a = d_[static_cast<size_t>(best - 1)], b = d_[static_cast<size_t>(best)], c = d_[static_cast<size_t>(best + 1)];
    double slope = std::max(a - b, c - b), off = slope > 1e-12 ? (a - c) / (2.0 * slope) : 0.0;
    double p = (best + std::clamp(off, -0.5, 0.5)) * decim_;
    double f = sr_ / p;
    if (f < fmin_ * 0.97 || f > fmax_) {
      voiced_ = false;
      conf_ = 0;
      return;
    }
    period_ = p;
    conf_ = std::clamp(1.0 - b, 0.0, 1.0);
    voiced_ = true;
  }
  std::vector<float> ring_, d_;
  double sr_ = 48000, dsr_ = 16000, fmin_ = 80, fmax_ = 1000, preA_ = 0, lpc_ = 0, pre_ = 0, period_ = 0, conf_ = 0;
  double lp_[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  int decim_ = 3, n_ = 200, minLag_ = 16, phase_ = 0, hop_ = 0;
  int64_t w_ = 0;
  bool voiced_ = false;
};

// One PSOLA output voice: pitch ratio (or a fixed grain spacing for the
// absolute pitch modes), grain resampling (formant) factor and gain.
struct PsVoice {
  double ratio = 1, absHop = 0, g = 1, gain = 0;
};

// Streaming TD-PSOLA on one channel: pitch marks on the analysis signal (the
// largest peak 0.75..1.1 periods after the previous mark), two-period Hann
// grains re-emitted at the voice's spacing and resampled by its formant
// factor. Unvoiced input uses 256-sample grains every 128 samples (transparent).
class PsolaCh {
 public:
  static constexpr int kRing = 1 << 16, kMarks = 128, kHann = 4096, kVoices = 4;
  PsolaCh() {
    src_.init(kRing);
    ana_.init(kRing);
    out_.init(kRing);
    hann_.resize(kHann + 1);
    for (int i = 0; i <= kHann; ++i) hann_[static_cast<size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * i / kHann));
  }
  void configure(int tmax) {
    tmax_ = std::clamp(tmax, 160, kRing / 4 - 16);
    lat_ = 3 * tmax_ + 8;
    clear();
  }
  void clear() {
    src_.clear();
    ana_.clear();
    out_.clear();
    w_ = 0;
    head_ = count_ = 0;
    last_ = -1;
    for (double& s : syn_) s = -1;
  }
  int latency() const { return lat_; }
  // x: audio, a: analysis signal, T: period (samples) when voiced.
  float process(float x, float a, double T, bool voiced, const PsVoice* v, int nv) {
    src_.at(w_) = x;
    ana_.at(w_) = a;
    ++w_;
    marks(voiced ? std::clamp(T, 16.0, static_cast<double>(tmax_ - 2)) : 128.0, voiced);
    const int64_t c = w_ - lat_;
    const double ahead = static_cast<double>(c + tmax_ + 2);
    for (int k = 0; k < nv && k < kVoices; ++k) {
      if (v[k].gain < 1e-5 || count_ == 0) {
        syn_[k] = ahead;
        continue;
      }
      if (syn_[k] < static_cast<double>(c)) syn_[k] = ahead;
      while (syn_[k] <= ahead) {
        const Mark& m = marks_[nearest(syn_[k])];
        double g = m.voiced ? std::clamp(v[k].g, 0.25, 4.0) : 1.0;
        double half = m.voiced ? std::min(m.period / g, static_cast<double>(tmax_)) : 128.0;
        double hop = !m.voiced ? 128.0 : (v[k].absHop > 0 ? v[k].absHop : m.period / std::max(0.05, v[k].ratio));
        hop = std::max(hop, 8.0);
        grain(m.pos, syn_[k], half, g, v[k].gain * std::min(1.5, hop / half));
        syn_[k] += hop;
      }
    }
    float y = out_.at(c);
    out_.at(c) = 0;
    return y;
  }

 private:
  struct Mark {
    int64_t pos = 0;
    double period = 128;
    bool voiced = false;
  };
  void marks(double T, bool voiced) {
    if (last_ < 0) last_ = w_ - lat_;
    while (true) {
      int64_t lo = last_ + static_cast<int64_t>(T * 0.75), hi = last_ + static_cast<int64_t>(T * 1.1) + 1;
      int64_t pos = last_ + static_cast<int64_t>(T + 0.5);
      if ((voiced ? hi : pos) + 1 >= w_) break;
      if (voiced) {
        float bv = -1e30f;
        for (int64_t k = lo; k <= hi; ++k)
          if (ana_.at(k) > bv) {
            bv = ana_.at(k);
            pos = k;
          }
      }
      if (pos <= last_) pos = last_ + 1;
      Mark& m = marks_[head_];
      m.pos = pos;
      m.period = T;
      m.voiced = voiced;
      head_ = (head_ + 1) % kMarks;
      count_ = std::min(count_ + 1, kMarks);
      last_ = pos;
    }
  }
  int nearest(double pos) const {
    int best = 0;
    double bd = 1e300;
    for (int k = 0; k < count_; ++k) {
      int idx = (head_ - 1 - k + kMarks) % kMarks;
      double mp = static_cast<double>(marks_[idx].pos);
      double d = std::fabs(mp - pos);
      if (d < bd) {
        bd = d;
        best = idx;
      }
      if (mp < pos - 4.0 * tmax_) break;
    }
    return best;
  }
  void grain(int64_t src, double centre, double half, double g, double gain) {
    int64_t c0 = static_cast<int64_t>(std::ceil(centre - half)), c1 = static_cast<int64_t>(std::floor(centre + half));
    const double inv = kHann / (2.0 * half);
    for (int64_t k = c0; k <= c1; ++k) {
      double off = static_cast<double>(k) - centre;
      int wi = std::clamp(static_cast<int>((off + half) * inv), 0, kHann);
      out_.at(k) += static_cast<float>(src_.linear(static_cast<double>(src) + off * g) * hann_[static_cast<size_t>(wi)] * gain);
    }
  }
  Ring src_, ana_, out_;
  std::vector<float> hann_;
  Mark marks_[kMarks];
  int head_ = 0, count_ = 0, tmax_ = 512, lat_ = 1544;
  int64_t w_ = 0, last_ = -1;
  double syn_[kVoices] = {-1, -1, -1, -1};
};

// Period-jump delay-line shifter (up to four read heads on one buffer): each
// head moves at the pitch ratio; when it gets within 128 samples of the write
// head (or too far behind) it jumps by whole detected periods while the old
// head fades out linearly over 128 samples. 4-point Hermite reads.
class JumpShifter {
 public:
  static constexpr int kRing = 1 << 15, kVoices = 4, kFade = 128;
  JumpShifter() { buf_.init(kRing); }
  void clear() {
    buf_.clear();
    w_ = 0;
    for (auto& v : v_) v = Head{};
  }
  void push(float x) {
    buf_.at(w_) = x;
    ++w_;
  }
  // Reads head k at pitch ratio (0.5..2) with jumps of whole periods P.
  float voice(int k, double ratio, double P) {
    Head& h = v_[k];
    ratio = std::clamp(ratio, 0.5, 2.0);
    P = std::clamp(P, 16.0, 4096.0);
    const double jump = P * std::ceil(384.0 / P), minD = kFade + 4.0;
    if (h.d <= 0) h.d = minD + jump;
    h.d += 1.0 - ratio;
    if (h.fade > 0) h.od += 1.0 - ratio;
    if (h.d < minD) {
      h.od = h.d;
      h.d += jump;
      h.fade = kFade;
    } else if (h.d > std::min(minD + 2.0 * jump + 1.0, static_cast<double>(kRing - 8))) {
      h.od = h.d;
      h.d -= jump;
      h.fade = kFade;
    }
    const double base = static_cast<double>(w_ - 1);
    float y = buf_.cubic(base - h.d);
    if (h.fade > 0) {
      float t = static_cast<float>(h.fade) / kFade;
      y = y * (1.0f - t) + buf_.cubic(base - std::max(2.0, h.od)) * t;
      --h.fade;
    }
    return y;
  }
  double delay(int k) const { return v_[k].d; }

 private:
  struct Head {
    double d = 0, od = 0;
    int fade = 0;
  };
  Ring buf_;
  int64_t w_ = 0;
  Head v_[kVoices];
};

// Stereo granular shifter: a new grain every L / density samples (+ random
// spacing), read at the pitch ratio from behind the write head (room for an
// upward shift, + random offset), Hann window, up to 20 grains.
class GrainCloud {
 public:
  static constexpr int kRing = 1 << 17, kGrains = 20, kWin = 1024;
  GrainCloud() {
    l_.init(kRing);
    r_.init(kRing);
    win_.resize(kWin + 1);
    for (int i = 0; i <= kWin; ++i) win_[static_cast<size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * i / kWin));
  }
  void clear() {
    l_.clear();
    r_.clear();
    w_ = 0;
    next_ = 0;
    for (auto& g : g_) g.on = false;
    rng_ = Lcg{};
  }
  void process(float xl, float xr, float& yl, float& yr, double ratio, double len, double density, double jitter, double rnd) {
    l_.at(w_) = xl;
    r_.at(w_) = xr;
    ++w_;
    len = std::clamp(len, 16.0, kRing / 4.0);
    density = std::clamp(density, 1.0, 5.0);
    const double spacing = len / density;
    if (--next_ <= 0) {
      next_ += spacing + jitter * rng_.next() * spacing;
      for (auto& g : g_)
        if (!g.on) {
          double back = std::max(0.0, ratio - 1.0) * len + 5.0 + rnd * rng_.next() * len;
          g.pos = static_cast<double>(w_ - 1) - std::min(back, kRing - len * 2.0 - 8.0);
          g.inc = ratio;
          g.len = len;
          g.age = 0;
          g.on = true;
          break;
        }
    }
    double sl = 0, sr = 0;
    for (auto& g : g_) {
      if (!g.on) continue;
      double t = g.age / g.len;
      float wv = win_[static_cast<size_t>(std::clamp(static_cast<int>(t * kWin), 0, kWin))];
      sl += l_.cubic(g.pos) * wv;
      sr += r_.cubic(g.pos) * wv;
      g.pos += g.inc;
      if (++g.age >= g.len) g.on = false;
    }
    const double norm = 2.0 / density * (1.0 + 0.5 * jitter);
    yl = static_cast<float>(sl * norm);
    yr = static_cast<float>(sr * norm);
  }

 private:
  struct Grain {
    double pos = 0, inc = 1, len = 1, age = 0;
    bool on = false;
  };
  Ring l_, r_;
  std::vector<float> win_;
  Grain g_[kGrains];
  int64_t w_ = 0;
  double next_ = 0;
  Lcg rng_;
};

// Feedback-path tone + saturation (pitch shifter delay): 1st-order 40 Hz high
// pass, 1st-order 7 kHz low pass, rational soft clip.
struct FbTone {
  double hx = 0, hy = 0, ly = 0, ha = 0, la = 0;
  void design(double sr) {
    ha = std::exp(-2.0 * kPi * 40.0 / sr);
    la = 1.0 - std::exp(-2.0 * kPi * 7000.0 / sr);
  }
  void clear() { hx = hy = ly = 0; }
  double process(double x) {
    double h = x - hx + ha * hy;
    hx = x;
    hy = h;
    ly += (h - ly) * la;
    double c = std::clamp(ly, -3.0, 3.0);
    return c * (27.0 + c * c) / (27.0 + 9.0 * c * c);
  }
};

}  // namespace gl::packs::spectral::blk
