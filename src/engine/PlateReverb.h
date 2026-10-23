// Gridline — J-26: "plate" reference reverb for gl.reverb (RvbModel = 1).
//
// A figure-of-eight plate network (behaviour study):
// mono input → high-cut / low-cut → four input all-passes → pre-delay → two
// cross-coupled tanks (bass shelf, damping, two all-passes and two delays
// each, loop gain from the decay time) → seven output taps per side.
// All lengths scale with the size; buffers are sized once per sample rate.
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace gl::plate {

// Engine settings in reference units.
struct Settings {
  double lowCutHz = 0;    // <= 20 → off
  double highCutHz = 0;   // >= 22100 → off
  double preDelayMs = 0;  // whole milliseconds (0..250)
  int size = 40;          // 1..100
  double diffusion = 1;   // 0..1 (steps of 0.01)
  double decaySec = 1.5;  // T60 at mid frequencies
  double bassMult = 1;    // low-frequency decay multiplier (500 Hz crossover)
  double dampHz = 22100;  // >= 22100 → off
};

// Reference value steps (raw control 0..65536 → engine units).
inline int lowCutStep(double raw) { return static_cast<int>(2981.0 * static_cast<float>(raw / 65536.0) + 19.0); }
inline int cutStep(double raw) { return static_cast<int>(216.0 * static_cast<float>(raw / 65536.0) + 5.0); }
inline int sizeStep(double raw) { return static_cast<int>(99.0 * static_cast<float>(raw / 65536.0) + 1.0); }
inline double diffusionStep(double raw) { return static_cast<int>(100.0 * static_cast<float>(raw / 65536.0)) / 100.0; }
inline double decayStep(double raw) { return static_cast<int>(199.0 * static_cast<float>(raw / 65536.0) + 1.0) * 0.1; }
// Colour 0..40 → five steps (round half to even), Brighter … Warmer.
inline int colourStep(double raw) { return std::clamp(static_cast<int>(std::nearbyint(static_cast<float>(raw) * 0.1f)), 0, 4); }
inline constexpr double kColourBass[5] = {0.2, 0.6, 1.0, 1.5, 3.0};

class Plate {
 public:
  void init(double sr) {
    sr_ = sr;
    for (int k = 0; k < 12; ++k) line_[k].assign(static_cast<size_t>(std::max<long>(1, static_cast<long>(kLen[k]) * static_cast<long>(sr) / 20000)) + 2, 0.f);
    pre_.assign(static_cast<size_t>(0.25 * sr) + 4, 0.f);
    key_ = Settings{};
    key_.size = -1;
    reset();
  }
  void reset() {
    for (auto& l : line_) std::fill(l.begin(), l.end(), 0.f);
    std::fill(pre_.begin(), pre_.end(), 0.f);
    for (int& p : pos_) p = 0;
    prePos_ = 0;
    hc_ = lcIn_ = lcOut_ = 0;
    for (int t = 0; t < 2; ++t) shIn_[t] = shOut_[t] = damp_[t] = 0;
  }
  bool ready() const { return sr_ > 0; }

  // Processes one block: L/R in place, dry/wet linear gains.
  void process(const Settings& s, double dry, double wet, float* L, float* R, int n) {
    update(s);
    for (int i = 0; i < n; ++i) {
      double in = (static_cast<double>(L[i]) + R[i]) * 0.5 + 1e-30;
      hc_ += (in - hc_) * aHc_;
      double x = hc_;
      if (cLc_ != 0) {
        double ap = cLc_ * (x - lcOut_) + lcIn_;
        lcIn_ = x, lcOut_ = ap;
        x = (x - ap) * 0.5 + 1e-30;
      }
      for (int k = 0; k < 4; ++k) x = allpass(k, x, gIn_);
      // pre-delay (after the diffusers)
      double xp = pre_[static_cast<size_t>(prePos_)];
      pre_[static_cast<size_t>(prePos_)] = static_cast<float>(x);
      if (++prePos_ >= preLen_) prePos_ = 0;
      // tank A, fed by tank B's last delay
      double a = tank(0, read(11) * g_, xp, 4, 5, 6, 7);
      // tank B, fed by tank A's last delay
      double b = tank(1, a * g_, xp, 8, 9, 10, 11);
      (void)b;
      double left = tap(9, kTapA[0]) - w25_ * tap(9, kTapA[1]) - w7_ * tap(10, kTapA[2]) - w2_ * tap(11, kTapA[3]) +
                    w6_ * tap(5, kTapA[4]) + tap(6, kTapA[5]) + w5_ * tap(7, kTapA[6]);
      double right = tap(5, kTapB[0]) + w25_ * tap(5, kTapB[1]) - w7_ * tap(6, kTapB[2]) - w2_ * tap(7, kTapB[3]) -
                     w6_ * tap(9, kTapB[4]) + tap(10, kTapB[5]) + w5_ * tap(11, kTapB[6]);
      L[i] = static_cast<float>(L[i] * dry + left * wet);
      R[i] = static_cast<float>(R[i] * dry + right * wet);
    }
  }

 private:
  static constexpr int kLen[12] = {142, 107, 379, 277, 672, 4453, 1800, 3720, 908, 4217, 2656, 3163};
  static constexpr int kTapA[7] = {266, 2974, 1913, 2496, 1690, 187, 1566};
  static constexpr int kTapB[7] = {353, 3627, 1728, 3173, 1811, 335, 621};

  // Value written k samples ago (k scaled like the lines).
  double tap(int line, int t) const {
    int k = std::min(scaled(t), len_[line] - 1);
    int p = pos_[line] - 1 - k;
    while (p < 0) p += len_[line];
    return line_[line][static_cast<size_t>(p)];
  }
  // Oldest value of a delay line (the one about to be overwritten).
  double read(int line) const { return line_[line][static_cast<size_t>(pos_[line])]; }
  void write(int line, double v) {
    line_[line][static_cast<size_t>(pos_[line])] = static_cast<float>(v);
    if (++pos_[line] >= len_[line]) pos_[line] = 0;
  }
  double allpass(int line, double x, double g) {
    double v = read(line) - g * x;
    write(line, x + g * v);
    return v;
  }
  double tank(int t, double v, double xp, int ap1, int d1, int ap2, int d2) {
    double z = cB_ * (v - shOut_[t]) + shIn_[t];
    shIn_[t] = v, shOut_[t] = z;
    v += (v + z) * h_;
    v += xp;
    damp_[t] += (v - damp_[t]) * aHd_;
    v = allpass(ap1, damp_[t], gD1_);
    double old = read(d1);
    write(d1, v);
    v = allpass(ap2, old * g_, gD2_);
    double out = read(d2);
    write(d2, v);
    return out;
  }
  int scaled(int t) const { return std::max(1, t * scale_ / 20000); }
  static double onePoleCoef(double hz100, double sr) {
    double w = 2 * 3.14159265358979323846 * hz100 / sr, c = 2 - std::cos(w);
    return 1 - (c - std::sqrt(c * c - 1));
  }
  void update(const Settings& s) {
    if (s.lowCutHz == key_.lowCutHz && s.highCutHz == key_.highCutHz && s.preDelayMs == key_.preDelayMs && s.size == key_.size &&
        s.diffusion == key_.diffusion && s.decaySec == key_.decaySec && s.bassMult == key_.bassMult && s.dampHz == key_.dampHz)
      return;
    key_ = s;
    const double pi = 3.14159265358979323846, K = std::log(0.001);
    const int size = std::clamp(s.size, 1, 100);
    scale_ = static_cast<int>(static_cast<long>(size + 15) * static_cast<long>(sr_) / 115);
    for (int k = 0; k < 12; ++k) {
      len_[k] = std::min(scaled(kLen[k]), static_cast<int>(line_[k].size()));
      if (pos_[k] >= len_[k]) pos_[k] = 0;
    }
    preLen_ = std::clamp(static_cast<int>(std::lround(s.preDelayMs)) * static_cast<int>(sr_) / 1000, 1, static_cast<int>(pre_.size()));
    if (prePos_ >= preLen_) prePos_ = 0;
    if (s.lowCutHz <= 20.5) {
      cLc_ = 0;
    } else {
      double k = std::tan(pi * std::min(s.lowCutHz, sr_ / 2 - 1) / sr_);
      cLc_ = (k - 1) / (k + 1);
    }
    aHc_ = s.highCutHz >= 22099.5 ? 1.0 : onePoleCoef(s.highCutHz, sr_);
    aHd_ = s.dampHz >= 22099.5 ? 1.0 : onePoleCoef(s.dampHz, sr_);
    const double D = std::clamp(s.diffusion, 0.0, 1.0);
    auto dg = [&](int a, int b, double c) { return D <= 0 ? 0.0 : std::exp(K * ((len_[a] + len_[b]) * 0.5 / sr_) / (D * c)); };
    gIn_ = dg(0, 1, 0.07);
    gD1_ = dg(4, 8, 0.35);
    gD2_ = dg(6, 10, 0.51);
    const double T = std::max(0.05, s.decaySec), tl = (len_[5] + len_[7] + len_[9] + len_[11]) / sr_;
    g_ = std::pow(std::exp(K * tl / T), 0.25);
    double r = std::pow(std::exp(K * tl / (T * std::max(0.05, s.bassMult))), 0.25) / g_;
    h_ = (r - 1) / 2;
    double rr = std::min(r, 1.0), t = std::tan(pi * 500 / sr_);
    cB_ = (t - rr) / (t + rr);
    w6_ = g_ + 0.6 * (1 - g_), w25_ = g_ + 0.25 * (1 - g_), w7_ = g_ + 0.7 * (1 - g_), w5_ = g_ + 0.5 * (1 - g_), w2_ = g_ + 0.2 * (1 - g_);
  }

  double sr_ = 0;
  std::vector<float> line_[12], pre_;
  int len_[12] = {}, pos_[12] = {}, preLen_ = 1, prePos_ = 0, scale_ = 1;
  Settings key_;
  double cLc_ = 0, aHc_ = 1, aHd_ = 1, gIn_ = 0, gD1_ = 0, gD2_ = 0, g_ = 0, h_ = 0, cB_ = 0;
  double w6_ = 0, w25_ = 0, w7_ = 0, w5_ = 0, w2_ = 0;
  double hc_ = 0, lcIn_ = 0, lcOut_ = 0, shIn_[2] = {}, shOut_[2] = {}, damp_[2] = {};
};

}  // namespace gl::plate
