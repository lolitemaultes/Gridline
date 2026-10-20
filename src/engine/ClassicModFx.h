// Gridline — J-26: reference engines of the classic phaser and flanger
// (studies analysis/demos/specs/j26_phaser.md, j26_flanger.md). Both run a
// 4096-entry LFO table with a 24-bit phase (12 bits index, 12 bits fraction,
// linear interpolation). Knob positions n (0..1) are the inputs; delay lines
// are fixed-size (no allocation on the audio thread).
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace gl::classic {

inline constexpr double kPi = 3.14159265358979323846;

struct LfoTable {
  float t[4097];
  float at(uint32_t ph) const {
    uint32_t i = (ph >> 12) & 4095, f = ph & 4095;
    return t[i] + (t[i + 1] - t[i]) * (static_cast<float>(f) / 4096.0f);
  }
};
inline uint32_t lfoInc(double hz, double sr) { return static_cast<uint32_t>(std::llround(hz * 16777216.0 / sr)) & 0xFFFFFF; }

// ----------------------------------------------------------------- phaser
inline double phaserRateHz(double sweepN, bool large) { return (large ? 10.0 : 2.0) * (std::exp(4 * sweepN) - 1) / 53.598150033144236; }
// J-36: clamp before rounding (decoded raw values can be huge or NaN: int overflow UB).
inline int phaserStages(double raw) { return std::clamp(static_cast<int>(std::lround(raw >= 0.0 ? std::min(raw, 64.0) : 0.0)) + 1, 3, 23); }

struct PhaserSettings {
  double sweep = 0.5, depthMin = 0.1, depthMax = 0.8, stereo = 0.5, feedback = 0.4, dry = 0.5, gain = 0.8;
  bool large = false;
  int stages = 8;
};

class Phaser {
 public:
  Phaser() {
    for (int i = 0; i <= 4096; ++i) tab_.t[i] = static_cast<float>(0.5 * (1 + std::sin(2 * kPi * (i & 4095) / 4096.0)));
  }
  void reset() {
    for (auto& c : st_)
      for (auto& s : c) s = {0, 0};
    y_[0] = y_[1] = 0;
    ph_ = 0;
  }
  void process(const PhaserSettings& s, double sr, float* L, float* R, int n) {
    const uint32_t inc = lfoInc(phaserRateHz(s.sweep, s.large), sr);
    const uint32_t off = static_cast<uint32_t>(std::lround(std::clamp(s.stereo, 0.0, 1.0) * 8388608.0));
    const double g = 0.99 * s.feedback, G = 2 * s.gain * std::sqrt(std::max(0.0, 1 - s.feedback * s.feedback));
    const int N = std::clamp(s.stages, 3, 23);
    for (int i = 0; i < n; ++i) {
      const double lfo[2] = {tab_.at(ph_), tab_.at((ph_ + off) & 0xFFFFFF)};
      float* io[2] = {L + i, R + i};
      for (int c = 0; c < 2; ++c) {
        const double x = *io[c];
        const double co = -(s.depthMin + lfo[c] * (s.depthMax - s.depthMin));
        double u = x + g * y_[c];
        for (int k = 0; k < N; ++k) {
          auto& z = st_[c][static_cast<size_t>(k)];
          double y = co * (u - z.y) + z.x;
          z.x = u, z.y = y;
          u = y;
        }
        if (std::fabs(u) < 1e-30) u = 0;
        y_[c] = u;
        *io[c] = static_cast<float>((s.dry * x + (1 - s.dry) * u) * G);
      }
      ph_ = (ph_ + inc) & 0xFFFFFF;
    }
  }

 private:
  struct Z {
    double x, y;
  };
  LfoTable tab_;
  std::array<std::array<Z, 23>, 2> st_{};
  double y_[2] = {};
  uint32_t ph_ = 0;
};

// ----------------------------------------------------------------- flanger
inline double kappa(double x, double k) { return (k + 1) * x / (k * x + 1); }
inline double flangerMs(double n) { return 20 * kappa(std::clamp(n, 0.0, 1.0), -0.8); }
inline double flangerRateHz(double n) { return 5 * kappa(std::clamp(n, 0.0, 1.0), -0.9); }

struct FlangerSettings {
  double delay = 0, depth = 0.4, rate = 0.4, phase = 118 / 360.0, damp = 0, shape = 0, feed = 0.23, dry = 0.7, wet = 0.6, cross = 0.5;
  bool invFeedback = true, invWet = true;
};

class Flanger {
 public:
  void reset() {
    for (auto& l : line_) l.fill(0.f);
    w_ = 0, ph_ = 0, yd_[0] = yd_[1] = 0;
    shapeQ_ = -1;
  }
  void process(const FlangerSettings& s, double sr, float* L, float* R, int n) {
    const int q = static_cast<int>(std::lround(std::clamp(s.shape, 0.0, 1.0) * 4096));
    if (q != shapeQ_) build(q);
    const double dMs = flangerMs(s.delay), depthMs = flangerMs(s.depth);
    const double centre = sr * (dMs + depthMs / 2) / 1000.0, half = std::trunc(sr * depthMs / 2 / 1000.0);
    const uint32_t inc = lfoInc(flangerRateHz(s.rate), sr);
    const uint32_t off = static_cast<uint32_t>(std::lround(std::clamp(s.phase, 0.0, 1.0) * 16777216.0)) & 0xFFFFFF;
    const double f = (s.invFeedback ? -0.98 : 0.98) * std::clamp(s.feed, 0.0, 1.0);
    const double k = 1 - std::clamp(s.damp, 0.0, 1.0);
    const double wet = s.invWet ? -s.wet : s.wet, cross = s.invWet ? -s.cross : s.cross;
    for (int i = 0; i < n; ++i) {
      const double lfo[2] = {tab_.at(ph_), tab_.at((ph_ + off) & 0xFFFFFF)};
      const double x[2] = {L[i], R[i]};
      for (int c = 0; c < 2; ++c) {
        line_[c][w_] = static_cast<float>(x[c] + f * yd_[c]);
        double d = std::floor((centre + half * lfo[c]) * 4096.0) / 4096.0;
        d = std::clamp(d, 0.0, static_cast<double>(kLen - 2));
        const uint32_t di = static_cast<uint32_t>(d);
        const double fr = d - di;
        const float s0 = line_[c][(w_ - di) & kMask], s1 = line_[c][(w_ - di - 1) & kMask];
        const double tap = s0 + (s1 - s0) * fr;
        yd_[c] += (tap - yd_[c]) * k;
        if (std::fabs(yd_[c]) < 1e-30) yd_[c] = 0;
      }
      L[i] = static_cast<float>(s.dry * x[0] + wet * yd_[0] + cross * yd_[1]);
      R[i] = static_cast<float>(s.dry * x[1] + wet * yd_[1] + cross * yd_[0]);
      w_ = (w_ + 1) & kMask;
      ph_ = (ph_ + inc) & 0xFFFFFF;
    }
  }

 private:
  static constexpr uint32_t kLen = 32768, kMask = kLen - 1;  // ≥ sr / 10 up to 192 kHz (max delay 40 ms)
  void build(int q) {
    shapeQ_ = q;
    for (int i = 0; i <= 4096; ++i) {
      int j = i & 4095;
      double sn = std::trunc(std::sin(2 * kPi * j / 4096.0) * 32767.0);
      double tr = j < 1024 ? j / 1024.0 : j < 3072 ? 1 - (j - 1024) / 1024.0 : -1 + (j - 3072) / 1024.0;
      tr = std::trunc(tr * 32767.0);
      tab_.t[i] = static_cast<float>(((4096 - q) * sn + q * tr) / 4096.0 / 32767.0);
    }
  }
  LfoTable tab_{};
  std::array<std::array<float, kLen>, 2> line_{};
  uint32_t w_ = 0, ph_ = 0;
  double yd_[2] = {};
  int shapeQ_ = -1;
};

}  // namespace gl::classic
