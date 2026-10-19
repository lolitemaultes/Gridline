// Gridline — value curves of the reference delay (κ1 study), shared by the
// delay DSP, the FL importer, the editor read-outs and the tests. Arguments
// named `n` are the reference knob positions normalised to 0..1.
#pragma once
#include <algorithm>
#include <cmath>

namespace gl::dly {

// Piecewise-linear table lookup at position x (0..count-1).
inline double lerpTable(const double* t, int count, double x) {
  x = std::clamp(x, 0.0, static_cast<double>(count - 1));
  int i = std::min(static_cast<int>(x), count - 2);
  double f = x - i;
  return t[i] * (1 - f) + t[i + 1] * f;
}
inline double invLerpTable(const double* t, int count, double v) {  // monotonic increasing tables
  if (v <= t[0]) return 0;
  for (int i = 0; i + 1 < count; ++i)
    if (v <= t[i + 1]) return i + (v - t[i]) / std::max(1e-12, t[i + 1] - t[i]);
  return count - 1;
}

// Free time (seconds): ((0.94 n + 0.06)³ − 0.000216) · 1.000216 · 0.999 + 0.001 → 1 ms .. 1 s.
inline double freeTimeSec(double n) {
  double x = 0.94 * std::clamp(n, 0.0, 1.0) + 0.06;
  return (x * x * x - 0.000216) * 1.0002160466660799 * 0.999 + 0.001;
}
inline double freeTimeNorm(double sec) {
  double c = ((std::clamp(sec, 0.001, 1.0) - 0.001) / 0.999) / 1.0002160466660799 + 0.000216;
  return std::clamp((std::cbrt(c) - 0.06) / 0.94, 0.0, 1.0);
}

// Modulation period table (s); rate = 1 / lerp(table, n · 5.9994) → 0.113 .. 20 Hz.
inline constexpr double kModPeriods[7] = {8.872, 3.808, 1.833, 0.878, 0.398, 0.15, 0.05};
inline double modRateHz(double n) { return 1.0 / lerpTable(kModPeriods, 7, std::clamp(n, 0.0, 1.0) * 5.9994001388549805); }
inline double modRateNorm(double hz) {
  // periods decrease: search on the reversed table
  double p = 1.0 / std::max(1e-6, hz);
  for (int i = 0; i + 1 < 7; ++i)
    if (p <= kModPeriods[i] && p >= kModPeriods[i + 1])
      return std::clamp((i + (kModPeriods[i] - p) / (kModPeriods[i] - kModPeriods[i + 1])) / 5.9994001388549805, 0.0, 1.0);
  return p > kModPeriods[0] ? 0.0 : 1.0;
}

// Feedback filter cutoff tables (Hz) by mode: low pass / band pass (×3.6) and high pass.
inline constexpr double kLpCut[5] = {270, 480, 1300, 7000, 22000};
inline constexpr double kHpCut[5] = {20, 90, 400, 1300, 4200};
// mode: 0 low pass, 1 high pass, 2 band pass, 3 off.
inline double cutoffHz(int mode, double n) {
  n = std::clamp(n, 0.0, 1.0);
  if (mode == 1) return lerpTable(kHpCut, 5, n * 3.999);
  if (mode == 2) return lerpTable(kLpCut, 5, n * 3.6);
  return lerpTable(kLpCut, 5, n * 3.999);
}
inline double cutoffNorm(int mode, double hz) {
  if (mode == 1) return std::clamp(invLerpTable(kHpCut, 5, hz) / 3.999, 0.0, 1.0);
  if (mode == 2) return std::clamp(invLerpTable(kLpCut, 5, hz) / 3.6, 0.0, 1.0);
  return std::clamp(invLerpTable(kLpCut, 5, hz) / 3.999, 0.0, 1.0);
}
// Filter Q from resonance r (0..1): low / high pass 0.707 + r², band pass 0.107 + 1.15 r².
inline double filterQ(int mode, double r) {
  r = std::clamp(r, 0.0, 1.0);
  return mode == 2 ? 0.107 + 4.6 * (r * 0.5) * (r * 0.5) : 0.707 + 4.0 * (r * 0.5) * (r * 0.5);
}

// Bit depth: 2 + 14 (1 − √(1 − n)); n = 1 → off (16 bits).
inline double bits(double n) { return 2.0 + 14.0 * (1.0 - std::sqrt(1.0 - std::clamp(n, 0.0, 1.0))); }
inline double bitsNorm(double b) {
  double r = 1.0 - (std::clamp(b, 2.0, 16.0) - 2.0) / 14.0;
  return std::clamp(1.0 - r * r, 0.0, 1.0);
}
// Rate reduction: hold factor 1 + 0.7435x − 0.0171x² + 0.0002x³, x = 127 (1 − n); rate = sr / factor.
inline double rateFactor(double n) {
  double x = 127.0 * (1.0 - std::clamp(n, 0.0, 1.0));
  return std::max(1.0, 1.0 + 0.7434999942779541 * x - 0.017100000753998756 * x * x + 0.00019999999494757503 * x * x * x);
}
inline double rateHz(double n, double sr = 44100) { return sr / rateFactor(n); }
inline double rateNorm(double hz, double sr = 44100) {  // factor is monotonic in x
  double f = sr / std::max(1.0, hz);
  double lo = 0, hi = 1;
  for (int k = 0; k < 40; ++k) {
    double mid = 0.5 * (lo + hi);
    if (rateFactor(mid) > f) lo = mid;
    else hi = mid;
  }
  return 0.5 * (lo + hi);
}

// Distortion threshold (dB) and saturation make-up.
inline double distThreshDb(double n) { return -28.5 * std::clamp(n, 0.0, 1.0); }
inline double distMakeup(double n) {
  double p = 0.95 * std::clamp(n, 0.0, 1.0);
  double poly = 1.0 + 3.7032 * p * p - 7.5177 * p * p * p + 2.9843 * p * p * p * p;
  return std::pow(1.0 / std::max(1e-6, poly), 0.7070000171661377);
}

// Modulation depth: fraction n² · 0.1 of the delay time.
inline double modDepthFrac(double n) { return 0.1 * n * n; }

// Fake-stereo spread offset (samples at 44.1 kHz scale): 65 .. 655, shortened for delays under 15 ms.
inline double spreadSamples(double n, double sr, double timeSamples) {
  return (std::clamp(n, 0.0, 1.0) * 590.0 + 65.0) * (sr / 44100.0) * std::min(1.0, timeSamples / (sr * 0.015));
}

}  // namespace gl::dly
