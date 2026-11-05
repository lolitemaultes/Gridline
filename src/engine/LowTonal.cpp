// Gridline: K-61, see LowTonal.h. Runs on the clip renderer's thread, never on the audio thread.
#include "LowTonal.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace gl::stretch {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kAnalysisRate = 4000;    // Hz: the search and the analysis run on the low band
constexpr double kWindowSec = 0.06;       // two periods of a 33 Hz note
constexpr int kOverlap = 4;               // windows over each frame
constexpr double kSearchSec = 0.015;      // either way: a whole period of a 33 Hz note in all
constexpr double kPeakBand = 0.05;        // correlation peaks this close to the best are as good: the nearest to the map wins

// All channels summed to one.
std::vector<float> monoOf(const float* const* in, int nch, int64_t n) {
  std::vector<float> m(static_cast<size_t>(n), 0.0f);
  const float g = 1.0f / static_cast<float>(nch);
  for (int c = 0; c < nch; ++c)
    for (int64_t i = 0; i < n; ++i) m[static_cast<size_t>(i)] += g * in[c][i];
  return m;
}

// x low-passed (a Hann kernel `4 * d` long) and kept every d frames.
std::vector<float> decimate(const std::vector<float>& x, int d) {
  const int len = 4 * d;
  std::vector<float> h(static_cast<size_t>(len));
  double sum = 0;
  for (int i = 0; i < len; ++i) {
    h[static_cast<size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2 * kPi * (i + 0.5) / len));
    sum += h[static_cast<size_t>(i)];
  }
  const auto gain = static_cast<float>(1.0 / sum);
  std::vector<float> y(x.size() / static_cast<size_t>(d));
  const auto n = static_cast<int64_t>(x.size());
  for (size_t j = 0; j < y.size(); ++j) {
    const int64_t c = static_cast<int64_t>(j) * d - len / 2;
    float v = 0;
    for (int i = 0; i < len; ++i) {
      const int64_t k = c + i;
      if (k >= 0 && k < n) v += h[static_cast<size_t>(i)] * x[static_cast<size_t>(k)];
    }
    y[j] = v * gain;
  }
  return y;
}

int decimationFor(double sr) { return std::max(1, static_cast<int>(std::lround(sr / kAnalysisRate))); }

}  // namespace

bool lowTonal(const float* const* in, int nch, int64_t n, double sr) {
  const LowTonalMeasure m = measureLowTonal(in, nch, n, sr);
  return m.side <= 0.1 && m.low >= 0.75 && m.periodic >= 0.6;
}

LowTonalMeasure measureLowTonal(const float* const* in, int nch, int64_t n, double sr) {
  LowTonalMeasure res;
  if (n <= 0 || nch <= 0 || sr <= 0) return res;
  // Close to mono: what differs between the channels would be cut along with the rest.
  if (nch == 2) {
    double mid = 0, side = 0;
    for (int64_t i = 0; i < n; ++i) {
      const double a = in[0][i], b = in[1][i];
      mid += (a + b) * (a + b), side += (a - b) * (a - b);
    }
    res.side = side / std::max(mid, 1e-20);
  } else {
    res.side = 0;
  }
  const std::vector<float> mono = monoOf(in, nch, n);
  double full = 0;
  for (float v : mono) full += static_cast<double>(v) * v;
  if (full / static_cast<double>(n) < 1e-9) return res;  // silence
  const int dec = decimationFor(sr);
  const std::vector<float> low = decimate(mono, dec);
  const double rate = sr / dec;
  // Almost all of its energy low: below about 1 kHz.
  double lowE = 0;
  {
    const int k = std::max(1, static_cast<int>(rate / 1000));
    double run = 0;
    std::vector<float> smooth(low.size());
    for (size_t i = 0; i < low.size(); ++i) {
      run += low[i];
      if (i >= static_cast<size_t>(k)) run -= low[i - static_cast<size_t>(k)];
      smooth[i] = static_cast<float>(run / k);
      lowE += static_cast<double>(smooth[i]) * smooth[i];
    }
    lowE *= dec;
  }
  res.low = std::min(1.0, lowE / full);
  // Periodic, frame by frame, for most of its energy.
  const int frame = std::max(8, static_cast<int>(0.07 * rate));
  const int lagLo = std::max(1, static_cast<int>(rate / 300)), lagHi = static_cast<int>(rate / 30);
  const auto m = static_cast<int64_t>(low.size());
  if (m < static_cast<int64_t>(frame + lagHi)) return res;
  // About 200 frames at most (a whole song's stem takes no longer than a clip).
  const int64_t hop = std::max<int64_t>(static_cast<int64_t>(0.04 * rate), (m - frame - lagHi) / 200 + 1);
  double all = 0, periodic = 0;
  for (int64_t at = 0; at + frame + lagHi <= m; at += hop) {
    const float* p = low.data() + at;
    double e0 = 0;
    for (int i = 0; i < frame; ++i) e0 += static_cast<double>(p[i]) * p[i];
    all += e0;
    if (e0 < 1e-12) continue;
    double best = 0;
    for (int k = lagLo; k <= lagHi; ++k) {
      double c = 0, ek = 0;
      for (int i = 0; i < frame; ++i) c += static_cast<double>(p[i]) * p[i + k], ek += static_cast<double>(p[i + k]) * p[i + k];
      best = std::max(best, c / std::sqrt(e0 * ek + 1e-20));
    }
    if (best >= 0.8) periodic += e0;
  }
  res.periodic = all > 0 ? periodic / all : 0;
  return res;
}

void lowTonalStream(const float* const* in, int nch, int64_t n, float* const* out, int64_t len, double sr,
                    const std::function<double(int64_t)>& x, const std::function<void(int64_t)>* progress) {
  if (len <= 0 || nch <= 0) return;
  const int hop = std::max(1, static_cast<int>(std::lround(kWindowSec * sr / kOverlap)));
  const int win = hop * kOverlap;
  const int dec = decimationFor(sr);
  const int winLow = std::max(2, win / dec), searchLow = std::max(1, static_cast<int>(kSearchSec * sr) / dec);
  const std::vector<float> mono = monoOf(in, nch, n);
  const std::vector<float> low = decimate(mono, dec);
  const auto nLow = static_cast<int64_t>(low.size());
  const auto monoAt = [&](int64_t k) { return k >= 0 && k < n ? mono[static_cast<size_t>(k)] : 0.0f; };
  const auto lowAt = [&](int64_t k) { return k >= 0 && k < nLow ? low[static_cast<size_t>(k)] : 0.0f; };
  // A periodic Hann window: windows `hop` apart add up to 2 exactly.
  std::vector<float> hann(static_cast<size_t>(win));
  for (int i = 0; i < win; ++i) hann[static_cast<size_t>(i)] = 0.25f - 0.25f * static_cast<float>(std::cos(2 * kPi * i / win));
  // The map before frame 0 runs on at its starting rate (the audio that leads in).
  const double x0 = x(0);
  const auto probe = std::min<int64_t>(len, std::max<int64_t>(16, static_cast<int64_t>(0.01 * sr)));
  const double r0 = std::max(1e-4, (x(probe) - x0) / static_cast<double>(probe));
  const auto mapped = [&](int64_t q) { return q < 0 ? x0 + static_cast<double>(q) * r0 : x(q); };
  std::vector<double> corr(static_cast<size_t>(2 * searchLow + 1));
  const int refineStep = std::max(1, win / 512);
  int64_t prev = 0;
  bool first = true;
  for (int64_t q = hop - win; q < len; q += hop) {
    // The window is centred on where the map puts it.
    const auto target = static_cast<int64_t>(std::llround(mapped(q + win / 2))) - win / 2;
    int64_t start = target;
    const int64_t natural = prev + hop;  // what would continue the last window unbroken
    double eNatural = 0;
    if (!first) {
      const int64_t nl = natural / dec;
      for (int i = 0; i < winLow; ++i) eNatural += static_cast<double>(lowAt(nl + i)) * lowAt(nl + i);
    }
    if (!first && eNatural > 1e-12) {
      const int64_t nl = natural / dec, tl = target / dec;
      double top = -2;
      for (int k = -searchLow; k <= searchLow; ++k) {
        double c = 0, e = 0;
        for (int i = 0; i < winLow; ++i) {
          const double a = lowAt(nl + i), b = lowAt(tl + k + i);
          c += a * b, e += b * b;
        }
        const double v = c / std::sqrt(eNatural * e + 1e-20);
        corr[static_cast<size_t>(k + searchLow)] = v;
        top = std::max(top, v);
      }
      // Of the peaks about as good as the best (one per period), the nearest to the map.
      int nearest = 0, dist = 2 * searchLow + 2;
      const int last = 2 * searchLow;
      for (int j = 0; j <= last; ++j) {
        const double v = corr[static_cast<size_t>(j)];
        const bool peak = (j == 0 || v >= corr[static_cast<size_t>(j - 1)]) && (j == last || v >= corr[static_cast<size_t>(j + 1)]);
        if (peak && v >= top - kPeakBand && std::abs(j - searchLow) < dist) dist = std::abs(j - searchLow), nearest = j - searchLow;
      }
      // To the frame, on the whole band.
      const int64_t coarse = (tl + nearest) * dec;
      double bestFine = -2;
      for (int64_t s = coarse - dec; s <= coarse + dec; ++s) {
        double c = 0, ea = 0, eb = 0;
        for (int i = 0; i < win; i += refineStep) {
          const double a = monoAt(natural + i), b = monoAt(s + i);
          c += a * b, ea += a * a, eb += b * b;
        }
        const double v = c / std::sqrt(ea * eb + 1e-20);
        if (v > bestFine) bestFine = v, start = s;
      }
    }
    first = false;
    prev = start;
    const int64_t from = std::max<int64_t>(0, -q), to = std::min<int64_t>(win, len - q);
    for (int c = 0; c < nch; ++c)
      for (int64_t i = from; i < to; ++i) {
        const int64_t k = start + i;
        if (k >= 0 && k < n) out[c][q + i] += hann[static_cast<size_t>(i)] * in[c][k];
      }
    const int64_t done = std::min(len, q + hop);
    if (done > 0 && progress && *progress) (*progress)(done);
  }
}

}  // namespace gl::stretch
