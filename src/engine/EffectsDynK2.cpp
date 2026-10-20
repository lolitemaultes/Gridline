// Gridline — dynamics, reference-parity runtimes (κ2): Limiter.
//
// Limiter signal flow (studied from the reference's behaviour; original code):
//   peak detector  = sliding maximum of max(|L|,|R|) over the "sustain" window
//                    (never shorter than the attack), then N release stages
//                    (instant rise, 1 %-settling time 2·release/√N each);
//   compressor     = RMS over its window (own or sidechain signal), N curve
//                    stages (attack / release), static curve above the
//                    threshold with ratio (compression up to 20:1 or
//                    expansion down to 1:4) and an optional bend ("knee");
//   gain           = input gain · compressor gain, lowered so that
//                    peak · gain never exceeds the ceiling; below the noise
//                    threshold the gain glides to the noise gain;
//   smoothing      = N symmetric attack stages (0.8·attack/√N each) on the
//                    gain, applied to the input delayed by the attack time
//                    (look-ahead), then a hard clip at the ceiling or the
//                    exponential soft saturation (threshold → min(ceiling, 1)).
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>
#include "../core/PluginParams.h"
#include "Dsp.h"
#include "Effects.h"

namespace gl {

using namespace dsp;

namespace {

float prm(const PluginState& s, int i, float dflt = 0.f) {
  return i >= 0 && i < static_cast<int>(s.params.size()) ? s.params[static_cast<size_t>(i)] : dflt;
}
int iprm(const PluginState& s, int i, int dflt = 0) { return static_cast<int>(std::lround(prm(s, i, static_cast<float>(dflt)))); }

// One envelope stage: separate rise / fall coefficients, each the per-sample
// factor that settles to 1 % after `samples` samples (0 = instant).
struct Stage {
  double up = 0, down = 0, s = 0;
  static double coefFor(double samples) { return samples <= 0 ? 0.0 : std::exp(-4.605170185988091 / std::max(1.0, samples)); }
  void set(double riseSamples, double fallSamples) {
    up = coefFor(riseSamples);
    down = coefFor(fallSamples);
  }
  double run(double x) {
    s = x > s ? x + (s - x) * up : x + (s - x) * down;
    if (std::fabs(s) < 1e-30) s = 0;
    return s;
  }
};

constexpr int kMaxStages = 8;
struct StageChain {
  Stage st[kMaxStages];
  int n = 0;
  void set(int count, double rise, double fall) {
    n = std::clamp(count, 0, kMaxStages);
    double k = n > 0 ? 1.0 / std::sqrt(static_cast<double>(n)) : 1.0;
    for (int i = 0; i < n; ++i) st[i].set(rise * k, fall * k);
  }
  double run(double x) {
    for (int i = 0; i < n; ++i) x = st[i].run(x);
    return x;
  }
  void reset(double v) {
    for (auto& s : st) s.s = v;
  }
};

// Sliding maximum over the last `w` samples (monotonic queue, fixed storage).
class SlidingMax {
 public:
  explicit SlidingMax(int cap) : cap_(cap), val_(static_cast<size_t>(cap)), idx_(static_cast<size_t>(cap)) {}
  void setWindow(int w) { w_ = std::clamp(w, 1, cap_ - 1); }
  float push(float x) {
    while (tail_ > head_ && val_[static_cast<size_t>((tail_ - 1) % cap_)] <= x) --tail_;
    val_[static_cast<size_t>(tail_ % cap_)] = x;
    idx_[static_cast<size_t>(tail_ % cap_)] = t_;
    ++tail_;
    while (idx_[static_cast<size_t>(head_ % cap_)] <= t_ - w_) ++head_;
    ++t_;
    return val_[static_cast<size_t>(head_ % cap_)];
  }
  void reset() { head_ = tail_ = t_ = 0; }

 private:
  int cap_, w_ = 1;
  int64_t head_ = 0, tail_ = 0, t_ = 0;
  std::vector<float> val_;
  std::vector<int64_t> idx_;
};

// Moving mean of squares over the last `w` samples.
class MovingRms {
 public:
  explicit MovingRms(int cap) : cap_(cap), ring_(static_cast<size_t>(cap), 0.f) {}
  void setWindow(int w) {
    w = std::clamp(w, 1, cap_ - 1);
    if (w != w_) {
      w_ = w;
      recompute();
    }
  }
  double push(double sq) {
    ring_[static_cast<size_t>(pos_)] = static_cast<float>(sq);
    sum_ += sq - ring_[static_cast<size_t>((pos_ - w_ + cap_) % cap_)];
    pos_ = (pos_ + 1) % cap_;
    if (++since_ >= 4096) recompute();
    return std::sqrt(std::max(0.0, sum_ / w_));
  }
  void reset() {
    std::fill(ring_.begin(), ring_.end(), 0.f);
    sum_ = 0;
    pos_ = 0;
  }

 private:
  void recompute() {
    since_ = 0;
    double s = 0;
    for (int k = 1; k <= w_; ++k) s += ring_[static_cast<size_t>((pos_ - k + cap_ * 2) % cap_)];
    sum_ = s;
  }
  int cap_, w_ = 1, pos_ = 0, since_ = 0;
  std::vector<float> ring_;
  double sum_ = 0;
};

// ======================================================================= Limiter
class LimiterK2 final : public Effect {
 public:
  static constexpr int kCap = 1 << 17;  // 1 s up to ~131 kHz
  LimiterK2() : dl_(kCap, 0.f), dr_(kCap, 0.f), peak_(kCap), rms_(kCap) {}
  void setSideIo(const SideIo& io) override { io_ = io; }
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    using namespace pp;
    const double sr = ctx.sampleRate;
    auto smp = [&](double sec) { return sec * sr; };
    const double pre = dbToGain(prm(s, LimGain));
    const double ceil = dbToGain(prm(s, LimCeiling));
    const double ceilEff = ceil * (prm(s, LimSafety) >= 0.5f ? 0.975 : 1.0);
    // Attack = look-ahead delay; the peak window never shorter than it.
    const int A = lookahead(s, sr);
    const int W = std::max(std::max(A, 1), static_cast<int>(smp(prm(s, LimHold, 0.01f))));
    peak_.setWindow(W);
    atkStages_.set(A > 0 ? iprm(s, LimAtkCurve, 2) + 1 : 0, 0.8 * A, 0.8 * A);
    const double rel = prm(s, LimRelease, 0.0855f);
    relStages_.set(rel > 0 ? iprm(s, LimRelCurve, 2) + 1 : 0, 0.0, smp(rel));
    // Compressor.
    const double thrDb = prm(s, LimCompThresh);
    const bool compOn = thrDb < -0.01;
    const double thr = std::max(dbToGain(thrDb), dbToGain(-60.0));
    const double ratio = std::max(0.25, static_cast<double>(prm(s, LimCompRatio, 1)));
    const double slope = 1.0 / ratio;  // < 1 compresses, > 1 expands
    const double shape = std::clamp(static_cast<double>(prm(s, LimCompShape)), -1.0, 1.0);
    const double softKnee = std::max(0.0, static_cast<double>(prm(s, LimCompKnee)));
    // Shape factor k (same law as the reference: 0.005·(201^|x|−1) bending
    // down, 0.001·(201^|x|−1) bending up).
    double k = 0;
    if (shape > 0) k = 0.005 * (std::pow(201.0, shape) - 1.0);
    else if (shape < 0) k = 0.001 * (std::pow(201.0, -shape) - 1.0);
    const double lnk = std::log1p(k);
    rms_.setWindow(std::max(1, static_cast<int>(smp(prm(s, LimCompRms, 0.001f)))));
    double catk = prm(s, LimCompAttack, 0.0001f);
    if (catk < 0.00011) catk = 0;
    compStages_.set(iprm(s, LimCompCurve) + 1, smp(catk), smp(prm(s, LimCompRelease, 0.248f)));
    const bool side = iprm(s, LimSidechain) > 0 && io_.scL && io_.scR;
    // Noise gate.
    const double gThrDb = prm(s, LimGateThresh, -80);
    const double gateThr = gThrDb <= -79.9 ? 0.0 : std::min(dbToGain(gThrDb), ceilEff);
    const double noiseGain = prm(s, LimGateFloor) <= -79.9f ? 0.0 : dbToGain(prm(s, LimGateFloor));
    const double gateCoef = std::exp(-4.605170185988091 / std::max(1.0, smp(prm(s, LimGateRelease, 0.05f))));
    // Output stage.
    const double satT = dbToGain(prm(s, LimSaturation));
    const bool sat = satT < 0.999;
    const double satCeil = std::min(ceil, 1.0);
    const double satRange = std::max(1.5e-5, satCeil - satT), satInv = 1.0 / satRange;
    double minG = 0, minComp = 0, minGate = 0, outPk = 0, maxIn = 0;
    for (int i = 0; i < n; ++i) {
      const double xl = L[i], xr = R[i];
      const double pk = std::max(std::fabs(xl), std::fabs(xr));
      maxIn = std::max(maxIn, pk);
      double env = relStages_.run(peak_.push(static_cast<float>(pk)));
      double g = pre;
      if (compOn) {
        double kl = side ? io_.scL[i] : xl, kr = side ? io_.scR[i] : xr;
        double lv = compStages_.run(rms_.push(0.5 * (kl * kl + kr * kr)));
        double cg = compGainDb(lv, thr, slope, k, lnk, shape, softKnee);
        g = pre * dbToGain(cg);
        minComp = std::min(minComp, cg);
      } else {
        rms_.push(0);
      }
      if (env * g > ceilEff) g = ceilEff / env;
      if (env < gateThr) {
        double before = g;
        g = (prevG_ - noiseGain) * gateCoef + noiseGain;
        if (before > 1e-12) minGate = std::min(minGate, gainToDb(std::max(1e-9, g / before)));
      }
      prevG_ = g;
      g = atkStages_.run(g);
      // Look-ahead delay.
      dl_[static_cast<size_t>(wp_)] = static_cast<float>(xl);
      dr_[static_cast<size_t>(wp_)] = static_cast<float>(xr);
      int rp = (wp_ - A + kCap) % kCap;
      wp_ = (wp_ + 1) % kCap;
      double yl = dl_[static_cast<size_t>(rp)] * g, yr = dr_[static_cast<size_t>(rp)] * g;
      if (sat) {
        yl = saturate(yl, satT, satRange, satInv);
        yr = saturate(yr, satT, satRange, satInv);
      } else {
        yl = std::clamp(yl, -ceil, ceil);
        yr = std::clamp(yr, -ceil, ceil);
      }
      L[i] = static_cast<float>(yl);
      R[i] = static_cast<float>(yr);
      outPk = std::max(outPk, std::max(std::fabs(yl), std::fabs(yr)));
      minG = std::min(minG, gainToDb(std::max(1e-9, g / pre)));
    }
    meter_ = static_cast<float>(minG);
    in_ = static_cast<float>(gainToDb(maxIn));
    m_[0] = static_cast<float>(minG);
    m_[1] = static_cast<float>(minComp);
    m_[2] = static_cast<float>(minGate);
    m_[3] = static_cast<float>(gainToDb(outPk));
  }

  // Static compressor curve (dB of gain change) for a detector level.
  static double compGainDb(double lv, double thr, double slope, double k, double lnk, double shape, double softKnee) {
    double lvDb = gainToDb(std::max(lv, 1e-9)), thrDb = gainToDb(thr);
    double over = gainToDb(std::min(lv, thr * 100.0) / thr);
    if (softKnee > 0) {  // Gridline extra: quadratic soft knee around the threshold
      double o = lvDb - thrDb;
      if (o < -softKnee * 0.5) return 0.0;
      if (o < softKnee * 0.5) {
        double e = o + softKnee * 0.5;
        return (slope - 1.0) * e * e / (2.0 * softKnee);
      }
    } else if (lv <= thr) {
      return 0.0;
    }
    if (over <= 0) return 0.0;
    double h = over;
    if (shape > 0 && k > 0) h = std::log1p(k * (over + 1.0)) / lnk - 1.0;
    else if (shape < 0 && k > 0) h = (std::exp((over + 1.0) * lnk) - 1.0) / k - 1.0;
    return slope * h - over;
  }

  static double saturate(double x, double t, double range, double inv) {
    double a = std::fabs(x);
    if (a <= t) return x;
    double y = t + range * (1.0 - std::exp(-(a - t) * inv));
    return x < 0 ? -y : y;
  }

  void reset() override {
    std::fill(dl_.begin(), dl_.end(), 0.f);
    std::fill(dr_.begin(), dr_.end(), 0.f);
    peak_.reset();
    rms_.reset();
    atkStages_.reset(1.0);
    relStages_.reset(0.0);
    compStages_.reset(0.0);
    prevG_ = 1.0;
    wp_ = 0;
  }
  float meter() const override { return meter_; }
  float inputLevel() const override { return in_; }
  int meters(float* out, int max) const override {
    int c = std::min(max, 4);
    for (int i = 0; i < c; ++i) out[i] = m_[i];
    return c;
  }
  // The audio is delayed by the attack (look-ahead) time.
  int latencyFor(const PluginState& s, double sr) const override { return lookahead(s, sr); }

 private:
  static int lookahead(const PluginState& s, double sr) {
    double att = prm(s, pp::LimAttack, 0.002f);
    if (att < 0.00011) att = 0;
    return std::clamp(static_cast<int>(att * sr), 0, kCap - 2);
  }
  SideIo io_;
  std::vector<float> dl_, dr_;
  SlidingMax peak_;
  MovingRms rms_;
  StageChain atkStages_, relStages_, compStages_;
  double prevG_ = 1.0;
  int wp_ = 0;
  float meter_ = 0, in_ = -120, m_[4] = {0, 0, 0, -120};
};

// ======================================================================= Compressor
// Reference-compressor flow: 3 Hz DC high-pass on the signal path; detector =
// max(|L|,|R|) (or the sidechain) through a fast peak follower (0.01 ms up,
// 1 ms down); log-domain gain curve with a quadratic soft knee (knee width
// from the type), or the "vintage" curve (smoothstep knee, reduction that
// stops growing above the knee); gain smoothing with attack and a
// program-dependent release (faster after deep reduction for the fast types);
// make-up gain; 0 dBFS output limiter with a 64-sample look-ahead.
// All time constants: per-sample factor reaching 10 % after the given time.
class CompK2 final : public Effect {
 public:
  static constexpr int kLa = 64;
  void setSideIo(const SideIo& io) override { io_ = io; }
  static double coef10(double ms, double sr) { return ms <= 0 ? 0.0 : std::pow(0.1, 1.0 / (ms * sr * 0.001)); }

  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    using namespace pp;
    const double sr = ctx.sampleRate;
    if (sr != sr_) {
      sr_ = sr;
      double t = std::tan(3.0 * kPi / sr);
      hpC_ = (t - 1.0) / (t + 1.0);
    }
    // ---- settings (reference recalculation)
    const double thr = dbToGain(prm(s, CmpThreshold, -18));
    const double ratio = std::max(0.4, static_cast<double>(prm(s, CmpRatio, 4)));
    double slope = 1.0 / ratio - 1.0;
    const double makeup = dbToGain(prm(s, CmpOutput) + prm(s, CmpMakeup));
    const double kneeDb = std::max(0.0, static_cast<double>(prm(s, CmpKnee, 6)));
    const bool vintage = iprm(s, CmpCharacter) == 1;
    const bool fast = prm(s, CmpFastRelease) >= 0.5f;
    const double atkMs = prm(s, CmpAttack, 0.005f) * 1000.0, relMs = prm(s, CmpRelease, 0.12f) * 1000.0;
    const double detUp = coef10(0.01, sr), detDown = coef10(1.0, sr);
    const double gAtk = coef10(atkMs < 0.11 ? 0.0 : atkMs, sr);
    const double relFastC = coef10(relMs * (fast ? 0.1 : 1.0), sr), relC = coef10(relMs, sr), holdC = relC;
    const double kneeRatio = dbToGain(-kneeDb);
    const double lnThr = std::log(thr);
    const double ks = std::log(thr * kneeRatio);
    double ke = std::log(thr / kneeRatio);
    double kneeHalf = std::min(1.0, lnThr - ks);
    double above = 1.0;  // vintage: constant gain above the knee
    if (vintage) {
      ke = lnThr + 1.25 * std::pow(ratio, 0.25);
      above = std::exp((ke - lnThr) * slope);
      ke = ke - std::log(kneeRatio);
      slope = std::log(above) / (ke - lnThr);
    }
    const double kneeQuad = ke > ks ? 0.5 / (ke - ks) : 0.0;
    const double invSpan = ke > ks ? 1.0 / (ke - ks) : 0.0;
    const double vintA = (ke - lnThr) * slope;
    const bool rmsDet = iprm(s, CmpDetector) == 1;
    const double rmsC = std::exp(-1.0 / std::max(1.0, 0.015 * sr));
    const double hpf = prm(s, CmpKeyHpf, 20);
    const bool useHpf = hpf > 20.5;
    if (useHpf && hpf != keyHz_) {
      keyHz_ = hpf;
      for (auto& k : key_) k.set(Biquad::HighPass, sr, hpf, 0.7071, 0);
    }
    const int la = lookahead(s, sr);
    const double mix = std::clamp(static_cast<double>(prm(s, CmpMix, 1)), 0.0, 1.0);
    const double dryTrim = dbToGain(prm(s, CmpOutput));
    const bool limiter = limiterOn(s);
    const bool side = iprm(s, CmpSidechain) > 0 && io_.scL && io_.scR;
    double maxGr = 0, maxIn = 0;
    for (int i = 0; i < n; ++i) {
      // 3 Hz DC high-pass: half the difference between the input and a first-order all-pass.
      double xl = L[i] + 1e-30, xr = R[i] + 1e-30;
      apL_ = (xl - apL_) * hpC_ + xlPrev_;
      xlPrev_ = xl;
      double sl = (xl - apL_) * 0.5;
      apR_ = (xr - apR_) * hpC_ + xrPrev_;
      xrPrev_ = xr;
      double srr = (xr - apR_) * 0.5;
      double kl = side ? io_.scL[i] : sl, kr = side ? io_.scR[i] : srr;
      if (useHpf) kl = key_[0].process(kl), kr = key_[1].process(kr);
      double det = std::max(std::fabs(kl), std::fabs(kr));
      maxIn = std::max(maxIn, det);
      if (rmsDet) {
        ms_ = det * det + (ms_ - det * det) * rmsC;
        det = std::sqrt(ms_ * 2.0);
      }
      det += 1e-30;
      env_ = det + (env_ - det) * (det <= env_ ? detDown : detUp);
      double lv = std::log(env_), g = 1.0;
      if (lv >= ks) {
        if (!vintage) {
          if (lv < ke) {
            double a = lv - ks;
            g = std::exp(slope * a * a * kneeQuad);
          } else {
            g = std::exp((lv - lnThr) * slope);
          }
        } else if (lv < ke) {
          double u = (lv - ks) * invSpan;
          double sm = (3.0 - 2.0 * u) * u * u;
          u = (sm - u) * kneeHalf + u;
          g = std::exp(vintA * u);
        } else {
          g = above;
        }
      }
      gs_ = g + (gs_ - g) * (g <= gs_ ? gAtk : relDyn_);
      hold_ = (hold_ - gs_) * holdC + gs_;
      if (hold_ < gs_) hold_ = gs_;
      double depth = vintage ? (0.707 - gs_) * 1.414 : hold_ - gs_;
      relDyn_ = std::max(0.0, (relFastC - relC) * depth + relC);
      maxGr = std::min(maxGr, gainToDb(gs_));
      // Optional look-ahead (Gridline): delay the audio relative to the detector.
      dl_[wp_] = static_cast<float>(sl);
      dr_[wp_] = static_cast<float>(srr);
      rawL_[wp_] = L[i];
      rawR_[wp_] = R[i];
      int rp = (wp_ - la + kDly) % kDly;
      wp_ = (wp_ + 1) % kDly;
      double gg = gs_ * makeup;
      double wl = dl_[rp] * gg, wr = dr_[rp] * gg;
      double dl = rawL_[rp] * dryTrim, dr = rawR_[rp] * dryTrim;  // Gridline mix: untouched input
      double ol = dl + (wl - dl) * mix, orr = dr + (wr - dr) * mix;
      if (limiter) outLimit(ol, orr);
      L[i] = static_cast<float>(ol);
      R[i] = static_cast<float>(orr);
    }
    meter_ = static_cast<float>(maxGr);
    in_ = static_cast<float>(gainToDb(maxIn));
  }

  // 0 dBFS limiter: 64-sample look-ahead, smoothstep attack ramp, adaptive recovery.
  void outLimit(double& l, double& r) {
    double pk = std::max(std::fabs(l), std::fabs(r));
    auto recover = [&](double target) {
      if (cnt_ < 1) {
        rate_ = (0.09 - rate_) * 5e-5 + rate_;
        lg_ = (target - lg_) * rate_ + lg_;
      }
    };
    if (pk <= 1.0) {
      recover(1.0);
    } else {
      double need = 0.9885531 / pk;
      if (lg_ <= need) {
        recover(need);
      } else {
        lg_ = need;
        double cur = curG_;
        if (cnt_ < 1)
          for (double& v : gr_) v = cur;
        cnt_ = kLa;
        rate_ = 0;
        for (int j = 0; j < kLa * 2; ++j) {
          int idx = (gpos_ + j) % (kLa * 2);
          double w = std::min(1.0, static_cast<double>(j + 1) / kLa);
          double v = (3.0 - 2.0 * w) * (need - cur) * w * w + cur;
          if (gr_[idx] > v) gr_[idx] = v;
        }
      }
    }
    if (cnt_ > 0) --cnt_;
    curG_ = gr_[gpos_];
    gr_[gpos_] = lg_;
    gpos_ = (gpos_ + 1) % (kLa * 2);
    // Audio delayed by the look-ahead.
    double ol = al_[apos_], orr = ar_[apos_];
    al_[apos_] = l;
    ar_[apos_] = r;
    apos_ = (apos_ + 1) % kLa;
    l = ol * curG_;
    r = orr * curG_;
  }

  void reset() override {
    env_ = 1e-30, gs_ = 1, hold_ = 1, ms_ = 0;
    apL_ = apR_ = xlPrev_ = xrPrev_ = 0;
    relDyn_ = 0;
    lg_ = 1, curG_ = 1, rate_ = 0.01;
    cnt_ = 0, gpos_ = 0, apos_ = 0, wp_ = 0;
    for (double& v : gr_) v = 1;
    for (double& v : al_) v = 0;
    for (double& v : ar_) v = 0;
    std::fill(std::begin(dl_), std::end(dl_), 0.f);
    std::fill(std::begin(dr_), std::end(dr_), 0.f);
    std::fill(std::begin(rawL_), std::end(rawL_), 0.f);
    std::fill(std::begin(rawR_), std::end(rawR_), 0.f);
    key_[0].clear(), key_[1].clear();
  }
  float meter() const override { return meter_; }
  float inputLevel() const override { return in_; }
  // Optional look-ahead delay of the audio, plus the output limiter's
  // fixed 64-sample look-ahead when it is on.
  int latencyFor(const PluginState& s, double sr) const override {
    return lookahead(s, sr) + (limiterOn(s) ? kLa : 0);
  }

 private:
  static constexpr int kDly = 2048;
  static int lookahead(const PluginState& s, double sr) {
    return std::clamp(static_cast<int>(prm(s, pp::CmpLookahead) * 0.001 * sr), 0, kDly - 2);
  }
  static bool limiterOn(const PluginState& s) { return prm(s, pp::CmpLimiter, 1) >= 0.5f; }
  SideIo io_;
  double sr_ = 0, hpC_ = 0, keyHz_ = -1;
  double env_ = 1e-30, gs_ = 1, hold_ = 1, ms_ = 0, relDyn_ = 0;
  double apL_ = 0, apR_ = 0, xlPrev_ = 0, xrPrev_ = 0;
  double lg_ = 1, curG_ = 1, rate_ = 0.01, gr_[kLa * 2] = {}, al_[kLa] = {}, ar_[kLa] = {};
  int cnt_ = 0, gpos_ = 0, apos_ = 0, wp_ = 0;
  float dl_[kDly] = {}, dr_[kDly] = {}, rawL_[kDly] = {}, rawR_[kDly] = {};
  Biquad key_[2];
  float meter_ = 0, in_ = -120;
};

// ======================================================================= Multiband
// Reference multiband engine: each band has its own edge filters (low band:
// low-pass at its edge; mid band: high-pass at its low edge and low-pass at its
// high edge; high band: high-pass at its edge), as 4th-order Butterworth
// (two biquads, Q 0.5412 / 1.3066) or 105-tap Kaiser-windowed linear-phase FIR
// (Gridline adds a flat Linkwitz-Riley option). Band compressor: peak hold of
// max(|L|,|R|) with exponential release, one-pole low-pass smoothing at
// 1 / attack Hz, linear-domain knee around the threshold, gain
// T / (T + over · (1 - 1/ratio)). Band gain, sum, master gain, optional output
// limiter (same envelope with a fast adaptive release + a soft clip at 0.998).

// Envelope + gain computer of the reference engine (linear amplitudes).
struct MbEnv {
  double T = 1, ratioN = 0.5, kk = 0.64, rel = 0.1, sr = 44100;
  double held = 0, peak0 = 0, lp = 0, lpX = 0, a = 1, b = 0;
  double fac = 1.0;  // adaptive release factor (limiter variant)
  int64_t count = 0;
  bool adaptive = false;
  void set(double sampleRate, double thr, double ratioNorm, double attackSec, double releaseSec, double hard) {
    sr = sampleRate;
    T = thr;
    ratioN = ratioNorm;
    rel = std::max(1e-4, releaseSec);
    kk = hard * hard * 0.995 + 0.005;
    double fc = std::min(1.0 / (attackSec + 5.882353e-05), sr * 0.49);
    double t = std::tan(kPi * fc / sr);
    a = t / (1.0 + t);
    b = (1.0 - t) / (1.0 + t);
  }
  // Held peak, decaying exponentially (time constant rel × fac) for 10 s after
  // it was set. J-30: the decay steps by a cached per-sample factor instead of
  // an exp() per sample (re-anchored exactly whenever the time constant
  // changes; drift < 1e-10 relative over the 10 s).
  double peakHold(double x) {
    if (x > held) {
      held = peak0 = x;
      count = 0;
      if (adaptive) {
        if (fac < 1.0) fac = 1.0;
        else fac = std::max(0.01, 1.0 - 0.4 * (x - 1.0));
      }
    } else if (count <= static_cast<int64_t>(sr * 10.0)) {
      const double tau = rel * (adaptive ? fac : 1.0);
      if (count == 0 || tau != stepTau || sr != stepSr) {
        held = peak0 * std::exp(-(static_cast<double>(count) / sr) / tau);
        step = std::exp(-(1.0 / sr) / tau);
        stepTau = tau, stepSr = sr;
      } else {
        held *= step;
      }
      ++count;
    }
    return held;
  }
  double step = 1, stepTau = -1, stepSr = 0;
  double over(double x) const {
    double lo = kk * T - 0.005, hi = 2.0 * T - lo;
    if (x < lo) return 0.0;
    if (x > hi) return x - T;
    double c = 1.0 / ((lo - T) * 4.0), d = -2.0 * hi * c, e = c * hi * hi + T;
    return x - (c * x * x + d * x + e);
  }
  double gain(double level) {
    double h = peakHold(level);
    double y = (lpX + h) * a + b * lp;
    lpX = h;
    lp = std::fabs(y) < 1e-30 ? 0.0 : y;
    double o = over(lp);
    return T / (T + o * ratioN);
  }
  void reset() {
    held = peak0 = lp = lpX = 0;
    count = 0;
    fac = 1.0;
  }
};

// 105-tap linear-phase FIR (Kaiser beta 5.10226) with a trapezoid response
// (transition 0.83·f .. 1.17·f), as low-pass, high-pass (delta minus low-pass)
// or band-pass (difference of two low-passes).
struct MbFir {
  static constexpr int N = 105, H = 52;
  double h[N] = {};
  double z[2][N] = {};
  int pos = 0;
  static double kaiser(int k) {
    auto bessel = [](double x) {
      double s = 1.0, t = 1.0;
      for (int i = 1; i < 16; ++i) {
        t *= (x * 0.5) / i;
        s += t * t;
      }
      return s;
    };
    double r = (2.0 * k) / 104.0, w = std::max(0.0, 1.0 - r * r);
    return bessel(5.10226 * std::sqrt(w)) / bessel(5.10226);
  }
  static void lowpass(double* out, double f, double sr) {
    double f1 = 0.83 * f, f2 = std::min(1.17 * f, sr * 0.5 - 1.0);
    for (int n = -H; n <= H; ++n) {
      double v;
      if (n == 0) v = (f1 + f2) / sr;
      else {
        double w1 = 2 * kPi * f1 / sr, w2 = 2 * kPi * f2 / sr;
        v = (std::cos(w1 * n) - std::cos(w2 * n)) / (kPi * n * n * (w2 - w1));
      }
      out[n + H] = v * kaiser(std::abs(n));
    }
  }
  void design(int kind, double fLo, double fHi, double sr) {  // kind 0 LP(fHi), 1 HP(fLo), 2 BP
    double lp[N];
    if (kind == 0) {
      lowpass(h, fHi, sr);
    } else if (kind == 1) {
      lowpass(lp, fLo, sr);
      for (int i = 0; i < N; ++i) h[i] = (i == H ? 1.0 : 0.0) - lp[i];
    } else {
      lowpass(h, fHi, sr);
      lowpass(lp, fLo, sr);
      for (int i = 0; i < N; ++i) h[i] -= lp[i];
    }
  }
  double run(int c, double x) {
    z[c][pos] = x;
    double s = 0;
    int p = pos;
    for (int i = 0; i < N; ++i) {
      s += h[i] * z[c][p];
      p = p == 0 ? N - 1 : p - 1;
    }
    return s;
  }
  void advance() { pos = (pos + 1) % N; }
  void clear() {
    for (auto& r : z)
      for (double& v : r) v = 0;
    pos = 0;
  }
};

// The three band FIRs over one shared input history (J-30): the history is
// stored twice in a row so each tap sum reads contiguous memory without a
// wrap test, and the three sums share every load. Each sum adds the same
// products in the same order as MbFir::run (identical output).
struct MbFirBank {
  static constexpr int N = MbFir::N;
  double h[3][N] = {};
  double z[2][2 * N] = {};
  int pos = 0;
  void run(int c, double x, double out[3]) {
    double* zz = z[c];
    zz[pos] = x;
    zz[pos + N] = x;
    const double* p = zz + pos + N;  // p[-i] = input i samples ago
    double s0 = 0, s1 = 0, s2 = 0;
    for (int i = 0; i < N; ++i) {
      const double v = p[-i];
      s0 += h[0][i] * v;
      s1 += h[1][i] * v;
      s2 += h[2][i] * v;
    }
    out[0] = s0, out[1] = s1, out[2] = s2;
  }
  void advance() { pos = pos + 1 == N ? 0 : pos + 1; }
  void clear() {
    for (auto& r : z)
      for (double& v : r) v = 0;
    pos = 0;
  }
};

class MultibandK2 final : public Effect {
 public:
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    using namespace pp;
    const double sr = ctx.sampleRate;
    const bool indep = iprm(s, MbEdgeMode) == 1;
    double fL = prm(s, MbLowXover, 300.3f), fH = prm(s, MbHighXover, 9236.7f);
    double fML = indep ? prm(s, MbMidLowEdge, 300.3f) : fL, fMH = indep ? prm(s, MbMidHighEdge, 9236.7f) : fH;
    auto clampF = [&](double f) { return std::clamp(f, 20.0, sr * 0.45); };
    fL = clampF(fL), fH = clampF(fH), fML = clampF(fML), fMH = clampF(fMH);
    const int ftype = filterType(s);
    if (fL != f_[0] || fH != f_[1] || fML != f_[2] || fMH != f_[3] || sr != sr_ || ftype != ftype_) {
      f_[0] = fL, f_[1] = fH, f_[2] = fML, f_[3] = fMH, sr_ = sr, ftype_ = ftype;
      designFilters();
    }
    const double master = dbToGain(prm(s, MbOutput));
    const bool lim = prm(s, MbLimiter) >= 0.5f;
    double gainB[3];
    int state[3];
    for (int b = 0; b < 3; ++b) {
      double ratio = prm(s, MbLowRatio + b * 3, 2);
      double rn = ratio >= 999.0 ? 1.0 : 1.0 - 1.0 / std::max(1.0, ratio);
      double atk = prm(s, MbLowAtk + b, 0.01f);
      double attN = std::clamp((atk * 1000.0 - 0.1) / 99.9, 0.0, 1.0);
      env_[b].set(sr, dbToGain(prm(s, MbLowThresh + b * 3, -18)), rn, attN * 0.1, prm(s, MbLowRel + b, 0.1f), prm(s, MbLowHard + b, 0.8f));
      gainB[b] = dbToGain(prm(s, MbLowGain + b * 3));
      state[b] = std::clamp(iprm(s, MbLowState + b), 0, 2);
    }
    double minCg[3] = {1, 1, 1};  // lowest compressor gain of the block (meter; dB taken once)
    double outPk = 0;
    for (int i = 0; i < n; ++i) {
      double x[2] = {L[i], R[i]}, band[3][2];
      splitSample(x, band);
      double out[2] = {0, 0};
      for (int b = 0; b < 3; ++b) {
        if (state[b] == 2) continue;  // muted
        double g = gainB[b];
        if (state[b] == 0) {
          double cg = env_[b].gain(std::max(std::fabs(band[b][0]), std::fabs(band[b][1])));
          g *= cg;
          minCg[b] = std::min(minCg[b], cg);
        }
        out[0] += band[b][0] * g;
        out[1] += band[b][1] * g;
      }
      for (double& v : out) v *= master;
      if (lim)
        for (int c = 0; c < 2; ++c) {
          double gg = limEnv_[c].gain(std::fabs(out[c]));
          out[c] = softClip(out[c] * gg) * 0.99999;
        }
      L[i] = static_cast<float>(out[0]);
      R[i] = static_cast<float>(out[1]);
      outPk = std::max(outPk, std::max(std::fabs(out[0]), std::fabs(out[1])));
    }
    for (int b = 0; b < 3; ++b) gr_[b] = std::min(0.0f, static_cast<float>(gainToDb(minCg[b])));
    level_ = static_cast<float>(gainToDb(outPk));
  }

  static double softClip(double x) {
    double a = std::fabs(x);
    if (a < 0.998) return x;
    double u = std::min(1.0, (a - 0.998) / 0.0029999614);
    double y = 0.998 + (u >= 1.0 ? 1.0 : (u * (-0.5 * u * u + 1.5))) * 0.0019999743;
    return x < 0 ? -y : y;
  }

  void reset() override {
    for (auto& e : env_) e.reset();
    for (auto& e : limEnv_) e.reset();
    for (auto& c : iir_)
      for (auto& q : c) q.clear();
    firBank_.clear();
    for (auto& c : lr_)
      for (auto& q : c) q.clear();
  }
  float meter() const override { return std::min({gr_[0], gr_[1], gr_[2]}); }
  int meters(float* out, int max) const override {
    int k = std::min(max, 4);
    for (int i = 0; i < k && i < 3; ++i) out[i] = gr_[i];
    if (k > 3) out[3] = level_;
    return k;
  }
  // Linear-phase band filters: every band is delayed by the FIR's centre tap.
  int latencyFor(const PluginState& s, double) const override { return filterType(s) == 1 ? MbFir::H : 0; }
  MultibandK2() {
    for (int c = 0; c < 2; ++c) {
      limEnv_[c].adaptive = true;
      limEnv_[c].set(44100, 0.871, 0.75, 3.0e-6, 0.01495, std::sqrt((0.81095 - 0.005) / 0.995));
    }
  }

 private:
  // iir_[c][k]: k 0-1 low band LP, 2-3 mid HP, 4-5 mid LP, 6-7 high HP (two biquads each);
  // lr_[c][k]: 0-1 LP low, 2-3 HP low, 4-5 LP high, 6-7 HP high, 8-11 low-band all-pass
  void designFilters() {
    const double q[2] = {0.5412, 1.3066};
    for (int c = 0; c < 2; ++c)
      for (int k = 0; k < 2; ++k) {
        iir_[c][0 + k].set(Biquad::LowPass, sr_, f_[0], q[k], 0);
        iir_[c][2 + k].set(Biquad::HighPass, sr_, f_[2], q[k], 0);
        iir_[c][4 + k].set(Biquad::LowPass, sr_, f_[3], q[k], 0);
        iir_[c][6 + k].set(Biquad::HighPass, sr_, f_[1], q[k], 0);
        // Linkwitz-Riley 4th order (two Q 0.7071 sections), serial split at the two
        // crossovers with the high split's all-pass on the low band: flat sum.
        lr_[c][0 + k].set(Biquad::LowPass, sr_, f_[0], 0.7071, 0);
        lr_[c][2 + k].set(Biquad::HighPass, sr_, f_[0], 0.7071, 0);
        lr_[c][4 + k].set(Biquad::LowPass, sr_, f_[1], 0.7071, 0);
        lr_[c][6 + k].set(Biquad::HighPass, sr_, f_[1], 0.7071, 0);
        lr_[c][8 + k].set(Biquad::LowPass, sr_, f_[1], 0.7071, 0);
        lr_[c][10 + k].set(Biquad::HighPass, sr_, f_[1], 0.7071, 0);
      }
    fir_[0].design(0, 0, f_[0], sr_);
    fir_[1].design(2, f_[2], f_[3], sr_);
    fir_[2].design(1, f_[1], 0, sr_);
    for (int b = 0; b < 3; ++b) std::copy(std::begin(fir_[b].h), std::end(fir_[b].h), firBank_.h[b]);
    for (int c = 0; c < 2; ++c) limEnv_[c].set(sr_, 0.871, 0.75, 3.0e-6, 0.01495, std::sqrt((0.81095 - 0.005) / 0.995));
  }
  void splitSample(const double* x, double band[3][2]) {
    for (int c = 0; c < 2; ++c) {
      if (ftype_ == 1) {
        double o[3];
        firBank_.run(c, x[c], o);
        for (int b = 0; b < 3; ++b) band[b][c] = o[b];
      } else if (ftype_ == 0) {
        Biquad* f = iir_[c];
        band[0][c] = f[1].process(f[0].process(x[c]));
        band[1][c] = f[5].process(f[4].process(f[3].process(f[2].process(x[c]))));
        band[2][c] = f[7].process(f[6].process(x[c]));
      } else {
        Biquad* f = lr_[c];
        double lo = f[1].process(f[0].process(x[c])), rest = f[3].process(f[2].process(x[c]));
        band[0][c] = f[9].process(f[8].process(lo)) + f[11].process(f[10].process(lo));
        band[1][c] = f[5].process(f[4].process(rest));
        band[2][c] = f[7].process(f[6].process(rest));
      }
    }
    if (ftype_ == 1) firBank_.advance();
  }

  static int filterType(const PluginState& s) { return std::clamp(iprm(s, pp::MbFilterType), 0, 2); }
  MbEnv env_[3], limEnv_[2];
  Biquad iir_[2][8], lr_[2][12];
  MbFir fir_[3];  // designs (their histories are unused: firBank_ runs them)
  MbFirBank firBank_;
  double f_[4] = {-1, -1, -1, -1}, sr_ = 0;
  int ftype_ = -1;
  float gr_[3] = {0, 0, 0}, level_ = -120;
};

// ======================================================================= Transient shaper
// Reference transient processor: first-order split (low passes dry, the high
// part is processed); detector = mean of |L| and |R| of the high part.
// Attack: ratio of an instant peak follower to a slow-rising one (5/10/20 ms
// by shape), both falling over 200 ms; release: ratio of a 4 s peak memory to
// a follower falling over 50/100/200 ms (smoothed adaptively); the two ratios
// (softly limited when boosting) scaled by the attack / release amounts give
// the gain (1 + s, or 1 / (1 - s) when cutting). Drive pushes the result into
// a rational soft clipper, scaled back afterwards. Output = low * low weight +
// processed high * high weight (split balance), times the gain.
class TransientK2 final : public Effect {
 public:
  static double coef10(double sec, double sr) { return std::pow(0.1, 1.0 / std::max(1e-9, sec * sr)); }
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    using namespace pp;
    const double sr = ctx.sampleRate;
    const bool on = prm(s, TrEnabled, 1) >= 0.5f;
    const int as = std::clamp(iprm(s, TrAttackSpeed, 1), 0, 2), rs = std::clamp(iprm(s, TrReleaseSpeed, 1), 0, 2);
    static const double shapeMul[3] = {0.5, 1.0, 2.0};
    const double sA = shapeMul[as], sR = shapeMul[rs];
    const double c200 = coef10(0.2, sr), aA = 1.0 - coef10(0.01 * sA, sr), aR = 1.0 - coef10(0.01 * sR, sr);
    const double dR = coef10(0.1 * sR, sr), c4s = coef10(4.0, sr);
    double av = std::clamp(static_cast<double>(prm(s, TrAttack, 0.4f)), -1.0, 1.0), rv = std::clamp(static_cast<double>(prm(s, TrSustain)), -1.0, 1.0);
    av = av * std::fabs(av), rv = rv * std::fabs(rv);
    const double amtA = av >= 0 ? av * 0.4 : av * 0.6, amtR = rv > 0 ? rv * 0.6 : rv;
    const double dx = std::clamp(static_cast<double>(prm(s, TrDrive)), 0.0, 1.0);
    const double drive = dx < 0.1 ? dx * 9.0 + 0.1 : std::pow(15.0, (dx - 0.1) * 1.1111111);
    const double gain = dbToGain(prm(s, TrOutput));
    const double bal = std::clamp(static_cast<double>(prm(s, TrBalance)), -1.0, 1.0);
    const double wLow = std::min(1.0, 1.0 - bal), wHigh = std::min(1.0, 1.0 + bal);
    const double mix = std::clamp(static_cast<double>(prm(s, TrMix, 1)), 0.0, 1.0);
    const double split = std::clamp(static_cast<double>(prm(s, TrSplit, 253.75f)), 1.0, sr * 0.45);
    if (split != split_ || sr != sr_) {
      split_ = split, sr_ = sr;
      double t = std::tan(kPi * split / sr);
      lpA_ = t / (1.0 + t), lpB_ = (1.0 - t) / (1.0 + t);  // y = a (x + x1) + b y1
      hpA_ = 1.0 / (1.0 + t);                             // y = a (x - x1) + b y1
    }
    double peakGain = 0, outPk = 0, inPk = 0;
    for (int i = 0; i < n; ++i) {
      double x[2] = {L[i], R[i]}, lo[2], hi[2];
      for (int c = 0; c < 2; ++c) {
        lo[c] = lpA_ * (x[c] + x1_[c]) + lpB_ * ly_[c];
        hi[c] = hpA_ * (x[c] - x1_[c]) + lpB_ * hy_[c];
        x1_[c] = x[c], ly_[c] = lo[c], hy_[c] = hi[c];
      }
      inPk = std::max(inPk, std::max(std::fabs(x[0]), std::fabs(x[1])));
      double lvl = (std::fabs(hi[0]) + std::fabs(hi[1])) * 0.5;
      e1_ = lvl > e1_ ? lvl : e1_ * c200;
      e2_ = lvl > e2_ ? e2_ + (lvl - e2_) * aA : e2_ * c200;
      double gA = 1.0;
      if (e2_ > 0) {
        double r = e1_ / e2_;
        if (r >= 1.0) gA = amtA < 0 ? r : (r > 10.0 ? 6.6 : r * 0.96 - 0.003 * r * r * r);
      }
      e3_ = lvl >= e3_ ? lvl : e3_ * c4s;
      e4_ = lvl >= e4_ ? lvl : e4_ * dR;
      double k = e4_ > 0 ? 3.0 * (e4_ + 1e-4 - e5_) / (e4_ + 1e-4) : 3.0;
      if (amtR < 0) k = 3.0;
      k = std::clamp(k, 0.0, 1.0);
      e5_ += (e4_ - e5_) * (aR + (1.0 - aR) * k);
      double gR = 1.0;
      if (e5_ > 1e-6) {
        double r2 = e3_ / e5_;
        if (r2 >= 1.0) gR = amtR < 0 ? r2 : (r2 > 20.0 ? 12.0 : r2 * 0.84 - 0.0006 * r2 * r2 * r2);
      }
      double sum = amtA * (gA - 1.0) + amtR * (gR - 1.0);
      double g = sum >= 0 ? 1.0 + sum : 1.0 / (1.0 - sum);
      peakGain = std::max(peakGain, std::fabs(gainToDb(g)));
      for (int c = 0; c < 2; ++c) {
        double y = hi[c] * g * drive;
        y = y < -3.0 ? -1.0 : y > 3.0 ? 1.0 : y * (y * y + 27.0) / (9.0 * y * y + 27.0);
        y /= drive;
        double wet = (lo[c] * wLow + y * wHigh) * gain;
        double o = on ? x[c] + (wet - x[c]) * mix : x[c];
        (c ? R : L)[i] = static_cast<float>(o);
        outPk = std::max(outPk, std::fabs(o));
      }
      if (std::fabs(e1_) < 1e-30) e1_ = 0;
      if (std::fabs(e2_) < 1e-30) e2_ = 0;
    }
    meter_ = static_cast<float>(peakGain);
    in_ = static_cast<float>(gainToDb(inPk));
    out_ = static_cast<float>(gainToDb(outPk));
  }
  void reset() override {
    e1_ = e2_ = e3_ = e4_ = e5_ = 0;
    for (int c = 0; c < 2; ++c) x1_[c] = ly_[c] = hy_[c] = 0;
  }
  float meter() const override { return meter_; }
  float inputLevel() const override { return in_; }
  int meters(float* out, int max) const override {
    if (max < 1) return 0;
    out[0] = out_;
    return 1;
  }

 private:
  double e1_ = 0, e2_ = 0, e3_ = 0, e4_ = 0, e5_ = 0;
  double x1_[2] = {0, 0}, ly_[2] = {0, 0}, hy_[2] = {0, 0};
  double split_ = -1, sr_ = 0, lpA_ = 0, lpB_ = 0, hpA_ = 0;
  float meter_ = 0, in_ = -120, out_ = -120;
};

}  // namespace

std::unique_ptr<Effect> createDynK2Effect(const std::string& type) {
  if (type == "gl.transient") {
    auto fx = std::make_unique<TransientK2>();
    fx->reset();
    return fx;
  }
  if (type == "gl.multiband") {
    auto fx = std::make_unique<MultibandK2>();
    fx->reset();
    return fx;
  }
  if (type == "gl.comp") {
    auto fx = std::make_unique<CompK2>();
    fx->reset();
    return fx;
  }
  if (type == "gl.limiter") {
    auto fx = std::make_unique<LimiterK2>();
    fx->reset();
    return fx;
  }
  return nullptr;
}

}  // namespace gl
