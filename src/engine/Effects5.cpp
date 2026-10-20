// Gridline — 0.5 parity effects: Filter, Delay, Reverb, Chorus, Flanger,
// Phaser, Auto Pan, Freq Shifter. Audio thread only; buffers are sized when
// the sample rate changes (never per block).
#include <array>
#include <cmath>
#include <memory>
#include <vector>
#include "../core/DelayCurves.h"
#include "../core/PluginParams.h"
#include "Dsp.h"
#include "Effects.h"
#include "ClassicDelay.h"
#include "ClassicModFx.h"
#include "FlFilterRef.h"
#include "FreqShiftRef.h"
#include "PlateReverb.h"

namespace gl {

using namespace dsp;

namespace {

float param(const PluginState& s, int i, float dflt = 0.f) {
  return i >= 0 && i < static_cast<int>(s.params.size()) ? s.params[static_cast<size_t>(i)] : dflt;
}
int iparam(const PluginState& s, int i, int dflt = 0) { return static_cast<int>(std::lround(param(s, i, static_cast<float>(dflt)))); }
bool bparam(const PluginState& s, int i, bool dflt = false) { return param(s, i, dflt ? 1.f : 0.f) >= 0.5f; }

// Circular delay line with fractional (linear) reads.
struct Line {
  std::vector<float> buf;
  size_t w = 0;
  void init(size_t len) {
    buf.assign(std::max<size_t>(len, 4), 0.f);
    w = 0;
  }
  void push(float x) {
    buf[w] = x;
    w = (w + 1) % buf.size();
  }
  float read(double d) const {
    double len = static_cast<double>(buf.size());
    d = std::clamp(d, 1.0, len - 2);
    double rp = static_cast<double>(w) - d;
    while (rp < 0) rp += len;
    size_t i0 = static_cast<size_t>(rp) % buf.size(), i1 = (i0 + 1) % buf.size();
    float fr = static_cast<float>(rp - std::floor(rp));
    return buf[i0] + (buf[i1] - buf[i0]) * fr;
  }
  void clear() { std::fill(buf.begin(), buf.end(), 0.f); }
};

// Schroeder all-pass on a fixed buffer with a variable length.
struct Diffuser {
  FixedDelay<8192> d;
  double process(double x, int len, double g) {
    double b = d.tap(len);
    double y = -g * x + b;
    d.push(static_cast<float>(x + g * y));
    return y;
  }
  void clear() { d.clear(); }
};

// Sine → triangle morph LFO value for phase ph.
inline double lfoMorph(double ph, double shape) {
  double tri = ph < 0.25 ? ph * 4 : ph < 0.75 ? 2 - ph * 4 : ph * 4 - 4;
  return std::sin(kTwoPi * ph) * (1 - shape) + tri * shape;
}

// ======================================================================= Filter
class FilterFx final : public Effect {
 public:
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    using namespace pp;
    const double sr = ctx.sampleRate;
    if (iparam(s, FltModel) == 1) {  // J-26 classic multi-tap model
      if (!classic_) ref_.clear(), classic_ = true;
      ref_.process(param(s, FltCutK, 1), param(s, FltResK, 0.2f), param(s, FltLPLevel, 1), param(s, FltBPLevel), param(s, FltHPLevel),
                   iparam(s, FltSlope) == 1, bparam(s, FltDcBlock), sr, L, R, n);
      return;
    }
    classic_ = false;
    int mode = std::clamp(iparam(s, FltMode), 0, 6);
    double mix = param(s, FltMix, 1);
    target_ = param(s, FltCutoff, 20000);
    double reso = param(s, FltReso);
    bool steep = iparam(s, FltSlope) == 1;
    bool blend = bparam(s, FltBlend);
    double lpl = param(s, FltLPLevel, 1), bpl = param(s, FltBPLevel), hpl = param(s, FltHPLevel);
    double gain = param(s, FltGain);
    bool dc = bparam(s, FltDcBlock);
    if (dc && sr != dcSr_) dcSr_ = sr, dc_[0].setFreq(sr), dc_[1].setFreq(sr);
    for (int j = 0; j < n; ++j) {
      if ((ctr_++ & 15) == 0) {  // coefficient update on the effect's own clock
        cur_ = cur_ <= 0 ? target_ : cur_ + (target_ - cur_) * 0.25;
        for (int c = 0; c < 2; ++c) {
          f_[c][0].set(sr, cur_, steep ? reso * 0.75 : reso);
          f_[c][1].set(sr, cur_, 0.0);
          if (mode >= 4) {
            Biquad::Type t = mode == 4 ? Biquad::LowShelf : mode == 5 ? Biquad::Peak : Biquad::HighShelf;
            for (int k = 0; k < 2; ++k) {
              Biquad& b = bq_[c][k];
              double x1 = b.x1, x2 = b.x2, y1 = b.y1, y2 = b.y2;
              b.set(t, sr, cur_, 0.5 + reso * 9.5, steep ? gain * 0.5 : gain);
              b.x1 = x1, b.x2 = x2, b.y1 = y1, b.y2 = y2;
            }
          }
        }
      }
      float* ch[2] = {L, R};
      for (int c = 0; c < 2; ++c) {
        double x = ch[c][j], y;
        if (mode >= 4 && !blend) {
          y = bq_[c][0].process(x);
          if (steep) y = bq_[c][1].process(y);
        } else {
          double band = 0, high = 0;
          double low = f_[c][0].process(x, &band, &high);
          if (steep) {
            double b2 = 0, h2 = 0, l2 = f_[c][1].process(blend ? x : (mode == 0 ? low : mode == 1 ? band : mode == 2 ? high : low + high), &b2, &h2);
            if (blend)
              y = lpl * low + bpl * band + hpl * high;  // 24 dB blend uses the first stage taps
            else
              y = mode == 0 ? l2 : mode == 1 ? b2 : mode == 2 ? h2 : l2 + h2;
          } else {
            y = blend ? lpl * low + bpl * band + hpl * high : mode == 0 ? low : mode == 1 ? band : mode == 2 ? high : low + high;
          }
        }
        if (dc) y = dc_[c].process(y);
        ch[c][j] = static_cast<float>(x + (y - x) * mix);
      }
    }
  }
  void reset() override {
    for (auto& c : f_)
      for (auto& f : c) f.reset();
    for (auto& c : bq_)
      for (auto& b : c) b.clear();
    dc_[0].reset(), dc_[1].reset();
    cur_ = 0;
    ref_.clear();
  }

 private:
  flref::MultiSvf ref_;
  bool classic_ = false;
  Svf f_[2][2];
  Biquad bq_[2][2];
  DcBlock dc_[2];
  double cur_ = 0, target_ = 20000, dcSr_ = 0;
  uint32_t ctr_ = 0;
};

// ======================================================================= Delay (reference model)
// Signal flow of the reference delay: (input × input level + feedback tap ×
// feedback) → DC blocker → rate / bit reduction → distortion (limit or
// saturate) → resonant feedback filter (LFO-modulated cutoff) → diffusion
// all-passes → line. Taps are read with the smoothed time (or two crossfading
// heads with "keep pitch") plus the triangle LFO's time modulation. Stereo
// types: fake stereo (mono line, two taps), true stereo (two lines), ping-pong
// (cross-fed lines, mono input into the left), off (dry only).
class DelayRef {
 public:
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) {
    using namespace pp;
    if (sr_ != ctx.sampleRate) init(ctx.sampleRate);
    const int type = std::clamp(iparam(s, DlyType3, 1), 0, 3);
    // Time (samples): synced ticks of 1/192 beat, or the free time.
    double T;
    if (bparam(s, DlySync, true)) {
      T = std::clamp(param(s, DlyTicks, 192), 1.0f, 768.0f) / 192.0 * 60.0 / std::max(ctx.tempo, 20.0) * sr_;
    } else {
      T = std::clamp(static_cast<double>(param(s, DlyTimeMs, 300)), 1.0, 3000.0) * 0.001 * sr_;
    }
    T = std::max(1.0, std::floor(T));
    // R/L offset: |x| moves one side. True stereo / ping-pong: + T·x²/4; fake stereo: + T·|x|/2 after the spread offset.
    const double off = std::clamp(param(s, DlyTimeR), -1.0f, 1.0f), mag = std::fabs(off);
    const double spread = param(s, DlySpread3, 1);
    double tA, tB;
    if (type == 0) {
      double sp = dly::spreadSamples(spread, sr_, T);
      double base = std::max(1.0, T - std::floor(sp));
      double extra = T * mag * 0.5;
      tA = base, tB = base + extra;
      if (off < 0) std::swap(tA, tB);
    } else {
      double extra = std::max(1.0, T * mag * mag * 0.25);
      tA = T, tB = T;
      if (mag > 0) (off >= 0 ? tB : tA) += extra;
    }
    tA = std::min(tA, maxDelay_), tB = std::min(tB, maxDelay_);
    const double fb = param(s, DlyFeedback, 0.625f);
    const double inLevel = param(s, DlyInput, 1), dry = param(s, DlyDry, 1), wet = param(s, DlyWet, 1);
    const bool keepPitch = bparam(s, DlyKeepPitch);
    const double smoothMs = param(s, DlyTimeSmooth, 500);
    const double glide = smoothMs > 0.01 ? 1.0 - std::pow(0.01, 1.0 / (smoothMs * 0.001 * sr_)) : 1.0;
    // Modulation: triangle LFO, rate curve, time depth n²·0.1 of the delay time, cutoff depth.
    const double rate = std::clamp(param(s, DlyModRate3, 1.14f), 0.05f, 25.0f);
    const double depth = dly::modDepthFrac(param(s, DlyModDepth)) * T;
    const double modCut = param(s, DlyModCutoff);
    // Feedback filter.
    const int fmode = std::clamp(iparam(s, DlyFilter3, 0), 0, 3);
    const double cutN = dly::cutoffNorm(fmode, param(s, DlyCutoff3, 22000));
    const double Q = dly::filterQ(fmode, param(s, DlyResonance));
    // Crush.
    const double bits = param(s, DlyBits, 16), rateHz = param(s, DlyRate3, 48000);
    const bool crushBits = bits < 15.999, crushRate = rateHz < sr_ * 0.999;
    const double steps = std::pow(2.0, bits - 1);
    const double hold = crushRate ? sr_ / std::max(10.0, rateHz) : 1.0;
    // Distortion.
    const int dist = iparam(s, DlyDistType);  // 0 off, 1 limit, 2 saturate
    const double thrN = std::clamp(param(s, DlyDistThresh) / -28.5f, 0.0f, 1.0f);
    const double T0 = dsp::dbToGain(dly::distThreshDb(thrN)), makeup = dly::distMakeup(thrN);
    const double hard = param(s, DlyDistHardness), sym = param(s, DlyDistSymmetry);
    // Diffusion (two all-passes per line; spread sets their lengths).
    const double diff = param(s, DlyDiffusion), dsp_ = param(s, DlyDiffSpread, 0.5f);
    const int dl1 = std::clamp(static_cast<int>((0.003 + 0.02 * dsp_) * sr_), 8, 8000), dl2 = std::clamp(static_cast<int>((0.005 + 0.035 * dsp_) * sr_), 8, 8000);
    // Output tone and width.
    const double tone = param(s, DlyTone);
    if (tone != tone_) {
      tone_ = tone;
      double hz = tone < 0 ? 270.0 * std::pow(22000.0 / 270.0, 1.0 + tone) : 20.0 * std::pow(4200.0 / 20.0, tone);
      for (auto& tf : toneF_) tf.setFreq(sr_, hz);
    }
    const double wA = type == 0 ? 1.0 : 0.5 + 0.5 * spread, wB = 1.0 - wA;
    if (curA_ < 0) curA_ = tA, curB_ = tB;
    const double limRel = std::exp(-1.0 / (0.05 * sr_));
    for (int i = 0; i < n; ++i) {
      const double inL = L[i], inR = R[i];
      if (type == 3) {  // off
        L[i] = static_cast<float>(inL * dry), R[i] = static_cast<float>(inR * dry);
        continue;
      }
      // LFO (triangle, 0..1)
      ph_ += rate / sr_;
      ph_ -= std::floor(ph_);
      const double tri = ph_ <= 0.5 ? 2 * ph_ : 2 - 2 * ph_;
      modS_ += (depth * tri - modS_) * modA_;
      if ((ctr_++ & 15) == 0 && fmode < 3) {
        double fc = dly::cutoffHz(fmode, std::clamp(cutN + (tri - 0.5) * modCut, 0.0, 1.0));
        for (auto& f : svf_) f.set(sr_, fc, 0), f.k = 1.0 / Q;
        for (auto& f : svf_) {
          f.a1 = 1.0 / (1.0 + f.g * (f.g + f.k));
          f.a2 = f.g * f.a1;
          f.a3 = f.g * f.a2;
        }
      }
      // Time: glide, or crossfading heads (keep pitch, 100 ms).
      if (keepPitch) {
        if ((std::fabs(tA - curA_) > 0.5 || std::fabs(tB - curB_) > 0.5) && xf_ <= 0) {
          oldA_ = curA_, oldB_ = curB_, curA_ = tA, curB_ = tB;
          xf_ = 1.0;
        }
      } else {
        curA_ += (tA - curA_) * glide;
        curB_ += (tB - curB_) * glide;
      }
      auto rd = [&](const Line& l, double d) { return l.read(d + modS_); };
      const Line& la = lines_[0];
      const Line& lb = type == 0 ? lines_[0] : lines_[1];
      double ya = rd(la, curA_), yb = rd(lb, curB_);
      if (xf_ > 0) {
        double w = xf_;
        ya = ya * (1 - w) + rd(la, oldA_) * w;
        yb = yb * (1 - w) + rd(lb, oldB_) * w;
        xf_ -= 1.0 / (0.1 * sr_);
      }
      auto chain = [&](int c, double x) {
        x = dcF_[c].process(x);
        if (crushRate) {
          holdPh_[c] += 1.0;
          if (holdPh_[c] >= hold) holdPh_[c] -= hold, held_[c] = x;
          x = held_[c];
        }
        if (crushBits) x = std::round(x * steps) / steps;
        if (dist == 1) {
          double g = T0 * makeup, ax = std::fabs(x);
          env_[c] = ax > env_[c] ? ax : ax + (env_[c] - ax) * limRel;
          if (env_[c] > g) x *= g / env_[c];
        } else if (dist == 2) {
          double top = T0 * (x >= 0 ? 1 + sym : 1 - sym);
          double ax = std::fabs(x), lin = hard * top, k = std::max(1e-11, (1 - hard) * top);
          double y = ax <= lin ? ax : (ax < lin + 2 * k ? ax - (ax - lin) * (ax - lin) / (4 * k) : lin + k);
          x = (x < 0 ? -y : y) * makeup;
        }
        if (fmode < 3) {
          double band = 0, high = 0, low = svf_[c].process(x, &band, &high);
          x = fmode == 0 ? low : fmode == 1 ? high : band * svf_[c].k;
        }
        if (diff > 0) {
          double g = 0.7 * diff;
          x = dif_[c][0].process(x, dl1 + c * 37, g);
          x = dif_[c][1].process(x, dl2 + c * 53, g);
        }
        return x;
      };
      const double mono = (inL + inR) * 0.5 * inLevel;
      switch (type) {
        case 0: {  // fake stereo: one line, the feedback taken from the longer side
          double fbTap = off < 0 ? ya : yb;
          lines_[0].push(static_cast<float>(chain(0, mono + fbTap * fb)));
          break;
        }
        case 1:  // true stereo
          lines_[0].push(static_cast<float>(chain(0, inL * inLevel + ya * fb)));
          lines_[1].push(static_cast<float>(chain(1, inR * inLevel + yb * fb)));
          break;
        default:  // ping-pong: mono input into the left line, lines cross-feed
          lines_[0].push(static_cast<float>(chain(0, mono + yb * fb)));
          lines_[1].push(static_cast<float>(chain(1, ya * fb)));
          break;
      }
      double oa = ya, ob = yb;
      if (std::fabs(tone) > 0.001) {
        double ta = toneF_[0].process(oa), tb = toneF_[1].process(ob);
        oa = tone < 0 ? ta : oa - ta;
        ob = tone < 0 ? tb : ob - tb;
      }
      L[i] = static_cast<float>(inL * dry + (oa * wA + ob * wB) * wet);
      R[i] = static_cast<float>(inR * dry + (ob * wA + oa * wB) * wet);
    }
  }
  void reset() {
    for (auto& l : lines_) l.clear();
    for (auto& f : svf_) f.reset();
    for (auto& c : dif_)
      for (auto& d : c) d.clear();
    for (int c = 0; c < 2; ++c) toneF_[c].z = 0, dcF_[c] = dsp::DcBlock{}, dcF_[c].setFreq(sr_ > 0 ? sr_ : 44100, 10.0), env_[c] = 0, held_[c] = 0, holdPh_[c] = 0;
    curA_ = curB_ = -1, xf_ = 0, modS_ = 0, ph_ = 0;
  }

 private:
  void init(double sr) {
    sr_ = sr;
    maxDelay_ = sr * 18.0;
    for (auto& l : lines_) l.init(static_cast<size_t>(maxDelay_ + sr * 0.2) + 8);
    modA_ = 1.0 - std::pow(0.01, 1.0 / (sr * 0.1));
    tone_ = 9;
    reset();
  }
  double sr_ = 0, maxDelay_ = 0, modA_ = 0, modS_ = 0, ph_ = 0, tone_ = 9;
  double curA_ = -1, curB_ = -1, oldA_ = 0, oldB_ = 0, xf_ = 0;
  double env_[2] = {0, 0}, held_[2] = {0, 0}, holdPh_[2] = {0, 0};
  Line lines_[2];
  Svf svf_[2];
  Diffuser dif_[2][2];
  OnePole toneF_[2];
  dsp::DcBlock dcF_[2];
  uint32_t ctr_ = 0;
};

// ======================================================================= Delay
// in × send (panned) → line(s); read with smoothed time (or two crossfading heads)
// + LFO; feedback path: filter → rate reduction → bits → distortion → diffusion.
class DelayFx final : public Effect {
 public:
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    using namespace pp;
    if (iparam(s, DlyModel) == 2) {  // J-26 classic tempo delay
      if (!cls_.ready() || clsSr_ != ctx.sampleRate) cls_.init(ctx.sampleRate), clsSr_ = ctx.sampleRate;
      cdelay::Settings cs;
      cs.ticks = std::clamp(param(s, DlyTicks, 144), 1.f, 768.f), cs.stepRound = bparam(s, DlyClsStepRound), cs.bpm = param(s, DlyClsBpm);
      cs.coef = param(s, DlyClsCoef, 1), cs.scaleSr = bparam(s, DlyClsScaleSr, true), cs.feedback = param(s, DlyClsFb, 0.5f);
      cs.mode = std::clamp(iparam(s, DlyClsMode), 0, 2), cs.offsetMs = param(s, DlyClsOffset), cs.dry = param(s, DlyClsDry, 1);
      const double g = param(s, DlyClsIn, 1), pan = std::clamp(param(s, DlyClsPan), -1.f, 1.f);
      if (pan >= 0) cs.inLL = (1 - pan) * g, cs.inLR = 0.5 * pan * g, cs.inRR = g - 0.5 * pan * g, cs.inRL = 0;
      else cs.inRR = (1 + pan) * g, cs.inRL = -0.5 * pan * g, cs.inLL = g + 0.5 * pan * g, cs.inLR = 0;
      cls_.process(cs, ctx.tempo, L, R, n);
      return;
    }
    if (bparam(s, DlyModel)) {
      ref_.process(s, ctx, L, R, n);
      return;
    }
    if (sr_ != ctx.sampleRate) {
      sr_ = ctx.sampleRate;
      size_t len = static_cast<size_t>(sr_ * 12.5) + 8;  // one bar at 20 BPM (+ offset)
      dl_.init(len);
      dr_.init(len);
      curL_ = curR_ = -1;
    }
    double t;
    if (bparam(s, DlySync, true)) {
      int ti = std::clamp(iparam(s, DlyTime), 0, kDelayBeatCount - 1);
      t = kDelayBeats[ti] * 60.0 / std::max(ctx.tempo, 20.0);
    } else {
      t = param(s, DlyTimeMs, 300) * 0.001;
    }
    double off = param(s, DlyOffset) * 0.001;
    double tL = std::max(1.0, (t - std::min(0.0, off)) * sr_), tR = std::max(1.0, (t + std::max(0.0, off)) * sr_);
    double fb = param(s, DlyFeedback, 0.4f);
    int mode = std::clamp(iparam(s, DlyMode, 1), 0, 4);
    int ftype = std::clamp(iparam(s, DlyFilterType), 0, 4);
    double cutoff = param(s, DlyCutoff, 7000), reso = param(s, DlyResonance, 0.3f);
    lpL_.setFreq(sr_, cutoff);
    lpR_.a = lpL_.a;
    hpL_.setFreq(sr_, param(s, DlyLowCut, 60));
    hpR_.a = hpL_.a;
    double modRate = param(s, DlyModRate, 0.5f), modDepth = param(s, DlyModDepth) * 0.003 * sr_, modCut = param(s, DlyModCutoff);
    double sat = param(s, DlySaturation), dry = param(s, DlyDry, 1), wet = param(s, DlyWet, 0.3f);
    double diff = param(s, DlyDiffusion), spread = param(s, DlyDiffSpread, 0.5f);
    int dlen1 = std::clamp(static_cast<int>((0.003 + 0.02 * spread) * sr_), 8, 8000), dlen2 = std::clamp(static_cast<int>((0.005 + 0.035 * spread) * sr_), 8, 8000);
    int distType = iparam(s, DlyDistType);
    double T = dbToGain(param(s, DlyDistThresh)), hard = param(s, DlyDistHardness), sym = param(s, DlyDistSymmetry);
    double makeup = 1.0 / std::pow(T, 0.7);
    double bits = param(s, DlyBits, 16), rateHz = param(s, DlyRateReduce, 48000);
    bool crushBits = bits < 15.99, crushRate = rateHz < 47999;
    double steps = std::pow(2.0, bits - 1);
    bool keepPitch = bparam(s, DlyKeepPitch);
    double smoothMs = param(s, DlyTimeSmooth, 20);
    double smooth = smoothMs > 0.5 ? 1.0 - std::pow(0.01, 1.0 / (smoothMs * 0.001 * sr_)) : 1.0;
    double stereo = param(s, DlyStereoSpread, 1), a = 0.5 + 0.5 * stereo;
    double tone = param(s, DlyTone);
    if (tone != tone_ || sr_ != toneSr_) {
      tone_ = tone, toneSr_ = sr_;
      double hz = tone < 0 ? 270.0 * std::pow(22000.0 / 270.0, 1.0 + tone) : 20.0 * std::pow(4200.0 / 20.0, tone);
      for (auto& tf : toneF_) tf.setFreq(sr_, hz);
    }
    double send = param(s, DlyInput, 1), ipl, ipr;
    panGains(param(s, DlyInputPan), ipl, ipr);
    double limRel = std::exp(-1.0 / (0.05 * sr_));
    if (curL_ < 0) curL_ = tL, curR_ = tR;
    for (int i = 0; i < n; ++i) {
      double lfo = std::sin(kTwoPi * ph_);
      double m = modDepth > 0 ? modDepth * (0.5 + 0.5 * lfo) : 0;
      ph_ += modRate / sr_;
      if (ph_ >= 1) ph_ -= 1;
      // Time changes: glide, or jump with a crossfade between two read heads.
      if (keepPitch) {
        if ((std::fabs(tL - curL_) > 1 || std::fabs(tR - curR_) > 1) && xf_ <= 0) {
          oldL_ = curL_, oldR_ = curR_, curL_ = tL, curR_ = tR;
          xf_ = 1.0;
        }
      } else {
        curL_ += (tL - curL_) * smooth;
        curR_ += (tR - curR_) * smooth;
      }
      double yl = dl_.read(curL_ + m), yr = dr_.read(curR_ + m * 0.9);
      if (xf_ > 0) {
        double w = std::sin(xf_ * kPi * 0.5);
        yl = yl * std::sqrt(1 - w * w) + dl_.read(oldL_ + m) * w;
        yr = yr * std::sqrt(1 - w * w) + dr_.read(oldR_ + m * 0.9) * w;
        xf_ -= 1.0 / (0.05 * sr_);
      }
      if (ftype >= 1 && ftype <= 3 && (ctr_++ & 15) == 0) {
        double fc = cutoff * std::pow(2.0, modCut * 2.0 * lfo);
        svf_[0].set(sr_, fc, reso), svf_[1].set(sr_, fc, reso);
      }
      auto shape = [&](int c, double x, OnePole& lp, OnePole& hp) {
        double v = x;
        switch (ftype) {
          case 0:
            v = lp.process(v);
            v -= hp.process(v);
            break;
          case 4: break;
          default: {
            double band = 0, high = 0, low = svf_[c].process(v, &band, &high);
            v = ftype == 1 ? low : ftype == 2 ? high : band;
          }
        }
        if (crushRate) {
          hold_[c] += rateHz / sr_;
          if (hold_[c] >= 1) hold_[c] -= std::floor(hold_[c]), held_[c] = v;
          v = held_[c];
        }
        if (crushBits) v = std::round(v * steps) / steps;
        if (distType == 1) {  // limit: level-dependent gain against the threshold
          double av = std::fabs(v);
          env_[c] = av > env_[c] ? av : av + (env_[c] - av) * limRel;
          if (env_[c] > T) v *= T / env_[c];
          v *= makeup;
        } else if (distType == 2) {  // saturate: linear to h·T, quadratic knee, flat above
          double top = T * (v >= 0 ? 1 + sym : 1 - sym);
          double av = std::fabs(v), lin = hard * top, k = (1 - hard) * top;
          double y = av <= lin ? av : (k > 1e-9 && av < lin + 2 * k ? av - (av - lin) * (av - lin) / (4 * k) : top);
          v = (v < 0 ? -y : y) * makeup;
        } else if (sat > 0) {
          v = (1 - sat) * v + sat * std::tanh(v * 1.5) / 1.5;
        }
        if (diff > 0) {
          double g = 0.7 * diff;
          v = dif_[c][0].process(v, dlen1 + c * 37, g);
          v = dif_[c][1].process(v, dlen2 + c * 53, g);
        }
        return v;
      };
      double inL = L[i], inR = R[i];
      double sL = inL * send * ipl, sR = inR * send * ipr;
      double wl, wr;
      if (mode == 4) {  // off: the network processes the input once
        wl = shape(0, sL, lpL_, hpL_), wr = shape(1, sR, lpR_, hpR_);
      } else {
        double fl = shape(0, yl, lpL_, hpL_) * fb, fr = shape(1, yr, lpR_, hpR_) * fb;
        switch (mode) {
          case 1:  // ping-pong: mono input into the left line, lines cross-feed
            dl_.push(static_cast<float>((sL + sR) * 0.5 + fr));
            dr_.push(static_cast<float>(fl));
            break;
          case 2:  // inverted feedback
            dl_.push(static_cast<float>(sL - fl));
            dr_.push(static_cast<float>(sR - fr));
            break;
          case 3: {  // mono: summed input, stereo only from the offset
            double mo = (sL + sR) * 0.5;
            dl_.push(static_cast<float>(mo + fl));
            dr_.push(static_cast<float>(mo + fr));
            break;
          }
          default:
            dl_.push(static_cast<float>(sL + fl));
            dr_.push(static_cast<float>(sR + fr));
        }
        wl = yl, wr = yr;
      }
      if (std::fabs(tone) > 0.001) {
        double tl = toneF_[0].process(wl), tr = toneF_[1].process(wr);
        wl = tone < 0 ? tl : wl - tl;
        wr = tone < 0 ? tr : wr - tr;
      }
      double ol = a * wl + (1 - a) * wr, orr = a * wr + (1 - a) * wl;
      L[i] = static_cast<float>(inL * dry + ol * wet);
      R[i] = static_cast<float>(inR * dry + orr * wet);
    }
  }
  void reset() override {
    ref_.reset();
    if (cls_.ready()) cls_.reset();
    dl_.clear(), dr_.clear();
    lpL_.z = lpR_.z = hpL_.z = hpR_.z = 0;
    svf_[0].reset(), svf_[1].reset();
    for (auto& c : dif_)
      for (auto& d : c) d.clear();
    toneF_[0].z = toneF_[1].z = 0;
    env_[0] = env_[1] = 0, hold_[0] = hold_[1] = 0, held_[0] = held_[1] = 0;
    xf_ = 0;
    curL_ = curR_ = -1;
  }

 private:
  double sr_ = 0, ph_ = 0, curL_ = -1, curR_ = -1, oldL_ = 0, oldR_ = 0, xf_ = 0, tone_ = 9, toneSr_ = 0;
  double env_[2] = {0, 0}, hold_[2] = {0, 0}, held_[2] = {0, 0};
  cdelay::Line cls_;
  double clsSr_ = 0;
  Line dl_, dr_;
  OnePole lpL_, lpR_, hpL_, hpR_, toneF_[2];
  Svf svf_[2];
  Diffuser dif_[2][2];
  uint32_t ctr_ = 0;
  DelayRef ref_;
};

// ======================================================================= Reverb
// Pre-delay → early reflections + input diffusers → 8 modulated combs per side
// (RT60-derived feedback, damping, bass multiplier) → 4 all-passes → colour,
// cuts, width.
class ReverbFx final : public Effect {
 public:
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    using namespace pp;
    if (sr_ != ctx.sampleRate) init(ctx.sampleRate);
    if (iparam(s, RvbModel) == 1) {  // J-26 plate reference
      plate::Settings ps;
      ps.lowCutHz = param(s, RvbLowCut, 75), ps.highCutHz = param(s, RvbHighCut, 4000), ps.preDelayMs = param(s, RvbPreDelay);
      ps.size = static_cast<int>(std::lround(param(s, RvbSize, 0.4f) * 100)), ps.diffusion = param(s, RvbDiffusion, 1);
      ps.decaySec = param(s, RvbDecayTime, 1.5f), ps.bassMult = param(s, RvbBassMult, 1), ps.dampHz = param(s, RvbDampHz, 22100);
      plate_.process(ps, param(s, RvbDry, 1), param(s, RvbWet, 0.2f), L, R, n);
      return;
    }
    double size = param(s, RvbSize, 0.6f), damp = param(s, RvbDamp, 0.4f);
    double width = param(s, RvbWidth, 1), early = param(s, RvbEarly, 0.3f), dry = param(s, RvbDry, 1), wet = param(s, RvbWet, 0.25f);
    // 1.12: the comb network decays slightly faster than its loop gains predict (measured RT60 calibration).
    double rt60 = param(s, RvbDecayTime, 1.5f) * 1.12, bass = param(s, RvbBassMult, 1), xo = param(s, RvbCrossover, 500);
    double diffusion = param(s, RvbDiffusion, 1), modDepth = param(s, RvbModDepth) * 0.001 * sr_, modRate = param(s, RvbModRate, 0.6f);
    bool stereoIn = iparam(s, RvbInputMode) == 1;
    int freeze = iparam(s, RvbFreeze);
    double dampHz = param(s, RvbDampHz, 22100);
    double dHz = dampHz < 22000 ? std::exp(-kTwoPi * std::max(20.0, dampHz) / sr_) : 0.0;  // one-pole in the comb loop
    double d1 = freeze == 1 ? 0.0 : std::max(damp * 0.45, dHz), d2 = 1 - d1;
    double preMs = param(s, RvbPreDelay, 10);
    // Tempo pre-delay: the value counts 1/192-beat ticks (384 ms ≙ 384 ticks = 2 beats, as the reference).
    double preSec = bparam(s, RvbPreSync) ? (preMs * 0.384 / 192.0) * 60.0 / std::max(20.0, ctx.tempo) : preMs * 0.001;
    double pre = std::min(static_cast<double>(pre_.buf.size() - 3), preSec * sr_ + 1);
    double lenScale = 0.55 + size * 0.75;
    for (int c = 0; c < 8; ++c)
      for (int side = 0; side < 2; ++side) {
        Comb& cb = combs_[side][c];
        cb.len = std::max(16.0, cb.base * lenScale);
        double g = std::pow(10.0, -3.0 * cb.len / (std::max(0.1, rt60) * sr_));
        double gLow = std::pow(10.0, -3.0 * cb.len / (std::max(0.1, rt60 * bass) * sr_));
        cb.g = freeze ? 1.0 : std::min(0.995, g);
        cb.gLow = freeze ? 1.0 : std::min(0.995, gLow);
      }
    if (xo != xo_ || param(s, RvbLowCut, 80) != lc_ || param(s, RvbHighCut, 9000) != hc_ || param(s, RvbColor) != color_) {
      xo_ = xo, lc_ = param(s, RvbLowCut, 80), hc_ = param(s, RvbHighCut, 9000), color_ = param(s, RvbColor);
      xoA_ = std::exp(-kTwoPi * xo / sr_);
      lcL_.setFreq(sr_, lc_), lcR_.a = lcL_.a;
      hcL_.setFreq(sr_, hc_), hcR_.a = hcL_.a;
      for (int c = 0; c < 2; ++c) {
        colLo_[c].set(Biquad::LowShelf, sr_, 1000, 0.6, -6.0 * color_);
        colHi_[c].set(Biquad::HighShelf, sr_, 1000, 0.6, 6.0 * color_);
      }
    }
    static const double erMs[6] = {7.1, 11.3, 17.9, 23.3, 31.7, 41.9};
    static const double erGain[6] = {0.82, 0.71, 0.58, 0.49, 0.38, 0.29};
    static const int difLen[4] = {142, 107, 379, 277};
    double wet1 = width / 2 + 0.5, wet2 = (1 - width) / 2;
    double inGain = freeze == 1 ? 0.0 : 1.0;
    // Per-block constants (J-25: were recomputed every sample).
    double erPos[6];
    for (int k = 0; k < 6; ++k) erPos[k] = pre + erMs[k] * (0.4 + size) * 0.001 * sr_;
    int difN[2][4];
    for (int c = 0; c < 2; ++c)
      for (int k = 0; k < 4; ++k) difN[c][k] = static_cast<int>(difLen[k] * sr_ / 44100.0) + c * 13;
    // The 16 comb modulators are one sine at phase offsets of k * 45 degrees:
    // sin(t + k pi/4) = sin t cos(k pi/4) + cos t sin(k pi/4) (one sin + cos per sample).
    static const double rotC[10] = {1, 0.70710678118654752, 0, -0.70710678118654752, -1, -0.70710678118654752, 0, 0.70710678118654752, 1,
                                    0.70710678118654752};
    static const double rotS[10] = {0, 0.70710678118654752, 1, 0.70710678118654752, 0, -0.70710678118654752, -1, -0.70710678118654752, 0,
                                    0.70710678118654752};
    for (int i = 0; i < n; ++i) {
      double inL = L[i], inR = R[i];
      double mid = (inL + inR) * 0.5;
      pre_.push(static_cast<float>(mid));
      preS_.push(static_cast<float>((inL - inR) * 0.5));
      double x = pre_.read(pre), sd = stereoIn ? preS_.read(pre) : 0.0;
      double erL = 0, erR = 0;
      for (int k = 0; k < 6; ++k) {
        double v = pre_.read(erPos[k]) * erGain[k];
        (k % 2 ? erR : erL) += v;
      }
      double xin[2] = {(x + sd) * 0.015 * inGain, (x - sd) * 0.015 * inGain};
      if (diffusion > 0.001)
        for (int c = 0; c < 2; ++c)
          for (int k = 0; k < 4; ++k) xin[c] = indiff_[c][k].process(xin[c], difN[c][k], 0.7 * diffusion);
      modPh_ += modRate / sr_;
      modPh_ -= std::floor(modPh_);
      double out[2] = {0, 0};
      double ms = 0, mc = 0;
      if (modDepth > 0) ms = std::sin(kTwoPi * modPh_), mc = std::cos(kTwoPi * modPh_);
      for (int side = 0; side < 2; ++side)
        for (int c = 0; c < 8; ++c) {
          Comb& cb = combs_[side][c];
          const int k = c + 2 * side;
          double mo = modDepth > 0 ? modDepth * (1 + ms * rotC[k] + mc * rotS[k]) : 0;
          double y = cb.line.read(cb.len + mo);
          double low = cb.xo = y + (cb.xo - y) * xoA_;
          double fbv = low * cb.gLow + (y - low) * cb.g;
          cb.store = fbv * d2 + cb.store * d1;
          double w = xin[side] + cb.store;
          if (freeze == 2) w = std::tanh(w);
          cb.line.push(static_cast<float>(w));
          out[side] += y;
        }
      for (int a = 0; a < 4; ++a) {
        out[0] = allpass(al_[a], out[0]);
        out[1] = allpass(ar_[a], out[1]);
      }
      double tl = (out[0] * wet1 + out[1] * wet2) * 3.0 + erL * early * 0.6;
      double tr = (out[1] * wet1 + out[0] * wet2) * 3.0 + erR * early * 0.6;
      tl = hcL_.process(tl);
      tr = hcR_.process(tr);
      tl -= lcL_.process(tl);
      tr -= lcR_.process(tr);
      if (std::fabs(color_) > 0.001) {
        tl = colHi_[0].process(colLo_[0].process(tl));
        tr = colHi_[1].process(colLo_[1].process(tr));
      }
      L[i] = static_cast<float>(inL * dry + tl * wet);
      R[i] = static_cast<float>(inR * dry + tr * wet);
    }
  }
  void reset() override {
    for (auto& side : combs_)
      for (auto& c : side) c.line.clear(), c.store = 0, c.xo = 0;
    for (auto* v : {&al_, &ar_})
      for (auto& a : *v) std::fill(a.buf.begin(), a.buf.end(), 0.f);
    for (auto& c : indiff_)
      for (auto& d : c) d.clear();
    pre_.clear(), preS_.clear();
    if (plate_.ready()) plate_.reset();
    lcL_.z = lcR_.z = hcL_.z = hcR_.z = 0;
    for (int c = 0; c < 2; ++c) colLo_[c].clear(), colHi_[c].clear();
  }

 private:
  struct Comb {
    Line line;
    double base = 1000, len = 1000, g = 0.8, gLow = 0.8, store = 0, xo = 0;
  };
  struct Allpass {
    std::vector<float> buf;
    size_t i = 0;
  };
  static double allpass(Allpass& a, double x) {
    double b = a.buf[a.i];
    double y = -x + b;
    a.buf[a.i] = static_cast<float>(x + b * 0.5);
    if (++a.i == a.buf.size()) a.i = 0;
    return y;
  }
  void init(double sr) {
    sr_ = sr;
    static const int combs[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
    static const int aps[4] = {556, 441, 341, 225};
    double k = sr / 44100.0;
    for (int c = 0; c < 8; ++c)
      for (int side = 0; side < 2; ++side) {
        Comb& cb = combs_[side][c];
        cb.base = (combs[c] + side * 23) * k;
        cb.line.init(static_cast<size_t>(cb.base * 1.4 + 0.004 * sr) + 8);
      }
    for (int a = 0; a < 4; ++a) {
      al_[a].buf.assign(static_cast<size_t>(aps[a] * k), 0.f);
      ar_[a].buf.assign(static_cast<size_t>((aps[a] + 23) * k), 0.f);
    }
    pre_.init(static_cast<size_t>(1.0 * sr) + 8);
    preS_.init(static_cast<size_t>(1.0 * sr) + 8);
    plate_.init(sr);
    xo_ = -1;
  }
  double sr_ = 0, modPh_ = 0, xo_ = -1, xoA_ = 0, lc_ = -1, hc_ = -1, color_ = 9;
  Comb combs_[2][8];
  std::array<Allpass, 4> al_, ar_;
  Diffuser indiff_[2][4];
  Line pre_, preS_;
  OnePole lcL_, lcR_, hcL_, hcR_;
  Biquad colLo_[2], colHi_[2];
  plate::Plate plate_;
};

// ======================================================================= Chorus
class ChorusFx final : public Effect {
 public:
  ChorusFx() {
    // Reference LFO wave tables (one cycle, x = i / 2048 in [0, 2)), stored as 0..1 weights:
    // parabolic sine, its cube, a three-partial "multi-sine" and its cube.
    for (int i = 0; i < kTab; ++i) {
      double x = i / 2048.0;
      double sn = x < 1 ? 4 * x * (1 - x) : -4 * (2 - x) * (x - 1);
      double ms = 0.78361 * (std::sin(3.1415 * x) + (std::sin(2 * 3.1415 * x) + std::sin(3 * 3.1415 * x)) / 3);
      double v[4] = {sn, sn * sn * sn, ms, ms * ms * ms};
      for (int w = 0; w < 4; ++w) tab_[w][i] = static_cast<float>((std::clamp(v[w], -1.0, 1.0) + 1) * 0.5);
    }
  }
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    using namespace pp;
    if (sr_ != ctx.sampleRate) {
      sr_ = ctx.sampleRate;
      bl_.init(static_cast<size_t>(sr_ * 0.08) + 4);
      br_.init(static_cast<size_t>(sr_ * 0.08) + 4);
      xf_ = -1;
    }
    if (bparam(s, ChoClassic, true)) {
      classic(s, L, R, n);
      return;
    }
    double rates[3] = {param(s, ChoRate, 0.6f), param(s, ChoRate2, 1.0f), param(s, ChoRate3, 1.6f)};
    double depth = param(s, ChoDepth), base = param(s, ChoDelay, 12) * 0.001 * sr_;
    double mix = param(s, ChoMix);
    int voices = std::clamp(iparam(s, ChoVoices) + 1, 1, 3);
    double stereo = param(s, ChoStereo, 0.5f), shape = param(s, ChoShape);
    int cross = iparam(s, ChoCrossMode);
    double xf = param(s, ChoCrossFreq, 800);
    if (cross && (xf != xf_)) {
      xf_ = xf;
      for (int c = 0; c < 2; ++c)
        for (int k = 0; k < 2; ++k) xlp_[c][k].set(Biquad::LowPass, sr_, xf, 0.7071, 0), xhp_[c][k].set(Biquad::HighPass, sr_, xf, 0.7071, 0);
    }
    bool wetOnly = bparam(s, ChoWetOnly);
    double fb = param(s, ChoFeedback);
    double dryG = wetOnly ? 0.0 : 1 - mix * 0.5, wetG = wetOnly ? 1.0 : mix;
    for (int i = 0; i < n; ++i) {
      double in[2] = {L[i], R[i]}, toLine[2] = {in[0], in[1]}, keep[2] = {0, 0};
      if (cross) {
        for (int c = 0; c < 2; ++c) {
          double lo = xlp_[c][1].process(xlp_[c][0].process(in[c])), hi = xhp_[c][1].process(xhp_[c][0].process(in[c]));
          toLine[c] = cross == 1 ? hi : lo;  // the other band stays dry
          keep[c] = cross == 1 ? lo : hi;
        }
      }
      double w[2] = {0, 0};
      for (int v = 0; v < voices; ++v) {
        double ph = ph_[v];
        double lfoL = lfoMorph(ph, shape), lfoR = lfoMorph(std::fmod(ph + stereo * 0.5, 1.0), shape);
        w[0] += bl_.read(base * (1 + depth * 0.5 * lfoL));
        w[1] += br_.read(base * (1 + depth * 0.5 * lfoR));
        ph_[v] += rates[v] / sr_;
        if (ph_[v] >= 1) ph_[v] -= 1;
      }
      w[0] /= voices, w[1] /= voices;
      bl_.push(static_cast<float>(toLine[0] + w[0] * fb));
      br_.push(static_cast<float>(toLine[1] + w[1] * fb));
      if (cross) {
        L[i] = static_cast<float>(keep[0] + toLine[0] * dryG + w[0] * wetG);
        R[i] = static_cast<float>(keep[1] + toLine[1] * dryG + w[1] * wetG);
      } else {
        L[i] = static_cast<float>(in[0] * dryG + w[0] * wetG);
        R[i] = static_cast<float>(in[1] * dryG + w[1] * wetG);
      }
    }
  }
  // Reference engine: three voices (own rate and wave), unipolar sweep delay .. delay + depth,
  // one-pole band split (only one band is chorused), dry 0.7 + wet 0.6 x the voice sum.
  void classic(const PluginState& s, float* L, float* R, int n) {
    using namespace pp;
    const double delay = param(s, ChoDelay, 15.03f) * 0.001 * sr_, depth = param(s, ChoDepthMs, 2.25f) * 0.001 * sr_;
    const double rates[3] = {param(s, ChoRate, 0.45f), param(s, ChoRate2, 1.25f), param(s, ChoRate3, 2.45f)};
    const int waves[3] = {std::clamp(iparam(s, ChoWave1), 0, 3), std::clamp(iparam(s, ChoWave2), 0, 3), std::clamp(iparam(s, ChoWave3), 0, 3)};
    const double stereo = std::clamp(static_cast<double>(param(s, ChoStereo, 1.f / 3)), 0.0, 1.0) * 0.5;
    const int cross = iparam(s, ChoCrossMode, 1);
    const bool wetOnly = bparam(s, ChoWetOnly);
    const double f = std::min(static_cast<double>(param(s, ChoCrossFreq, 320.2f)), sr_ / 4.1), w = kTwoPi * f / sr_;
    const double c1 = (1 - std::sin(w)) / std::cos(w), c0 = (1 - c1) * 0.5;
    auto look = [&](int wv, double ph) {
      double p = (ph - std::floor(ph)) * kTab;
      int i0 = static_cast<int>(p) % kTab, i1 = (i0 + 1) % kTab;
      double fr = p - std::floor(p);
      return tab_[wv][i0] + (tab_[wv][i1] - tab_[wv][i0]) * fr;
    };
    for (int i = 0; i < n; ++i) {
      double x[2] = {L[i] * 0.7, R[i] * 0.7}, toLine[2];
      for (int c = 0; c < 2; ++c) {
        lp_[c] = c1 * lp_[c] + c0 * (x[c] + xp_[c]);
        if (std::fabs(lp_[c]) < 1e-11) lp_[c] = 0;
        xp_[c] = x[c];
        toLine[c] = cross == 0 ? x[c] : cross == 2 ? lp_[c] : x[c] - lp_[c];
      }
      double wl = 0, wr = 0;
      for (int k = 0; k < 3; ++k) {
        wl += bl_.read(delay + depth * look(waves[k], ph_[k]));
        wr += br_.read(delay + depth * look(waves[k], ph_[k] + stereo));
        ph_[k] += rates[k] / sr_;
        ph_[k] -= std::floor(ph_[k]);
      }
      bl_.push(static_cast<float>(toLine[0]));
      br_.push(static_cast<float>(toLine[1]));
      wl *= 0.6, wr *= 0.6;
      L[i] = static_cast<float>(wetOnly ? wl : wl + x[0]);
      R[i] = static_cast<float>(wetOnly ? wr : wr + x[1]);
    }
  }
  void reset() override {
    bl_.clear(), br_.clear();
    lp_[0] = lp_[1] = xp_[0] = xp_[1] = 0;
    ph_[0] = 0, ph_[1] = 0.33, ph_[2] = 0.66;
    for (auto& c : xlp_)
      for (auto& b : c) b.clear();
    for (auto& c : xhp_)
      for (auto& b : c) b.clear();
  }

 private:
  static constexpr int kTab = 4096;
  double sr_ = 0, ph_[3] = {0, 0.33, 0.66}, xf_ = -1, lp_[2] = {}, xp_[2] = {};
  float tab_[4][kTab] = {};
  Line bl_, br_;
  Biquad xlp_[2][2], xhp_[2][2];
};

// ======================================================================= Flanger
class FlangerFx final : public Effect {
 public:
  FlangerFx() : ref_(std::make_unique<classic::Flanger>()) { ref_->reset(); }
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    using namespace pp;
    if (iparam(s, FlgModel) == 1) {  // J-26 reference engine
      classic::FlangerSettings fs;
      fs.delay = param(s, FlgDelayN), fs.depth = param(s, FlgDepthN, 0.4f), fs.rate = param(s, FlgRateN, 0.4f);
      fs.phase = param(s, FlgPhase, 118) / 360.0, fs.damp = param(s, FlgDampN), fs.shape = param(s, FlgShape);
      fs.feed = param(s, FlgFeedN, 0.23f), fs.invFeedback = bparam(s, FlgInvFb, true), fs.invWet = bparam(s, FlgInvertWet);
      fs.dry = param(s, FlgDryN, 0.7f), fs.wet = param(s, FlgWetN, 0.6f), fs.cross = param(s, FlgCrossN, 0.5f);
      ref_->process(fs, ctx.sampleRate, L, R, n);
      return;
    }
    if (sr_ != ctx.sampleRate) {
      sr_ = ctx.sampleRate;
      bl_.init(static_cast<size_t>(sr_ * 0.03) + 4);
      br_.init(static_cast<size_t>(sr_ * 0.03) + 4);
    }
    double base = param(s, FlgDelay, 1.5f) * 0.001 * sr_, depth = param(s, FlgDepth, 0.6f), rate = param(s, FlgRate, 0.2f);
    double fb = param(s, FlgFeedback, 0.5f), mix = param(s, FlgMix, 0.5f);
    double phase = param(s, FlgPhase, 90) / 360.0, shape = param(s, FlgShape), cross = param(s, FlgCross);
    double wsign = bparam(s, FlgInvertWet) ? -1.0 : 1.0;
    double damp = param(s, FlgDamp, 20000);
    bool useDamp = damp < 19900;
    if (useDamp) dpL_.setFreq(sr_, damp), dpR_.a = dpL_.a;
    for (int i = 0; i < n; ++i) {
      double lfoL = 0.5 + 0.5 * lfoMorph(ph_, shape), lfoR = 0.5 + 0.5 * lfoMorph(std::fmod(ph_ + phase, 1.0), shape);
      double dL = 1 + base * (1 + depth * 2 * lfoL), dR = 1 + base * (1 + depth * 2 * lfoR);
      double yl = bl_.read(dL), yr = br_.read(dR);
      if (useDamp) yl = dpL_.process(yl), yr = dpR_.process(yr);
      double fl = yl * (1 - cross) + yr * cross, fr = yr * (1 - cross) + yl * cross;
      bl_.push(static_cast<float>(L[i] + fl * fb));
      br_.push(static_cast<float>(R[i] + fr * fb));
      L[i] = static_cast<float>(L[i] * (1 - mix * 0.5) + wsign * yl * mix);
      R[i] = static_cast<float>(R[i] * (1 - mix * 0.5) + wsign * yr * mix);
      ph_ += rate / sr_;
      if (ph_ >= 1) ph_ -= 1;
    }
  }
  void reset() override {
    ref_->reset();
    bl_.clear(), br_.clear();
    ph_ = 0;
    dpL_.z = dpR_.z = 0;
  }

 private:
  std::unique_ptr<classic::Flanger> ref_;  // 256 KB of delay line: on the heap
  double sr_ = 0, ph_ = 0;
  Line bl_, br_;
  OnePole dpL_, dpR_;
};

// ======================================================================= Phaser
class PhaserFx final : public Effect {
 public:
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    using namespace pp;
    const double sr = ctx.sampleRate;
    if (iparam(s, PhsModel) == 1) {  // J-26 reference engine
      classic::PhaserSettings ps;
      ps.sweep = param(s, PhsSweepN, 0.5f), ps.large = iparam(s, PhsRangeLarge) == 1;
      ps.depthMin = param(s, PhsDepthMin, 0.1f), ps.depthMax = param(s, PhsDepthMax, 0.8f), ps.stereo = param(s, PhsStereoN, 0.5f);
      ps.stages = iparam(s, PhsStagesN, 8), ps.feedback = param(s, PhsFeedbackN, 0.4f), ps.dry = param(s, PhsDryN, 0.5f);
      ps.gain = param(s, PhsGainN, 0.8f);
      ref_.process(ps, sr, L, R, n);
      return;
    }
    static const int kStages[9] = {2, 4, 6, 8, 10, 12, 16, 20, 24};
    int stages = kStages[std::clamp(iparam(s, PhsStages, 2), 0, 8)];
    double rate = param(s, PhsRate, 0.4f), depth = param(s, PhsDepth, 0.7f), centre = param(s, PhsCenter, 800);
    double fb = param(s, PhsFeedback, 0.4f), mix = param(s, PhsMix, 0.5f);
    bool minmax = iparam(s, PhsRangeMode) == 1;
    double fmin = param(s, PhsMinFreq, 200), fmax = std::max(fmin, static_cast<double>(param(s, PhsMaxFreq, 3000)));
    double stereo = param(s, PhsStereo, 90) / 360.0, out = dbToGain(param(s, PhsOutGain));
    bool tri = iparam(s, PhsShape) == 1, manual = bparam(s, PhsManual);
    float* ch[2] = {L, R};
    for (int i = 0; i < n; ++i) {
      for (int c = 0; c < 2; ++c) {
        double ph = std::fmod(ph_ + c * stereo, 1.0);
        double lfo = manual ? -1.0 : tri ? (ph < 0.25 ? ph * 4 : ph < 0.75 ? 2 - ph * 4 : ph * 4 - 4) : std::sin(kTwoPi * ph);
        double f = minmax ? fmin * std::pow(fmax / fmin, 0.5 + 0.5 * lfo) : centre * std::pow(2.0, depth * 2.0 * lfo);
        f = std::clamp(f, 20.0, sr * 0.45);
        double t = std::tan(kPi * f / sr);
        double a = (t - 1) / (t + 1);
        double x = ch[c][i] + last_[c] * fb;
        for (int k = 0; k < stages; ++k) {  // first-order all-pass
          double y = a * x + z_[c][static_cast<size_t>(k)];
          z_[c][static_cast<size_t>(k)] = x - a * y;
          x = y;
        }
        last_[c] = x;
        ch[c][i] = static_cast<float>((ch[c][i] * (1 - mix * 0.5) + x * mix) * out);
      }
      ph_ += rate / sr;
      if (ph_ >= 1) ph_ -= 1;
    }
  }
  void reset() override {
    ref_.reset();
    for (auto& c : z_) c.fill(0);
    last_[0] = last_[1] = 0;
    ph_ = 0;
  }

 private:
  classic::Phaser ref_;
  std::array<std::array<double, 24>, 2> z_{};
  double last_[2] = {0, 0}, ph_ = 0;
};

// ======================================================================= Auto Pan
inline double apShape(int shape, double ph) {
  switch (shape) {
    case 1: return ph < 0.5 ? 4 * ph - 1 : 3 - 4 * ph;
    case 2: return ph < 0.5 ? 1 : -1;
    case 3: return 2 * ph - 1;
    case 4: return 1 - 2 * ph;
    default: return std::sin(kTwoPi * ph);
  }
}

class AutoPanFx final : public Effect {
 public:
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    using namespace pp;
    if (bparam(s, ApClassic, true)) {
      classic(s, ctx, L, R, n);
      return;
    }
    static const double beats[] = {16, 8, 4, 2, 1, 0.5, 0.25};
    int shape = iparam(s, ApShape);
    bool sync = bparam(s, ApSync, true) && ctx.playing;
    double b = beats[std::clamp(iparam(s, ApRate, 3), 0, 6)];
    double hz = param(s, ApRateHz, 1), panAmt = param(s, ApPan, 0.7f), volAmt = param(s, ApVolume), phase = param(s, ApPhase);
    double span = param(s, ApStaticPan), svol = param(s, ApStaticVol, 1);
    double spb = 60.0 / std::max(1.0, ctx.tempo) * ctx.sampleRate;
    for (int i = 0; i < n; ++i) {
      double ph;
      if (sync) {
        double bt = ctx.tick / std::max(1, ctx.ppq) + i / spb;
        ph = bt / b + phase;
      } else {
        free_ += hz / ctx.sampleRate;
        free_ -= std::floor(free_);
        ph = free_ + phase;
      }
      ph -= std::floor(ph);
      double v = apShape(shape, ph);
      sm_ += (v - sm_) * (shape >= 2 ? 0.02 : 1.0);  // soften square/saw edges
      double pl, pr;
      panGains(std::clamp(span + sm_ * panAmt, -1.0, 1.0), pl, pr);
      double g = (1 - volAmt * (0.5 - 0.5 * sm_)) * svol;
      L[i] = static_cast<float>(L[i] * pl * g);
      R[i] = static_cast<float>(R[i] * pr * g);
      lastPan_ = static_cast<float>(std::clamp(span + sm_ * panAmt, -1.0, 1.0)), lastVol_ = static_cast<float>(g);
    }
  }
  void reset() override { free_ = 0, sm_ = 0, primed_ = false; }
  // Reference engine: static pan / volume knob, one LFO (sine / triangle / square) on pan or volume,
  // evaluated per block; pan law folds half of the attenuated side into the other; gains glide.
  void classic(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) {
    using namespace pp;
    const int shape = std::clamp(iparam(s, ApShape), 0, 2), target = std::clamp(iparam(s, ApTarget), 0, 2);
    const double x = param(s, ApStaticVol, 1), sp = param(s, ApSpeedK, 27500.f / 65536.f) * 65536.0;
    double p = std::clamp(static_cast<double>(param(s, ApStaticPan)), -1.0, 1.0);
    double g = x == 1.0 ? 1.0 : 0.1 * (std::pow(11.0, x) - 1.0);
    const double period = std::round(std::pow(50000.0, (65536.0 - sp) / 65536.0));  // LFO period in samples
    const double inc = 1.0 / std::max(1.0, period);
    double lfo = 0;
    if (target) {
      double ph = free_ - std::floor(free_);
      lfo = shape == 1 ? (ph < 0.25 ? 4 * ph : ph < 0.75 ? 2 - 4 * ph : 4 * ph - 4) : shape == 2 ? (ph < 0.5 ? 1.0 : -1.0) : std::sin(kTwoPi * ph);
      if (target == 1) p = std::clamp(p + param(s, ApAmount, 0.5f) * lfo, -1.0, 1.0);
      else g = std::max(0.0, g + param(s, ApAmount, 0.5f) * 0.5 * lfo);
    }
    double tLL, tRR, tRL, tLR;
    if (p < 0) tRR = (1 + p) * g, tRL = -p * 0.5 * g, tLL = g - tRL, tLR = 0;
    else tLL = (1 - p) * g, tLR = p * 0.5 * g, tRR = g * (1 - 0.5 * p), tRL = 0;
    if (!primed_) gLL_ = tLL, gRR_ = tRR, gRL_ = tRL, gLR_ = tLR, primed_ = true;
    const double c = 1.0 - std::exp(-1.0 / (0.005 * ctx.sampleRate));
    for (int i = 0; i < n; ++i) {
      gLL_ += (tLL - gLL_) * c, gRR_ += (tRR - gRR_) * c, gRL_ += (tRL - gRL_) * c, gLR_ += (tLR - gLR_) * c;
      double l = L[i], r = R[i];
      L[i] = static_cast<float>(gLL_ * l + gRL_ * r);
      R[i] = static_cast<float>(gRR_ * r + gLR_ * l);
    }
    free_ += inc * n;
    free_ -= std::floor(free_);
    lastPan_ = static_cast<float>(p), lastVol_ = static_cast<float>(g);
  }
  int meters(float* out, int max) const override {  // current pan / volume (editor's position display)
    if (max < 2) return 0;
    out[0] = lastPan_, out[1] = lastVol_;
    return 2;
  }

 private:
  double free_ = 0, sm_ = 0, gLL_ = 1, gRR_ = 1, gRL_ = 0, gLR_ = 0;
  bool primed_ = false;
  float lastPan_ = 0, lastVol_ = 1;
};

// ======================================================================= Freq Shifter
// Hilbert pair from two 4-stage all-pass chains (Niemitalo coefficients).
struct Hilbert {
  static constexpr double a1[4] = {0.6923878, 0.9360654322959, 0.9882295226860, 0.9987488452737};
  static constexpr double a2[4] = {0.4021921162426, 0.8561710882420, 0.9722909545651, 0.9952884791278};
  double x1[4][2] = {}, y1[4][2] = {}, x2[4][2] = {}, y2[4][2] = {}, delayed = 0;
  static double ap(double a, double x, double (&xs)[2], double (&ys)[2]) {
    double y = a * a * (x + ys[1]) - xs[1];
    xs[1] = xs[0], xs[0] = x;
    ys[1] = ys[0], ys[0] = y;
    return y;
  }
  void process(double x, double& i, double& q) {
    double p = x, r = x;
    for (int k = 0; k < 4; ++k) p = ap(a1[k], p, x1[k], y1[k]);
    for (int k = 0; k < 4; ++k) r = ap(a2[k], r, x2[k], y2[k]);
    i = delayed;
    delayed = p;
    q = r;
  }
};

class FreqShiftFx final : public Effect {
 public:
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    using namespace pp;
    if (iparam(s, FsModel) == 1) {
      fsref::Settings fs;
      fs.type = std::clamp(iparam(s, FsType), 0, 2), fs.range = std::clamp(iparam(s, FsRange), 0, 2);
      fs.freqPos = param(s, FsFreqPos, 0.5f), fs.mix = param(s, FsMixRef, 1), fs.lrPhase = param(s, FsLrPhase);
      fs.shapeL = param(s, FsShapeL, 1), fs.shapeR = param(s, FsShapeR, 1), fs.feedback = param(s, FsFeedbackRef, 0.5f);
      fs.stereo = param(s, FsWidth, 1), fs.startPhase = param(s, FsStartRef, 0.5f);
      ref_.process(fs, ctx.sampleRate, ctx.tempo, ctx.playing, ctx.tick / std::max(1, ctx.ppq), L, R, n);
      return;
    }
    int range = iparam(s, FsRange);
    double hz = range == 1 ? param(s, FsShiftWide) : range == 2 ? param(s, FsShiftBeats, 1) * ctx.tempo / 60.0 : param(s, FsShift, 50);
    double fb = param(s, FsFeedback), stereo = param(s, FsStereo, 0.25f), mix = param(s, FsMix, 0.5f);
    int type = iparam(s, FsType);
    double width = param(s, FsWidth, 1);
    double shape[2] = {param(s, FsShapeL, 1), param(s, FsShapeR, 1)};
    if (ctx.playing && !wasPlaying_) ph_ = param(s, FsStartPhase) / 360.0;  // repeatable from the transport start
    wasPlaying_ = ctx.playing;
    auto warp = [](double c, double k) { return k >= 0.999 ? c : (c < 0 ? -1 : 1) * std::pow(std::fabs(c), std::max(0.05, k)); };
    const double inc = hz / ctx.sampleRate;
    for (int i = 0; i < n; ++i) {
      float* ch[2] = {L, R};
      double y[2];
      for (int c = 0; c < 2; ++c) {
        double x = ch[c][i] + last_[c] * fb;
        double phc = kTwoPi * (ph_ + c * stereo * 0.5);
        if (type == 2) {  // ring modulator: both sidebands
          y[c] = x * warp(std::cos(phc), shape[c]);
        } else if (type == 1) {  // HQ: shift at twice the rate
          double sub = 0;
          int k = 0;
          y[c] = ovs_[c].run(x, 1, [&](double v) {
            double I, Q;
            hq_[c].process(v, I, Q);
            double p = phc + kTwoPi * inc * 0.5 * (k++ == 0 ? 0.0 : 1.0);
            sub = I * warp(std::cos(p), shape[c]) - Q * warp(std::sin(p), shape[c]);
            return sub;
          });
        } else {
          double I, Q;
          h_[c].process(x, I, Q);
          y[c] = I * warp(std::cos(phc), shape[c]) - Q * warp(std::sin(phc), shape[c]);
        }
        last_[c] = y[c];
      }
      if (width < 0.999) {
        double m = 0.5 * (y[0] + y[1]), sd = 0.5 * (y[0] - y[1]) * width;
        y[0] = m + sd, y[1] = m - sd;
      }
      for (int c = 0; c < 2; ++c) ch[c][i] = static_cast<float>(ch[c][i] * (1 - mix) + y[c] * mix);
      ph_ += inc;
      ph_ -= std::floor(ph_);
    }
  }
  void reset() override {
    h_[0] = Hilbert{}, h_[1] = Hilbert{}, hq_[0] = Hilbert{}, hq_[1] = Hilbert{};
    ovs_[0].reset(), ovs_[1].reset();
    last_[0] = last_[1] = 0;
    ph_ = 0;
    ref_.reset();
  }

 private:
  fsref::Shifter ref_;
  Hilbert h_[2], hq_[2];
  Oversampler ovs_[2];
  double last_[2] = {0, 0}, ph_ = 0;
  bool wasPlaying_ = false;
};

}  // namespace

std::unique_ptr<Effect> createEffectW5(const std::string& type) {
  if (type == "gl.filter") return std::make_unique<FilterFx>();
  if (type == "gl.delay") return std::make_unique<DelayFx>();
  if (type == "gl.reverb") return std::make_unique<ReverbFx>();
  if (type == "gl.chorus") return std::make_unique<ChorusFx>();
  if (type == "gl.flanger") return std::make_unique<FlangerFx>();
  if (type == "gl.phaser") return std::make_unique<PhaserFx>();
  if (type == "gl.autopan") return std::make_unique<AutoPanFx>();
  if (type == "gl.freqshift") return std::make_unique<FreqShiftFx>();
  return nullptr;
}

}  // namespace gl
