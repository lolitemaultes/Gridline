// Gridline — "mini" pack: utility family (Trim, Breadth, Skew, Weave).
#include "mini_rt.h"

namespace gl::packs::mini::rt {

namespace {

class Trim final : public MiniFx {
  Smooth g_[2];
  void clear() override {
    for (auto& g : g_) {
      g.setRate(sr_);
      g.init = false;
    }
  }
  void run(const PluginState& s, const ProcessContext&, float* L, float* R, int n) override {
    const double g = dbg(pv(s, trim::Gain));
    const double tl = pb(s, trim::FlipL) ? -g : g, tr = pb(s, trim::FlipR) ? -g : g;
    for (int i = 0; i < n; ++i) {
      L[i] = static_cast<float>(L[i] * g_[0].next(tl));
      R[i] = static_cast<float>(R[i] * g_[1].next(tr));
    }
  }
};

class Breadth final : public MiniFx {
  dsp::Biquad hp_[2];
  Smooth w_, m_, pl_, pr_;
  void clear() override {
    for (auto& b : hp_) b.clear();
    for (Smooth* s : {&w_, &m_, &pl_, &pr_}) {
      s->setRate(sr_);
      s->init = false;
    }
  }
  void run(const PluginState& s, const ProcessContext&, float* L, float* R, int n) override {
    const double width = pv(s, breadth::Width), mid = pv(s, breadth::Mid), pan = pv(s, breadth::Pan);
    const double mono = pv(s, breadth::MonoBelow);
    const bool lowMono = mono >= 20.0;
    if (lowMono)
      for (auto& b : hp_) b.set(dsp::Biquad::HighPass, sr_, mono, 0.7071, 0);
    const double gl = pan > 0 ? 1.0 - pan : 1.0, gr = pan < 0 ? 1.0 + pan : 1.0;
    for (int i = 0; i < n; ++i) {
      double M = 0.5 * (L[i] + R[i]), S = 0.5 * (L[i] - R[i]);
      if (lowMono) S = hp_[1].process(hp_[0].process(S));
      S *= w_.next(width);
      M *= m_.next(mid);
      L[i] = static_cast<float>((M + S) * pl_.next(gl));
      R[i] = static_cast<float>((M - S) * pr_.next(gr));
    }
  }
};

class Skew final : public MiniFx {
  Ring d_;
  Smooth t_, pl_, pr_;

 public:
  Skew() { d_.init(static_cast<size_t>(0.045 * kBufRate)); }

 private:
  void clear() override {
    d_.clear();
    for (Smooth* s : {&t_, &pl_, &pr_}) {
      s->setRate(sr_, 30.0);
      s->init = false;
    }
  }
  void run(const PluginState& s, const ProcessContext&, float* L, float* R, int n) override {
    const double del = std::clamp(static_cast<double>(pv(s, skew::Delay)) * 0.001 * sr_, 1.0, d_.maxDelay());
    const bool right = pi(s, skew::Side) == 1;
    const double bal = pv(s, skew::Balance);
    const double gl = bal > 0 ? 1.0 - bal : 1.0, gr = bal < 0 ? 1.0 + bal : 1.0;
    for (int i = 0; i < n; ++i) {
      float& late = right ? R[i] : L[i];
      d_.push(late);
      late = d_.read(t_.next(del) + 1.0);  // read(1) is the sample just pushed
      L[i] = static_cast<float>(L[i] * pl_.next(gl));
      R[i] = static_cast<float>(R[i] * pr_.next(gr));
    }
  }
};

class Weave final : public MiniFx {
  Smooth m_[4];
  void clear() override {
    for (auto& m : m_) {
      m.setRate(sr_);
      m.init = false;
    }
  }
  void run(const PluginState& s, const ProcessContext&, float* L, float* R, int n) override {
    const double t[4] = {pv(s, weave::LL), pv(s, weave::RL), pv(s, weave::LR), pv(s, weave::RR)};
    for (int i = 0; i < n; ++i) {
      double ll = m_[0].next(t[0]), rl = m_[1].next(t[1]), lr = m_[2].next(t[2]), rr = m_[3].next(t[3]);
      const double l = L[i], r = R[i];
      L[i] = static_cast<float>(ll * l + rl * r);
      R[i] = static_cast<float>(lr * l + rr * r);
    }
  }
};

}  // namespace

std::unique_ptr<Effect> createUtility(const std::string& type) {
  if (type == "gl.minitrim") return std::make_unique<Trim>();
  if (type == "gl.minibreadth") return std::make_unique<Breadth>();
  if (type == "gl.miniskew") return std::make_unique<Skew>();
  if (type == "gl.miniweave") return std::make_unique<Weave>();
  return nullptr;
}

}  // namespace gl::packs::mini::rt
