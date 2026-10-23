// Gridline — "spacefx" pack, J-26: reference engine of gl.ensemble
// (EnModel = 1; behaviour study). Two lanes of
// four voices (the second lane only in 8-line mode); every voice reads its own
// row of a range table for delay, modulation rate / depth and movement rate.
// Movement balances each voice's input between the two lines (equal power);
// lines read with 4-point Hermite interpolation; wet → low pass → high pass
// (10 ms crossfade on / off) → equal-power dry / wet law. Each knob position
// glides per sample (90 % in 50 ms). Fixed-size lines, no allocation.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>

namespace gl::packs::spacefx::hcref {

inline constexpr double kPi = 3.14159265358979323846;
struct Row {
  float d0, d1, r0, r1, m0, m1;
};
inline constexpr Row kRows[8] = {{2.0f, 6.0f, 1.50f, 2.47f, 0.33f, 3.56f},  {3.5f, 8.25f, 1.57f, 3.41f, 0.29f, 2.66f},
                                 {6.5f, 10.0f, 0.38f, 3.64f, 0.26f, 3.78f}, {5.0f, 12.8f, 1.29f, 3.68f, 0.14f, 2.98f},
                                 {7.1f, 16.6f, 3.41f, 5.03f, 0.44f, 5.06f}, {9.1f, 15.1f, 2.47f, 5.37f, 0.66f, 8.08f},
                                 {8.3f, 16.8f, 3.01f, 6.02f, 0.93f, 6.02f}, {9.6f, 17.7f, 3.30f, 6.50f, 0.41f, 10.22f}};
inline double lowPassHz(double n) { return 714.0 * std::pow(2.0, 4.9454355 * std::clamp(n, 0.0, 1.0)); }
inline double highPassHz(double n) { return n <= 0 ? 0.0 : 12.0 * std::pow(2.0, 6.5051498 * std::clamp(n, 0.0, 1.0)); }
inline double wetShare(double n) {
  n = std::clamp(n, 0.0, 1.0);
  return n < 0.5 ? 0.5 * (2 * n) * (2 * n) : 1 - 0.5 * (2 - 2 * n) * (2 - 2 * n);
}
// Parabolic sine of a phase in [0, 1).
inline double psine(double p) {
  double t = 2 * p, a = std::min(t, 1.0), b = std::max(t, 1.0) - 1;
  return 4 * ((a - a * a) - (b - b * b));
}

struct Biquad {
  float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  float x1[2] = {}, x2[2] = {}, y1[2] = {}, y2[2] = {};
  void set(bool hp, double f, double sr) {
    double w = 2 * kPi * std::clamp(f, 1.0, sr * 0.49) / sr, c = std::cos(w), al = std::sin(w) / (2 * 0.5), a0 = 1 + al;
    double B0 = hp ? (1 + c) / 2 : (1 - c) / 2, B1 = hp ? -(1 + c) : 1 - c;
    b0 = static_cast<float>(B0 / a0), b1 = static_cast<float>(B1 / a0), b2 = b0;
    a1 = static_cast<float>(-2 * c / a0), a2 = static_cast<float>((1 - al) / a0);
  }
  float tick(int ch, float x) {
    float y = b0 * x + b1 * x1[ch] + b2 * x2[ch] - a1 * y1[ch] - a2 * y2[ch];
    x2[ch] = x1[ch], x1[ch] = x, y2[ch] = y1[ch], y1[ch] = std::fabs(y) < 1e-30f ? 0.f : y;
    return y;
  }
  void clear() {
    for (int c = 0; c < 2; ++c) x1[c] = x2[c] = y1[c] = y2[c] = 0;
  }
};

class Engine {
 public:
  Engine() { reset(); }
  void reset() {
    for (auto& v : lines_)
      for (auto& l : v) l.fill(0.f);
    for (auto& y : y_) y = {0, 0};
    // Fixed starting phases (the original draws random ones per instance).
    for (int v = 0; v < 8; ++v) p1_[v] = std::fmod(0.137 + 0.618034 * v, 1.0), p2_[v] = std::fmod(0.411 + 0.381966 * v, 1.0);
    w_ = 0, primed_ = false, hpMix_ = 0;
    lp_.clear(), hp_.clear();
  }
  // k = the eleven knob positions in tag order.
  void process(const float* k, double sr, float* L, float* R, int n) {
    if (sr != sr_) sr_ = sr, kSm_ = 1 - std::pow(0.1, 1 / (0.05 * sr)), primed_ = false, lp_.clear(), hp_.clear();
    if (!primed_) {
      for (int i = 0; i < 11; ++i) sm_[i] = k[i];
      primed_ = true, dirty_ = true;
      hpMix_ = highPassHz(sm_[8]) > 0 ? 1 : 0;
    }
    const int xf = std::max(1, static_cast<int>(sr / 100));
    for (int i = 0; i < n; ++i) {
      bool moved = dirty_;
      for (int p = 0; p < 11; ++p) {
        if (sm_[p] == k[p]) continue;
        sm_[p] += (k[p] - sm_[p]) * kSm_;
        if (std::fabs(k[p] - sm_[p]) < 1e-4) sm_[p] = k[p];
        moved = true;
      }
      if (moved) update();
      const bool eight = sm_[6] >= 0.5;
      const int voices = eight ? 8 : 4;
      double wl = 0, wr = 0;
      for (int v = 0; v < voices; ++v) {
        const double m = 0.5 + mv_ * psine(p2_[v]);
        const double inL = L[i] * 1.41442716 * std::sqrt(std::max(0.0, 1 - m)), inR = R[i] * 1.41442716 * std::sqrt(m);
        const double tL = D_[v] * (1 + depth_[v] * psine(p1_[v]));
        double q = p1_[v] + phd_;
        q -= std::floor(q);
        const double tR = D_[v] * (1 + depth_[v] * psine(q));
        lines_[v][0][w_] = static_cast<float>(inL + fb_ * y_[v][0]);
        lines_[v][1][w_] = static_cast<float>(inR + fb_ * y_[v][1]);
        y_[v][0] = read(lines_[v][0], tL);
        y_[v][1] = read(lines_[v][1], tR);
        for (double& y : y_[v])
          if (std::fabs(y) < 1e-30) y = 0;
        wl += y_[v][0], wr += y_[v][1];
        p1_[v] += inc1_[v], p2_[v] += inc2_[v];
        p1_[v] -= std::floor(p1_[v]), p2_[v] -= std::floor(p2_[v]);
      }
      w_ = (w_ + 1) & kMask;
      const double g = eight ? 0.281170487 : 0.562340975;
      float yl = lp_.tick(0, static_cast<float>(wl * g)), yr = lp_.tick(1, static_cast<float>(wr * g));
      const bool hpOn = hpHz_ > 0;
      if (hpOn || hpMix_ > 0) {
        float hl = hp_.tick(0, yl), hr = hp_.tick(1, yr);
        hpMix_ = hpOn ? std::min(1.0, hpMix_ + 1.0 / xf) : std::max(0.0, hpMix_ - 1.0 / xf);
        yl = static_cast<float>(yl * (1 - hpMix_) + hl * hpMix_);
        yr = static_cast<float>(yr * (1 - hpMix_) + hr * hpMix_);
      }
      L[i] = static_cast<float>(L[i] * dryG_ + yl * wetG_);
      R[i] = static_cast<float>(R[i] * dryG_ + yr * wetG_);
    }
  }

 private:
  static constexpr int kLen = 8192, kMask = kLen - 1;  // ≥ 23.4 ms · 1.32 at 192 kHz
  double read(const std::array<float, kLen>& b, double t) const {
    const int ii = static_cast<int>(t);
    const double f = t - ii;
    const double y0 = b[(w_ - ii) & kMask], y1 = b[(w_ - ii - 1) & kMask], y2 = b[(w_ - ii - 2) & kMask], y3 = b[(w_ - ii - 3) & kMask];
    const double c1 = 0.5 * (y2 - y0), c2 = y0 - 2.5 * y1 + 2 * y2 - 0.5 * y3, c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
    return ((c3 * f + c2) * f + c1) * f + y1;
  }
  void update() {
    dirty_ = false;
    for (int v = 0; v < 8; ++v) {
      const Row& r = kRows[v];
      D_[v] = (r.d0 + (r.d1 - r.d0) * sm_[0]) * 0.001 * sr_;
      inc1_[v] = (r.r0 + (r.r1 - r.r0) * sm_[2]) / sr_;
      const double d = (20 + 60 * sm_[3]) * 0.01;
      depth_[v] = d * d * 0.5;
      inc2_[v] = (r.m0 + (r.m1 - r.m0) * sm_[5]) / sr_;
    }
    fb_ = 0.7 * sm_[1], mv_ = 0.499 * sm_[4];
    phd_ = 0.25 * sm_[10] * sm_[10] * sm_[10];
    lp_.set(false, lowPassHz(sm_[7]), sr_);
    hpHz_ = highPassHz(sm_[8]);
    if (hpHz_ > 0) hp_.set(true, hpHz_, sr_);
    const double w = wetShare(sm_[9]);
    dryG_ = std::sqrt(1 - w), wetG_ = std::sqrt(w);
  }
  std::array<std::array<std::array<float, kLen>, 2>, 8> lines_{};
  std::array<std::array<double, 2>, 8> y_{};
  double p1_[8] = {}, p2_[8] = {}, D_[8] = {}, inc1_[8] = {}, inc2_[8] = {}, depth_[8] = {};
  double sr_ = 0, kSm_ = 0, sm_[11] = {}, fb_ = 0, mv_ = 0, phd_ = 0, hpHz_ = 0, hpMix_ = 0, dryG_ = 0.7071, wetG_ = 0.7071;
  bool primed_ = false, dirty_ = true;
  int w_ = 0;
  Biquad lp_, hp_;
};

}  // namespace gl::packs::spacefx::hcref
