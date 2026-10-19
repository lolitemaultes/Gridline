// Gridline — "edison" pack: analysis — levels (peak, RMS, BS.1770 loudness,
// true peak), pitch detection and tuning, onset / beat detection, tempo, and
// the min/max peak pyramid used to draw waveforms.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "../../engine/Dsp.h"
#include "edison.h"
#include "edison_internal.h"

namespace gl::packs::edison {

using namespace gl::dsp;

// ------------------------------------------------------------------ K-weighting
KWeight::KWeight(double fs) {
  {  // high shelf (+4 dB above ~1.7 kHz)
    const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
    const double K = std::tan(kPi * f0 / fs), Vh = std::pow(10.0, G / 20.0), Vb = std::pow(Vh, 0.4996667741545416);
    const double a0 = 1.0 + K / Q + K * K;
    b[0][0] = (Vh + Vb * K / Q + K * K) / a0;
    b[0][1] = 2.0 * (K * K - Vh) / a0;
    b[0][2] = (Vh - Vb * K / Q + K * K) / a0;
    a[0][0] = 1;
    a[0][1] = 2.0 * (K * K - 1.0) / a0;
    a[0][2] = (1.0 - K / Q + K * K) / a0;
  }
  {  // high pass (~38 Hz)
    const double f0 = 38.13547087602444, Q = 0.5003270373238773;
    const double K = std::tan(kPi * f0 / fs), a0 = 1.0 + K / Q + K * K;
    b[1][0] = 1;
    b[1][1] = -2;
    b[1][2] = 1;
    a[1][0] = 1;
    a[1][1] = 2.0 * (K * K - 1.0) / a0;
    a[1][2] = (1.0 - K / Q + K * K) / a0;
  }
}

double KWeight::run(double x) {
  for (int s = 0; s < 2; ++s) {  // transposed direct form II
    double y = b[s][0] * x + z[s][0];
    z[s][0] = b[s][1] * x - a[s][1] * y + z[s][1];
    z[s][1] = b[s][2] * x - a[s][2] * y;
    x = y;
  }
  return x;
}

namespace {

Range clampTo(const Audio& a, Range r) {
  r.a = std::clamp<int64_t>(r.a, 0, a.frames());
  r.b = std::clamp<int64_t>(r.b, r.a, a.frames());
  if (r.empty()) r = {0, a.frames()};
  return r;
}

// Mean-square energy of K-weighted audio per 100 ms step (both channels summed).
std::vector<double> loudnessSteps(const Audio& a, Range r, int64_t& step) {
  step = std::max<int64_t>(1, static_cast<int64_t>(std::llround(a.rate * 0.1)));
  KWeight kw[2] = {KWeight(a.rate), KWeight(a.rate)};
  const int chans = a.channels == 1 ? 1 : 2;
  std::vector<double> e;
  double acc = 0;
  int64_t cnt = 0;
  for (int64_t i = r.a; i < r.b; ++i) {
    for (int ch = 0; ch < chans; ++ch) {
      double y = kw[ch].run(a.ch(ch)[i]);
      acc += y * y;
    }
    if (++cnt == step) {
      e.push_back(acc);
      acc = 0;
      cnt = 0;
    }
  }
  if (cnt > 0) e.push_back(acc * static_cast<double>(step) / static_cast<double>(cnt));
  return e;
}

double lufsOf(double meanSquare) { return meanSquare > 1e-20 ? -0.691 + 10.0 * std::log10(meanSquare) : -180.0; }

}  // namespace

double integratedLufs(const Audio& a, Range r) {
  r = clampTo(a, r);
  if (r.empty()) return -180.0;
  int64_t step = 0;
  std::vector<double> e = loudnessSteps(a, r, step);
  // 400 ms blocks, 75 % overlap (4 steps each).
  std::vector<double> blocks;
  if (e.size() < 4) {
    double s = 0;
    for (double v : e) s += v;
    blocks.push_back(s / (static_cast<double>(e.size()) * static_cast<double>(step)));
  } else {
    for (size_t k = 0; k + 4 <= e.size(); ++k) blocks.push_back((e[k] + e[k + 1] + e[k + 2] + e[k + 3]) / (4.0 * static_cast<double>(step)));
  }
  double sum = 0;
  int n = 0;
  for (double b : blocks)
    if (lufsOf(b) > -70.0) sum += b, ++n;
  if (n == 0) return -180.0;
  const double rel = lufsOf(sum / n) - 10.0;
  sum = 0;
  n = 0;
  for (double b : blocks)
    if (lufsOf(b) > -70.0 && lufsOf(b) > rel) sum += b, ++n;
  return n ? lufsOf(sum / n) : -180.0;
}

double truePeakDb(const Audio& a, Range r) {
  r = clampTo(a, r);
  // 4x oversampling with a windowed-sinc interpolator (16 taps each side).
  const int H = 16;
  double coef[3][2 * H];
  for (int p = 1; p <= 3; ++p)
    for (int k = -H + 1; k <= H; ++k) {
      double t = static_cast<double>(k) - p / 4.0;  // distance from the interpolated point
      double s = std::sin(kPi * t) / (kPi * t);
      double w = 0.5 + 0.5 * std::cos(kPi * t / (H + 0.5));
      coef[p - 1][k + H - 1] = s * w;
    }
  double peak = 0;
  for (int ch = 0; ch < (a.channels == 1 ? 1 : 2); ++ch) {
    const float* x = a.ch(ch);
    for (int64_t i = r.a; i < r.b; ++i) {
      peak = std::max(peak, static_cast<double>(std::fabs(x[i])));
      for (int p = 0; p < 3; ++p) {
        double s = 0;
        for (int k = -H + 1; k <= H; ++k) {
          int64_t j = i + k;
          if (j >= r.a && j < r.b) s += x[j] * coef[p][k + H - 1];
        }
        peak = std::max(peak, std::fabs(s));
      }
    }
  }
  return gainToDb(peak);
}

Levels measure(const Audio& a, Range r) {
  Levels L;
  r = clampTo(a, r);
  if (r.empty()) return L;
  double peak = 0, sq = 0, sum = 0;
  const int chans = a.channels == 1 ? 1 : 2;
  for (int ch = 0; ch < chans; ++ch)
    for (int64_t i = r.a; i < r.b; ++i) {
      double v = a.ch(ch)[i];
      peak = std::max(peak, std::fabs(v));
      sq += v * v;
      sum += v;
    }
  const double n = static_cast<double>(r.len()) * chans;
  L.peakDb = gainToDb(peak);
  L.rmsDb = gainToDb(std::sqrt(sq / n));
  L.dc = sum / n;
  L.lufs = integratedLufs(a, r);
  L.truePeakDb = truePeakDb(a, r);
  int64_t step = 0;
  std::vector<double> e = loudnessSteps(a, r, step);
  for (size_t k = 0; k < e.size(); ++k) {
    size_t w = std::min<size_t>(30, e.size());
    if (k + w > e.size()) break;
    double s = 0;
    for (size_t j = k; j < k + w; ++j) s += e[j];
    L.shortTermMax = std::max(L.shortTermMax, lufsOf(s / (static_cast<double>(w) * static_cast<double>(step))));
  }
  return L;
}

// ------------------------------------------------------------------ pitch
std::string noteName(int midi) {
  static const char* kNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  if (midi < 0) return "-";
  char b[16];
  std::snprintf(b, sizeof b, "%s%d", kNames[midi % 12], midi / 12 - 1);
  return b;
}

namespace {

// YIN estimate over one frame; returns 0 when no clear period.
double yinFrame(const float* x, int W, double rate, double& conf) {
  const int tauMin = std::max(2, static_cast<int>(rate / 2000.0)), tauMax = std::min(W / 2, static_cast<int>(rate / 40.0));
  conf = 0;
  if (tauMax <= tauMin + 2) return 0;
  std::vector<double> d(static_cast<size_t>(tauMax + 1), 0.0);
  for (int tau = 1; tau <= tauMax; ++tau) {
    double s = 0;
    for (int i = 0; i < W - tauMax; ++i) {
      double q = static_cast<double>(x[i]) - x[i + tau];
      s += q * q;
    }
    d[static_cast<size_t>(tau)] = s;
  }
  std::vector<double> dn(d.size(), 1.0);
  double run = 0;
  for (int tau = 1; tau <= tauMax; ++tau) {
    run += d[static_cast<size_t>(tau)];
    dn[static_cast<size_t>(tau)] = run > 0 ? d[static_cast<size_t>(tau)] * tau / run : 1.0;
  }
  int best = -1;
  for (int tau = tauMin; tau < tauMax; ++tau)
    if (dn[static_cast<size_t>(tau)] < 0.15) {
      while (tau + 1 < tauMax && dn[static_cast<size_t>(tau + 1)] < dn[static_cast<size_t>(tau)]) ++tau;
      best = tau;
      break;
    }
  if (best < 0) {
    double m = 1e9;
    for (int tau = tauMin; tau < tauMax; ++tau)
      if (dn[static_cast<size_t>(tau)] < m) m = dn[static_cast<size_t>(tau)], best = tau;
    if (m > 0.4) return 0;
  }
  conf = std::clamp(1.0 - dn[static_cast<size_t>(best)], 0.0, 1.0);
  // Parabolic interpolation on the raw difference.
  double t = best;
  if (best > 1 && best < tauMax) {
    double y0 = dn[static_cast<size_t>(best - 1)], y1 = dn[static_cast<size_t>(best)], y2 = dn[static_cast<size_t>(best + 1)];
    double den = y0 - 2 * y1 + y2;
    if (std::fabs(den) > 1e-12) t += 0.5 * (y0 - y2) / den;
  }
  return rate / t;
}

}  // namespace

PitchInfo detectPitch(const Audio& a, Range r) {
  PitchInfo p;
  r = clampTo(a, r);
  const int W = static_cast<int>(std::min<int64_t>(r.len(), static_cast<int64_t>(std::max(2048.0, a.rate / 40.0 * 3))));
  if (W < 256) return p;
  std::vector<float> mono(static_cast<size_t>(r.len()));
  for (int64_t i = 0; i < r.len(); ++i) mono[static_cast<size_t>(i)] = 0.5f * (a.L[static_cast<size_t>(r.a + i)] + a.R[static_cast<size_t>(r.a + i)]);
  std::vector<std::pair<double, double>> est;
  const int frames = static_cast<int>(std::min<int64_t>(9, std::max<int64_t>(1, r.len() / W)));
  for (int f = 0; f < frames; ++f) {
    int64_t s = frames == 1 ? 0 : (r.len() - W) * f / (frames - 1);
    double conf = 0, hz = yinFrame(mono.data() + s, W, a.rate, conf);
    if (hz > 0) est.push_back({hz, conf});
  }
  if (est.empty()) return p;
  std::sort(est.begin(), est.end());
  p.hz = est[est.size() / 2].first;
  double cs = 0;
  for (auto& e : est) cs += e.second;
  p.confidence = cs / static_cast<double>(est.size());
  double midi = 69.0 + 12.0 * std::log2(p.hz / 440.0);
  p.note = static_cast<int>(std::lround(midi));
  p.cents = (midi - p.note) * 100.0;
  return p;
}

Content tuneToNote(const Content& c, Range r, int note, PitchInfo* found) {
  r = rangeOrAll(c, r);
  PitchInfo p = detectPitch(c.a(), r);
  if (found) *found = p;
  if (p.hz <= 0) return c;
  if (note < 0) note = p.note;
  const double target = 440.0 * std::pow(2.0, (note - 69) / 12.0);
  const double semis = 12.0 * std::log2(target / p.hz);
  if (std::fabs(semis) < 1e-4) return c;
  return stretch(c, r, 1.0, semis, Stretch::Elastic, 2);
}

// ------------------------------------------------------------------ onsets / tempo
std::vector<int64_t> detectOnsets(const Audio& a, Range r, double sensitivity) {
  std::vector<int64_t> out;
  r = clampTo(a, r);
  const int fft = 1024, hop = 256;
  if (r.len() < fft) return out;
  std::vector<float> mono(static_cast<size_t>(r.len()));
  for (int64_t i = 0; i < r.len(); ++i) mono[static_cast<size_t>(i)] = 0.5f * (a.L[static_cast<size_t>(r.a + i)] + a.R[static_cast<size_t>(r.a + i)]);
  const Fft F(fft);
  const std::vector<double> w = hannWindow(fft);
  const int bins = fft / 2;
  const int64_t frames = r.len() / hop + 1;
  std::vector<double> flux(static_cast<size_t>(frames), 0.0), prev(static_cast<size_t>(bins), 0.0);
  std::vector<cplx> buf(static_cast<size_t>(fft));
  for (int64_t k = 0; k < frames; ++k) {
    const int64_t c0 = k * hop - fft / 2;
    for (int i = 0; i < fft; ++i) {
      int64_t s = c0 + i;
      buf[static_cast<size_t>(i)] = s >= 0 && s < r.len() ? mono[static_cast<size_t>(s)] * w[static_cast<size_t>(i)] : 0.0;
    }
    F.forward(buf);
    double f = 0;
    for (int b = 1; b < bins; ++b) {
      double m = std::log1p(100.0 * std::abs(buf[static_cast<size_t>(b)]));
      f += std::max(0.0, m - prev[static_cast<size_t>(b)]);
      prev[static_cast<size_t>(b)] = m;
    }
    flux[static_cast<size_t>(k)] = f;
  }
  double mx = *std::max_element(flux.begin(), flux.end());
  if (mx <= 0) return out;
  const double sens = std::clamp(sensitivity, 0.0, 1.0);
  const double delta = mx * (0.30 - 0.25 * sens);
  const int64_t minGap = static_cast<int64_t>(a.rate * 0.05) / hop + 1;
  int64_t last = -minGap * 2;
  std::vector<int64_t> peaks;
  for (int64_t k = 1; k + 1 < frames; ++k) {
    double v = flux[static_cast<size_t>(k)];
    bool localMax = true;
    for (int64_t j = std::max<int64_t>(0, k - 3); j <= std::min(frames - 1, k + 3); ++j)
      if (flux[static_cast<size_t>(j)] > v) localMax = false;
    if (!localMax) continue;
    std::vector<double> win;
    for (int64_t j = std::max<int64_t>(0, k - 8); j <= std::min(frames - 1, k + 8); ++j) win.push_back(flux[static_cast<size_t>(j)]);
    std::nth_element(win.begin(), win.begin() + static_cast<long>(win.size() / 2), win.end());
    if (v > win[win.size() / 2] * 1.5 + delta && k - last >= minGap) {
      peaks.push_back(k);
      last = k;
    }
  }
  // Refine each detection to the attack start (first rise of a short RMS envelope).
  const int E = 32;
  int64_t prevOn = -1;
  for (int64_t k : peaks) {
    const int64_t centre = k * hop;
    int64_t s0 = std::max<int64_t>(prevOn < 0 ? 0 : prevOn + E, centre - fft), s1 = std::min<int64_t>(r.len(), centre + fft / 2);
    if (s1 - s0 < 2 * E) continue;
    std::vector<double> env(static_cast<size_t>(s1 - s0), 0.0);
    double acc = 0;
    for (int64_t i = s0; i < s1; ++i) {
      double v = mono[static_cast<size_t>(i)];
      acc += v * v;
      if (i - s0 >= E) {
        double o = mono[static_cast<size_t>(i - E)];
        acc -= o * o;
      }
      env[static_cast<size_t>(i - s0)] = std::sqrt(std::max(0.0, acc) / E);
    }
    size_t pk = static_cast<size_t>(std::max_element(env.begin(), env.end()) - env.begin());
    double base = *std::min_element(env.begin(), env.begin() + static_cast<long>(pk) + 1);
    double thr = base + 0.15 * (env[pk] - base);
    size_t on = 0;
    while (on < pk && env[on] < thr) ++on;
    // The envelope lags its window: step back to where the signal itself rises.
    int64_t pos = s0 + static_cast<int64_t>(on);
    int64_t first = pos;
    for (int64_t i = std::max(s0, pos - E); i <= pos; ++i)
      if (std::fabs(mono[static_cast<size_t>(i)]) > thr * 0.5) {
        first = i;
        break;
      }
    out.push_back(r.a + first);
    prevOn = first;
  }
  return out;
}

double estimateTempo(const std::vector<int64_t>& onsets, double rate) {
  if (onsets.size() < 3 || rate <= 0) return 0;
  std::vector<double> bpm;
  for (size_t i = 1; i < onsets.size(); ++i) {
    double dt = static_cast<double>(onsets[i] - onsets[i - 1]) / rate;
    if (dt <= 0.02) continue;
    double b = 60.0 / dt;
    while (b < 70) b *= 2;
    while (b > 180) b /= 2;
    bpm.push_back(b);
  }
  if (bpm.empty()) return 0;
  std::sort(bpm.begin(), bpm.end());
  return bpm[bpm.size() / 2];
}

// ------------------------------------------------------------------ peak pyramid
void PeakCache::build(const Audio& a) {
  levels_.clear();
  frames_ = a.frames();
  int64_t block = kBase;
  while (frames_ / block >= 2 || levels_.empty()) {
    Level L;
    L.block = block;
    const int64_t nb = (frames_ + block - 1) / block;
    for (int ch = 0; ch < 2; ++ch) {
      L.mn[ch].assign(static_cast<size_t>(nb), 0.f);
      L.mx[ch].assign(static_cast<size_t>(nb), 0.f);
      if (levels_.empty()) {
        const float* x = a.ch(ch);
        for (int64_t b = 0; b < nb; ++b) {
          float mn = 0, mx = 0;
          bool first = true;
          for (int64_t i = b * block; i < std::min(frames_, (b + 1) * block); ++i) {
            if (first) mn = mx = x[i], first = false;
            mn = std::min(mn, x[i]);
            mx = std::max(mx, x[i]);
          }
          L.mn[ch][static_cast<size_t>(b)] = mn;
          L.mx[ch][static_cast<size_t>(b)] = mx;
        }
      } else {
        const Level& P = levels_.back();
        const int64_t pn = static_cast<int64_t>(P.mn[ch].size());
        for (int64_t b = 0; b < nb; ++b) {
          float mn = 1e9f, mx = -1e9f;
          for (int64_t j = b * kFactor; j < std::min(pn, (b + 1) * kFactor); ++j) {
            mn = std::min(mn, P.mn[ch][static_cast<size_t>(j)]);
            mx = std::max(mx, P.mx[ch][static_cast<size_t>(j)]);
          }
          L.mn[ch][static_cast<size_t>(b)] = mn;
          L.mx[ch][static_cast<size_t>(b)] = mx;
        }
      }
    }
    levels_.push_back(std::move(L));
    if (frames_ / block < 2) break;
    block *= kFactor;
  }
}

void PeakCache::query(const Audio& a, int c, int64_t f0, int64_t f1, float& mn, float& mx) const {
  f0 = std::clamp<int64_t>(f0, 0, a.frames());
  f1 = std::clamp<int64_t>(f1, f0, a.frames());
  mn = mx = 0;
  if (f1 <= f0) {
    if (f0 < a.frames()) mn = mx = a.ch(c)[f0];
    return;
  }
  const int64_t span = f1 - f0;
  const Level* L = nullptr;
  for (const auto& l : levels_)
    if (l.block * 2 <= span) L = &l;
  if (!L || frames_ != a.frames()) {
    const float* x = a.ch(c);
    mn = mx = x[f0];
    for (int64_t i = f0 + 1; i < f1; ++i) mn = std::min(mn, x[i]), mx = std::max(mx, x[i]);
    return;
  }
  const int64_t b0 = f0 / L->block, b1 = (f1 - 1) / L->block;
  mn = 1e9f;
  mx = -1e9f;
  for (int64_t b = b0; b <= b1 && b < static_cast<int64_t>(L->mn[c].size()); ++b) {
    mn = std::min(mn, L->mn[c][static_cast<size_t>(b)]);
    mx = std::max(mx, L->mx[c][static_cast<size_t>(b)]);
  }
}

}  // namespace gl::packs::edison
