// Gridline — parametric EQ band design, shared by the DSP and the EQ editor so
// the drawn response is exactly what is heard.
//
// Reference band model (EqModel = 1, every new instance; see
// analysis notes in the κ1 spec):
//   • each band is a cascade of n = |order| + 1 second-order sections
//     (order "Normal" = 1 section, "4" = 2, "6" = 3, "8" = 4);
//   • cuts:   Q = 0.7071 · q, q = 0.1 + 9.9 (1 − w)³ (w = width 0..1,
//             default 0.5502 → q = 1, Butterworth). With n sections every
//             section uses q^(1/n); "steep" orders additionally scale each
//             section's q by a per-section factor (sharper knee), "gentle"
//             orders keep identical sections (soft knee);
//   • band pass / notch: bandwidth B = 0.1 + 4.9 w octaves, × 0.708·√n
//             (band pass) or ÷ (0.708·√n) (notch), identical sections;
//   • peak:   B = 0.1 + 4.9 w octaves; gentle orders split the gain over n
//             identical sections; steep orders spread n narrower sections
//             around the centre (offsets in units of B) with shared gain;
//   • shelves: slope S = 1/3 + 1.7193 (1 − w) (default w = 0.6122 → S = 1),
//             gain split over n sections, steep orders scale S per section;
//   • sections are RBJ bilinear designs (standard quality) or, with high
//     quality on, magnitude-matched designs that do not cramp near Nyquist.
// The legacy model (EqModel = 0) keeps the pre-κ1 Q / slope / order design.
#pragma once
#include <algorithm>
#include <cmath>
#include "../core/PluginInfo.h"
#include "../core/PluginParams.h"
#include "Dsp.h"

namespace gl {

struct EqBandFilters {
  dsp::Biquad stage[4];
  int count = 0;  // 0 = band off
};

inline float eqValue(const PluginState& s, int band, int field) {
  size_t i = static_cast<size_t>(pp::eqParam(band, field));
  return i < s.params.size() ? s.params[i] : 0.f;
}
inline float eqParamOr(const PluginState& s, int index, float dflt) {
  return index >= 0 && static_cast<size_t>(index) < s.params.size() ? s.params[static_cast<size_t>(index)] : dflt;
}

// First-order low/high pass as a biquad (b2 = a2 = 0).
inline void eqFirstOrder(dsp::Biquad& bq, bool highPass, double sr, double hz) {
  hz = std::clamp(hz, 10.0, sr * 0.49);
  double K = std::tan(dsp::kPi * hz / sr);
  double n = 1.0 / (1.0 + K);
  bq.b0 = highPass ? n : K * n;
  bq.b1 = highPass ? -n : K * n;
  bq.b2 = 0;
  bq.a1 = (K - 1.0) * n;
  bq.a2 = 0;
}

// ------------------------------------------------------------------ legacy model

inline void designEqBandLegacy(const PluginState& s, int band, double sr, EqBandFilters& f) {
  using namespace pp;
  using B = dsp::Biquad;
  int type = static_cast<int>(eqValue(s, band, EqType) + 0.5f);
  double hz = eqValue(s, band, EqFreq), gain = eqValue(s, band, EqGain), q = std::max(0.1f, eqValue(s, band, EqQ));
  int slope = std::clamp(static_cast<int>(eqValue(s, band, EqSlope) + 0.5f), 0, 4);
  int order = std::clamp(static_cast<int>(eqParamOr(s, eqExt(band, EqOrder), EqOrderNormal) + 0.5f), 0, 4);
  f.count = 0;
  int n = order <= EqOrderNormal ? 1 : order;
  double qs = order == EqOrderGentle ? q * 0.55 : q * (1.0 + 0.55 * (n - 1));
  double gs = gain / n;
  auto multi = [&](B::Type t, double g) {
    f.count = n;
    for (int i = 0; i < n; ++i) f.stage[i].set(t, sr, hz, qs, g);
  };
  switch (type) {
    case EqLowCut:
    case EqHighCut:
      if (slope == EqSlope6) {
        f.count = 1;
        eqFirstOrder(f.stage[0], type == EqLowCut, sr, hz);
      } else {
        f.count = slope + 1;
        for (int i = 0; i < f.count; ++i) {
          double bq = 1.0 / (2.0 * std::cos((2.0 * i + 1.0) * dsp::kPi / (4.0 * f.count)));
          f.stage[i].set(type == EqLowCut ? B::HighPass : B::LowPass, sr, hz, i == 0 ? bq * q / 0.707 : bq, 0);
        }
      }
      break;
    case EqBandPass: multi(B::BandPass, 0); break;
    case EqNotch: multi(B::Notch, 0); break;
    case EqLowShelf: multi(B::LowShelf, gs); break;
    case EqPeak: multi(B::Peak, gs); break;
    case EqHighShelf: multi(B::HighShelf, gs); break;
    default: break;
  }
}

// ------------------------------------------------------------------ reference model

namespace eqref {

// Per-section factors of the steep orders (row = |order|, column = section).
inline constexpr float kCutSteep[4][4] = {{1, 1, 1, 1}, {1.9834017f, 0.74921465f, 1, 1}, {2.64304f, 1.0815186f, 0.69424474f, 1},
                                          {3.5764515f, 1.1417328f, 1.0447108f, 0.65511858f}};
inline constexpr float kShelfSteep[4][4] = {{1, 1, 1, 1}, {2.6304471f, 0.61564445f, 1, 1}, {5.1612282f, 1.109271f, 0.50986731f, 1},
                                            {10.319123f, 1.5144353f, 0.69403285f, 0.5529967f}};
// Steep peaks: section centre offsets (× bandwidth, octaves), bandwidth factors, gain share.
inline constexpr float kPeakOffset[4][4] = {{0, 0, 0, 0}, {-0.25f, 0.25f, 0, 0}, {-0.475f, 0, 0.475f, 0}, {-0.775f, -0.3f, 0.3f, 0.775f}};
inline constexpr float kPeakWidth[4][4] = {{1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1.5707964f, 1, 1}, {1, 1.5707964f, 1.5707964f, 1}};
inline constexpr float kPeakGain[4] = {0.5f, 0.32125f, 0.2442f, 0.214445f};

enum Kind { LP, HP, BP, Notch, Peak, LShelf, HShelf };

// Squared analog magnitude of a normalised section (Ω = f / f0).
inline double analogMag2(Kind k, double W, double Q, double A) {
  double W2 = W * W, d = (1 - W2) * (1 - W2) + W2 / (Q * Q);
  switch (k) {
    case LP: return 1.0 / d;
    case HP: return W2 * W2 / d;
    case BP: return W2 / (Q * Q) / d;
    case Notch: return (1 - W2) * (1 - W2) / d;
    case Peak: return ((1 - W2) * (1 - W2) + W2 * A * A / (Q * Q)) / ((1 - W2) * (1 - W2) + W2 / (A * A * Q * Q));
    case LShelf: return A * A * ((A - W2) * (A - W2) + A * W2 / (Q * Q)) / ((1 - A * W2) * (1 - A * W2) + A * W2 / (Q * Q));
    case HShelf: return A * A * ((1 - A * W2) * (1 - A * W2) + A * W2 / (Q * Q)) / ((A - W2) * (A - W2) + A * W2 / (Q * Q));
  }
  return 1;
}

// RBJ bilinear section from A, alpha (shelves: alpha already includes 2√A·α/sin
// folded in as `sh`), sin / cos of w0.
inline void rbj(dsp::Biquad& bq, Kind k, double A, double alpha, double sw, double cw) {
  double b0, b1, b2, a0, a1, a2;
  switch (k) {
    case LP: b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = b0; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
    case HP: b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = b0; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
    case BP: b0 = alpha; b1 = 0; b2 = -alpha; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
    case Notch: b0 = 1; b1 = -2 * cw; b2 = 1; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
    case Peak: b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A; a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A; break;
    case LShelf: {
      double s = alpha * sw;  // 2√A·α_rbj
      b0 = A * ((A + 1) - (A - 1) * cw + s); b1 = 2 * A * ((A - 1) - (A + 1) * cw); b2 = A * ((A + 1) - (A - 1) * cw - s);
      a0 = (A + 1) + (A - 1) * cw + s; a1 = -2 * ((A - 1) + (A + 1) * cw); a2 = (A + 1) + (A - 1) * cw - s;
      break;
    }
    case HShelf: default: {
      double s = alpha * sw;
      b0 = A * ((A + 1) + (A - 1) * cw + s); b1 = -2 * A * ((A - 1) + (A + 1) * cw); b2 = A * ((A + 1) + (A - 1) * cw - s);
      a0 = (A + 1) - (A - 1) * cw + s; a1 = 2 * ((A - 1) - (A + 1) * cw); a2 = (A + 1) - (A - 1) * cw - s;
      break;
    }
  }
  bq.b0 = b0 / a0, bq.b1 = b1 / a0, bq.b2 = b2 / a0, bq.a1 = a1 / a0, bq.a2 = a2 / a0;
}

// Magnitude-matched section ("decramped"): poles by impulse invariance of the
// analog prototype's poles, numerator fitted so |H| equals the analog
// magnitude at DC, at Nyquist and at the section frequency.
inline void matched(dsp::Biquad& bq, Kind k, double f0, double sr, double Q, double A) {
  f0 = std::clamp(f0, 1.0, sr * 4.0);
  double w0 = dsp::kTwoPi * f0 / sr;
  // Analog pole pair (natural frequency factor, pole Q).
  double pw = 1, pq = Q;
  if (k == Peak) pq = Q * A;
  else if (k == LShelf) pw = 1.0 / std::sqrt(A);
  else if (k == HShelf) pw = std::sqrt(A);
  double wp = w0 * pw, z = 1.0 / (2.0 * pq);
  double a1, a2 = std::exp(-2 * z * wp);
  if (z <= 1) a1 = -2 * std::exp(-z * wp) * std::cos(std::sqrt(1 - z * z) * wp);
  else a1 = -2 * std::exp(-z * wp) * std::cosh(std::sqrt(z * z - 1) * wp);
  auto phis = [](double w, double& p0, double& p1, double& p2) {
    double s = std::sin(w / 2);
    p1 = s * s, p0 = 1 - p1, p2 = 4 * p0 * p1;
  };
  double A0 = (1 + a1 + a2) * (1 + a1 + a2), A1 = (1 - a1 + a2) * (1 - a1 + a2), A2 = -4 * a2;
  auto den = [&](double p0, double p1, double p2) { return A0 * p0 + A1 * p1 + A2 * p2; };
  double nyq = 0.5 * sr / f0;
  double B0 = A0 * analogMag2(k, 0.0, Q, A);
  double B1 = A1 * analogMag2(k, nyq, Q, A);
  double wm = std::min(w0, dsp::kPi * 0.98);  // match point (the section frequency, or just below Nyquist)
  double p0, p1, p2;
  phis(wm, p0, p1, p2);
  double target = analogMag2(k, wm / w0, Q, A) * den(p0, p1, p2);
  double B2 = p2 > 1e-12 ? (target - B0 * p0 - B1 * p1) / p2 : 0;
  double r0 = std::sqrt(std::max(0.0, B0)), r1 = std::sqrt(std::max(0.0, B1));
  double W = 0.5 * (r0 + r1);
  double b0 = 0.5 * (W + std::sqrt(std::max(0.0, W * W + B2)));
  double b1 = 0.5 * (r0 - r1);
  double b2 = b0 > 1e-15 ? -B2 / (4 * b0) : 0;
  bq.b0 = b0, bq.b1 = b1, bq.b2 = b2, bq.a1 = a1, bq.a2 = a2;
}

}  // namespace eqref

// Reference order choice → signed order (-3 steep 8 .. 0 normal .. 3 gentle 8).
inline int eqSignedOrder(int choice) { return std::clamp(choice, 0, 6) - 3; }

inline void designEqBandRef(const PluginState& s, int band, double sr, EqBandFilters& f) {
  using namespace pp;
  using namespace eqref;
  f.count = 0;
  int type = static_cast<int>(eqValue(s, band, EqType) + 0.5f);
  if (type == EqOff) return;
  const double hz = std::clamp(static_cast<double>(eqValue(s, band, EqFreq)), 10.0, 22000.0);
  const double w = std::clamp(static_cast<double>(eqParamOr(s, eqFl(band, EqWidth), 0.4f)), 0.0, 1.0);
  const int so = eqSignedOrder(static_cast<int>(eqParamOr(s, eqFl(band, EqFlOrder), EqOrd2) + 0.5f));
  const bool steep = so < 0;
  const int o = std::abs(so), n = o + 1;
  const double gain = (type == EqLowShelf || type == EqPeak || type == EqHighShelf) ? eqValue(s, band, EqGain) : 0.0;
  const bool hq = eqParamOr(s, EqHQ, 0) >= 0.5f;
  if ((type == EqLowShelf || type == EqPeak || type == EqHighShelf) && gain == 0.0) return;  // flat: bypassed
  const double ln2h = 0.5 * std::log(2.0);
  auto w0Of = [&](double f, double clampW) { return std::min(dsp::kTwoPi * f / sr, clampW); };
  f.count = n;
  for (int k = 0; k < n; ++k) {
    dsp::Biquad& bq = f.stage[k];
    switch (type) {
      case EqLowCut:
      case EqHighCut: {
        double q = 0.1 + 9.9 * (1 - w) * (1 - w) * (1 - w);
        if (o > 0) q = std::pow(q, 1.0 / n);
        if (steep) q *= kCutSteep[o][k];
        double Q = 0.70710678118654752 * q;
        Kind kind = type == EqLowCut ? HP : LP;
        double w0 = w0Of(hz, 3.138451099395752);
        if (hq) matched(bq, kind, hz, sr, Q, 1);
        else rbj(bq, kind, 1, std::sin(w0) / (2 * Q), std::sin(w0), std::cos(w0));
        break;
      }
      case EqBandPass:
      case EqNotch: {
        double bw = (0.1 + 4.9 * w) * (type == EqBandPass ? 0.70794578 * std::sqrt(n) : 1.0 / (0.70794578 * std::sqrt(n)));
        double w0 = w0Of(hz, 3.138451099395752), sw = std::sin(w0);
        double alpha = std::sinh(ln2h * bw * w0 / sw) * sw;
        Kind kind = type == EqBandPass ? BP : Notch;
        if (hq) matched(bq, kind, hz, sr, 1.0 / (2 * std::sinh(ln2h * bw)), 1);
        else rbj(bq, kind, 1, alpha, sw, std::cos(w0));
        break;
      }
      case EqPeak: {
        double bw = 0.1 + 4.9 * w, fk = hz, g;
        double clampW = 2.1991147994995117;
        if (o > 0 && steep) {
          bw *= 1.0 / n;
          fk = hz * std::exp(kPeakOffset[o][k] * bw * std::log(2.0));
          bw *= kPeakWidth[o][k];
          g = gain * (o == 1 ? kPeakGain[1] - (bw - 1) * 0.01 : kPeakGain[o]);
          clampW = 2.5132741928100586;
        } else {
          g = gain / n * 0.5;
        }
        double A = std::exp(g * 0.115129254649702);  // 10^(g/20); g already halved for the RBJ A
        double w0 = w0Of(fk, clampW), sw = std::sin(w0);
        double alpha = std::sinh(ln2h * bw * w0 / sw) * sw;
        if (hq) matched(bq, Peak, fk, sr, 1.0 / (2 * std::sinh(ln2h * bw)), A);
        else rbj(bq, Peak, A, alpha, sw, std::cos(w0));
        break;
      }
      case EqLowShelf:
      case EqHighShelf: {
        double A = std::exp(gain / n * 0.5 * 0.115129254649702);
        double S = (1 - w) * 1.7192982456140353 + 1.0 / 3.0;
        if (steep) S *= kShelfSteep[o][k];
        double w0 = w0Of(hz, 3.138451099395752), sw = std::sin(w0);
        double root = std::sqrt(std::max(0.0, (A + 1 / A) * (1 / S - 1) + 2));
        Kind kind = type == EqLowShelf ? LShelf : HShelf;
        if (hq) matched(bq, kind, hz, sr, 1.0 / std::max(1e-6, root), A);
        else rbj(bq, kind, A, std::sqrt(A) * root, sw, std::cos(w0));
        break;
      }
      default: f.count = 0; return;
    }
  }
}

// J-26 classic band model (EqModel = 2): one RBJ biquad per band at the host
// rate; width w = knob position: q = 0.1 + 18.9 w (cuts, band, notch), peak
// q = 0.1 + 0.0999 (1001^w - 1), shelf slope S = 0.1 + 1.9 w (spec j26_peq1.md).
inline void designEqBandClassic(const PluginState& s, int band, double sr, EqBandFilters& f) {
  using namespace pp;
  using namespace eqref;
  f.count = 0;
  const int type = static_cast<int>(eqValue(s, band, EqType) + 0.5f);
  if (type == EqOff) return;
  const double hz = std::min(static_cast<double>(eqValue(s, band, EqFreq)), 0.4999 * sr);
  const double w = std::clamp(static_cast<double>(eqParamOr(s, eqFl(band, EqWidth), 0.267f)), 0.0, 1.0);
  const double A = std::pow(10.0, eqValue(s, band, EqGain) / 40.0);
  const double w0 = 2 * 3.14159265358979323846 * hz / sr, sw = std::sin(w0), cw = std::cos(w0);
  dsp::Biquad& bq = f.stage[0];
  f.count = 1;
  switch (type) {
    case EqHighCut: rbj(bq, LP, 1, sw / (2 * (0.1 + 18.9 * w)), sw, cw); break;
    case EqLowCut: rbj(bq, HP, 1, sw / (2 * (0.1 + 18.9 * w)), sw, cw); break;
    case EqBandPass: rbj(bq, BP, 1, sw / (2 * (0.1 + 18.9 * w)), sw, cw); break;
    case EqNotch: rbj(bq, Notch, 1, sw / (2 * (0.1 + 18.9 * w)), sw, cw); break;
    case EqPeak: rbj(bq, Peak, A, sw / (2 * (0.1 + 0.0999 * (std::pow(1001.0, w) - 1))), sw, cw); break;
    case EqLowShelf:
    case EqHighShelf: {
      double S = 0.1 + 1.9 * w, beta = std::sqrt(std::max(0.0, (A * A + 1) / S - (A - 1) * (A - 1)));
      rbj(bq, type == EqLowShelf ? LShelf : HShelf, A, beta, sw, cw);
      break;
    }
    default: f.count = 0; break;
  }
}

// Disabled bands (Enabled off) have no stages.
inline void designEqBand(const PluginState& s, int band, double sr, EqBandFilters& f) {
  f.count = 0;
  size_t ei = static_cast<size_t>(pp::eqExt(band, pp::EqEnabled));
  if (ei < s.params.size() && s.params[ei] < 0.5f) return;
  if (eqParamOr(s, pp::EqModel, 0) >= 1.5f) designEqBandClassic(s, band, sr, f);
  else if (eqParamOr(s, pp::EqModel, 0) >= 0.5f) designEqBandRef(s, band, sr, f);
  else designEqBandLegacy(s, band, sr, f);
}

// Bandwidth of a band in octaves as the editor shows it (peaks / band types),
// or the equivalent for cuts and shelves.
inline double eqBandwidthOct(const PluginState& s, int band) {
  double w = eqParamOr(s, pp::eqFl(band, pp::EqWidth), 0.4f);
  return 0.1 + 4.9 * w;
}

// The "listen" filter (band solo). mode: 0/1 adaptive (follows the band type),
// 2 band pass one octave wide, 3 band pass with the band's own width.
inline void designEqListen(const PluginState& s, int band, double sr, dsp::Biquad& bq, int mode = 1) {
  using namespace pp;
  double hz = eqValue(s, band, EqFreq);
  int type = static_cast<int>(eqValue(s, band, EqType) + 0.5f);
  double w = eqParamOr(s, eqFl(band, EqWidth), 0.4f);
  double bwOct = 1.0;
  if (mode == 3 || mode <= 1) bwOct = std::clamp(0.1 + 4.9 * w, 0.2, 5.0);
  if (mode == 2) bwOct = 1.0;
  if (eqParamOr(s, EqModel, 0) < 0.5f) {  // legacy: Q-based
    double q = std::max(0.3f, eqValue(s, band, EqQ));
    bq.set(dsp::Biquad::BandPass, sr, hz, q, 0);
    return;
  }
  double q = 1.0 / (2 * std::sinh(0.5 * std::log(2.0) * bwOct));
  if (mode <= 1) {
    if (type == EqLowCut) return bq.set(dsp::Biquad::HighPass, sr, hz, 0.7071, 0);
    if (type == EqHighCut) return bq.set(dsp::Biquad::LowPass, sr, hz, 0.7071, 0);
    if (type == EqLowShelf) return bq.set(dsp::Biquad::LowPass, sr, hz, 0.7071, 0);
    if (type == EqHighShelf) return bq.set(dsp::Biquad::HighPass, sr, hz, 0.7071, 0);
  }
  bq.set(dsp::Biquad::BandPass, sr, hz, q, 0);
}

inline double eqBandMagnitude(const EqBandFilters& f, double hz, double sr) {
  double m = 1.0;
  for (int i = 0; i < f.count; ++i) m *= f.stage[i].magnitude(hz, sr);
  return m;
}

}  // namespace gl
