// Gridline — "mini" pack: dynamics family (Press, Ceiling, Shear, Pack, Span,
// Latch, Punch).
#include <array>
#include "mini_curves.h"
#include "mini_rt.h"

namespace gl::packs::mini::rt {

namespace {

// ---- Press: feed-forward compressor with optional sidechain key ---------------
class Press final : public MiniFx {
  Follower det_;
  dsp::OnePole ms_;
  double gr_ = 0;
  void clear() override {
    det_.env = 0;
    ms_.z = 0;
    ms_.setTime(sr_, 0.01);
    gr_ = 0;
    meter_ = 0;
    input_ = -120;
  }
  void run(const PluginState& s, const ProcessContext&, float* L, float* R, int n) override {
    const double th = pv(s, press::Threshold), ratio = pv(s, press::Ratio), knee = pv(s, press::Knee);
    const double att = coefFor(sr_, pv(s, press::Attack)), relMs = pv(s, press::Release), rel = coefFor(sr_, relMs);
    det_.set(sr_, 0.0, relMs * 0.5);
    const double makeup = pv(s, press::Makeup);
    const bool rms = pi(s, press::Detect) == 1;
    const bool side = pb(s, press::Sidechain) && io_.scL && io_.scR;
    double peakIn = -120;
    for (int i = 0; i < n; ++i) {
      const double kl = side ? io_.scL[i] : L[i], kr = side ? io_.scR[i] : R[i];
      double lvl;
      if (rms) {
        lvl = std::sqrt(std::max(0.0, ms_.process(std::max(kl * kl, kr * kr))) * 2.0);  // sine RMS reads as its peak
      } else {
        lvl = det_.process(std::max(std::fabs(kl), std::fabs(kr)));
      }
      const double db = todb(lvl);
      peakIn = std::max(peakIn, db);
      const double target = curve::pressGain(db, th, ratio, knee);
      gr_ = flushd(target + (gr_ - target) * (target < gr_ ? att : rel));
      const double g = dbg(gr_ + makeup);
      L[i] = static_cast<float>(L[i] * g);
      R[i] = static_cast<float>(R[i] * g);
    }
    meter_ = static_cast<float>(gr_);
    input_ = static_cast<float>(peakIn);
  }
};

// ---- Ceiling: look-ahead brickwall limiter (2 ms) -----------------------------
class Ceiling final : public MiniFx {
  static constexpr int kMax = 1024;
  std::array<float, kMax> dl_{}, dr_{};
  std::array<double, kMax> req_{}, box_{};
  int la_ = 2, pos_ = 0;
  double env_ = 1, sum_ = 0;
  Smooth in_;

 public:
  static int lookahead(double sr) { return std::clamp(static_cast<int>(std::lround(0.002 * sr)), 2, kMax); }
  int latencyFor(const PluginState&, double sampleRate) const override { return lookahead(sampleRate) - 1; }

 private:
  void clear() override {
    la_ = lookahead(sr_);
    dl_.fill(0);
    dr_.fill(0);
    req_.fill(1.0);
    box_.fill(1.0);
    pos_ = 0;
    env_ = 1;
    sum_ = la_;
    in_.setRate(sr_);
    in_.init = false;
    meter_ = 0;
  }
  void run(const PluginState& s, const ProcessContext&, float* L, float* R, int n) override {
    const double c = dbg(pv(s, ceiling::Ceiling)), rel = coefFor(sr_, pv(s, ceiling::Release));
    const double inT = dbg(pv(s, ceiling::Input));
    double minG = 1, peak = 0;
    for (int i = 0; i < n; ++i) {
      const double gi = in_.next(inT);
      const double l = L[i] * gi, r = R[i] * gi;
      const double m = std::max(std::fabs(l), std::fabs(r));
      peak = std::max(peak, m);
      const int p = pos_;
      req_[p] = m > c ? c / m : 1.0;
      double held = 1.0;
      for (int k = 0; k < la_; ++k) held = std::min(held, req_[(p - k + kMax) % kMax]);
      env_ = held <= env_ ? held : held + (env_ - held) * rel;
      sum_ += env_ - box_[(p - la_ + kMax) % kMax];
      box_[p] = env_;
      const double g = std::min(1.0, sum_ / la_);
      dl_[p] = static_cast<float>(l);
      dr_[p] = static_cast<float>(r);
      const int d = (p - (la_ - 1) + kMax) % kMax;
      L[i] = static_cast<float>(std::clamp(dl_[d] * g, -c, c));
      R[i] = static_cast<float>(std::clamp(dr_[d] * g, -c, c));
      pos_ = (p + 1) % kMax;
      minG = std::min(minG, g);
    }
    // The running sum drifts by rounding only; re-anchor it once per block.
    double s2 = 0;
    for (int k = 0; k < la_; ++k) s2 += box_[(pos_ - 1 - k + 2 * kMax) % kMax];
    sum_ = s2;
    meter_ = static_cast<float>(todb(minG));
    input_ = static_cast<float>(todb(peak));
  }
};

// ---- Shear: oversampled clipper with an adjustable knee ------------------------
class Shear final : public MiniFx {
  dsp::Oversampler os_[2];
  Smooth dr_, out_;

 public:
  static constexpr int kStages = 2;
  int latencyFor(const PluginState&, double) const override { return dsp::Oversampler::latency(kStages); }

 private:
  void clear() override {
    for (auto& o : os_) o.reset();
    dr_.setRate(sr_);
    out_.setRate(sr_);
    dr_.init = out_.init = false;
    meter_ = 0;
  }
  void run(const PluginState& s, const ProcessContext&, float* L, float* R, int n) override {
    const double dT = dbg(pv(s, shear::Drive)), c = dbg(pv(s, shear::Ceiling)), soft = pv(s, shear::Soft);
    const double oT = dbg(pv(s, shear::Output));
    auto f = [c, soft](double v) { return curve::shear(v, c, soft); };
    double over = 0;
    for (int i = 0; i < n; ++i) {
      const double d = dr_.next(dT), o = out_.next(oT);
      over = std::max(over, std::max(std::fabs(L[i]), std::fabs(R[i])) * d / c);
      L[i] = static_cast<float>(os_[0].run(L[i] * d, kStages, f) * o);
      R[i] = static_cast<float>(os_[1].run(R[i] * d, kStages, f) * o);
    }
    meter_ = over > 1.0 ? static_cast<float>(-todb(over)) : 0.f;
  }
};

// ---- Pack: look-ahead leveller that pulls the level towards a target ----------
class Pack final : public MiniFx {
  Ring dl_, dr_;
  Follower fl_, fr_;
  dsp::OnePole msl_, msr_;
  double gdb_ = 0;

 public:
  Pack() {
    dl_.init(static_cast<size_t>(0.012 * 192000.0));
    dr_.init(static_cast<size_t>(0.012 * 192000.0));
  }
  static int lookahead(double sr) { return std::max(1, static_cast<int>(std::lround(0.01 * sr))); }
  int latencyFor(const PluginState&, double sampleRate) const override { return lookahead(sampleRate); }

 private:
  void clear() override {
    dl_.clear();
    dr_.clear();
    fl_.env = fr_.env = 0;
    msl_.z = msr_.z = 0;
    msl_.setTime(sr_, 0.02);
    msr_.setTime(sr_, 0.02);
    gdb_ = 0;
    meter_ = 0;
  }
  void run(const PluginState& s, const ProcessContext&, float* L, float* R, int n) override {
    const double th = pv(s, pack::Threshold), amount = pv(s, pack::Amount), link = pv(s, pack::Link);
    const double att = coefFor(sr_, std::max(0.05, static_cast<double>(pv(s, pack::Attack))));
    const double rel = coefFor(sr_, pv(s, pack::Release));
    fl_.set(sr_, 0.0, 20.0);
    fr_.set(sr_, 0.0, 20.0);
    const bool rms = pi(s, pack::Detect) == 1;
    const int la = lookahead(sr_);
    double peakIn = -120;
    for (int i = 0; i < n; ++i) {
      double el, er;
      if (rms) {
        el = std::sqrt(std::max(0.0, msl_.process(static_cast<double>(L[i]) * L[i]) * 2.0));
        er = std::sqrt(std::max(0.0, msr_.process(static_cast<double>(R[i]) * R[i]) * 2.0));
      } else {
        el = fl_.process(std::fabs(L[i]));
        er = fr_.process(std::fabs(R[i]));
      }
      const double mx = std::max(el, er);
      const double e = link * mx + (1.0 - link) * 0.5 * (el + er);
      const double db = todb(e);
      peakIn = std::max(peakIn, db);
      // Full effect within 40 dB under the target; quieter material (noise, tails) is left alone.
      const double fade = std::clamp((db - (th - 60.0)) / 20.0, 0.0, 1.0);
      const double target = std::clamp((th - db) * amount, -40.0, 24.0) * fade;
      gdb_ = flushd(target + (gdb_ - target) * (target < gdb_ ? att : rel));
      const double g = dbg(gdb_);
      dl_.push(L[i]);
      dr_.push(R[i]);
      L[i] = static_cast<float>(dl_.at(static_cast<size_t>(la) + 1) * g);
      R[i] = static_cast<float>(dr_.at(static_cast<size_t>(la) + 1) * g);
    }
    meter_ = static_cast<float>(gdb_);
    input_ = static_cast<float>(peakIn);
  }
};

// ---- Span: upward / downward processing below and above two thresholds ---------
class Span final : public MiniFx {
  Follower det_;
  double gdb_ = 0;
  Smooth out_;
  void clear() override {
    det_.env = 0;
    gdb_ = 0;
    out_.setRate(sr_);
    out_.init = false;
    meter_ = 0;
  }

 public:
  // Static gain (dB) for a level: the curve the editor draws.
  static double gainFor(double db, double lt, double lr, double ht, double hr) {
    double g = 0;
    if (db < lt) g += std::clamp((lt - db) * lr, -60.0, 24.0);
    if (db > ht) g -= (db - ht) * (1.0 - 1.0 / std::max(1.0, hr));
    return g;
  }

 private:
  void run(const PluginState& s, const ProcessContext&, float* L, float* R, int n) override {
    const double lt = pv(s, span::LowThresh), lr = pv(s, span::LowRatio), ht = pv(s, span::HighThresh), hr = pv(s, span::HighRatio);
    const double sp = std::clamp(static_cast<double>(pv(s, span::Speed)), 0.0, 1.0);
    const double attMs = 30.0 + (1.0 - 30.0) * sp, relMs = 300.0 + (20.0 - 300.0) * sp;
    det_.set(sr_, attMs * 0.2, relMs);
    const double gAtt = coefFor(sr_, attMs), gRel = coefFor(sr_, relMs);
    const double outT = dbg(pv(s, span::Output));
    double peakIn = -120;
    for (int i = 0; i < n; ++i) {
      const double db = todb(det_.process(std::max(std::fabs(L[i]), std::fabs(R[i]))));
      peakIn = std::max(peakIn, db);
      const double target = db <= -119.0 ? 0.0 : gainFor(db, lt, lr, ht, hr);
      gdb_ = flushd(target + (gdb_ - target) * (target < gdb_ ? gAtt : gRel));
      const double g = dbg(gdb_) * out_.next(outT);
      L[i] = static_cast<float>(L[i] * g);
      R[i] = static_cast<float>(R[i] * g);
    }
    meter_ = static_cast<float>(gdb_);
    input_ = static_cast<float>(peakIn);
  }
};

// ---- Latch: gate / ducker with hysteresis and hold -----------------------------
class Latch final : public MiniFx {
  Follower det_;
  bool open_ = false;
  int hold_ = 0;
  double g_ = 0;
  bool init_ = false;
  void clear() override {
    det_.env = 0;
    open_ = false;
    hold_ = 0;
    init_ = false;
    meter_ = 0;
  }
  void run(const PluginState& s, const ProcessContext&, float* L, float* R, int n) override {
    const double th = pv(s, latch::Threshold), hy = pv(s, latch::Hysteresis);
    const double att = coefFor(sr_, pv(s, latch::Attack)), rel = coefFor(sr_, pv(s, latch::Release));
    const int holdN = static_cast<int>(pv(s, latch::Hold) * 0.001 * sr_);
    const double floorDb = pv(s, latch::Floor), fg = floorDb <= -79.9 ? 0.0 : dbg(floorDb);
    const bool flip = pb(s, latch::Flip);
    const bool side = pb(s, latch::Sidechain) && io_.scL && io_.scR;
    det_.set(sr_, 0.0, 10.0);
    if (!init_) {
      g_ = flip ? 1.0 : fg;
      init_ = true;
    }
    double peakIn = -120;
    for (int i = 0; i < n; ++i) {
      const double kl = side ? io_.scL[i] : L[i], kr = side ? io_.scR[i] : R[i];
      const double db = todb(det_.process(std::max(std::fabs(kl), std::fabs(kr))));
      peakIn = std::max(peakIn, db);
      if (db > th) {
        open_ = true;
        hold_ = holdN;
      } else if (db < th - hy) {
        if (hold_ > 0) --hold_;
        else open_ = false;
      }
      const double target = open_ != flip ? 1.0 : fg;
      g_ = flushd(target + (g_ - target) * (target > g_ ? att : rel));
      L[i] = static_cast<float>(L[i] * g_);
      R[i] = static_cast<float>(R[i] * g_);
    }
    meter_ = static_cast<float>(todb(g_));
    input_ = static_cast<float>(peakIn);
  }
};

// ---- Punch: transient shaper (attack and sustain emphasis) --------------------
class Punch final : public MiniFx {
  Follower a_, b_, c_;
  Smooth out_;
  void clear() override {
    a_.env = b_.env = c_.env = 0;
    out_.setRate(sr_);
    out_.init = false;
    meter_ = 0;
  }
  void run(const PluginState& s, const ProcessContext&, float* L, float* R, int n) override {
    const double at = pv(s, punch::Attack), su = pv(s, punch::Sustain), sp = std::max(0.25, static_cast<double>(pv(s, punch::Speed)));
    a_.set(sr_, 0.3, 30.0 * sp);
    b_.set(sr_, 10.0 * sp, 30.0 * sp);
    c_.set(sr_, 0.3, 300.0 * sp);
    const double outT = dbg(pv(s, punch::Output));
    const bool clip = pb(s, punch::Clip);
    double last = 0;
    for (int i = 0; i < n; ++i) {
      const double x = std::max(std::fabs(L[i]), std::fabs(R[i]));
      const double ea = todb(a_.process(x)), eb = todb(b_.process(x)), ec = todb(c_.process(x));
      const double gdb = std::clamp(at * std::clamp(ea - eb, 0.0, 24.0) + su * std::clamp(ec - ea, 0.0, 24.0), -24.0, 24.0);
      last = gdb;
      const double g = dbg(gdb) * out_.next(outT);
      double l = L[i] * g, r = R[i] * g;
      if (clip) {
        l = curve::shear(l, 1.0, 0.15);
        r = curve::shear(r, 1.0, 0.15);
      }
      L[i] = static_cast<float>(l);
      R[i] = static_cast<float>(r);
    }
    meter_ = static_cast<float>(last);
  }
};

}  // namespace

std::unique_ptr<Effect> createDynamics(const std::string& type) {
  if (type == "gl.minipress") return std::make_unique<Press>();
  if (type == "gl.miniceiling") return std::make_unique<Ceiling>();
  if (type == "gl.minishear") return std::make_unique<Shear>();
  if (type == "gl.minipack") return std::make_unique<Pack>();
  if (type == "gl.minispan") return std::make_unique<Span>();
  if (type == "gl.minilatch") return std::make_unique<Latch>();
  if (type == "gl.minipunch") return std::make_unique<Punch>();
  return nullptr;
}

}  // namespace gl::packs::mini::rt
