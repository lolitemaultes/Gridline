// Gridline — Octa Filter reference engine (J-42, OfModel = 1).
// Specs: analysis/demos/specs/j35_lovephilter.md + scratch j42 articulator notes
// (own wording). Differences to the legacy engine: control ticks of 64 (HQ 8)
// samples at 44.1 kHz scaled to the internal rate; targets built from rest
// value ⊕ maps (nearest 1025-point tables) ⊕ pattern envelope (song beats,
// loop end jumps back to loop start + 1) ⊕ LFO (32-bit phase triangle /
// pulse shaper × amplitude envelope y − 0.5); filters per octafilter_ref.h;
// per-sample ramps (SVF, 2-pole, pan / volume, shaper mix) with a slew cap;
// constant-power pan with fold; 16-bit shaper tables; cascaded 2x IIR
// half-band oversampling; DC tracker after the down-sampler.
#pragma once
#include <cstdint>
#include <memory>
#include "../../engine/Effects.h"
#include "creative.h"
#include "octafilter_ref.h"

namespace gl::packs::creative::ref {

inline double volLaw(double x) { return volCurve(x); }

// Value of an envelope at position b (pattern: beats) with FL's loop rules; off = value offset.
inline double envAt(const OEnv& e, double b, int ls, int le, double off) {
  const int n = e.n;
  if (n <= 0) return 0;
  const int lend = le >= 0 ? le : (ls >= 0 ? n - 1 : -1);
  bool wrapped = false;
  if (ls >= 0 && lend > ls && lend < n) {
    const double TL = e.x[ls], TE = e.x[lend], len = TE - TL;
    if (len > 0 && b >= TE) b = TL + std::fmod(b - TL, len), wrapped = true;
    else if (len <= 0 && b >= TE) return e.y[lend] - off;
  } else if (lend >= 0 && lend < n && b >= e.x[lend]) {
    return e.y[lend] - off;
  }
  if (b < e.x[0]) return e.y[0] - off;
  const int i = e.segmentAt(b);
  if (i >= n) return e.y[n - 1] - off;
  const double a = wrapped && i - 1 == ls ? e.y[lend] : e.y[i - 1];
  const double w = e.x[i] - e.x[i - 1];
  if (w <= 0) return e.y[i] - off;
  const double f = (b - e.x[i - 1]) / w;
  const double r = e.mode[i] == menv::Smooth ? menv::shape(menv::Smooth, e.c[i], f, e.tanS[i], e.tanE[i]) : menv::shape(e.mode[i], e.c[i], f);
  return a + (e.y[i] - a) * r - off;
}

// LFO phase shaper (tension / skew / pulse knobs raw).
struct LfoShape {
  uint32_t E = 0xFFFFFFFFu, M = 0x80000000u, R = 0x80000000u, start = 0xC0000000u;
  float slopeR = 0, slopeF = 0, k = 0, L = 0, c = 0;
  void setup(double T, double K, double W, uint32_t phase) {
    const float a = static_cast<float>(0.05 * (std::pow(21.0, std::fabs(T) / 128.0) - 1.0));
    c = static_cast<float>((T < 0 ? -1.0 : T > 0 ? 1.0 : 0.0) * (std::exp(static_cast<double>(a) * std::log(1e18)) - 1.0));
    k = c != 0 ? 1.f / std::fabs(c) : 0.f;
    L = static_cast<float>(std::log(std::fabs(static_cast<double>(c)) + 1.0));
    const float s = static_cast<float>(K / 256.0), w = std::clamp(static_cast<float>(W / 128.0), -1.0f, 1.0f);
    const double two32 = 4294967296.0;
    // J-46: full pulse width (w = 1) gives -1 here; converting a negative
    // double to uint32 is UB. The value wraps modulo 2^32 (0xFFFFFFFF: the
    // whole cycle, what the conversion produced on x86 before).
    E = w > 0 ? static_cast<uint32_t>(static_cast<int64_t>(std::nearbyint(static_cast<double>(static_cast<float>((1 - w) * two32)) - 1.0)))
              : 0xFFFFFFFFu;
    double m = static_cast<double>(static_cast<float>(((1 - w) * 0.5f - (1 - std::fabs(w)) * s) * static_cast<float>(two32)));
    m = std::clamp(m, 1.0, static_cast<double>(E) - 1.0);
    M = static_cast<uint32_t>(std::nearbyint(m));
    R = w < 0 ? static_cast<uint32_t>(std::max(1.0, std::nearbyint(static_cast<double>(w) * two32 + M))) : M;
    slopeR = static_cast<float>(2.0 / R);
    slopeF = static_cast<float>(2.0 / (static_cast<double>(E) - M));
    start = M + 1u + ((E - M) >> 1) + phase;
  }
  float curve(float u) const {  // C(c, u), u in 0..1
    if (c == 0) return u;
    if (c > 0) return 1.f - k * (std::exp(L * (1.f - u)) - 1.f);
    return k * (std::exp(L * u) - 1.f);
  }
  float at(uint32_t ph) const {
    float u;
    if (ph > E) return 1.f;
    if (ph < R) u = 1.f - static_cast<float>(ph) * slopeR;
    else if (ph < M) return -1.f;
    else u = static_cast<float>(ph - M) * slopeF - 1.f;
    return u < 0 ? -curve(-u) : curve(u);
  }
};

class RefEngine {
 public:
  RefEngine() : tabs_(std::make_unique<Tabs>()) {}

  // Rebuild derived tables after the envelope bank changed (no allocation).
  void rebuild(const OctaBank& bk, const PluginState& s) {
    Tabs& T = *tabs_;
    for (int u = 0; u < kUnits; ++u) {
      for (int t = 0; t < kTargets; ++t)
        for (int m = 0; m < 3; ++m) {
          const OEnv* e = bk.env(u, t, KMapX + m);
          hasMap_[u][t][m] = e != nullptr;
          if (!e) continue;
          const double off = additiveTarget(t) ? 0.5 : 0.0;
          for (int i = 0; i <= 1024; ++i) T.map[u][t][m][i] = static_cast<float>(e->eval(i / 1024.0) - off);
        }
      // Shaper table (16-bit): unipolar 16384 entries × 32767, bipolar 32768 × 65535.
      const bool bip = P(s, unitParam(u, UWsBipolar)) >= 0.5f;
      OEnv tmp;
      const OEnv* e = bk.wsCurve(u);
      if (!e) defaultWs(tmp), e = &tmp;
      const int N = bip ? 32768 : 16384, M = N - 1;
      const double sc = bip ? 65535.0 : 32767.0;
      for (int i = 0; i <= M; ++i) {
        double y = e->eval(static_cast<double>(i) / M) - (bip ? 0.5 : 0.0);
        T.ws[u][i + 1] = static_cast<int16_t>(std::clamp(std::nearbyint(y * sc), -32768.0, 32767.0));
      }
      T.ws[u][0] = T.ws[u][1];
      T.ws[u][N + 1] = T.ws[u][N];
      wsN_[u] = N;
    }
    for (int u = 0; u < kUnits; ++u)
      for (int t = 0; t < kTargets; ++t) {
        const double sp = P(s, lfoParam(u, t, LSpeed), 40000.f / 65536.f);
        lfo_[u][t].setup(P(s, lfoParam(u, t, LTension)) * 128.0, P(s, lfoParam(u, t, LSkew)) * 128.0, P(s, lfoParam(u, t, LPulse)) * 128.0,
                         static_cast<uint32_t>(static_cast<int64_t>(std::llround(P(s, lfoParam(u, t, LPhase)) * 4294967296.0)) & 0xFFFFFFFFll));
        lfoRate_[u][t] = static_cast<float>(std::pow(10001.0, static_cast<double>(static_cast<float>(sp))) - 1.0);
      }
    lfoKey_ = lfoKey(s);
  }
  static double lfoKey(const PluginState& s) {
    double h = 0;
    for (int i = LfoBase; i < OfCount; ++i) h = h * 1.000001 + P(s, i) * (i % 97 + 1);
    return h;
  }
  double lfoKey_ = -1;

  void reset() {
    for (auto& u : z_)
      for (auto& st : u)
        for (auto& c : st) c = StageS{};
    for (auto& h : up_)
      for (auto& c : h) c = Halfband{};
    for (auto& h : dn_)
      for (auto& c : h) c = Halfband{};
    dcS_[0] = dcS_[1] = dcW_[0] = dcW_[1] = 0;
    fol_ = 0, folPeak_ = 0;
    tickPos_ = 0;
    primed_ = false;
    freeBeat_ = 0;
  }

  void process(const OctaBank& bk, const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) {
    const int os = std::clamp(static_cast<int>(std::lround(P(s, OfOversample))), 0, 4);
    if (os != os_ || ctx.sampleRate != sr_) {
      os_ = os, sr_ = ctx.sampleRate;
      reset();
    }
    const int fac = 1 << os_;
    srInt_ = sr_ * fac;
    const int N = P(s, OfHqEnvelopes) >= 0.5f ? 8 : 64;
    tick_ = std::max(4, 4 * static_cast<int>(std::nearbyint(srInt_ * N / 44100.0 / 4.0)));
    const double spbInt = 60.0 / std::max(1.0, ctx.tempo) * srInt_;  // internal samples per beat
    for (int i = 0; i < n; ++i) {
      // Song position (beats) at this host sample.
      const double beat = ctx.playing ? ctx.tick / std::max(1, ctx.ppq) + i * fac / spbInt : freeBeat_;
      if (!ctx.playing) freeBeat_ += fac / spbInt;
      else freeBeat_ = beat;
      double xs[16][2];
      xs[0][0] = L[i], xs[0][1] = R[i];
      int cnt = 1;
      for (int st = 0; st < os_; ++st) {  // up: each stage doubles the count (in time order)
        double tmp[16][2];
        for (int k = 0; k < cnt; ++k) {
          double a0, a1, b0, b1;
          up_[st][0].up(xs[k][0], a0, a1);
          up_[st][1].up(xs[k][1], b0, b1);
          tmp[2 * k][0] = static_cast<float>(a0), tmp[2 * k + 1][0] = static_cast<float>(a1);
          tmp[2 * k][1] = static_cast<float>(b0), tmp[2 * k + 1][1] = static_cast<float>(b1);
        }
        cnt *= 2;
        for (int k = 0; k < cnt; ++k) xs[k][0] = tmp[k][0], xs[k][1] = tmp[k][1];
      }
      for (int k = 0; k < cnt; ++k) {
        if (tickPos_ == 0) tick(bk, s, ctx, beat + static_cast<double>(k) / spbInt);
        sample(s, xs[k][0], xs[k][1]);
        tickPos_ = tickPos_ + 1 >= tick_ ? 0 : tickPos_ + 1;
      }
      for (int st = os_ - 1; st >= 0; --st) {
        cnt /= 2;
        for (int k = 0; k < cnt; ++k) {
          xs[k][0] = static_cast<float>(dn_[st][0].down(xs[2 * k][0], xs[2 * k + 1][0]));
          xs[k][1] = static_cast<float>(dn_[st][1].down(xs[2 * k][1], xs[2 * k + 1][1]));
        }
      }
      double ol = xs[0][0], orr = xs[0][1];
      if (P(s, OfCenter) >= 0.5f) {
        const double a = static_cast<float>(1.0070235 / sr_), b = 0.96f;
        double* io[2] = {&ol, &orr};
        for (int c = 0; c < 2; ++c) {
          dcW_[c] += (*io[c] - dcS_[c]) * a;
          dcS_[c] += dcW_[c];
          dcW_[c] *= b;
          *io[c] = *io[c] - dcS_[c];
        }
      }
      L[i] = static_cast<float>(ol);
      R[i] = static_cast<float>(orr);
    }
  }
  float follower() const { return static_cast<float>(fol_); }

 private:
  static float P(const PluginState& s, int i, float d = 0.f) {
    return i >= 0 && i < static_cast<int>(s.params.size()) ? s.params[static_cast<size_t>(i)] : d;
  }
  struct Tabs {
    float map[kUnits][kTargets][3][1025];
    int16_t ws[kUnits][32768 + 2];
  };
  // A ramped value: start, per-sample step, current.
  struct Ramp {
    double v = 0, step = 0;
    void to(double target, int len, double cap, bool snap) {
      if (snap) {
        v = target, step = 0;
        return;
      }
      step = std::clamp((target - v) / len, -cap, cap);
    }
    double next() {
      const double r = v;
      v += step;
      return r;
    }
  };

  void tick(const OctaBank& bk, const PluginState& s, const ProcessContext& ctx, double beat) {
    const bool snap = !primed_;
    primed_ = true;
    // Mod X / Y (smoothed per tick when on) and the follower peak of the last tick.
    const double tx = P(s, OfModX, 0.5f) * 1024.0, ty = P(s, OfModY, 0.5f) * 1024.0;
    if (P(s, OfSmooth) >= 0.5f && !snap) {
      const double rho = P(s, OfHqEnvelopes) >= 0.5f ? 0.996 : 0.9685;
      mx_ = static_cast<float>(tx + (mx_ - tx) * rho), my_ = static_cast<float>(ty + (my_ - ty) * rho);
    } else {
      mx_ = static_cast<float>(tx), my_ = static_cast<float>(ty);
    }
    // J-46: clamped before the int conversion (a runaway follower peak overflowed int).
    auto cell = [](double v) { return std::isnan(v) ? 0 : static_cast<int>(std::clamp(std::nearbyint(v), 0.0, 1024.0)); };
    const int idx[3] = {cell(mx_), cell(my_), cell(folPeak_ * 1024.0)};
    folPeak_ = 0;
    const int pat = std::clamp(static_cast<int>(std::lround(P(s, OfPattern))), 0, kPatterns - 1);
    const double lfoS = signedVol(P(s, OfMasterLfo));
    const double beatsPerTick = std::max(1.0, ctx.tempo) / 60.0 * tick_ / srInt_;
    const double master = volLaw(P(s, OfMaster, 1.f));
    const int solo = static_cast<int>(std::lround(P(s, OfSolo))) - 1;
    const double K = 44100.0 / srInt_;
    const double cap = 0.01 * 44100.0 / srInt_;
    for (int u = 0; u < kUnits; ++u) {
      const double env = P(s, unitParam(u, UEnv), 1.f);
      float v[kTargets];
      for (int t = 0; t < kTargets; ++t) {
        const bool add = additiveTarget(t);
        float x = add ? 0.f : 1.f;
        const float envAmt = t == TCut ? static_cast<float>(env) : 1.f;
        for (int m = 0; m < 3; ++m) {
          if (!hasMap_[u][t][m]) continue;
          float q = tabs_->map[u][t][m][idx[m]];
          if (m == 2) q *= envAmt;
          x = add ? x + q : x * q;
        }
        if (const OEnv* e = bk.env(u, t, KPattern0 + pat)) {
          const float y = static_cast<float>(envAt(*e, beat, bk.loopS[u][t][pat], bk.loopE[u][t][pat], add ? 0.5 : 0.0));
          x = add ? x + y * envAmt : x * y * envAmt;
        }
        if (const OEnv* e = bk.env(u, t, KLfo)) {
          const float lfoAmt = static_cast<float>(lfoS) * (t == TCut ? static_cast<float>(env) : 1.f);
          float l = 0;
          if (e->n > 0 && beat >= e->x[0]) {
            const float o = static_cast<float>(envAt(*e, beat, -1, -1, 0.5));
            const double cyc = std::min(805306368.0, static_cast<double>(static_cast<float>(lfoRate_[u][t] * (4294967296.0 / 625.0))) * beatsPerTick);
            const int64_t inc = std::llround(cyc);
            const double ticks = beat / beatsPerTick;
            const uint32_t ph = lfo_[u][t].start + static_cast<uint32_t>(static_cast<uint64_t>(std::llround(ticks * static_cast<double>(inc))) & 0xFFFFFFFFull);
            l = lfo_[u][t].at(ph) * o;
          }
          x = add ? x + l * lfoAmt : x * (2.f * l * lfoAmt + 1.f);
        }
        v[t] = x;
      }
      // Design and ramps.
      UnitRun& R = run_[u];
      DesignIn di;
      di.type = std::clamp(static_cast<int>(std::lround(P(s, unitParam(u, UType)))), 0, FilterTypeCount - 1);
      di.mode = std::clamp(static_cast<int>(std::lround(P(s, unitParam(u, UOrder), 1.f))), 0, 5);
      di.alt = P(s, unitParam(u, UAlt)) >= 0.5f;
      di.c = P(s, unitParam(u, UCut), 0.5f) + v[TCut];
      di.r = P(s, unitParam(u, URes), 0.5f) + v[TRes];
      di.lowRaw = P(s, unitParam(u, ULow), 1.f) * 128.0;
      di.gLow = volLaw(P(s, unitParam(u, ULow), 1.f)) * v[TLow];
      di.gBand = volLaw(P(s, unitParam(u, UBand))) * v[TBand];
      di.gHigh = volLaw(P(s, unitParam(u, UHigh))) * v[THigh];
      di.K = K;
      const Design d = design(di);
      const bool changed = d.type != R.d.type || d.n != R.d.n;
      if (changed) {
        for (auto& st : z_[u])
          for (auto& c : st) c = StageS{};
      }
      R.d = d;
      const bool sn = snap || changed;
      for (int k = 0; k < d.n; ++k)
        for (int j = 0; j < 5; ++j) R.p[k][j].to(d.st[k].v[j], tick_, cap, sn);
      if (d.kind == KSvf && di.alt)  // ALT: damping from the f ramp's two ends
        for (int k = 0; k < d.n; ++k) {
          const double f0 = R.p[k][0].v, f1 = R.p[k][0].v + R.p[k][0].step * tick_;
          const double fm = std::max(std::max(f0, f1), 1e-9);
          const double q = (k == 0 ? R.p[0][1].v : 1.0) * K / fm;
          R.p[k][1].v = q;
          if (k > 0) R.p[k][1].step = 0;
        }
      // Gains: input / to-next (constants), output pan matrix (ramped).
      R.on = P(s, unitParam(u, UEnabled), u == 0 ? 1.f : 0.f) >= 0.5f;
      R.in = signedVol(P(s, unitParam(u, UIn), 1.f));
      R.next = signedVol(P(s, unitParam(u, UNext)));
      R.wsOn = P(s, unitParam(u, UWsOn)) >= 0.5f;
      R.wsBip = P(s, unitParam(u, UWsBipolar)) >= 0.5f;
      R.wsPre = P(s, unitParam(u, UWsPre), 1.f);
      R.wsHq = P(s, OfHqShaper) >= 0.5f;
      const double wsMix = std::clamp(static_cast<double>(P(s, unitParam(u, UWsMix), 1.f)) * v[TWsMix], 0.0, 1.0);
      R.mix.to(wsMix, tick_, cap, sn);
      const double vol = (solo >= 0 && solo != u) ? 0.0 : volLaw(P(s, unitParam(u, UOut), 1.f)) * master * v[TVol];
      const double p = P(s, unitParam(u, UPan)) + 2.0 * v[TPan];
      double m[4];
      panMatrix(p, vol, m);
      for (int j = 0; j < 4; ++j) R.g[j].to(m[j], tick_, cap, sn);
    }
  }

  // Constant-power pan ×√2 with a fold of the far side: [LL, RR, RL, LR].
  static void panMatrix(double p, double v, double m[4]) {
    if (v == 0) {
      m[0] = m[1] = m[2] = m[3] = 0;
      return;
    }
    if (p == 0) {
      m[0] = m[1] = v, m[2] = m[3] = 0;
      return;
    }
    const double th = (1.0 + p) * kPi / 4;
    const double g = static_cast<float>(v * static_cast<float>(1.0 / std::sin(kPi / 4)));
    double LL = g * std::cos(th), RR = g * std::sin(th), RL = 0, LR = 0;
    if (p > 0) {
      const double d = (1.0 - LL / v) / 2;
      LR = RR * d, RR *= 1.0 - d;
    } else {
      const double d = (1.0 - RR / v) / 2;
      RL = LL * d, LL *= 1.0 - d;
    }
    m[0] = LL, m[1] = RR, m[2] = RL, m[3] = LR;
  }

  double shape(int u, double x) const {
    const int16_t* T = tabs_->ws[u] + 1;  // T[-1] .. T[N] valid
    const int N = wsN_[u], M = N - 1;
    const double G = run_[u].wsPre;
    // J-46: a runaway (huge / inf / NaN) input made lrint return LONG_MIN and
    // -k overflow. Past the table ends the output is flat (T[-1] = T[0],
    // T[M + 1] = T[M]), so clamping just outside them changes nothing else.
    auto lim = [M](double v) { return std::isnan(v) ? 0.0 : std::clamp(v, -(M + 2.0), M + 2.0); };
    if (!run_[u].wsBip) {
      const double val = lim(x * G * M);
      if (!run_[u].wsHq) {
        const long k = std::lrint(val);
        return (k >= 0 ? T[std::min<long>(k, M)] : -T[std::min<long>(-k, M)]) / 32768.0;
      }
      const long k = std::lrint(val);
      const double f = val - static_cast<double>(k);
      if (k >= 0) {
        const long a = std::min<long>(k, M);
        return (T[a] + (T[a + 1] - T[a]) * f) / 32768.0;
      }
      const long a = std::min<long>(-k, M);
      return -(T[a] - (T[a + 1] - T[a]) * f) / 32768.0;
    }
    const double val = lim(x * G * M / 2 + M / 2.0);
    long k = std::lrint(val);
    if (!run_[u].wsHq) return T[std::clamp<long>(k, 0, M)] / 32768.0;
    const double f = val - static_cast<double>(k);
    k = std::clamp<long>(k, -1, M);
    return (T[k] + (T[k + 1] - T[k]) * f) / 32768.0;
  }

  void sample(const PluginState& s, double& l, double& r) {
    // Follower on the (internal-rate) input or unit 1's output.
    const bool use1 = P(s, OfIefUse1) >= 0.5f;
    const double lvl = volLaw(P(s, OfIefLevel, 1.f));
    auto follow = [&](double a, double b) {
      const double x = std::max(std::fabs(a), std::fabs(b)) * lvl;
      const double cA = std::exp(-std::log(100.0) / std::max(1e-9, P(s, OfIefAttack, 0.025f) * srInt_));
      const double cR = std::exp(-std::log(100.0) / std::max(1e-9, P(s, OfIefRelease, 0.1f) * srInt_));
      fol_ = static_cast<float>((fol_ - x) * (x > fol_ ? cA : cR) + x);
      if (std::fabs(fol_) < 5.9604644775390625e-08) fol_ = 0;
      folPeak_ = std::max(folPeak_, fol_);
    };
    if (!use1) follow(l, r);
    double outL = 0, outR = 0, uL = 0, uR = 0;
    bool prevOn = false;
    double prevNext = 0;
    for (int u = 0; u < kUnits; ++u) {
      UnitRun& R = run_[u];
      if (!R.on) {
        prevOn = false;
        continue;
      }
      double xl = (prevOn ? uL * prevNext : 0.0) + l * R.in, xr = (prevOn ? uR * prevNext : 0.0) + r * R.in;
      double pv[3][5];
      for (int k = 0; k < R.d.n; ++k)
        for (int j = 0; j < 5; ++j) pv[k][j] = R.p[k][j].next();
      for (int k = 0; k < R.d.n; ++k) {
        StageP sp = R.d.st[k];
        if (R.d.kind == KSvf || R.d.kind == KLime)
          for (int j = 0; j < 5; ++j) sp.v[j] = pv[k][j];
        xl = runStage(R.d, sp, z_[u][k][0], xl);
        xr = runStage(R.d, sp, z_[u][k][1], xr);
      }
      const double mix = R.mix.next();
      if (R.wsOn && mix > 0) {
        const double sl = shape(u, xl), sr = shape(u, xr);
        xl = xl * (1 - mix) + sl * mix;
        xr = xr * (1 - mix) + sr * mix;
      }
      if (u == 0 && use1) follow(xl, xr);
      const double LL = R.g[0].next(), RR = R.g[1].next(), RL = R.g[2].next(), LR = R.g[3].next();
      outL += xl * LL + xr * RL;
      outR += xr * RR + xl * LR;
      uL = xl, uR = xr, prevOn = true, prevNext = R.next;
    }
    l = outL, r = outR;
  }

  struct UnitRun {
    Design d;
    Ramp p[3][5];
    Ramp mix, g[4];
    bool on = false, wsOn = false, wsBip = false, wsHq = false;
    double in = 1, next = 0, wsPre = 1;
  };
  std::unique_ptr<Tabs> tabs_;
  bool hasMap_[kUnits][kTargets][3] = {};
  int wsN_[kUnits] = {};
  LfoShape lfo_[kUnits][kTargets];
  float lfoRate_[kUnits][kTargets] = {};
  UnitRun run_[kUnits];
  StageS z_[kUnits][3][2];
  Halfband up_[4][2], dn_[4][2];
  double dcS_[2] = {}, dcW_[2] = {};
  float fol_ = 0, folPeak_ = 0, mx_ = 512, my_ = 512;
  double sr_ = 0, srInt_ = 44100, freeBeat_ = 0;
  int os_ = -1, tick_ = 64, tickPos_ = 0;
  bool primed_ = false;
};

}  // namespace gl::packs::creative::ref
