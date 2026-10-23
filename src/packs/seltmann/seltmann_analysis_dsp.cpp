// Gridline — "seltmann" pack: audio analysis. Tempo, beats, meter, downbeats,
// sections, loopable phrases and key of a whole song; peaks for the views;
// background jobs with progress and a cache.
//
// Pipeline (see the pack plan):
//   mono mix -> decimate to ~22 kHz -> STFT (1024, hop rate/100)
//   -> log magnitude -> 8-band flux (superflux-style max filter), band-normalised
//   -> onset strength (local mean removed) -> tempogram (windowed ACF) with a
//   tempo prior and octave scoring -> local period curve -> dynamic-programming
//   beat tracker -> attack refinement -> regression BPM
//   -> per-beat kick / onset / harmonic-change features -> meter + downbeat
//   -> bar features (chroma + timbre) -> self-similarity -> novelty sections
//   -> labels by aligned similarity -> loop spots -> key.
#include "seltmann.h"
#include "../../engine/TimeStretch.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <numeric>
#include <sstream>
#include <thread>

namespace gl::packs::seltmann {

namespace fs = std::filesystem;

namespace {

constexpr double kPi = 3.14159265358979323846;
// Chroma peak-to-mean under which a short loop counts as drums (a break), tuned on sample packs.
constexpr double kDrumTonality = 2.6;
// Bumped when the analysis changes: older disk-cache entries are not reused.
constexpr const char* kAnalysisVersion = "seltmann-analysis-2";

// ------------------------------------------------------------------ FFT
// In-place iterative radix-2 complex FFT (n a power of two).
class Fft {
 public:
  explicit Fft(int n) : n_(n), cs_(static_cast<size_t>(n / 2)), sn_(static_cast<size_t>(n / 2)), rev_(static_cast<size_t>(n)) {
    for (int i = 0; i < n / 2; ++i) {
      cs_[static_cast<size_t>(i)] = std::cos(2 * kPi * i / n);
      sn_[static_cast<size_t>(i)] = -std::sin(2 * kPi * i / n);
    }
    int bits = 0;
    while ((1 << bits) < n) ++bits;
    for (int i = 0; i < n; ++i) {
      int r = 0;
      for (int b = 0; b < bits; ++b)
        if (i & (1 << b)) r |= 1 << (bits - 1 - b);
      rev_[static_cast<size_t>(i)] = r;
    }
  }
  int size() const { return n_; }
  void run(std::vector<double>& re, std::vector<double>& im) const {
    for (int i = 0; i < n_; ++i) {
      const int r = rev_[static_cast<size_t>(i)];
      if (r > i) {
        std::swap(re[static_cast<size_t>(i)], re[static_cast<size_t>(r)]);
        std::swap(im[static_cast<size_t>(i)], im[static_cast<size_t>(r)]);
      }
    }
    for (int len = 2; len <= n_; len <<= 1) {
      const int half = len >> 1, step = n_ / len;
      for (int i = 0; i < n_; i += len)
        for (int k = 0; k < half; ++k) {
          const double wr = cs_[static_cast<size_t>(k * step)], wi = sn_[static_cast<size_t>(k * step)];
          const size_t a = static_cast<size_t>(i + k), b = static_cast<size_t>(i + k + half);
          const double tr = re[b] * wr - im[b] * wi, ti = re[b] * wi + im[b] * wr;
          re[b] = re[a] - tr;
          im[b] = im[a] - ti;
          re[a] += tr;
          im[a] += ti;
        }
    }
  }

 private:
  int n_;
  std::vector<double> cs_, sn_;
  std::vector<int> rev_;
};

struct Progress {
  std::atomic<float>* p = nullptr;
  const std::atomic<bool>* cancel = nullptr;
  float a = 0, b = 1;
  void set(float t) const {
    if (p) p->store(a + (b - a) * std::clamp(t, 0.0f, 1.0f), std::memory_order_relaxed);
  }
  bool cancelled() const { return cancel && cancel->load(std::memory_order_relaxed); }
};

// ------------------------------------------------------------------ signal prep
std::vector<float> monoMix(const Sample& s) {
  std::vector<float> m(s.frames());
  const bool mono = s.mono() || s.right.size() != s.left.size();
  for (size_t i = 0; i < m.size(); ++i) m[i] = mono ? s.left[i] : 0.5f * (s.left[i] + s.right[i]);
  return m;
}

// Decimates by D with a windowed-sinc low-pass (cut-off just under the new Nyquist).
std::vector<float> decimate(const std::vector<float>& x, int D) {
  if (D <= 1) return x;
  const int half = 16 * D;
  std::vector<double> h(static_cast<size_t>(2 * half + 1));
  const double fc = 0.45 / D;  // of the input rate
  double sum = 0;
  for (int k = -half; k <= half; ++k) {
    const double t = k;
    const double sinc = k == 0 ? 2 * fc : std::sin(2 * kPi * fc * t) / (kPi * t);
    const double w = 0.42 + 0.5 * std::cos(kPi * t / (half + 1)) + 0.08 * std::cos(2 * kPi * t / (half + 1));
    h[static_cast<size_t>(k + half)] = sinc * w;
    sum += sinc * w;
  }
  for (auto& v : h) v /= sum;
  const int64_t n = static_cast<int64_t>(x.size());
  std::vector<float> y(static_cast<size_t>(n / D + 1));
  for (int64_t j = 0; j < static_cast<int64_t>(y.size()); ++j) {
    const int64_t c = j * D;
    double acc = 0;
    const int64_t k0 = std::max<int64_t>(-half, -c), k1 = std::min<int64_t>(half, n - 1 - c);
    for (int64_t k = k0; k <= k1; ++k) acc += h[static_cast<size_t>(k + half)] * x[static_cast<size_t>(c + k)];
    y[static_cast<size_t>(j)] = static_cast<float>(acc);
  }
  return y;
}

int decimationFor(double rate) { return std::max(1, static_cast<int>(std::floor(rate / 20000.0))); }

// ------------------------------------------------------------------ spectral features
constexpr int kBands = 8;
constexpr double kBandEdges[kBands + 1] = {30, 120, 250, 500, 1000, 2000, 4000, 8000, 11000};
constexpr int kMel = 24;

struct Frames {
  int count = 0;
  double rate = 100;     // frames per second
  int hop = 0;           // in decimated samples
  double ra = 22050;     // decimated rate
  int D = 1;
  std::vector<float> onset;           // onset strength (normalised)
  std::vector<float> band[kBands];    // per-band flux (normalised to mean 1)
  std::vector<float> mel;             // count * kMel log energies
  std::vector<float> rms;             // per frame
};

// STFT -> per-band flux + mel energies.
void spectralFrames(const std::vector<float>& y, double ra, Frames& F, const Progress& pr) {
  const int N = 1024;
  F.ra = ra;
  F.hop = std::max(1, static_cast<int>(std::lround(ra / 100.0)));
  F.rate = ra / F.hop;
  const int64_t n = static_cast<int64_t>(y.size());
  F.count = static_cast<int>(n / F.hop) + 1;
  Fft fft(N);
  std::vector<double> win(N), re(N), im(N);
  double wsum = 0;
  for (int i = 0; i < N; ++i) wsum += win[static_cast<size_t>(i)] = 0.5 - 0.5 * std::cos(2 * kPi * i / N);
  const int bins = N / 2 + 1;
  // bin -> band, bin -> mel
  std::vector<int> bandOf(static_cast<size_t>(bins), -1);
  for (int k = 0; k < bins; ++k) {
    const double f = k * ra / N;
    for (int b = 0; b < kBands; ++b)
      if (f >= kBandEdges[b] && f < kBandEdges[b + 1]) bandOf[static_cast<size_t>(k)] = b;
  }
  auto hzToMel = [](double f) { return 2595.0 * std::log10(1.0 + f / 700.0); };
  const double m0 = hzToMel(40), m1 = hzToMel(std::min(10000.0, ra * 0.5 - 100));
  std::vector<double> melEdge(kMel + 2);
  for (int i = 0; i < kMel + 2; ++i) melEdge[static_cast<size_t>(i)] = 700.0 * (std::pow(10.0, (m0 + (m1 - m0) * i / (kMel + 1)) / 2595.0) - 1.0);
  for (auto& v : F.band) v.assign(static_cast<size_t>(F.count), 0.f);
  F.mel.assign(static_cast<size_t>(F.count) * kMel, 0.f);
  F.rms.assign(static_cast<size_t>(F.count), 0.f);
  std::vector<double> prev(static_cast<size_t>(bins), 0.0), cur(static_cast<size_t>(bins), 0.0), prevMax(static_cast<size_t>(bins), 0.0);
  const double scale = 2.0 / wsum;
  for (int t = 0; t < F.count; ++t) {
    if ((t & 255) == 0) {
      pr.set(static_cast<float>(t) / F.count);
      if (pr.cancelled()) return;
    }
    const int64_t c = static_cast<int64_t>(t) * F.hop - N / 2;
    double e = 0;
    for (int i = 0; i < N; ++i) {
      const int64_t j = c + i;
      const double v = j >= 0 && j < n ? y[static_cast<size_t>(j)] : 0.0;
      e += v * v;
      re[static_cast<size_t>(i)] = v * win[static_cast<size_t>(i)];
      im[static_cast<size_t>(i)] = 0;
    }
    F.rms[static_cast<size_t>(t)] = static_cast<float>(std::sqrt(e / N));
    fft.run(re, im);
    double melAcc[kMel] = {};
    for (int k = 0; k < bins; ++k) {
      const double mag = std::sqrt(re[static_cast<size_t>(k)] * re[static_cast<size_t>(k)] + im[static_cast<size_t>(k)] * im[static_cast<size_t>(k)]) * scale;
      cur[static_cast<size_t>(k)] = std::log1p(100.0 * mag);
      const double f = k * ra / N;
      for (int m = 0; m < kMel; ++m) {
        const double lo = melEdge[static_cast<size_t>(m)], mid = melEdge[static_cast<size_t>(m + 1)], hi = melEdge[static_cast<size_t>(m + 2)];
        if (f <= lo || f >= hi) continue;
        const double w = f < mid ? (f - lo) / (mid - lo) : (hi - f) / (hi - mid);
        melAcc[m] += w * mag * mag;
      }
    }
    for (int m = 0; m < kMel; ++m) F.mel[static_cast<size_t>(t) * kMel + static_cast<size_t>(m)] = static_cast<float>(std::log(1e-9 + melAcc[m]));
    if (t > 0) {
      double bf[kBands] = {};
      for (int k = 0; k < bins; ++k) {
        const int b = bandOf[static_cast<size_t>(k)];
        if (b < 0) continue;
        const double d = cur[static_cast<size_t>(k)] - prevMax[static_cast<size_t>(k)];
        if (d > 0) bf[b] += d;
      }
      for (int b = 0; b < kBands; ++b) F.band[b][static_cast<size_t>(t)] = static_cast<float>(bf[b]);
    }
    // superflux: compare against the previous frame max-filtered over 3 bins (vibrato robust)
    for (int k = 0; k < bins; ++k) {
      double m = cur[static_cast<size_t>(k)];
      if (k > 0) m = std::max(m, cur[static_cast<size_t>(k - 1)]);
      if (k + 1 < bins) m = std::max(m, cur[static_cast<size_t>(k + 1)]);
      prevMax[static_cast<size_t>(k)] = m;
    }
    std::swap(prev, cur);
  }
  // band normalisation + onset strength
  F.onset.assign(static_cast<size_t>(F.count), 0.f);
  for (int b = 0; b < kBands; ++b) {
    double mean = 0;
    for (float v : F.band[b]) mean += v;
    mean = mean / std::max(1, F.count) + 1e-6;
    for (auto& v : F.band[b]) v = static_cast<float>(v / mean);
    static const float kBandWeight[kBands] = {1.8f, 1.5f, 1.0f, 1.0f, 1.0f, 0.9f, 0.7f, 0.6f};  // kicks lead the beat, off-beat hats less
    for (int t = 0; t < F.count; ++t) F.onset[static_cast<size_t>(t)] += kBandWeight[b] * F.band[b][static_cast<size_t>(t)];
  }
}

// x minus its moving average over +-w, half-wave rectified, then scaled to unit std.
void detrend(std::vector<float>& x, int w) {
  const int n = static_cast<int>(x.size());
  if (n == 0) return;
  std::vector<double> cs(static_cast<size_t>(n + 1), 0.0);
  for (int i = 0; i < n; ++i) cs[static_cast<size_t>(i + 1)] = cs[static_cast<size_t>(i)] + x[static_cast<size_t>(i)];
  std::vector<float> y(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    const int a = std::max(0, i - w), b = std::min(n, i + w + 1);
    const double m = (cs[static_cast<size_t>(b)] - cs[static_cast<size_t>(a)]) / (b - a);
    y[static_cast<size_t>(i)] = static_cast<float>(std::max(0.0, x[static_cast<size_t>(i)] - m));
  }
  double s2 = 0;
  for (float v : y) s2 += static_cast<double>(v) * v;
  const double sd = std::sqrt(s2 / n) + 1e-9;
  for (auto& v : y) v = static_cast<float>(v / sd);
  x.swap(y);
}

// ------------------------------------------------------------------ tempo
struct Tempogram {
  int maxLag = 0;
  std::vector<double> global;                 // normalised ACF, lag 0..maxLag
  std::vector<std::vector<double>> windows;   // per window
  std::vector<double> windowCentre;           // frame index of each window centre
  std::vector<double> windowWeight;
};

void acf(const float* x, int n, int maxLag, std::vector<double>& r) {
  r.assign(static_cast<size_t>(maxLag + 1), 0.0);
  for (int lag = 0; lag <= maxLag && lag < n; ++lag) {
    double s = 0;
    for (int i = lag; i < n; ++i) s += static_cast<double>(x[i]) * x[i - lag];
    r[static_cast<size_t>(lag)] = s / (n - lag);
  }
}

Tempogram tempogram(const std::vector<float>& O, double fps) {
  Tempogram T;
  const int n = static_cast<int>(O.size());
  T.maxLag = std::min(n - 1, static_cast<int>(std::ceil(fps * 60.0 / 25.0)));  // down to 25 BPM (harmonics of 100 BPM lags)
  if (T.maxLag < 4) return T;
  const int W = static_cast<int>(fps * 8.0), H = static_cast<int>(fps * 2.0);
  T.global.assign(static_cast<size_t>(T.maxLag + 1), 0.0);
  double wsum = 0;
  std::vector<double> r;
  auto addWindow = [&](int a, int len) {
    acf(O.data() + a, len, std::min(T.maxLag, len - 1), r);
    r.resize(static_cast<size_t>(T.maxLag + 1), 0.0);
    double e = r[0];
    if (e <= 1e-12) e = 1e-12;
    std::vector<double> rn(r.size());
    for (size_t k = 0; k < r.size(); ++k) rn[k] = r[k] / e;
    const double w = std::sqrt(e);
    for (size_t k = 0; k < r.size(); ++k) T.global[k] += w * rn[k];
    wsum += w;
    T.windows.push_back(std::move(rn));
    T.windowCentre.push_back(a + len * 0.5);
    T.windowWeight.push_back(w);
  };
  if (n <= W + H) {
    addWindow(0, n);
  } else {
    for (int a = 0; a + W <= n; a += H) addWindow(a, W);
  }
  for (auto& v : T.global) v /= std::max(1e-12, wsum);
  return T;
}

double lagValue(const std::vector<double>& r, double lag) {
  if (lag < 0 || lag >= static_cast<double>(r.size()) - 1) return 0;
  const auto i = static_cast<size_t>(lag);
  const double f = lag - static_cast<double>(i);
  return r[i] * (1 - f) + r[i + 1] * f;
}

// Peak-enhanced ACF strength around a lag (max over +-1 frame, interpolated).
double lagPeak(const std::vector<double>& r, double lag) {
  double best = 0;
  for (double d = -1.0; d <= 1.0; d += 0.25) best = std::max(best, lagValue(r, lag + d));
  return best;
}

// Tempo salience of a BPM: ACF at the period plus its harmonics (duple metre).
double salience(const std::vector<double>& r, double fps, double bpm) {
  const double tau = fps * 60.0 / bpm;
  return lagPeak(r, tau) + 0.5 * lagPeak(r, 2 * tau) + 0.25 * lagPeak(r, 0.5 * tau) + 0.25 * lagPeak(r, 4 * tau);
}

double tempoPrior(double bpm) {
  const double o = std::log2(bpm / 120.0);
  return std::exp(-0.5 * (o / 0.9) * (o / 0.9));
}

struct TempoPick {
  double bpm = 120, confidence = 0, halfScore = 0, doubleScore = 0;
};

TempoPick pickTempo(const Tempogram& T, double fps) {
  TempoPick p;
  if (T.global.empty()) return p;
  double best = -1, bestBpm = 120;
  for (double bpm = 50; bpm <= 220; bpm += 0.25) {
    const double s = salience(T.global, fps, bpm) * tempoPrior(bpm);
    if (s > best) best = s, bestBpm = bpm;
  }
  // refine: parabolic peak of the ACF near the period
  {
    const double tau = fps * 60.0 / bestBpm;
    const int i = static_cast<int>(std::lround(tau));
    if (i > 1 && i + 1 < static_cast<int>(T.global.size())) {
      int m = i;
      for (int k = i - 2; k <= i + 2; ++k)
        if (k > 0 && k + 1 < static_cast<int>(T.global.size()) && T.global[static_cast<size_t>(k)] > T.global[static_cast<size_t>(m)]) m = k;
      const double a = T.global[static_cast<size_t>(m - 1)], b = T.global[static_cast<size_t>(m)], c = T.global[static_cast<size_t>(m + 1)];
      const double den = a - 2 * b + c;
      const double off = std::fabs(den) > 1e-12 ? std::clamp(0.5 * (a - c) / den, -0.5, 0.5) : 0.0;
      const double t2 = m + off;
      if (std::fabs(t2 - tau) < 2.5) bestBpm = fps * 60.0 / t2;
    }
  }
  while (bestBpm < 60) bestBpm *= 2;
  while (bestBpm > 200) bestBpm /= 2;
  auto sc = [&](double b) { return b < 30 || b > 400 ? 0.0 : salience(T.global, fps, b) * tempoPrior(b); };
  const double s0 = sc(bestBpm);
  double rival = 0;
  for (double f : {0.5, 2.0, 2.0 / 3.0, 1.5})
    if (bestBpm * f >= 60 && bestBpm * f <= 200) rival = std::max(rival, sc(bestBpm * f));
  p.bpm = bestBpm;
  p.confidence = s0 > 0 ? std::clamp((s0 - rival) / s0 * 1.6, 0.0, 1.0) : 0.0;
  p.halfScore = s0 > 0 ? std::clamp(sc(bestBpm * 0.5) / s0, 0.0, 1.0) : 0.0;
  p.doubleScore = s0 > 0 ? std::clamp(sc(bestBpm * 2.0) / s0, 0.0, 1.0) : 0.0;
  return p;
}

// Local beat period (frames) per envelope frame, following drift / tempo changes near the global period.
std::vector<double> localPeriod(const Tempogram& T, double tau0, int n) {
  std::vector<double> per(static_cast<size_t>(n), tau0);
  if (T.windows.size() < 2) return per;
  std::vector<double> wt(T.windows.size(), tau0);
  for (size_t w = 0; w < T.windows.size(); ++w) {
    const auto& r = T.windows[w];
    double best = -1, bt = tau0;
    for (double t = tau0 * 0.88; t <= tau0 * 1.12; t += 0.05) {
      const double v = lagValue(r, t) + 0.5 * lagValue(r, 2 * t);
      if (v > best) best = v, bt = t;
    }
    wt[w] = best > 0.02 ? bt : tau0;
  }
  // median of three, then linear interpolation between window centres
  std::vector<double> sm(wt);
  for (size_t w = 1; w + 1 < wt.size(); ++w) {
    double a = wt[w - 1], b = wt[w], c = wt[w + 1];
    sm[w] = std::max(std::min(a, b), std::min(std::max(a, b), c));
  }
  for (int i = 0; i < n; ++i) {
    const double x = i;
    size_t k = 0;
    while (k + 1 < T.windowCentre.size() && T.windowCentre[k + 1] < x) ++k;
    if (x <= T.windowCentre.front()) per[static_cast<size_t>(i)] = sm.front();
    else if (k + 1 >= T.windowCentre.size()) per[static_cast<size_t>(i)] = sm.back();
    else {
      const double f = (x - T.windowCentre[k]) / (T.windowCentre[k + 1] - T.windowCentre[k]);
      per[static_cast<size_t>(i)] = sm[k] * (1 - f) + sm[k + 1] * f;
    }
  }
  return per;
}

// Ellis-style dynamic programming beat tracker with a local period curve.
std::vector<double> trackBeats(const std::vector<float>& O, const std::vector<double>& period, double tightness) {
  const int n = static_cast<int>(O.size());
  std::vector<double> C(static_cast<size_t>(n), 0.0);
  std::vector<int> P(static_cast<size_t>(n), -1);
  for (int t = 0; t < n; ++t) {
    const double tau = period[static_cast<size_t>(t)];
    const int lo = static_cast<int>(std::floor(t - 2.0 * tau)), hi = static_cast<int>(std::ceil(t - 0.5 * tau));
    double best = -1e300;
    int arg = -1;
    for (int u = std::max(0, lo); u <= hi && u < t; ++u) {
      const double l = std::log((t - u) / tau);
      const double v = C[static_cast<size_t>(u)] - tightness * l * l;
      if (v > best) best = v, arg = u;
    }
    C[static_cast<size_t>(t)] = O[static_cast<size_t>(t)] + (arg >= 0 ? best : 0.0);
    P[static_cast<size_t>(t)] = arg;
  }
  // end: best score over the last period
  const double tauEnd = period.empty() ? 50.0 : period.back();
  int end = n - 1;
  double bc = -1e300;
  for (int t = std::max(0, n - 1 - static_cast<int>(1.5 * tauEnd)); t < n; ++t)
    if (C[static_cast<size_t>(t)] > bc) bc = C[static_cast<size_t>(t)], end = t;
  std::vector<double> beats;
  for (int t = end; t >= 0; t = P[static_cast<size_t>(t)]) {
    beats.push_back(t);
    if (P[static_cast<size_t>(t)] < 0) break;
  }
  std::reverse(beats.begin(), beats.end());
  // extend with the period to both ends of the file
  if (!beats.empty()) {
    while (beats.front() - period.front() >= 0) beats.insert(beats.begin(), beats.front() - period[static_cast<size_t>(std::max(0.0, beats.front()))]);
    while (beats.back() + period.back() < n) beats.push_back(beats.back() + period[static_cast<size_t>(std::min<double>(n - 1, beats.back()))]);
  }
  return beats;
}

// Parabolic refinement of a beat on the onset envelope (only at a local maximum).
double refineOnEnvelope(const std::vector<float>& O, double t) {
  const int i = static_cast<int>(std::lround(t));
  if (i <= 0 || i + 1 >= static_cast<int>(O.size())) return t;
  int m = i;
  if (O[static_cast<size_t>(i - 1)] > O[static_cast<size_t>(m)]) m = i - 1;
  if (O[static_cast<size_t>(i + 1)] > O[static_cast<size_t>(m)]) m = i + 1;
  if (m <= 0 || m + 1 >= static_cast<int>(O.size())) return t;
  const double a = O[static_cast<size_t>(m - 1)], b = O[static_cast<size_t>(m)], c = O[static_cast<size_t>(m + 1)];
  if (b < a || b < c || b < 0.5) return t;
  const double den = a - 2 * b + c;
  return m + (std::fabs(den) > 1e-12 ? std::clamp(0.5 * (a - c) / den, -0.5, 0.5) : 0.0);
}

// Time-domain attack refinement: the start of the sharpest energy rise within
// +-radius of the beat (full-rate mono), when it is a clear hit.
double refineAttack(const std::vector<float>& x, double rate, double frame) {
  const int64_t n = static_cast<int64_t>(x.size());
  const int blk = std::max(8, static_cast<int>(rate * 0.001));  // 1 ms
  const int64_t radius = static_cast<int64_t>(rate * 0.030);
  const int64_t a = static_cast<int64_t>(frame) - radius - 8 * blk, b = static_cast<int64_t>(frame) + radius;
  if (a < 0 || b + blk >= n) return frame;
  const int nb = static_cast<int>((b - a) / blk);
  std::vector<double> e(static_cast<size_t>(nb), 0.0);
  float prev = x[static_cast<size_t>(a)];
  for (int k = 0; k < nb; ++k) {
    double s = 0;
    for (int j = 0; j < blk; ++j) {
      const float v = x[static_cast<size_t>(a + k * blk + j)];
      const float d = v - prev;  // first difference: a simple high-pass
      prev = v;
      s += static_cast<double>(d) * d + 0.05 * static_cast<double>(v) * v;
    }
    e[static_cast<size_t>(k)] = s / blk + 1e-12;
  }
  int bestK = -1;
  double bestR = 0;
  for (int k = 8; k < nb; ++k) {
    double before = 0;
    for (int j = k - 8; j < k - 2; ++j) before += e[static_cast<size_t>(j)];
    before /= 6;
    const double r = e[static_cast<size_t>(k)] / before;
    if (r > bestR) bestR = r, bestK = k;
  }
  if (bestK < 0 || bestR < 6.0) return frame;
  // first block of the rise
  int k = bestK;
  while (k > 8 && e[static_cast<size_t>(k - 1)] > e[static_cast<size_t>(bestK)] * 0.15) --k;
  return static_cast<double>(a + static_cast<int64_t>(k) * blk);
}

// ------------------------------------------------------------------ chroma
// Chroma frames (12 per frame) from a 4096-point STFT at the decimated rate.
std::vector<float> chromaFrames(const std::vector<float>& y, double ra, int& count, int& hop, const Progress& pr) {
  const int N = 4096;
  hop = 2048;
  const int64_t n = static_cast<int64_t>(y.size());
  count = static_cast<int>(n / hop) + 1;
  std::vector<float> out(static_cast<size_t>(count) * 12, 0.f);
  Fft fft(N);
  std::vector<double> win(N), re(N), im(N);
  for (int i = 0; i < N; ++i) win[static_cast<size_t>(i)] = 0.5 - 0.5 * std::cos(2 * kPi * i / N);
  std::vector<int> pc(N / 2 + 1, -1);
  for (int k = 1; k <= N / 2; ++k) {
    const double f = k * ra / N;
    if (f < 60 || f > 2200) continue;
    const double midi = 69 + 12 * std::log2(f / 440.0);
    pc[static_cast<size_t>(k)] = ((static_cast<int>(std::lround(midi)) % 12) + 12) % 12;
  }
  for (int t = 0; t < count; ++t) {
    if ((t & 63) == 0) {
      pr.set(static_cast<float>(t) / count);
      if (pr.cancelled()) return out;
    }
    const int64_t c = static_cast<int64_t>(t) * hop - N / 2;
    for (int i = 0; i < N; ++i) {
      const int64_t j = c + i;
      re[static_cast<size_t>(i)] = (j >= 0 && j < n ? y[static_cast<size_t>(j)] : 0.0) * win[static_cast<size_t>(i)];
      im[static_cast<size_t>(i)] = 0;
    }
    fft.run(re, im);
    for (int k = 1; k <= N / 2; ++k) {
      const int p = pc[static_cast<size_t>(k)];
      if (p < 0) continue;
      const double m = std::sqrt(re[static_cast<size_t>(k)] * re[static_cast<size_t>(k)] + im[static_cast<size_t>(k)] * im[static_cast<size_t>(k)]);
      out[static_cast<size_t>(t) * 12 + static_cast<size_t>(p)] += static_cast<float>(m);
    }
  }
  return out;
}

double cosine(const double* a, const double* b, int n) {
  double ab = 0, aa = 0, bb = 0;
  for (int i = 0; i < n; ++i) ab += a[i] * b[i], aa += a[i] * a[i], bb += b[i] * b[i];
  return aa > 1e-18 && bb > 1e-18 ? ab / std::sqrt(aa * bb) : 0.0;
}

// Mean of a per-frame feature over [t0, t1) frames.
void meanOver(const std::vector<float>& f, int dims, int count, double t0, double t1, double* out) {
  for (int d = 0; d < dims; ++d) out[d] = 0;
  int a = std::clamp(static_cast<int>(std::floor(t0)), 0, count - 1), b = std::clamp(static_cast<int>(std::ceil(t1)), a + 1, count);
  for (int t = a; t < b; ++t)
    for (int d = 0; d < dims; ++d) out[d] += f[static_cast<size_t>(t) * static_cast<size_t>(dims) + static_cast<size_t>(d)];
  for (int d = 0; d < dims; ++d) out[d] /= (b - a);
}

// Krumhansl-Kessler key profiles (published probe-tone ratings).
constexpr double kMajor[12] = {6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88};
constexpr double kMinor[12] = {6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17};

double pearson(const double* a, const double* b, int n) {
  double ma = 0, mb = 0;
  for (int i = 0; i < n; ++i) ma += a[i], mb += b[i];
  ma /= n, mb /= n;
  double ab = 0, aa = 0, bb = 0;
  for (int i = 0; i < n; ++i) ab += (a[i] - ma) * (b[i] - mb), aa += (a[i] - ma) * (a[i] - ma), bb += (b[i] - mb) * (b[i] - mb);
  return aa > 0 && bb > 0 ? ab / std::sqrt(aa * bb) : 0.0;
}

}  // namespace

// ================================================================== public pieces

const char* kindName(int kind) {
  static const char* const n[KindCount] = {"unknown", "break", "loop", "song", "oneshot", "ambient"};
  return kind >= 0 && kind < KindCount ? n[kind] : "unknown";
}

namespace {

// Note name at the start of a token: "c", "c#", "db", ... -> pitch class and length (0 none).
int noteOf(const std::string& t, size_t& len) {
  static const int base[7] = {9, 11, 0, 2, 4, 5, 7};  // a b c d e f g
  len = 0;
  if (t.empty() || t[0] < 'a' || t[0] > 'g') return -1;
  int pc = base[t[0] - 'a'];
  len = 1;
  if (t.size() > 1 && (t[1] == '#' || t[1] == 'b')) {
    // "b" after a note is a flat only when more follows that reads as a mode ("bbm", "ebmin"), or the token ends
    pc = (pc + (t[1] == '#' ? 1 : 11)) % 12;
    len = 2;
  }
  return pc;
}

bool isNumber(const std::string& t) {
  if (t.empty()) return false;
  int dots = 0;
  for (char c : t) {
    if (c == '.') ++dots;
    else if (c < '0' || c > '9') return false;
  }
  return dots <= 1 && t.front() != '.' && t.back() != '.';
}

}  // namespace

AnalyseHints analyseHintsFor(const std::string& path) {
  AnalyseHints h;
  std::string name = fs::path(path).stem().string();
  std::string s;
  for (char c : name) s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  // ---- tempo: "<n> bpm", "<n>bpm", "bpm <n>", "--110.00bpm-"
  auto numberBefore = [&](size_t at) -> double {
    size_t e = at;
    while (e > 0 && (s[e - 1] == ' ' || s[e - 1] == '_' || s[e - 1] == '-')) --e;
    size_t b = e;
    while (b > 0 && ((s[b - 1] >= '0' && s[b - 1] <= '9') || s[b - 1] == '.')) --b;
    if (b == e) return 0;
    const std::string t = s.substr(b, e - b);
    return isNumber(t) ? std::atof(t.c_str()) : 0.0;
  };
  auto numberAfter = [&](size_t at) -> double {
    size_t b = at;
    while (b < s.size() && (s[b] == ' ' || s[b] == '_' || s[b] == '-' || s[b] == ':')) ++b;
    size_t e = b;
    while (e < s.size() && ((s[e] >= '0' && s[e] <= '9') || s[e] == '.')) ++e;
    const std::string t = s.substr(b, e - b);
    return isNumber(t) ? std::atof(t.c_str()) : 0.0;
  };
  size_t bpmAt = std::string::npos;
  for (size_t at = s.find("bpm"); at != std::string::npos && h.bpm <= 0; at = s.find("bpm", at + 3)) {
    double v = numberBefore(at);
    if (!(v >= 40 && v <= 250)) v = numberAfter(at + 3);
    if (v >= 40 && v <= 250) {
      h.bpm = v;
      h.strong = true;
      bpmAt = at + 3;
    }
  }
  // tokens (separators: space _ - ( ) [ ] , +)
  std::vector<std::string> tok;
  std::vector<size_t> tokAt;
  {
    std::string cur;
    for (size_t i = 0; i <= s.size(); ++i) {
      const char c = i < s.size() ? s[i] : ' ';
      if (c == ' ' || c == '_' || c == '-' || c == '(' || c == ')' || c == '[' || c == ']' || c == ',' || c == '+') {
        if (!cur.empty()) tok.push_back(cur), tokAt.push_back(i - cur.size());
        cur.clear();
      } else {
        cur += c;
      }
    }
  }
  if (h.bpm <= 0) {
    // a bare number in a sample-pack name ("KMRBI_GHHS_80_drum_loop", "Jazz Loop 149.5"); not "Track 41", "Vol 2"
    static const char* const notTempo[] = {"track", "part", "pt", "vol", "volume", "disc", "cd", "take", "side", "no", "number", "#", "v", "version", "mix"};
    for (size_t i = 0; i < tok.size(); ++i) {
      if (!isNumber(tok[i])) continue;
      const double v = std::atof(tok[i].c_str());
      if (v < 60 || v > 200) continue;
      bool bad = false;
      if (i > 0)
        for (const char* w : notTempo)
          if (tok[i - 1] == w) bad = true;
      if (bad) continue;
      // "Drumloop 62", "Kick 120": a whole number after a space at the end of a name is an index;
      // sample-pack tempos sit between underscores / hyphens ("KMRBI_GHHS_80_drum") or carry a decimal ("149.5")
      const size_t at = tokAt[i], end = at + tok[i].size();
      const bool decimal = tok[i].find('.') != std::string::npos;
      const bool joined = (at > 0 && (s[at - 1] == '_' || s[at - 1] == '-')) || (end < s.size() && (s[end] == '_' || s[end] == '-'));
      if (!decimal && !joined) continue;
      h.bpm = v;
      bpmAt = end;
      break;
    }
  }
  if (h.bpm <= 0) {
    // the folder: "Drum Loops/Cool 165.5", "Loops 92 BPM"
    std::string dir = fs::path(path).parent_path().filename().string();
    for (char& c : dir) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const size_t at = dir.find("bpm");
    std::string last;
    for (size_t i = dir.size(); i > 0 && dir[i - 1] != ' ' && dir[i - 1] != '_' && dir[i - 1] != '-'; --i) last.insert(last.begin(), dir[i - 1]);
    if (at != std::string::npos) {
      std::string num;
      for (size_t i = 0; i < at; ++i)
        if ((dir[i] >= '0' && dir[i] <= '9') || dir[i] == '.') num += dir[i];
        else if (dir[i] != ' ' && dir[i] != '_') num.clear();
      const double v = isNumber(num) ? std::atof(num.c_str()) : 0.0;
      if (v >= 40 && v <= 250) h.bpm = v, h.strong = true;
    } else if (isNumber(last)) {
      const double v = std::atof(last.c_str());
      if (v >= 60 && v <= 200) h.bpm = v;
    }
  }
  // ---- key: "am", "a#m", "abmin", "f#minor", "cmaj", "d# maj", "-abm-", a lone note right after the tempo
  for (size_t i = 0; i < tok.size() && h.key < 0; ++i) {
    size_t n = 0;
    const int pc = noteOf(tok[i], n);
    if (pc < 0) continue;
    const std::string rest = tok[i].substr(n);
    const std::string next = i + 1 < tok.size() ? tok[i + 1] : std::string();
    auto modeOf = [](const std::string& m) -> int {  // 1 minor, 0 major, -1 not a mode
      if (m == "m" || m == "min" || m == "minor" || m == "mi") return 1;
      if (m == "maj" || m == "major" || m == "ma") return 0;
      return -1;
    };
    int mode = modeOf(rest);
    if (rest.empty() && modeOf(next) >= 0) mode = modeOf(next);
    const bool afterTempo = bpmAt != std::string::npos && tokAt[i] >= bpmAt && tokAt[i] <= bpmAt + 3;
    if (mode < 0 && rest.empty() && afterTempo) mode = 0;  // "--144.00bpm-C-": a lone note after the tempo is major
    if (mode < 0) continue;
    // "am" is also a word: only as a key near the tempo or at the end of the name
    if (tok[i] == "am" && !afterTempo && i + 2 < tok.size()) continue;
    h.key = pc;
    h.minor = mode == 1;
  }
  return h;
}

std::vector<float> onsetEnvelope(const float* mono, int64_t n, double rate, double& envRate, std::vector<float>* lowBand,
                                 std::vector<float>* midBand) {
  std::vector<float> x(mono, mono + n);
  const int D = decimationFor(rate);
  std::vector<float> y = decimate(x, D);
  Frames F;
  spectralFrames(y, rate / D, F, Progress{});
  detrend(F.onset, static_cast<int>(F.rate * 0.2));
  envRate = F.rate;
  if (lowBand) *lowBand = F.band[0];
  if (midBand) *midBand = F.band[4];
  return F.onset;
}

void buildPeaks(const Sample& s, Peaks& p) {
  const int64_t n = static_cast<int64_t>(s.frames());
  const int64_t nb = (n + kPeakBlock - 1) / kPeakBlock;
  p.frames = n;
  p.mn.assign(static_cast<size_t>(nb), 0.f);
  p.mx.assign(static_cast<size_t>(nb), 0.f);
  p.rms.assign(static_cast<size_t>(nb), 0.f);
  const bool st = !s.mono() && s.right.size() == s.left.size();
  for (int64_t b = 0; b < nb; ++b) {
    float mn = 0, mx = 0;
    double sq = 0;
    const int64_t e = std::min(n, (b + 1) * kPeakBlock);
    for (int64_t i = b * kPeakBlock; i < e; ++i) {
      const float l = s.left[static_cast<size_t>(i)];
      mn = std::min(mn, l);
      mx = std::max(mx, l);
      float m = l;
      if (st) {
        const float r = s.right[static_cast<size_t>(i)];
        mn = std::min(mn, r);
        mx = std::max(mx, r);
        m = 0.5f * (l + r);
      }
      sq += static_cast<double>(m) * m;
    }
    p.mn[static_cast<size_t>(b)] = mn;
    p.mx[static_cast<size_t>(b)] = mx;
    p.rms[static_cast<size_t>(b)] = static_cast<float>(std::sqrt(sq / std::max<int64_t>(1, e - b * kPeakBlock)));
  }
}

bool analyse(const Sample& s, Analysis& A, std::atomic<float>* progress, const std::atomic<bool>* cancel, const AnalyseHints& hints) {
  A = Analysis{};
  A.sampleRate = s.sampleRate;
  A.frames = static_cast<int64_t>(s.frames());
  if (s.frames() < 64 || s.sampleRate <= 0) return false;
  auto stage = [&](float a, float b) { return Progress{progress, cancel, a, b}; };
  const double sr = s.sampleRate;
  std::vector<float> x = monoMix(s);
  // ---- the audible span: leading silence (-45 dB under the peak, 1 ms blocks) and the
  // end of the tail (-60 dB); the start backs off 1 ms so the attack stays whole
  {
    const int64_t N = A.frames;
    const int blk = std::max(16, static_cast<int>(sr * 0.001));
    double peak = 0;
    for (float v : x) peak = std::max(peak, static_cast<double>(std::fabs(v)));
    int64_t first = 0, last = N;
    if (peak > 1e-6) {
      const double on = peak * std::pow(10.0, -45.0 / 20), off = peak * std::pow(10.0, -60.0 / 20);
      first = -1;
      for (int64_t b = 0; b < N && first < 0; b += blk) {
        const int64_t e = std::min(N, b + blk);
        for (int64_t i = b; i < e; ++i)
          if (std::fabs(x[static_cast<size_t>(i)]) > on) {
            first = std::max<int64_t>(0, i - blk);
            break;
          }
      }
      if (first < 0) first = 0;
      last = first;
      for (int64_t i = N - 1; i > first; --i)
        if (std::fabs(x[static_cast<size_t>(i)]) > off) {
          last = std::min(N, i + blk);
          break;
        }
    }
    A.contentStart = first;
    A.contentEnd = std::max(first + 1, last);
  }
  const int D = decimationFor(sr);
  std::vector<float> y = decimate(x, D);
  const double ra = sr / D;
  if (cancel && cancel->load()) return false;
  stage(0, 0.05f).set(1);

  // ---- onset strength
  Frames F;
  spectralFrames(y, ra, F, stage(0.05f, 0.45f));
  if (cancel && cancel->load()) return false;
  F.D = D;
  std::vector<float> O = F.onset;
  detrend(O, static_cast<int>(F.rate * 0.2));
  const int n = static_cast<int>(O.size());
  const double fps = F.rate;
  A.onsetEnv = O;
  A.envRate = fps;
  // frame index -> source frame (window centred on t * hop)
  auto toFrame = [&](double t) { return t * F.hop * D; };

  // ---- tempo
  Tempogram T = tempogram(O, fps);
  TempoPick tp = pickTempo(T, fps);
  // Octave choice between a tempo and its double: track beats at the faster one;
  // if its beats alternate strong / weak, the beat is the slower one (hats or
  // off-beats in between), if they are all strong (kick, snare, kick, snare),
  // the faster one is the beat.
  {
    const double fast = tp.bpm <= 100 ? 2 * tp.bpm : tp.bpm, slow = fast / 2;
    if (fast <= 205 && slow >= 58) {
      const double tauF = fps * 60.0 / fast;
      std::vector<double> perF = localPeriod(T, tauF, n);
      std::vector<double> bf = trackBeats(O, perF, 100.0);
      double e[2] = {0, 0};
      int c[2] = {0, 0};
      for (size_t k = 0; k < bf.size(); ++k) {
        const int i = static_cast<int>(std::lround(bf[k]));
        double v = 0;
        for (int d = -2; d <= 2; ++d)
          if (i + d >= 0 && i + d < n) v = std::max(v, static_cast<double>(O[static_cast<size_t>(i + d)]));
        e[k % 2] += v;
        ++c[k % 2];
      }
      const double m0 = c[0] ? e[0] / c[0] : 0, m1 = c[1] ? e[1] / c[1] : 0;
      const double alt = std::max(m0, m1) / std::max(1e-6, std::min(m0, m1));
      // below 75 BPM the half tempo needs overwhelming evidence (half-time feels count at the faster tempo)
      const double thr = slow < 75 ? 4.0 : 1.6;
      const double pick = alt < thr ? fast : slow;
      const double decisive = std::clamp(std::fabs(std::log(alt / thr)) / std::log(1.6), 0.0, 1.0);
      if (std::getenv("SELTMANN_DEBUG")) std::fprintf(stderr, "octave: salience %.2f fast %.2f alt %.2f -> %.2f\n", tp.bpm, fast, alt, pick);
      if (std::fabs(pick - tp.bpm) > 1e-6) {
        const double old = tp.bpm;
        tp.bpm = pick;
        std::swap(tp.halfScore, tp.doubleScore);
        if (pick > old) tp.halfScore = 1.0, tp.doubleScore = 0.0;
        else tp.doubleScore = 1.0, tp.halfScore = 0.0;
      }
      tp.confidence = std::clamp(0.5 * tp.confidence + 0.5 * decisive, 0.0, 1.0);
    }
  }
  // ---- the file name's tempo ("92 BPM", "_92_"): it picks the octave when the audio agrees
  // within 4 %, and a name that says "bpm" wins when the audio evidence is weak
  if (hints.bpm > 0) {
    double bestE = 1e9;
    for (double f : {1.0, 2.0, 0.5, 1.5, 2.0 / 3.0, 4.0, 0.25, 3.0, 1.0 / 3.0}) bestE = std::min(bestE, std::fabs(std::log2(tp.bpm * f / hints.bpm)));
    // a name that says "bpm" (stores, sample packs) is trusted over a drumless or ambiguous audio guess
    if (bestE <= std::log2(1.04) || hints.strong) {
      tp.bpm = hints.bpm;
      tp.confidence = std::max(tp.confidence, bestE <= std::log2(1.04) ? 0.7 : 0.4);
      A.nameBpm = hints.bpm;
    }
  }
  stage(0.45f, 0.5f).set(1);
  const double tau0 = fps * 60.0 / tp.bpm;
  std::vector<double> period = localPeriod(T, tau0, n);

  // ---- beats
  std::vector<double> bt = trackBeats(O, period, 100.0);
  stage(0.5f, 0.6f).set(1);
  if (cancel && cancel->load()) return false;
  A.beats.clear();
  for (double t : bt) {
    double f = toFrame(refineOnEnvelope(O, t));
    // the flux peaks a little before the hit in centred windows: refine on the waveform
    f = refineAttack(x, sr, f);
    if (f >= 0 && f < static_cast<double>(A.frames)) A.beats.push_back(f);
  }
  std::sort(A.beats.begin(), A.beats.end());
  // drop near-duplicates
  {
    std::vector<double> b2;
    const double minGap = toFrame(tau0) * 0.4;
    for (double f : A.beats)
      if (b2.empty() || f - b2.back() > minGap) b2.push_back(f);
    A.beats.swap(b2);
  }
  // regression tempo over the beats the audio supports (a hit near the beat), with
  // outliers trimmed: beats carried through silence or a ring-out do not bend it
  double bpm = tp.bpm;
  if (A.beats.size() >= 4) {
    const size_t m = A.beats.size();
    std::vector<double> str(m, 0.0);
    for (size_t i = 0; i < m; ++i) {
      const int c = static_cast<int>(std::lround(A.beats[i] / (F.hop * D)));
      for (int d = -2; d <= 2; ++d)
        if (c + d >= 0 && c + d < n) str[i] = std::max(str[i], static_cast<double>(O[static_cast<size_t>(c + d)]));
    }
    std::vector<double> sorted = str;
    std::sort(sorted.begin(), sorted.end());
    const double thr = 0.25 * sorted[sorted.size() * 3 / 4];
    std::vector<char> use(m);
    for (size_t i = 0; i < m; ++i) use[i] = str[i] >= thr;
    double slope = 0, icpt = 0;
    for (int pass = 0; pass < 3; ++pass) {
      double sx = 0, sy = 0, sxx = 0, sxy = 0, cnt = 0;
      for (size_t i = 0; i < m; ++i)
        if (use[i]) {
          const double xi = static_cast<double>(i), yi = A.beats[i];
          sx += xi, sy += yi, sxx += xi * xi, sxy += xi * yi, cnt += 1;
        }
      if (cnt < 4) break;
      const double den = cnt * sxx - sx * sx;
      if (den <= 0) break;
      slope = (cnt * sxy - sx * sy) / den;
      icpt = (sy - slope * sx) / cnt;
      for (size_t i = 0; i < m; ++i) use[i] = use[i] && std::fabs(A.beats[i] - (icpt + slope * static_cast<double>(i))) < 0.2 * slope;
    }
    if (slope > 0) bpm = 60.0 * sr / slope;
  }
  // ---- loops: a short file that holds a whole number of beats (bars first) gets a grid fitted
  // exactly to its length: tempo = beats * 60 / length, beat 0 on the loop's first frame. The beat
  // count is the one whose tempo is nearest the file name's tempo (when there is one) or the
  // detected tempo (octaves allowed). Loops that start after leading silence are tried from the
  // first sound too.
  const double secs = static_cast<double>(A.frames) / sr;
  if (secs < 45 && bpm > 0) {
    struct Fit {
      double err = 1e9, start = 0, bpm = 0;
      int beats = 0;
    } best;
    std::vector<double> starts = {0.0};
    if (static_cast<double>(A.contentStart) > 0.01 * sr) starts.push_back(static_cast<double>(A.contentStart));
    static const int kBeats[] = {1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128};
    const double prior = A.nameBpm > 0 ? A.nameBpm : hints.bpm;
    // a fitted grid has to agree with the beats the tracker found where the music plays: most
    // tracked beats within 12 % of a beat of a fitted one (a loop with a tail, or a song excerpt,
    // has a length that is not whole bars and drifts away from them)
    // (measured at the tracker's own period: it may have run at an octave of the fitted tempo)
    auto agrees = [&](double s0, int nb) {
      double step = (static_cast<double>(A.frames) - s0) / nb;
      const double tracked = 60.0 * sr / bpm;
      while (step > tracked * 1.414) step *= 0.5;
      while (step < tracked * 0.707) step *= 2.0;
      int hit = 0, all = 0;
      for (double f : A.beats) {
        if (f < static_cast<double>(A.contentStart) - 0.25 * step || f > static_cast<double>(A.contentEnd)) continue;
        ++all;
        const double k = std::round((f - s0) / step);
        if (std::fabs(f - (s0 + k * step)) < 0.12 * step) ++hit;
      }
      return all < 3 || hit >= 0.7 * all;
    };
    for (double s0 : starts) {
      const double len = (static_cast<double>(A.frames) - s0) / sr;
      if (len < 0.2) continue;
      for (int nb : kBeats) {
        const double cand = nb * 60.0 / len;
        if (cand < 50 || cand > 250) continue;
        double err = 1e9;
        // a length that is whole beats at the tempo the name states is evidence enough on its own
        const bool named = prior > 0 && std::fabs(std::log2(cand / prior)) <= std::log2(hints.strong ? 1.025 : 1.015);
        if (named) {
          err = std::fabs(std::log2(cand / prior));
        } else {
          for (double oct : {1.0, 2.0, 0.5}) err = std::min(err, std::fabs(std::log2(cand / (bpm * oct))) + (oct == 1.0 ? 0.0 : 0.012));
          if (err > std::log2(1.02) + 0.012) continue;
          if (prior > 0) err += 0.02;  // the name disagrees: a fit away from it only when nothing near it fits
        }
        if (!named && !agrees(s0, nb)) continue;
        if (nb % 4 != 0) err += 0.006;  // whole bars first
        if (nb < 4) err += 0.01;
        if (s0 > 0) err += 0.004;       // the file start first
        if (prior <= 0 && (cand < 70 || cand > 180)) err += 0.02;  // the usual tempo range first (breaks at 98, not 196)
        if (err < best.err) best = {err, s0, cand, nb};
      }
    }
    if (best.beats > 0) {
      bpm = best.bpm;
      const double step = (static_cast<double>(A.frames) - best.start) / best.beats;
      A.beats.clear();
      for (int k = 0; k < best.beats; ++k) A.beats.push_back(best.start + k * step);
      A.loopFit = true;
    }
  }
  // a tempo the name states, met by the audio within 3 %: the name's (stores state it exactly)
  // (live recordings drift a little around it; the beat grid keeps following the real beats)
  if (!A.loopFit && hints.strong && A.nameBpm > 0 && std::fabs(bpm - A.nameBpm) / A.nameBpm < 0.03) bpm = A.nameBpm;
  A.bpm = bpm;
  A.confidence = tp.confidence;
  A.halfScore = tp.halfScore;
  A.doubleScore = tp.doubleScore;
  stage(0.6f, 0.62f).set(1);

  // ---- chroma
  int cCount = 0, cHop = 0;
  std::vector<float> chroma = chromaFrames(y, ra, cCount, cHop, stage(0.62f, 0.8f));
  if (cancel && cancel->load()) return false;
  auto chromaFrameOf = [&](double srcFrame) { return srcFrame / D / cHop; };

  // ---- per-beat features
  const int nbeats = static_cast<int>(A.beats.size());
  std::vector<double> snare(static_cast<size_t>(nbeats), 0.0);
  std::vector<double> kick(static_cast<size_t>(nbeats), 0.0), hit(static_cast<size_t>(nbeats), 0.0), harm(static_cast<size_t>(nbeats), 0.0);
  std::vector<std::array<double, 12>> beatChroma(static_cast<size_t>(nbeats));
  const double beatLen = 60.0 / std::max(1.0, bpm) * sr;
  for (int b = 0; b < nbeats; ++b) {
    const double f0 = A.beats[static_cast<size_t>(b)];
    const double f1 = b + 1 < nbeats ? A.beats[static_cast<size_t>(b + 1)] : f0 + beatLen;
    const double t = f0 / (F.hop * D);
    const int w = std::max(1, static_cast<int>(fps * 0.04));
    double k = 0, h = 0, sn = 0;
    for (int d = -w; d <= w; ++d) {
      const int i = static_cast<int>(std::lround(t)) + d;
      if (i < 0 || i >= n) continue;
      k = std::max(k, static_cast<double>(F.band[0][static_cast<size_t>(i)] + F.band[1][static_cast<size_t>(i)]));
      h = std::max(h, static_cast<double>(O[static_cast<size_t>(i)]));
      sn = std::max(sn, static_cast<double>(F.band[3][static_cast<size_t>(i)] + F.band[4][static_cast<size_t>(i)] + F.band[5][static_cast<size_t>(i)]));
    }
    snare[static_cast<size_t>(b)] = sn;
    kick[static_cast<size_t>(b)] = k;
    hit[static_cast<size_t>(b)] = h;
    meanOver(chroma, 12, cCount, chromaFrameOf(f0), chromaFrameOf(f1), beatChroma[static_cast<size_t>(b)].data());
  }
  for (int b = 2; b + 2 <= nbeats; ++b) {
    double before[12] = {}, after[12] = {};
    for (int d = 0; d < 12; ++d) {
      before[d] = beatChroma[static_cast<size_t>(b - 2)][static_cast<size_t>(d)] + beatChroma[static_cast<size_t>(b - 1)][static_cast<size_t>(d)];
      after[d] = beatChroma[static_cast<size_t>(b)][static_cast<size_t>(d)] + (b + 1 < nbeats ? beatChroma[static_cast<size_t>(b + 1)][static_cast<size_t>(d)] : 0.0);
    }
    harm[static_cast<size_t>(b)] = 1.0 - cosine(before, after, 12);
  }
  auto zscore = [](std::vector<double>& v) {
    if (v.empty()) return;
    double m = 0, s2 = 0;
    for (double x2 : v) m += x2;
    m /= static_cast<double>(v.size());
    for (double x2 : v) s2 += (x2 - m) * (x2 - m);
    const double sd = std::sqrt(s2 / static_cast<double>(v.size())) + 1e-9;
    for (double& x2 : v) x2 = (x2 - m) / sd;
  };
  // phrase novelty: how much the sound of the 8 beats after a beat differs from the 8 before
  // it. Drops, new parts and fills resolve on bar 1 of a phrase, so its peaks mark downbeats.
  std::vector<double> phrase(static_cast<size_t>(nbeats), 0.0);
  {
    std::vector<std::array<double, kMel>> bm(static_cast<size_t>(nbeats));
    for (int b = 0; b < nbeats; ++b) {
      const double f0 = A.beats[static_cast<size_t>(b)];
      const double f1 = b + 1 < nbeats ? A.beats[static_cast<size_t>(b + 1)] : f0 + beatLen;
      meanOver(F.mel, kMel, F.count, f0 / (F.hop * D), f1 / (F.hop * D), bm[static_cast<size_t>(b)].data());
    }
    const int Wn = 8;
    for (int b = Wn; b + Wn <= nbeats; ++b) {
      double d2 = 0;
      for (int k = 0; k < kMel; ++k) {
        double before = 0, after = 0;
        for (int j = 1; j <= Wn; ++j) before += bm[static_cast<size_t>(b - j)][static_cast<size_t>(k)];
        for (int j = 0; j < Wn; ++j) after += bm[static_cast<size_t>(b + j)][static_cast<size_t>(k)];
        const double dd = (after - before) / Wn;
        d2 += dd * dd;
      }
      phrase[static_cast<size_t>(b)] = std::sqrt(d2 / kMel);
    }
    // keep only local peaks (a change spreads over neighbouring beats)
    std::vector<double> pk(phrase.size(), 0.0);
    for (int b = 0; b < nbeats; ++b) {
      bool isMax = true;
      for (int d = -3; d <= 3 && isMax; ++d)
        if (d && b + d >= 0 && b + d < nbeats && phrase[static_cast<size_t>(b + d)] > phrase[static_cast<size_t>(b)]) isMax = false;
      if (isMax) pk[static_cast<size_t>(b)] = phrase[static_cast<size_t>(b)];
    }
    phrase.swap(pk);
  }
  zscore(kick);
  zscore(snare);
  zscore(hit);
  zscore(harm);
  zscore(phrase);
  std::vector<double> dfeat(static_cast<size_t>(nbeats));
  for (int b = 0; b < nbeats; ++b)
    dfeat[static_cast<size_t>(b)] = 1.0 * kick[static_cast<size_t>(b)] + 1.2 * harm[static_cast<size_t>(b)] + 0.3 * hit[static_cast<size_t>(b)] +
                                    1.5 * phrase[static_cast<size_t>(b)];

  // ---- compound (ternary) subdivision
  {
    double tern = 0, bin = 0;
    int cnt = 0;
    for (int b = 0; b + 1 < nbeats; ++b) {
      const double t0 = A.beats[static_cast<size_t>(b)] / (F.hop * D), t1 = A.beats[static_cast<size_t>(b + 1)] / (F.hop * D);
      auto at = [&](double u) {
        const int i = static_cast<int>(std::lround(t0 + (t1 - t0) * u));
        double m = 0;
        for (int d = -1; d <= 1; ++d)
          if (i + d >= 0 && i + d < n) m = std::max(m, static_cast<double>(O[static_cast<size_t>(i + d)]));
        return m;
      };
      tern += 0.5 * (at(1.0 / 3) + at(2.0 / 3));
      bin += at(0.5);
      ++cnt;
    }
    if (cnt > 4) A.compound = tern > 1.5 * bin && tern / cnt > 0.4;
  }

  // ---- meter + downbeat
  {
    struct Cand {
      int M, phase;
      double score;
    };
    std::vector<Cand> cands;
    const std::vector<int> meters = A.compound ? std::vector<int>{2, 4} : std::vector<int>{3, 4};
    if (std::getenv("SELTMANN_DEBUG")) {
      for (int ph = 0; ph < 4; ++ph) {
        double k = 0, h = 0, o = 0, q = 0;
        int c = 0;
        for (int b = ph; b < nbeats; b += 4)
          k += kick[static_cast<size_t>(b)], h += harm[static_cast<size_t>(b)], o += hit[static_cast<size_t>(b)], q += phrase[static_cast<size_t>(b)], ++c;
        std::fprintf(stderr, "phase %d (first beat %.2fs): kick %.2f harm %.2f hit %.2f phrase %.2f\n", ph,
                     A.beats.empty() ? 0.0 : A.beats[static_cast<size_t>(ph)] / sr, k / std::max(1, c), h / std::max(1, c), o / std::max(1, c),
                     q / std::max(1, c));
      }
    }
    int firstLoud = -1;
    {
      double peakRms = 0;
      for (float r : F.rms) peakRms = std::max(peakRms, static_cast<double>(r));
      for (int b = 0; b < nbeats && firstLoud < 0; ++b) {
        const int i = std::clamp(static_cast<int>(std::lround(A.beats[static_cast<size_t>(b)] / (F.hop * D))) + 2, 0, std::max(0, F.count - 1));
        if (F.rms[static_cast<size_t>(i)] > 0.1 * peakRms) firstLoud = b;
      }
    }
    for (int M : meters)
      for (int ph = 0; ph < M; ++ph) {
        double on = 0, off = 0;
        int non = 0, noff = 0;
        for (int b = 0; b < nbeats; ++b) {
          if (b % M == ph) on += dfeat[static_cast<size_t>(b)], ++non;
          else off += dfeat[static_cast<size_t>(b)], ++noff;
        }
        double sc = (non ? on / non : 0) - (noff ? off / noff : 0);
        if (M == 4) {
          // backbeat: snare / clap on 2 and 4, kick on 1 and 3 (sets the phase modulo half a bar) ...
          double m4[4][4] = {};  // feature x position in the bar
          int c4[4] = {};
          for (int b = 0; b < nbeats; ++b) {
            const int pos = ((b - ph) % 4 + 4) % 4;
            m4[0][pos] += snare[static_cast<size_t>(b)];
            m4[1][pos] += kick[static_cast<size_t>(b)];
            m4[2][pos] += harm[static_cast<size_t>(b)];
            m4[3][pos] += phrase[static_cast<size_t>(b)];
            ++c4[pos];
          }
          for (auto& f : m4)
            for (int q = 0; q < 4; ++q) f[q] /= std::max(1, c4[q]);
          const double back = 0.5 * (m4[0][1] + m4[0][3]) - 0.5 * (m4[0][0] + m4[0][2]);
          const double kpat = 0.5 * (m4[1][0] + m4[1][2]) - 0.5 * (m4[1][1] + m4[1][3]);
          // ... then bar 1 against beat 3: chord changes, phrase changes and the stronger kick
          const double half = 0.9 * (m4[2][0] - m4[2][2]) + 1.1 * (m4[3][0] - m4[3][2]) + 0.4 * (m4[1][0] - m4[1][2]);
          sc = 0.35 * back + 0.3 * kpat + half;
        }
        // songs start on bar 1: the first beat where the music comes in is likely a downbeat
        // (short loops and breaks are cut that way: there it is the strongest cue)
        if (firstLoud >= 0 && ((firstLoud - ph) % M + M) % M == 0) sc += static_cast<double>(A.frames) / sr < 45 ? 1.0 : 0.3;
        if (M == 4) {        }
        cands.push_back({M, ph, sc});
      }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.score > b.score; });
    if (!cands.empty()) {
      // 4/4 is the default unless 3/4 is clearly better
      const Cand* best4 = nullptr;
      const Cand* bestOther = nullptr;
      for (const auto& c : cands) {
        if (c.M == 4 && !best4) best4 = &c;
        if (c.M != 4 && !bestOther) bestOther = &c;
      }
      const Cand* pick = best4;
      if (bestOther && (!best4 || bestOther->score > best4->score * 1.25 + 0.05)) pick = bestOther;
      if (A.compound && bestOther && bestOther->M == 2 && best4 && bestOther->score >= best4->score * 0.9) pick = bestOther;
      if (!pick) pick = &cands.front();
      A.beatsPerBar = pick->M;
      A.bar1 = pick->phase;
      const double rivalM = (pick == best4 ? (bestOther ? bestOther->score : 0) : (best4 ? best4->score : 0));
      A.meterConfidence = std::clamp((pick->score - rivalM) / (std::fabs(pick->score) + 0.25), 0.0, 1.0);
      double second = -1e9;
      for (const auto& c : cands)
        if (c.M == pick->M && c.phase != pick->phase) second = std::max(second, c.score);
      A.downbeatConfidence = std::clamp((pick->score - second) / (std::fabs(pick->score) + 0.25), 0.0, 1.0);
    }
  }
  // ---- fitted loops: the meter follows the beat count, bar 1 is the loop start, unless the loop
  // opens with a silent pickup: then bar 1 snaps to the strongest kick phase
  if (A.loopFit && nbeats > 0) {
    A.compound = false;
    A.beatsPerBar = nbeats % 4 == 0 || nbeats < 3 ? 4 : nbeats % 3 == 0 ? 3 : 4;
    A.bar1 = 0;
    double peakRms = 0;
    for (float r : F.rms) peakRms = std::max(peakRms, static_cast<double>(r));
    const double beatLenF = nbeats > 1 ? A.beats[1] - A.beats[0] : beatLen;
    auto rmsAt = [&](double f0, double f1) {
      const int i0 = std::clamp(static_cast<int>(f0 / (F.hop * D)), 0, F.count - 1), i1 = std::clamp(static_cast<int>(f1 / (F.hop * D)), i0 + 1, F.count);
      double m = 0;
      for (int i = i0; i < i1; ++i) m = std::max(m, static_cast<double>(F.rms[static_cast<size_t>(i)]));
      return m;
    };
    const int M = std::max(1, A.beatsPerBar);
    if (nbeats >= 2 * M && rmsAt(A.beats[0], A.beats[0] + 0.5 * beatLenF) < 0.05 * peakRms) {
      int bestPh = 0;
      double bestK = -1e9;
      for (int ph = 1; ph < M; ++ph) {
        double k = 0;
        int c = 0;
        for (int b = ph; b < nbeats; b += M) k += kick[static_cast<size_t>(b)] + 0.5 * hit[static_cast<size_t>(b)], ++c;
        if (c && k / c > bestK) bestK = k / c, bestPh = ph;
      }
      A.bar1 = bestPh;
    }
    A.downbeatConfidence = std::max(A.downbeatConfidence, 0.8);
    A.meterConfidence = std::max(A.meterConfidence, 0.8);
  }
  stage(0.8f, 0.85f).set(1);

  // ---- bar features: chroma (12) + timbre (13)
  const int M = std::max(1, A.beatsPerBar);
  std::vector<int> barBeat;  // first beat of each bar
  for (int b = A.bar1 % M; b + M <= nbeats; b += M) barBeat.push_back(b);
  const int nbars = static_cast<int>(barBeat.size());
  std::vector<std::array<double, 12>> bc(static_cast<size_t>(nbars));
  std::vector<std::array<double, 13>> bt13(static_cast<size_t>(nbars));
  for (int i = 0; i < nbars; ++i) {
    const double f0 = A.beats[static_cast<size_t>(barBeat[static_cast<size_t>(i)])];
    const int e = barBeat[static_cast<size_t>(i)] + M;
    const double f1 = e < nbeats ? A.beats[static_cast<size_t>(e)] : static_cast<double>(A.frames);
    meanOver(chroma, 12, cCount, chromaFrameOf(f0), chromaFrameOf(f1), bc[static_cast<size_t>(i)].data());
    double mel[kMel];
    meanOver(F.mel, kMel, F.count, f0 / (F.hop * D), f1 / (F.hop * D), mel);
    for (int k = 0; k < 13; ++k) {
      double s2 = 0;
      for (int m2 = 0; m2 < kMel; ++m2) s2 += mel[m2] * std::cos(kPi * k * (m2 + 0.5) / kMel);
      bt13[static_cast<size_t>(i)][static_cast<size_t>(k)] = s2 * (k == 0 ? 0.25 : 1.0);
    }
  }
  // z-score timbre across bars
  for (int k = 0; k < 13; ++k) {
    double m2 = 0, s2 = 0;
    for (int i = 0; i < nbars; ++i) m2 += bt13[static_cast<size_t>(i)][static_cast<size_t>(k)];
    m2 /= std::max(1, nbars);
    for (int i = 0; i < nbars; ++i) s2 += std::pow(bt13[static_cast<size_t>(i)][static_cast<size_t>(k)] - m2, 2);
    const double sd = std::sqrt(s2 / std::max(1, nbars)) + 1e-9;
    for (int i = 0; i < nbars; ++i) bt13[static_cast<size_t>(i)][static_cast<size_t>(k)] = (bt13[static_cast<size_t>(i)][static_cast<size_t>(k)] - m2) / sd;
  }
  std::vector<double> S(static_cast<size_t>(nbars) * static_cast<size_t>(nbars), 0.0);
  auto Sat = [&](int i, int j) -> double& { return S[static_cast<size_t>(i) * static_cast<size_t>(nbars) + static_cast<size_t>(j)]; };
  for (int i = 0; i < nbars; ++i)
    for (int j = i; j < nbars; ++j) {
      const double sc = cosine(bc[static_cast<size_t>(i)].data(), bc[static_cast<size_t>(j)].data(), 12);
      const double st = 0.5 * (1.0 + cosine(bt13[static_cast<size_t>(i)].data(), bt13[static_cast<size_t>(j)].data(), 13));
      Sat(i, j) = Sat(j, i) = 0.5 * sc + 0.5 * st;
    }

  // ---- sections: checkerboard novelty with a phrase prior
  std::vector<int> bounds = {0};
  if (nbars >= 8) {
    // lag embedding: compare 4-bar sequences, not single bars (chords repeat across sections)
    std::vector<double> E(static_cast<size_t>(nbars) * static_cast<size_t>(nbars), 0.0);
    for (int i = 0; i < nbars; ++i)
      for (int j = 0; j < nbars; ++j) {
        double s2 = 0;
        int c = 0;
        for (int k = -1; k <= 2; ++k) {
          const int p = i + k, q = j + k;
          if (p < 0 || q < 0 || p >= nbars || q >= nbars) continue;
          s2 += Sat(p, q);
          ++c;
        }
        E[static_cast<size_t>(i) * static_cast<size_t>(nbars) + static_cast<size_t>(j)] = c ? s2 / c : 0;
      }
    const int K = 4;
    std::vector<double> nov(static_cast<size_t>(nbars), 0.0);
    for (int i = 1; i < nbars; ++i) {
      double s2 = 0, wsum = 0;
      for (int a = -K; a < K; ++a)
        for (int b = -K; b < K; ++b) {
          const int p = i + a, q = i + b;
          if (p < 0 || q < 0 || p >= nbars || q >= nbars) continue;
          const double g = std::exp(-0.5 * ((a + 0.5) * (a + 0.5) + (b + 0.5) * (b + 0.5)) / (K * K * 0.5));
          const double sign = ((a < 0) == (b < 0)) ? 1.0 : -1.0;
          s2 += sign * g * E[static_cast<size_t>(p) * static_cast<size_t>(nbars) + static_cast<size_t>(q)];
          wsum += g;
        }
      nov[static_cast<size_t>(i)] = wsum > 0 ? s2 / wsum : 0;
    }
    if (std::getenv("SELTMANN_DEBUG")) {
      for (int i = 0; i < nbars; ++i) std::fprintf(stderr, "nov %d %.4f\n", i, nov[static_cast<size_t>(i)]);
    }
    // statistics over the interior (the truncated kernel inflates the edges)
    double mean = 0, sd = 0;
    int cnt = 0;
    for (int i = 2; i + 2 <= nbars; ++i) mean += nov[static_cast<size_t>(i)], ++cnt;
    mean /= std::max(1, cnt);
    for (int i = 2; i + 2 <= nbars; ++i) sd += (nov[static_cast<size_t>(i)] - mean) * (nov[static_cast<size_t>(i)] - mean);
    sd = std::sqrt(sd / std::max(1, cnt));
    const int firstBar = (A.bar1 - A.bar1 % M) / M;  // bar index of bar 1 inside barBeat
    std::vector<std::pair<double, int>> peaks;
    for (int i = 2; i + 2 <= nbars; ++i) {
      bool isMax = true;
      for (int d = -2; d <= 2; ++d)
        if (d && i + d > 0 && i + d < nbars && nov[static_cast<size_t>(i + d)] > nov[static_cast<size_t>(i)]) isMax = false;
      if (!isMax) continue;
      const int rel = i - firstBar;
      const double prior = 1.0 + (rel % 4 == 0 ? 0.35 : 0.0) + (rel % 8 == 0 ? 0.2 : 0.0);
      const double v = nov[static_cast<size_t>(i)] * prior;
      if (nov[static_cast<size_t>(i)] > mean + 0.1 * sd && nov[static_cast<size_t>(i)] > 0.015) peaks.push_back({v, i});
    }
    std::sort(peaks.rbegin(), peaks.rend());
    std::vector<int> chosen;
    for (const auto& [v, i] : peaks) {
      bool ok = i >= 4 && nbars - i >= 2;
      for (int c : chosen)
        if (std::abs(c - i) < 4) ok = false;
      if (ok) chosen.push_back(i);
    }
    // phrase grid: a boundary within a bar of a 4-bar line moves onto it
    for (int& c : chosen)
      for (int d : {0, -1, 1})
        if (((c + d - firstBar) % 4 + 4) % 4 == 0 && c + d >= 4 && c + d <= nbars - 2) {
          c += d;
          break;
        }
    std::sort(chosen.begin(), chosen.end());
    chosen.erase(std::unique(chosen.begin(), chosen.end()), chosen.end());
    for (int c : chosen) bounds.push_back(c);
  }
  bounds.push_back(nbars);
  // labels by aligned similarity
  {
    const int ns = static_cast<int>(bounds.size()) - 1;
    std::vector<int> label(static_cast<size_t>(std::max(0, ns)), -1);
    int next = 0;
    auto segSim = [&](int s1, int s2) {
      const int a1 = bounds[static_cast<size_t>(s1)], l1 = bounds[static_cast<size_t>(s1 + 1)] - a1;
      const int a2 = bounds[static_cast<size_t>(s2)], l2 = bounds[static_cast<size_t>(s2 + 1)] - a2;
      const int L = std::min(l1, l2);
      if (L <= 0) return 0.0;
      double best = 0;
      // allow the shorter one to sit anywhere in the longer one (bar steps)
      const int slack = std::max(l1, l2) - L;
      for (int o = 0; o <= slack; ++o) {
        double s2v = 0;
        for (int k = 0; k < L; ++k) s2v += l1 >= l2 ? Sat(a1 + o + k, a2 + k) : Sat(a1 + k, a2 + o + k);
        best = std::max(best, s2v / L);
      }
      return best;
    };
    for (int s1 = 0; s1 < ns; ++s1) {
      int bestL = -1;
      double bestV = 0;
      for (int s0 = 0; s0 < s1; ++s0) {
        const double v = segSim(s0, s1);
        if (v > bestV) bestV = v, bestL = label[static_cast<size_t>(s0)];
      }
      label[static_cast<size_t>(s1)] = bestV >= 0.86 && bestL >= 0 ? bestL : std::min(next++, kSlotCount - 1);
    }
    // relabel in order of appearance after merging neighbours that got the same letter
    for (int s1 = 0; s1 < ns; ++s1) {
      Section sec;
      sec.firstBeat = barBeat.empty() ? 0 : barBeat[static_cast<size_t>(bounds[static_cast<size_t>(s1)])];
      const int eb = bounds[static_cast<size_t>(s1 + 1)];
      sec.endBeat = eb < nbars ? barBeat[static_cast<size_t>(eb)] : nbeats;
      sec.label = label[static_cast<size_t>(s1)];
      if (sec.endBeat <= sec.firstBeat) continue;
      if (!A.sections.empty() && A.sections.back().label == sec.label) A.sections.back().endBeat = sec.endBeat;
      else A.sections.push_back(sec);
    }
    {
      int map[kSlotCount];
      for (int& v : map) v = -1;
      int nx = 0;
      for (auto& sec : A.sections) {
        if (map[sec.label] < 0) map[sec.label] = nx++;
        sec.label = map[sec.label];
      }
    }
    if (nbars == 0 && nbeats > 0) A.sections.push_back({0, nbeats, 0});
  }
  stage(0.85f, 0.95f).set(1);

  // ---- loop spots
  {
    struct C2 {
      int bar, L;
      double score;
    };
    std::vector<C2> cs;
    const int firstBar = (A.bar1 - A.bar1 % M) / M;
    for (int L : {1, 2, 4, 8})
      for (int i = 0; i + 2 * L <= nbars; ++i) {
        double rep = 0;
        for (int k = 0; k < L; ++k) rep += Sat(i + k, i + L + k);
        rep /= L;
        const int rel = i - firstBar;
        double sc = rep + (rel % 4 == 0 ? 0.04 : 0.0) + (rel % L == 0 ? 0.02 : 0.0) + 0.01 * L;
        // inside one section
        int si = -1, sj = -1;
        for (size_t q = 0; q < A.sections.size(); ++q) {
          if (barBeat[static_cast<size_t>(i)] >= A.sections[q].firstBeat && barBeat[static_cast<size_t>(i)] < A.sections[q].endBeat) si = static_cast<int>(q);
          const int lastBeat = barBeat[static_cast<size_t>(i + L - 1)];
          if (lastBeat >= A.sections[q].firstBeat && lastBeat < A.sections[q].endBeat) sj = static_cast<int>(q);
        }
        if (si != sj) sc -= 0.1;
        cs.push_back({i, L, sc});
      }
    std::sort(cs.begin(), cs.end(), [](const C2& a, const C2& b) { return a.score > b.score; });
    std::vector<C2> keep;
    for (const auto& c : cs) {
      bool ok = true;
      for (const auto& k : keep)
        if (k.L == c.L && std::abs(k.bar - c.bar) < c.L) ok = false;
      if (!ok) continue;
      keep.push_back(c);
      if (keep.size() >= 12) break;
    }
    for (const auto& k : keep) A.loops.push_back({barBeat[static_cast<size_t>(k.bar)], k.L, static_cast<float>(std::clamp(k.score, 0.0, 1.0))});
  }

  // ---- key
  {
    double tot[12] = {};
    for (int t = 0; t < cCount; ++t) {
      double e = 0;
      for (int d = 0; d < 12; ++d) e += chroma[static_cast<size_t>(t) * 12 + static_cast<size_t>(d)];
      if (e <= 0) continue;
      for (int d = 0; d < 12; ++d) tot[d] += chroma[static_cast<size_t>(t) * 12 + static_cast<size_t>(d)];
    }
    double best = -2, second = -2;
    int bk = -1;
    bool bm = false;
    for (int k = 0; k < 12; ++k)
      for (int mi = 0; mi < 2; ++mi) {
        double prof[12];
        for (int d = 0; d < 12; ++d) prof[d] = (mi ? kMinor : kMajor)[((d - k) % 12 + 12) % 12];
        const double r = pearson(tot, prof, 12);
        if (r > best) second = best, best = r, bk = k, bm = mi == 1;
        else if (r > second) second = r;
      }
    double e = 0;
    for (double v : tot) e += v;
    if (e > 0 && bk >= 0) {
      A.key = bk;
      A.minor = bm;
      A.keyConfidence = std::clamp((best - second) * 6.0 + std::max(0.0, best) * 0.3, 0.0, 1.0);
    }
  }
  // ---- what the material is
  {
    const double dur = static_cast<double>(A.contentEnd - A.contentStart) / sr;
    const int t0 = std::clamp(static_cast<int>(static_cast<double>(A.contentStart) / (F.hop * D)), 0, std::max(0, n - 1));
    const int t1 = std::clamp(static_cast<int>(static_cast<double>(A.contentEnd) / (F.hop * D)) + 1, t0, n);
    // onsets with the transient detector the chop uses (the normalised envelope makes any
    // texture look busy); songs are told apart by their length and need no count
    int count = 0;
    if (secs < 45 && A.contentEnd > A.contentStart)
      count = static_cast<int>(stretch::detectOnsets(x.data() + A.contentStart, A.contentEnd - A.contentStart, sr, 0.5).size());
    (void)t0, (void)t1;
    A.onsetRate = dur > 0 ? count / dur : 0;
    std::vector<double> pk;
    const int c0 = std::clamp(static_cast<int>(static_cast<double>(A.contentStart) / D / std::max(1, cHop)), 0, std::max(0, cCount - 1));
    const int c1 = std::clamp(static_cast<int>(static_cast<double>(A.contentEnd) / D / std::max(1, cHop)) + 1, c0, cCount);
    for (int t = c0; t < c1; ++t) {
      double mx = 0, sum = 0;
      for (int d = 0; d < 12; ++d) {
        const double v = chroma[static_cast<size_t>(t) * 12 + static_cast<size_t>(d)];
        mx = std::max(mx, v), sum += v;
      }
      if (sum > 1e-3) pk.push_back(mx / (sum / 12));
    }
    if (!pk.empty()) {
      std::nth_element(pk.begin(), pk.begin() + static_cast<std::ptrdiff_t>(pk.size() / 2), pk.end());
      A.tonality = pk[pk.size() / 2];
    }
    if (secs >= 45) A.kind = A.confidence < 0.15 && A.onsetRate < 0.6 ? KindAmbient : KindSong;
    else if (dur < 1.0 || (count <= 2 && !A.loopFit && dur < 3.0)) A.kind = KindOneShot;
    else if (!A.loopFit && (A.onsetRate < 0.35 || (A.confidence < 0.35 && (A.onsetRate < 1.0 || A.tonality > 3.5)))) A.kind = KindAmbient;
    else A.kind = A.tonality < kDrumTonality && A.onsetRate >= 1.0 ? KindBreak : KindLoop;  // drums: noisy and full of hits
  }
  stage(0.95f, 1.0f).set(1);
  return true;
}

// ================================================================== text form

std::string analysisToText(const Analysis& a) {
  std::ostringstream o;
  o.precision(10);
  o << "rate " << a.sampleRate << " frames " << a.frames << "\n";
  o << "tempo " << a.bpm << " " << a.confidence << " " << a.halfScore << " " << a.doubleScore << "\n";
  o << "meter " << a.beatsPerBar << " " << (a.compound ? 1 : 0) << " " << a.bar1 << " " << a.meterConfidence << " " << a.downbeatConfidence << "\n";
  o << "key " << a.key << " " << (a.minor ? 1 : 0) << " " << a.keyConfidence << "\n";
  o << "kind " << a.kind << " " << a.contentStart << " " << a.contentEnd << " " << (a.loopFit ? 1 : 0) << " " << a.nameBpm << "\n";
  o << "beats " << a.beats.size();
  o.precision(4);
  double prev = 0;
  for (double b : a.beats) {
    o << " " << std::fixed << (b - prev);
    prev = b;
  }
  o << std::defaultfloat << "\n";
  o.precision(6);
  o << "sections " << a.sections.size();
  for (const auto& s : a.sections) o << " " << s.firstBeat << " " << s.endBeat << " " << s.label;
  o << "\n";
  o << "loops " << a.loops.size();
  for (const auto& l : a.loops) o << " " << l.firstBeat << " " << l.bars << " " << l.score;
  o << "\n";
  return o.str();
}

bool analysisFromText(const std::string& text, Analysis& a) {
  std::istringstream in(text);
  std::string line;
  bool any = false;
  while (std::getline(in, line)) {
    std::istringstream l(line);
    std::string k;
    l >> k;
    if (k == "rate") {
      std::string f;
      l >> a.sampleRate >> f >> a.frames;
      any = true;
    } else if (k == "tempo") {
      l >> a.bpm >> a.confidence >> a.halfScore >> a.doubleScore;
    } else if (k == "meter") {
      int c = 0;
      l >> a.beatsPerBar >> c >> a.bar1 >> a.meterConfidence >> a.downbeatConfidence;
      a.compound = c != 0;
    } else if (k == "key") {
      int mi = 0;
      l >> a.key >> mi >> a.keyConfidence;
      a.minor = mi != 0;
    } else if (k == "kind") {
      int lf = 0;
      l >> a.kind >> a.contentStart >> a.contentEnd >> lf >> a.nameBpm;
      a.kind = std::clamp(a.kind, 0, KindCount - 1);
      a.loopFit = lf != 0;
    } else if (k == "beats") {
      size_t n = 0;
      l >> n;
      if (n > 10000000) return false;
      a.beats.clear();
      a.beats.reserve(n);
      double acc = 0, d = 0;
      for (size_t i = 0; i < n && (l >> d); ++i) a.beats.push_back(acc += d);
    } else if (k == "sections") {
      size_t n = 0;
      l >> n;
      a.sections.clear();
      for (size_t i = 0; i < n && i < 4096; ++i) {
        Section s;
        if (!(l >> s.firstBeat >> s.endBeat >> s.label)) break;
        a.sections.push_back(s);
      }
    } else if (k == "loops") {
      size_t n = 0;
      l >> n;
      a.loops.clear();
      for (size_t i = 0; i < n && i < 4096; ++i) {
        LoopSpot s;
        if (!(l >> s.firstBeat >> s.bars >> s.score)) break;
        a.loops.push_back(s);
      }
    }
  }
  a.beatsPerBar = std::clamp(a.beatsPerBar, 1, 16);
  if (!a.beats.empty()) a.bar1 = std::clamp(a.bar1, 0, static_cast<int>(a.beats.size()) - 1);
  else a.bar1 = 0;
  return any;
}

// ================================================================== jobs

namespace {

struct Manager {
  std::mutex mu;
  std::map<std::string, JobPtr> jobs;
  std::vector<std::thread> threads;
  std::string cacheDir;
  ~Manager() {
    for (auto& [p, j] : jobs) j->cancel = true;
    for (auto& t : threads)
      if (t.joinable()) t.join();
  }
};
Manager& mgr() {
  static Manager m;
  return m;
}

std::string cacheKey(const std::string& path) {
  std::error_code ec;
  const auto size = fs::file_size(path, ec);
  const auto mt = fs::last_write_time(path, ec).time_since_epoch().count();
  const std::string s = std::string(kAnalysisVersion) + "|" + path + "|" + std::to_string(ec ? 0ull : static_cast<unsigned long long>(size)) + "|" + std::to_string(static_cast<long long>(mt));
  uint64_t h = 1469598103934665603ull;  // FNV-1a
  for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
  char b[32];
  std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(h));
  return b;
}

void runJob(const JobPtr& job, std::string cacheDir) {
  job->worker = std::this_thread::get_id();
  std::string err;
  SamplePtr s = sampleCache().get(job->path);
  if (!s || s->frames() == 0) {
    job->error = "Cannot decode the file";
    job->state = 2;
    return;
  }
  job->sample = s;
  job->progress = 0.05f;
  buildPeaks(*s, job->peaks);
  job->progress = 0.1f;
  const std::string key = cacheDir.empty() ? std::string() : cacheKey(job->path);
  if (!key.empty()) {
    std::ifstream in(fs::path(cacheDir) / (key + ".txt"));
    if (in) {
      std::stringstream ss;
      ss << in.rdbuf();
      Analysis a;
      if (analysisFromText(ss.str(), a) && a.frames == static_cast<int64_t>(s->frames()) && !a.beats.empty()) {
        job->result = std::move(a);
        job->fromCache = true;
        job->progress = 1;
        job->state = 1;
        return;
      }
    }
  }
  Analysis a;
  const AnalyseHints hints = std::getenv("SELTMANN_NO_HINTS") ? AnalyseHints{} : analyseHintsFor(job->path);
  // progress: analysis maps 0..1 to 0.1..1
  std::atomic<float> local{0};
  bool ok = false;
  {
    // forward progress through a small proxy (analyse writes 0..1)
    std::atomic<bool> stop{false};
    std::thread fwd([&] {
      while (!stop.load()) {
        job->progress = 0.1f + 0.9f * local.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
      }
    });
    ok = analyse(*s, a, &local, &job->cancel, hints);
    stop = true;
    fwd.join();
  }
  if (!ok) {
    job->error = job->cancel ? "Cancelled" : "Analysis failed";
    job->state = 2;
    return;
  }
  if (!key.empty()) {
    std::error_code ec;
    fs::create_directories(cacheDir, ec);
    std::ofstream out(fs::path(cacheDir) / (key + ".txt"));
    Analysis slim = a;
    slim.onsetEnv.clear();
    out << analysisToText(slim);
  }
  job->result = std::move(a);
  job->progress = 1;
  job->state = 1;
}

}  // namespace

JobPtr requestAnalysis(const std::string& path) {
  Manager& m = mgr();
  std::lock_guard<std::mutex> lk(m.mu);
  auto it = m.jobs.find(path);
  if (it != m.jobs.end() && it->second->state != 2) return it->second;
  auto job = std::make_shared<Job>();
  job->path = path;
  m.jobs[path] = job;
  m.threads.emplace_back(runJob, job, m.cacheDir);
  return job;
}

void forgetAnalysis(const std::string& path) {
  Manager& m = mgr();
  std::lock_guard<std::mutex> lk(m.mu);
  auto it = m.jobs.find(path);
  if (it != m.jobs.end() && it->second->state != 0) m.jobs.erase(it);
  if (!m.cacheDir.empty()) {
    std::error_code ec;
    fs::remove(fs::path(m.cacheDir) / (cacheKey(path) + ".txt"), ec);
  }
}

void setAnalysisCacheDir(const std::string& dir) {
  Manager& m = mgr();
  std::lock_guard<std::mutex> lk(m.mu);
  m.cacheDir = dir;
}

void dropAnalysisJobs() {
  Manager& m = mgr();
  std::lock_guard<std::mutex> lk(m.mu);
  for (auto it = m.jobs.begin(); it != m.jobs.end();) {
    if (it->second->state != 0) it = m.jobs.erase(it);
    else ++it;
  }
}

void waitAnalysisIdle() {
  Manager& m = mgr();
  std::vector<std::thread> ts;
  {
    std::lock_guard<std::mutex> lk(m.mu);
    ts.swap(m.threads);
  }
  for (auto& t : ts)
    if (t.joinable()) t.join();
}

}  // namespace gl::packs::seltmann
