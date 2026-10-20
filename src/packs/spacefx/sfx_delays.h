// Gridline — "spacefx" pack: Multi Delay and Band Delay (included by spacefx_dsp.cpp only).
#pragma once
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include "sfx_common.h"
#include "sfx_flcurves.h"
#include "sfx_resample.h"

namespace gl::packs::spacefx::detail {

// =============================================================== Multi Delay
// Eight delay units. Each: input level/pan/filter → stereo delay line with its
// own feedback loop (mode, pan, filter, width, diffusion, modulation) → gate
// ("grain") → output level/pan. "To next" routes a unit's output into the
// next unit's input, building cascades. Zero latency; per-sample schedule.
// MdModel = 1 runs the J-1 reference model (processRef below).
class MultiDelayFx final : public Effect {
 public:
  MultiDelayFx() {
    for (auto& t : taps_)
      for (auto& a : t.ap) a.init(4096);  // the delay lines: see prepareHost
    coefs_.init();
    for (auto& h : up_) h.c = &coefs_;
    for (auto& h : dn_) h.c = &coefs_;
  }
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    if (!prepared_.load(std::memory_order_acquire))  // used outside the engine: every unit now
      for (int k = 0; k < kTaps; ++k) ensureTap(k);
    const bool refModel = pi(s, MdModel, 0) == 1;
    if (refModel != lastRef_) {
      reset();
      lastRef_ = refModel;
    }
    if (refModel) {
      processRef(s, ctx, L, R, n);
      return;
    }
    const double sr = ctx.sampleRate;
    const bool sync = pb(s, MdSync);
    const double stepSec = 15.0 / std::max(1.0, ctx.tempo);
    const double dry = pv(s, MdDry, 1), wet = pv(s, MdWet, 0.5), gin = pv(s, MdInput, 1), fbScale = pv(s, MdFbScale, 1);
    const double maxD = static_cast<double>(kMdMaxDelay - 8);
    // Per-block parameter setup.
    for (int k = 0; k < kTaps; ++k) {
      Tap& t = taps_[k];
      auto P = [&](int f, float d = 0.f) { return pv(s, mdParam(k, f), d); };
      t.on = P(MdOn) >= 0.5f;
      double sec = sync ? P(MdSteps, 4) * stepSec : P(MdTimeMs, 300) * 0.001;
      t.target = clampd(sec * sr, 2.0, maxD - 0.06 * sr);
      t.offset = P(MdOffset) * 0.001 * sr;
      t.inLevel = clampd(P(MdInLevel, 1), 0, 1.25) * gin;  // legacy ranges (J-1 widened them for the reference model)
      balance(P(MdInPan), t.inL, t.inR);
      t.inType = std::clamp(static_cast<int>(std::lround(P(MdInFType))), 0, 3);
      t.inF[0].set(sr, P(MdInCut, 2000), P(MdInRes, 0.1f));
      t.inF[1] = copyCoef(t.inF[1], t.inF[0]);
      t.fb = clampd(P(MdFeedback, 0.35f) * fbScale, 0.0, 0.995);
      t.mode = std::clamp(static_cast<int>(std::lround(P(MdFbMode))), 0, 2);
      balance(P(MdFbPan), t.fbL, t.fbR);
      t.fbType = std::clamp(static_cast<int>(std::lround(P(MdFbFType, 1))), 0, 3);
      t.fbF[0].set(sr, P(MdFbCut, 6000), P(MdFbRes, 0.1f));
      t.fbF[1] = copyCoef(t.fbF[1], t.fbF[0]);
      t.width = 1.0 + P(MdSep);
      t.modInc = P(MdModRate, 0.4f) / sr;
      t.modDepth = P(MdModDepth) * 0.004 * sr;
      t.diffuse = P(MdDiffuse) * 0.7;
      t.gateDiv = static_cast<int>(std::lround(P(MdGateDiv)));
      t.gateShape = P(MdGateShape);
      t.outLevel = clampd(P(MdOutLevel, 0.8f), 0, 1.25);
      balance(P(MdOutPan), t.outL, t.outR);
      t.toNext = clampd(P(MdToNext), -1, 1);
      t.smooth.setTime(sr, 0.05);
      t.apD[0] = 0.0071 * sr;
      t.apD[1] = 0.0113 * sr;
      t.apD[2] = 0.0089 * sr;
      t.apD[3] = 0.0137 * sr;
    }
    for (int i = 0; i < n; ++i) {
      const float xl = L[i], xr = R[i];
      double wl = 0, wr = 0, nextL = 0, nextR = 0;
      for (int k = 0; k < kTaps; ++k) {
        Tap& t = taps_[k];
        if ((!t.on && !t.active) || !ready_[k].load(std::memory_order_acquire)) {
          nextL = nextR = 0;
          continue;
        }
        double d = t.smooth.next(t.target);
        double mod = 0;
        if (t.modDepth > 0) {
          mod = t.modDepth * (0.5 + 0.5 * std::sin(kTwoPi * t.modPh));
          t.modPh += t.modInc;
          if (t.modPh >= 1) t.modPh -= 1;
        }
        double dl = d + std::max(0.0, -t.offset) + mod, dr = d + std::max(0.0, t.offset) + mod;
        float yl = t.line[0].read(dl), yr = t.line[1].read(dr);
        // Gate ("grain"): chops each echo period into `div` slices.
        if (t.gateDiv > 0) {
          double g = gate(t.gatePh, t.gateShape);
          yl = static_cast<float>(yl * g);
          yr = static_cast<float>(yr * g);
          t.gatePh += t.gateDiv / std::max(1.0, d);
          if (t.gatePh >= 1) t.gatePh -= std::floor(t.gatePh);
        }
        // Input (plus the previous unit's routed output).
        double il = 0, ir = 0;
        if (t.on) {
          il = (xl * t.inL + nextL) * t.inLevel;
          ir = (xr * t.inR + nextR) * t.inLevel;
          il = t.inF[0].process(static_cast<float>(il), t.inType);
          ir = t.inF[1].process(static_cast<float>(ir), t.inType);
        }
        // Feedback path: filter, width, pan, diffusion, mode.
        double fl = t.fbF[0].process(yl, t.fbType), fr = t.fbF[1].process(yr, t.fbType);
        double m = 0.5 * (fl + fr), sd = 0.5 * (fl - fr) * t.width;
        fl = (m + sd) * t.fbL;
        fr = (m - sd) * t.fbR;
        if (t.diffuse > 0) {
          float g = static_cast<float>(t.diffuse);
          fl = t.ap[0].process(t.ap[1].process(static_cast<float>(fl), t.apD[1], g), t.apD[0], g);
          fr = t.ap[2].process(t.ap[3].process(static_cast<float>(fr), t.apD[3], g), t.apD[2], g);
        }
        double wL, wR;
        switch (t.mode) {
          case 1:  // ping-pong: input enters left (mono), echoes swap sides
            wL = 0.5 * (il + ir) + fr * t.fb;
            wR = fl * t.fb;
            break;
          case 2:
            wL = il - fl * t.fb;
            wR = ir - fr * t.fb;
            break;
          default:
            wL = il + fl * t.fb;
            wR = ir + fr * t.fb;
            break;
        }
        // Soft safety clip inside the loop keeps runaway feedback bounded.
        t.line[0].push(flush(static_cast<float>(soft(wL))));
        t.line[1].push(flush(static_cast<float>(soft(wR))));
        double ol = yl * t.outLevel * t.outL, orr = yr * t.outLevel * t.outR;
        if (t.on) {
          wl += ol;
          wr += orr;
        }
        nextL = t.on ? yl * t.toNext : 0;
        nextR = t.on ? yr * t.toNext : 0;
        // Keep running a disabled unit until its tail is gone (no clicks on re-enable).
        double e = std::fabs(yl) + std::fabs(yr);
        t.active = t.on || e > 1e-6;
        if (!t.on && !t.active) t.clear();
      }
      L[i] = flush(static_cast<float>(xl * dry + wl * wet));
      R[i] = flush(static_cast<float>(xr * dry + wr * wet));
    }
  }
  void reset() override {
    for (int k = 0; k < kTaps; ++k)
      if (ready_[k].load(std::memory_order_acquire)) {  // (a unit's lines may be being allocated otherwise)
        taps_[k].clear();
        ref_[k].clear();
      }
    for (auto& h : up_) h.clear();
    for (auto& h : dn_) h.clear();
    refLeft_ = refJ_ = 0;
  }

  // J-30: a unit's delay lines (3 MB) are allocated when the unit is first
  // switched on (UI thread, every program build) and published to the audio
  // thread; units without lines are silent. An instance that is never
  // prepared (used outside the engine) allocates every unit on first use.
  void prepareHost(const PluginState& s, double) override {
    prepared_.store(true, std::memory_order_release);
    for (int k = 0; k < kTaps; ++k)
      if (pv(s, mdParam(k, MdOn)) >= 0.5f) ensureTap(k);
  }

 private:
  void ensureTap(int k) {
    if (ready_[k].load(std::memory_order_acquire)) return;
    taps_[k].line[0].init(kMdMaxDelay);
    taps_[k].line[1].init(kMdMaxDelay);
    ref_[k].off[0].init(1 << 17);
    ref_[k].off[1].init(1 << 17);
    ready_[k].store(true, std::memory_order_release);
  }
  std::atomic<bool> ready_[kTaps] = {};
  std::atomic<bool> prepared_{false};
  int refLeft_ = 0, refJ_ = 0;  // reference model: position in the current ramp sub-block
  // ------------------------------------------------------------ reference model
  // Per unit: input (level / pan, plus the previous unit's output × "to next")
  // → input filter (pre) → stereo line whose period is the unit time: each
  // pass is filtered and mixed through a 2×2 feedback matrix (pan /
  // separation; ping-pong swaps straight and cross; invert swaps the stored
  // sides) and the output is the processed pass, or the raw pass with "leave
  // first echo" → grain window → input filter (post) → output level / pan.
  // Optional 2× oversampling of the whole bank. Gridline's modulation and
  // diffusion extras stay available (0 = off, as imported).
  struct BqState {
    double z1 = 0, z2 = 0;
    double run(const flc::Bq& q, double x) {
      double y = q.b0 * x + z1;
      z1 = flushd(q.b1 * x - q.a1 * y + z2);
      z2 = flushd(q.b2 * x - q.a2 * y);
      return y;
    }
  };
  // Per-block linear parameter ramp.
  struct Ramp {
    double a = 0, b = 0;
    bool primed = false;
    void target(double v) {
      a = primed ? b : v;
      b = v;
      primed = true;
    }
    double at(double t) const { return a + (b - a) * t; }
  };
  struct RefTap {
    Ring off[2];
    BqState inS[2][3], fbS[2][3];
    flc::Bq inQ, fbQ;
    int inType = 0, inStages = 1, fbType = 0, fbStages = 1, mode = 0, D = 1, offN[2] = {0, 0}, div = 0, gLen = 2;
    bool on = false, post = false, leaveFirst = false;
    double gPos = 0, gPeriod = 1, gShape = 0, modInc = 0, modDepth = 0, modPh = 0, diffuse = 0;
    Ramp mi[4], mo[4], toNext, m[4];
    void clear() {
      off[0].clear();
      off[1].clear();
      for (auto& c : inS)
        for (auto& b : c) b = BqState{};
      for (auto& c : fbS)
        for (auto& b : c) b = BqState{};
      gPos = modPh = 0;
      toNext = Ramp{};
      for (auto& q : mi) q = Ramp{};
      for (auto& q : mo) q = Ramp{};
      for (auto& r : m) r = Ramp{};
    }
  };
  // Feedback matrix of a unit: m0 L→L, m1 R→R, m2 R→L, m3 L→R from the
  // feedback volume g, pan and stereo separation (sep < 0 widens, > 0 blends
  // toward mono); ping-pong swaps straight and cross.
  static void fbMatrix(double g, double pan, double sep, bool pingPong, double m[4]) {
    flc::mdPanMatrix(pan, g, sep, true, m);
    if (pingPong) {
      double a = m[0], b = m[1];
      m[0] = m[3], m[1] = m[2], m[2] = b, m[3] = a;
    }
  }
  // Parameter ramps of the reference model run over sub-blocks of kRefSub
  // samples on the effect's own clock (J-30: ramps over each host block made
  // the output depend on the block size).
  static constexpr int kRefSub = 32;
  void processRef(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) {
    const bool os = pb(s, MdOversample);
    if (os != lastOs_) {
      for (auto& h : up_) h.clear();
      for (auto& h : dn_) h.clear();
      lastOs_ = os;
    }
    const double dry = pv(s, MdDry, 1);
    const int sub = os ? 2 : 1;
    const double inv = 1.0 / (static_cast<double>(kRefSub) * sub);
    for (int i = 0; i < n; ++i) {
      if (refLeft_ == 0) {
        setupRef(s, ctx, os);
        refLeft_ = kRefSub;
        refJ_ = 0;
      }
      --refLeft_;
      const double xl = L[i], xr = R[i];
      double wl = 0, wr = 0;
      if (os) {
        double ul[2], ur[2], ol[2], orr[2];
        up_[0].up(xl, ul[0], ul[1]);
        up_[1].up(xr, ur[0], ur[1]);
        for (int q = 0; q < 2; ++q, ++refJ_) bankRef(ul[q], ur[q], (refJ_ + 1) * inv, ol[q], orr[q]);
        wl = dn_[0].down(ol[0], ol[1]);
        wr = dn_[1].down(orr[0], orr[1]);
      } else {
        bankRef(xl, xr, (refJ_ + 1) * inv, wl, wr);
        ++refJ_;
      }
      L[i] = flush(static_cast<float>(xl * dry + wl));
      R[i] = flush(static_cast<float>(xr * dry + wr));
    }
  }
  void setupRef(const PluginState& s, const ProcessContext& ctx, bool os) {
    const double sr = ctx.sampleRate * (os ? 2.0 : 1.0);
    const double stepSec = 15.0 / std::max(1.0, ctx.tempo);
    const double wet = pv(s, MdWet, 0.5), gin = pv(s, MdInput, 1), fbScale = pv(s, MdFbScale, 1);
    const int maxD = static_cast<int>(kMdMaxDelay) - 8, maxOff = (1 << 17) - 8;
    for (int k = 0; k < kTaps; ++k) {
      RefTap& r = ref_[k];
      auto P = [&](int f, float d = 0.f) { return static_cast<double>(pv(s, mdParam(k, f), d)); };
      auto X = [&](int e, float d = 0.f) { return static_cast<double>(pv(s, mdExt(k, e), d)); };
      r.on = P(MdOn) >= 0.5;
      r.mode = std::clamp(static_cast<int>(std::lround(P(MdFbMode))), 0, 3);
      double sec = X(MdxTempo, 1) >= 0.5 ? P(MdSteps, 4) * stepSec : P(MdTimeMs, 300) * 0.001;
      r.D = std::clamp(static_cast<int>(std::lround(sec * sr)), 1, maxD);
      double offMs = P(MdOffset);
      int offN = std::clamp(static_cast<int>(std::lround(std::fabs(offMs) * 0.001 * sr)), 0, maxOff);
      r.offN[0] = offMs < 0 ? offN : 0;
      r.offN[1] = offMs > 0 ? offN : 0;
      double mm[4];
      flc::mdPanMatrix(clampd(P(MdInPan), -1, 1), P(MdInLevel, 1) * gin, 0, false, mm);
      for (int j = 0; j < 4; ++j) r.mi[j].target(mm[j]);
      r.inType = std::clamp(static_cast<int>(std::lround(P(MdInFType))), 0, 7);
      r.inStages = std::clamp(static_cast<int>(std::lround(X(MdxInStages, 1))), 1, 3);
      r.inQ = flc::mdDesign(r.inType, P(MdInCut, 2000), P(MdInRes, 0.1f), X(MdxInGain, 1), r.inStages, sr);
      r.post = X(MdxInPost) >= 0.5;
      r.fbType = std::clamp(static_cast<int>(std::lround(P(MdFbFType, 1))), 0, 7);
      r.fbStages = std::clamp(static_cast<int>(std::lround(X(MdxFbStages, 1))), 1, 3);
      r.fbQ = flc::mdDesign(r.fbType, P(MdFbCut, 6000), P(MdFbRes, 0.1f), X(MdxFbGain, 1), r.fbStages, sr);
      double m[4];
      const double sep = clampd(P(MdSep), -1, 1);
      // J-35: a negative separation also scales the feedback volume by (1 + s/2).
      fbMatrix((P(MdFeedback, 0.5f) * (sep < 0 ? 1.0 + 0.5 * sep : 1.0)) * fbScale, clampd(P(MdFbPan), -1, 1), sep, r.mode == MfbPingPong, m);
      for (int j = 0; j < 4; ++j) r.m[j].target(m[j]);
      r.leaveFirst = X(MdxLeaveFirst) >= 0.5;
      r.div = std::clamp(static_cast<int>(std::lround(P(MdGateDiv))), 0, 16);
      r.gShape = clampd(P(MdGateShape), -1, 1);
      if (r.div > 0) {
        r.gPeriod = sec * sr / r.div;  // the unrounded delay time
        r.gLen = std::max(2, static_cast<int>(std::lround(r.gPeriod)));
      }
      flc::mdPanMatrix(clampd(P(MdOutPan), -1, 1), P(MdOutLevel, 0.8f) * wet, 0, false, mm);
      for (int j = 0; j < 4; ++j) r.mo[j].target(mm[j]);
      r.toNext.target(P(MdToNext));
      r.modInc = P(MdModRate, 0.4f) / sr;
      r.modDepth = P(MdModDepth) * 0.004 * sr;
      r.diffuse = P(MdDiffuse) * 0.7;
      Tap& t = taps_[k];
      t.apD[0] = 0.0071 * sr;
      t.apD[1] = 0.0113 * sr;
      t.apD[2] = 0.0089 * sr;
      t.apD[3] = 0.0137 * sr;
    }
  }
  static double runStages(BqState* st, const flc::Bq& q, int stages, double x) {
    for (int k = 0; k < stages; ++k) x = st[k].run(q, x);
    return x;
  }
  // Loop guard: linear to ±2, then saturates toward ±3 (runaway feedback stays bounded).
  static double safe(double x) { return std::fabs(x) < 2.0 ? x : (x > 0 ? 2.0 + std::tanh(x - 2.0) : -2.0 + std::tanh(x + 2.0)); }
  void bankRef(double xl, double xr, double t, double& outL, double& outR) {
    double prevL = 0, prevR = 0, prevG = 0, wl = 0, wr = 0;
    for (int k = 0; k < kTaps; ++k) {
      RefTap& r = ref_[k];
      if (!r.on || !ready_[k].load(std::memory_order_acquire)) {
        prevL = prevR = prevG = 0;
        continue;
      }
      Tap& tp = taps_[k];
      double y[2];
      double in[2] = {r.mi[0].at(t) * xl + r.mi[2].at(t) * xr + prevL * prevG, r.mi[1].at(t) * xr + r.mi[3].at(t) * xl + prevR * prevG};
      if (r.inType > 0 && !r.post)
        for (int c = 0; c < 2; ++c) in[c] = runStages(r.inS[c], r.inQ, r.inStages, in[c]);
      if (r.mode == MfbOff) {
        y[0] = in[0], y[1] = in[1];
      } else {
        double cpass[2], f[2];
        double mod = 0;
        if (r.modDepth > 0) {
          mod = r.modDepth * (0.5 + 0.5 * std::sin(kTwoPi * r.modPh));
          r.modPh += r.modInc;
          if (r.modPh >= 1) r.modPh -= 1;
        }
        for (int c = 0; c < 2; ++c) {
          r.off[c].push(static_cast<float>(in[c]));
          cpass[c] = mod > 0 ? tp.line[c].read(r.D + mod) : tp.line[c].at(static_cast<size_t>(r.D));
          f[c] = r.fbType > 0 ? runStages(r.fbS[c], r.fbQ, r.fbStages, cpass[c]) : cpass[c];
        }
        double p[2] = {f[0] * r.m[0].at(t) + f[1] * r.m[2].at(t), f[1] * r.m[1].at(t) + f[0] * r.m[3].at(t)};
        if (r.diffuse > 0) {
          float g = static_cast<float>(r.diffuse);
          p[0] = tp.ap[0].process(tp.ap[1].process(static_cast<float>(p[0]), tp.apD[1], g), tp.apD[0], g);
          p[1] = tp.ap[2].process(tp.ap[3].process(static_cast<float>(p[1]), tp.apD[3], g), tp.apD[2], g);
        }
        const bool swap = r.mode == MfbInvert;
        for (int c = 0; c < 2; ++c) {
          double xo = r.offN[c] > 0 ? r.off[c].at(static_cast<size_t>(r.offN[c]) + 1) : in[c];
          tp.line[c].push(flush(static_cast<float>(safe(xo + p[c]))));
          y[c] = r.leaveFirst ? cpass[c] : p[c];
        }
        if (swap) std::swap(y[0], y[1]);  // invert: the unit's output sides swap (J-35), not the feedback
      }
      if (r.div > 0) {
        while (r.gPos >= r.gPeriod) r.gPos -= r.gPeriod;
        int idx = static_cast<int>(std::lround(r.gPos)) % r.gLen;
        double w = flc::mdGrainWindow(idx, r.gLen, r.gShape);
        y[0] *= w, y[1] *= w;
        r.gPos += 1.0;
      }
      if (r.inType > 0 && r.post)
        for (int c = 0; c < 2; ++c) y[c] = runStages(r.inS[c], r.inQ, r.inStages, y[c]);
      wl += r.mo[0].at(t) * y[0] + r.mo[2].at(t) * y[1];
      wr += r.mo[1].at(t) * y[1] + r.mo[3].at(t) * y[0];
      prevL = y[0], prevR = y[1], prevG = r.toNext.at(t);
    }
    outL = wl, outR = wr;
  }
  RefTap ref_[kTaps];
  HalfbandCoefs coefs_;
  Halfband2x up_[2], dn_[2];
  bool lastRef_ = false, lastOs_ = false;

  static double soft(double x) { return std::fabs(x) < 1.5 ? x : (x > 0 ? 1.5 + std::tanh(x - 1.5) : -1.5 + std::tanh(x + 1.5)); }
  static Svf copyCoef(Svf dst, const Svf& src) {
    dst.a1 = src.a1;
    dst.a2 = src.a2;
    dst.a3 = src.a3;
    dst.k = src.k;
    return dst;
  }
  // Gate envelope over one slice (phase 0..1). shape < 0: decaying slices,
  // 0: square with soft edges, > 0: swelling slices.
  static double gate(double ph, double shape) {
    double sq = ph < 0.5 ? std::min(1.0, ph * 40.0) : std::max(0.0, 1.0 - (ph - 0.5) * 40.0);
    if (shape < 0) return sq * (1 + shape) + (-shape) * std::pow(1.0 - ph, 3.0);
    return sq * (1 - shape) + shape * ph * ph;
  }
  struct Tap {
    Ring line[2];
    Allpass ap[4];
    Svf inF[2], fbF[2];
    Smooth smooth;
    bool on = false, active = false;
    double target = 1000, offset = 0, inLevel = 1, inL = 1, inR = 1, fb = 0, fbL = 1, fbR = 1, width = 1, modInc = 0, modDepth = 0,
           modPh = 0, diffuse = 0, gateShape = 0, gatePh = 0, outLevel = 1, outL = 1, outR = 1, toNext = 0;
    double apD[4] = {300, 500, 400, 600};
    int inType = 0, fbType = 0, mode = 0, gateDiv = 0;
    void clear() {
      line[0].clear();
      line[1].clear();
      for (auto& a : ap) a.clear();
      for (auto& f : inF) f.clear();
      for (auto& f : fbF) f.clear();
      gatePh = modPh = 0;
      active = false;
    }
  };
  Tap taps_[kTaps];
};

// ================================================================ Band Delay
// Spectral delay: STFT (1024-point Hann, hop 256), every bin delayed by a
// whole number of frames with its own feedback, pan and level taken from
// curves drawn over 32 log-spaced bands (two banks, morphable). Wet latency
// is one FFT frame (1024 samples); the delay curve compensates for it, so
// the drawn time is the heard time (minimum = one frame).
constexpr int kBdBins = kBdFft / 2 + 1;
constexpr int kBdFrames = 1024;  // frame history (2.7 s at 96 kHz, 5.9 s at 44.1 kHz)

class BandDelayFx final : public Effect {
 public:
  BandDelayFx() {
    fft_.init(kBdFft);
    win_.resize(kBdFft);
    for (int i = 0; i < kBdFft; ++i) win_[static_cast<size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(kTwoPi * i / kBdFft));
    for (int c = 0; c < 2; ++c) {
      inBuf_[c].assign(kBdFft, 0.f);
      outAcc_[c].assign(2 * kBdFft, 0.f);
      hist_[c].assign(static_cast<size_t>(kBdFrames) * kBdBins * 2, 0.f);
    }
    work_.assign(2 * kBdFft, 0.f);
    work2_.assign(2 * kBdFft, 0.f);
    for (auto& d : delay_) d = 0;
  }
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    ChannelAux none;
    processAux(s, none, ctx, L, R, n);
  }
  void processAux(const PluginState& s, const ChannelAux& aux, const ProcessContext& ctx, float* L, float* R, int n) override {
    sr_ = ctx.sampleRate;
    if (SamplePtr sp = aux.sample(0); sp && sp->path == kCurveTag && sp->left.size() * sizeof(float) >= sizeof(BandCurves)) {
      std::memcpy(&cur_, sp->left.data(), sizeof(BandCurves));
    } else {
      if (!defaultsReady_) {
        defaults_ = defaultBandCurves();
        defaultsReady_ = true;
      }
      cur_ = defaults_;
    }
    curves_ = &cur_;
    state_ = &s;
    tempo_ = ctx.tempo;
    const float dry = pv(s, BdDry, 1), wet = pv(s, BdWet, 0.5f);
    const int sat = pi(s, BdSat, 0);
    for (int i = 0; i < n; ++i) {
      float xl = L[i], xr = R[i];
      inBuf_[0][static_cast<size_t>(fill_)] = xl;
      inBuf_[1][static_cast<size_t>(fill_)] = xr;
      // Output: the oldest completed hop of the overlap-add accumulator.
      float yl = outAcc_[0][static_cast<size_t>(outPos_)], yr = outAcc_[1][static_cast<size_t>(outPos_)];
      outAcc_[0][static_cast<size_t>(outPos_)] = 0;
      outAcc_[1][static_cast<size_t>(outPos_)] = 0;
      outPos_ = (outPos_ + 1) % (2 * kBdFft);
      fill_ = (fill_ + 1) % kBdFft;
      if (++hopCount_ == kBdHop) {
        hopCount_ = 0;
        frame();
      }
      if (sat == 1) {
        yl = std::tanh(yl);
        yr = std::tanh(yr);
      } else if (sat == 2) {
        yl = std::clamp(yl * 1.4f, -1.f, 1.f);
        yr = std::clamp(yr * 1.4f, -1.f, 1.f);
      }
      L[i] = flush(xl * dry + yl * wet);
      R[i] = flush(xr * dry + yr * wet);
    }
  }
  void reset() override {
    for (int c = 0; c < 2; ++c) {
      std::fill(inBuf_[c].begin(), inBuf_[c].end(), 0.f);
      std::fill(outAcc_[c].begin(), outAcc_[c].end(), 0.f);
      std::fill(hist_[c].begin(), hist_[c].end(), 0.f);
    }
    fill_ = hopCount_ = histPos_ = 0;
    outPos_ = 0;
    primed_ = false;
  }

  static constexpr const char* kCurveTag = "\x01gl.banddelay.curves";

 private:
  // Curve value at a bin (log-frequency interpolation between points, with morph).
  double curve(int target, double hz, double morph) const {
    double pos = std::log(std::max(hz, 20.0) / 20.0) / std::log(1000.0) * (kBdPoints - 1);
    pos = clampd(pos, 0, kBdPoints - 1);
    int a = static_cast<int>(pos), b = std::min(a + 1, kBdPoints - 1);
    double f = pos - a;
    double va = curves_->v[0][target][a] * (1 - f) + curves_->v[0][target][b] * f;
    double vb = curves_->v[1][target][a] * (1 - f) + curves_->v[1][target][b] * f;
    return va + (vb - va) * morph;
  }
  void frame() {
    const PluginState& s = *state_;
    const double morph = clampd(pv(s, BdMorph), 0, 1);
    double maxT = pv(s, BdMaxTime, 0.8f);
    if (pb(s, BdSync)) maxT = kBdSyncBeats[std::clamp(pi(s, BdSyncTime, 3), 0, 7)] * 60.0 / std::max(1.0, tempo_);
    const double fbG = clampd(pv(s, BdFeedback, 0.6f), 0, 1);
    const double lowCut = pv(s, BdLowCut, 40);
    const double smoothSec = pv(s, BdSmooth, 0.15f);
    const bool keepPitch = pb(s, BdKeepPitch);
    const double hopSec = kBdHop / sr_;
    const double alpha = 1.0 - std::exp(-hopSec / std::max(0.001, smoothSec));
    const double latency = static_cast<double>(kBdFft) / sr_;
    histPos_ = (histPos_ + 1) % kBdFrames;
    // Analysis of both channels.
    for (int c = 0; c < 2; ++c) {
      float* w = (c == 0 ? work_ : work2_).data();
      for (int i = 0; i < kBdFft; ++i) {
        int idx = (fill_ + i) % kBdFft;  // oldest first
        w[2 * i] = inBuf_[c][static_cast<size_t>(idx)] * win_[static_cast<size_t>(i)];
        w[2 * i + 1] = 0;
      }
      fft_.run(w, false);
    }
    const double binHz = sr_ / kBdFft;
    for (int k = 0; k < kBdBins; ++k) {
      double hz = std::max(binHz * 0.5, k * binHz);
      double tSec = curve(BtDelay, hz, morph) * maxT;
      double targetFrames = clampd((tSec - latency) / hopSec, 1.0, kBdFrames - 2.0);
      if (!primed_) delay_[k] = targetFrames;
      if (keepPitch) {
        // Jump in whole frames once the smoothed target moves by a frame (no glide).
        double sm = delayT_[k] + (targetFrames - delayT_[k]) * alpha;
        delayT_[k] = primed_ ? sm : targetFrames;
        if (std::fabs(delayT_[k] - delay_[k]) >= 1.0 || !primed_) delay_[k] = std::round(delayT_[k]);
      } else {
        delay_[k] = primed_ ? delay_[k] + (targetFrames - delay_[k]) * alpha : targetFrames;
        delayT_[k] = delay_[k];
      }
      double fb = std::min(0.98, curve(BtFeedback, hz, morph) * fbG);
      double pan = curve(BtPan, hz, morph) * 2 - 1;
      double lvl = curve(BtLevel, hz, morph);
      double lc = hz < lowCut ? (hz / lowCut) * (hz / lowCut) : 1.0;
      double gl, gr;
      balance(pan, gl, gr);
      double d = delay_[k];
      int d0 = static_cast<int>(d);
      float fr = static_cast<float>(d - d0);
      for (int c = 0; c < 2; ++c) {
        float* hist = hist_[c].data();
        const float* X = (c == 0 ? work_ : work2_).data();
        auto at = [&](int age) -> const float* {
          int slot = ((histPos_ - age) % kBdFrames + kBdFrames) % kBdFrames;
          return hist + (static_cast<size_t>(slot) * kBdBins + static_cast<size_t>(k)) * 2;
        };
        // Y_t = X_t + fb · Y_{t-D}; the output reads Y_{t-D} (delayed).
        const float* y0 = at(std::max(1, d0));
        const float* y1 = at(std::min(kBdFrames - 1, d0 + 1));
        float dr = y0[0] + (y1[0] - y0[0]) * fr, di = y0[1] + (y1[1] - y0[1]) * fr;
        float* cur = hist + (static_cast<size_t>(histPos_) * kBdBins + static_cast<size_t>(k)) * 2;
        cur[0] = flush(static_cast<float>(X[2 * k] + fb * dr));
        cur[1] = flush(static_cast<float>(X[2 * k + 1] + fb * di));
        double g = lvl * lc * (c == 0 ? gl : gr);
        outSpec_[c][2 * k] = static_cast<float>(dr * g);
        outSpec_[c][2 * k + 1] = static_cast<float>(di * g);
      }
    }
    primed_ = true;
    // Synthesis: inverse transform, window, overlap-add.
    const float norm = 1.0f / (kBdFft * 1.5f);  // Hann² at 75% overlap sums to 1.5
    for (int c = 0; c < 2; ++c) {
      float* w = (c == 0 ? work_ : work2_).data();
      for (int k = 0; k < kBdBins; ++k) {
        w[2 * k] = outSpec_[c][2 * k];
        w[2 * k + 1] = outSpec_[c][2 * k + 1];
        if (k > 0 && k < kBdFft / 2) {
          w[2 * (kBdFft - k)] = outSpec_[c][2 * k];
          w[2 * (kBdFft - k) + 1] = -outSpec_[c][2 * k + 1];
        }
      }
      fft_.run(w, true);
      for (int i = 0; i < kBdFft; ++i) {
        size_t o = static_cast<size_t>((outPos_ + i) % (2 * kBdFft));
        outAcc_[c][o] += w[2 * i] * win_[static_cast<size_t>(i)] * norm;
      }
    }
  }

  Fft fft_;
  std::vector<float> win_, inBuf_[2], outAcc_[2], hist_[2], work_, work2_;
  float outSpec_[2][2 * kBdBins] = {};
  double delay_[kBdBins], delayT_[kBdBins] = {};
  int fill_ = 0, hopCount_ = 0, histPos_ = 0, outPos_ = 0;
  bool primed_ = false, defaultsReady_ = false;
  double sr_ = 48000, tempo_ = 120;
  const BandCurves* curves_ = nullptr;
  const PluginState* state_ = nullptr;
  BandCurves defaults_{}, cur_{};
};

}  // namespace gl::packs::spacefx::detail
