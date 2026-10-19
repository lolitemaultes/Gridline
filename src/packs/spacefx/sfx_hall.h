// Gridline — "spacefx" pack: Hall reverb (included by spacefx_dsp.cpp only).
//
// Signal flow: input gain → low/high cut → pre-delay ─┬→ early reflections (tapped) ─────────────┐
//                                                     └→ 4 allpass diffusers/side → 16-line FDN ─┤
// Shimmer: reverb out (feedback) or input (feed-forward) → delay → pitch shift → filters → back   │
// into the FDN input and/or straight to the wet bus.                                             │
// Wet: envelope (duck / gate / swell) → peak EQ → width → level; limiter on the sum. ←───────────┘
// The FDN uses a normalized Hadamard matrix (lossless) and per-line three-band
// absorption so RT60 is set independently below/above two crossovers.
#pragma once
#include <complex>
#include "sfx_common.h"
#include "sfx_hallref.h"

namespace gl::packs::spacefx::detail {

constexpr int kHallLines = 16;
constexpr double kHallBaseMs[kHallLines] = {31.1, 37.3, 41.9, 47.7, 53.3, 59.9, 66.1, 71.3, 79.7, 83.9, 89.3, 97.1, 103.7, 109.9, 117.1, 127.3};
inline double hallSizeScale(double size) { return 0.25 + 1.35 * clampd(size, 0, 1); }

// One-pole low-pass (as OnePole::lp) frequency response at hz.
inline std::complex<double> onePoleLp(double sr, double fc, double hz) {
  double a = std::exp(-kTwoPi * clampd(fc, 1.0, sr * 0.49) / sr);
  std::complex<double> z = std::polar(1.0, -kTwoPi * hz / sr);
  return (1.0 - a) / (1.0 - a * z);
}

// Per-pass gain of the three-band absorption filter at hz.
inline double hallBandGain(double sr, double hz, double gL, double gM, double gH, double xl, double xh) {
  std::complex<double> lo = onePoleLp(sr, xl, hz), lpH = onePoleLp(sr, xh, hz);
  std::complex<double> hi = 1.0 - lpH, mid = 1.0 - lo - hi;
  return std::abs(gL * lo + gM * mid + gH * hi);
}

class HallFx final : public Effect {
 public:
  HallFx() {
    for (auto& l : line_) l.init(static_cast<size_t>(0.130 * 1.6 * kMaxRate) + 1024);
    pre_[0].init(static_cast<size_t>(0.62 * kMaxRate));
    pre_[1].init(static_cast<size_t>(0.62 * kMaxRate));
    for (auto& d : diff_) d.init(static_cast<size_t>(0.02 * kMaxRate));
    shim_.init(static_cast<size_t>(2.1 * kMaxRate));
    pitch_.init(static_cast<size_t>(0.2 * kMaxRate));
    rng_.seed(0x51A11u);
    reset();
  }
  void setSideIo(const SideIo& io) override { side_ = io; }
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    refModel_ = pi(s, HvModel, 1) == 1;
    if (refModel_) {
      ref_.process(s, ctx, side_.scL, side_.scR, L, R, n);
      side_ = SideIo{};
      return;
    }
    side_ = SideIo{};
    const double sr = ctx.sampleRate;
    setup(s, ctx);
    for (int i = 0; i < n; ++i) {
      double xl = L[i], xr = R[i];
      // Envelope follower on the dry input (drives duck / gate / swell).
      double lvl = std::max(std::fabs(xl), std::fabs(xr));
      envLvl_ = lvl > envLvl_ ? lvl + (envLvl_ - lvl) * detA_ : lvl + (envLvl_ - lvl) * detR_;
      double envDb = dsp::gainToDb(envLvl_);
      // Input conditioning.
      double il = xl * inGain_, ir = xr * inGain_;
      il = inHp_[0].process(inLp_[0].process(il));
      ir = inHp_[1].process(inLp_[1].process(ir));
      pre_[0].push(static_cast<float>(il));
      pre_[1].push(static_cast<float>(ir));
      double pl = preD_ >= 1 ? pre_[0].read(preD_ + 1) : il, pr = preD_ >= 1 ? pre_[1].read(preD_ + 1) : ir;
      // Early reflections from the pre-delayed input.
      double el = 0, er = 0;
      if (erLevel_ > 0)
        for (int k = 0; k < kEr; ++k) {
          el += pre_[0].read(preD_ + 1 + erD_[k]) * erG_[k];
          er += pre_[1].read(preD_ + 1 + erD_[k] * 1.07 + 11) * erG_[(k + 3) % kEr];
        }
      // Shimmer path (pitch-shifted delay) fed from the previous wet output or the input.
      double shimOut = 0;
      if (fbOn_) {
        double src = fbMode_ == 0 ? 0.5 * (lastWetL_ + lastWetR_) : 0.5 * (pl + pr);
        shim_.push(static_cast<float>(src));
        double d = shim_.read(fbDelay_);
        pitch_.push(static_cast<float>(d));
        double ps = pitchShift();
        ps = fbLp_.process(fbHp_.process(ps));
        shimOut = soft(ps * fbGain_);
      }
      // FDN input.
      double inL = freeze_ == 1 ? 0.0 : pl + shimOut * fbMix_;
      double inR = freeze_ == 1 ? 0.0 : pr + shimOut * fbMix_;
      if (freeze_ == 2) {
        inL *= 0.3;
        inR *= 0.3;
      }
      for (int k = 0; k < 4; ++k) {
        inL = diff_[k].process(static_cast<float>(inL), diffD_[k], diffG_);
        inR = diff_[k + 4].process(static_cast<float>(inR), diffD_[k] * 1.13, diffG_);
      }
      // FDN: read, absorb, mix (Hadamard), write.
      double y[kHallLines];
      double outL = 0, outR = 0;
      for (int k = 0; k < lines_; ++k) {
        double md = 0;
        if (modDepth_ > 0) {
          md = modDepth_ * std::sin(kTwoPi * modPh_[k]);
          modPh_[k] += modInc_[k];
          if (modPh_[k] >= 1) modPh_[k] -= 1;
        }
        double v = line_[k].read(lineD_[k] + md);
        if (freeze_ == 0) {
          double lo = abL_[k].lp(static_cast<float>(v));
          double hiLp = abH_[k].lp(static_cast<float>(v));
          double hi = v - hiLp, mid = v - lo - hi;
          v = gL_[k] * lo + gM_[k] * mid + gH_[k] * hi;
        }
        y[k] = v;
        outL += (k & 1) ? -v : v;
        outR += ((k >> 1) & 1) ? -v : v;
      }
      hadamard(y, lines_);
      const double nrm = 1.0 / std::sqrt(static_cast<double>(lines_));
      for (int k = 0; k < lines_; ++k) {
        double inj = (k & 1) ? inR : inL;
        inj *= ((k >> 1) & 1) ? -1.0 : 1.0;
        double v = y[k] * nrm + inj * injG_;
        if (freeze_ == 2) v = soft(v);
        line_[k].push(flush(static_cast<float>(v)));
      }
      double late = revOn_ ? outNorm_ : 0.0;
      double wl = outL * late + el * erLevel_ + shimOut * (1.0 - fbMix_);
      double wr = outR * late + er * erLevel_ + shimOut * (1.0 - fbMix_);
      lastWetL_ = wl;
      lastWetR_ = wr;
      // Envelope shaping of the wet signal.
      if (envMode_ > 0) {
        double target;
        if (envMode_ == 1)
          target = 1.0 - clampd((envDb - envThr_) / 18.0, 0.0, 1.0);  // duck
        else
          target = envDb > envThr_ ? 1.0 : 0.0;  // gate / swell
        double c = target > envGain_ ? envAtk_ : envRel_;
        envGain_ = target + (envGain_ - target) * c;
        double g = (1.0 - envAmt_) + envAmt_ * envGain_;
        wl *= g;
        wr *= g;
        envOut_ = g;
      } else {
        envOut_ = 1.0;
      }
      wl = eq_[0].process(wl);
      wr = eq_[1].process(wr);
      double m = 0.5 * (wl + wr), sd = 0.5 * (wl - wr) * width_;
      double ol = xl * dry_ + (m + sd) * wet_, orr = xr * dry_ + (m - sd) * wet_;
      if (limiter_) {
        double pk = std::max(std::fabs(ol), std::fabs(orr));
        double need = pk > 0.98 ? 0.98 / pk : 1.0;
        limG_ = need < limG_ ? need : limG_ + (1.0 - limG_) * limRel_;
        if (limG_ > 1.0) limG_ = 1.0;
        ol *= limG_;
        orr *= limG_;
      }
      L[i] = flush(static_cast<float>(ol));
      R[i] = flush(static_cast<float>(orr));
    }
    (void)sr;
  }
  void reset() override {
    ref_.reset();
    for (auto& l : line_) l.clear();
    pre_[0].clear();
    pre_[1].clear();
    for (auto& d : diff_) d.clear();
    shim_.clear();
    pitch_.clear();
    for (int k = 0; k < kHallLines; ++k) {
      abL_[k].clear();
      abH_[k].clear();
      modPh_[k] = static_cast<double>(k) / kHallLines;
    }
    for (auto& f : inLp_) f.clear();
    for (auto& f : inHp_) f.clear();
    for (auto& f : eq_) f.clear();
    fbLp_.clear();
    fbHp_.clear();
    lastWetL_ = lastWetR_ = envLvl_ = 0;
    envGain_ = 1;
    limG_ = 1;
    pitchPh_ = 0;
  }
  float meter() const override { return refModel_ ? ref_.meterDb() : static_cast<float>(dsp::gainToDb(std::max(1e-6, envOut_))); }

 private:
  static constexpr int kEr = 12;
  static double soft(double x) { return std::fabs(x) < 0.8 ? x : std::tanh(x); }
  static void hadamard(double* y, int n) {
    for (int len = 1; len < n; len <<= 1)
      for (int i = 0; i < n; i += len << 1)
        for (int j = i; j < i + len; ++j) {
          double a = y[j], b = y[j + len];
          y[j] = a + b;
          y[j + len] = a - b;
        }
  }
  // Two crossfaded read heads sweeping a short window (classic delay-line pitch shifter).
  double pitchShift() {
    const double W = 0.06 * sr_;
    pitchPh_ += (1.0 - pitchRatio_) / W;
    pitchPh_ -= std::floor(pitchPh_);
    double p2 = pitchPh_ + 0.5;
    p2 -= std::floor(p2);
    double a = pitch_.read(2 + pitchPh_ * W), b = pitch_.read(2 + p2 * W);
    double wa = std::sin(kPi * pitchPh_), wb = std::sin(kPi * p2);
    return a * wa * wa + b * wb * wb;
  }
  void setup(const PluginState& s, const ProcessContext& ctx) {
    const double sr = ctx.sampleRate;
    sr_ = sr;
    inGain_ = dbg(pv(s, HvInGain, 0));
    inLp_[0].set(dsp::Biquad::LowPass, sr, pv(s, HvInHighCut, 13000), 0.707, 0);
    inHp_[0].set(dsp::Biquad::HighPass, sr, pv(s, HvInLowCut, 20), 0.707, 0);
    copyBq(inLp_[1], inLp_[0]);
    copyBq(inHp_[1], inHp_[0]);
    revOn_ = pb(s, HvOn);
    double pre = pv(s, HvPreDelay, 12) * 0.001;
    if (pb(s, HvPreSync)) pre = kSyncBeats[std::clamp(pi(s, HvPreSyncTime, 1), 0, 8)] * 60.0 / std::max(1.0, ctx.tempo);
    preD_ = clampd(pre * sr, 0.0, 0.5 * sr);
    double size = pv(s, HvSize, 0.55f), scale = hallSizeScale(size);
    double rt = std::max(0.05, static_cast<double>(pv(s, HvDecay, 2.6f)));
    double lowM = pv(s, HvLowMult, 1.2f), highM = pv(s, HvHighMult, 0.55f);
    double xl = pv(s, HvLowXover, 250), xh = pv(s, HvHighXover, 4500);
    lines_ = pb(s, HvHQ) ? 16 : 8;
    freeze_ = std::clamp(pi(s, HvFreeze, 0), 0, 2);
    double modDepth = pv(s, HvModDepth, 0.2f), modRate = pv(s, HvModRate, 0.7f);
    modDepth_ = modDepth * 0.0016 * sr;
    for (int k = 0; k < lines_; ++k) {
      int src = lines_ == 16 ? k : k * 2 + 1;
      lineD_[k] = kHallBaseMs[src] * 0.001 * scale * sr;
      double d = lineD_[k] / sr;
      gL_[k] = std::pow(10.0, -3.0 * d / (rt * lowM));
      gM_[k] = std::pow(10.0, -3.0 * d / rt);
      gH_[k] = std::pow(10.0, -3.0 * d / (rt * highM));
      abL_[k].setHz(sr, xl);
      abH_[k].setHz(sr, xh);
      modInc_[k] = modRate * (0.8 + 0.4 * k / static_cast<double>(lines_)) / sr;
    }
    injG_ = 1.0 / std::sqrt(lines_ * 0.5);
    outNorm_ = 0.9 / std::sqrt(static_cast<double>(lines_));
    // Early reflections.
    double ch = pv(s, HvCharacter, 0.5f);
    erLevel_ = ch * 0.9;
    static const double erMs[kEr] = {3.1, 5.3, 7.9, 10.6, 13.4, 17.2, 21.0, 25.9, 31.3, 38.0, 45.2, 53.9};
    static const double erGain[kEr] = {0.85, -0.72, 0.66, -0.6, 0.52, 0.47, -0.42, 0.37, -0.32, 0.28, -0.24, 0.2};
    double erScale = 0.4 + 1.2 * size;
    for (int k = 0; k < kEr; ++k) {
      erD_[k] = erMs[k] * 0.001 * erScale * sr;
      erG_[k] = erGain[k] * 0.5;
    }
    double diff = pv(s, HvDiffusion, 0.85f);
    diffG_ = static_cast<float>(0.72 * diff);
    static const double dMs[4] = {4.7, 6.3, 8.9, 11.7};
    for (int k = 0; k < 4; ++k) diffD_[k] = dMs[k] * 0.001 * (0.5 + 0.5 * size) * sr;
    // Shimmer.
    fbOn_ = pb(s, HvFbOn);
    fbMode_ = pi(s, HvFbMode, 0);
    pitchRatio_ = std::pow(2.0, pv(s, HvFbPitch, 12) / 12.0);
    fbGain_ = pv(s, HvFbGain, 0.45f) * (fbMode_ == 0 ? 0.9 : 1.0);
    fbDelay_ = clampd(pv(s, HvFbDelay, 120) * 0.001 * sr, 1.0, 2.0 * sr);
    fbHp_.set(dsp::Biquad::HighPass, sr, pv(s, HvFbLowCut, 300), 0.707, 0);
    fbLp_.set(dsp::Biquad::LowPass, sr, pv(s, HvFbHighCut, 7000), 0.707, 0);
    fbMix_ = pv(s, HvFbMix, 1);
    // Envelope.
    envMode_ = pi(s, HvEnvMode, 0);
    envThr_ = pv(s, HvEnvThresh, -30);
    envAmt_ = pv(s, HvEnvAmount, 0.7f);
    double atk = pv(s, HvEnvAttack, 15) * 0.001, rel = pv(s, HvEnvRelease, 350) * 0.001;
    if (envMode_ == 3) atk *= 8;  // swell: slow fade-in of the tail
    envAtk_ = std::exp(-1.0 / std::max(1.0, atk * sr));
    envRel_ = std::exp(-1.0 / std::max(1.0, rel * sr));
    detA_ = std::exp(-1.0 / (0.001 * sr));
    detR_ = std::exp(-1.0 / (0.08 * sr));
    // Output.
    dry_ = pv(s, HvDry, 0) <= -59.9f ? 0.0 : dbg(pv(s, HvDry, 0));
    wet_ = pv(s, HvWet, -9) <= -59.9f ? 0.0 : dbg(pv(s, HvWet, -9));
    eq_[0].set(dsp::Biquad::Peak, sr, pv(s, HvEqFreq, 3000), pv(s, HvEqQ, 0.8f), pv(s, HvEqGain, 0));
    copyBq(eq_[1], eq_[0]);
    width_ = pv(s, HvWidth, 1.2f);
    limiter_ = pb(s, HvLimiter);
    limRel_ = 1.0 - std::exp(-1.0 / (0.08 * sr));
  }
  static void copyBq(dsp::Biquad& d, const dsp::Biquad& s) {
    d.b0 = s.b0;
    d.b1 = s.b1;
    d.b2 = s.b2;
    d.a1 = s.a1;
    d.a2 = s.a2;
  }

  HallRef ref_;
  SideIo side_;
  bool refModel_ = true;
  Ring line_[kHallLines], pre_[2], shim_, pitch_;
  Allpass diff_[8];
  OnePole abL_[kHallLines], abH_[kHallLines];
  dsp::Biquad inLp_[2], inHp_[2], eq_[2], fbLp_, fbHp_;
  dsp::Noise rng_;
  double lineD_[kHallLines] = {}, gL_[kHallLines] = {}, gM_[kHallLines] = {}, gH_[kHallLines] = {}, modPh_[kHallLines] = {},
         modInc_[kHallLines] = {};
  double erD_[kEr] = {}, erG_[kEr] = {}, diffD_[4] = {};
  double sr_ = 48000, inGain_ = 1, preD_ = 0, modDepth_ = 0, injG_ = 0.35, outNorm_ = 0.2, erLevel_ = 0, pitchRatio_ = 2, fbGain_ = 0,
         fbDelay_ = 100, fbMix_ = 1, envThr_ = -30, envAmt_ = 0, envAtk_ = 0.99, envRel_ = 0.999, detA_ = 0.9, detR_ = 0.999, dry_ = 1,
         wet_ = 0.3, width_ = 1, limRel_ = 0.001;
  double lastWetL_ = 0, lastWetR_ = 0, envLvl_ = 0, envGain_ = 1, envOut_ = 1, limG_ = 1, pitchPh_ = 0;
  float diffG_ = 0.6f;
  int lines_ = 16, freeze_ = 0, fbMode_ = 0, envMode_ = 0;
  bool revOn_ = true, fbOn_ = false, limiter_ = false;
};

}  // namespace gl::packs::spacefx::detail
