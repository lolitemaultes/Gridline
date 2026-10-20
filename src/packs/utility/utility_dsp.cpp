// Gridline — "utility" pack: runtimes (filters, EQ, meters, visual taps).
#include "../../engine/FlFilterRef.h"
#include "../Packs.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include "../../engine/Dsp.h"
#include "utility_rt.h"

namespace gl::packs::utility {

namespace {

constexpr double kPi = 3.14159265358979323846;

float prm(const PluginState& s, int k, float def = 0.f) {
  return k >= 0 && static_cast<size_t>(k) < s.params.size() ? s.params[static_cast<size_t>(k)] : def;
}

// Transposed direct form II biquad with float state per channel.
struct Tdf2 {
  double z1 = 0, z2 = 0;
  double run(const Coefs& c, double x) {
    double y = c.b0 * x + z1;
    z1 = c.b1 * x - c.a1 * y + z2;
    z2 = c.b2 * x - c.a2 * y;
    return y;
  }
  void flush() {
    if (std::fabs(z1) < 1e-25) z1 = 0;
    if (std::fabs(z2) < 1e-25) z2 = 0;
  }
  void clear() { z1 = z2 = 0; }
};

// Per-block coefficient for a one-pole glide of `seconds` over `frames`.
double glide(double sr, double seconds, int frames) {
  if (seconds <= 1e-4) return 0.0;
  return std::exp(-static_cast<double>(frames) / (seconds * sr));
}

// Linear gain ramp helper (no zipper noise on gain changes).
struct Ramp {
  double cur = 1;
  bool primed = false;
  // Returns per-sample increment towards target over n frames.
  double begin(double target, int n) {
    if (!primed) {
      cur = target;
      primed = true;
    }
    return n > 0 ? (target - cur) / n : 0.0;
  }
};

// Sub-blocks of a fixed length on the effect's own sample clock (J-30):
// smoothing steps, coefficient updates and denormal flushes happen at the same
// samples whatever the host block size (per-call sub-blocks made the output
// depend on it).
struct SubClock {
  int left = 0;
  // Frames of the current sub-block that fit in `avail`; `start`: a new sub-block begins.
  int take(int avail, int len, bool& start) {
    start = left == 0;
    if (start) left = len;
    const int m = std::min(left, avail);
    left -= m;
    return m;
  }
  bool ended() const { return left == 0; }
};

// ================================================================ Curve EQ
class CurveEqFx final : public UtilityFx {
 public:
  CurveEqFx() {
    fftK_.init(kCeLinearKernel);
    fftN_.init(kN);
    reset();
  }
  void reset() override {
    for (auto& ch : bq_)
      for (auto& b : ch) b.clear();
    std::memset(inL_, 0, sizeof inL_);
    std::memset(inR_, 0, sizeof inR_);
    std::memset(accL_, 0, sizeof accL_);
    std::memset(accR_, 0, sizeof accR_);
    pos_ = 0;
    primed_ = false;
    designedLinear_ = designedMin_ = false;
    mode_ = -1;
    sub_ = {};
  }

  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    const double sr = ctx.sampleRate;
    if (sr != bank_.sr) {
      bank_.prepare(sr);
      designedLinear_ = designedMin_ = false;
    }
    int mode = linearPhase(s) ? 1 : 0;
    if (mode != mode_) {
      for (auto& ch : bq_)
        for (auto& b : ch) b.clear();
      std::memset(accL_, 0, sizeof accL_);
      std::memset(accR_, 0, sizeof accR_);
      pos_ = 0;
      mode_ = mode;
      designedLinear_ = designedMin_ = false;
    }
    const double tx = prm(s, CeMorphX), ty = prm(s, CeMorphY), td = prm(s, CeDepth, 1), tsh = prm(s, CeShift);
    if (!primed_) {
      x_ = tx, y_ = ty, depth_ = td, shift_ = tsh;
      primed_ = true;
    }
    const double smooth = prm(s, CeSmooth);
    const double outTarget = dsp::dbToGain(prm(s, CeOutput));
    for (int off = 0; off < n;) {
      bool start = false;
      const int m = sub_.take(n - off, kSub, start);
      if (start) {
        double a = glide(sr, smooth, kSub);
        x_ = tx + (x_ - tx) * a;
        y_ = ty + (y_ - ty) * a;
        depth_ = td + (depth_ - td) * a;
        shift_ = tsh + (shift_ - tsh) * a;
        float morphed[kCeBands];
        ceMorph(s, x_, y_, morphed);
        bool changed = false;
        for (int b = 0; b < kCeBands; ++b) changed = changed || std::fabs(morphed[b] - last_[b]) > 1e-4f;
        changed = changed || std::fabs(depth_ - lastDepth_) > 1e-5 || std::fabs(shift_ - lastShift_) > 1e-5;
        if (changed) {
          std::memcpy(last_, morphed, sizeof last_);
          lastDepth_ = depth_;
          lastShift_ = shift_;
          designedLinear_ = designedMin_ = false;
        }
        outInc_ = out_.begin(outTarget, kSub);
      }
      float* l = L + off;
      float* r = R + off;
      if (mode == 0) {
        if (!designedMin_) designMinimum(sr);
        for (int i = 0; i < m; ++i) {
          double xl = l[i], xr = r[i];
          for (int b = 0; b < kCeBands; ++b) {
            if (!active_[b]) continue;
            xl = bq_[0][b].run(coef_[b], xl);
            xr = bq_[1][b].run(coef_[b], xr);
          }
          l[i] = static_cast<float>(xl);
          r[i] = static_cast<float>(xr);
        }
        if (sub_.ended())
          for (auto& ch : bq_)
            for (auto& b : ch) b.flush();
      } else {
        for (int i = 0; i < m; ++i) {
          inL_[pos_] = l[i];
          inR_[pos_] = r[i];
          l[i] = accL_[pos_];
          r[i] = accR_[pos_];
          if (++pos_ == kCeLinearHop) {
            if (!designedLinear_) designLinear(sr);
            convolveBlock();
            pos_ = 0;
          }
        }
      }
      for (int i = 0; i < m; ++i) {
        out_.cur += outInc_;
        l[i] = static_cast<float>(l[i] * out_.cur);
        r[i] = static_cast<float>(r[i] * out_.cur);
      }
      off += m;
    }
  }

  // Linear phase: one hop of block buffering + half the FIR kernel.
  int latencyFor(const PluginState& s, double) const override { return linearPhase(s) ? kCeLinearLatency : 0; }
  static bool linearPhase(const PluginState& s) { return prm(s, CePhase) >= 0.5f; }

  // Realised response (dB, without output gain) — exposed for tests.
  double responseDb(double hz, double sr) const {
    if (mode_ == 1) return ceTargetDb(last_, lastDepth_, lastShift_, hz);
    double m = 1;
    for (int b = 0; b < kCeBands; ++b)
      if (active_[b]) m *= coef_[b].magnitude(hz, sr);
    return 20 * std::log10(std::max(1e-9, m));
  }

 private:
  static constexpr int kSub = 64;
  static constexpr int kN = 4096;  // >= kernel + hop - 1

  void designMinimum(double sr) {
    double target[kCeBands], gains[kCeBands];
    for (int b = 0; b < kCeBands; ++b) target[b] = ceTargetDb(last_, lastDepth_, lastShift_, ceBandHz(b));
    bank_.solve(target, gains);
    for (int b = 0; b < kCeBands; ++b) {
      active_[b] = std::fabs(gains[b]) > 1e-3;
      coef_[b] = ceBandCoefs(sr, b, gains[b]);
    }
    designedMin_ = true;
  }

  void designLinear(double sr) {
    const int K = kCeLinearKernel;
    for (int k = 0; k <= K / 2; ++k) {
      double hz = static_cast<double>(k) * sr / K;
      double mag = dsp::dbToGain(ceTargetDb(last_, lastDepth_, lastShift_, std::max(hz, 1.0)));
      wr_[k] = static_cast<float>(mag);
      wi_[k] = 0;
      if (k > 0 && k < K / 2) {
        wr_[K - k] = static_cast<float>(mag);
        wi_[K - k] = 0;
      }
    }
    fftK_.transform(wr_, wi_, true);
    // Centre the zero-phase response, window it, zero-pad to the convolution size.
    for (int i = 0; i < kN; ++i) {
      hr_[i] = 0;
      hi_[i] = 0;
    }
    for (int i = 0; i < K; ++i) {
      double w = 0.42 - 0.5 * std::cos(2 * kPi * i / K) + 0.08 * std::cos(4 * kPi * i / K);
      hr_[i] = static_cast<float>(wr_[(i + K / 2) % K] * w);
    }
    fftN_.transform(hr_, hi_, false);
    designedLinear_ = true;
  }

  // One hop: FFT-convolve the collected input (L in re, R in im) and overlap-add.
  void convolveBlock() {
    for (int i = 0; i < kN; ++i) {
      wr_[i] = i < kCeLinearHop ? inL_[i] : 0.f;
      wi_[i] = i < kCeLinearHop ? inR_[i] : 0.f;
    }
    fftN_.transform(wr_, wi_, false);
    for (int i = 0; i < kN; ++i) {
      float a = wr_[i], b = wi_[i];
      wr_[i] = a * hr_[i] - b * hi_[i];
      wi_[i] = a * hi_[i] + b * hr_[i];
    }
    fftN_.transform(wr_, wi_, true);
    std::memmove(accL_, accL_ + kCeLinearHop, sizeof(float) * (kN - kCeLinearHop));
    std::memmove(accR_, accR_ + kCeLinearHop, sizeof(float) * (kN - kCeLinearHop));
    for (int i = kN - kCeLinearHop; i < kN; ++i) accL_[i] = accR_[i] = 0;
    for (int i = 0; i < kN; ++i) {
      accL_[i] += wr_[i];
      accR_[i] += wi_[i];
    }
  }

  CeBank bank_;
  Tdf2 bq_[2][kCeBands];
  Coefs coef_[kCeBands];
  bool active_[kCeBands] = {};
  float last_[kCeBands] = {};
  double lastDepth_ = 1, lastShift_ = 0;
  double x_ = 0, y_ = 0, depth_ = 1, shift_ = 0;
  bool primed_ = false, designedLinear_ = false, designedMin_ = false;
  int mode_ = -1;
  Ramp out_;
  double outInc_ = 0;
  SubClock sub_;
  Fft fftK_, fftN_;
  float wr_[kN] = {}, wi_[kN] = {}, hr_[kN] = {}, hi_[kN] = {};
  float inL_[kCeLinearHop] = {}, inR_[kCeLinearHop] = {};
  float accL_[kN] = {}, accR_[kN] = {};
  int pos_ = 0;
};

// ============================================================= Free Filter
class FreeFilterFx final : public UtilityFx {
 public:
  void reset() override {
    for (auto& f : f_) f.clear();
    dcX_[0] = dcX_[1] = dcY_[0] = dcY_[1] = 0;
    primed_ = false;
    sub_ = {};
    ref_.clear(), dcr_[0].clear(), dcr_[1].clear();
  }
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    const double sr = ctx.sampleRate;
    if (prm(s, FfModel, 1) >= 0.5f) {  // J-26 reference: fixed design, no glide
      int type = std::clamp(static_cast<int>(prm(s, FfType) + 0.5f), 0, 6);
      double hz = prm(s, FfFreq, 1000), qk = prm(s, FfQK, 0.165f), g = prm(s, FfGain);
      if (type != rType_ || hz != rHz_ || qk != rQ_ || g != rG_) {
        ref_.design(type, hz, qk, g);
        rType_ = type, rHz_ = hz, rQ_ = qk, rG_ = g;
      }
      if (sr != rSr_) rSr_ = sr, dcr_[0].set(sr), dcr_[1].set(sr);
      const bool dc = prm(s, FfDc, 1) >= 0.5f;
      for (int i = 0; i < n; ++i) {
        float l = ref_.tick(0, L[i]), r = ref_.tick(1, R[i]);
        L[i] = dc ? dcr_[0].tick(l) : l;
        R[i] = dc ? dcr_[1].tick(r) : r;
      }
      return;
    }
    int type = std::clamp(static_cast<int>(prm(s, FfType) + 0.5f), 0, FtCount - 1);
    double tf = std::log(std::clamp<double>(prm(s, FfFreq, 1000), 10.0, sr * 0.49)), tq = prm(s, FfQ, 0.707), tg = prm(s, FfGain);
    if (!primed_ || type != type_) {
      lf_ = tf, q_ = tq, g_ = tg;
      primed_ = true;
      type_ = type;
    }
    const bool dc = prm(s, FfDc, 1) >= 0.5f;
    const double R1 = 1.0 - 2 * kPi * 8.0 / sr;
    for (int off = 0; off < n;) {
      bool start = false;
      const int m = sub_.take(n - off, kSub, start);
      if (start) {
        double a = glide(sr, 0.012, kSub);
        lf_ = tf + (lf_ - tf) * a;
        q_ = tq + (q_ - tq) * a;
        g_ = tg + (g_ - tg) * a;
        c_ = freeFilterCoefs(type, sr, std::exp(lf_), q_, g_);
      }
      const Coefs& c = c_;
      for (int i = off; i < off + m; ++i) {
        double l = f_[0].run(c, L[i]), r = f_[1].run(c, R[i]);
        if (dc) {
          double yl = l - dcX_[0] + R1 * dcY_[0];
          dcX_[0] = l;
          dcY_[0] = yl;
          double yr = r - dcX_[1] + R1 * dcY_[1];
          dcX_[1] = r;
          dcY_[1] = yr;
          l = yl;
          r = yr;
        }
        L[i] = static_cast<float>(l);
        R[i] = static_cast<float>(r);
      }
      off += m;
      if (sub_.ended()) {
        for (auto& f : f_) f.flush();
        for (int k = 0; k < 2; ++k)
          if (std::fabs(dcY_[k]) < 1e-25) dcY_[k] = 0;
      }
    }
  }

 private:
  static constexpr int kSub = 32;
  flref::FixedBiquad ref_;
  flref::DcRemover dcr_[2];
  double rHz_ = -1, rQ_ = -1, rG_ = -1e9, rSr_ = 0;
  int rType_ = -1;
  Tdf2 f_[2];
  Coefs c_;
  SubClock sub_;
  double dcX_[2] = {}, dcY_[2] = {};
  double lf_ = 0, q_ = 0.7, g_ = 0;
  int type_ = -1;
  bool primed_ = false;
};

// ============================================================ Quick Lowpass
// Two-pole state-variable lowpass; the cheapest useful resonant filter.
class QuickLpFx final : public UtilityFx {
 public:
  void reset() override {
    svf_[0].reset();
    svf_[1].reset();
    primed_ = false;
    sub_ = {};
    ref_.clear();
  }
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    if (prm(s, QlModel, 1) >= 0.5f) {  // J-26 reference
      ref_.process(prm(s, QlCutK, 1), prm(s, QlResK), L, R, n);
      return;
    }
    const double sr = ctx.sampleRate;
    double tc = std::log(std::clamp<double>(prm(s, QlCutoff, 20000), 20.0, 22000.0)), tr = prm(s, QlReso);
    if (!primed_) {
      lc_ = tc, reso_ = tr;
      primed_ = true;
    }
    for (int off = 0; off < n;) {
      bool start = false;
      const int m = sub_.take(n - off, kSub, start);
      if (start) {
        double a = glide(sr, 0.01, kSub);
        lc_ = tc + (lc_ - tc) * a;
        reso_ = tr + (reso_ - tr) * a;
        double hz = std::exp(lc_);
        // Fully open: pass straight through (no high-end loss at the default).
        open_ = hz >= std::min(19900.0, sr * 0.45) && reso_ < 1e-3;
        for (auto& f : svf_) f.set(sr, hz, reso_ * 0.97);
      }
      for (int i = off; i < off + m; ++i) {
        double l = svf_[0].process(L[i]), r = svf_[1].process(R[i]);
        if (!open_) {
          L[i] = static_cast<float>(l);
          R[i] = static_cast<float>(r);
        }
      }
      off += m;
    }
  }

 private:
  static constexpr int kSub = 32;
  SubClock sub_;
  bool open_ = false;
  flref::QuickLp ref_;
  dsp::Svf svf_[2];
  double lc_ = 0, reso_ = 0;
  bool primed_ = false;
};

// =============================================================== Bass Boost
// Low shelf at `freq` lifting by bassBoostDb(amount) (see utility.h).
class BassBoostFx final : public UtilityFx {
 public:
  void reset() override {
    for (auto& f : f_) f.clear();
    primed_ = false;
    sub_ = {};
    ref_.clear();
  }
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    const double sr = ctx.sampleRate;
    if (prm(s, BbModel, 1) >= 0.5f) {  // J-26 reference: integer Hz, (11^n - 1)/2
      ref_.process(std::nearbyint(prm(s, BbFreq, 60)), flref::bassBoostGain(prm(s, BbAmount, 0.75f)), sr, L, R, n);
      return;
    }
    double tf = std::log(std::clamp<double>(prm(s, BbFreq, 80), 20.0, 500.0)), ta = prm(s, BbAmount, 0.75);
    if (!primed_) {
      lf_ = tf, amt_ = ta;
      primed_ = true;
    }
    for (int off = 0; off < n;) {
      bool start = false;
      const int m = sub_.take(n - off, kSub, start);
      if (start) {
        double a = glide(sr, 0.015, kSub);
        lf_ = tf + (lf_ - tf) * a;
        amt_ = ta + (amt_ - ta) * a;
        c_ = bassBoostCoefs(sr, std::exp(lf_), amt_);
      }
      for (int i = off; i < off + m; ++i) {
        L[i] = static_cast<float>(f_[0].run(c_, L[i]));
        R[i] = static_cast<float>(f_[1].run(c_, R[i]));
      }
      off += m;
      if (sub_.ended())
        for (auto& f : f_) f.flush();
    }
  }

 private:
  static constexpr int kSub = 32;
  flref::BassBooster ref_;
  Tdf2 f_[2];
  Coefs c_;
  SubClock sub_;
  double lf_ = 0, amt_ = 0;
  bool primed_ = false;
};

// ================================================================= metering
// Shared by Scope and Level Meter: peaks, true peak, RMS, VU, correlation, loudness.
struct Metering {
  LoudnessMeter lufs;
  TruePeak tp[2];
  double ms[2] = {}, vu[2] = {}, cLR = 0, cLL = 0, cRR = 0;
  float maxPeak = 0, maxTp = 0;
  void reset() {
    lufs.reset();
    tp[0].reset();
    tp[1].reset();
    ms[0] = ms[1] = vu[0] = vu[1] = cLR = cLL = cRR = 0;
    maxPeak = maxTp = 0;
  }
  void process(LevelState& st, const float* L, const float* R, int n, double sr, bool truePeak) {
    lufs.setSampleRate(sr);
    if (st.resetRequest.exchange(false, std::memory_order_acq_rel)) {
      lufs.reset();
      maxPeak = maxTp = 0;
    }
    lufs.process(L, R, n);
    const double aRms = std::exp(-1.0 / (0.3 * sr / 3.0));  // ~300 ms window (time constant 100 ms)
    const double aVu = std::exp(-1.0 / (0.065 * sr));
    const double aC = std::exp(-1.0 / (0.1 * sr));
    float pk[2] = {0, 0};
    const float* in[2] = {L, R};
    for (int c = 0; c < 2; ++c) {
      double m = ms[c], v = vu[c];
      for (int i = 0; i < n; ++i) {
        double x = in[c][i];
        pk[c] = std::max(pk[c], static_cast<float>(std::fabs(x)));
        m = x * x + (m - x * x) * aRms;
        v = std::fabs(x) + (v - std::fabs(x)) * aVu;
      }
      ms[c] = m < 1e-30 ? 0 : m;
      vu[c] = v < 1e-30 ? 0 : v;
    }
    for (int i = 0; i < n; ++i) {
      cLR = L[i] * R[i] + (cLR - L[i] * R[i]) * aC;
      cLL = L[i] * L[i] + (cLL - L[i] * L[i]) * aC;
      cRR = R[i] * R[i] + (cRR - R[i] * R[i]) * aC;
    }
    if (std::fabs(cLR) < 1e-30) cLR = 0;
    if (cLL < 1e-30) cLL = 0;
    if (cRR < 1e-30) cRR = 0;
    float tpk[2] = {pk[0], pk[1]};
    if (truePeak) {
      tpk[0] = std::max(pk[0], tp[0].process(L, n));
      tpk[1] = std::max(pk[1], tp[1].process(R, n));
    }
    auto maxStore = [](std::atomic<float>& a, float v) {
      if (v > a.load(std::memory_order_relaxed)) a.store(v, std::memory_order_relaxed);
    };
    maxStore(st.peakL, pk[0]);
    maxStore(st.peakR, pk[1]);
    maxStore(st.truePeakL, tpk[0]);
    maxStore(st.truePeakR, tpk[1]);
    maxPeak = std::max({maxPeak, pk[0], pk[1]});
    maxTp = std::max({maxTp, tpk[0], tpk[1]});
    st.maxPeak.store(maxPeak, std::memory_order_relaxed);
    st.maxTruePeak.store(maxTp, std::memory_order_relaxed);
    st.rmsL.store(static_cast<float>(std::sqrt(ms[0])), std::memory_order_relaxed);
    st.rmsR.store(static_cast<float>(std::sqrt(ms[1])), std::memory_order_relaxed);
    // VU: average rectified value scaled so a sine reads its RMS.
    st.vuL.store(static_cast<float>(vu[0] * 1.1107), std::memory_order_relaxed);
    st.vuR.store(static_cast<float>(vu[1] * 1.1107), std::memory_order_relaxed);
    double den = std::sqrt(cLL * cRR);
    st.correlation.store(den > 1e-12 ? static_cast<float>(std::clamp(cLR / den, -1.0, 1.0)) : 0.f, std::memory_order_relaxed);
    st.momentary.store(lufs.momentary(), std::memory_order_relaxed);
    st.shortTerm.store(lufs.shortTerm(), std::memory_order_relaxed);
    st.integrated.store(lufs.integrated(), std::memory_order_relaxed);
    st.range.store(lufs.range(), std::memory_order_relaxed);
    st.maxMomentary.store(lufs.maxMomentary(), std::memory_order_relaxed);
  }
};

// Pass-through analysers. Audio is never modified (bit-exact).
class ScopeFx final : public UtilityFx {
 public:
  explicit ScopeFx(bool meters, bool truePeak) : meters_(meters), truePeak_(truePeak) {}
  void reset() override { m_.reset(); }
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    ring_.write(L, R, n);
    bool tp = truePeak_ && (s.type != "gl.meter" || prm(s, LmTruePeak, 1) >= 0.5f);
    if (meters_) m_.process(st_, L, R, n, ctx.sampleRate, tp);
  }
  const StereoRing* ring() const override { return &ring_; }
  LevelState* levels() override { return meters_ ? &st_ : nullptr; }
  float meter() const override { return 0.0f; }

 private:
  bool meters_, truePeak_;
  StereoRing ring_;
  LevelState st_;
  Metering m_;
};

// ===================================================================== Clock
class ClockFx final : public UtilityFx {
 public:
  void reset() override {}
  void process(const PluginState&, const ProcessContext& ctx, float*, float*, int n) override {
    if (ctx.playing && !wasPlaying_) played_ = 0;
    if (ctx.playing) played_ += n;
    wasPlaying_ = ctx.playing;
    t_.tick.store(ctx.tick, std::memory_order_relaxed);
    t_.tempo.store(ctx.tempo, std::memory_order_relaxed);
    t_.sampleRate.store(ctx.sampleRate, std::memory_order_relaxed);
    t_.ppq.store(ctx.ppq, std::memory_order_relaxed);
    t_.playing.store(ctx.playing, std::memory_order_relaxed);
    t_.playedSamples.store(played_, std::memory_order_relaxed);
  }
  const TransportState* transport() const override { return &t_; }

 private:
  TransportState t_;
  bool wasPlaying_ = false;
  int64_t played_ = 0;
};

// ===================================================================== Notes
class NotesFx final : public UtilityFx {
 public:
  void reset() override {}
  void process(const PluginState&, const ProcessContext&, float*, float*, int) override {}
};

// ====================================================================== Mute
class MuteFx final : public UtilityFx {
 public:
  void reset() override { g_[0].primed = g_[1].primed = false; }
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    bool mute = prm(s, MuMute) >= 0.5f;
    int side = std::clamp(static_cast<int>(prm(s, MuSide) + 0.5f), 0, 2);  // both / left / right
    double fade = std::max(0.0005, static_cast<double>(prm(s, MuFade, 0.005)));
    double target[2] = {mute && side != 2 ? 0.0 : 1.0, mute && side != 1 ? 0.0 : 1.0};
    float* ch[2] = {L, R};
    double step = 1.0 / (fade * ctx.sampleRate);
    for (int c = 0; c < 2; ++c) {
      Ramp& g = g_[c];
      if (!g.primed) {
        g.cur = target[c];
        g.primed = true;
      }
      for (int i = 0; i < n; ++i) {
        if (g.cur < target[c]) g.cur = std::min(target[c], g.cur + step);
        else if (g.cur > target[c]) g.cur = std::max(target[c], g.cur - step);
        ch[c][i] = static_cast<float>(ch[c][i] * g.cur);
      }
    }
  }

 private:
  Ramp g_[2];
};

// ================================================================ Send Level
// The insert path keeps `dry` of the signal; send volume / pan / target are
// read by the mixer routing (see docs/plugins/utility.md).
class StereoMatrixFx final : public UtilityFx {
 public:
  void reset() override {
    std::memset(dl_, 0, sizeof dl_);
    std::memset(dr_, 0, sizeof dr_);
    w_ = 0;
    ap_[0] = ap_[1] = {};
    for (auto& g : g_) g.primed = false;
    sub_ = {};
  }
  void process(const PluginState& s, const ProcessContext& ctx, float* L, float* R, int n) override {
    const double sr = ctx.sampleRate;
    const bool classic = prm(s, SmClassic, 1) >= 0.5f;
    double target[4] = {prm(s, SmLL, 1), prm(s, SmRL), prm(s, SmLR), prm(s, SmRR, 1)};
    if (classic)
      for (double& x : target) x = (x < 0 ? -1.0 : 1.0) * (std::pow(3.0, std::fabs(x)) - 1.0) * 0.5;
    double delayMs = prm(s, SmDelay);
    double deph = prm(s, SmDephase);
    // First-order allpass on one side: corner sweeps 20 kHz -> 40 Hz with |amount|.
    double fc = 20000.0 * std::pow(0.002, std::fabs(deph));
    if (classic) {
      double dk = prm(s, SmDelayK), pk = prm(s, SmDephaseK);
      delayMs = (dk < 0 ? -1.0 : 1.0) * (std::pow(1001.0, std::fabs(dk)) - 1.0) * 0.5;
      deph = pk;
      fc = (std::pow(5001.0, 1.0 - std::fabs(pk)) - 1.0) * 4.398 + 10.0;
    }
    int d = std::min(kDelay - 1, static_cast<int>(std::lround(std::fabs(delayMs) * 0.001 * sr)));
    double t = std::tan(kPi * std::min(fc, sr * 0.45) / sr), apc = (t - 1) / (t + 1);
    const bool pre = prm(s, SmFxPos, 1) < 0.5f;  // delay / dephase before the matrix
    float* diffL = io_.multiL[0];  // J-45 difference output: dry input minus the result
    float* diffR = io_.multiR[0];
    for (int i = 0; i < n; ++i) {
      const float inL = L[i], inR = R[i];
      if (sub_.left == 0) {  // gain ramps over sub-blocks of the effect's own clock (J-30)
        for (int k = 0; k < 4; ++k) inc_[k] = g_[k].begin(target[k], kSub);
        sub_.left = kSub;
      }
      --sub_.left;
      for (int k = 0; k < 4; ++k) g_[k].cur += inc_[k];
      double l = L[i], r = R[i];
      double ol = pre ? l : g_[0].cur * l + g_[1].cur * r;
      double orr = pre ? r : g_[2].cur * l + g_[3].cur * r;
      dl_[w_] = static_cast<float>(ol);
      dr_[w_] = static_cast<float>(orr);
      if (d > 0) {
        int rd = (w_ - d + kDelay) % kDelay;
        if (delayMs > 0) orr = dr_[rd];  // positive: right later
        else ol = dl_[rd];
      }
      w_ = (w_ + 1) % kDelay;
      if (std::fabs(deph) > 1e-4) {
        Ap& a = ap_[deph > 0 ? 1 : 0];
        double& x = deph > 0 ? orr : ol;
        double y = apc * x + a.x1 - apc * a.y1;
        a.x1 = x;
        a.y1 = std::fabs(y) < 1e-25 ? 0 : y;
        x = y;
      }
      if (pre) {
        double ml = g_[0].cur * ol + g_[1].cur * orr, mr = g_[2].cur * ol + g_[3].cur * orr;
        ol = ml, orr = mr;
      }
      L[i] = static_cast<float>(ol);
      R[i] = static_cast<float>(orr);
      if (diffL && diffR) {
        diffL[i] += inL - L[i];
        diffR[i] += inR - R[i];
      }
    }
  }
  void setSideIo(const SideIo& io) override { io_ = io; }

 private:
  static constexpr int kDelay = 131072;  // > 500 ms at 192 kHz (reference delay range)
  struct Ap {
    double x1 = 0, y1 = 0;
  };
  float dl_[kDelay] = {}, dr_[kDelay] = {};
  int w_ = 0;
  SideIo io_;
  Ap ap_[2];
  Ramp g_[4];
  static constexpr int kSub = 32;
  double inc_[4] = {};
  SubClock sub_;
};

}  // namespace

std::unique_ptr<Generator> createGenerator(const std::string& type) {
  (void)type;
  return nullptr;
}

std::unique_ptr<Effect> createEffect(const std::string& type) {
  if (type == "gl.drawneq") return std::make_unique<CurveEqFx>();
  if (type == "gl.bankeq") return createBankEq();
  if (type == "gl.freefilter") return std::make_unique<FreeFilterFx>();
  if (type == "gl.fastlp") return std::make_unique<QuickLpFx>();
  if (type == "gl.bassboost") return std::make_unique<BassBoostFx>();
  if (type == "gl.scope") return std::make_unique<ScopeFx>(true, true);
  if (type == "gl.spectro") return std::make_unique<ScopeFx>(false, false);
  if (type == "gl.meter") return std::make_unique<ScopeFx>(true, true);
  if (type == "gl.clock") return std::make_unique<ClockFx>();
  if (type == "gl.notes") return std::make_unique<NotesFx>();
  if (type == "gl.mute") return std::make_unique<MuteFx>();
  if (type == "gl.stereomatrix") return std::make_unique<StereoMatrixFx>();
  return nullptr;
}

bool prepareAux(const PluginState& state, ChannelAux& aux) {
  (void)state;
  (void)aux;
  return false;
}

}  // namespace gl::packs::utility
static_assert(gl::packs::utility::SmClassic == 9, "migration in normalizePluginState uses index 9");
