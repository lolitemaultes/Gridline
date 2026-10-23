// Gridline — J-26: reference frequency shifter for gl.freqshift (FsModel = 1).
//
// Behaviour study. Knob positions (0..1) are the
// stored values; the shift law depends on type and range. Per sample: input +
// soft-limited feedback → 20 Hz high-pass / 20 kHz low-pass → quadrature pair
// (two all-pass chains) → upper / lower sideband blend per channel (or a ring
// modulator) → dry / wet law → stereo cross-mix. The HQ type band-limits each
// sideband's input with 16th-order Linkwitz-Riley filters so no component
// folds over 0 Hz or Nyquist.
#pragma once
#include <algorithm>
#include <cmath>

namespace gl::fsref {

inline constexpr double kPi = 3.14159265358979323846;

// Range (Gridline order): 0 narrow (±200 Hz), 1 wide (±20 kHz), 2 tempo.
// Type (Gridline order): 0 shifter, 1 HQ shifter, 2 ring modulator.
// Shift in Hz for knob position n (0..1, 0.5 = none) — tempo range: in
// periods of `beats` (signed, 0.25 .. 32 beats), returned via `periodBeats`.
inline double tempoPeriodBeats(double x) { return 0.25 * std::floor(48.0 * std::pow(2.0, 7.0 * (1.0 - std::clamp(x, 0.0, 1.0)))) / 48.0; }
inline double shiftHz(double n, int type, int range, double tempo) {
  n = std::clamp(n, 0.0, 1.0);
  double x, sign = 1;
  if (type == 2) {
    x = n;
  } else {
    x = 2 * std::fabs(n - 0.5);
    sign = n < 0.5 ? -1 : 1;
  }
  if (range == 2) return sign / (tempoPeriodBeats(x) * 60.0 / std::max(1.0, tempo));
  if (x <= 0) return 0;
  return sign * 0.1 * std::pow(range == 1 ? 200000.0 : 2000.0, x);
}
// Inverse of the free-range law (for editors / presets): Hz → knob position.
inline double shiftPos(double hz, int type, int range) {
  double top = range == 1 ? 200000.0 : 2000.0;
  double a = std::fabs(hz);
  double x = a < 0.1 ? 0.0 : std::clamp(std::log(a / 0.1) / std::log(top), 0.0, 1.0);
  if (type == 2) return x;
  return 0.5 + (hz < 0 ? -x : x) * 0.5;
}

struct Biquad {
  double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
  // RBJ cookbook low / high pass.
  void set(bool hp, double f, double q, double sr) {
    double w = 2 * kPi * std::clamp(f, 1.0, sr * 0.49) / sr, c = std::cos(w), al = std::sin(w) / (2 * q), a0 = 1 + al;
    if (hp) b0 = (1 + c) / 2 / a0, b1 = -(1 + c) / a0;
    else b0 = (1 - c) / 2 / a0, b1 = (1 - c) / a0;
    b2 = b0, a1 = -2 * c / a0, a2 = (1 - al) / a0;
  }
  double tick(double x) {
    double y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
  void clear() { z1 = z2 = 0; }
};

// 16th-order Linkwitz-Riley section (an 8th-order Butterworth applied twice).
struct Lr16 {
  Biquad s[8];
  void set(bool hp, double f, double sr) {
    static const double q[4] = {0.5098, 0.6013, 0.9000, 2.5629};
    for (int k = 0; k < 8; ++k) {
      double z1 = s[k].z1, z2 = s[k].z2;
      s[k].set(hp, f, q[k & 3], sr);
      s[k].z1 = z1, s[k].z2 = z2;
    }
  }
  double tick(double x) {
    for (auto& b : s) x = b.tick(x);
    return x;
  }
  void clear() {
    for (auto& b : s) b.clear();
  }
};
// Linkwitz-Riley all-pass: low + high pass sum.
struct Lr16AllPass {
  Lr16 lo, hi;
  void set(double f, double sr) { lo.set(false, f, sr), hi.set(true, f, sr); }
  double tick(double x) { return lo.tick(x) + hi.tick(x); }
  void clear() { lo.clear(), hi.clear(); }
};

// Quadrature pair: two chains of second-order all-passes y = c²(x + y[n-2]) - x[n-2];
// the real chain's output is delayed one sample.
struct Quadrature {
  double xr[4][2] = {}, yr[4][2] = {}, xi[4][2] = {}, yi[4][2] = {}, rd = 0;
  void tick(double x, double& re, double& im) {
    static const double cr[4] = {0.6923878, 0.9360654322959, 0.9882295226860, 0.9987488452737};
    static const double ci[4] = {0.4021921162426, 0.8561710882420, 0.9722909545651, 0.9952884791278};
    double a = x, b = x;
    for (int k = 0; k < 4; ++k) {
      double c2 = cr[k] * cr[k], y = c2 * (a + yr[k][1]) - xr[k][1];
      xr[k][1] = xr[k][0], xr[k][0] = a, yr[k][1] = yr[k][0], yr[k][0] = y, a = y;
      double d2 = ci[k] * ci[k], z = d2 * (b + yi[k][1]) - xi[k][1];
      xi[k][1] = xi[k][0], xi[k][0] = b, yi[k][1] = yi[k][0], yi[k][0] = z, b = z;
    }
    re = rd, rd = a, im = b;
  }
};

struct Settings {
  int type = 0, range = 0;
  double freqPos = 0.5, mix = 1, lrPhase = 0, shapeL = 1, shapeR = 1, feedback = 0.5, stereo = 1, startPhase = 0.5;
};

class Shifter {
 public:
  void reset() {
    for (int c = 0; c < 2; ++c) {
      hp_[c].clear(), lp_[c].clear(), fb_[c] = 0;
      q_[c][0] = q_[c][1] = Quadrature{};
      for (int k = 0; k < 2; ++k) bandLo_[c][k].clear(), apLo_[c][k].clear(), dryAp_[c][k].clear();
    }
    primed_ = false;
    phase_ = 0, beat_ = 0;
  }
  void process(const Settings& s, double sr, double tempo, bool playing, double posBeats, float* L, float* R, int n) {
    if (sr != sr_) {
      sr_ = sr;
      k5_ = 1 - std::pow(0.1, 1 / (0.005 * sr));
      k100_ = 1 - std::pow(0.1, 1 / (0.1 * sr));
      for (int c = 0; c < 2; ++c) hp_[c].set(true, 20, 0.5, sr), lp_[c].set(false, 20000, 0.5, sr);
      design_ = 0;
    }
    const double tgt[8] = {s.mix, s.lrPhase, s.shapeL, s.shapeR, s.feedback, s.stereo, s.startPhase, s.freqPos};
    if (!primed_) {
      for (int k = 0; k < 8; ++k) sm_[k] = tgt[k];
      swap_ = -1;
      primed_ = true;
      retrigger(s, tempo, playing, posBeats);
    }
    const bool tempoRange = s.range == 2;
    if ((playing && !wasPlaying_) || tempoRange != wasTempo_) retrigger(s, tempo, playing, posBeats);
    wasPlaying_ = playing, wasTempo_ = tempoRange;
    const double beatInc = tempo / 60.0 / sr;
    for (int i = 0; i < n; ++i) {
      for (int k = 0; k < 7; ++k) {
        sm_[k] += (tgt[k] - sm_[k]) * k5_;
        if (std::fabs(sm_[k] - tgt[k]) < 1e-5) sm_[k] = tgt[k];
      }
      sm_[7] += (tgt[7] - sm_[7]) * k100_;
      if (std::fabs(sm_[7] - tgt[7]) < 1e-4) sm_[7] = tgt[7];
      if (sm_[7] != hzPos_ || s.type != hzType_ || s.range != hzRange_ || tempo != hzTempo_) {
        hzPos_ = sm_[7], hzType_ = s.type, hzRange_ = s.range, hzTempo_ = tempo;
        hz_ = shiftHz(hzPos_, s.type, s.range, tempo);
      }
      const double mix = sm_[0], kfb = 1.005 * (2 * sm_[4] - 1);
      const double dryG = std::min(1.0, 2 - 2 * mix), wetG = std::min(1.0, 2 * mix);
      const bool hq = s.type == 1;
      if (hq && --design_ <= 0) design(s);
      if (hq) {  // sideband swap crossfade (2 ms) when the shift changes sign
        double target = hz_ >= 0 ? -1.0 : 1.0;
        double st = 1.0 / (0.002 * sr);
        swap_ = target > swap_ ? std::min(target, swap_ + 2 * st) : std::max(target, swap_ - 2 * st);
      }
      double th[2] = {2 * kPi * phase_, 2 * kPi * (phase_ + 0.25 * sm_[1] - std::floor(phase_ + 0.25 * sm_[1]))};
      float* io[2] = {L + i, R + i};
      double y[2], dry[2];
      for (int c = 0; c < 2; ++c) {
        double x = *io[c];
        double u = lp_[c].tick(hp_[c].tick(x + std::tanh(fb_[c] * kfb)));
        double cs = std::cos(th[c]), sn = std::sin(th[c]);
        if (s.type == 2) {
          y[c] = u * cs;
          dry[c] = x;
        } else {
          double shape = c == 0 ? sm_[2] : sm_[3];
          double ua = u, ub = u;
          if (hq) {
            double lo = apLo_[c][0].tick(bandLo_[c][0].tick(u));   // low-passed, then all-pass at the high edge
            double hi = apLo_[c][1].tick(bandLo_[c][1].tick(u));   // high-passed, then all-pass at the low edge
            double w = (swap_ + 1) * 0.5;                           // 0: A ← lo, B ← hi; 1: swapped
            ua = lo + (hi - lo) * w, ub = hi + (lo - hi) * w;
            dry[c] = dryAp_[c][1].tick(dryAp_[c][0].tick(x));
          } else {
            dry[c] = x;
          }
          double re, im, re2, im2;
          q_[c][0].tick(ua, re, im);
          double A = im * cs - re * sn;
          double B;
          if (hq) {
            q_[c][1].tick(ub, re2, im2);
            B = im2 * cs + re2 * sn;
          } else {
            B = im * cs + re * sn;
          }
          y[c] = A * shape + B * (1 - shape);
        }
        fb_[c] = y[c];
      }
      double o[2];
      for (int c = 0; c < 2; ++c) o[c] = dry[c] * dryG + y[c] * wetG;
      const double e = 0.5 * (1 - sm_[5]);
      *io[0] = static_cast<float>(o[0] * (1 - e) + o[1] * e);
      *io[1] = static_cast<float>(o[0] * e + o[1] * (1 - e));
      phase_ += hz_ / sr;
      phase_ -= std::floor(phase_);
      beat_ += beatInc;
    }
  }

 private:
  void retrigger(const Settings& s, double tempo, bool playing, double posBeats) {
    const double ph0 = s.startPhase + 0.5;
    if (s.range == 2) {
      double p = std::fabs(shiftHz(s.freqPos, s.type, 2, tempo));
      double period = p > 0 ? (1.0 / p) * tempo / 60.0 : 1.0;  // beats
      double pos = playing ? posBeats : beat_;
      double v = pos / period + ph0;
      phase_ = v - std::floor(v);
    } else {
      phase_ = ph0 - std::floor(ph0);
    }
  }
  void design(const Settings& s) {
    design_ = 32;
    double a = std::fabs(hz_);
    if (s.range == 2 && a > 0) a = 1.0 / a;  // reference quirk: the tempo range designs at the period value
    double fHp = std::clamp(a, 20.0, 20000.0), fLp = std::clamp(22050.0 - a, 20.0, 20000.0);
    for (int c = 0; c < 2; ++c) {
      bandLo_[c][0].set(false, fLp, sr_), apLo_[c][0].set(fHp, sr_);
      bandLo_[c][1].set(true, fHp, sr_), apLo_[c][1].set(fLp, sr_);
      dryAp_[c][0].set(fLp, sr_), dryAp_[c][1].set(fHp, sr_);
    }
  }
  double sr_ = 0, k5_ = 0, k100_ = 0, sm_[8] = {}, phase_ = 0, beat_ = 0, hz_ = 0, swap_ = -1;
  double hzPos_ = -1, hzTempo_ = 0;
  int hzType_ = -1, hzRange_ = -1, design_ = 0;
  bool primed_ = false, wasPlaying_ = false, wasTempo_ = false;
  Biquad hp_[2], lp_[2];
  double fb_[2] = {};
  Quadrature q_[2][2];
  Lr16 bandLo_[2][2];
  Lr16AllPass apLo_[2][2], dryAp_[2][2];
};

}  // namespace gl::fsref
