// Gridline — 0.5 parity effects: Distortion, Bitcrusher, Stereo Shaper,
// Utility, Shaper (drawn envelopes + filter), Low Lifter (Time Gate moved to
// EffectsK3.cpp). Audio thread only; no allocation in process().
#include <array>
#include <functional>
#include <cmath>
#include <memory>
#include <string>
#include "Bitcrush.h"
#include "LowLifterRef.h"
#include "../core/PluginParams.h"
#include "Dsp.h"
#include "Effects.h"

namespace gl {

using namespace dsp;

namespace {

float param(const PluginState& s, int i, float dflt = 0.f) {
  return i >= 0 && i < static_cast<int>(s.params.size()) ? s.params[static_cast<size_t>(i)] : dflt;
}
int iparam(const PluginState& s, int i, int dflt = 0) { return static_cast<int>(std::lround(param(s, i, static_cast<float>(dflt)))); }
bool bparam(const PluginState& s, int i, bool dflt = false) { return param(s, i, dflt ? 1.f : 0.f) >= 0.5f; }
inline double coef(double seconds, double sr) { return std::exp(-1.0 / std::max(1.0, seconds * sr)); }

struct SyncClock {  // song position in beats at sample i of the block
  static double beat(const ProcessContext& ctx, int i) {
    double spb = 60.0 / std::max(1.0, ctx.tempo) * ctx.sampleRate;
    return ctx.tick / std::max(1, ctx.ppq) + i / spb;
  }
};

// ======================================================================= Distortion
class DistFx final : public Effect {
 public:
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    using namespace pp;
    const double sr = ctx.sampleRate;
    int mode = std::clamp(iparam(s, DstMode), 0, 7);
    double driveDb = param(s, DstDrive, 12) + (bparam(s, DstExtreme) ? 40.0 : 0.0);
    double drive = dbToGain(driveDb), mix = param(s, DstMix, 1), out = dbToGain(param(s, DstOutput, -6));
    double color = param(s, DstColor), thr = dbToGain(param(s, DstThreshold));
    int os = stages(s);
    bool dc = bparam(s, DstDcRemove), softOut = bparam(s, DstSoftOut);
    double preAmt = param(s, DstPreAmount), preHz = param(s, DstPreBand, 1000);
    if (sr != sr_ || preHz != preHz_ || preAmt != preAmt_ || param(s, DstTone, 8000) != tone_) {
      sr_ = sr, preHz_ = preHz, preAmt_ = preAmt, tone_ = param(s, DstTone, 8000);
      for (int c = 0; c < 2; ++c) {
        pre_[c].set(Biquad::Peak, sr, preHz, 0.9, preAmt);
        post_[c].set(Biquad::Peak, sr, preHz, 0.9, -preAmt);
        dcb_[c].setFreq(sr, 5.0);
      }
      tl_.setFreq(sr, tone_);
      tr_.a = tl_.a;
    }
    double bias = color * 0.3;
    auto shape = [mode, thr, bias, color](double x) {
      x += bias;
      double y;
      switch (mode) {
        case 1: y = std::clamp(x, -thr, thr); break;
        case 2: y = thr * std::sin(x / thr); break;  // wavefold
        case 3: y = x >= 0 ? 1 - std::exp(-x) : -(1 - std::exp(x * 1.4)) / 1.2; break;  // asymmetric "tube"
        case 4: y = thr * std::tanh(x / thr); break;                                       // fast A: soft at threshold
        case 5: {                                                                          // fast B: hard fold at threshold
          double a = std::fabs(x);
          double f = std::fmod(a, 4 * thr);
          double v = f < thr ? f : f < 3 * thr ? 2 * thr - f : f - 4 * thr;
          y = x < 0 ? -v : v;
          break;
        }
        case 6: y = x >= 0 ? std::tanh(x * 1.2) : std::tanh(x * 0.6) * 0.8; break;  // diode-like overdrive
        case 7: y = std::fabs(x) * (0.5 + 0.5 * std::clamp(color, -1.0, 1.0)) + x * (0.5 - 0.5 * std::clamp(color, -1.0, 1.0)); break;
        default: y = std::tanh(x); break;
      }
      return y - (mode == 7 ? 0.0 : std::tanh(bias));
    };
    for (int i = 0; i < n; ++i) {
      float* ch[2] = {L, R};
      double y[2];
      for (int c = 0; c < 2; ++c) {
        double x = ch[c][i];
        if (preAmt > 0.01) x = pre_[c].process(x);
        x *= drive;
        double v = os ? ovs_[c].run(x, os, shape) : shape(x);
        if (preAmt > 0.01) v = post_[c].process(v);
        if (dc || mode == 7 || std::fabs(color) > 0.001) v = dcb_[c].process(v);
        y[c] = v;
      }
      double tlv = tl_.process(y[0]), trv = tr_.process(y[1]);
      int lat = Oversampler::latency(os);
      dryL_.push(L[i]), dryR_.push(R[i]);
      double dl = lat ? dryL_.tap(lat) : L[i], dr = lat ? dryR_.tap(lat) : R[i];
      double ol = (dl * (1 - mix) + tlv * mix) * out, orr = (dr * (1 - mix) + trv * mix) * out;
      if (softOut) ol = std::tanh(ol), orr = std::tanh(orr);
      L[i] = static_cast<float>(ol);
      R[i] = static_cast<float>(orr);
    }
  }
  void reset() override {
    tl_.z = tr_.z = 0;
    for (int c = 0; c < 2; ++c) pre_[c].clear(), post_[c].clear(), dcb_[c].reset(), ovs_[c].reset();
    dryL_.clear(), dryR_.clear();
  }
  // Oversampling delays the wet path; the dry path is delayed to match.
  int latencyFor(const PluginState& s, double) const override { return Oversampler::latency(stages(s)); }

 private:
  static int stages(const PluginState& s) { return std::clamp(iparam(s, pp::DstOversample), 0, 3); }
  OnePole tl_, tr_;
  Biquad pre_[2], post_[2];
  DcBlock dcb_[2];
  Oversampler ovs_[2];
  FixedDelay<64> dryL_, dryR_;
  double sr_ = 0, preHz_ = -1, preAmt_ = -1, tone_ = -1;
};

// ======================================================================= Bitcrusher
// J-104: Rate (Hz) is the main control: a phase-accumulated sample-and-hold at any
// rate, with the input read at the exact hold instant; anti-alias / smoothing
// converter filters, fractional bits with TPDF dither (core in Bitcrush.h).
class BitcrushFx final : public Effect {
 public:
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    using namespace pp;
    const double sr = ctx.sampleRate;
    const double bits = std::clamp(static_cast<double>(param(s, BcBits, 8)), 1.0, 24.0), mix = param(s, BcMix, 1);
    const double P = crush::periodFor(param(s, BcRate, 11025), sr, kBcRateOff), logP = std::log2(P);
    bool filt = bparam(s, BcFilterOn);
    int ftype = iparam(s, BcFilterType), fpos = iparam(s, BcFilterPos, 1);
    double out = dbToGain(param(s, BcOutput));
    double punch = param(s, BcPunch), preserve = param(s, BcPreserve, 0.001f), impact = param(s, BcImpact, 0.01f);
    const double sq = param(s, BcSquarize), aa = param(s, BcAntiAlias), sm = param(s, BcSmooth), dither = param(s, BcDither);
    bool punchOn = punch > 0.001 || preserve > 0.0015;
    if (filt)
      for (auto& f : svf_) f.set(sr, param(s, BcFilterFreq, 2000), param(s, BcFilterReso, 0.3f));
    auto filter = [&](int c, double x) {
      double band = 0, high = 0, low = svf_[c].process(x, &band, &high);
      switch (ftype) {
        case 1: return high;
        case 2: return band;
        case 3: return low + high;
        default: return low;
      }
    };
    if (!primed_ || sr != sr_) primed_ = true, sr_ = sr, logP_ = logP, bits_ = bits, period_ = P, steps_ = std::exp2(bits - 1);
    // Rate and Bits glide (about 8 ms) so automation sweeps are smooth; settled values are exact.
    const double glide = std::min(1.0, (1.0 - coef(0.008, sr)) * 16);
    double fa = coef(0.0005, sr), fr = coef(0.02, sr), sa = coef(0.02, sr), sr2 = coef(0.3, sr);
    for (int i = 0; i < n; ++i) {
      if ((tick_++ & 15) == 0) {  // every 16 samples on a grid that runs across blocks (block-size invariant)
        if (logP_ != logP) {
          logP_ += (logP - logP_) * glide;
          if (std::fabs(logP - logP_) < 1e-6) logP_ = logP;
          period_ = logP_ == logP ? P : std::exp2(logP_);
        }
        if (bits_ != bits) {
          bits_ += (bits - bits_) * glide;
          if (std::fabs(bits - bits_) < 1e-6) bits_ = bits;
          steps_ = std::exp2(bits_ - 1);
        }
        core_.setFilters(sr, period_, aa, sm);
      }
      double in[2] = {L[i], R[i]};
      double x[2] = {in[0], in[1]};
      if (filt && fpos == 0) x[0] = filter(0, x[0]), x[1] = filter(1, x[1]);
      // Onset detection for the puncher.
      double lift = 1.0, keepDry = 0.0;
      if (punchOn) {
        double a = std::max(std::fabs(in[0]), std::fabs(in[1]));
        fast_ = a > fast_ ? a + (fast_ - a) * fa : a + (fast_ - a) * fr;
        slow_ = a > slow_ ? a + (slow_ - a) * sa : a + (slow_ - a) * sr2;
        if (fast_ > slow_ * 1.8 && fast_ > 0.01 && since_ > 0.05 * sr) since_ = 0;
        double t = since_ / sr;
        since_ += 1;
        if (t < preserve) keepDry = 1.0 - t / preserve;
        if (t < impact) lift = 1.0 + punch * 3.0 * (1.0 - t / impact);
      }
      double y[2] = {x[0], x[1]};
      core_.frame(y[0], y[1], period_, steps_, sq, dither);
      for (int c = 0; c < 2; ++c) {
        double v = y[c] * (1 - keepDry) + x[c] * keepDry;
        if (filt && fpos == 1) v = filter(c, v);
        v *= lift;
        (c ? R : L)[i] = static_cast<float>((in[c] * (1 - mix) + v * mix) * out);
      }
    }
  }
  void reset() override {
    core_.reset();
    fast_ = slow_ = 0, since_ = 1e9, primed_ = false, tick_ = 0;
    for (auto& f : svf_) f.reset();
  }

 private:
  crush::Crusher core_;
  uint64_t tick_ = 0;
  double fast_ = 0, slow_ = 0, since_ = 1e9, sr_ = 0, logP_ = 0, bits_ = 8, period_ = 1, steps_ = 128;
  bool primed_ = false;
  Svf svf_[2];
};

// ======================================================================= Stereo Shaper
class StereoFx final : public Effect {
 public:
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    using namespace pp;
    const double sr = ctx.sampleRate;
    if (bparam(s, StClassic, true)) {
      classic(s, sr, L, R, n);
      return;
    }
    double width = param(s, StWidth, 1) * (1.0 - param(s, StSep)), mid = dbToGain(param(s, StMid)), side = dbToGain(param(s, StSide));
    double haas = param(s, StHaas) * 0.001 * sr;
    double pl, pr;
    panGains(param(s, StPan), pl, pr);
    double ll = param(s, StLL, 1), rr = param(s, StRR, 1), rl = param(s, StRL), lr = param(s, StLR);
    int inv = iparam(s, StInvert);
    double deph = param(s, StDephase);
    bool fxAfter = iparam(s, StFxPos, 1) == 1;
    double monoHz = param(s, StMonoBelow, 10), vol = dbToGain(param(s, StVolume));
    bool mono = monoHz > 10.5;
    if (sr != sr_ || deph != deph_ || monoHz != mono_) {
      sr_ = sr, deph_ = deph, mono_ = monoHz;
      double f = 20.0 * std::pow(1000.0, std::fabs(deph));
      for (auto& a : ap_) a.setFreq(sr, f);
      xo_lp_[0].set(Biquad::HighPass, sr, monoHz, 0.7071, 0), xo_lp_[1] = xo_lp_[0];
    }
    double sl = inv == 1 || inv == 3 ? -1 : 1, sR = inv == 2 || inv == 3 ? -1 : 1;
    for (int i = 0; i < n; ++i) {
      double l = L[i], r = R[i];
      auto effects = [&](double& a, double& b) {
        dl_.push(static_cast<float>(a));
        dr_.push(static_cast<float>(b));
        if (haas > 0.5) b = dr_.tapFrac(haas);
        if (haas < -0.5) a = dl_.tapFrac(-haas);
        if (deph < -0.001) a = ap_[1].process(ap_[0].process(a));
        if (deph > 0.001) b = ap_[1].process(ap_[0].process(b));
      };
      if (!fxAfter) effects(l, r);
      double ml = ll * l + rl * r, mr = rr * r + lr * l;
      l = ml * sl, r = mr * sR;
      double m = (l + r) * 0.5 * mid, sd = (l - r) * 0.5 * side * width;
      if (mono) {  // remove the side signal below the cutoff (LR4 low part)
        sd = xo_lp_[1].process(xo_lp_[0].process(sd));  // LR4 high part of the side
      }
      l = m + sd, r = m - sd;
      if (fxAfter) effects(l, r);
      L[i] = static_cast<float>(l * pl * vol);
      R[i] = static_cast<float>(r * pr * vol);
    }
  }
  void reset() override {
    dl_.clear(), dr_.clear();
    for (auto& a : ap_) a.reset();
    xo_lp_[0].clear(), xo_lp_[1].clear();
    std::fill(bigL_.begin(), bigL_.end(), 0.f), std::fill(bigR_.begin(), bigR_.end(), 0.f);
  }

 private:
  // Reference engine: separation matrix LL = RR = (1 - s/2) g, cross = LL s; pan scales one side;
  // invert flips one output; phase offset = delay of one side (before or after the matrix).
  void classic(const PluginState& s, double sr, float* L, float* R, int n) {
    using namespace pp;
    if (bigL_.size() < static_cast<size_t>(sr * 0.52) + 4) return;  // sized at construction for 192 kHz
    const double sep = 0.75 * std::clamp(static_cast<double>(param(s, StSep)), -1.0, 1.0);
    const double x = param(s, StVolK, 1), g = x == 1.0 ? 1.0 : 0.1 * (std::pow(11.0, x) - 1.0);
    const double p = std::clamp(static_cast<double>(param(s, StPan)), -1.0, 1.0);
    double LL = (1 - sep * 0.5) * g, RR = LL, RL = LL * sep, LR = RL;
    if (p < 0) RR *= 1 + p, LR *= 1 + p;
    else LL *= 1 - p, RL *= 1 - p;
    const int inv = iparam(s, StInvert);
    if (inv == 1 || inv == 3) LL = -LL, RL = -RL;
    if (inv == 2 || inv == 3) RR = -RR, LR = -LR;
    const double pk = param(s, StPhaseK);
    const double ms = (std::pow(1001.0, std::fabs(pk)) - 1.0) * 0.5;
    const size_t N = bigL_.size();
    const size_t d = std::min(N - 2, static_cast<size_t>(std::lround(ms * 0.001 * sr)));
    const bool post = iparam(s, StFxPos, 1) == 1;
    for (int i = 0; i < n; ++i) {
      double l = L[i], r = R[i];
      auto delay = [&](double& a, double& b) {
        bigL_[bw_] = static_cast<float>(a), bigR_[bw_] = static_cast<float>(b);
        if (d > 0) {
          size_t rd = (bw_ + N - d) % N;
          if (pk < 0) a = bigL_[rd];
          else b = bigR_[rd];
        }
        bw_ = (bw_ + 1) % N;
      };
      if (!post) delay(l, r);
      double ol = LL * l + RL * r, orr = RR * r + LR * l;
      if (post) delay(ol, orr);
      L[i] = static_cast<float>(ol);
      R[i] = static_cast<float>(orr);
    }
  }
  std::vector<float> bigL_ = std::vector<float>(static_cast<size_t>(192000 * 0.52) + 8, 0.f), bigR_ = bigL_;
  size_t bw_ = 0;
  double sr_ = 0, deph_ = -9, mono_ = -1;
  FixedDelay<8192> dl_, dr_;
  Allpass1 ap_[2];
  Biquad xo_lp_[2];
};

// ======================================================================= Utility
class UtilityFx final : public Effect {
 public:
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    using namespace pp;
    double g = dbToGain(param(s, UtlGain)), width = bparam(s, UtlMono) ? 0.0 : param(s, UtlWidth, 1);
    double pan = param(s, UtlPan), pl, pr;
    const int law = iparam(s, UtlPanLaw);
    // Folded balance (reference): power-compensated sine / cosine gains; the attenuated side is folded into the other.
    double fLL = 1, fRR = 1, fRL = 0, fLR = 0;
    if (law == 2) {
      double x = (pan + 1) * 0.5, comp = 1.0 - 0.5 * std::sin(std::fabs(x - 0.5) * kPi), vol = std::sqrt(2.0) * comp, th = (pan + 1) * kPi * 0.25;
      fLL = vol * std::cos(th), fRR = vol * std::sin(th);
      if (x < 0.5) fRL = fLL - fRR;
      else fLR = fRR - fLL;
      if (std::fabs(pan) < 1e-9) fLL = fRR = 1, fRL = fLR = 0;
    }
    if (law == 1) {  // constant power
      double a = (pan + 1) * 0.25 * kPi;
      pl = std::cos(a) * std::sqrt(2.0), pr = std::sin(a) * std::sqrt(2.0);
    } else {
      panGains(pan, pl, pr);
    }
    bool inv = bparam(s, UtlInvert), swap = bparam(s, UtlSwap);
    int invMode = iparam(s, UtlInvertMode);
    double il = inv && invMode != 2 ? -1 : 1, ir = inv && invMode != 1 ? -1 : 1;
    bool mute = bparam(s, UtlMute);
    int mch = iparam(s, UtlMuteChannel);
    double tml = mute && mch != 2 ? 0 : 1, tmr = mute && mch != 1 ? 0 : 1;
    double ramp = 1.0 / std::max(1.0, 0.005 * ctx.sampleRate);
    bool dc = bparam(s, UtlDcRemove);
    if (dc && ctx.sampleRate != sr_) {
      sr_ = ctx.sampleRate;
      dcb_[0].setFreq(sr_), dcb_[1].setFreq(sr_);
    }
    for (int i = 0; i < n; ++i) {
      double l = L[i], r = R[i];
      if (dc) l = dcb_[0].process(l), r = dcb_[1].process(r);
      if (swap) std::swap(l, r);
      double m = (l + r) * 0.5, sd = (l - r) * 0.5 * width;
      ml_ += std::clamp(tml - ml_, -ramp, ramp);
      mr_ += std::clamp(tmr - mr_, -ramp, ramp);
      double ol = m + sd, orr = m - sd;
      if (law == 2) {
        double a = fLL * ol + fRL * orr, b = fRR * orr + fLR * ol;
        ol = a, orr = b, pl = pr = 1;
      }
      L[i] = static_cast<float>(ol * g * pl * il * ml_);
      R[i] = static_cast<float>(orr * g * pr * ir * mr_);
      pkL_ = std::max(pkL_ * 0.99995f, std::fabs(L[i]));
      pkR_ = std::max(pkR_ * 0.99995f, std::fabs(R[i]));
    }
  }
  int meters(float* out, int max) const override {  // output peaks (dBFS) for the editor's meter
    if (max < 2) return 0;
    out[0] = static_cast<float>(gainToDb(pkL_ + 1e-9f));
    out[1] = static_cast<float>(gainToDb(pkR_ + 1e-9f));
    return 2;
  }
  void reset() override {
    dcb_[0].reset(), dcb_[1].reset();
    ml_ = mr_ = 1;
  }

 private:
  DcBlock dcb_[2];
  double sr_ = 0, ml_ = 1, mr_ = 1;
  float pkL_ = 0, pkR_ = 0;
};

// ======================================================================= Shaper
inline double lfoShape(int shape, double ph) {
  switch (shape) {
    case 1: return ph < 0.5 ? 4 * ph - 1 : 3 - 4 * ph;
    case 2: return 2 * ph - 1;
    case 3: return ph < 0.5 ? 1 : -1;
    default: return std::sin(kTwoPi * ph);
  }
}

class ShaperFx final : public Effect {
 public:
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    using namespace pp;
    const double sr = ctx.sampleRate;
    static const double beats[] = {1, 2, 4, 8, 16};
    static const double lfoBeats[] = {0.25, 0.5, 1, 2, 4, 8, 16};
    double len = beats[std::clamp(iparam(s, ShLength, 2), 0, 4)];
    int filt = std::clamp(iparam(s, ShFilter), 0, 8);
    double cmin = param(s, ShCutMin, 200), cmax = param(s, ShCutMax, 12000), reso = param(s, ShReso, 0.3f);
    double sm = coef(param(s, ShSmooth, 4) * 0.001, sr), mix = param(s, ShMix, 1);
    double tension = param(s, ShTension), depth = param(s, ShCutDepth, 1);
    double drive = dbToGain(param(s, ShDrive)), driveMix = param(s, ShDriveMix);
    int stages = std::clamp(iparam(s, ShSlope) + 1, 1, 3);
    double shelf = param(s, ShShelfGain);
    double follow = param(s, ShFollow), fAtt = coef(param(s, ShFollowAtt, 0.01f), sr), fRel = coef(param(s, ShFollowRel, 0.15f), sr);
    double lfoAmt = param(s, ShLfoAmt), lfoLen = lfoBeats[std::clamp(iparam(s, ShLfoRate, 2), 0, 6)];
    int lfoSh = iparam(s, ShLfoShape);
    double inG = dbToGain(param(s, ShInput)), outG = dbToGain(param(s, ShOutput));
    double spb = 60.0 / std::max(1.0, ctx.tempo) * sr;
    for (int i = 0; i < n; ++i) {
      double beat;
      if (ctx.playing) {
        beat = SyncClock::beat(ctx, i);
      } else {
        free_ += 1.0 / spb;
        if (free_ > 1e6) free_ = 0;
        beat = free_;
      }
      double ph = std::fmod(beat, len) / len;
      double f = ph * kShaperPoints;
      int k = std::min(kShaperPoints - 1, static_cast<int>(f));
      auto lane = [&](int base, float dflt) {
        double a = param(s, base + k, dflt);
        if (std::fabs(tension) < 1e-4) return a;  // steps
        double b = param(s, base + (k + 1) % kShaperPoints, dflt);
        double fr = f - k, kk = std::pow(2.0, -tension * 3.0);
        return a + (b - a) * std::pow(fr, kk);
      };
      double vol = lane(ShVol, 1), cut = lane(ShCut, 1), pan = lane(ShPan, 0);
      vol_ = vol + (vol_ - vol) * sm;
      cut_ = cut + (cut_ - cut) * sm;
      pan_ = pan + (pan_ - pan) * sm;
      double l = L[i] * inG, r = R[i] * inG;
      double a = std::max(std::fabs(l), std::fabs(r));
      fol_ = a > fol_ ? a + (fol_ - a) * fAtt : a + (fol_ - a) * fRel;
      if (filt > 0) {
        if ((ctr_++ & 7) == 0) {
          double lfo = lfoAmt != 0 ? lfoShape(lfoSh, std::fmod(beat, lfoLen) / lfoLen) * lfoAmt * 0.5 : 0;
          double env = depth >= 0 ? 1.0 - depth * (1.0 - cut_) : 1.0 + depth * cut_;
          double norm = std::clamp(env + follow * std::min(1.0, fol_ * 2) + lfo, 0.0, 1.0);
          double fc = cmin * std::pow(cmax / std::max(1.0, cmin), norm);
          for (auto& st : svf_)
            for (auto& sv : st) sv.set(sr, fc, reso);
          if (filt >= 5) {
            Biquad::Type t = filt == 5 ? Biquad::LowShelf : filt == 6 ? Biquad::Peak : Biquad::HighShelf;
            for (auto& st : bq_)
              for (auto& b : st) {
                double x1 = b.x1, x2 = b.x2, y1 = b.y1, y2 = b.y2;
                if (filt == 8) {  // all-pass (RBJ)
                  double qq = 0.5 + reso * 4, w = kTwoPi * std::min(fc, sr * 0.45) / sr, al = std::sin(w) / (2 * qq), a0 = 1 + al;
                  b.b0 = (1 - al) / a0, b.b1 = -2 * std::cos(w) / a0, b.b2 = 1.0, b.a1 = b.b1, b.a2 = b.b0;
                } else {
                  b.set(t, sr, fc, 0.5 + reso * 4, shelf / stages);
                }
                b.x1 = x1, b.x2 = x2, b.y1 = y1, b.y2 = y2;
              }
          }
        }
        for (int c = 0; c < 2; ++c) {
          double v = c ? r : l;
          for (int st = 0; st < stages; ++st) {
            if (filt >= 5) {
              v = bq_[st][c].process(v);
            } else {
              double band = 0, high = 0, low = svf_[st][c].process(v, &band, &high);
              v = filt == 1 ? low : filt == 2 ? band : filt == 3 ? high : low + high;
            }
          }
          (c ? r : l) = v;
        }
      }
      if (driveMix > 0) {
        double dl = std::tanh(l * drive), dr = std::tanh(r * drive);
        l = l * (1 - driveMix) + dl * driveMix;
        r = r * (1 - driveMix) + dr * driveMix;
      }
      double pl, pr;
      panGains(pan_, pl, pr);
      l *= vol_ * pl * outG, r *= vol_ * pr * outG;
      L[i] = static_cast<float>(L[i] * (1 - mix) + l * mix);
      R[i] = static_cast<float>(R[i] * (1 - mix) + r * mix);
    }
  }
  void reset() override {
    free_ = 0, vol_ = cut_ = 1, pan_ = 0, fol_ = 0;
    for (auto& st : svf_)
      for (auto& sv : st) sv.reset();
    for (auto& st : bq_)
      for (auto& b : st) b.clear();
  }

 private:
  double free_ = 0, vol_ = 1, cut_ = 1, pan_ = 0, fol_ = 0;
  Svf svf_[3][2];
  Biquad bq_[3][2];
  uint32_t ctr_ = 0;
};

// ======================================================================= Low Lifter
llref::Settings lowLiftRefSettings(const PluginState& s) {
  using namespace pp;
  llref::Settings r;
  r.on = bparam(s, LlOn, true), r.lookahead = bparam(s, LlLookahead, true), r.invert = bparam(s, LlInvertHarm);
  r.multiband = bparam(s, LlMultiband), r.highOn = bparam(s, LlHighOn, true), r.lowOn = bparam(s, LlLowOn, true);
  r.mix = param(s, LlMix, 1), r.dmix = param(s, LlDiffuse), r.dcoef = param(s, LlDiffCoef);
  const double lo = param(s, LlLowDb), hi = param(s, LlHighDb);
  r.lowDb = lo <= -119.999 ? -1e9 : lo, r.highDb = hi <= -119.999 ? -1e9 : hi;
  r.slopeDb = param(s, LlSlopeDb, -6.5f), r.preDb = param(s, LlPreDb, 9), r.xoverHz = param(s, LlXoverHz, 99.5f), r.lowCutHz = param(s, LlLowCutHz, 20);
  return r;
}

class LowLiftFx final : public Effect {
 public:
  LowLiftFx() : ref_(std::make_unique<llref::Engine>()) {}  // fixed buffers, sized once (≈ 1.6 MB)
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    using namespace pp;
    const double sr = ctx.sampleRate;
    if (iparam(s, LlModel) == 1) {  // J-26 reference engine
      ref_->process(lowLiftRefSettings(s), sr, L, R, n);
      return;
    }
    double xo = param(s, LlCrossover, 120), lc = param(s, LlLowCut, 25);
    double slope = param(s, LlSlope), bf = param(s, LlBoostFreq, 60), boost = param(s, LlBoost), dAmt = param(s, LlDiffuseAmt, 0.5f);
    if (xo != xo_ || lc != lc_ || sr != sr_ || slope != slope_ || bf != bf_ || boost != boost_ || dAmt != dAmt_) {
      xo_ = xo, lc_ = lc, sr_ = sr, slope_ = slope, bf_ = bf, boost_ = boost, dAmt_ = dAmt;
      for (int c = 0; c < 2; ++c) {
        for (int k = 0; k < 2; ++k) {
          lp_[c][k].set(Biquad::LowPass, sr, xo, 0.7071, 0);
          hp_[c][k].set(Biquad::HighPass, sr, xo, 0.7071, 0);
        }
        cut_[c].set(Biquad::HighPass, sr, lc, 0.7071, 0);
        shelf_[c].set(Biquad::LowShelf, sr, bf, 0.9, 18.0 * boost);
        harmLp_[c].setFreq(sr, std::min(sr * 0.45, xo * std::pow(2.0, 8.0 * (1.0 - slope))));
        for (int k = 0; k < 4; ++k) diff_[c][k].setFreq(sr, xo * (0.3 + 0.25 * k) * (0.2 + dAmt * 1.6));
      }
    }
    double lg = bparam(s, LlLowOn, true) ? dbToGain(param(s, LlLowGain, 3)) : 0.0;
    double hg = bparam(s, LlHighOn, true) ? dbToGain(param(s, LlHighGain)) : 0.0;
    double harm = param(s, LlHarmonics, 0.3f), mix = param(s, LlMix, 1);
    double pre = dbToGain(param(s, LlDrive));
    double sign = bparam(s, LlInvertHarm) ? -1.0 : 1.0;
    double diffuse = param(s, LlDiffuse);
    bool multiband = bparam(s, LlMultiband, true);
    for (int i = 0; i < n; ++i) {
      float* ch[2] = {L, R};
      for (int c = 0; c < 2; ++c) {
        double x = cut_[c].process(ch[c][i]);
        if (boost > 0.001) x = shelf_[c].process(x);
        double lo, hi;
        if (multiband) {
          lo = lp_[c][1].process(lp_[c][0].process(x));
          hi = hp_[c][1].process(hp_[c][0].process(x));
        } else {
          lo = x, hi = 0;
        }
        if (diffuse > 0) {
          double d = lo;
          for (auto& a : diff_[c]) d = a.process(d);
          lo = lo * (1 - diffuse) + d * diffuse;
        }
        double drv = (1 + harm * 6) * pre;
        double h = std::tanh(lo * drv) / (1 + harm * 2) - lo * drv / (1 + harm * 2) / std::max(1.0, pre);  // added harmonics only
        if (slope > 0.001) h = harmLp_[c].process(h);
        double y = (lo + sign * h * harm * 2) * lg + hi * hg;
        ch[c][i] = static_cast<float>(ch[c][i] * (1 - mix) + y * mix);
      }
    }
  }
  int latencyFor(const PluginState& s, double sr) const override {
    return iparam(s, pp::LlModel) == 1 ? llref::Engine::latency(lowLiftRefSettings(s), sr) : 0;
  }
  void reset() override {
    if (ref_) ref_->reset();
    for (auto* f : {&lp_, &hp_})
      for (auto& c : *f)
        for (auto& b : c) b.clear();
    for (int c = 0; c < 2; ++c) {
      cut_[c].clear(), shelf_[c].clear();
      harmLp_[c].z = 0;
      for (auto& a : diff_[c]) a.reset();
    }
  }

 private:
  std::unique_ptr<llref::Engine> ref_;
  Biquad lp_[2][2], hp_[2][2], cut_[2], shelf_[2];
  OnePole harmLp_[2];
  Allpass1 diff_[2][4];
  double xo_ = -1, lc_ = -1, sr_ = 0, slope_ = -1, bf_ = -1, boost_ = -1, dAmt_ = -1;
};

}  // namespace

std::unique_ptr<Effect> createEffectW4(const std::string& type) {
  if (type == "gl.dist") return std::make_unique<DistFx>();
  if (type == "gl.bitcrush") return std::make_unique<BitcrushFx>();
  if (type == "gl.stereo") return std::make_unique<StereoFx>();
  if (type == "gl.utility") return std::make_unique<UtilityFx>();
  if (type == "gl.shaper") return std::make_unique<ShaperFx>();
  if (type == "gl.lowlift") return std::make_unique<LowLiftFx>();
  return nullptr;
}

}  // namespace gl
