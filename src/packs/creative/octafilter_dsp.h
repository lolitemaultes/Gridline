// Gridline — Octa Filter engine (shared by the pack DSP and the tests).
// Eight filter units; per control tick (64 samples) every unit's eight
// targets are articulated (pattern envelope, LFO, X / Y / follower maps),
// then each sample runs: in (input gain + previous unit's to-next feed) →
// filter stages → shaper → out (pan with cross-feed, volume) → master bus.
// No allocation in process(): the envelope bank is a fixed pool parsed when
// the state text changes.
#pragma once
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include "../../core/PluginInfo.h"
#include "../../engine/Dsp.h"
#include "../../engine/Effects.h"
#include "creative.h"

namespace gl::packs::creative {

// Filter coefficient maths for one stage (also used by the editor's response display).
struct StageCoef {
  int kind = 0;  // 0 SVF, 1 biquad, 2 ladder, -1 bypass
  // SVF
  double f = 0, q = 1, gl = 1, gb = 0, gh = 0;
  // biquad
  double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  // ladder
  double g = 0, k = 0, comp = 1, drive = 1;
};

// Normalised cutoff (cycles per sample at 44.1 kHz scale) of a type for c in 0..1.
inline double octaFcn(int type, double c) {
  c = std::clamp(c, 0.0, 1.0);
  switch (type) {
    case FtSvf: return 0.0159 * (std::pow(51.0, c) - 1.0);
    case FtLp2:
    case FtHp2: return std::min(1.6, 0.01324 * (std::pow(76.0, c) - 1.0));
    case FtLpRbj: return 0.000664667 * (std::pow(1500.0, c) - 1.0);
    case FtHpRbj: return std::min(0.6001, 0.00039993 * (std::pow(1500.0, c) - 1.0));
    case FtLadder: return std::min(3.0, 0.01495 * (std::pow(201.0, c) - 1.0));
    default: return std::min(1.0001, 0.00039993 * (std::pow(1500.0, c) - 1.0));
  }
}

// Coefficients of stage `s` (0-based) of `stages` for a unit's settings.
// fcn: the stage's normalised cutoff (staggered modes raise it per stage);
// r: resonance 0..1; low/band/high: 0..1 knob x target; sr: sample rate.
inline StageCoef octaStage(int type, double fcn, double r, bool alt, double low, double band, double high, int stages, double sr) {
  StageCoef s;
  const double R = 44100.0 / sr, inv = 1.0 / std::max(1, stages);
  r = std::clamp(r, 0.0, 1.0);
  if (alt && type != FtSvf) r = std::clamp(0.5 * (fcn + 1.0) * r, 0.0, 1.0);
  auto rbj = [&](double w, double alpha, int mode, double A) {
    double cw = std::cos(w), nb0, nb1, nb2, na0, na1, na2;
    switch (mode) {
      case 0: nb0 = (1 - cw) / 2, nb1 = 1 - cw, nb2 = (1 - cw) / 2, na0 = 1 + alpha, na1 = -2 * cw, na2 = 1 - alpha; break;   // LP
      case 1: nb0 = (1 + cw) / 2, nb1 = -(1 + cw), nb2 = (1 + cw) / 2, na0 = 1 + alpha, na1 = -2 * cw, na2 = 1 - alpha; break;  // HP
      case 2: nb0 = alpha, nb1 = 0, nb2 = -alpha, na0 = 1 + alpha, na1 = -2 * cw, na2 = 1 - alpha; break;                        // BP
      case 3: nb0 = 1, nb1 = -2 * cw, nb2 = 1, na0 = 1 + alpha, na1 = -2 * cw, na2 = 1 - alpha; break;                           // notch
      case 4: nb0 = 1 - alpha, nb1 = -2 * cw, nb2 = 1 + alpha, na0 = 1 + alpha, na1 = -2 * cw, na2 = 1 - alpha; break;          // all pass
      case 5:                                                                                                                       // peak
        nb0 = 1 + alpha * A, nb1 = -2 * cw, nb2 = 1 - alpha * A, na0 = 1 + alpha / A, na1 = -2 * cw, na2 = 1 - alpha / A;
        break;
      case 6: {  // low shelf
        double sq = 2 * std::sqrt(A) * alpha;
        nb0 = A * ((A + 1) - (A - 1) * cw + sq), nb1 = 2 * A * ((A - 1) - (A + 1) * cw), nb2 = A * ((A + 1) - (A - 1) * cw - sq);
        na0 = (A + 1) + (A - 1) * cw + sq, na1 = -2 * ((A - 1) + (A + 1) * cw), na2 = (A + 1) + (A - 1) * cw - sq;
        break;
      }
      default: {  // high shelf
        double sq = 2 * std::sqrt(A) * alpha;
        nb0 = A * ((A + 1) + (A - 1) * cw + sq), nb1 = -2 * A * ((A - 1) + (A + 1) * cw), nb2 = A * ((A + 1) + (A - 1) * cw - sq);
        na0 = (A + 1) - (A - 1) * cw + sq, na1 = 2 * ((A - 1) - (A + 1) * cw), na2 = (A + 1) - (A - 1) * cw - sq;
        break;
      }
    }
    s.kind = 1;
    s.b0 = nb0 / na0, s.b1 = nb1 / na0, s.b2 = nb2 / na0, s.a1 = na1 / na0, s.a2 = na2 / na0;
  };
  const double kPi = 3.14159265358979323846;
  const double gainA = std::pow(0.125 * std::pow(8.0, std::clamp(low, 0.0, 1.0) * 2.0), inv);  // -18 .. +18 dB per unit
  switch (type) {
    case FtSvf: {
      s.kind = 0;
      s.f = 2.0 * std::sin(std::min(0.625, (fcn + 0.005) * R));
      double d = std::max(0.2, 1.0 - 0.9 * r);
      if (alt) d = d * d;
      s.q = std::pow(d, inv);
      s.gl = std::pow(std::max(0.0, low), inv), s.gb = std::pow(std::max(0.0, band), inv), s.gh = std::pow(std::max(0.0, high), inv);
      break;
    }
    case FtLp2:
    case FtHp2: {
      double w = std::min(0.4 * kPi, (fcn + 0.007) * R * kPi * 0.5);
      double Q = std::pow(1.0 + 0.049 * (std::pow(1000.0, r) - 1.0), inv);
      rbj(w, std::sin(w) * 0.5 / Q, type == FtLp2 ? 0 : 1, 1);
      break;
    }
    case FtLpRbj:
    case FtHpRbj:
    case FtAllPass: {
      double w = std::min(2.8495, (fcn + (type == FtLpRbj ? 0.003 : 0.0001)) * 2 * kPi * R);
      double Q = std::pow(0.1 + 0.02495 * (std::pow(2000.0, r) - 1.0), inv);
      rbj(w, std::sin(w) * 0.5 / Q, type == FtLpRbj ? 0 : type == FtHpRbj ? 1 : 4, 1);
      break;
    }
    case FtBp:
    case FtNotch: {
      double w = std::min(2.8495, (fcn + 0.0001) * 2 * kPi * R);
      double bw = (0.01 + 0.1199 * (std::pow(101.0, r) - 1.0)) * inv;
      double sw = std::sin(w);
      rbj(w, sw * std::sinh(std::min(8.0, bw * w / std::max(1e-6, sw))), type == FtBp ? 2 : 3, 1);
      break;
    }
    case FtLowShelf:
    case FtHighShelf: {
      double w = std::min(2.8495, (fcn + 0.0001) * 2 * kPi * R);
      double S = 0.1 + 0.45 * (std::pow(3.0, 1.0 - r) - 1.0);
      double alpha = std::sin(w) * 0.5 * std::sqrt(std::max(0.0, (gainA + 1.0 / gainA) * (1.0 / S - 1.0) + 2.0));
      rbj(w, alpha, type == FtLowShelf ? 6 : 7, gainA);
      break;
    }
    case FtPeak: {
      double w = std::min(2.8495, (fcn + 0.0001) * 2 * kPi * R);
      double Q = 0.1 + 0.04995 * (std::pow(2000.0, 1.0 - r) - 1.0);
      rbj(w, std::sin(w) / (2 * Q), 5, gainA);
      break;
    }
    case FtLadder: {
      s.kind = 2;
      double x = std::min(0.65, (fcn + 0.01) * R * 0.5);
      s.g = 1.0 - std::exp(-2 * kPi * x * (0.9988 - 0.649 * x + 0.4955 * x * x + 1.873 * x * x * x));
      double drv = 0.12 * (std::pow(251.0, std::clamp(low, 0.0, 1.0)) - 1.0);
      s.drive = std::max(0.05, drv);
      s.k = r * (1.9 + 0.6 * std::min(drv, 1.0));
      s.comp = 0.9968 + 1.8409 * x - 3.9364 * x * x;
      break;
    }
    default: s.kind = -1; break;
  }
  return s;
}

struct StageState {
  double l = 0, b = 0;                // SVF
  double x1 = 0, x2 = 0, y1 = 0, y2 = 0;  // biquad
  double z[4] = {0, 0, 0, 0};          // ladder
  void reset() { *this = StageState{}; }
};

inline double runStage(const StageCoef& c, StageState& st, double x) {
  switch (c.kind) {
    case 0: {
      double h = x - st.l - c.q * st.b;
      st.b += c.f * h;
      st.l += c.f * st.b;
      if (std::fabs(st.b) > 1e6 || !std::isfinite(st.b)) st.l = st.b = 0;
      return c.gl * st.l + c.gb * st.b + c.gh * h;
    }
    case 1: {
      double y = c.b0 * x + c.b1 * st.x1 + c.b2 * st.x2 - c.a1 * st.y1 - c.a2 * st.y2;
      st.x2 = st.x1, st.x1 = x, st.y2 = st.y1, st.y1 = std::fabs(y) < 1e-20 ? 0 : y;
      return y;
    }
    case 2: {
      double in = std::tanh(x * c.drive - c.k * c.comp * st.z[3]);
      for (int i = 0; i < 4; ++i) {
        st.z[i] += c.g * (in - std::tanh(st.z[i]));
        in = std::tanh(st.z[i]);
      }
      if (!std::isfinite(st.z[3])) st.z[0] = st.z[1] = st.z[2] = st.z[3] = 0;
      return st.z[3] / c.drive;
    }
    default: return x;
  }
}

class OctaFilterFx final : public Effect {
 public:
  OctaFilterFx();
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override;
  void reset() override;
  int meters(float* out, int max) const override;
  // Current articulated value of a target (UI display), or NaN.
  float targetValue(int u, int t) const { return static_cast<float>(cur_[u][t]); }

 private:
  void tick(const PluginState& s, const ProcessContext& ctx, double beat);
  std::unique_ptr<OctaBank> bank_;
  size_t dataHash_ = 0;
  double sr_ = 0;
  // Per unit / target values (start and end of the current tick).
  double cur_[kUnits][kTargets] = {}, prev_[kUnits][kTargets] = {};
  StageCoef coef_[kUnits][3];
  int stages_[kUnits] = {};
  StageState state_[kUnits][3][2];
  float wsTab_[kUnits][1025] = {};
  double ief_ = 0, modX_ = 0.5, modY_ = 0.5, patStart_ = 0, freeBeat_ = 0;
  int lastPattern_ = -1;
  double lfoPh_[kUnits][kTargets] = {};
  int tickPos_ = 0;
  dsp::DcBlock dc_[2];
  float ieMeter_ = 0;
};

}  // namespace gl::packs::creative
