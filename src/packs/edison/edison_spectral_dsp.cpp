// Gridline — "edison" pack: frequency-domain and resampling tools — FFT,
// sample-rate conversion, time stretch / pitch shift (engine/TimeStretch,
// varispeed), noise reduction, drawn EQ, convolution
// reverb, blur, spectral editing and the spectrogram.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "../../engine/Dsp.h"
#include "../../engine/TimeStretch.h"
#include "../../engine/pitch/Kernel.h"
#include "edison.h"
#include "edison_internal.h"

namespace gl::packs::edison {

using namespace gl::dsp;

// ------------------------------------------------------------------ FFT
Fft::Fft(int n) : n_(n), tw_(static_cast<size_t>(n / 2)), rev_(static_cast<size_t>(n)) {
  for (int i = 0; i < n / 2; ++i) tw_[static_cast<size_t>(i)] = std::polar(1.0, -kTwoPi * i / n);
  int bits = 0;
  while ((1 << bits) < n) ++bits;
  for (int i = 0; i < n; ++i) {
    int r = 0;
    for (int b = 0; b < bits; ++b)
      if (i & (1 << b)) r |= 1 << (bits - 1 - b);
    rev_[static_cast<size_t>(i)] = r;
  }
}

void Fft::run(std::vector<cplx>& a, bool inv) const {
  const int n = n_;
  for (int i = 0; i < n; ++i) {
    int r = rev_[static_cast<size_t>(i)];
    if (i < r) std::swap(a[static_cast<size_t>(i)], a[static_cast<size_t>(r)]);
  }
  for (int len = 2; len <= n; len <<= 1) {
    const int step = n / len, h = len / 2;
    for (int i = 0; i < n; i += len)
      for (int j = 0; j < h; ++j) {
        cplx w = tw_[static_cast<size_t>(j * step)];
        if (inv) w = std::conj(w);
        cplx u = a[static_cast<size_t>(i + j)], v = a[static_cast<size_t>(i + j + h)] * w;
        a[static_cast<size_t>(i + j)] = u + v;
        a[static_cast<size_t>(i + j + h)] = u - v;
      }
  }
  if (inv)
    for (auto& v : a) v /= static_cast<double>(n);
}

std::vector<double> hannWindow(int n) {
  std::vector<double> w(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) w[static_cast<size_t>(i)] = 0.5 - 0.5 * std::cos(kTwoPi * i / n);
  return w;
}

namespace {

double besselI0(double x) {
  double sum = 1, term = 1, q = x * x / 4;
  for (int k = 1; k < 64; ++k) {
    term *= q / (static_cast<double>(k) * k);
    sum += term;
    if (term < sum * 1e-15) break;
  }
  return sum;
}

double kaiser(double t, double beta) {  // t in -1..1
  if (t <= -1 || t >= 1) return 0;
  return besselI0(beta * std::sqrt(1 - t * t)) / besselI0(beta);
}

}  // namespace

std::vector<double> firFromMagnitude(const std::vector<double>& mag, int n) {
  const Fft F(n);
  std::vector<cplx> s(static_cast<size_t>(n));
  for (int b = 0; b <= n / 2; ++b) {
    double m = b < static_cast<int>(mag.size()) ? mag[static_cast<size_t>(b)] : 0.0;
    s[static_cast<size_t>(b)] = m;
    if (b > 0 && b < n / 2) s[static_cast<size_t>(n - b)] = m;
  }
  F.inverse(s);
  const int M = n - 1, D = (M - 1) / 2;
  std::vector<double> f(static_cast<size_t>(M));
  for (int i = 0; i < M; ++i) {
    int src = ((i - D) % n + n) % n;
    f[static_cast<size_t>(i)] = s[static_cast<size_t>(src)].real() * kaiser(static_cast<double>(i - D) / (D + 1), 8.0);
  }
  return f;
}

std::vector<double> convolveReal(const float* x, int64_t n, const std::vector<double>& k) {
  const int64_t K = static_cast<int64_t>(k.size());
  std::vector<double> y(static_cast<size_t>(std::max<int64_t>(0, n + K - 1)), 0.0);
  if (n <= 0 || K <= 0) return y;
  if (K <= 64) {  // direct form for short kernels
    for (int64_t i = 0; i < n; ++i)
      for (int64_t j = 0; j < K; ++j) y[static_cast<size_t>(i + j)] += static_cast<double>(x[i]) * k[static_cast<size_t>(j)];
    return y;
  }
  const int N = nextPow2(std::max<int64_t>(2 * K, 4096));
  const int64_t B = N - K + 1;
  const Fft F(N);
  std::vector<cplx> H(static_cast<size_t>(N)), X(static_cast<size_t>(N));
  for (int64_t j = 0; j < K; ++j) H[static_cast<size_t>(j)] = k[static_cast<size_t>(j)];
  F.forward(H);
  for (int64_t s = 0; s < n; s += B) {
    std::fill(X.begin(), X.end(), cplx(0, 0));
    const int64_t m = std::min(B, n - s);
    for (int64_t i = 0; i < m; ++i) X[static_cast<size_t>(i)] = x[s + i];
    F.forward(X);
    for (int i = 0; i < N; ++i) X[static_cast<size_t>(i)] *= H[static_cast<size_t>(i)];
    F.inverse(X);
    for (int64_t i = 0; i < m + K - 1 && s + i < static_cast<int64_t>(y.size()); ++i) y[static_cast<size_t>(s + i)] += X[static_cast<size_t>(i)].real();
  }
  return y;
}

// ------------------------------------------------------------------ resampling
Audio resampleAudio(const Audio& a, double newRate, int quality) {
  Audio o;
  o.channels = a.channels;
  o.rate = newRate;
  if (a.rate <= 0 || newRate <= 0) return a;
  const double ratio = newRate / a.rate;
  if (std::fabs(ratio - 1.0) < 1e-12) {
    o = a;
    o.rate = newRate;
    return o;
  }
  const int64_t n = a.frames();
  const int64_t outN = static_cast<int64_t>(std::llround(static_cast<double>(n) * ratio));
  o.resize(outN);
  // J-140: the shared band-limited kernel (was a private Kaiser sinc).
  // Fast = realtime table, Normal and Best = offline table.
  const pitch::Quality q = std::clamp(quality, 0, 2) == 0 ? pitch::Quality::Realtime : pitch::Quality::Offline;
  if (a.channels == 1) {
    pitch::resample(a.L.data(), n, 0.0, 1.0 / ratio, o.L.data(), outN, q);
    o.R = o.L;
  } else {
    pitch::resample2(a.L.data(), a.R.data(), n, 0.0, 1.0 / ratio, o.L.data(), o.R.data(), outN, q);
  }
  return o;
}

Content resample(const Content& c, double newRate, int quality) {
  if (c.rate() <= 0 || newRate <= 0) return c;
  const double ratio = newRate / c.rate();
  Content o = c;
  auto map = [ratio](int64_t p) { return static_cast<int64_t>(std::llround(static_cast<double>(p) * ratio)); };
  for (auto& m : o.markers) m.pos = map(m.pos);
  for (auto& g : o.regions) g.start = map(g.start), g.end = map(g.end);
  if (o.hasLoop()) o.loopStart = map(o.loopStart), o.loopEnd = map(o.loopEnd);
  return withAudio(o, resampleAudio(c.a(), newRate, quality));
}

// ------------------------------------------------------------------ time stretch / pitch shift
const char* stretchName(Stretch s) {
  switch (s) {
    case Stretch::Elastic: return "Stretch";
    case Stretch::Tape: return "Resample";
    default: return "?";
  }
}

Audio stretchAudio(const Audio& a, double timeFactor, double semitones, Stretch mode, int quality) {
  const double P = std::pow(2.0, semitones / 12.0);
  Audio o;
  o.rate = a.rate;
  o.channels = a.channels;
  if (a.frames() == 0) return o;
  if (mode == Stretch::Tape) {
    // Varispeed: faster playback raises the pitch and shortens the sound.
    Audio t = a;
    t.rate = a.rate * P;
    o = resampleAudio(t, a.rate, quality);
    o.rate = a.rate;
    return o;
  }
  timeFactor = std::clamp(timeFactor, 0.05, 20.0);
  const int64_t target = static_cast<int64_t>(std::llround(static_cast<double>(a.frames()) * timeFactor));
  stretch::Settings set;
  set.mode = stretch::kStretch;
  set.pitch = P;
  const int nch = a.channels == 1 ? 1 : 2;
  o.L.assign(static_cast<size_t>(std::max<int64_t>(0, target)), 0.f);
  o.R.assign(o.L.size(), 0.f);
  const float* in[2] = {a.L.data(), a.R.data()};
  float* out[2] = {o.L.data(), o.R.data()};
  stretch::process(in, nch, a.frames(), out, target, a.rate, set);
  if (nch == 1) o.R = o.L;
  fixMono(o);
  return o;
}

Content stretch(const Content& c, Range r, double timeFactor, double semitones, Stretch mode, int quality) {
  r = rangeOrAll(c, r);
  if (r.empty()) return c;
  Audio seg = stretchAudio(extract(c.a(), r), timeFactor, semitones, mode, quality);
  Content o = replaceRange(c, r, seg);
  // Markers inside the range scale with it (replaceRange dropped them).
  const double f = r.len() > 0 ? static_cast<double>(seg.frames()) / static_cast<double>(r.len()) : 1.0;
  for (const auto& m : c.markers)
    if (m.pos > r.a && m.pos < r.b) o = addMarker(o, r.a + static_cast<int64_t>(std::llround((m.pos - r.a) * f)), m.name);
  return o;
}

// ------------------------------------------------------------------ noise reduction
NoiseProfile acquireNoise(const Audio& a, Range r) {
  NoiseProfile p;
  p.rate = a.rate;
  r.a = std::clamp<int64_t>(r.a, 0, a.frames());
  r.b = std::clamp<int64_t>(r.b, r.a, a.frames());
  if (r.len() < p.fft) return p;
  const Fft F(p.fft);
  const std::vector<double> w = hannWindow(p.fft);
  const int bins = p.fft / 2 + 1;
  std::vector<double> sum(static_cast<size_t>(bins), 0.0);
  std::vector<cplx> buf(static_cast<size_t>(p.fft));
  int count = 0;
  for (int64_t s = r.a; s + p.fft <= r.b; s += p.fft / 4) {
    for (int ch = 0; ch < 2; ++ch) {
      for (int i = 0; i < p.fft; ++i) buf[static_cast<size_t>(i)] = a.ch(ch)[s + i] * w[static_cast<size_t>(i)];
      F.forward(buf);
      for (int b = 0; b < bins; ++b) sum[static_cast<size_t>(b)] += std::abs(buf[static_cast<size_t>(b)]);
      ++count;
    }
  }
  p.mag.resize(static_cast<size_t>(bins));
  for (int b = 0; b < bins; ++b) p.mag[static_cast<size_t>(b)] = static_cast<float>(sum[static_cast<size_t>(b)] / std::max(1, count));
  return p;
}

Content denoise(const Content& c, Range r, const NoiseProfile& p, double reductionDb, double sensitivity) {
  r = rangeOrAll(c, r);
  if (!p.valid() || r.empty()) return c;
  Audio a = c.a();
  const int fft = p.fft, bins = fft / 2 + 1;
  const double floorG = dbToGain(-std::fabs(reductionDb));
  const double over = 1.0 + 3.0 * std::clamp(sensitivity, 0.0, 1.0);
  for (int ch = 0; ch < (a.channels == 1 ? 1 : 2); ++ch) {
    std::vector<double> prev(static_cast<size_t>(bins), 1.0);
    std::vector<float> y = stftProcess(a.ch(ch) + r.a, r.len(), fft, fft / 4, [&](int64_t, std::vector<cplx>& X) {
      for (int b = 0; b < bins; ++b) {
        double m = std::abs(X[static_cast<size_t>(b)]);
        double g = m > 1e-12 ? 1.0 - over * p.mag[static_cast<size_t>(b)] / m : 0.0;
        g = std::max(floorG, g);
        g = 0.7 * g + 0.3 * prev[static_cast<size_t>(b)];  // temporal smoothing (less "musical noise")
        prev[static_cast<size_t>(b)] = g;
        X[static_cast<size_t>(b)] *= g;
        if (b > 0 && b < fft / 2) X[static_cast<size_t>(fft - b)] = std::conj(X[static_cast<size_t>(b)]);
      }
    });
    std::copy(y.begin(), y.end(), a.ch(ch) + r.a);
  }
  return withAudio(c, std::move(a));
}

// ------------------------------------------------------------------ drawn EQ
double eqCurveDb(const std::vector<EqPoint>& pts, double hz) {
  if (pts.empty()) return 0.0;
  if (hz <= pts.front().hz) return pts.front().db;
  if (hz >= pts.back().hz) return pts.back().db;
  for (size_t k = 1; k < pts.size(); ++k)
    if (hz <= pts[k].hz) {
      double l0 = std::log(std::max(1e-3, pts[k - 1].hz)), l1 = std::log(std::max(1e-3, pts[k].hz));
      double t = l1 > l0 ? (std::log(hz) - l0) / (l1 - l0) : 1.0;
      return pts[k - 1].db + (pts[k].db - pts[k - 1].db) * t;
    }
  return pts.back().db;
}

Content equalize(const Content& c, Range r, const std::vector<EqPoint>& ptsIn) {
  r = rangeOrAll(c, r);
  if (r.empty() || ptsIn.empty()) return c;
  std::vector<EqPoint> pts = ptsIn;
  std::sort(pts.begin(), pts.end(), [](const EqPoint& x, const EqPoint& y) { return x.hz < y.hz; });
  Audio a = c.a();
  const int n = 8192;
  std::vector<double> mag(static_cast<size_t>(n / 2 + 1));
  for (int b = 0; b <= n / 2; ++b) mag[static_cast<size_t>(b)] = dbToGain(eqCurveDb(pts, std::max(1.0, b * a.rate / n)));
  const std::vector<double> f = firFromMagnitude(mag, n);
  const int64_t D = static_cast<int64_t>(f.size() - 1) / 2;
  for (int ch = 0; ch < (a.channels == 1 ? 1 : 2); ++ch) {
    std::vector<double> y = convolveReal(a.ch(ch) + r.a, r.len(), f);
    for (int64_t i = 0; i < r.len(); ++i) a.ch(ch)[r.a + i] = static_cast<float>(y[static_cast<size_t>(i + D)]);
  }
  return withAudio(c, std::move(a));
}

// ------------------------------------------------------------------ convolution, reverb, blur
Audio makeReverbIr(double rate, const ReverbSettings& s) {
  const double decay = std::clamp(s.decay, 0.05, 20.0);
  const int64_t pre = static_cast<int64_t>(std::max(0.0, s.predelayMs) * 0.001 * rate);
  const int64_t len = pre + static_cast<int64_t>(decay * rate * 1.1) + 1;
  Audio ir = makeAudio(rate, 2, len);
  uint32_t seed = s.seed ? s.seed : 1;
  auto rnd = [&seed]() {
    seed = seed * 1664525u + 1013904223u;
    return static_cast<double>(seed >> 8) / static_cast<double>(1u << 24) * 2.0 - 1.0;
  };
  const double k = std::log(1000.0) / (decay * rate);  // -60 dB after `decay`
  double lpL = 0, lpR = 0;
  for (int64_t i = 0; i < len - pre; ++i) {
    const double env = std::exp(-k * static_cast<double>(i));
    // Damping: the noise gets darker as the tail decays.
    const double t = static_cast<double>(i) / (decay * rate);
    const double coef = std::clamp(0.05 + 0.9 * (1.0 - s.damping * std::min(1.0, t * 1.5)), 0.02, 1.0);
    double nl = rnd(), nr = rnd();
    double ns = 0.5 * (nl + nr), wd = std::clamp(s.width, 0.0, 1.0);
    nl = ns + (nl - ns) * wd;
    nr = ns + (nr - ns) * wd;
    lpL += coef * (nl - lpL);
    lpR += coef * (nr - lpR);
    ir.L[static_cast<size_t>(pre + i)] = static_cast<float>(lpL * env);
    ir.R[static_cast<size_t>(pre + i)] = static_cast<float>(lpR * env);
  }
  // Unit energy per channel.
  for (int ch = 0; ch < 2; ++ch) {
    double e = 0;
    for (float v : (ch == 0 ? ir.L : ir.R)) e += static_cast<double>(v) * v;
    const double g = e > 0 ? 1.0 / std::sqrt(e) : 0.0;
    for (float& v : (ch == 0 ? ir.L : ir.R)) v = static_cast<float>(v * g);
  }
  return ir;
}

Audio convolveAudio(const Audio& x, const Audio& ir) {
  Audio o;
  o.rate = x.rate;
  o.channels = x.channels == 2 || ir.channels == 2 ? 2 : 1;
  const int64_t n = x.frames(), K = ir.frames();
  if (n == 0 || K == 0) return o;
  for (int ch = 0; ch < 2; ++ch) {
    std::vector<double> k(ir.ch(ch), ir.ch(ch) + K);
    std::vector<double> y = convolveReal(x.ch(ch), n, k);
    std::vector<float>& dst = ch == 0 ? o.L : o.R;
    dst.assign(y.begin(), y.end());
  }
  return o;
}

Content convolve(const Content& c, Range r, const Audio& irIn, double wet, double dry) {
  r = rangeOrAll(c, r);
  if (r.empty() || irIn.frames() == 0) return c;
  Audio ir = std::fabs(irIn.rate - c.rate()) > 1e-6 ? resampleAudio(irIn, c.rate(), 1) : irIn;
  const Audio seg = extract(c.a(), r);
  const Audio y = convolveAudio(seg, ir);
  Audio a = c.a();
  const int64_t need = r.a + y.frames();
  if (need > a.frames()) a.resize(need);
  if (ir.channels == 2 && a.channels == 1 && std::fabs(wet) > 0) a.channels = 2;  // a stereo room widens mono audio
  for (int ch = 0; ch < 2; ++ch) {
    float* d = a.ch(ch);
    const float* w = y.ch(ch);
    for (int64_t i = 0; i < y.frames(); ++i) {
      const int64_t p = r.a + i;
      const double dryPart = i < r.len() ? d[p] * (dry - 1.0) : 0.0;  // range: dry*x; tail: added onto what follows
      d[p] = static_cast<float>(d[p] + dryPart + wet * w[i]);
    }
  }
  Content o = c;
  return withAudio(o, std::move(a));
}

Content reverb(const Content& c, Range r, const ReverbSettings& s) {
  return convolve(c, r, makeReverbIr(c.rate(), s), s.wet, s.dry);
}

Content blur(const Content& c, Range r, double ms, double mix) {
  r = rangeOrAll(c, r);
  const int64_t len = std::max<int64_t>(2, static_cast<int64_t>(ms * 0.001 * c.rate()));
  Audio k = makeAudio(c.rate(), c.a().channels, len);
  uint32_t seed = 0x9E3779B9u;
  double e = 0;
  for (int64_t i = 0; i < len; ++i) {
    seed = seed * 1664525u + 1013904223u;
    double v = (static_cast<double>(seed >> 8) / static_cast<double>(1u << 24) * 2.0 - 1.0) *
               std::exp(-3.0 * static_cast<double>(i) / static_cast<double>(len));
    k.L[static_cast<size_t>(i)] = k.R[static_cast<size_t>(i)] = static_cast<float>(v);
    e += v * v;
  }
  for (int64_t i = 0; i < len; ++i) k.L[static_cast<size_t>(i)] = k.R[static_cast<size_t>(i)] = static_cast<float>(k.L[static_cast<size_t>(i)] / std::sqrt(e));
  mix = std::clamp(mix, 0.0, 1.0);
  return convolve(c, r, k, mix, 1.0 - mix);
}

// ------------------------------------------------------------------ spectral editing
Content spectralGain(const Content& c, const std::vector<SpectralRect>& rects, double gainDb) {
  if (rects.empty() || c.frames() == 0) return c;
  Audio a = c.a();
  const int fft = 2048, hop = fft / 4;
  const double g = dbToGain(gainDb);
  int64_t lo = c.frames(), hi = 0;
  for (const auto& q : rects) lo = std::min(lo, q.f0), hi = std::max(hi, q.f1);
  lo = std::max<int64_t>(0, lo - fft);
  hi = std::min<int64_t>(c.frames(), hi + fft);
  if (hi <= lo) return c;
  for (int ch = 0; ch < (a.channels == 1 ? 1 : 2); ++ch) {
    std::vector<float> y = stftProcess(a.ch(ch) + lo, hi - lo, fft, hop, [&](int64_t k, std::vector<cplx>& X) {
      const int64_t centre = lo + k * hop;
      for (const auto& q : rects) {
        if (centre < q.f0 || centre >= q.f1) continue;
        int b0 = std::max(0, static_cast<int>(std::floor(q.hz0 * fft / a.rate)));
        int b1 = std::min(fft / 2, static_cast<int>(std::ceil(q.hz1 * fft / a.rate)));
        for (int b = b0; b <= b1; ++b) {
          X[static_cast<size_t>(b)] *= g;
          if (b > 0 && b < fft / 2) X[static_cast<size_t>(fft - b)] *= g;
        }
      }
    });
    std::copy(y.begin(), y.end(), a.ch(ch) + lo);
  }
  return withAudio(c, std::move(a));
}

// ------------------------------------------------------------------ spectrogram
Spectrogram computeSpectrogram(const Audio& a, int fftSize, int maxCols) {
  Spectrogram s;
  s.fft = nextPow2(std::max(64, fftSize));
  s.bins = s.fft / 2;
  s.rate = a.rate;
  const int64_t n = a.frames();
  if (n == 0 || maxCols <= 0) return s;
  s.hop = std::max<int64_t>(s.fft / 4, (n + maxCols - 1) / maxCols);
  s.cols = static_cast<int>(n / s.hop + 1);
  s.db.assign(static_cast<size_t>(s.cols) * static_cast<size_t>(s.bins), 0);
  const Fft F(s.fft);
  const std::vector<double> w = hannWindow(s.fft);
  double wsum = 0;
  for (double v : w) wsum += v;
  std::vector<cplx> buf(static_cast<size_t>(s.fft));
  for (int c = 0; c < s.cols; ++c) {
    const int64_t c0 = static_cast<int64_t>(c) * s.hop - s.fft / 2;
    for (int i = 0; i < s.fft; ++i) {
      int64_t p = c0 + i;
      double v = p >= 0 && p < n ? 0.5 * (a.L[static_cast<size_t>(p)] + a.R[static_cast<size_t>(p)]) : 0.0;
      buf[static_cast<size_t>(i)] = v * w[static_cast<size_t>(i)];
    }
    F.forward(buf);
    for (int b = 0; b < s.bins; ++b) {
      double m = std::abs(buf[static_cast<size_t>(b)]) * 2.0 / wsum;
      double db = m > 1e-9 ? 20.0 * std::log10(m) : -180.0;
      s.db[static_cast<size_t>(c) * static_cast<size_t>(s.bins) + static_cast<size_t>(b)] =
          static_cast<uint8_t>(std::clamp((db + 120.0) / 120.0 * 255.0, 0.0, 255.0));
    }
  }
  return s;
}

}  // namespace gl::packs::edison
