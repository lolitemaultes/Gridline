// Gridline — Bitcrusher core (J-104): continuous-rate sample-and-hold, fractional
// bit-depth quantiser with TPDF dither, and the two converter filters
// (anti-alias before the hold, smoothing after it). Shared by the effect
// (Effects4.cpp) and the editor's staircase preview, so the picture is the sound.
// Header only, no allocation: safe on the audio thread.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace gl::crush {

// Hold period in host samples for a Rate value in Hz (the top of the travel, or any
// rate at or above the host rate, holds every sample). Periods within 1e-4 of an
// integer snap to it, so a rate saved as host / N (float) holds exactly N samples.
inline double periodFor(double rateHz, double sr, double offHz) {
  if (!(rateHz > 0) || rateHz >= offHz - 0.5 || rateHz >= sr) return 1.0;
  double p = sr / rateHz;
  const double r = std::round(p);
  if (std::fabs(p - r) < 1e-4 * p) p = r;
  return std::max(1.0, p);
}

// Cutoff of a converter filter: amount 0 = no filter, 1 = just under half the hold
// rate (clean). In between the cutoff glides on a log scale from the host's top
// down to the hold rate's Nyquist, so less filtering lets more aliasing through.
inline double filterHz(double amount, double period, double sr) {
  const double top = 0.45 * sr, clean = 0.42 * sr / std::max(1.0, period);
  if (clean >= top) return top;
  return std::exp((1.0 - amount) * std::log(top) + amount * std::log(clean));
}

// 12-pole Butterworth low-pass as six TPT state-variable sections (stable while
// the cutoff moves).
class Lowpass12 {
 public:
  void set(double sr, double hz) {
    static const double kInvQ[6] = {2 * 0.13052619222, 2 * 0.38268343237, 2 * 0.60876142901,
                                    2 * 0.79335334029, 2 * 0.92387953251, 2 * 0.99144486137};  // 1/Q = 2 sin((2k-1) pi / 24)
    const double g = std::tan(3.14159265358979323846 * std::clamp(hz, 10.0, 0.49 * sr) / sr);
    for (int k = 0; k < 6; ++k) {
      Sec& s = s_[k];
      s.a1 = 1.0 / (1.0 + g * (g + kInvQ[k]));
      s.a2 = g * s.a1;
      s.a3 = g * s.a2;
    }
  }
  double run(double x) {
    for (Sec& s : s_) {
      const double v3 = x - s.ic2, v1 = s.a1 * s.ic1 + s.a2 * v3, v2 = s.ic2 + s.a2 * s.ic1 + s.a3 * v3;
      s.ic1 = 2 * v1 - s.ic1, s.ic2 = 2 * v2 - s.ic2;
      x = v2;
    }
    return x;
  }
  void reset() {
    for (Sec& s : s_) s.ic1 = s.ic2 = 0;
  }

 private:
  struct Sec {
    double ic1 = 0, ic2 = 0, a1 = 1, a2 = 0, a3 = 0;
  };
  Sec s_[6];
};

class Crusher {
 public:
  // Filters for the coming frames (call per sub-block; amounts 0..1).
  void setFilters(double sr, double period, double antiAlias, double smooth) {
    const bool pre = antiAlias > 1e-3 && period > 1.0, post = smooth > 1e-3 && period > 1.0;
    if (pre) {
      if (!preOn_) pre_[0].reset(), pre_[1].reset();
      const double hz = filterHz(antiAlias, period, sr);
      pre_[0].set(sr, hz), pre_[1].set(sr, hz);
    }
    if (post) {
      if (!postOn_) post_[0].reset(), post_[1].reset();
      const double hz = filterHz(smooth, period, sr);
      post_[0].set(sr, hz), post_[1].set(sr, hz);
    }
    preOn_ = pre, postOn_ = post;
  }
  // One stereo frame. period >= 1 host samples per held value; steps = 2^(bits - 1)
  // (continuous); squarize 0..1; dither 0..1 of a +-1 LSB triangular noise.
  void frame(double& l, double& r, double period, double steps, double squarize, double dither) {
    double x[2] = {l, r};
    if (preOn_) x[0] = pre_[0].run(x[0]), x[1] = pre_[1].run(x[1]);
    cnt_ += 1.0;
    if (cnt_ >= period) {
      cnt_ -= period;
      if (cnt_ >= period) cnt_ = std::fmod(cnt_, period);  // the period shrank by more than one hold
      // The hold instant fell cnt_ samples ago: take the input there (linear), so any rate is exact.
      const double f = std::min(cnt_, 1.0);
      for (int c = 0; c < 2; ++c) {
        double v = x[c] - f * (x[c] - prev_[c]);
        if (squarize > 0) v = (v < 0 ? -1 : 1) * std::pow(std::fabs(v), 1.0 - 0.95 * squarize);
        if (dither > 0) v += dither * (rand01() - rand01()) / steps;
        hold_[c] = std::round(v * steps) / steps;
      }
      ++holds_;
    }
    prev_[0] = x[0], prev_[1] = x[1];
    l = hold_[0], r = hold_[1];
    if (postOn_) l = post_[0].run(l), r = post_[1].run(r);
  }
  void reset() {
    cnt_ = 0, hold_[0] = hold_[1] = prev_[0] = prev_[1] = 0, holds_ = 0;
    pre_[0].reset(), pre_[1].reset(), post_[0].reset(), post_[1].reset();
    rng_ = 0x9E3779B9u;
  }
  uint64_t holds() const { return holds_; }  // values taken since reset (tests: measured hold rate)

 private:
  double rand01() {
    rng_ ^= rng_ << 13, rng_ ^= rng_ >> 17, rng_ ^= rng_ << 5;
    return (rng_ >> 8) * (1.0 / 16777216.0);
  }
  double cnt_ = 0, hold_[2] = {0, 0}, prev_[2] = {0, 0};
  uint64_t holds_ = 0;
  uint32_t rng_ = 0x9E3779B9u;
  bool preOn_ = false, postOn_ = false;
  Lowpass12 pre_[2], post_[2];
};

}  // namespace gl::crush
