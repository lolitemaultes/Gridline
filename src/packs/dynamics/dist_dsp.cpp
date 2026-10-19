// Gridline — "dynamics" pack: Overdrive (gl.overdrive), Clip Dist
// (gl.fastdist), Squeeze (gl.squeeze) and Transient Split (gl.transplit).
#include "dyn_dsp.h"

namespace gl::packs::dynamics {

// ===================================================================== curves

double overdriveCurve(double x, double color) {
  // color 0.5 = symmetric; away from it the curve gets a bias (even harmonics)
  // and the negative side softens like a single-ended stage.
  double b = (clampd(color, 0, 1) - 0.5) * 0.9;
  double tb = std::tanh(b);
  double norm = 1.0 / (1.0 - tb * tb);
  double y = (std::tanh(x + b) - tb) * norm;
  return clampd(y, -2.5, 2.5);
}

double fastDistCurve(double x, double threshold, int type) {
  double T = clampd(threshold, 0.01, 1.0);
  if (type == 0) return clampd(x, -T, T);
  // Fold-back: reflections at ±T.
  double u = (x + T) / (4.0 * T);
  double f = u - std::floor(u);
  return T * (1.0 - 4.0 * std::fabs(f - 0.5));
}

// ===================================================================== gl.overdrive

class OverdriveFx final : public DynFx {
 public:
  OverdriveFx() : DynFx("gl.overdrive") {}
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    const double sr = ctx.sampleRate;
    const double band = clampd(p(s, OdPreBand), 0, 1), color = p(s, OdColor), amp = clampd(p(s, OdPreAmp), 0, 1);
    const double post = clampd(p(s, OdPostFilter), 0, 1);
    if (sr != sr_ || band != band_ || post != post_) {
      double hpHz = 20.0 * std::pow(40.0, band), lpHz = 20000.0 * std::pow(0.15, band);
      double postHz = 20000.0 * std::pow(250.0 / 20000.0, post);
      for (int c = 0; c < 2; ++c) {
        preHp_[c].set(sr, hpHz, 0.7071);
        preLp_[c].set(sr, lpHz, 0.7071);
        postLp_[c].set(sr, postHz, 0.7071);
        dc_[c].set(sr);
      }
      sr_ = sr, band_ = band, post_ = post;
    }
    double drive = std::pow(10.0, amp * 2.0);  // 0 … +40 dB
    if (pb(s, OdBoost)) drive *= 100.0;          // … +80 dB
    const double gain = dbToLin(p(s, OdPostGain));
    const int osf = oversampling(s);
    double peak = 0;
    for (int i = 0; i < n; ++i) {
      float* io[2] = {L + i, R + i};
      for (int c = 0; c < 2; ++c) {
        double x = *io[c];
        peak = std::max(peak, std::fabs(x));
        preHp_[c].tick(x);
        preLp_[c].tick(preHp_[c].hp);
        double v = preLp_[c].lp * drive;
        v = os_[c].process(v, osf, [&](double u) { return overdriveCurve(u, color); });
        v = dc_[c].process(v);
        postLp_[c].tick(v);
        *io[c] = static_cast<float>(postLp_[c].lp * gain);
      }
    }
    input_ = static_cast<float>(linToDb(peak));
  }
  // Wet only: the oversampled shaper delays the whole output.
  int latencyFor(const PluginState& s, double) const override { return osLatency(oversampling(s)); }
  void reset() override {
    for (int c = 0; c < 2; ++c) {
      preHp_[c].reset(); preLp_[c].reset(); postLp_[c].reset();
      dc_[c].reset();
      os_[c].reset();
    }
  }

 private:
  int oversampling(const PluginState& s) const { return osFactor(pi(s, OdOversample)); }
  SvfTpt preHp_[2], preLp_[2], postLp_[2];
  DcBlock dc_[2];
  Oversampler os_[2];
  double sr_ = 0, band_ = -1, post_ = -1;
};

// ===================================================================== gl.fastdist

double fastDistRef(double a, int type, int drive) {
  a = clampd(a, 0.0, 1.0);
  double t = clampd(drive, 1, 10);
  double d = a > 0 ? 20.0 * std::log10(a) : (type == 0 ? -10000.0 : -1000.0);
  if (type == 0) {
    if (d >= -25) d = (d + 25) * (11 - t) / 10 - 25;
    if (d <= -1.6 * (t - 1)) d += 1.6 * (t - 1);
  } else {
    d = -std::pow(-d, 0.8 + 0.02 * (10 - t));
  }
  return std::pow(10.0, d / 20.0);
}

class FastDistFx final : public DynFx {
 public:
  FastDistFx() : DynFx("gl.fastdist"), tab_(8193, 0.f) {}
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    (void)ctx;
    if (pb(s, FdClassic)) {
      classic(s, L, R, n);
      return;
    }
    const double pre = dbToLin(p(s, FdPreAmp)), thr = clampd(p(s, FdThreshold), 0.01, 1.0);
    const int type = pi(s, FdType);
    const double mix = clampd(p(s, FdMix), 0, 1), post = dbToLin(p(s, FdPost));
    double peak = 0;
    for (int i = 0; i < n; ++i) {
      float* io[2] = {L + i, R + i};
      for (int c = 0; c < 2; ++c) {
        double x = *io[c];
        peak = std::max(peak, std::fabs(x));
        double wet = os_[c].process(x * pre, 2, [&](double u) { return fastDistCurve(u, thr, type); });
        // The oversampler delays the wet path; keep the dry path aligned.
        dry_[c][pos_ & 15] = x;
        double d = dry_[c][(pos_ - kLatency) & 15];
        *io[c] = static_cast<float>((d + (wet - d) * mix) * post);
      }
      ++pos_;
    }
    input_ = static_cast<float>(linToDb(peak));
  }
  void reset() override {
    for (int c = 0; c < 2; ++c) {
      os_[c].reset();
      std::fill(dry_[c], dry_[c] + 16, 0.0);
    }
    pos_ = 0;
  }
  // Fixed 2x oversampling; the dry path is delayed to match (the reference engine is not oversampled).
  int latencyFor(const PluginState& s, double) const override { return pb(s, FdClassic) ? 0 : kLatency; }

 private:
  // Reference engine: out = sign(x) T(|x pre|) wet + x dry, T an 8192-step table (linear interpolation).
  static double g(double x) { return x == 1.0 ? 1.0 : 0.1 * (std::pow(11.0, x) - 1.0); }
  void classic(const PluginState& s, float* L, float* R, int n) {
    int type = clampd(pi(s, FdType), 0, 1), drive = static_cast<int>(clampd(std::lround(p(s, FdDrive)), 1, 10));
    if (type != tabType_ || drive != tabDrive_) {
      tabType_ = type, tabDrive_ = drive;
      for (int k = 0; k <= 8191; ++k) tab_[static_cast<size_t>(k)] = static_cast<float>(fastDistRef(k / 8191.0, type, drive));
      tab_[8192] = tab_[8191];
    }
    const double pre = g(p(s, FdPreK)), post = g(p(s, FdPostK)), wet = g(p(s, FdMixK)) * post, dry = (post - wet) * pre * post;
    double peak = 0;
    for (int i = 0; i < n; ++i) {
      float* io[2] = {L + i, R + i};
      for (int c = 0; c < 2; ++c) {
        double x = *io[c];
        peak = std::max(peak, std::fabs(x));
        double u = std::min(std::fabs(x * pre) * 8191.0, 8191.0);
        int k = static_cast<int>(u);
        double y = tab_[static_cast<size_t>(k)] + (tab_[static_cast<size_t>(k) + 1] - tab_[static_cast<size_t>(k)]) * (u - k);
        *io[c] = static_cast<float>((x < 0 ? -y : y) * wet + x * dry);
      }
    }
    input_ = static_cast<float>(linToDb(peak));
  }
  std::vector<float> tab_;
  int tabType_ = -1, tabDrive_ = -1;
  static constexpr int kLatency = 15;  // osLatency(2)
  Oversampler os_[2];
  double dry_[2][16] = {};
  uint32_t pos_ = 0;
};

// ===================================================================== gl.squeeze

class SqueezeFx final : public DynFx {
 public:
  SqueezeFx() : DynFx("gl.squeeze") {}
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    if (pb(s, SqModel)) {
      reference(s, ctx.sampleRate, L, R, n);
      return;
    }
    const double sr = ctx.sampleRate;
    const double gain = dbToLin(p(s, SqGain)), mix = clampd(p(s, SqMix), 0, 1);
    const double sq = clampd(p(s, SqSquare), 0, 1);
    const double steps = clampd(p(s, SqSteps), 2, 4096);
    const bool quant = steps < 4095.5;
    const double punch = clampd(p(s, SqPunchAmount), 0, 1);
    const double preserveS = p(s, SqPreserve) * 0.001, impactS = p(s, SqImpact) * 0.001;
    const int relation = std::clamp(pi(s, SqRelation), 0, 3);
    const double ringHz = p(s, SqRingFreq), ringAmt = clampd(p(s, SqRingAmount), 0, 1);
    const int div = std::max(1, pi(s, SqRateDiv));
    const bool fOn = pb(s, SqFilterOn);
    const int fType = pi(s, SqFilterType), fPos = pi(s, SqFilterPos);
    const double cutoff = p(s, SqCutoff), res = clampd(p(s, SqResonance), 0, 1);
    if (sr != sr_ || cutoff != cut_ || res != res_) {
      for (auto& f : filt_) f.set(sr, cutoff, 0.5 + res * 11.5);
      sr_ = sr, cut_ = cutoff, res_ = res;
    }
    const double ringInc = ringHz / sr;
    // Puncher: every zero-crossing cycle is measured; cycles longer than
    // `preserve` are kept, shorter ones are punched (inverted, squared or
    // gated, depending on the relation), and the first `impact` seconds after a
    // loud onset are left alone so attacks stay intact.
    const double preserveN = preserveS * sr, impactN = impactS * sr;
    const double onsetC = timeCoef(0.05, sr);
    auto filter = [&](int c, double x) {
      if (!fOn) return x;
      filt_[c].tick(x);
      switch (fType) {
        case 0: return filt_[c].lp;
        case 1: return filt_[c].hp;
        case 2: return filt_[c].bp;
        case 3: return x - filt_[c].k * filt_[c].bp;  // notch
        default: return x + filt_[c].bp * 2.0;        // peak
      }
    };
    double peak = 0;
    for (int i = 0; i < n; ++i) {
      double in[2] = {L[i] * gain, R[i] * gain};
      peak = std::max(peak, std::max(std::fabs(in[0]), std::fabs(in[1])));
      // sample-rate division (hold)
      if (holdCount_ <= 0) {
        held_[0] = in[0];
        held_[1] = in[1];
        holdCount_ = div;
      }
      --holdCount_;
      double ringV = std::sin(kTwoPi * ringPhase_);
      ringPhase_ += ringInc;
      ringPhase_ -= std::floor(ringPhase_);
      for (int c = 0; c < 2; ++c) {
        double x = div > 1 ? held_[c] : in[c];
        if (fPos == 0) x = filter(c, x);
        // squarize
        if (sq > 0) {
          double a = std::fabs(x);
          double shaped = std::pow(a, 1.0 / (1.0 + 9.0 * sq));
          double mag = a + (std::min(shaped, 1.0) * (a > 1e-6 ? 1.0 : 0.0) - a) * sq;
          x = x < 0 ? -mag : mag;
        }
        if (quant) x = std::round(x * steps * 0.5) / (steps * 0.5);
        // puncher
        if (punch > 0) {
          double a = std::fabs(x);
          env_[c] = a > env_[c] ? a : a + (env_[c] - a) * onsetC;
          if (a > env_[c] * 0.95 && a > 0.05 && sinceOnset_[c] > impactN) sinceOnset_[c] = 0;
          bool sign = x >= 0;
          if (sign != lastSign_[c]) {
            lastLen_[c] = cycle_[c];
            cycle_[c] = 0;
            ++cycleIndex_[c];
            lastSign_[c] = sign;
          }
          ++cycle_[c];
          ++sinceOnset_[c];
          bool protectedAttack = sinceOnset_[c] < impactN;
          bool shortCycle = lastLen_[c] < preserveN;
          if (!protectedAttack && shortCycle) {
            double y = x;
            switch (relation) {
              case 0: y = (cycleIndex_[c] & 1) ? -x : x; break;     // invert every other half-cycle
              case 1: y = (cycleIndex_[c] & 3) == 0 ? x : 0.0; break;  // keep one in four
              case 2: y = x < 0 ? 0.0 : x * 2.0; break;               // half-wave punch
              default: y = std::copysign(std::sqrt(std::fabs(x)), x); break;  // compand
            }
            x = x + (y - x) * punch;
          }
        }
        if (ringAmt > 0) x = x * (1.0 - ringAmt) + x * ringV * ringAmt;
        if (fPos == 1) x = filter(c, x);
        double dry = c ? R[i] : L[i];
        double out = dry + (x - dry) * mix;
        out = clampd(out, -8.0, 8.0);
        if (c)
          R[i] = static_cast<float>(out);
        else
          L[i] = static_cast<float>(out);
      }
    }
    input_ = static_cast<float>(linToDb(peak));
  }
  void reset() override {
    for (auto& f : filt_) f.reset();
    holdCount_ = 0;
    held_[0] = held_[1] = 0;
    ringPhase_ = 0;
    for (int c = 0; c < 2; ++c) {
      env_[c] = 0;
      cycle_[c] = lastLen_[c] = 0;
      cycleIndex_[c] = 0;
      lastSign_[c] = true;
      sinceOnset_[c] = 1e9;
      lp_[c] = bp_[c] = 0;
    }
    inImpact_ = false;
    cI_ = cP_ = 0;
  }

 private:
  // Reference engine (study: analysis/demos/specs/j26_squeeze.md).
  void reference(const PluginState& s, double sr, float* L, float* R, int n) {
    double peak = 0;
    for (int i = 0; i < n; ++i) peak = std::max(peak, static_cast<double>(std::max(std::fabs(L[i]), std::fabs(R[i]))));
    input_ = static_cast<float>(linToDb(peak));
    if (peak < 1e-6) return;  // silent block: passed through, state untouched
    const double g = clampd(p(s, SqGainRef), 0, 2), m = clampd(p(s, SqMix), 0, 1);
    const double S = p(s, SqStepsRef);
    const bool quant = S < 32768;
    const float Sf = static_cast<float>(S);
    const bool fOn = pb(s, SqFilterOn), post = pi(s, SqFilterPos) == 1;
    const int type = std::clamp(pi(s, SqFilterType), 0, 4);
    const double fc = sqCutoffHz(p(s, SqCutoffPos), sr), r = clampd(p(s, SqResonance), 0, 1);
    const double f = 2 * std::sin(kPi * std::min(0.25, fc / (2 * sr)));
    const double q = std::min(std::min(2.0, 2 / f - f / 2), 2 * (1 - std::pow(r, 0.25)));
    const double impactLen = p(s, SqImpactMs) * 0.001 * sr, preserveLen = p(s, SqPreserveMs) * 0.001 * sr;
    const double a = clampd(p(s, SqAmountRef), 0, 1);
    auto svf = [&](int c, double x) {
      double out = 0;
      for (int pass = 0; pass < 2; ++pass) {
        double nn = x - q * bp_[c];
        if (std::fabs(nn) < 1e-30) nn = 0;
        lp_[c] += f * bp_[c];
        double hp = nn - lp_[c];
        bp_[c] += f * hp;
        double o = type == 0 ? lp_[c] : type == 1 ? hp : type == 2 ? bp_[c] : type == 3 ? lp_[c] + hp : lp_[c] - hp;
        out += 0.5 * o;
      }
      return out;
    };
    for (int i = 0; i < n; ++i) {
      double wImp = 0, wPass = 1;
      if (impactLen > 0) {
        if (inImpact_) {
          if (cI_ > impactLen) {
            double t = cI_ - impactLen;
            wImp = 1 - t, wPass = t, cI_ = 0, cP_ = t, inImpact_ = false;
          } else {
            wImp = 1, wPass = 0, cI_ += 1;
          }
        } else {
          if (cP_ > preserveLen) {
            double t = cP_ - preserveLen;
            wImp = t, wPass = 1 - t, cI_ = t, cP_ = 0, inImpact_ = true;
          } else {
            wPass = 1, wImp = 0, cP_ += 1;
          }
        }
      }
      float* io[2] = {L + i, R + i};
      for (int c = 0; c < 2; ++c) {
        double x = *io[c], w = x;
        if (fOn && !post) w = svf(c, w);
        if (quant) w = std::nearbyint(static_cast<float>(w) * Sf) / Sf;
        if (impactLen > 0) w = w * (wImp * a) + w * wPass;
        if (fOn && post) w = svf(c, w);
        *io[c] = static_cast<float>((x * (1 - m) + w * m) * g);
      }
    }
  }
  double lp_[2] = {}, bp_[2] = {}, cI_ = 0, cP_ = 0;
  bool inImpact_ = false;
  SvfTpt filt_[2];
  double sr_ = 0, cut_ = -1, res_ = -1;
  int holdCount_ = 0;
  double held_[2] = {};
  double ringPhase_ = 0;
  double env_[2] = {};
  double cycle_[2] = {}, lastLen_[2] = {};
  uint32_t cycleIndex_[2] = {};
  bool lastSign_[2] = {true, true};
  double sinceOnset_[2] = {1e9, 1e9};
};

// ===================================================================== gl.transplit

class TransplitFx final : public DynFx {
 public:
  TransplitFx() : DynFx("gl.transplit") {}
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    const double sr = ctx.sampleRate;
    const double split = p(s, TsSplitFreq);
    if (sr != sr_ || split != split_) {
      for (auto& x : xo_) x.set(sr, split, 2);
      sr_ = sr, split_ = split;
    }
    const double bal = clampd(p(s, TsBalance), -1, 1);
    const double wLow = bal <= 0 ? 1.0 : 1.0 - bal, wHigh = bal >= 0 ? 1.0 : 1.0 + bal;
    const double atk = clampd(p(s, TsAttack), -1, 1), rel = clampd(p(s, TsRelease), -1, 1);
    static const double kFastRel[3] = {0.020, 0.045, 0.090};
    static const double kSlowAtk[3] = {0.012, 0.030, 0.070};
    static const double kTailRel[3] = {0.150, 0.400, 1.000};
    const int as = std::clamp(pi(s, TsAttackShape), 0, 2), rs = std::clamp(pi(s, TsReleaseShape), 0, 2);
    const double fa = timeCoef(0.0005, sr), fr = timeCoef(kFastRel[as], sr);
    const double sa = timeCoef(kSlowAtk[as], sr), srl = fr;  // same release: only attacks differ
    const double ta = timeCoef(0.002, sr), tr = timeCoef(kTailRel[rs], sr);
    const double drive = clampd(p(s, TsDrive), 0, 1), mix = clampd(p(s, TsMix), 0, 1), out = dbToLin(p(s, TsGain));
    const double dg = 1.0 + drive * 7.0;
    const bool fullBand = split <= 20.5;
    double peakIn = 0, peakGain = 0;
    for (int i = 0; i < n; ++i) {
      double x[2] = {L[i], R[i]};
      double a = std::max(std::fabs(x[0]), std::fabs(x[1]));
      peakIn = std::max(peakIn, a);
      double ef = fast_.process(a, fa, fr), es = slow_.process(a, sa, srl), et = tail_.process(a, ta, tr);
      double dAtk = clampd(linToDb(ef + 1e-9) - linToDb(es + 1e-9), 0, 12);
      double dSus = clampd(linToDb(et + 1e-9) - linToDb(ef + 1e-9), 0, 12);
      double gDb = atk * dAtk * 1.5 + rel * dSus * 1.5;
      if (std::fabs(gDb) > std::fabs(peakGain)) peakGain = gDb;
      double g = dbToLin(gDb);
      for (int c = 0; c < 2; ++c) {
        double lo, hi;
        if (fullBand) {
          lo = 0;
          hi = x[c];
        } else {
          xo_[c].process(x[c], lo, hi);
        }
        double y = lo * (1.0 + (g - 1.0) * wLow) + hi * (1.0 + (g - 1.0) * wHigh);
        if (drive > 0) y = std::tanh(y * dg) / std::tanh(dg);
        double dry = fullBand ? x[c] : lo + hi;
        double o = (dry + (y - dry) * mix) * out;
        if (c)
          R[i] = static_cast<float>(o);
        else
          L[i] = static_cast<float>(o);
      }
    }
    meter_ = static_cast<float>(peakGain);
    input_ = static_cast<float>(linToDb(peakIn));
  }
  void reset() override {
    for (auto& x : xo_) x.reset();
    fast_.v = slow_.v = tail_.v = 0;
  }

 private:
  Split xo_[2];
  Follower fast_, slow_, tail_;
  double sr_ = 0, split_ = -1;
};

std::unique_ptr<Effect> createDistEffect(const std::string& type) {
  if (type == "gl.overdrive") return std::make_unique<OverdriveFx>();
  if (type == "gl.fastdist") return std::make_unique<FastDistFx>();
  if (type == "gl.squeeze") return std::make_unique<SqueezeFx>();
  if (type == "gl.transplit") return std::make_unique<TransplitFx>();
  return nullptr;
}

}  // namespace gl::packs::dynamics
static_assert(gl::packs::dynamics::FdClassic == 9, "migration in normalizePluginState uses index 9");
