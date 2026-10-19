// Gridline — "spacefx" pack: Hall reference model (HvModel = 1; included by
// sfx_hall.h only). Behaviour follows the J-1 study of the classic big reverb
// (analysis/demos/specs/j1_luxeverb.md); the implementation is original.
//
//   in ×gain → 1-pole low cut → 1-pole high cut → [feed-forward pitch voices]
//   → pre-delay → (1−d)·x + d·(8 all-passes per side) → 16-line FDN
//     (prime-table lengths from Size, Givens-rotated Hadamard mixing whose angle
//      is Character, per-line two-band shelf absorption from Decay + Brightness,
//      parabolic-LFO length modulation, shimmer injection)
//   → line sum ×0.125·k → [limiter] → peak EQ → M/S width → ×wet ; + in ×dry
// Envelope follower (input or sidechain → 12 dB/oct HPF → peak follower →
// smoothing → dB) modulates the wet level or the decay time.
// All buffers are allocated in the constructor for 192 kHz; process() never
// allocates and works per sample (output independent of the block size).
#pragma once
#include "sfx_common.h"

namespace gl::packs::spacefx::detail {

// The first primes (sieve, built once on first use — the constructor calls it).
inline const std::vector<int>& hallPrimes() {
  static const std::vector<int> p = [] {
    std::vector<int> out;
    const int lim = 40000;
    std::vector<char> comp(static_cast<size_t>(lim), 0);
    for (int i = 2; i < lim; ++i) {
      if (comp[static_cast<size_t>(i)]) continue;
      out.push_back(i);
      for (long j = static_cast<long>(i) * i; j < lim; j += i) comp[static_cast<size_t>(j)] = 1;
    }
    return out;
  }();
  return p;
}

// Reference line lengths (samples at 44.1 kHz) for HvSize 0..1 (S = 1 + 99·size).
inline void hallRefLengths(double size01, int out[16]) {
  static const double T[16] = {155, 217, 296, 313, 398, 388, 494, 531, 601, 599, 664, 722, 755, 809, 870, 900};
  const std::vector<int>& pr = hallPrimes();
  double S = 1.0 + 99.0 * clampd(size01, 0, 1);
  double a = 19.685 * S + 230.315, b = 1.357586 * S + 103.81483;
  double m = -0.20805368 * a * (1.0 - 5.806452 * b / a), sp = 0.006451613 * (b - m);
  for (int k = 0; k < 16; ++k) {
    long idx = static_cast<long>(std::floor(T[k] * sp + m));
    idx = std::clamp(idx, 0L, static_cast<long>(pr.size()) - 1);
    out[k] = pr[static_cast<size_t>(idx)];
  }
}
// Output gain factor k (bigger rooms are not quieter).
inline double hallRefOutK(double size01) {
  double S = 1.0 + 99.0 * clampd(size01, 0, 1);
  return 0.6666667 + 0.0013333333 * (19.685 * S + 230.315);
}

// Two crossfaded read heads on a delay ring (delay + pitch shift in one).
struct PitchVoice {
  Ring r;
  double ph = 0;
  void init(size_t n) { r.init(n); }
  void clear() {
    r.clear();
    ph = 0;
  }
  // Push x, return the signal delayed by `d` samples and shifted by `ratio`.
  float process(float x, double d, double ratio, double W, bool hq) {
    r.push(x);
    const double base = std::max(1.0, d) + 1.0;  // x was just pushed: read(1) is the current sample
    if (std::fabs(ratio - 1.0) < 1e-6) return hq ? r.read(base) : r.readLinear(base);
    ph += (1.0 - ratio) / W;
    ph -= std::floor(ph);
    double p2 = ph + 0.5;
    p2 -= std::floor(p2);
    double a = hq ? r.read(base + ph * W) : r.readLinear(base + ph * W);
    double b = hq ? r.read(base + p2 * W) : r.readLinear(base + p2 * W);
    double wa = std::sin(kPi * ph), wb = std::sin(kPi * p2);
    return static_cast<float>(a * wa * wa + b * wb * wb);
  }
};

// Bilinear one-pole (TPT) low pass; hp = x − lp.
struct TptOnePole {
  double s = 0, g = 0;
  void set(double sr, double hz) {
    double w = std::tan(kPi * clampd(hz, 1.0, sr * 0.49) / sr);
    g = w / (1.0 + w);
  }
  double lp(double x) {
    double v = (x - s) * g, y = v + s;
    s = flushd(y + v);
    return y;
  }
  void clear() { s = 0; }
};

class HallRef {
 public:
  static constexpr int kN = 16;
  static constexpr double kFfWindow = 0.05;  // s, pitch-shifter crossfade window

  HallRef() {
    (void)hallPrimes();
    for (auto& l : line_) l.init(static_cast<size_t>(0.64 * kMaxRate));
    for (auto& p : pre_) p.init(static_cast<size_t>(4.1 * kMaxRate));
    for (auto& d : diff_) d.init(static_cast<size_t>(1000.0 * kMaxRate / 44100.0));
    shim_.init(static_cast<size_t>(2.2 * kMaxRate));
    for (auto& v : ff_) v.init(static_cast<size_t>(2.2 * kMaxRate));
    reset();
  }

  void reset() {
    for (auto& l : line_) l.clear();
    for (auto& p : pre_) p.clear();
    for (auto& d : diff_) d.clear();
    shim_.clear();
    for (auto& v : ff_) v.clear();
    for (int k = 0; k < kN; ++k) shX_[k] = shY_[k] = 0;
    for (auto& f : inLp_) f.clear();
    for (auto& f : inHp_) f.clear();
    for (auto& f : eq_) f = dsp::Biquad{};
    fbHp_ = fbLp_ = envHp_ = dsp::Biquad{};
    lfoPh_ = 0;
    tankL_ = tankR_ = 0;
    envPk_ = envSm_ = 0;
    limLvl_ = 0;
    limG_ = 1;
    rr_ = 0;
    primed_ = false;
    tankLive_ = true;
    shimLive_ = true;
    lastEnvDb_ = -120;
  }

  float meterDb() const { return static_cast<float>(std::clamp(lastEnvDb_, -120.0, 0.0)); }

  void process(const PluginState& s, const ProcessContext& ctx, const float* scL, const float* scR, float* L, float* R, int n) {
    setup(s, ctx);
    for (int i = 0; i < n; ++i) {
      const double xl = L[i], xr = R[i];
      // Smoothed controls.
      const double inG = sIn_.next(inGain_), dry = sDry_.next(dry_), wetT = sWet_.next(wet_);
      const double gate = sGate_.next(freezeIn_), decEn = sDecEn_.next(decayOn_);
      // Input conditioning: low cut then high cut (6 dB/oct each).
      double il = xl * inG, ir = xr * inG;
      il -= inHp_[0].lp(il);
      ir -= inHp_[1].lp(ir);
      il = inLp_[0].lp(il);
      ir = inLp_[1].lp(ir);
      // Envelope follower.
      double det = (envSc_ && scL && scR) ? 0.5 * (scL[i] + scR[i]) : 0.5 * (il + ir);
      det = std::fabs(envHp_.process(det));
      envPk_ = det > envPk_ ? det + (envPk_ - det) * envAtk_ : det + (envPk_ - det) * envRel_;
      envSm_ += (envPk_ - envSm_) * envSmooth_;
      envSm_ = flushd(envSm_);
      const double envDb = envSm_ > 1e-10 ? 20.0 * std::log10(envSm_) : -200.0;
      lastEnvDb_ = envDb;
      const double envMod = envScale_ * (std::max(envDb - envThr_, 0.0) + envOffset_ * envThr_);
      // Feed-forward pitch voices replace the reverb input.
      if (ffOn_) {
        double vl = il, vr = ir;
        il = ffDirect_ * vl + ffAmp_[0] * ff_[0].process(static_cast<float>(vl), ffD_[0], ffRatio_[0], ffW_, hq_) +
             ffAmp_[1] * ff_[1].process(static_cast<float>(vl), ffD_[1], ffRatio_[1], ffW_, hq_);
        ir = ffDirect_ * vr + ffAmp_[0] * ff_[2].process(static_cast<float>(vr), ffD_[0], ffRatio_[0], ffW_, hq_) +
             ffAmp_[1] * ff_[3].process(static_cast<float>(vr), ffD_[1], ffRatio_[1], ffW_, hq_);
      }
      const double srcL = il, srcR = ir;
      // Pre-delay.
      pre_[0].push(static_cast<float>(il));
      pre_[1].push(static_cast<float>(ir));
      if (preD_ >= 1.0) {
        il = hq_ ? pre_[0].read(preD_ + 1) : pre_[0].readLinear(preD_ + 1);
        ir = hq_ ? pre_[1].read(preD_ + 1) : pre_[1].readLinear(preD_ + 1);
      }
      il *= gate;
      ir *= gate;
      // Input diffusion.
      if (diff_g_ > 0) {
        double al = il, ar = ir;
        for (int k = 0; k < 8; ++k) {
          al = diff_[k].process(static_cast<float>(al), apD_[k], 0.6f);
          ar = diff_[k + 8].process(static_cast<float>(ar), apD_[k + 8], 0.6f);
        }
        il = (1.0 - diff_g_) * il + diff_g_ * al;
        ir = (1.0 - diff_g_) * ir + diff_g_ * ar;
      }
      // Shimmer (feedback pitch path) into the tank input.
      double shim = 0;
      if (fbOn_) {
        double src = 0.5 * ((1.0 - fbMix_) * (srcL + srcR) + fbMix_ * (tankL_ + tankR_));
        double ps = shim_.process(static_cast<float>(src), fbD_, fbRatio_, fbW_, hq_);
        ps = fbLp_.process(fbHp_.process(ps));
        shim = ps * fbGain_ * decEn;
        if (std::fabs(shim) > 1.0) shim = std::tanh(shim);
      }
      // Decay (envelope → decay) and the round-robin absorption refresh.
      double T = sDecay_.next(decay_);
      if (envRoute_ == HeDecay) T += envMod;
      T = clampd(T, 0.25, 20.0);
      refreshLine(rr_, T, decEn);
      rr_ = (rr_ + 1) % kN;
      // FDN.
      double y[kN];
      lfoPh_ += lfoInc_;
      lfoPh_ -= std::floor(lfoPh_);
      double outL = 0, outR = 0;
      if (tankLive_) {
        for (int k = 0; k < kN; ++k) {
          double d = len_[k];
          if (modRel_ > 0) {
            double ph = lfoPh_ + k * (1.0 / kN);
            ph -= std::floor(ph);
            double x2 = 2.0 * ph;  // parabolic sine: +x(1−x) then −(x−1)(2−x)
            double par = x2 < 1.0 ? x2 - x2 * x2 : -((x2 - 1.0) - (x2 - 1.0) * (x2 - 1.0));
            d *= 1.0 + modRel_ * 4.0 * par;
          }
          double v = hq_ ? line_[k].read(d) : line_[k].readLinear(d);
          // Two-band shelf absorption.
          double w = sb0_[k] * v + sb1_[k] * shX_[k] - sa1_[k] * shY_[k];
          shX_[k] = v;
          shY_[k] = flushd(w);
          y[k] = w;
          double o = ((k >> 2) & 1) ? -w : w;
          if (k & 1)
            outR += o;
          else
            outL += o;
        }
        mix(y);
        for (int k = 0; k < kN; ++k) {
          double inj = (k & 1) ? ir : il;
          if ((k >> 1) & 1) inj = -inj;
          double v = y[k] + 0.5 * (inj + shim);
          if (std::fabs(v) > 4.0) v = 4.0 * std::tanh(v * 0.25);  // safety for sustain / runaway shimmer
          line_[k].push(flush(static_cast<float>(v)));
        }
      }
      outL *= 0.125 * outK_;
      outR *= 0.125 * outK_;
      tankL_ = outL;
      tankR_ = outR;
      double wl = outL, wr = outR;
      if (limiter_) {
        double pk = std::max(std::fabs(wl), std::fabs(wr));
        limLvl_ = pk > limLvl_ ? pk : pk + (limLvl_ - pk) * limRel_;
        double tg = 1.0 / std::max(1.0, 4.0 * limLvl_);
        limG_ = tg < limG_ ? tg : tg + (limG_ - tg) * limRel_;
        wl *= limG_;
        wr *= limG_;
      }
      wl = eq_[0].process(wl);
      wr = eq_[1].process(wr);
      double m = 0.5 * (wl + wr), sd = 0.5 * (wl - wr) * width_;
      double wet = wetT;
      if (envRoute_ == HeWet) wet = clampd(wet * std::pow(10.0, clampd(envMod, -120.0, 40.0) / 20.0), 0.0, 5.6);
      L[i] = flush(static_cast<float>(xl * dry + (m + sd) * wet));
      R[i] = flush(static_cast<float>(xr * dry + (m - sd) * wet));
    }
  }

 private:
  // Lossless 16×16 mixing: Hadamard inside each block of 4, then Givens
  // rotations (half of θ = 90°·character) across the blocks.
  void mix(double* y) const {
    for (int b = 0; b < 4; ++b) {
      double* q = y + 4 * b;
      double a0 = q[0] + q[1], a1 = q[0] - q[1], a2 = q[2] + q[3], a3 = q[2] - q[3];
      q[0] = 0.5 * (a0 + a2);
      q[1] = 0.5 * (a1 + a3);
      q[2] = 0.5 * (a0 - a2);
      q[3] = 0.5 * (a1 - a3);
    }
    if (rs_ == 0) return;
    for (int j = 0; j < 4; ++j) {
      double v0 = y[j], v1 = y[4 + j], v2 = y[8 + j], v3 = y[12 + j];
      double r0 = rc_ * v0 - rs_ * v1, r1 = rs_ * v0 + rc_ * v1;  // pairs (0,1), (2,3)
      double r2 = rc_ * v2 - rs_ * v3, r3 = rs_ * v2 + rc_ * v3;
      y[j] = rc_ * r0 - rs_ * r2;  // then pairs (0,2), (1,3)
      y[8 + j] = rs_ * r0 + rc_ * r2;
      y[4 + j] = rc_ * r1 - rs_ * r3;
      y[12 + j] = rs_ * r1 + rc_ * r3;
    }
  }
  void refreshLine(int k, double T, double decEn) {
    double d = len_[k] / sr_;
    double glo = std::pow(10.0, -3.0 * d / T), ghi = std::pow(10.0, -3.0 * d / std::max(1e-3, bright_ * T));
    glo = 1.0 - decEn * (1.0 - glo);
    ghi = 1.0 - decEn * (1.0 - ghi);
    const double a = shelfA_;
    sb0_[k] = (glo + a * ghi) / (1.0 + a);
    sb1_[k] = (glo - a * ghi) / (1.0 + a);
    sa1_[k] = (1.0 - a) / (1.0 + a);
  }
  static double pitchComp(double st) {
    return 0.5 + (st > 0 ? 0.058333 : -0.166667) * st;
  }
  void setup(const PluginState& s, const ProcessContext& ctx) {
    const double sr = ctx.sampleRate;
    const bool srChanged = sr != sr_;
    sr_ = sr;
    const double ratio = sr / 44100.0;
    inGain_ = levelGain(pv(s, HvInGain, 0));
    dry_ = levelGain(pv(s, HvDry, 0));
    wet_ = levelGain(pv(s, HvWet, -9));
    inHp_[0].set(sr, pv(s, HvInLowCut, 20));
    inHp_[1] = TptOnePole{inHp_[1].s, inHp_[0].g};
    inLp_[0].set(sr, pv(s, HvInHighCut, 13000));
    inLp_[1] = TptOnePole{inLp_[1].s, inLp_[0].g};
    hq_ = pb(s, HvHQ);
    // Reverb section.
    bool on = pb(s, HvOn);
    if (!on && tankLive_) {
      for (auto& l : line_) l.clear();
      for (int k = 0; k < kN; ++k) shX_[k] = shY_[k] = 0;
    }
    tankLive_ = on;
    decay_ = clampd(pv(s, HvDecay, 2.6f), 0.25, 20.0);
    bright_ = clampd(pv(s, HvBright, 0.85f), 0.0, 1.0);
    double pre = pv(s, HvPreDelay, 12) * 0.001;
    if (pb(s, HvPreSync)) pre = hallTicksSeconds(pv(s, HvPreTicks, 192), ctx.tempo);
    preD_ = clampd(pre * sr, 0.0, 4.0 * sr);
    int lens[16];
    hallRefLengths(pv(s, HvSize, 0.55f), lens);
    for (int k = 0; k < kN; ++k) len_[k] = lens[k] * ratio;
    outK_ = hallRefOutK(pv(s, HvSize, 0.55f));
    double th = 0.5 * (kPi / 2.0) * clampd(pv(s, HvCharacter, 0.5f), 0, 1);
    rc_ = std::cos(th);
    rs_ = std::sin(th);
    if (rs_ < 1e-12) rs_ = 0;
    lfoInc_ = clampd(pv(s, HvModFreq, 0.1f), 0, 1) / sr;
    modRel_ = 0.1 * clampd(pv(s, HvModDepth, 0.1f), 0, 1) * ratio;
    diff_g_ = clampd(pv(s, HvDiffusion, 1), 0, 1);
    static const int apL[8] = {113, 167, 229, 317, 421, 547, 677, 829};
    static const int apR[8] = {127, 181, 241, 331, 439, 563, 691, 853};
    for (int k = 0; k < 8; ++k) {
      apD_[k] = apL[k] * ratio;
      apD_[k + 8] = apR[k] * ratio;
    }
    shelfA_ = sr * 0.000149807;
    int fz = std::clamp(pi(s, HvFreeze, 0), 0, 3);
    freezeIn_ = fz == HfFreeze ? 0.0 : 1.0;
    decayOn_ = fz == HfNormal ? 1.0 : 0.0;
    if (!primed_ || srChanged) {
      for (int k = 0; k < kN; ++k) refreshLine(k, decay_, decayOn_);
      primed_ = true;
      for (Smooth* sm : {&sIn_, &sDry_, &sWet_, &sGate_, &sDecEn_}) sm->setTime(sr, 0.01);
      sDecay_.setTime(sr, 0.05);
    }
    // Shimmer / feed-forward.
    bool fbLed = pb(s, HvFbOn);
    bool ff = pi(s, HvFbMode, 0) == 1;
    fbOn_ = fbLed && !ff;
    ffOn_ = fbLed && ff;
    if (!fbOn_ && shimLive_) shim_.clear();
    shimLive_ = fbOn_;
    double st = pv(s, HvFbPitch, 12);
    fbRatio_ = std::pow(2.0, st / 12.0);
    double fbDelay = pv(s, HvFbDelay, 100) * 0.001;
    if (pb(s, HvFbSync)) fbDelay = hallTicksSeconds(pv(s, HvFbTicks, 192), ctx.tempo);
    fbW_ = kFfWindow * sr;
    fbD_ = clampd(fbDelay * sr, 1.0, 2.2 * sr - fbW_ - 8);
    fbGain_ = clampd(pv(s, HvFbGain, 0.7f), 0, 1) * 8.0 * std::min(1.0, std::pow(decay_, -0.2)) * pitchComp(st);
    fbMix_ = clampd(pv(s, HvFbMix, 1), 0, 1);
    fbHp_.set(dsp::Biquad::HighPass, sr, pv(s, HvFbLowCut, 20), 0.70710678, 0);
    fbLp_.set(dsp::Biquad::LowPass, sr, pv(s, HvFbHighCut, 5000), 0.70710678, 0);
    ffDirect_ = clampd(pv(s, HvFfDirect, 0), 0, 1);
    for (int v = 0; v < 2; ++v) {
      ffAmp_[v] = clampd(pv(s, HvFfAmp1 + v, 0), 0, 1);
      ffRatio_[v] = std::pow(2.0, pv(s, HvFfPitch1 + v, 0) / 12.0);
      double d = pv(s, HvFfDelay1 + v, 100) * 0.001;
      if (pb(s, HvFfSync1 + v)) d = hallTicksSeconds(pv(s, HvFfTicks1 + v, 192), ctx.tempo);
      ffD_[v] = clampd(d * sr, 1.0, 2.2 * sr - fbW_ - 8);
    }
    ffW_ = fbW_;
    // Envelope.
    envRoute_ = std::clamp(pi(s, HvEnvRoute, 0), 0, 2);
    envScale_ = envRoute_ == HeOff ? 0.0 : clampd(pv(s, HvEnvScale, 0), -1, 1);
    envOffset_ = clampd(pv(s, HvEnvOffset, 0), -0.5, 0.5);
    envThr_ = clampd(pv(s, HvEnvThresh, -40), -60, 0);
    envAtk_ = std::exp(-2.2 / (std::max(1.0f, pv(s, HvEnvAttack, 50)) * 0.001 * sr));
    envRel_ = std::exp(-2.2 / (std::max(1.0f, pv(s, HvEnvRelease, 400)) * 0.001 * sr));
    envSmooth_ = 1.0 - std::exp(-1.4344 / (std::max(1.0f, pv(s, HvEnvSmooth, 1)) * 0.001 * sr));
    envSc_ = pb(s, HvEnvSidechain);
    envHp_.set(dsp::Biquad::HighPass, sr, pv(s, HvEnvLowCut, 60), 0.75, 0);
    // Output.
    eq_[0].set(dsp::Biquad::Peak, sr, pv(s, HvEqFreq, 3000), pv(s, HvEqQ, 0.8f), pv(s, HvEqGain, 0));
    eq_[1].b0 = eq_[0].b0;
    eq_[1].b1 = eq_[0].b1;
    eq_[1].b2 = eq_[0].b2;
    eq_[1].a1 = eq_[0].a1;
    eq_[1].a2 = eq_[0].a2;
    width_ = clampd(pv(s, HvWidth, 1.25f), 0, 2);
    limiter_ = pb(s, HvLimiter);
    limRel_ = std::exp(-1.0 / (0.05 * sr));
  }
  // dB level (FL: shape(−60, 15, 2)) → gain, clamped to 0..5.6.
  static double levelGain(double db) { return clampd(std::pow(10.0, db / 20.0), 0.0, 5.6); }

  Ring line_[kN], pre_[2];
  Allpass diff_[16];
  PitchVoice shim_, ff_[4];
  TptOnePole inLp_[2], inHp_[2];
  dsp::Biquad eq_[2], fbHp_, fbLp_, envHp_;
  Smooth sIn_, sDry_, sWet_, sGate_, sDecEn_, sDecay_;
  double len_[kN] = {}, sb0_[kN] = {}, sb1_[kN] = {}, sa1_[kN] = {}, shX_[kN] = {}, shY_[kN] = {}, apD_[16] = {};
  double sr_ = 0, inGain_ = 1, dry_ = 1, wet_ = 0.35, decay_ = 2.6, bright_ = 0.85, preD_ = 0, outK_ = 1, rc_ = 1, rs_ = 0, lfoInc_ = 0,
         modRel_ = 0, diff_g_ = 1, shelfA_ = 7, freezeIn_ = 1, decayOn_ = 1;
  double fbRatio_ = 2, fbW_ = 2400, fbD_ = 4800, fbGain_ = 0, fbMix_ = 1, ffDirect_ = 0, ffAmp_[2] = {}, ffRatio_[2] = {1, 1}, ffD_[2] = {},
         ffW_ = 2400;
  double envScale_ = 0, envOffset_ = 0, envThr_ = -40, envAtk_ = 0.9, envRel_ = 0.99, envSmooth_ = 1, width_ = 1.25, limRel_ = 0.999;
  double lfoPh_ = 0, tankL_ = 0, tankR_ = 0, envPk_ = 0, envSm_ = 0, limLvl_ = 0, limG_ = 1, lastEnvDb_ = -120;
  int rr_ = 0, envRoute_ = 0;
  bool hq_ = true, fbOn_ = false, ffOn_ = false, limiter_ = false, envSc_ = false, primed_ = false, tankLive_ = true, shimLive_ = true;
};

}  // namespace gl::packs::spacefx::detail
