// Gridline: harmonic / percussive separation (J-145). See Hpss.h.
#include "Hpss.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <signalsmith-linear/fft.h>

namespace gl::pitch {

namespace {
constexpr double kPi = 3.14159265358979323846;
using Cpx = std::complex<float>;

float medianOf(float* v, int n) {
  std::nth_element(v, v + n / 2, v + n);
  return v[n / 2];
}
}  // namespace

void harmonicPart(const float* const* x, int nch, int64_t n, double sampleRate, float* const* h, const HpssSettings& s) {
  if (nch <= 0 || n <= 0) return;
  for (int c = 0; c < nch; ++c) std::fill(h[c], h[c] + n, 0.0f);
  const double sr = sampleRate > 0 ? sampleRate : 44100.0;
  int N = 256;
  while (N < s.windowSec * sr && N < (1 << 16)) N <<= 1;
  const int hop = N / 4, half = N / 2;
  const int bins = half + 1;
  // Median lengths (odd), in frames and in bins.
  const int L = std::max(3, static_cast<int>(std::lround(s.timeSec * sr / hop)) | 1);
  const int K = std::max(3, static_cast<int>(std::lround(s.freqHz * N / sr)) | 1);
  const int L2 = L / 2, K2 = K / 2;
  // Frames start at t * hop - (N - hop): every input frame is covered by N / hop windows.
  const int64_t T = (n - 1 + N - hop) / hop + 1;
  std::vector<float> win(static_cast<size_t>(N));
  for (int i = 0; i < N; ++i) win[static_cast<size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2 * kPi * i / N));
  // Periodic Hann, analysis and synthesis, hop N/4: the squared windows sum to 1.5; the inverse FFT is unscaled.
  const float outScale = static_cast<float>(1.0 / (1.5 * N));
  signalsmith::linear::Pow2RealFFT<float> fft(static_cast<size_t>(N));
  const int CR = L2 + 1;  // complex spectra kept: the centre frame up to the newest
  std::vector<std::vector<Cpx>> spec(static_cast<size_t>(CR * nch), std::vector<Cpx>(static_cast<size_t>(half)));
  std::vector<std::vector<float>> mag(static_cast<size_t>(L), std::vector<float>(static_cast<size_t>(bins)));
  std::vector<float> seg(static_cast<size_t>(N)), buf(static_cast<size_t>(std::max(L, K))), mask(static_cast<size_t>(bins));
  std::vector<float> hm(static_cast<size_t>(bins)), pm(static_cast<size_t>(bins));
  std::vector<Cpx> tmp(static_cast<size_t>(half));
  const double bias2 = s.percussiveBias * s.percussiveBias;
  // Onset handling: the harmonic magnitudes of the last frame before the hit
  // (its window ends at or before it) cap the frames that reach the hit.
  static const std::vector<int64_t> kNone;
  const std::vector<int64_t>& on = s.onsets ? *s.onsets : kNone;
  size_t reached = 0, gov = SIZE_MAX;  // hits [0, reached) are within or behind the window; the governing one
  std::vector<float> ref(static_cast<size_t>(bins), 0.0f), prevH(static_cast<size_t>(bins), 0.0f);
  const double ramp = std::max(1.0, s.rampSec * sr);
  for (int64_t t = 0; t < T + L2; ++t) {
    if (t < T) {
      const int64_t start = t * hop - (N - hop);
      float* m = mag[static_cast<size_t>(t % L)].data();
      std::fill(m, m + bins, 0.0f);
      for (int c = 0; c < nch; ++c) {
        for (int i = 0; i < N; ++i) {
          const int64_t k = start + i;
          seg[static_cast<size_t>(i)] = k >= 0 && k < n ? x[c][k] * win[static_cast<size_t>(i)] : 0.0f;
        }
        Cpx* X = spec[static_cast<size_t>((t % CR) * nch + c)].data();
        fft.fft(seg.data(), X);
        // Bin 0 packs DC (real) and Nyquist (imaginary).
        m[0] += X[0].real() * X[0].real();
        m[half] += X[0].imag() * X[0].imag();
        for (int k = 1; k < half; ++k) m[k] += std::norm(X[k]);
      }
      for (int k = 0; k < bins; ++k) m[k] = std::sqrt(m[k]);
    }
    const int64_t cf = t - L2;  // the frame whose mask is complete
    if (cf < 0 || cf >= T) continue;
    const float* mc = mag[static_cast<size_t>(cf % L)].data();
    const int64_t f0 = std::max<int64_t>(0, cf - L2), f1 = std::min<int64_t>(T - 1, cf + L2);
    for (int k = 0; k < bins; ++k) {
      int q = 0;
      for (int64_t f = f0; f <= f1; ++f) buf[static_cast<size_t>(q++)] = mag[static_cast<size_t>(f % L)][static_cast<size_t>(k)];
      hm[static_cast<size_t>(k)] = medianOf(buf.data(), q);
      const int k0 = std::max(0, k - K2), k1 = std::min(bins - 1, k + K2);
      q = 0;
      for (int j = k0; j <= k1; ++j) buf[static_cast<size_t>(q++)] = mc[j];
      pm[static_cast<size_t>(k)] = medianOf(buf.data(), q);
    }
    // Soft masks of the 4th power: the parts go through different engines,
    // so what they share adds by power, not amplitude, after the stretch (a
    // half-and-half split of noise would lose 3 dB): the masks stay close to
    // 0 or 1 (a 2:1 magnitude ratio gives 94 %).
    for (int k = 0; k < bins; ++k) {
      const double a2 = static_cast<double>(hm[static_cast<size_t>(k)]) * hm[static_cast<size_t>(k)];
      const double b2 = bias2 * static_cast<double>(pm[static_cast<size_t>(k)]) * pm[static_cast<size_t>(k)];
      const double a = a2 * a2, b = b2 * b2;
      mask[static_cast<size_t>(k)] = a + b > 1e-30 ? static_cast<float>(a / (a + b)) : 1.0f;
    }
    const int64_t start = cf * hop - (N - hop);
    if (!on.empty()) {
      while (reached < on.size() && on[reached] < start + N) ++reached;
      if (reached > 0) {
        const size_t g = reached - 1;  // the latest hit the window has reached
        if (g != gov) gov = g, ref = prevH;  // the frame before was the last one entirely before it
        const auto o = static_cast<double>(on[g]);
        if (static_cast<double>(start) < o + ramp) {
          // 0 while the window holds the hit's start, then up to 1 over the ramp.
          const double r = std::clamp((static_cast<double>(start) - o) / ramp, 0.0, 1.0);
          for (int k = 0; k < bins; ++k) {
            const double x = mc[k];
            if (x <= 1e-20) continue;
            const double cap = 1.26 * ref[static_cast<size_t>(k)] + r * x;
            mask[static_cast<size_t>(k)] = static_cast<float>(std::min<double>(mask[static_cast<size_t>(k)], cap / x));
          }
        }
      }
      for (int k = 0; k < bins; ++k) prevH[static_cast<size_t>(k)] = mask[static_cast<size_t>(k)] * mc[k];
    }
    for (int c = 0; c < nch; ++c) {
      const Cpx* X = spec[static_cast<size_t>((cf % CR) * nch + c)].data();
      tmp[0] = Cpx(X[0].real() * mask[0], X[0].imag() * mask[static_cast<size_t>(half)]);
      for (int k = 1; k < half; ++k) tmp[static_cast<size_t>(k)] = X[k] * mask[static_cast<size_t>(k)];
      fft.ifft(tmp.data(), seg.data());
      float* y = h[c];
      const int i0 = static_cast<int>(std::max<int64_t>(0, -start)), i1 = static_cast<int>(std::min<int64_t>(N, n - start));
      for (int i = i0; i < i1; ++i) y[start + i] += seg[static_cast<size_t>(i)] * win[static_cast<size_t>(i)] * outScale;
    }
  }
  // No subnormal values (they stall the FPU in what processes the parts next).
  for (int c = 0; c < nch; ++c)
    for (int64_t i = 0; i < n; ++i)
      if (std::fabs(h[c][i]) < 1e-25f) h[c][i] = 0.0f;
}

}  // namespace gl::pitch
