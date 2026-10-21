// Gridline — J-68: models behind live editor graphics (see GraphModels.h).
#include "GraphModels.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include "../core/PluginParams.h"
#include "Dsp.h"
#include "Delay3Ref.h"
#include "FullReverbRef.h"

namespace gl::graphmodel {

namespace {

constexpr double kPi = 3.14159265358979323846;

float pv(const PluginState& s, int i, float dflt = 0.f) { return i >= 0 && static_cast<size_t>(i) < s.params.size() ? s.params[static_cast<size_t>(i)] : dflt; }
int iv(const PluginState& s, int i, int dflt = 0) { return static_cast<int>(std::lround(pv(s, i, static_cast<float>(dflt)))); }

// |H| of the loop damping one-pole y += c (x - y) at hz.
double onePoleMag(double c, double hz, double sr) {
  if (c >= 1) return 1;
  const double w = 2 * kPi * hz / sr, a = 1 - c;
  return c / std::sqrt(1 - 2 * a * std::cos(w) + a * a);
}
// T60 of a loop of `loopSec` whose per-pass gain is `passGain`.
double t60(double loopSec, double passGain) {
  if (passGain >= 1) return 1e9;
  if (passGain <= 0) return 0;
  return -3.0 * loopSec / std::log10(passGain);
}
// Reference-style cosine one-pole coefficient for a cutoff in Hz (plate / full networks).
double cutCoef(double hz, double sr) {
  const double h = std::min(hz, 0.5 * sr), k = 2.0 - std::cos(2 * kPi * h / sr);
  return 1 - (k - std::sqrt(k * k - 1));
}

}  // namespace

double decayAmp(double sec, double rt60) { return sec <= 0 ? 1.0 : std::pow(10.0, -3.0 * sec / std::max(1e-3, rt60)); }

ReverbView reverbView(const PluginState& st, double sr, double tempo) {
  using namespace pp;
  ReverbView v;
  sr = sr > 0 ? sr : 44100;
  v.model = std::clamp(iv(st, RvbModel), 0, 2);
  if (v.model == 2) {  // full reference: the standalone reverb's integer controls
    fullrev::StudioKnobs k;
    k.lowCut = iv(st, RvbFLowCut, 75), k.highCut = iv(st, RvbFHighCut, 40), k.predelay = iv(st, RvbFPredelay, 0), k.room = iv(st, RvbFRoom, 50);
    k.diffusion = iv(st, RvbFDiffusion, 100), k.decay = iv(st, RvbFDecay, 15), k.damp = iv(st, RvbFDamp, 40), k.bass = iv(st, RvbFBass, 100);
    k.cross = iv(st, RvbFCross, 500), k.er = iv(st, RvbFEr, 128), k.wet = iv(st, RvbFWet, 128), k.dry = iv(st, RvbFDry, 128);
    k.modDepth = iv(st, RvbFModDepth, 0);
    k.tempoPredelay = pv(st, RvbPreSync) >= 0.5f;
    const int sri = static_cast<int>(sr);
    const fullrev::StudioSettings s = fullrev::studioSettings(k, sri, tempo);
    v.preSec = s.predelay / sr;
    // Loop: the four tank delays (lengths follow the room), four loop gains.
    const int64_t S = fullrev::roomScale(s.room, sri);
    const int sumN = fullrev::lineLen(fullrev::kLeftConst[1], S) + fullrev::lineLen(fullrev::kLeftConst[3], S) +
                     fullrev::lineLen(fullrev::kRightConst[1], S) + fullrev::lineLen(fullrev::kRightConst[3], S);
    // The loop gains are set from the four delays alone, but a round trip also
    // runs through the tank all-passes: the tail decays over the whole loop.
    const int apN = fullrev::lineLen(fullrev::kLeftConst[0], S) + fullrev::lineLen(fullrev::kLeftConst[2], S) +
                    fullrev::lineLen(fullrev::kRightConst[0], S) + fullrev::lineLen(fullrev::kRightConst[2], S);
    const double loopSec = (sumN + apN) / sr, ds = std::max(1.0, static_cast<double>(s.decaySamples));
    v.decayKnob = ds / sr;
    const double g4 = std::exp(sumN * std::log(0.001) / ds);  // the four loop gains
    v.rt60 = t60(loopSec, g4);
    v.rt60Low = t60(loopSec, std::exp(sumN * std::log(0.001) / (ds * std::max(0.05f, s.bass))));
    const double cd = s.damp == 0 ? 1.0 : cutCoef(s.damp * 100.0, sr);
    v.dampHz = s.damp * 100.0;
    v.rt60High = t60(loopSec, g4 * std::pow(onePoleMag(cd, ReverbView::kHighHz, sr), 2));
    v.crossHz = s.cross;
    v.size = (s.room - 1) / 99.0;
    v.diffusion = s.diffusion / 100.0;
    v.modDepth = std::clamp(k.modDepth / 2048.0, 0.0, 1.0);
    v.dry = fullrev::levelCurve(std::clamp(k.dry, 0, 160) / 128.0);
    v.er = fullrev::levelCurve(std::clamp(k.er, 0, 160) / 256.0);
    v.wet = fullrev::levelCurve(std::clamp(k.wet, 0, 160) / 256.0);
    // Reflection taps (not pre-delayed), as the engine places and weights them.
    const float e = static_cast<float>((s.room + 3) * sri) * 3.302292e-7f;
    int tl[7], tr[8];
    for (int i = 0; i < 7; ++i) tl[i] = static_cast<int>(std::nearbyint(static_cast<float>(fullrev::kErL[i]) * e));
    for (int i = 0; i < 8; ++i) tr[i] = static_cast<int>(std::nearbyint(static_cast<float>(fullrev::kErR[i]) * e));
    const double M = std::min(tl[0], tr[0]);
    for (int i = 0; i < 7; ++i) v.erSec[static_cast<size_t>(v.erCount)] = tl[i] / sr, v.erGain[static_cast<size_t>(v.erCount++)] = 0.5 * M / std::max(1, tl[i]);
    for (int i = 0; i < 8; ++i) v.erSec[static_cast<size_t>(v.erCount)] = tr[i] / sr, v.erGain[static_cast<size_t>(v.erCount++)] = 0.36 * M / std::max(1, tr[i]);
    return v;
  }
  const double preMs = pv(st, RvbPreDelay, 10);
  if (v.model == 1) {  // plate reference: same tank as the full network, fixed 500 Hz crossover, no reflections
    const int size = std::clamp(static_cast<int>(std::lround(pv(st, RvbSize, 0.4f) * 100)), 1, 100);
    const int scale = static_cast<int>(static_cast<long>(size + 15) * static_cast<long>(sr) / 115);
    auto len = [&](int c) { return std::max(1, c * scale / 20000); };
    const double delaySec = (len(4453) + len(3720) + len(4217) + len(3163)) / sr;
    const double loopSec = delaySec + (len(672) + len(1800) + len(908) + len(2656)) / sr;  // + tank all-passes
    v.preSec = std::lround(preMs) / 1000.0;
    v.decayKnob = std::max(0.05, static_cast<double>(pv(st, RvbDecayTime, 1.5f)));
    const double bass = std::max(0.05, static_cast<double>(pv(st, RvbBassMult, 1)));
    const double dampHz = pv(st, RvbDampHz, 22100);
    const double g4 = std::exp(std::log(0.001) * delaySec / v.decayKnob);
    v.rt60 = t60(loopSec, g4);
    v.rt60Low = t60(loopSec, std::exp(std::log(0.001) * delaySec / (v.decayKnob * bass)));
    v.dampHz = dampHz >= 22099.5 ? 0 : dampHz;
    v.rt60High = t60(loopSec, g4 * std::pow(onePoleMag(v.dampHz > 0 ? cutCoef(dampHz, sr) : 1.0, ReverbView::kHighHz, sr), 2));
    v.crossHz = 500;
    v.size = (size - 1) / 99.0;
    v.diffusion = std::clamp(static_cast<double>(pv(st, RvbDiffusion, 1)), 0.0, 1.0);
    v.dry = pv(st, RvbDry, 1), v.er = 0, v.wet = pv(st, RvbWet, 0.2f);
    return v;
  }
  // Hall: 8 modulated combs per side, RT60-derived feedback (×1.12 calibration),
  // one-pole damping in the loop, a crossover for the bass multiplier.
  const double size = pv(st, RvbSize, 0.6f), damp = pv(st, RvbDamp, 0.4f);
  const double rt = pv(st, RvbDecayTime, 1.5f), bass = pv(st, RvbBassMult, 1);
  v.freeze = std::clamp(iv(st, RvbFreeze), 0, 2);
  v.preSec = pv(st, RvbPreSync) >= 0.5f ? (preMs * 0.384 / 192.0) * 60.0 / std::max(20.0, tempo) : preMs * 0.001;
  static const int kComb[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
  double mean = 0;
  for (int c : kComb) mean += c;
  const double len = std::max(16.0, (mean / 8 + 11.5) * sr / 44100.0 * (0.55 + size * 0.75));
  const double rt60 = std::max(0.1, rt) * 1.12;
  const double g = std::min(0.995, std::pow(10.0, -3.0 * len / (rt60 * sr)));
  const double gLow = std::min(0.995, std::pow(10.0, -3.0 * len / (std::max(0.1, rt * bass * 1.12) * sr)));
  const double dampHz = pv(st, RvbDampHz, 22100);
  const double dHz = dampHz < 22000 ? std::exp(-2 * kPi * std::max(20.0, dampHz) / sr) : 0.0;
  const double d1 = v.freeze == 1 ? 0.0 : std::max(damp * 0.45, dHz), d2 = 1 - d1;
  const double w = 2 * kPi * ReverbView::kHighHz / sr;
  const double hMag = d2 / std::sqrt(1 - 2 * d1 * std::cos(w) + d1 * d1);
  const double loopSec = len / sr;
  if (v.freeze) {
    v.rt60 = v.rt60Low = v.rt60High = 1e9;
  } else {
    // Loop T60s over the 1.12 calibration (the network's tail decays that much
    // faster than its loop gains predict, so the knob value is the measured T60).
    v.rt60 = t60(loopSec, g) / 1.12;
    v.decayKnob = rt;
    v.rt60Low = t60(loopSec, gLow) / 1.12;
    v.rt60High = t60(loopSec, g * hMag) / 1.12;
  }
  v.dampHz = d1 > 0 ? -std::log(std::max(1e-9, d1)) * sr / (2 * kPi) : 0;
  v.crossHz = pv(st, RvbCrossover, 500);
  v.size = std::clamp(size, 0.0, 1.0);
  v.diffusion = std::clamp(static_cast<double>(pv(st, RvbDiffusion, 1)), 0.0, 1.0);
  v.modDepth = std::clamp(pv(st, RvbModDepth) / 10.0, 0.0, 1.0);
  v.dry = pv(st, RvbDry, 1), v.er = pv(st, RvbEarly, 0.3f), v.wet = pv(st, RvbWet, 0.25f);
  static const double erMs[6] = {7.1, 11.3, 17.9, 23.3, 31.7, 41.9};
  static const double erGain[6] = {0.82, 0.71, 0.58, 0.49, 0.38, 0.29};
  for (int k = 0; k < 6; ++k) v.erSec[static_cast<size_t>(k)] = v.preSec + erMs[k] * (0.4 + size) * 0.001, v.erGain[static_cast<size_t>(k)] = erGain[k];
  v.erCount = 6;
  return v;
}

namespace {
using cplx = std::complex<double>;
// Complex response of a biquad at hz.
cplx biquadAt(const dsp::Biquad& b, double hz, double sr) {
  const cplx z1 = std::polar(1.0, -2 * kPi * hz / sr), z2 = z1 * z1;
  return (b.b0 + b.b1 * z1 + b.b2 * z2) / (1.0 + b.a1 * z1 + b.a2 * z2);
}
dsp::Biquad design(dsp::Biquad::Type type, double sr, double hz, double q, double db = 0) {
  dsp::Biquad b;
  b.set(type, sr, hz, q, db);
  return b;
}
// Bilinear first-order high-pass at hz.
cplx firstOrderHpAt(double fc, double hz, double sr) {
  const double K = std::tan(kPi * std::min(fc, sr * 0.49) / sr), n = 1.0 / (1.0 + K);
  const cplx z1 = std::polar(1.0, -2 * kPi * hz / sr);
  return (n - n * z1) / (1.0 + (K - 1.0) * n * z1);
}
}  // namespace

namespace {
// TPT state-variable filter taps at hz (the bilinear image of the analog
// prototype, prewarped at the cutoff — what dsp::Svf computes).
struct SvfTaps {
  cplx low, band, high;
};
SvfTaps svfAt(double fc, double reso01, double hz, double sr) {
  fc = std::clamp(fc, 10.0, sr * 0.49);
  const double k = 2.0 - 1.98 * std::clamp(reso01, 0.0, 1.0);
  const double w = std::tan(kPi * std::min(hz, sr * 0.4999) / sr) / std::tan(kPi * fc / sr);
  const cplx s(0, w), den = s * s + k * s + 1.0;
  return {1.0 / den, s / den, s * s / den};
}
}  // namespace

double filterResponseDb(const PluginState& st, double sr, double hz) {
  using namespace pp;
  sr = sr > 0 ? sr : 44100;
  const int mode = std::clamp(iv(st, FltMode), 0, 6);
  const double fc = pv(st, FltCutoff, 20000), reso = pv(st, FltReso), gain = pv(st, FltGain), mix = pv(st, FltMix, 1);
  const bool steep = iv(st, FltSlope) == 1, blend = pv(st, FltBlend) >= 0.5f;
  cplx y;
  if (mode >= 4 && !blend) {
    const dsp::Biquad b = design(mode == 4 ? dsp::Biquad::LowShelf : mode == 5 ? dsp::Biquad::Peak : dsp::Biquad::HighShelf, sr, fc, 0.5 + reso * 9.5,
                                 steep ? gain * 0.5 : gain);
    y = biquadAt(b, hz, sr);
    if (steep) y *= biquadAt(b, hz, sr);
  } else {
    const SvfTaps a = svfAt(fc, steep ? reso * 0.75 : reso, hz, sr);
    if (blend) {
      y = a.low * static_cast<double>(pv(st, FltLPLevel, 1)) + a.band * static_cast<double>(pv(st, FltBPLevel)) + a.high * static_cast<double>(pv(st, FltHPLevel));
    } else {
      auto pick = [&](const SvfTaps& t) { return mode == 0 ? t.low : mode == 1 ? t.band : mode == 2 ? t.high : t.low + t.high; };
      y = pick(a);
      if (steep) y *= pick(svfAt(fc, 0.0, hz, sr));
    }
  }
  return 20 * std::log10(std::max(1e-9, std::abs((1.0 - mix) + y * mix)));
}

LowLiftResponse lowLiftResponse(const PluginState& st, double sr, double hz) {
  using namespace pp;
  sr = sr > 0 ? sr : 44100;
  LowLiftResponse r;
  if (iv(st, LlModel) == 1) {  // reference engine
    const double fs = std::min(sr, 192000.0);
    const double fc = std::max(20.0, static_cast<double>(pv(st, LlXoverHz, 99.5f)));
    const dsp::Biquad lp = design(dsp::Biquad::LowPass, fs, fc, 0.7071067811865476), hp = design(dsp::Biquad::HighPass, fs, fc, 0.7071067811865476);
    const cplx lo = biquadAt(lp, hz, fs) * biquadAt(lp, hz, fs), hi = biquadAt(hp, hz, fs) * biquadAt(hp, hz, fs);
    auto lin = [](double db) { return db <= -119.999 ? 0.0 : std::pow(10.0, db / 20.0); };
    const double lowLin = lin(pv(st, LlLowDb)), highLin = lin(pv(st, LlHighDb)), s = lin(pv(st, LlSlopeDb, -6.5f)), pre = lin(pv(st, LlPreDb, 9));
    // Small-signal gain of the shaper (u + s·T2 + s²·T3) / (1 + 2s): (1 − 3s²) / (1 + 2s), after the pre gain.
    const double shapeGain = pre * (1 - 3 * s * s) / (1 + 2 * s);
    const cplx hp20 = biquadAt(design(dsp::Biquad::HighPass, fs, 20, 0.7071067811865476), hz, fs);
    const bool lowOn = pv(st, LlLowOn, 1) >= 0.5f, highOn = pv(st, LlHighOn, 1) >= 0.5f, inv = pv(st, LlInvertHarm) >= 0.5f;
    const cplx wl = lowOn ? lo * hp20 * (shapeGain * lowLin * (inv ? -1.0 : 1.0)) : cplx(0), wh = highOn ? hi * highLin : cplx(0);
    const cplx cut = firstOrderHpAt(std::max(1.0, static_cast<double>(pv(st, LlLowCutHz, 20))), hz, fs);
    const double dryW = pv(st, LlOn, 1) >= 0.5f ? 1 - pv(st, LlMix, 1) : 1.0;
    r.low = std::abs(wl * cut), r.high = std::abs(wh * cut);
    r.total = std::abs((lo + hi) * dryW + (wl + wh) * cut * (1 - dryW));
    return r;
  }
  // Legacy: low cut → (boost shelf) → LR4 split → band gains (harmonics are added on top, not drawn).
  const double xo = pv(st, LlCrossover, 120), lc = pv(st, LlLowCut, 25), boost = pv(st, LlBoost), bf = pv(st, LlBoostFreq, 60);
  const dsp::Biquad lp = design(dsp::Biquad::LowPass, sr, xo, 0.7071), hp = design(dsp::Biquad::HighPass, sr, xo, 0.7071);
  cplx pre = biquadAt(design(dsp::Biquad::HighPass, sr, lc, 0.7071), hz, sr);
  if (boost > 0.001) pre *= biquadAt(design(dsp::Biquad::LowShelf, sr, bf, 0.9, 18.0 * boost), hz, sr);
  const double lg = pv(st, LlLowOn, 1) >= 0.5f ? std::pow(10.0, pv(st, LlLowGain, 3) / 20.0) : 0.0;
  const double hg = pv(st, LlHighOn, 1) >= 0.5f ? std::pow(10.0, pv(st, LlHighGain) / 20.0) : 0.0;
  cplx lo, hi;
  if (pv(st, LlMultiband, 1) >= 0.5f) {
    lo = pre * biquadAt(lp, hz, sr) * biquadAt(lp, hz, sr) * lg, hi = pre * biquadAt(hp, hz, sr) * biquadAt(hp, hz, sr) * hg;
  } else {
    lo = pre * lg, hi = 0;
  }
  const double mix = pv(st, LlMix, 1);
  r.low = std::abs(lo), r.high = std::abs(hi);
  r.total = std::abs((1 - mix) + (lo + hi) * mix);
  return r;
}

double delay3Transfer(int distType, double thrN, double hard, double sym, double x) {
  const float tn = static_cast<float>(std::clamp(thrN, 0.0, 1.0));
  const float M = d3ref::makeUp(tn), T = std::pow(10.f, d3ref::threshDb(tn) / 20.f);
  if (distType == 0) {  // envelope limiter, steady state: |y| = min(|x|, T·M)
    const double lim = static_cast<double>(T) * M;
    return std::clamp(x, -lim, lim);
  }
  return static_cast<double>(d3ref::kneeApply(d3ref::kneeFor(tn, static_cast<float>(hard), static_cast<float>(sym)), static_cast<float>(x))) * M;
}

}  // namespace gl::graphmodel
