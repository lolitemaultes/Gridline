// Gridline: loudness matching (J-140). See Level.h.
#include "Level.h"
#include <algorithm>
#include <cmath>

namespace gl::pitch {

namespace {
constexpr double kMaxGain = 3.981, kMinGain = 0.2512;  // ±12 dB
constexpr double kSilent = 1e-9;                       // mean square (-90 dBFS)
constexpr double kPi = 3.14159265358979323846;
// A boost may take the peak at most 3 dB over the source's own peak (a
// shift may change the crest factor, but not into a spike).
constexpr double kPeakRoom = 1.4125;
}  // namespace

void matchLevel(float* const* y, int nch, int64_t len, const float* const* x, int64_t n, const std::function<double(int64_t)>& srcOf,
                double sampleRate) {
  if (len <= 0 || n <= 0 || nch <= 0) return;
  const auto hop = std::max<int64_t>(64, static_cast<int64_t>(0.02 * sampleRate));
  const int64_t w = 2 * hop;
  auto hann = [](double u) { return u <= 0 || u >= 1 ? 0.0 : 0.5 - 0.5 * std::cos(2 * kPi * u); };
  // Gains at window centres hop/2 + k·hop.
  const int64_t windows = (len + hop - 1) / hop;
  std::vector<double> g(static_cast<size_t>(windows), 1.0), lim(static_cast<size_t>(windows), kMaxGain);
  double last = 1.0;
  for (int64_t k = 0; k < windows; ++k) {
    const int64_t c = k * hop + hop / 2;
    const int64_t a = std::max<int64_t>(0, c - w / 2), b = std::min(len, c + w / 2);
    double ey = 0, wy = 0, py = 0;
    for (int64_t i = a; i < b; ++i) {
      const double wi = hann((static_cast<double>(i - a) + 0.5) / static_cast<double>(b - a));
      for (int ch = 0; ch < nch; ++ch) {
        ey += wi * static_cast<double>(y[ch][i]) * y[ch][i];
        py = std::max(py, static_cast<double>(std::fabs(y[ch][i])));
      }
      wy += wi * nch;
    }
    ey /= std::max(wy, 1e-12);
    const auto sa = std::clamp<int64_t>(static_cast<int64_t>(std::floor(srcOf(a))), 0, n);
    const auto sb = std::clamp<int64_t>(static_cast<int64_t>(std::ceil(srcOf(b))), sa, n);
    double ex = 0, wx = 0, px = 0;
    for (int64_t i = sa; i < sb; ++i) {
      const double wi = hann((static_cast<double>(i - sa) + 0.5) / static_cast<double>(sb - sa));
      for (int ch = 0; ch < nch; ++ch) {
        ex += wi * static_cast<double>(x[ch][i]) * x[ch][i];
        px = std::max(px, static_cast<double>(std::fabs(x[ch][i])));
      }
      wx += wi * nch;
    }
    ex /= std::max(wx, 1e-12);
    if (ey > kSilent && ex > kSilent) last = std::clamp(std::sqrt(ex / ey), kMinGain, kMaxGain);
    g[static_cast<size_t>(k)] = last;
    // Never far above the source's own peak: a quiet, spiky result (a voice
    // whose harmonics left its formants) is not raised into clipping.
    if (py > 1e-9 && px > 0) lim[static_cast<size_t>(k)] = std::max(1.0, kPeakRoom * px / py);
  }
  // Smooth (three-point mean in the log domain), then interpolate per frame.
  std::vector<double> s(g.size());
  for (size_t k = 0; k < g.size(); ++k) {
    const double a = std::log(g[k > 0 ? k - 1 : k]), b = std::log(g[k]), c = std::log(g[k + 1 < g.size() ? k + 1 : k]);
    s[k] = std::exp((a + 2 * b + c) / 4);
    // The peak limit of this window and its neighbours (the interpolation reaches them).
    const double l = std::min({lim[k > 0 ? k - 1 : k], lim[k], lim[k + 1 < g.size() ? k + 1 : k]});
    s[k] = std::min(s[k], l);
  }
  for (int64_t i = 0; i < len; ++i) {
    const double p = (static_cast<double>(i) - static_cast<double>(hop / 2)) / static_cast<double>(hop);
    const auto k0 = std::clamp<int64_t>(static_cast<int64_t>(std::floor(p)), 0, windows - 1);
    const int64_t k1 = std::min(windows - 1, k0 + 1);
    const double f = std::clamp(p - static_cast<double>(k0), 0.0, 1.0);
    const double gi = s[static_cast<size_t>(k0)] + (s[static_cast<size_t>(k1)] - s[static_cast<size_t>(k0)]) * f;
    for (int ch = 0; ch < nch; ++ch) y[ch][i] = static_cast<float>(y[ch][i] * gi);
  }
}

void LevelFollower::apply(float* const* y, int nch, int m, double srcMs, double outMs, double peakLimit) {
  if (m <= 0) return;
  // Both levels smoothed alike (two one-pole stages over tau_ frames), so
  // their ratio follows the loudness, not the waveform.
  const double a = std::exp(-static_cast<double>(m) / tau_);
  src_[0] = src_[0] * a + srcMs * (1.0 - a);
  out_[0] = out_[0] * a + outMs * (1.0 - a);
  src_[1] = src_[1] * a + src_[0] * (1.0 - a);
  out_[1] = out_[1] * a + out_[0] * (1.0 - a);
  double target = gain_;
  if (out_[1] > kSilent && src_[1] > kSilent) target = std::clamp(std::sqrt(src_[1] / out_[1]), kMinGain, kMaxGain);
  // Move a third of the way per block (about 20 ms at 256-frame blocks), ramped.
  const double next = gain_ * std::pow(target / gain_, 1.0 / 3.0);
  // J-145: never far above the source's own peak (no new clipping), as a
  // limiter, not a step: each frame has the bound that keeps it within the
  // room (the block's peak frame gets kPeakRoom × peakLimit, quieter frames
  // more). The applied gain moves at most attack_ per frame towards a bound,
  // looking ahead within the block so it gets there in time, and recovers at
  // release_. It starts where the last block ended: it never steps.
  double pk = 0;
  if (peakLimit > 0)
    for (int c = 0; c < nch; ++c)
      for (int j = 0; j < m; ++j) pk = std::max(pk, static_cast<double>(std::fabs(y[c][j])));
  const double room = pk > 1e-12 ? std::max(1.0, kPeakRoom * peakLimit) * pk : 0.0;  // the largest |output| allowed
  auto boundAt = [&](int j) {
    if (room <= 0) return kMaxGain;
    double v = 0;
    for (int c = 0; c < nch; ++c) v = std::max(v, static_cast<double>(std::fabs(y[c][j])));
    return v * kMaxGain > room ? std::max(1.0, room / v) : kMaxGain;
  };
  const int reach = static_cast<int>(std::ceil(kMaxGain / attack_));  // further ahead, no bound pulls the gain down
  double env[kStep];
  for (int s0 = 0; s0 < m; s0 += kStep) {
    const int s1 = std::min(m, s0 + kStep);
    // Look-ahead bound: env[j] = min over k >= j of bound(k) + attack_ × (k - j).
    double e = kMaxGain;
    for (int j = std::min(m, s1 + reach) - 1; j >= s0; --j) {
      e = std::min(boundAt(j), e + attack_);
      if (j < s1) env[j - s0] = e;
    }
    for (int j = s0; j < s1; ++j) {
      const double want = gain_ + (next - gain_) * (j + 1) / m;
      const double g = std::max(std::min({want, env[j - s0], applied_ + release_}), applied_ - attack_);
      applied_ = g;
      for (int c = 0; c < nch; ++c) y[c][j] = static_cast<float>(y[c][j] * g);
    }
  }
  gain_ = next;
}

}  // namespace gl::pitch
