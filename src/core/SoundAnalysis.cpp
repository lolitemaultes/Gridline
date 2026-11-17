// Gridline — K-218: tempo, key and sound kind from the audio. See SoundAnalysis.h.
#include "SoundAnalysis.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>

namespace gl::sound {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTarget = 11025.0;  // analysis rate
constexpr double kMaxSeconds = 30.0;

// Averages blocks of samples down to about 11 kHz and keeps the first 30 seconds.
std::vector<float> reduce(const std::vector<float>& in, double rate, double& outRate) {
  const int f = std::max(1, static_cast<int>(std::lround(rate / kTarget)));
  outRate = rate / f;
  const size_t n = std::min(in.size() / static_cast<size_t>(f), static_cast<size_t>(kMaxSeconds * outRate));
  std::vector<float> out(n);
  for (size_t i = 0; i < n; ++i) {
    float s = 0;
    for (int k = 0; k < f; ++k) s += in[i * static_cast<size_t>(f) + static_cast<size_t>(k)];
    out[i] = s / static_cast<float>(f);
  }
  return out;
}

constexpr int kHop = 128;

// Frame energies (window 2 hops).
std::vector<double> energies(const std::vector<float>& x) {
  std::vector<double> e;
  for (size_t p = 0; p + 2 * kHop <= x.size(); p += kHop) {
    double s = 0;
    for (size_t i = p; i < p + 2 * kHop; ++i) s += static_cast<double>(x[i]) * x[i];
    e.push_back(s / (2 * kHop));
  }
  return e;
}

// Onset strength: the rise of the log energy of the signal's upper half (its first
// difference), less its local mean.
std::vector<double> novelty(const std::vector<float>& x) {
  std::vector<float> d(x.size());
  for (size_t i = 1; i < x.size(); ++i) d[i] = x[i] - 0.5f * x[i - 1];
  const std::vector<double> e = energies(d);
  std::vector<double> n(e.size(), 0.0);
  for (size_t k = 1; k < e.size(); ++k) n[k] = std::max(0.0, std::log(1e-9 + e[k]) - std::log(1e-9 + e[k - 1]));
  std::vector<double> out(n.size(), 0.0);
  const int w = 8;
  for (size_t k = 0; k < n.size(); ++k) {
    double m = 0;
    int c = 0;
    for (int j = -w; j <= w; ++j) {
      const long i = static_cast<long>(k) + j;
      if (i >= 0 && i < static_cast<long>(n.size())) m += n[static_cast<size_t>(i)], ++c;
    }
    out[k] = std::max(0.0, n[k] - m / std::max(1, c));
  }
  return out;
}

double autoc(const std::vector<double>& v, size_t lag) {
  double s = 0;
  for (size_t i = 0; i + lag < v.size(); ++i) s += v[i] * v[i + lag];
  return s;
}

int onsetCount(const std::vector<double>& nov, double fps) {
  if (nov.empty()) return 0;
  const double mx = *std::max_element(nov.begin(), nov.end());
  if (mx <= 0) return 0;
  int c = 0;
  long last = -100000;
  const long gap = static_cast<long>(0.1 * fps);
  for (size_t k = 1; k + 1 < nov.size(); ++k)
    if (nov[k] > std::max(0.3 * mx, 0.7) && nov[k] >= nov[k - 1] && nov[k] >= nov[k + 1] && static_cast<long>(k) - last >= gap) ++c, last = static_cast<long>(k);
  return c;
}

double rmsOf(const std::vector<float>& x, size_t a, size_t b) {
  b = std::min(b, x.size());
  if (b <= a) return 0;
  double s = 0;
  for (size_t i = a; i < b; ++i) s += static_cast<double>(x[i]) * x[i];
  return std::sqrt(s / static_cast<double>(b - a));
}

// Brightness as a frequency: a sine of frequency f has a first difference 2 sin(pi f / r)
// times its own level.
double brightHz(const std::vector<float>& x, size_t a, size_t b, double r) {
  b = std::min(b, x.size());
  double d = 0, s = 0;
  for (size_t i = a + 1; i < b; ++i) d += static_cast<double>(x[i] - x[i - 1]) * (x[i] - x[i - 1]), s += static_cast<double>(x[i]) * x[i];
  if (s <= 0) return 0;
  const double ratio = std::min(2.0, std::sqrt(d / s));
  return std::asin(ratio / 2) * r / kPi;
}

// Pitch of a window by normalised autocorrelation: {f0, clarity 0..1}.
std::pair<double, double> pitchAt(const std::vector<float>& x, size_t at, double r) {
  const size_t win = 1024;
  const size_t lo = static_cast<size_t>(r / 1000.0), hi = static_cast<size_t>(r / 40.0);
  if (at + win + hi > x.size()) return {0, 0};
  double best = 0;
  size_t bestLag = 0;
  std::vector<double> c(hi + 2, 0.0);
  for (size_t L = lo; L <= hi + 1; ++L) {
    double xy = 0, xx = 0, yy = 0;
    for (size_t i = at; i < at + win; ++i) xy += static_cast<double>(x[i]) * x[i + L], xx += static_cast<double>(x[i]) * x[i], yy += static_cast<double>(x[i + L]) * x[i + L];
    c[L] = xx > 0 && yy > 0 ? xy / std::sqrt(xx * yy) : 0;
  }
  // The first lag that reaches nearly the best (avoids octave errors to longer lags).
  for (size_t L = lo + 1; L <= hi; ++L) best = std::max(best, c[L]);
  for (size_t L = lo + 1; L <= hi; ++L)
    if (c[L] >= 0.9 * best && c[L] >= c[L - 1] && c[L] >= c[L + 1]) {
      bestLag = L;
      break;
    }
  if (!bestLag || best <= 0) return {0, 0};
  const double a = c[bestLag - 1], b = c[bestLag], d = c[bestLag + 1];
  const double den = a - 2 * b + d;
  const double off = std::fabs(den) > 1e-12 ? 0.5 * (a - d) / den : 0.0;
  return {r / (static_cast<double>(bestLag) + off), std::clamp(b, 0.0, 1.0)};
}

// A sung line: pitched in the voice's range, gliding rather than stepping, with breaks.
bool looksVocal(const std::vector<float>& x, double r) {
  const size_t step = static_cast<size_t>(0.046 * r);
  const double total = rmsOf(x, 0, x.size());
  std::vector<double> f;
  int frames = 0, voiced = 0;
  for (size_t at = 0; at + 1024 + 300 < x.size(); at += step) {
    ++frames;
    if (rmsOf(x, at, at + 1024) < 0.3 * total) continue;
    const auto [f0, cl] = pitchAt(x, at, r);
    if (cl > 0.6 && f0 >= 90 && f0 <= 900) ++voiced, f.push_back(12 * std::log2(f0 / 440.0));
  }
  if (voiced < 6 || frames == 0) return false;
  const double share = static_cast<double>(voiced) / frames;
  if (share < 0.25 || share > 0.92) return false;
  std::vector<double> steps;
  for (size_t i = 1; i < f.size(); ++i) steps.push_back(std::fabs(f[i] - f[i - 1]));
  std::sort(steps.begin(), steps.end());
  const double med = steps[steps.size() / 2];
  const auto [mn, mx] = std::minmax_element(f.begin(), f.end());
  return med > 0.04 && med < 2.0 && (*mx - *mn) > 0.6;
}

constexpr std::array<double, 12> kMajor = {6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88};
constexpr std::array<double, 12> kMinor = {6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17};

double pearson(const std::array<double, 12>& a, const std::array<double, 12>& prof, int shift) {
  double ma = 0, mb = 0;
  for (int i = 0; i < 12; ++i) ma += a[static_cast<size_t>(i)], mb += prof[static_cast<size_t>(i)];
  ma /= 12, mb /= 12;
  double xy = 0, xx = 0, yy = 0;
  for (int i = 0; i < 12; ++i) {
    const double x = a[static_cast<size_t>((i + shift) % 12)] - ma, y = prof[static_cast<size_t>(i)] - mb;
    xy += x * y, xx += x * x, yy += y * y;
  }
  return xx > 0 && yy > 0 ? xy / std::sqrt(xx * yy) : 0;
}

}  // namespace

bool isPercussive(const std::string& k) {
  return k == "kick" || k == "snare" || k == "hat" || k == "cymbal" || k == "perc" || k == "drums";
}

double detectTempo(const std::vector<float>& mono, double rate, double seconds, double* confidence) {
  if (confidence) *confidence = 0;
  double r = 0;
  const std::vector<float> x = reduce(mono, rate, r);
  if (seconds <= 0) seconds = static_cast<double>(mono.size()) / rate;
  if (x.size() < static_cast<size_t>(1.5 * r)) return 0;
  const std::vector<double> nov = novelty(x);
  const double fps = r / kHop;
  if (onsetCount(nov, fps) < 4) return 0;
  const double a0 = autoc(nov, 0);
  if (a0 <= 0) return 0;
  const size_t lo = static_cast<size_t>(fps * 60.0 / 200.0), hi = std::min(nov.size() / 2, static_cast<size_t>(fps * 60.0 / 55.0));
  if (hi <= lo + 2) return 0;
  std::vector<double> ac(hi * 2 + 2, 0.0);
  for (size_t L = lo - 1; L < ac.size() && L < nov.size(); ++L) ac[L] = autoc(nov, L) / a0;
  double best = -1;
  size_t bestLag = 0;
  for (size_t L = lo; L <= hi; ++L) {
    const double bpm = 60.0 * fps / static_cast<double>(L);
    const double w = std::exp(-0.5 * std::pow(std::log2(bpm / 120.0) / 1.2, 2));
    const double s = (ac[L] + 0.5 * (2 * L < ac.size() ? ac[2 * L] : 0.0)) * w;
    if (s > best) best = s, bestLag = L;
  }
  if (!bestLag || ac[bestLag] < 0.08) return 0;
  const double a = ac[bestLag - 1], b = ac[bestLag], c = ac[bestLag + 1];
  const double den = a - 2 * b + c;
  const double off = std::fabs(den) > 1e-12 ? std::clamp(0.5 * (a - c) / den, -0.5, 0.5) : 0.0;
  double bpm = 60.0 * fps / (static_cast<double>(bestLag) + off);
  while (bpm < 70) bpm *= 2;
  while (bpm >= 180) bpm /= 2;
  // A loop's length settles it: a whole number of bars of 4/4 near the beat found.
  const double beats = seconds * bpm / 60.0;
  for (double bars : {1.0, 2.0, 4.0, 8.0, 16.0, 32.0}) {
    if (std::fabs(beats - bars * 4) < 0.6) {
      bpm = bars * 4 * 60.0 / seconds;
      break;
    }
  }
  if (std::fabs(bpm - std::round(bpm)) < 0.15) bpm = std::round(bpm);
  if (confidence) *confidence = std::clamp(b, 0.0, 1.0);
  return std::round(bpm * 100) / 100;
}

double kickTempo(const std::vector<float>& mono, double rate) {
  double r = 0;
  const std::vector<float> x = reduce(mono, rate, r);
  if (x.size() < static_cast<size_t>(1.0 * r)) {
    return 0;
  }
  // The low end only (two one-pole low-passes at 150 Hz): kicks, not hats or snares' noise.
  const float a = static_cast<float>(std::exp(-2 * kPi * 150.0 / r));
  std::vector<float> lp(x.size());
  float y1 = 0, y2 = 0;
  for (size_t i = 0; i < x.size(); ++i) {
    y1 = (1 - a) * x[i] + a * y1;
    y2 = (1 - a) * y1 + a * y2;
    lp[i] = y2;
  }
  const std::vector<double> e = energies(lp);
  if (e.size() < 8) {
    return 0;
  }
  const double emax = *std::max_element(e.begin(), e.end());
  if (emax < 1e-8) {
    return 0;
  }
  const double fps = r / kHop;
  // Onsets: a rise of the low end's level by 6 dB or more, to within 12 dB of its loudest, 0.15 s apart.
  std::vector<double> at;
  for (size_t k = 1; k + 1 < e.size(); ++k) {
    const double rise = std::log10((1e-12 + e[k]) / (1e-12 + e[k - 1])) * 10.0;
    if (rise < 6.0 || e[k + 1] < 0.06 * emax) {
      continue;
    }
    if (!at.empty() && (static_cast<double>(k) - at.back()) < 0.15 * fps) {
      continue;
    }
    at.push_back(static_cast<double>(k));
  }
  if (at.size() < 3) {
    return 0;
  }
  std::vector<double> ioi;
  for (size_t i = 1; i < at.size(); ++i) {
    ioi.push_back((at[i] - at[i - 1]) / fps);
  }
  std::vector<double> sorted = ioi;
  std::sort(sorted.begin(), sorted.end());
  const double med = sorted[sorted.size() / 2];
  // Steady: most gaps are the median one (or two of it: a kick left out).
  int steady = 0;
  double sum = 0;
  int n = 0;
  for (double g : ioi) {
    const double q = g / med;
    if (std::fabs(q - 1.0) < 0.12) {
      ++steady, sum += g, ++n;
    } else if (std::fabs(q - 2.0) < 0.2) {
      ++steady;
    }
  }
  if (n == 0 || steady * 10 < static_cast<int>(ioi.size()) * 7) {
    return 0;
  }
  const double bpm = 60.0 / (sum / n);
  return bpm >= 40.0 && bpm <= 260.0 ? bpm : 0.0;
}

bool detectKey(const std::vector<float>& mono, double rate, int& pitchClass, bool& minor, double* confidence) {
  if (confidence) *confidence = 0;
  double r = 0;
  const std::vector<float> x = reduce(mono, rate, r);
  const size_t win = 4096;
  if (x.size() < win) return false;
  std::array<double, 12> chroma{};
  std::vector<float> hann(win);
  for (size_t i = 0; i < win; ++i) hann[i] = static_cast<float>(0.5 - 0.5 * std::cos(2 * kPi * static_cast<double>(i) / (win - 1)));
  const size_t frames = (x.size() - win) / (win / 2) + 1;
  const size_t stride = std::max<size_t>(1, frames / 48);
  std::vector<float> w(win);
  for (size_t fr = 0; fr < frames; fr += stride) {
    const size_t at = fr * (win / 2);
    for (size_t i = 0; i < win; ++i) w[i] = x[at + i] * hann[i];
    for (int m = 36; m < 96; ++m) {
      const double f = 440.0 * std::pow(2.0, (m - 69) / 12.0);
      if (f >= r / 2) break;
      const double k = 2 * std::cos(2 * kPi * f / r);
      double s1 = 0, s2 = 0;
      for (size_t i = 0; i < win; ++i) {
        const double s0 = w[i] + k * s1 - s2;
        s2 = s1, s1 = s0;
      }
      const double p = s1 * s1 + s2 * s2 - k * s1 * s2;
      chroma[static_cast<size_t>(m % 12)] += std::sqrt(std::max(0.0, p));
    }
  }
  const double sum = std::accumulate(chroma.begin(), chroma.end(), 0.0);
  if (sum <= 0) return false;
  double best = -2, second = -2;
  int bk = -1;
  bool bm = false;
  for (int k = 0; k < 12; ++k)
    for (int mode = 0; mode < 2; ++mode) {
      const double c = pearson(chroma, mode ? kMinor : kMajor, k);
      if (c > best) second = best, best = c, bk = k, bm = mode == 1;
      else if (c > second) second = c;
    }
  if (confidence) *confidence = std::clamp(best, 0.0, 1.0);
  if (bk < 0 || best < 0.55) return false;
  pitchClass = bk;
  minor = bm;
  return true;
}

std::string detectKind(const std::vector<float>& mono, double rate) {
  double r = 0;
  const std::vector<float> x = reduce(mono, rate, r);
  if (x.size() < 1400) return "";
  const std::vector<double> e = energies(x);
  if (e.empty()) return "";
  const double emax = *std::max_element(e.begin(), e.end());
  if (emax < 1e-8) return "";
  size_t pk = 0;  // the attack: the first frame near the loudest (a held tone has no single peak)
  while (pk + 1 < e.size() && e[pk] < 0.7 * emax) ++pk;
  size_t end = pk;
  while (end < e.size() && e[end] > emax * 0.01) ++end;  // -20 dB in level
  const double decay = static_cast<double>(end - pk) * kHop / r;
  const double dur = static_cast<double>(x.size()) / r;
  const size_t a = pk * kHop, b = std::min(x.size(), a + static_cast<size_t>(0.2 * r));
  const double fc = brightHz(x, a, b, r);
  const size_t pAt = std::min(x.size() > 1400 ? x.size() - 1400 : 0, a + static_cast<size_t>(0.03 * r));
  const auto [f0, clarity] = pitchAt(x, pAt, r);
  const int onsets = onsetCount(novelty(x), r / kHop);
  if (onsets >= 4 && dur > 1.5) {  // a loop or a phrase
    if (looksVocal(x, r)) return "vocal";
    if (clarity < 0.5 || fc > 2500) return "drums";
    if (f0 > 0 && f0 < 180) return "bass";
    return "tonal";
  }
  const bool sustained = decay > 0.6;
  if (fc < 500 && decay < 0.45) return "kick";
  if (fc > 1500) {  // noise first: a drum's ringing tone does not make it a pitched sound
    if (fc > 3500) return decay < 0.3 ? "hat" : "cymbal";
    return decay < 0.5 ? "snare" : "fx";
  }
  if (clarity >= 0.7 && f0 > 0) {
    if (f0 < 220 && fc < 900) return "bass";
    if (sustained && looksVocal(x, r)) return "vocal";
    if (!sustained && decay < 0.3) return "perc";
    return "tonal";
  }
  if (fc > 3500) return decay < 0.3 ? "hat" : "cymbal";
  if (fc > 700) return decay < 0.5 ? "snare" : "fx";
  return sustained ? "fx" : "perc";
}

Analysis analyse(const std::vector<float>& mono, double rate, double seconds) {
  Analysis out;
  if (mono.empty() || rate <= 0) return out;
  out.kind = detectKind(mono, rate);
  if (out.kind.empty()) return out;
  out.bpm = detectTempo(mono, rate, seconds, &out.confidenceBpm);
  if (!isPercussive(out.kind) && !detectKey(mono, rate, out.key, out.minor, &out.confidenceKey)) out.key = -1;
  return out;
}

}  // namespace gl::sound
