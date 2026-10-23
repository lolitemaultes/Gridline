// Gridline — "dynamics" pack, J-26: peak-emphasis limiter of gl.enhance
// (EnCustom = 2; behaviour study). Linked peak
// detector scaled by 1 - e²/2, ~25 ms moving maximum, attack / two-speed
// release envelope, gain to a -0.1 dB ceiling on a 2 ms look-ahead, then a hard
// or soft clipper, output gain, optional level matching and a bypass fade.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>

namespace gl::packs::dynamics::emref {

inline constexpr double kTwoPi = 6.283185307179586;
inline double outLin(double n) { return n >= 1 ? 1.0 : (std::pow(11.0, std::clamp(n, 0.0, 1.0)) - 1) / 10.0; }

struct Settings {
  double inDb = 0, emphasis = 0.5, outGain = 1;
  bool soft = false, match = false, bypass = false;
};

class Limiter {
 public:
  void reset() {
    for (auto& d : dl_) d.fill(0.f);
    for (auto& d : dry_) d.fill(0.f);
    held_.fill(0), cnt_.fill(0);
    env_ = 0, gHold_ = 1, gPrev_ = 1, fast_ = false, w_ = 0, primed_ = false;
  }
  static int latency(double sr) { return static_cast<int>(std::lround(0.002 * sr)); }
  void process(const Settings& s, double sr, float* L, float* R, int n) {
    if (sr != sr_) sr_ = sr, reset();
    const int D = std::min(latency(sr), static_cast<int>(kLen) - 1);
    const int H = std::max(1, static_cast<int>(std::lround(0.025 * sr / 8)));
    const double c20 = std::exp(-kTwoPi * 20 / sr);
    const double q = s.emphasis * s.emphasis, k = 1 - q / 2;
    const double aAtt = std::exp(-kTwoPi / (sr * (4 * q + 0.002)));
    const double relT = 3 - 3 * q + 0.1, aSlow = std::exp(-kTwoPi / (sr * relT)), aFast = std::exp(-kTwoPi / (sr * relT / 4));
    const double ceil = 0.9885531, t = 0.84139514164519513;  // -0.1 dB, -1.5 dB
    const double gInT = std::pow(10.0, s.inDb / 20), bT = s.bypass ? 0.0 : 1.0;
    if (!primed_) gIn_ = gInT, gOut_ = s.outGain, b_ = bT, primed_ = true;
    for (int i = 0; i < n; ++i) {
      gIn_ = c20 * gIn_ + (1 - c20) * gInT;
      gOut_ = c20 * gOut_ + (1 - c20) * s.outGain;
      b_ = c20 * b_ + (1 - c20) * bT;
      const double xL = L[i], xR = R[i];
      const double A = xL * gIn_, B = xR * gIn_;
      // moving maximum (8 held stages)
      double p = std::max(std::fabs(A), std::fabs(B)) * k;
      for (size_t st = 0; st < 8; ++st) {
        if (p >= held_[st] || cnt_[st] <= 0) held_[st] = p, cnt_[st] = H;
        else --cnt_[st];
        p = held_[st];
      }
      const double a = p > env_ ? aAtt : (fast_ ? aFast : aSlow);
      env_ = a * env_ + (1 - a) * p;
      const double g = ceil / std::max(env_, ceil);
      if (g <= gPrev_) gHold_ = g;  // the gain at the start of each release
      gPrev_ = g;
      const double r = std::max(1.0, 0.5 * p / (gHold_ * k));
      fast_ = g * g < gHold_ / r;
      // look-ahead
      const size_t rp = (w_ + kLen - static_cast<size_t>(D)) % kLen;
      dl_[0][w_] = static_cast<float>(A), dl_[1][w_] = static_cast<float>(B);
      dry_[0][w_] = static_cast<float>(xL), dry_[1][w_] = static_cast<float>(xR);
      const double ys[2] = {dl_[0][rp] * g, dl_[1][rp] * g}, ds[2] = {dry_[0][rp], dry_[1][rp]};
      w_ = (w_ + 1) % kLen;
      double out[2];
      for (int c = 0; c < 2; ++c) {
        double y = ys[c];
        if (!s.soft) {
          y = std::clamp(y, -1.0, 1.0);
        } else if (std::fabs(y) > t) {
          const double u = (std::fabs(y) - t) / (1 - t);
          y = std::copysign(t + (1 - t) * u / std::sqrt(1 + u * u), y);
        }
        y *= gOut_;
        if (s.match) y /= std::max(1e-9, gIn_);
        out[c] = (ds[c] + (y - ds[c]) * b_) * 0.9999;
      }
      L[i] = static_cast<float>(out[0]);
      R[i] = static_cast<float>(out[1]);
    }
  }
  double gainReductionDb() const { return 20 * std::log10(std::max(1e-9, gPrev_)); }

 private:
  static constexpr size_t kLen = 512;  // ≥ 2 ms at 192 kHz
  std::array<std::array<float, kLen>, 2> dl_{}, dry_{};
  std::array<double, 8> held_{};
  std::array<int, 8> cnt_{};
  double sr_ = 0, env_ = 0, gHold_ = 1, gPrev_ = 1, gIn_ = 1, gOut_ = 1, b_ = 1;
  bool fast_ = false, primed_ = false;
  size_t w_ = 0;
};

}  // namespace gl::packs::dynamics::emref
