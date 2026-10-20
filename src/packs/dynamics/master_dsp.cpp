// Gridline — "dynamics" pack: Mastering Dynamics (gl.master), Enhance
// (gl.enhance) and Loudness (gl.emphasis) runtimes.
#include <cstring>
#include "dyn_dsp.h"
#include "emphasizer_ref.h"

namespace gl::packs::dynamics {

// ===================================================================== curve

double masterCurveParams(double thHi, double ratioHi, double knee, double thLo, double ratioLo, double gate, double in) {
  double out = in;
  thLo = std::min(thLo, thHi);
  knee = std::max(0.0, knee);
  // Above: downward compression with a quadratic soft knee.
  double over = in - thHi, sHi = 1.0 / std::max(1.0, ratioHi) - 1.0;
  if (knee > 0 && std::fabs(over) <= knee * 0.5)
    out += sHi * (over + knee * 0.5) * (over + knee * 0.5) / (2.0 * knee);
  else if (over > knee * 0.5)
    out += sHi * over;
  // Below: upward compression (ratio < 1) or downward expansion (ratio > 1).
  // The below section acts over a 36 dB range under the low threshold; further
  // down the curve runs parallel to the diagonal, so noise floors and fades
  // are not lifted without limit.
  double under = std::max(in - thLo, -36.0), sLo = ratioLo - 1.0;
  double lo = 0;  // positive = lift
  if (knee > 0 && std::fabs(under) <= knee * 0.5)
    lo = -sLo * (under - knee * 0.5) * (under - knee * 0.5) / (2.0 * knee);
  else if (under < -knee * 0.5)
    lo = sLo * under;
  lo = std::min(lo, 30.0);  // never lift more than 30 dB
  out += lo;
  // Gate: steep expansion below the gate threshold.
  if (gate > -89.9 && in < gate) out += std::max(-90.0, (in - gate) * 9.0);
  return out;
}

double masterCurve(const PluginState& s, int band, double in) {
  auto g = [&](int k) -> double {
    size_t i = static_cast<size_t>(msParam(band, k));
    if (i < s.params.size()) return s.params[i];
    const PluginInfo* info = findPlugin("gl.master");
    return info && i < info->params.size() ? info->params[i].def : 0.0;
  };
  return masterCurveParams(g(MbThreshHi), g(MbRatioHi), g(MbKnee), g(MbThreshLo), g(MbRatioLo), g(MbGate), in);
}

// ===================================================================== drawn curve (κ2)

double DrawnCurve::at(double xv) const {
  if (n <= 0) return 0;
  if (xv <= x[0]) return y[0];
  for (int i = 1; i < n; ++i) {
    if (xv <= x[i]) {
      double w = x[i] - x[i - 1];
      double f = w > 1e-12 ? (xv - x[i - 1]) / w : 1.0;
      double tn = t[i];  // tension lives on the segment's end point
      if (std::fabs(tn) > 1e-4) f = std::pow(f, std::pow(2.0, -tn * 4.0));
      return y[i - 1] + (y[i] - y[i - 1]) * f;
    }
  }
  return y[n - 1];
}

DrawnCurve masterDrawnCurve(const PluginState& s, int band) {
  const PluginInfo* info = findPlugin("gl.master");
  auto g = [&](int k) -> double {
    size_t i = static_cast<size_t>(msExt(band, k));
    if (i < s.params.size()) return s.params[i];
    return info && i < info->params.size() ? info->params[i].def : 0.0;
  };
  DrawnCurve c;
  c.n = std::clamp(static_cast<int>(std::lround(g(MxPoints))), 2, kMsCurvePts);
  double last = 0;
  for (int k = 0; k < c.n; ++k) {
    c.x[k] = std::max(last, clampd(g(MxPt0 + k * 3), 0, 1));
    c.y[k] = clampd(g(MxPt0 + k * 3 + 1), 0, 1);
    c.t[k] = clampd(g(MxPt0 + k * 3 + 2), -1, 1);
    last = c.x[k];
  }
  return c;
}

// Reference law: curve x = log9(2a + 1) for a linear level a (0..4, x 0.5 = 0 dBFS),
// output level (9^y − 1) / 2; the gain is output / input.
double drawnCurveGain(const DrawnCurve& c, double a) {
  a = clampd(a, 1.0 / 8192.0, 4.0);
  double xv = std::log(2.0 * a + 1.0) / std::log(9.0);
  double out = (std::pow(9.0, c.at(xv)) - 1.0) * 0.5;
  return out / a;
}

// ===================================================================== engine

// Envelope stage (1 %-settling coefficients), as used by the reference dynamics.
struct EnvStage {
  double up = 0, down = 0, s = 0;
  static double coef(double samples) { return samples <= 0 ? 0.0 : std::exp(-4.605170185988091 / std::max(1.0, samples)); }
  double run(double x) {
    s = x > s ? x + (s - x) * up : x + (s - x) * down;
    s = flushDenormal(s);
    return s;
  }
};
struct EnvChain {
  EnvStage st[8];
  int n = 0;
  void set(int count, double rise, double fall) {
    n = std::clamp(count, 0, 8);
    double k = n > 0 ? 1.0 / std::sqrt(static_cast<double>(n)) : 1.0;
    for (int i = 0; i < n; ++i) st[i].up = EnvStage::coef(rise * k), st[i].down = EnvStage::coef(fall * k);
  }
  double run(double x) {
    for (int i = 0; i < n; ++i) x = st[i].run(x);
    return x;
  }
  void reset(double v) {
    for (auto& s : st) s.s = v;
  }
};

struct BandSet {
  int mode = 1;  // 0 bypass, 1 active, 2 mute
  double pre = 1, post = 1;
  double atk = 0.01, rel = 0.15, window = 0;
  double thHi = 0, ratioHi = 1, knee = 6, thLo = -60, ratioLo = 1, gate = -90;
  double satThr = 1, satCeil = 2;
  double stereo = 0;
  // κ2 drawn-curve (reference) engine
  bool drawn = false;
  DrawnCurve curve;
  double rel2 = 0;
  int atkCurve = 1, relCurve = 2, envMode = 1, satMode = 1;
};

struct MasterSet {
  double f1 = 150, f2 = 3500;
  int s1 = 2, s2 = 2;
  double lookaheadMs = 2;
  double bandMix = 1;
  double lowCut = 10;
  int solo = 0;
  int os = 0;
  BandSet band[kMsBands];
};

class MasterEngine {
 public:
  static constexpr int kDelaySize = 8192;
  // Look-ahead (samples) of each of the two delay stages (bands, master band).
  static int lookahead(const MasterSet& S, double sr) {
    const int la = static_cast<int>(std::lround(clampd(S.lookaheadMs, 0, 20) * 0.001 * sr));
    return std::min(la, kDelaySize - 2);
  }
  // Output delay: the bands and the master band each delay the audio by the
  // look-ahead; an active (compressing) stage also oversamples its saturator.
  static int latency(const MasterSet& S, double sr) {
    int lat = 2 * lookahead(S, sr);
    const int os = osLatency(osFactor(S.os));
    if (os > 0) {
      bool bandOs = false;
      for (int b = 0; b < 3; ++b)
        if (S.band[b].mode == 1 && (S.solo == 0 || S.solo == b + 1)) bandOs = true;
      if (bandOs) lat += os;
      if (S.solo == 0 && S.band[3].mode == 1) lat += os;
    }
    return lat;
  }
  MasterEngine() {
    for (auto& d : dl_) d = DelayLine(kDelaySize);
  }

  void reset() {
    for (auto& c : xo_) c.reset();
    for (auto& c : lowCut_) c.reset();
    for (auto& d : dl_) d.reset();
    for (auto& o : os_) o.reset();
    for (int b = 0; b < kMsBands; ++b) {
      env_[b] = 0;
      gainDb_[b] = 0;
      grPeak_[b] = 0;
      hold_[b] = 0, holdCnt_[b] = 0, ms_[b] = 0;
      relCh_[b].reset(0), atkCh_[b].reset(1.0);
    }
    lastSr_ = 0;
  }

  void process(const MasterSet& S, double sr, float* L, float* R, int n) {
    if (sr != lastSr_ || S.f1 != lf1_ || S.f2 != lf2_ || S.s1 != ls1_ || S.s2 != ls2_) {
      double f1 = clampd(S.f1, 20, sr * 0.2), f2 = clampd(std::max(S.f2, f1 * 1.25), 40, sr * 0.45);
      for (auto& c : xo_) c.set(sr, f1, S.s1, f2, S.s2);
      lf1_ = S.f1, lf2_ = S.f2, ls1_ = S.s1, ls2_ = S.s2;
    }
    if (sr != lastSr_ || S.lowCut != lcut_) {
      for (auto& c : lowCut_) c.set(sr, std::max(10.0, S.lowCut), 0.7071);
      lcut_ = S.lowCut;
    }
    lastSr_ = sr;
    const size_t dly = static_cast<size_t>(lookahead(S, sr)) + 1;
    const int osf = osFactor(S.os);
    double atkC[kMsBands], relC[kMsBands], detRel[kMsBands], winC[kMsBands], satT[kMsBands], satC[kMsBands];
    for (int b = 0; b < kMsBands; ++b) {
      const BandSet& B = S.band[b];
      atkC[b] = timeCoef(B.atk, sr);
      relC[b] = timeCoef(B.rel, sr);
      detRel[b] = timeCoef(0.03, sr);
      winC[b] = timeCoef(B.window, sr);
      satT[b] = B.satThr;
      satC[b] = B.satCeil;
      grPeak_[b] = 0;
      if (B.drawn) {  // reference envelope: hold window, release / release 2 stages, attack smoothing
        holdLen_[b] = std::max(1, static_cast<int>(std::max(B.window, B.atk) * sr));
        // Level: release stages (instant rise). Gain: "attack / release 2" stages —
        // falling gain follows 0.8 x attack, recovering gain release 2 (0 = instant).
        relCh_[b].set(B.relCurve + 1, 0.0, B.rel * sr);
        atkCh_[b].set(B.atk > 0 || B.rel2 > 0 ? B.atkCurve + 1 : 0, B.rel2 * sr, 0.8 * B.atk * sr);
        rmsC_[b] = timeCoef(std::max(B.window, 0.0005), sr);
      }
    }
    const bool lowCutOn = S.lowCut > 10.5;
    double inPeak = 0;

    for (int i = 0; i < n; ++i) {
      double x[2] = {L[i], R[i]};
      if (lowCutOn)
        for (int c = 0; c < 2; ++c) {
          lowCut_[c].tick(x[c]);
          x[c] = lowCut_[c].hp;
        }
      // Split, then delay the raw bands (the detectors see them early).
      double band[3][2];
      for (int c = 0; c < 2; ++c) {
        xo_[c].process(x[c], band[0][c], band[1][c], band[2][c]);
        for (int b = 0; b < 3; ++b) dl_[b * 2 + c].push(band[b][c]);
      }
      double sum[2] = {0, 0}, dry[2] = {0, 0}, solo[2] = {0, 0};
      for (int b = 0; b < 3; ++b) {
        const BandSet& B = S.band[b];
        double now[2] = {band[b][0], band[b][1]};
        double late[2] = {dl_[b * 2].at(dly), dl_[b * 2 + 1].at(dly)};
        dry[0] += late[0];
        dry[1] += late[1];
        double y[2];
        if (B.mode == 2) {
          y[0] = y[1] = 0;
        } else if (B.mode == 0) {
          y[0] = late[0];
          y[1] = late[1];
        } else {
          shape(B, now);
          double g = B.drawn ? detectDrawn(b, B, now) : detect(b, B, now, atkC[b], relC[b], detRel[b], winC[b]);
          shape(B, late);
          for (int c = 0; c < 2; ++c) {
            double v = late[c] * g;
            v = os_[b * 2 + c].process(v, osf, [&](double s) { return saturate(B, s, satT[b], satC[b]); });
            y[c] = v * B.post;
          }
        }
        sum[0] += y[0];
        sum[1] += y[1];
        if (S.solo == b + 1) {
          solo[0] = y[0];
          solo[1] = y[1];
        }
      }
      double m[2];
      for (int c = 0; c < 2; ++c) m[c] = dry[c] + (sum[c] - dry[c]) * S.bandMix;
      // Master band.
      const BandSet& M = S.band[3];
      for (int c = 0; c < 2; ++c) dl_[6 + c].push(S.solo ? solo[c] : m[c]);
      double late[2] = {dl_[6].at(dly), dl_[7].at(dly)};
      double out[2];
      if (S.solo || M.mode == 0) {
        out[0] = late[0];
        out[1] = late[1];
      } else if (M.mode == 2) {
        out[0] = out[1] = 0;
      } else {
        shape(M, m);
        inPeak = std::max(inPeak, std::max(std::fabs(m[0]), std::fabs(m[1])));
        double g = M.drawn ? detectDrawn(3, M, m) : detect(3, M, m, atkC[3], relC[3], detRel[3], winC[3]);
        shape(M, late);
        for (int c = 0; c < 2; ++c) {
          double v = late[c] * g;
          v = os_[6 + c].process(v, osf, [&](double s) { return saturate(M, s, satT[3], satC[3]); });
          out[c] = v * M.post;
        }
      }
      L[i] = static_cast<float>(out[0]);
      R[i] = static_cast<float>(out[1]);
    }
    inLevel_ = static_cast<float>(linToDb(inPeak));
  }

  float gr(int b) const { return static_cast<float>(grPeak_[b]); }
  float inLevel() const { return inLevel_; }
  float level(int b) const { return static_cast<float>(linToDb(env_[b])); }

 private:
  // Pre gain and stereo separation (memoryless, applied to both the detector
  // copy and the delayed copy).
  static void shape(const BandSet& B, double* v) {
    double l = v[0] * B.pre, r = v[1] * B.pre;
    if (B.stereo != 0) {
      double mid = (l + r) * 0.5, side = (l - r) * 0.5 * (1.0 + B.stereo);
      l = mid + side;
      r = mid - side;
    }
    v[0] = l;
    v[1] = r;
  }

  // Reference (drawn-curve) detector: peak hold over the sustain window (or an
  // RMS window), release / release-2 stages, the drawn curve as gain computer,
  // attack smoothing of the gain.
  double detectDrawn(int b, const BandSet& B, const double* v) {
    double a = std::max(std::fabs(v[0]), std::fabs(v[1]));
    double lvl;
    if (B.envMode == 1) {
      ms_[b] = flushDenormal(a * a + (ms_[b] - a * a) * rmsC_[b]);
      lvl = std::sqrt(2.0 * ms_[b]);
    } else {
      if (a >= hold_[b]) hold_[b] = a, holdCnt_[b] = holdLen_[b];
      else if (holdCnt_[b] > 0) --holdCnt_[b];
      else hold_[b] = a;
      lvl = hold_[b];
    }
    double e = relCh_[b].run(lvl);
    env_[b] = e;
    double g = atkCh_[b].run(drawnCurveGain(B.curve, std::max(e, 1e-6)));
    double gdb = linToDb(std::max(g, 1e-9));
    gainDb_[b] = gdb;
    if (std::fabs(gdb) > std::fabs(grPeak_[b])) grPeak_[b] = gdb;
    return g;
  }

  // Saturation stage: Gridline's tanh knee, or the reference modes (A: exponential
  // approach to the ceiling, B: hard knee with a short tanh shoulder).
  static double saturate(const BandSet& B, double x, double thr, double ceil) {
    if (!B.drawn || B.satMode == 1) return softSat(x, thr, ceil);
    if (B.satMode == 0) return x;
    double a = std::fabs(x);
    if (a <= thr) return x;
    double span = std::max(1e-6, ceil - thr);
    double y = thr + span * (1.0 - std::exp(-(a - thr) / span));
    return x < 0 ? -y : y;
  }

  double detect(int b, const BandSet& B, const double* v, double atkC, double relC, double detRel, double winC) {
    double a = std::max(std::fabs(v[0]), std::fabs(v[1]));
    double lev;
    if (B.window > 0) {
      ms_[b] = flushDenormal(a * a + (ms_[b] - a * a) * winC);
      env_[b] = std::sqrt(ms_[b]);
      lev = env_[b] * 1.41421356;  // RMS of a sine reads as its peak
    } else {
      env_[b] = a > env_[b] ? a : flushDenormal(a + (env_[b] - a) * detRel);
      lev = env_[b];
    }
    double inDb = linToDb(std::max(lev, 1e-7));
    double target = masterCurveParams(B.thHi, B.ratioHi, B.knee, B.thLo, B.ratioLo, B.gate, inDb) - inDb;
    double& g = gainDb_[b];
    g = target + (g - target) * (target < g ? atkC : relC);
    if (std::fabs(g) > std::fabs(grPeak_[b])) grPeak_[b] = g;
    return dbToLin(g);
  }

  Crossover3 xo_[2];
  SvfTpt lowCut_[2];
  DelayLine dl_[8];
  Oversampler os_[8];
  double env_[kMsBands] = {}, ms_[kMsBands] = {}, gainDb_[kMsBands] = {}, grPeak_[kMsBands] = {};
  EnvChain relCh_[kMsBands], atkCh_[kMsBands];
  double hold_[kMsBands] = {}, rmsC_[kMsBands] = {};
  int holdCnt_[kMsBands] = {}, holdLen_[kMsBands] = {1, 1, 1, 1};
  double lastSr_ = 0, lf1_ = -1, lf2_ = -1, lcut_ = -1;
  int ls1_ = -1, ls2_ = -1;
  float inLevel_ = -120;
};

// ===================================================================== gl.master

// Fills a MasterSet from a getter over the gl.master parameter layout (used by
// gl.master itself and by gl.enhance's embedded program).
template <class G>
void readMasterSet(MasterSet& S, const G& p) {
  auto pi = [&](int k) { return static_cast<int>(std::lround(p(k))); };
  S.f1 = p(MsLowFreq);
  S.f2 = p(MsHighFreq);
  S.s1 = pi(MsLowSlope);
  S.s2 = pi(MsHighSlope);
  S.lookaheadMs = p(MsLookahead);
  S.bandMix = p(MsBandMix);
  S.lowCut = p(MsLowCut);
  S.solo = pi(MsSolo);
  S.os = pi(MsOversample);
  for (int b = 0; b < kMsBands; ++b) {
    BandSet& B = S.band[b];
    auto q = [&](int k) { return p(msParam(b, k)); };
    B.mode = static_cast<int>(std::lround(q(MbMode)));
    B.pre = dbToLin(q(MbPre));
    B.post = dbToLin(q(MbPost));
    B.atk = q(MbAttack) * 0.001;
    B.rel = q(MbRelease) * 0.001;
    B.window = q(MbWindow) * 0.001;
    B.thHi = q(MbThreshHi);
    B.ratioHi = q(MbRatioHi);
    B.knee = q(MbKnee);
    B.thLo = q(MbThreshLo);
    B.ratioLo = q(MbRatioLo);
    B.gate = q(MbGate);
    B.satThr = dbToLin(q(MbSatThresh));
    B.satCeil = dbToLin(q(MbSatCeil));
    B.stereo = q(MbStereo);
    auto x = [&](int k) { return p(msExt(b, k)); };
    B.drawn = static_cast<int>(std::lround(x(MxCurveMode))) == 1;
    if (B.drawn) {
      DrawnCurve& c = B.curve;
      c.n = std::clamp(static_cast<int>(std::lround(x(MxPoints))), 2, kMsCurvePts);
      double last = 0;
      for (int k = 0; k < c.n; ++k) {
        c.x[k] = std::max(last, clampd(x(MxPt0 + k * 3), 0, 1));
        c.y[k] = clampd(x(MxPt0 + k * 3 + 1), 0, 1);
        c.t[k] = clampd(x(MxPt0 + k * 3 + 2), -1, 1);
        last = c.x[k];
      }
      B.rel2 = x(MxRelease2) * 0.001;
      B.atkCurve = static_cast<int>(std::lround(x(MxAtkCurve)));
      B.relCurve = static_cast<int>(std::lround(x(MxRelCurve)));
      B.envMode = static_cast<int>(std::lround(x(MxEnvMode)));
      B.satMode = static_cast<int>(std::lround(x(MxSatMode)));
    }
  }
}

class MasterFx final : public DynFx {
 public:
  MasterFx() : DynFx("gl.master") {}
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    MasterSet S;
    readMasterSet(S, [&](int k) { return p(s, k); });
    eng_.process(S, ctx.sampleRate, L, R, n);
    meter_ = eng_.gr(3);
    input_ = eng_.inLevel();
  }
  int latencyFor(const PluginState& s, double sr) const override {
    MasterSet S;
    readMasterSet(S, [&](int k) { return p(s, k); });
    return MasterEngine::latency(S, sr);
  }
  int meters(float* out, int max) const override {
    int k = std::min(max, kMsBands);
    for (int b = 0; b < k; ++b) out[b] = eng_.gr(b);
    return k;
  }
  void reset() override { eng_.reset(); }

 private:
  MasterEngine eng_;
};

// ===================================================================== gl.enhance
// One amount knob over four voicings of the mastering engine.
class EnhanceFx final : public DynFx {
 public:
  EnhanceFx() : DynFx("gl.enhance") {}
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    if (pi(s, EnCustom) == 2) {  // J-26 peak emphasis
      emref::Settings es;
      es.inDb = p(s, EnPeakIn), es.emphasis = clampd(p(s, EnPeakEmph), 0, 1), es.outGain = clampd(p(s, EnPeakOut), 0, 1);
      es.soft = pb(s, EnPeakSoft), es.match = pb(s, EnPeakMatch), es.bypass = pb(s, EnPeakBypass);
      double pk = 0;
      for (int i = 0; i < n; ++i) pk = std::max(pk, static_cast<double>(std::max(std::fabs(L[i]), std::fabs(R[i]))));
      peak_.process(es, ctx.sampleRate, L, R, n);
      input_ = static_cast<float>(linToDb(pk));
      meter_ = static_cast<float>(peak_.gainReductionDb());
      return;
    }
    const double amount = clampd(p(s, EnAmount), 0, 1);
    const int mode = pi(s, EnMode);
    MasterSet S;
    if (pi(s, EnCustom) == 1) {  // embedded program; the amount is its band mix (reference behaviour)
      readMasterSet(S, [&](int k) { return p(s, EnProg0 + k); });
      S.bandMix = amount;
      eng_.process(S, ctx.sampleRate, L, R, n);
      meter_ = eng_.gr(3);
      input_ = eng_.inLevel();
      return;
    }
    // Built-in voicings at full strength; the amount blends the processed bands with
    // the dry bands before the master stage, as the reference's amount does.
    const double a = 1.0;
    S.bandMix = amount;
    voicingTiming(S);
    // Per voicing: crossovers, then per band {pre dB, thresh, ratio, lift ratio, stereo, sat}.
    struct V {
      double f1, f2;
      double pre[3], th[3], ratio[3], lift[3], stereo[3];
      double atk, rel, masterPre, masterSat;
    };
    static const V voices[4] = {
        // A: balanced loudness with a gentle smile curve
        {160, 4200, {2, 0.5, 2}, {-20, -22, -24}, {3, 2.5, 3}, {0.8, 0.85, 0.8}, {0, 0.05, 0.15}, 12, 160, 3, -2},
        // B: punchy — slower attack keeps transients, firm low end
        {120, 2500, {3, 1.5, 1}, {-18, -20, -22}, {4, 3, 2.5}, {0.85, 0.75, 0.9}, {-0.1, 0, 0.1}, 35, 110, 4, -1.5},
        // C: airy — lifted top, wide highs
        {200, 5500, {0, 1, 4}, {-24, -24, -26}, {2.5, 2.5, 3.5}, {0.9, 0.85, 0.7}, {0, 0.1, 0.35}, 8, 200, 2.5, -2.5},
        // D: dense — heavy compression on every band and more saturation
        {140, 3200, {3, 2, 3}, {-26, -26, -28}, {6, 5, 6}, {0.7, 0.7, 0.7}, {0, 0.05, 0.2}, 6, 90, 5, -4},
    };
    const V& v = voices[std::clamp(mode, 0, 3)];
    S.f1 = v.f1;
    S.f2 = v.f2;
    for (int b = 0; b < 3; ++b) {
      BandSet& B = S.band[b];
      B.pre = dbToLin(v.pre[b] * a);
      B.thHi = v.th[b];
      B.ratioHi = 1.0 + (v.ratio[b] - 1.0) * a;
      B.thLo = v.th[b] - 6;
      B.ratioLo = 1.0 + (v.lift[b] - 1.0) * a;
      B.knee = 10;
      B.atk = v.atk * 0.001;
      B.rel = v.rel * 0.001;
      B.stereo = v.stereo[b] * a;
      B.satThr = dbToLin(0);
      B.satCeil = dbToLin(6);
      B.post = dbToLin(6.0 * a);  // make-up: the processed bands are louder than the dry ones
    }
    BandSet& M = S.band[3];
    M.pre = dbToLin(v.masterPre * a);
    M.thHi = -1.0;
    M.ratioHi = 1.0 + 29.0 * a;
    M.knee = 3;
    M.atk = 0.001;
    M.rel = 0.08;
    M.satThr = dbToLin(v.masterSat * a - 0.3);
    M.satCeil = dbToLin(-0.2);
    eng_.process(S, ctx.sampleRate, L, R, n);
    meter_ = eng_.gr(3);
    input_ = eng_.inLevel();
  }
  int meters(float* out, int max) const override {
    int k = std::min(max, kMsBands);
    for (int b = 0; b < k; ++b) out[b] = eng_.gr(b);
    return k;
  }
  void reset() override {
    eng_.reset();
    peak_.reset();
  }
  int latencyFor(const PluginState& s, double sr) const override {
    if (pi(s, EnCustom) == 2) return emref::Limiter::latency(sr);
    MasterSet S;
    if (pi(s, EnCustom) == 1)
      readMasterSet(S, [&](int k) { return p(s, EnProg0 + k); });
    else
      voicingTiming(S);  // built-in voicings: every band and the master band active
    return MasterEngine::latency(S, sr);
  }

 private:
  emref::Limiter peak_;
  // Look-ahead and oversampling shared by the built-in voicings.
  static void voicingTiming(MasterSet& S) {
    S.lookaheadMs = 1.5;
    S.os = 1;
  }
  MasterEngine eng_;
};

// ===================================================================== gl.emphasis

// Look-ahead gain computer: sliding minimum over W samples, release
// smoothing, then a W-sample moving average. Applied to audio delayed by
// W - 1 samples, the result never exceeds the required gain.
class LookaheadGain {
 public:
  static constexpr int kCap = 4096;
  void setWindow(int w) {
    w = std::clamp(w, 1, kCap / 2);
    if (w != w_) {
      w_ = w;
      clear();
    }
  }
  int window() const { return w_; }
  double process(double req, double relC) {
    // sliding minimum (monotonic deque)
    while (tail_ > head_ && dqv_[(tail_ - 1) & (kCap - 1)] >= req) --tail_;
    dqv_[tail_ & (kCap - 1)] = req;
    dqi_[tail_ & (kCap - 1)] = t_;
    ++tail_;
    while (dqi_[head_ & (kCap - 1)] <= t_ - w_) ++head_;
    double hold = dqv_[head_ & (kCap - 1)];
    rel_ = hold < rel_ ? hold : hold + (rel_ - hold) * relC;
    // moving average of the released gain over the last W samples
    sum_ += rel_ - ring_[(t_ - w_) & (kCap - 1)];
    ring_[t_ & (kCap - 1)] = rel_;
    ++t_;
    if ((t_ & 1023) == 0) {  // exact recompute now and then keeps rounding from drifting
      double s = 0;
      for (int k = 0; k < w_; ++k) s += ring_[(t_ - 1 - k) & (kCap - 1)];
      sum_ = s;
    }
    return std::min(1.0, sum_ / w_);
  }
  void clear() {
    head_ = tail_ = 0;
    t_ = 0;
    rel_ = 1;
    for (int i = 0; i < kCap; ++i) ring_[i] = 1.0;
    sum_ = static_cast<double>(w_);
  }

 private:
  int w_ = 0;
  int64_t head_ = 0, tail_ = 0, t_ = 0;
  double dqv_[kCap] = {};
  int64_t dqi_[kCap] = {};
  double ring_[kCap] = {};
  double rel_ = 1, sum_ = 0;
};

class LoudnessFx final : public DynFx {
 public:
  LoudnessFx() : DynFx("gl.emphasis") {
    for (auto& d : dly_) d = DelayLine(8192);
    for (auto& d : dryDly_) d = DelayLine(8192);
  }
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    const double sr = ctx.sampleRate;
    const double inG = dbToLin(p(s, EmInput));
    const double ceil = dbToLin(p(s, EmCeiling));
    const int ch = std::clamp(pi(s, EmCharacter), 0, 3);
    static const double kRelMul[4] = {1.0, 0.5, 1.6, 2.5};
    static const double kKnee[4] = {0.75, 0.9, 0.6, 0.5};
    const int W = window(ch, sr);
    for (auto& g : lim_) g.setWindow(W);
    const double relC = timeCoef(p(s, EmRelease) * 0.001 * kRelMul[ch], sr);
    const double lowE = p(s, EmLowEmph), highE = p(s, EmHighEmph);
    if (sr != sr_ || lowE != lowE_ || highE != highE_) {
      for (int c = 0; c < 2; ++c) {
        preLo_[c].set(dsp::Biquad::LowShelf, sr, 150, 0.7071, lowE);
        postLo_[c].set(dsp::Biquad::LowShelf, sr, 150, 0.7071, -lowE);
        preHi_[c].set(dsp::Biquad::HighShelf, sr, 4000, 0.7071, highE);
        postHi_[c].set(dsp::Biquad::HighShelf, sr, 4000, 0.7071, -highE);
        dc_[c].set(sr);
      }
      sr_ = sr, lowE_ = lowE, highE_ = highE;
    }
    const double clip = clampd(p(s, EmClip), 0, 1);
    const double clipT = ceil * dbToLin((1.0 - clip) * 9.0);
    const bool hard = pb(s, EmClipHard);
    const double knee = kKnee[ch];
    const double link = clampd(p(s, EmLink), 0, 1);
    const bool ms = pi(s, EmStereoMode) == 1;
    const bool dcb = pb(s, EmDcBlock);
    const int osf = oversampling(s);
    const bool match = pb(s, EmLevelMatch), delta = pb(s, EmDelta);
    const double osLat = osLatency(osf);  // measured oversampler delay (as latencyFor reports)
    const double dryD = static_cast<double>(W - 1) + osLat + 1.0;
    double minG = 1, peak = 0;
    for (int i = 0; i < n; ++i) {
      double x[2] = {L[i] * inG, R[i] * inG};
      if (dcb)
        for (int c = 0; c < 2; ++c) x[c] = dc_[c].process(x[c]);
      peak = std::max(peak, std::max(std::fabs(x[0]), std::fabs(x[1])));
      for (int c = 0; c < 2; ++c) dryDly_[c].push(x[c]);
      // Emphasis → clipper → de-emphasis.
      double y[2];
      for (int c = 0; c < 2; ++c) {
        double e = preHi_[c].process(preLo_[c].process(x[c]));
        e = os_[c].process(e, osf, [&](double v) { return hard ? clampd(v, -clipT, clipT) : softSat(v, clipT * knee, clipT); });
        y[c] = postHi_[c].process(postLo_[c].process(e));
      }
      if (ms) {
        double m = (y[0] + y[1]) * 0.5, sd = (y[0] - y[1]) * 0.5;
        y[0] = m;
        y[1] = sd;
      }
      double req[2];
      for (int c = 0; c < 2; ++c) {
        double a = std::fabs(y[c]);
        req[c] = a > ceil ? ceil / a : 1.0;
      }
      double lk = std::min(req[0], req[1]);
      double g[2];
      for (int c = 0; c < 2; ++c) {
        double r = req[c] + (std::min(req[c], lk) - req[c]) * link;
        dly_[c].push(y[c]);
        g[c] = lim_[c].process(r, relC);
      }
      double o[2];
      for (int c = 0; c < 2; ++c) o[c] = dly_[c].at(static_cast<size_t>(W)) * g[c];
      if (ms) {
        double l = o[0] + o[1], r = o[0] - o[1];
        o[0] = l;
        o[1] = r;
      }
      for (int c = 0; c < 2; ++c) {
        o[c] = clampd(o[c], -ceil, ceil);  // final safety: never above the ceiling
        if (delta) o[c] = dryDly_[c].frac(dryD) - o[c];
        if (match) o[c] /= inG;
      }
      minG = std::min(minG, std::min(g[0], g[1]));
      L[i] = static_cast<float>(o[0]);
      R[i] = static_cast<float>(o[1]);
    }
    meter_ = static_cast<float>(linToDb(minG));
    input_ = static_cast<float>(linToDb(peak));
  }
  // The limited signal is read W - 1 samples behind its gain computer, after
  // the oversampled clipper.
  int latencyFor(const PluginState& s, double sr) const override {
    return window(std::clamp(pi(s, EmCharacter), 0, 3), sr) - 1 + osLatency(oversampling(s));
  }
  void reset() override {
    for (auto& g : lim_) g.clear();
    for (auto& d : dly_) d.reset();
    for (auto& d : dryDly_) d.reset();
    for (int c = 0; c < 2; ++c) {
      preLo_[c].clear(); postLo_[c].clear(); preHi_[c].clear(); postHi_[c].clear();
      dc_[c].reset();
      os_[c].reset();
    }
  }

 private:
  // Look-ahead window (samples) of a character, as LookaheadGain clamps it.
  static int window(int ch, double sr) {
    static const double kWinMs[4] = {1.5, 1.0, 3.0, 5.0};
    return std::clamp(static_cast<int>(std::lround(kWinMs[ch] * 0.001 * sr)), 1, LookaheadGain::kCap / 2);
  }
  int oversampling(const PluginState& s) const { return osFactor(pi(s, EmOversample)); }
  LookaheadGain lim_[2];
  DelayLine dly_[2], dryDly_[2];
  dsp::Biquad preLo_[2], postLo_[2], preHi_[2], postHi_[2];
  DcBlock dc_[2];
  Oversampler os_[2];
  double sr_ = 0, lowE_ = -999, highE_ = -999;
};

std::unique_ptr<Effect> createMasterEffect(const std::string& type) {
  if (type == "gl.master") return std::make_unique<MasterFx>();
  if (type == "gl.enhance") return std::make_unique<EnhanceFx>();
  if (type == "gl.emphasis") return std::make_unique<LoudnessFx>();
  return nullptr;
}

}  // namespace gl::packs::dynamics
