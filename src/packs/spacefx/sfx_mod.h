// Gridline — "spacefx" pack: modulation effects (included by spacefx_dsp.cpp only):
// Ensemble, Bucket Chorus, Swirl, Retro Phaser.
#pragma once
#include "sfx_common.h"
#include "sfx_vintage.h"
#include "hyperchorus_ref.h"

namespace gl::packs::spacefx::detail {

// Equal-power dry/wet crossfade.
inline void eqPowerMix(double mix, double& dry, double& wet) {
  mix = clampd(mix, 0, 1);
  dry = std::cos(mix * kPi * 0.5);
  wet = std::sin(mix * kPi * 0.5);
}

// ================================================================== Ensemble
// 4 or 8 modulated delay voices. Each voice has its own delay range, LFO rate
// and seeded start phase; "movement" sweeps the voice through the stereo field
// with equal-power panning and wanders its delay slowly. Wet band-limited by a
// high pass and low pass; equal-power dry/wet.
constexpr int kEnsVoices = 8;
class EnsembleFx final : public Effect {
 public:
  EnsembleFx() : ref_(std::make_unique<hcref::Engine>()) {
    for (auto& v : voice_) v.line.init(static_cast<size_t>(0.075 * kMaxRate));
    reset();
  }
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    const double sr = ctx.sampleRate;
    if (pi(s, EnModel, 0) == 1) {  // J-26 reference engine
      float k[11];
      for (int i = 0; i < 11; ++i) k[i] = static_cast<float>(pv(s, EnK0 + i, 0.5));
      ref_->process(k, sr, L, R, n);
      return;
    }
    const int nv = pi(s, EnLines, 0) == 1 ? 8 : 4;
    const double delay = pv(s, EnDelay, 14) * 0.001 * sr;
    const double fb = pv(s, EnFeedback, 0) * 0.8;
    const double modAmt = pv(s, EnModAmount, 0.4f), modDepth = modAmt * modAmt * 0.6;  // fraction of the delay
    const double modRate = pv(s, EnModRate, 0.7f), moveAmt = pv(s, EnMoveAmount, 0.3f), moveRate = pv(s, EnMoveRate, 0.25f);
    const double phase = pv(s, EnPhase, 0.5f), spread = pv(s, EnSpread, 1);
    double dryG, wetG;
    eqPowerMix(pv(s, EnMix, 0.5f), dryG, wetG);
    for (int c = 0; c < 2; ++c) {  // Biquad::set keeps the filter state
      hp_[c].set(dsp::Biquad::HighPass, sr, pv(s, EnHighPass, 120), 0.707, 0);
      lp_[c].set(dsp::Biquad::LowPass, sr, std::min(static_cast<double>(pv(s, EnLowPass, 16000)), sr * 0.45), 0.707, 0);
    }
    const double outScale = 1.1 / std::sqrt(static_cast<double>(nv));
    for (int v = 0; v < nv; ++v) {
      Voice& vc = voice_[v];
      double u = nv > 1 ? static_cast<double>(v) / (nv - 1) : 0.5;
      vc.base = delay * (0.55 + 0.9 * u);
      vc.inc = modRate * (0.82 + 0.36 * ((v * 5) % nv) / std::max(1, nv - 1)) / sr;
      vc.moveInc = moveRate * (0.7 + 0.6 * ((v * 3) % nv) / std::max(1, nv - 1)) / sr;
      vc.pan = (u - 0.5) * spread;
      vc.phOff = phase * static_cast<double>(v) / nv;
    }
    for (int i = 0; i < n; ++i) {
      double xl = L[i], xr = R[i];
      double wl = 0, wr = 0;
      for (int v = 0; v < nv; ++v) {
        Voice& vc = voice_[v];
        double lfo = std::sin(kTwoPi * (vc.ph + vc.phOff));
        vc.ph += vc.inc;
        if (vc.ph >= 1) vc.ph -= 1;
        double wander = vc.move.next(vc.moveInc);  // -1..1
        double d = vc.base * (1.0 + modDepth * lfo + 0.12 * moveAmt * wander);
        double y = vc.line.read(std::max(2.0, d));
        double in = (v & 1) ? xr : xl;
        vc.line.push(flush(static_cast<float>(in + y * fb)));
        double p = clampd(0.5 + vc.pan * 0.5 + moveAmt * 0.499 * std::sin(kTwoPi * vc.movePh), 0.0, 1.0);
        vc.movePh += vc.moveInc * 0.5;
        if (vc.movePh >= 1) vc.movePh -= 1;
        wl += y * std::sqrt(1.0 - p);
        wr += y * std::sqrt(p);
      }
      wl = lp_[0].process(hp_[0].process(wl * outScale));
      wr = lp_[1].process(hp_[1].process(wr * outScale));
      L[i] = flush(static_cast<float>(xl * dryG + wl * wetG));
      R[i] = flush(static_cast<float>(xr * dryG + wr * wetG));
    }
  }
  void reset() override {
    if (ref_) ref_->reset();
    dsp::Noise r;
    r.seed(0xC0FFEEu);
    for (int v = 0; v < kEnsVoices; ++v) {
      voice_[v].line.clear();
      voice_[v].ph = 0.5 + 0.5 * r.next();
      voice_[v].movePh = 0.5 + 0.5 * r.next();
      voice_[v].move = Wander{};
      voice_[v].move.rng.seed(0x1234u + 77u * static_cast<uint32_t>(v));
    }
    for (auto& f : hp_) f.clear();
    for (auto& f : lp_) f.clear();
  }

 private:
  struct Voice {
    Ring line;
    Wander move;
    double ph = 0, inc = 0, base = 0, pan = 0, phOff = 0, moveInc = 0, movePh = 0;
  };
  std::unique_ptr<hcref::Engine> ref_;  // ≈ 0.5 MB of lines
  Voice voice_[kEnsVoices];
  dsp::Biquad hp_[2], lp_[2];
};

// ============================================================= Bucket Chorus
// Two bucket-brigade style lines (L/R) swept by one LFO with an L/R phase
// offset. Fixed modes reproduce the classic slow / faster / fast-shallow
// settings; Custom exposes the sweep range and speed. Band-limited wet
// (anti-alias style low pass before and after the line), gentle compander
// saturation, optional hiss with an input-following gate. BcModel = 1 runs
// the J-1 reference model (BbdRef, sfx_vintage.h).
class BbdChorusFx final : public Effect {
 public:
  BbdChorusFx() {
    line_[0].init(static_cast<size_t>(0.06 * kMaxRate));
    line_[1].init(static_cast<size_t>(0.06 * kMaxRate));
    ref_.init();
    reset();
  }
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    const bool refModel = pi(s, BcModel, 0) == 1;
    if (refModel != lastRef_) {
      reset();
      lastRef_ = refModel;
    }
    if (refModel) {
      ref_.process(s, ctx, L, R, n);
      return;
    }
    const double sr = ctx.sampleRate;
    BbdModeSettings m = bbdSettings(s, ctx.tempo);
    const double lo = m.minMs * 0.001 * sr, hi = m.maxMs * 0.001 * sr;
    const double inc = m.rateHz / sr;
    const double lr = pv(s, BcPhaseLR, 0.5f), start = pv(s, BcStartPhase, 0);
    const double fb = pv(s, BcFeedback, 0);
    const bool monoIn = pb(s, BcMonoIn), invert = pb(s, BcInvertWet), gate = pb(s, BcNoiseGate);
    const double hiss = pv(s, BcHiss, 0.1f) * 0.004;
    const double gain = dbg(pv(s, BcGain, 0)) * (invert ? -1.0 : 1.0);
    const double mix = pv(s, BcMix, 0.5f);
    const double dryG = std::min(1.0, 2.0 * (1.0 - mix)), wetG = std::min(1.0, 2.0 * mix);
    const double bw = std::min(sr * 0.45, 9000.0);
    for (int c = 0; c < 2; ++c) {
      setCoef(pre_[c], dsp::Biquad::LowPass, sr, bw, 0.7);
      setCoef(post_[c], dsp::Biquad::LowPass, sr, bw * 1.1, 0.6);
      setCoef(hp_[c], dsp::Biquad::HighPass, sr, pv(s, BcHighPass, 60), 0.707);
    }
    hissLp_.setHz(sr, 5000);
    const double gateRel = std::exp(-1.0 / (0.15 * sr));
    if (!started_) {
      ph_ = start;
      started_ = true;
    }
    for (int i = 0; i < n; ++i) {
      double xl = L[i], xr = R[i];
      double inL = monoIn ? 0.5 * (xl + xr) : xl, inR = monoIn ? 0.5 * (xl + xr) : xr;
      double lvl = std::max(std::fabs(xl), std::fabs(xr));
      env_ = lvl > env_ ? lvl : env_ * gateRel;
      double nz = hissLp_.lp(noise_.next()) * hiss * (gate ? clampd(env_ * 300.0, 0.0, 1.0) : 1.0);
      double lfoL = m.wave == 1 ? lfoTri(ph_) : lfoSine(ph_);
      double lfoR = m.wave == 1 ? lfoTri(ph_ + lr) : lfoSine(ph_ + lr);
      ph_ += inc;
      if (ph_ >= 1) ph_ -= 1;
      double dl = lo + (hi - lo) * (0.5 + 0.5 * lfoL), dr = lo + (hi - lo) * (0.5 + 0.5 * lfoR);
      double yl = post_[0].process(line_[0].read(std::max(1.5, dl))), yr = post_[1].process(line_[1].read(std::max(1.5, dr)));
      line_[0].push(flush(static_cast<float>(pre_[0].process(compand(inL + yl * fb) + nz))));
      line_[1].push(flush(static_cast<float>(pre_[1].process(compand(inR + yr * fb) + nz))));
      double wl = hp_[0].process(yl) * gain, wr = hp_[1].process(yr) * gain;
      L[i] = flush(static_cast<float>(xl * dryG + wl * wetG));
      R[i] = flush(static_cast<float>(xr * dryG + wr * wetG));
    }
  }
  void reset() override {
    line_[0].clear();
    line_[1].clear();
    for (int c = 0; c < 2; ++c) {
      pre_[c].clear();
      post_[c].clear();
      hp_[c].clear();
    }
    hissLp_.clear();
    noise_.seed(0xB0Bu);
    env_ = 0;
    started_ = false;
    ph_ = 0;
    ref_.reset();
  }

 private:
  static double compand(double x) { return std::fabs(x) < 0.5 ? x : (x > 0 ? 0.5 + 0.5 * std::tanh((x - 0.5) * 2) : -0.5 + 0.5 * std::tanh((x + 0.5) * 2)); }
  static void setCoef(dsp::Biquad& b, dsp::Biquad::Type t, double sr, double hz, double q) {
    double x1 = b.x1, x2 = b.x2, y1 = b.y1, y2 = b.y2;
    b.set(t, sr, hz, q, 0);
    b.x1 = x1, b.x2 = x2, b.y1 = y1, b.y2 = y2;
  }
  Ring line_[2];
  dsp::Biquad pre_[2], post_[2], hp_[2];
  OnePole hissLp_;
  dsp::Noise noise_;
  double ph_ = 0, env_ = 0;
  bool started_ = false, lastRef_ = false;
  BbdRef ref_;
};

// ===================================================================== Swirl
// Up to nine voices reading one line per side at LFO-swept delays spread in
// phase and in the stereo field; "cross" feeds each side's wet into the other
// side's line (signed), giving flanger-like resonances. Bipolar dry/wet levels.
class SwirlFx final : public Effect {
 public:
  SwirlFx() {
    line_[0].init(static_cast<size_t>(0.16 * kMaxRate));  // reference engine: up to ~150 ms
    line_[1].init(static_cast<size_t>(0.16 * kMaxRate));
    reset();
  }
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    const double sr = ctx.sampleRate;
    if (pv(s, SwModel, 1) >= 0.5f) {
      reference(s, sr, L, R, n);
      return;
    }
    const int order = std::clamp(static_cast<int>(std::lround(pv(s, SwOrder, 4))), 1, 9);
    const double depth = pv(s, SwDepth, 0.5f), inc = pv(s, SwSpeed, 0.4f) / sr;
    const double base = pv(s, SwDelay, 3) * 0.001 * sr, spread = pv(s, SwSpread, 1), cross = pv(s, SwCross, 0);
    const double dry = pv(s, SwDry, 1), wet = pv(s, SwWet, 0.7f);
    const double norm = 1.0 / std::sqrt(static_cast<double>(order));
    double gl[9], gr[9], off[9], rate[9];
    for (int v = 0; v < order; ++v) {
      double u = order > 1 ? static_cast<double>(v) / (order - 1) : 0.5;
      balance((u * 2 - 1) * spread, gl[v], gr[v]);
      off[v] = static_cast<double>(v) / order;
      rate[v] = 1.0 + 0.045 * v;
    }
    for (int i = 0; i < n; ++i) {
      double xl = L[i], xr = R[i];
      double wl = 0, wr = 0;
      for (int v = 0; v < order; ++v) {
        double ph = ph_ * rate[v] + off[v];
        double lfo = 0.5 + 0.5 * std::sin(kTwoPi * ph);
        double d = 1.5 + base * (1.0 + depth * 1.6 * lfo);
        double lfoR = 0.5 + 0.5 * std::sin(kTwoPi * (ph + 0.25 * spread));
        double dR = 1.5 + base * (1.0 + depth * 1.6 * lfoR);
        wl += line_[0].read(d) * gl[v];
        wr += line_[1].read(dR) * gr[v];
      }
      wl *= norm;
      wr *= norm;
      line_[0].push(flush(static_cast<float>(soft(xl + wr * cross))));
      line_[1].push(flush(static_cast<float>(soft(xr + wl * cross))));
      ph_ += inc;
      if (ph_ >= 1000.0) ph_ -= 1000.0;
      L[i] = flush(static_cast<float>(xl * dry + wl * wet));
      R[i] = flush(static_cast<float>(xr * dry + wr * wet));
    }
  }
  void reset() override {
    line_[0].clear();
    line_[1].clear();
    ph_ = 0;
    key_ = -1;
    lastN_ = 0;
    started_ = false;
  }

 private:
  static double soft(double x) { return std::fabs(x) < 1.0 ? x : std::tanh(x); }
  static double lvl(double x) {  // signed level curve 0.1 (11^|x| - 1)
    double a = std::fabs(x);
    double g = a >= 1.0 ? 1.0 : 0.1 * (std::pow(11.0, a) - 1.0);
    return x < 0 ? -g : g;
  }
  // Reference engine (J-35 study j35_flangus.md): 2 x order voices (a set per side, the second
  // set mirrored and half a cycle later), each a delay swept by the reference's contour (a
  // triangle bent by t(2 - |t|), starting at the minimum delay) on an int32 phase, read with
  // linear interpolation; cross = each tap also feeds the other output (no feedback).
  void reference(const PluginState& s, double sr, float* L, float* R, int n) {
    static const double kRatio[20] = {0.967, 1.039, 1.021, 0.1013, 0.977, 1.033, 0.997, 0.971, 1.061, 0.947,
                                      1.031, 0.1009, 1.063, 0.953, 0.991, 1.049, 0.983, 1.051, 1.019, 1.069};
    const int N = std::clamp(static_cast<int>(std::lround(pv(s, SwOrder, 4))), 1, 9);
    const double d = std::clamp(static_cast<double>(pv(s, SwDepthK, 0.5f)), 0.0, 1.0), sp = std::clamp(static_cast<double>(pv(s, SwSpeedK, 0.75f)), 0.0, 1.0);
    const double t = std::clamp(static_cast<double>(pv(s, SwDelayK, 0.0625f)), 0.0, 1.0), spr = std::clamp(static_cast<double>(pv(s, SwSpreadK, 1)), 0.0, 1.0);
    const double c = std::clamp(static_cast<double>(pv(s, SwCrossK, -0.25f)), -1.0, 1.0);
    const double dryT = lvl(pv(s, SwDryK, 0.75f)), wetT = lvl(pv(s, SwWetK, 0.75f));
    const double D = 109.25 * (std::pow(11.0, d) - 1.0), T = 441.0 * (std::pow(11.0, t) - 1.0);
    const double S = (std::pow(1001.0, spr) - 1.0) / 1000.0, P = 50000.0 + 75000.0 * (std::pow(101.0, 1.0 - sp) - 1.0);
    const double K = 2147483648.0 / P * 500.0, k = static_cast<float>(44100.0 / sr);
    auto lerp = [](double a, double b, double w) { return (1 - w) * b + w * a; };
    double sumG = 0, maxDelay = 0;
    uint32_t phase0[18];
    for (int i = 0; i < 2 * N; ++i) {
      const bool mirror = i >= N;
      const int j = mirror ? 2 * N - 1 - i : i;
      const double u = 1.0 - static_cast<double>(j) / N;
      const double r = kRatio[i % 20], rr = lerp(r * r, 1.0, S);
      const double A = D * u + 10.0;
      incI_[i] = static_cast<uint32_t>(static_cast<int32_t>(std::lround(k * lerp(rr * K / A, K / (D + 10.0), S))));
      amp_[i] = A / k;
      base_[i] = std::max(lerp(T * u * rr, T, S) / k, 8.0) + amp_[i];
      other_[i] = c * u;
      side_[i] = mirror ? 1 : 0;
      sumG += 1.0 + std::fabs(other_[i]);
      maxDelay = std::max(maxDelay, base_[i] + amp_[i]);
      phase0[i] = (mirror ? 0x80000000u : 0u) + (static_cast<uint32_t>(std::lround(u * S * 2147483648.0 / 256.0)) << 8);
    }
    // The reference re-allocates (clears) its line and restarts the phases when the line
    // length or the spread changes (its length also counts the host block: left out here).
    const long key = std::lround(maxDelay) * 4096 + std::lround(spr * 4095);
    if (key != key_ || N != lastN_) {
      if (key != key_) {
        line_[0].clear(), line_[1].clear();
        for (int i = 0; i < 2 * N; ++i) p_[i] = phase0[i];
      } else {
        for (int i = lastN_ * 2; i < 2 * N; ++i) p_[i] = 0;
      }
      key_ = key, lastN_ = N;
    }
    if (!started_) dry_ = dryT, wet_ = wetT, started_ = true;
    const double norm = 1.0 - (std::pow(1001.0, 1.0 - 1.0 / std::max(1.0, sumG)) - 1.0) / 1000.0;
    const double maxStep = 0.001 * 44100.0 / sr;
    auto slew = [maxStep](double& v, double tgt) { v += std::clamp(tgt - v, -maxStep, maxStep); };
    for (int i = 0; i < n; ++i) {
      double xl = L[i], xr = R[i];
      line_[0].push(flush(static_cast<float>(xl)));
      line_[1].push(flush(static_cast<float>(xr)));
      double wl = 0, wr = 0;
      for (int v = 0; v < 2 * N; ++v) {
        const double tau = 2.0 * std::fabs(static_cast<double>(static_cast<int32_t>(p_[v]))) / 2147483648.0 - 1.0;
        const double pos = base_[v] + amp_[v] * (tau * (2.0 - std::fabs(tau)));
        const double fl = std::floor(pos);
        const size_t di = static_cast<size_t>(std::max(0.0, fl));
        const double fr = pos - fl;
        const double a = line_[side_[v]].at(di + 1), b = line_[side_[v]].at(di + 2);
        const double tap = (a + fr * (b - a)) * norm;
        wl += side_[v] ? other_[v] * tap : tap;
        wr += side_[v] ? tap : other_[v] * tap;
        p_[v] += incI_[v];
      }
      slew(dry_, dryT), slew(wet_, wetT);
      L[i] = flush(static_cast<float>(xl * dry_ + wl * wet_));
      R[i] = flush(static_cast<float>(xr * dry_ + wr * wet_));
    }
  }
  Ring line_[2];
  double ph_ = 0;
  double amp_[18] = {}, base_[18] = {}, other_[18] = {}, dry_ = 0, wet_ = 0;
  uint32_t incI_[18] = {}, p_[18] = {};
  int side_[18] = {}, lastN_ = 0;
  bool started_ = false;
  long key_ = -1;
};

// ============================================================== Retro Phaser
// Chain of first-order allpass stages swept exponentially between two
// frequencies by an LFO (six shapes with tension) or held at a manual
// frequency; feedback (optionally inverted, high-passed) around the chain,
// input drive and noise, low-shelf "color" on the wet, and a filtered stereo
// echo after the phaser. VpModel = 1 runs the J-1 reference model
// (PhaserRef, sfx_vintage.h).
class RetroPhaserFx final : public Effect {
  static int measureOs(int stages) {
    detail::HalfbandCoefs hc;
    hc.init();
    detail::Halfband2x up[3], dn[3];
    for (int k = 0; k < 3; ++k) up[k].c = dn[k].c = &hc;
    int best = 0;
    double peak = 0;
    for (int i = 0; i < 256; ++i) {
      double buf[8], tmp[8];
      buf[0] = i == 0 ? 1.0 : 0.0;
      int cnt = 1;
      for (int st = 0; st < stages; ++st) {
        for (int k = 0; k < cnt; ++k) up[st].up(buf[k], tmp[2 * k], tmp[2 * k + 1]);
        cnt *= 2;
        std::memcpy(buf, tmp, sizeof(double) * static_cast<size_t>(cnt));
      }
      for (int st = stages - 1; st >= 0; --st) {
        cnt /= 2;
        for (int k = 0; k < cnt; ++k) buf[k] = dn[st].down(buf[2 * k], buf[2 * k + 1]);
      }
      if (std::fabs(buf[0]) > peak) peak = std::fabs(buf[0]), best = i;
    }
    return best;
  }

 public:
  RetroPhaserFx() {
    echo_[0].init(static_cast<size_t>(1.6 * kMaxRate));
    echo_[1].init(static_cast<size_t>(1.6 * kMaxRate));
    ref_.init();
    reset();
  }
  // Reference model: the whole output passes the 4× / 8× half-band
  // oversampler; its delay (measured once on an impulse) is the latency.
  int latencyFor(const PluginState& s, double) const override {
    if (pi(s, VpModel, 0) != 1) return 0;
    static const int lat[2] = {measureOs(2), measureOs(3)};
    return lat[pb(s, VpHq) ? 1 : 0];
  }
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    const bool refModel = pi(s, VpModel, 0) == 1;
    if (refModel != lastRef_) {
      reset();
      lastRef_ = refModel;
    }
    if (refModel) {
      ref_.process(s, ctx, L, R, n);
      return;
    }
    const double sr = ctx.sampleRate;
    const int stages = kPhaserStages[std::clamp(pi(s, VpStages, 0), 0, 4)];
    const double fmin = pv(s, VpFreqMin, 180), fmax = std::max(fmin, static_cast<double>(pv(s, VpFreqMax, 2800)));
    const bool manual = pb(s, VpManual);
    const double fman = pv(s, VpManualFreq, 700);
    const double fb = pv(s, VpFeedback, 0.35f) * (pb(s, VpInvertFb) ? -1.0 : 1.0);
    const int wave = pi(s, VpWave, 0);
    double rate = pv(s, VpRate, 0.35f);
    if (pb(s, VpSync)) rate = std::max(1.0, ctx.tempo) / 60.0 / kLfoSyncBeats[std::clamp(pi(s, VpSyncRate, 6), 0, 8)];
    const double tension = pv(s, VpTension, 0.5f), lr = pv(s, VpPhaseLR, 0.25f), start = pv(s, VpStartPhase, 0);
    const double drive = dbg(pv(s, VpDrive, 0)), noise = pv(s, VpNoise, 0) * 0.02;
    const bool gate = pb(s, VpNoiseGate), mono = pb(s, VpMonoIn);
    const double mix = pv(s, VpMix, 0.5f);
    for (int c = 0; c < 2; ++c) {
      keepSet(color_[c], dsp::Biquad::LowShelf, sr, pv(s, VpColorFreq, 200), 0.7, pv(s, VpColorGain, 0));
      keepSet(inHp_[c], dsp::Biquad::HighPass, sr, pv(s, VpInputHp, 10), 0.707, 0);
      keepSet(fbHp_[c], dsp::Biquad::HighPass, sr, pv(s, VpFbHp, 10), 0.707, 0);
    }
    // Echo.
    const bool echoOn = pb(s, VpDelayOn);
    double et = pv(s, VpDelayTime, 280) * 0.001;
    if (pb(s, VpDelaySync)) et = kSyncBeats[std::clamp(pi(s, VpDelaySyncTime, 4), 0, 8)] * 60.0 / std::max(1.0, ctx.tempo);
    const double eStereo = pv(s, VpDelayStereo, 0.5f);
    const double edL = clampd(et * sr, 2.0, 1.5 * sr), edR = clampd(et * sr * (1.0 + 0.25 * eStereo), 2.0, 1.5 * sr);
    const double efb = pv(s, VpDelayFb, 0.35f), emix = pv(s, VpDelayMix, 0.3f);
    for (int c = 0; c < 2; ++c) {
      keepSet(eLp_[c], dsp::Biquad::LowPass, sr, pv(s, VpDelayLp, 7000), 0.707, 0);
      keepSet(eHp_[c], dsp::Biquad::HighPass, sr, pv(s, VpDelayHp, 100), 0.707, 0);
    }
    const double inc = rate / sr;
    const double gateRel = std::exp(-1.0 / (0.2 * sr));
    if (!started_) {
      ph_ = start;
      started_ = true;
    }
    for (int i = 0; i < n; ++i) {
      double x[2] = {L[i], R[i]};
      if (mono) x[0] = x[1] = 0.5 * (x[0] + x[1]);
      double lvl = std::max(std::fabs(x[0]), std::fabs(x[1]));
      env_ = lvl > env_ ? lvl : env_ * gateRel;
      double nz = noise_.next() * noise * (gate ? clampd(env_ * 300.0, 0.0, 1.0) : 1.0);
      double out[2];
      for (int c = 0; c < 2; ++c) {
        double v = manual ? 0.0 : phaserLfo(wave, ph_ + start + (c ? lr : 0.0), tension);
        double f = manual ? fman : fmin * std::pow(fmax / fmin, v);
        double t = std::tan(kPi * clampd(f, 10.0, sr * 0.45) / sr);
        double a = (t - 1.0) / (t + 1.0);
        double in = inHp_[c].process(x[c]) + nz;
        if (drive > 1.0001) in = std::tanh(in * drive) / drive;
        double y = in + fbHp_[c].process(last_[c]) * fb;
        for (int k = 0; k < stages; ++k) {
          double o = a * y + z_[c][k];
          z_[c][k] = flushd(y - a * o);
          y = o;
        }
        last_[c] = flushd(y);
        double wet = color_[c].process(y);
        out[c] = x[c] * (1.0 - mix) + wet * mix;
      }
      ph_ += inc;
      if (ph_ >= 1e6) ph_ -= 1e6;
      if (echoOn) {
        double yl = echo_[0].read(edL), yr = echo_[1].read(edR);
        double fl = eLp_[0].process(eHp_[0].process(yl)), fr = eLp_[1].process(eHp_[1].process(yr));
        // Stereo control blends straight feedback into cross (ping-pong) feedback.
        double wl = out[0] + efb * (fl * (1 - eStereo) + fr * eStereo);
        double wr = out[1] + efb * (fr * (1 - eStereo) + fl * eStereo);
        echo_[0].push(flush(static_cast<float>(wl)));
        echo_[1].push(flush(static_cast<float>(wr)));
        out[0] += yl * emix;
        out[1] += yr * emix;
      }
      L[i] = flush(static_cast<float>(out[0]));
      R[i] = flush(static_cast<float>(out[1]));
    }
  }
  void reset() override {
    std::memset(z_, 0, sizeof z_);
    last_[0] = last_[1] = 0;
    for (int c = 0; c < 2; ++c) {
      color_[c].clear();
      inHp_[c].clear();
      fbHp_[c].clear();
      eLp_[c].clear();
      eHp_[c].clear();
      echo_[c].clear();
    }
    noise_.seed(0xFA5Eu);
    env_ = 0;
    ph_ = 0;
    started_ = false;
    ref_.reset();
  }

 private:
  static void keepSet(dsp::Biquad& b, dsp::Biquad::Type t, double sr, double hz, double q, double g) {
    double x1 = b.x1, x2 = b.x2, y1 = b.y1, y2 = b.y2;
    b.set(t, sr, hz, q, g);
    b.x1 = x1, b.x2 = x2, b.y1 = y1, b.y2 = y2;
  }
  double z_[2][12] = {};
  double last_[2] = {};
  dsp::Biquad color_[2], inHp_[2], fbHp_[2], eLp_[2], eHp_[2];
  Ring echo_[2];
  dsp::Noise noise_;
  double ph_ = 0, env_ = 0;
  bool started_ = false, lastRef_ = false;
  PhaserRef ref_;
};

}  // namespace gl::packs::spacefx::detail
