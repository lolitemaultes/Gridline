// Gridline — J-26: reference engine of gl.lowlift (LlModel = 1; study
// analysis/demos/specs/j26_lowlifter.md). The low band (below the crossover,
// optionally split again at fc/2 and fc/4) is normalised by a held, smoothed
// peak envelope (floored at full scale), reshaped with a Chebyshev blend
// (u + s·T2 + s²·T3)/(1 + 2s), high-passed at 20 Hz, optionally diffused by
// four lattice all-passes, and recombined with the (delayed) high band; a
// first-order high-pass at the low cut and a phase-aligned dry/wet follow.
// Every control glides through a 20 Hz one-pole. Fixed-size buffers.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

namespace gl::llref {

inline constexpr double kPi = 3.14159265358979323846;

// Knob law lo + (hi - lo)·n^(1/c) of the original's controls.
inline double knob(double n, double lo, double hi, double c) { return lo + (hi - lo) * std::pow(std::clamp(n, 0.0, 1.0), 1.0 / c); }
inline double lowDb(double n) { return n <= 0 ? -1e9 : knob(n, -120, 12, 7.275); }
inline double slopeDb(double n) { return knob(n, -60, 0, 3.105); }
inline double highDb(double n) { return n <= 0 ? -1e9 : knob(n, -120, 0, 5); }
inline double cutHz(double n) { return knob(n, 20, 120, 0.498918); }
inline double dbLin(double db) { return db <= -1e8 ? 0.0 : std::pow(10.0, db / 20.0); }

struct Settings {
  bool on = true, lookahead = true, invert = false, multiband = false, highOn = true, lowOn = true;
  double mix = 1, lowDb = 0, slopeDb = -6.5, preDb = 9, dmix = 0, dcoef = 0, highDb = 0, xoverHz = 99.5, lowCutHz = 20;
};

// TPT state-variable section (Butterworth, k = √2): low / high outputs.
struct Svf {
  double ic1 = 0, ic2 = 0;
  void tick(double x, double g, double& lp, double& hp) {
    const double k = 1.4142135623730951, a1 = 1 / (1 + g * (g + k)), a2 = g * a1, a3 = g * a2;
    const double v3 = x - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
    ic1 = 2 * v1 - ic1, ic2 = 2 * v2 - ic2;
    if (std::fabs(ic1) < 1e-30) ic1 = 0;
    if (std::fabs(ic2) < 1e-30) ic2 = 0;
    lp = v2, hp = x - k * v1 - v2;
  }
  void clear() { ic1 = ic2 = 0; }
};
// Linkwitz-Riley 4: two cascaded sections on each side.
struct Lr4 {
  Svf a, b, c, d;
  void tick(double x, double g, double& lo, double& hi) {
    double l1, h1, l2, h2, l3, h3;
    a.tick(x, g, l1, h1);
    b.tick(l1, g, lo, h2);
    c.tick(x, g, l3, h3);
    d.tick(h3, g, l2, hi);
    (void)h1, (void)h2, (void)l2, (void)l3;
  }
  void clear() { a.clear(), b.clear(), c.clear(), d.clear(); }
};

class Engine {
 public:
  Engine() { reset(); }
  void reset() {
    for (auto& c : ch_) {
      c.split.clear(), c.dryAp.clear(), c.split2.clear(), c.split4.clear();
      for (auto& b : c.band) {
        b.dl.fill(0.f), b.held.fill(0), b.env.fill(1), b.cnt.fill(0), b.hp.clear();
        b.w = 0;
      }
      c.delay.fill(0.f), c.dryDelay.fill(0.f);
      for (auto& d : c.diff) d.buf.fill(0.f), d.w = 0;
      c.lc = 0;
    }
    primed_ = false;
    pos_ = 0;
    fade_ = 0;
  }
  static int latency(const Settings& s, double sr) { return s.lookahead ? static_cast<int>(std::lround(0.01 * std::min(sr, 192000.0))) : 0; }

  void process(const Settings& s, double rate, float* L, float* R, int n) {
    const double fs = std::min(rate, 192000.0);
    if (fs != fs_) fs_ = fs, primed_ = false;
    const double g20 = std::tan(kPi * 20 / fs), ks = 2 * g20 / (1 + g20);
    const double fade1 = 1 - std::exp(-2 * kPi * 1.0 / fs);
    const double look = s.lookahead ? 0.01 : 0.0;
    const int D = static_cast<int>(std::lround(look * fs));
    const int hold = static_cast<int>(std::lround((look + 0.05) * fs / 4));
    const double cUp = look > 0 ? std::exp(-14.444787 / (fs * look)) : 0.0, cDown = std::exp(-144.44786 / fs);
    const double tgt[9] = {s.on ? 1 - s.mix : 1.0, dbLin(s.lowDb), dbLin(s.slopeDb), dbLin(s.preDb), s.dmix,
                           std::clamp(s.dcoef, -0.99, 0.99), dbLin(s.highDb), std::max(20.0, s.xoverHz), std::max(1.0, s.lowCutHz)};
    if (!primed_) {
      for (int k = 0; k < 9; ++k) sm_[k] = tgt[k];
      primed_ = true;
      // Silence through the shaper is the constant -s/(1 + 2s) (T2 at 0): start the 20 Hz
      // high-passes settled on it so silence stays silent.
      const double dcOut = -sm_[2] / (1 + 2 * sm_[2]);
      for (auto& c : ch_)
        for (auto& b : c.band) b.hp.ic1 = 0, b.hp.ic2 = dcOut;
    }
    const double gHp20 = std::tan(kPi * 20 / fs);
    for (int i = 0; i < n; ++i) {
      for (int k = 0; k < 9; ++k) sm_[k] += ks * (tgt[k] - sm_[k]);
      fade_ += (1 - fade_) * fade1;
      const double dryW = sm_[0], lowLin = sm_[1], sLin = sm_[2], pre = sm_[3], dmix = sm_[4], dc = sm_[5], highLin = sm_[6];
      const double fc = sm_[7], lcHz = sm_[8];
      const double g1 = std::tan(kPi * fc / fs), g2 = std::tan(kPi * fc / (2 * fs)), g4 = std::tan(kPi * fc / (4 * fs));
      const double glc = std::tan(kPi * lcHz / fs);
      const double dk = std::sqrt(std::max(0.0, 1 - dc * dc));
      static const double kDiff[4] = {0.5, 0.20710678, 0.15891862, 0.1339746};
      float* io[2] = {L + i, R + i};
      for (int c = 0; c < 2; ++c) {
        Channel& C = ch_[c];
        const double x = *io[c];
        double low, high;
        C.split.tick(x, g1, low, high);
        // dry: the same split summed back (all-pass), delayed with the bands
        double dlo, dhi;
        C.dryAp.tick(x, g1, dlo, dhi);
        double dry = dlo + dhi;
        // bands of the low part
        double bands[3] = {low, 0, 0};
        int nb = 1;
        if (s.multiband) {
          double l2, h2, l4, h4;
          C.split2.tick(low, g2, l2, h2);
          C.split4.tick(l2, g4, l4, h4);
          bands[0] = l4, bands[1] = h4, bands[2] = h2, nb = 3;
        }
        double shaped = 0;
        for (int b = 0; b < nb; ++b) shaped += shape(C.band[b], bands[b] * pre, sLin, D, hold, cUp, cDown, gHp20);
        // diffuser
        double diff = shaped;
        for (int k = 0; k < 4; ++k) {
          int m = static_cast<int>(std::clamp(fs * kDiff[k] / fc - 1, 1.0, 48000.0));
          diff = C.diff[k].tick(diff, m, dc, dk);
        }
        shaped = shaped * (1 - dmix) + diff * dmix;
        shaped *= fade_;
        // delayed high band and dry (look-ahead)
        C.delay[pos_ % kMaxD] = static_cast<float>(high);
        C.dryDelay[pos_ % kMaxD] = static_cast<float>(dry);
        const size_t rp = (pos_ + kMaxD - static_cast<size_t>(D)) % kMaxD;
        const double hiD = C.delay[rp], dryD = C.dryDelay[rp];
        double wet = (s.highOn ? hiD * highLin : 0.0) + (s.lowOn ? shaped * lowLin * (s.invert ? -1 : 1) : 0.0);
        // first-order TPT high-pass at the low cut
        const double gg = glc / (1 + glc);
        const double v = (wet - C.lc) * gg, lpo = v + C.lc;
        C.lc = lpo + v;
        wet -= lpo;
        *io[c] = static_cast<float>(dryD * dryW + wet * (1 - dryW));
      }
      ++pos_;
    }
  }

 private:
  static constexpr size_t kMaxD = 2048;  // ≥ 10 ms at 192 kHz
  struct Band {
    std::array<float, kMaxD> dl{};
    std::array<double, 4> held{}, env{};
    std::array<int, 4> cnt{};
    Svf hp;
    size_t w = 0;
  };
  struct Lattice {
    std::array<float, 48002> buf{};
    size_t w = 0;
    double tick(double x, int m, double d, double dk) {
      const size_t r = (w + buf.size() - static_cast<size_t>(m)) % buf.size();
      const double z = buf[r];
      const double v = dk * x - d * z;  // normalised lattice: energy-preserving rotation
      const double y = d * x + dk * z;
      buf[w] = static_cast<float>(std::fabs(v) < 1e-30 ? 0.0 : v);
      w = (w + 1) % buf.size();
      return y;
    }
  };
  struct Channel {
    Lr4 split, dryAp, split2, split4;
    Band band[3];
    std::array<float, kMaxD> delay{}, dryDelay{};
    std::array<Lattice, 4> diff{};
    double lc = 0;
  };
  // One shaped band: peak envelope of the undelayed input (4 held stages,
  // floored at 1, 4 smoothing stages), applied to the look-ahead-delayed input.
  double shape(Band& B, double x, double sLin, int D, int hold, double cUp, double cDown, double g20) {
    double e = std::fabs(x);
    for (int k = 0; k < 4; ++k) {
      if (e >= B.held[static_cast<size_t>(k)] || B.cnt[static_cast<size_t>(k)] <= 0) {
        B.cnt[static_cast<size_t>(k)] = hold;
        B.held[static_cast<size_t>(k)] = e;
      } else {
        --B.cnt[static_cast<size_t>(k)];
      }
      e = B.held[static_cast<size_t>(k)];
    }
    e = std::max(1.0, e);
    for (int k = 0; k < 4; ++k) {
      double& y = B.env[static_cast<size_t>(k)];
      if (y < 1) y = 1;
      const double c = e > y ? cUp : cDown;
      y = (y - e) * c + e;
      e = y;
    }
    B.dl[B.w % kMaxD] = static_cast<float>(x);
    const double xd = B.dl[(B.w + kMaxD - static_cast<size_t>(D)) % kMaxD];
    ++B.w;
    const double u = std::clamp(xd / e, -1.0, 1.0);
    const double t2 = 2 * u * u - 1, t3 = u * (2 * t2 - 1);
    double y = (u + sLin * t2 + sLin * sLin * t3) / (1 + 2 * sLin);
    double lp, hp;
    B.hp.tick(y, g20, lp, hp);
    return hp;
  }
  std::array<Channel, 2> ch_{};
  double fs_ = 0, sm_[9] = {}, fade_ = 0;
  bool primed_ = false;
  size_t pos_ = 0;
};

}  // namespace gl::llref
