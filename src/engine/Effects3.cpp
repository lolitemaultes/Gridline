// Gridline — dynamics (0.5 parity): Compressor, Limiter (gate + RMS comp +
// saturation + look-ahead limiter), Gate, Multiband Compressor, Maximizer,
// Transient Shaper, Soft Clipper. Audio thread only; no allocation in process.
#include <array>
#include <cmath>
#include <memory>
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
inline double coef(double seconds, double sr) { return std::exp(-1.0 / std::max(1.0, seconds * sr)); }

// Static compression curve: gain change (dB, <= 0) for an input level (dB).
inline double compCurve(double db, double th, double ratio, double knee) {
  double over = db - th;
  if (knee > 0 && 2 * std::fabs(over) <= knee) return (1.0 / ratio - 1.0) * (over + knee / 2) * (over + knee / 2) / (2 * knee);
  if (2 * over < -knee) return 0;
  return (1.0 / ratio - 1.0) * over;
}

// Linkwitz-Riley crossover (2 or 4th order) of one channel into low/high.
struct Crossover {
  Biquad lp[2], hp[2];
  bool lr4 = true;
  void set(double sr, double hz, bool fourth) {
    lr4 = fourth;
    for (int k = 0; k < 2; ++k) {
      lp[k].set(Biquad::LowPass, sr, hz, fourth ? 0.7071 : 0.5, 0);
      hp[k].set(Biquad::HighPass, sr, hz, fourth ? 0.7071 : 0.5, 0);
    }
  }
  void split(double x, double& lo, double& hi) {
    if (lr4) {
      lo = lp[1].process(lp[0].process(x));
      hi = hp[1].process(hp[0].process(x));
    } else {  // LR2: high band inverted to sum flat
      lo = lp[0].process(x);
      hi = -hp[0].process(x);
    }
  }
  void clear() {
    for (auto& b : lp) b.clear();
    for (auto& b : hp) b.clear();
  }
};

// ======================================================================= Gate
class GateFx final : public Effect {
 public:
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    using namespace pp;
    const double sr = ctx.sampleRate;
    double thDb = param(s, GtThreshold, -40), hyst = param(s, GtHysteresis);
    double thOpen = dbToGain(thDb), thClose = dbToGain(thDb - hyst), floor = dbToGain(param(s, GtRange, -80));
    double att = coef(param(s, GtAttack, 0.001f), sr), rel = coef(param(s, GtRelease, 0.1f), sr);
    int64_t hold = static_cast<int64_t>(param(s, GtHold, 0.02f) * sr);
    int la = lookahead(s, sr);
    bool flip = param(s, GtFlip) >= 0.5f;
    double hp = param(s, GtKeyHpf, 20), lp = param(s, GtKeyLpf, 20000);
    bool useHp = hp > 20.5, useLp = lp < 19900;
    if (hp != hp_ || lp != lp_ || sr != sr_) {
      hp_ = hp, lp_ = lp, sr_ = sr;
      for (int c = 0; c < 2; ++c) {
        khp_[c].set(Biquad::HighPass, sr, hp, 0.7071, 0);
        klp_[c].set(Biquad::LowPass, sr, std::min(lp, sr * 0.45), 0.7071, 0);
      }
    }
    double detRel = coef(0.01, sr);
    double maxIn = -120, minG = 1;
    for (int i = 0; i < n; ++i) {
      double kl = L[i], kr = R[i];
      if (useHp) kl = khp_[0].process(kl), kr = khp_[1].process(kr);
      if (useLp) kl = klp_[0].process(kl), kr = klp_[1].process(kr);
      double x = std::max(std::fabs(kl), std::fabs(kr));
      maxIn = std::max(maxIn, gainToDb(x));
      det_ = x > det_ ? x : x + (det_ - x) * detRel;
      if (!open_ && det_ >= thOpen) open_ = true;
      if (open_ && det_ < thClose && holdLeft_ <= 0) open_ = false;
      if (det_ >= thClose)
        holdLeft_ = hold;
      else if (holdLeft_ > 0)
        --holdLeft_;
      bool on = open_ || holdLeft_ > 0;
      double target = flip ? (on ? floor : 1.0) : (on ? 1.0 : floor);
      g_ = target > g_ ? target + (g_ - target) * att : target + (g_ - target) * rel;
      dl_.push(L[i]);
      dr_.push(R[i]);
      double l = la > 0 ? dl_.tap(la + 1) : L[i], r = la > 0 ? dr_.tap(la + 1) : R[i];
      L[i] = static_cast<float>(l * g_);
      R[i] = static_cast<float>(r * g_);
      minG = std::min(minG, g_);
    }
    meter_ = static_cast<float>(gainToDb(minG));
    in_ = static_cast<float>(maxIn);
    state_ = open_ ? 1.f : 0.f;
  }
  void reset() override {
    det_ = 0, g_ = 0, holdLeft_ = 0, open_ = false;
    dl_.clear(), dr_.clear();
    for (int c = 0; c < 2; ++c) khp_[c].clear(), klp_[c].clear();
  }
  float meter() const override { return meter_; }
  float inputLevel() const override { return in_; }
  int meters(float* out, int max) const override {
    if (max < 1) return 0;
    out[0] = state_;
    return 1;
  }
  // The audio is delayed by the look-ahead.
  int latencyFor(const PluginState& s, double sr) const override { return lookahead(s, sr); }

 private:
  static int lookahead(const PluginState& s, double sr) {
    return std::clamp(static_cast<int>(param(s, pp::GtLookahead) * 0.001 * sr), 0, 1000);
  }
  double det_ = 0, g_ = 0, hp_ = -1, lp_ = -1, sr_ = 0;
  int64_t holdLeft_ = 0;
  bool open_ = false;
  FixedDelay<1024> dl_, dr_;
  Biquad khp_[2], klp_[2];
  float meter_ = 0, in_ = -120, state_ = 0;
};

// ======================================================================= Maximizer
// Low-cut → 3-band split (LR2/LR4) → per band: gain, compressor, width, saturation
// → band mix with the dry input → master drive, look-ahead limiter (two-stage release),
// width, soft ceiling → amount (dry/processed).
class MaximizerFx final : public Effect {
 public:
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    using namespace pp;
    const double sr = ctx.sampleRate;
    double f1 = param(s, MxLowFreq, 140), f2 = std::max(f1 * 1.5, static_cast<double>(param(s, MxHighFreq, 4000))), lc = param(s, MxLowCut, 20);
    bool lr4 = iparam(s, MxSlope, 1) == 1;
    if (f1 != lf1_ || f2 != lf2_ || lc != lc_ || sr != sr_ || lr4 != lr4_) {
      lf1_ = f1, lf2_ = f2, lc_ = lc, sr_ = sr, lr4_ = lr4;
      for (int c = 0; c < 2; ++c) {
        x1_[c].set(sr, f1, lr4);
        x2_[c].set(sr, f2, lr4);
        cut_[c].set(Biquad::HighPass, sr, lc, 0.7071, 0);
      }
    }
    double g[3], th[3], ratio[3], att[3], rel[3], width[3], sat[3];
    int mode[3];
    for (int b = 0; b < 3; ++b) {
      g[b] = dbToGain(param(s, MxLowGain + b));
      th[b] = param(s, MxLowThresh + b);
      ratio[b] = param(s, MxLowRatio + b, 3);
      att[b] = coef(param(s, MxLowAttack + b, 0.003f), sr);
      rel[b] = coef(param(s, MxLowRel + b, 0.15f), sr);
      mode[b] = iparam(s, MxLowMode + b);
      width[b] = param(s, MxLowWidth + b, 1);
      sat[b] = param(s, MxLowSat + b);
    }
    int solo = iparam(s, MxSolo);
    double bandMix = param(s, MxBandMix, 1), amount = param(s, MxAmount, 1), mwidth = param(s, MxWidth, 1);
    double drive = dbToGain(param(s, MxDrive, 4)), ceil = dbToGain(param(s, MxCeiling, -0.3f));
    double relT = param(s, MxRelease, 0.12f), rel2T = param(s, MxRelease2);
    double relC = coef(relT, sr), relR = 60.0 / std::max(1.0, relT * sr);
    double rel2C = coef(std::max(0.001, rel2T), sr);
    int relShape = iparam(s, MxRelCurve);
    double attC = coef(param(s, MxAttack, 0.001f), sr);
    int la = lookahead(s, sr);
    double minG = 0;
    float bandGr[3] = {0, 0, 0};
    for (int i = 0; i < n; ++i) {
      double dry[2] = {L[i], R[i]};
      double band[3][2];
      for (int c = 0; c < 2; ++c) {
        double x = cut_[c].process(dry[c]), lo, rest, mid, hi;
        x1_[c].split(x, lo, rest);
        x2_[c].split(rest, mid, hi);
        band[0][c] = lo, band[1][c] = mid, band[2][c] = hi;
      }
      double sum[2] = {0, 0};
      for (int b = 0; b < 3; ++b) {
        if (mode[b] == 2 || (solo > 0 && solo - 1 != b)) continue;
        double bl = band[b][0] * g[b], br = band[b][1] * g[b];
        if (mode[b] == 0) {
          double pk = std::max(std::fabs(bl), std::fabs(br));
          double gr = compCurve(gainToDb(pk), th[b], ratio[b], 6.0);
          benv_[b] = gr < benv_[b] ? gr + (benv_[b] - gr) * att[b] : gr + (benv_[b] - gr) * rel[b];
          double k = dbToGain(benv_[b]);
          bl *= k, br *= k;
          bandGr[b] = std::min(bandGr[b], static_cast<float>(benv_[b]));
        }
        if (width[b] != 1.0) {
          double m = 0.5 * (bl + br), sd = 0.5 * (bl - br) * width[b];
          bl = m + sd, br = m - sd;
        }
        if (sat[b] > 0) {
          double k = 1 + sat[b] * 4;
          bl = bl * (1 - sat[b]) + sat[b] * std::tanh(bl * k) / k * 1.3;
          br = br * (1 - sat[b]) + sat[b] * std::tanh(br * k) / k * 1.3;
        }
        sum[0] += bl, sum[1] += br;
      }
      double out[2];
      for (int c = 0; c < 2; ++c) out[c] = (dry[c] * (1 - bandMix) + sum[c] * bandMix) * drive;
      // Master stage: look-ahead peak limiter with optional second (slow) release.
      double pk = std::max(std::fabs(out[0]), std::fabs(out[1]));
      double want = pk > ceil ? gainToDb(ceil / pk) : 0.0;
      if (want < env_)
        env_ = want + (env_ - want) * attC;
      else
        env_ = curveStep(env_, want, relC, relR, relShape);
      if (rel2T > 0.0005) {  // slow plateau: never recovers faster than the second release
        env2_ = want < env2_ ? want : want + (env2_ - want) * rel2C;
        env_ = std::min(env_, env2_ * 0.5 + env_ * 0.5);
      }
      minG = std::min(minG, env_);
      dl_.push(static_cast<float>(out[0]));
      dr_.push(static_cast<float>(out[1]));
      ddl_.push(static_cast<float>(dry[0]));
      ddr_.push(static_cast<float>(dry[1]));
      double gl = dbToGain(env_);
      double y[2] = {(la > 0 ? dl_.tap(la + 1) : out[0]) * gl, (la > 0 ? dr_.tap(la + 1) : out[1]) * gl};
      if (mwidth != 1.0) {
        double m = 0.5 * (y[0] + y[1]), sd = 0.5 * (y[0] - y[1]) * mwidth;
        y[0] = m + sd, y[1] = m - sd;
      }
      double d[2] = {la > 0 ? ddl_.tap(la + 1) : dry[0], la > 0 ? ddr_.tap(la + 1) : dry[1]};
      for (int c = 0; c < 2; ++c) {
        double v = ceil * std::tanh(y[c] / ceil);  // soft ceiling catches what the envelope misses
        (c ? R : L)[i] = static_cast<float>(d[c] * (1 - amount) + v * amount);
      }
    }
    meter_ = static_cast<float>(minG);
    for (int b = 0; b < 3; ++b) bandGrOut_[b] = bandGr[b];
  }
  void reset() override {
    for (auto& c : x1_) c.clear();
    for (auto& c : x2_) c.clear();
    cut_[0].clear(), cut_[1].clear();
    env_ = env2_ = 0;
    benv_[0] = benv_[1] = benv_[2] = 0;
    dl_.clear(), dr_.clear(), ddl_.clear(), ddr_.clear();
  }
  float meter() const override { return meter_; }
  int meters(float* out, int max) const override {
    int k = std::min(max, 4);
    for (int i = 0; i < k && i < 3; ++i) out[i] = bandGrOut_[i];
    if (k > 3) out[3] = meter_;
    return k;
  }
  // The processed and the dry signal are both delayed by the look-ahead.
  int latencyFor(const PluginState& s, double sr) const override { return lookahead(s, sr); }

 private:
  static int lookahead(const PluginState& s, double sr) {
    return std::clamp(static_cast<int>(param(s, pp::MxLookahead) * 0.001 * sr), 0, 2000);
  }
  Crossover x1_[2], x2_[2];
  Biquad cut_[2];
  double lf1_ = -1, lf2_ = -1, lc_ = -1, sr_ = 0, env_ = 0, env2_ = 0;
  bool lr4_ = true;
  double benv_[3] = {0, 0, 0};
  FixedDelay<2048> dl_, dr_, ddl_, ddr_;
  float meter_ = 0, bandGrOut_[3] = {0, 0, 0};
};

// ======================================================================= Soft clipper
class ClipperFx final : public Effect {
 public:
  void process(const PluginState& s, const ProcessContext&, float* L, float* R, int n) override {
    using namespace pp;
    double T = dbToGain(param(s, ClpThreshold, -6)), post = dbToGain(param(s, ClpPostGain)), in = dbToGain(param(s, ClpInput));
    bool toFull = iparam(s, ClpCeilingMode) == 1;
    int os = stages(s);
    double mix = param(s, ClpMix, 1);
    double k = T * 2.0 / 3.0, span = T - k;
    auto clip = [&](double x) {
      double a = std::fabs(x), y;
      if (toFull) {  // reference curve: exponential approach from the threshold to full scale
        if (a <= T) return x;
        double range = std::max(1.5e-5, 1.0 - T);
        y = T + range * (1.0 - std::exp(-(a - T) / range));
      } else {
        if (a <= k) return x;
        y = k + span * (1 - std::exp(-(a - k) / span));
      }
      return x < 0 ? -y : y;
    };
    int lat = Oversampler::latency(os);
    double peak = 0;
    for (int i = 0; i < n; ++i) {
      double xl = L[i] * in, xr = R[i] * in;
      double yl = os ? osL_.run(xl, os, clip) : clip(xl), yr = os ? osR_.run(xr, os, clip) : clip(xr);
      dl_.push(static_cast<float>(L[i])), dr_.push(static_cast<float>(R[i]));
      double dl = lat ? dl_.tap(lat) : L[i], dr = lat ? dr_.tap(lat) : R[i];
      L[i] = static_cast<float>((dl * (1 - mix) + yl * mix) * post);
      R[i] = static_cast<float>((dr * (1 - mix) + yr * mix) * post);
      peak = std::max(peak, std::max(std::fabs(yl), std::fabs(yr)));
      peakIn_ = static_cast<float>(gainToDb(std::max(std::fabs(xl), std::fabs(xr))));
    }
    out_ = static_cast<float>(gainToDb(peak * post));
  }
  void reset() override {
    osL_.reset(), osR_.reset(), dl_.clear(), dr_.clear();
  }
  float inputLevel() const override { return peakIn_; }
  int meters(float* out, int max) const override {
    if (max < 1) return 0;
    out[0] = out_;
    return 1;
  }
  // Oversampling delays the wet path; the dry path is delayed to match.
  int latencyFor(const PluginState& s, double) const override { return Oversampler::latency(stages(s)); }

 private:
  static int stages(const PluginState& s) { return std::clamp(iparam(s, pp::ClpOversample), 0, 2); }
  Oversampler osL_, osR_;
  FixedDelay<64> dl_, dr_;
  float out_ = -120, peakIn_ = -120;
};

}  // namespace

std::unique_ptr<Effect> createDynK2Effect(const std::string& type);  // EffectsDynK2.cpp

std::unique_ptr<Effect> createEffectW3(const std::string& type) {
  if (auto fx = createDynK2Effect(type)) return fx;
  if (type == "gl.gate") return std::make_unique<GateFx>();
  if (type == "gl.maximizer") return std::make_unique<MaximizerFx>();
  if (type == "gl.clipper") return std::make_unique<ClipperFx>();
  return nullptr;
}

}  // namespace gl
