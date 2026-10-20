// Gridline — "synths" pack, J-33 group "samplekeys": Sample Keys runtime, the
// reference keyboard instrument's voice engine as analysed:
//   note → key group (hardness shifts the group, velocity adds to the shift),
//   velocity layer, per-key tuning pattern (detune) and stretch; 16.16 fixed
//   point reader with 7-bit linear interpolation; envelope = plain multiplier
//   (key-dependent decay / release rates); positive-half overdrive; one-pole
//   "muffle" with a Nyquist zero; key stereo gains; master treble shelf,
//   quadrature LFO (tremolo / auto-pan) and a 128-sample widener.
#include <algorithm>
#include <cmath>
#include "../Packs.h"
#include "samplekeys.h"

namespace gl::packs::synths {

using namespace skeys;

namespace {

struct KeysAux final : AuxData {
  std::shared_ptr<const KeySet> set;
};

class SampleKeysGen final : public Generator {
 public:
  bool active() const override {
    for (const auto& v : v_)
      if (v.on) return true;
    return tail_ > 0;
  }

 protected:
  static constexpr int kVoices = 32;
  struct Voice {
    bool on = false;
    uint32_t id = 0;
    const KeySample* s = nullptr;
    int64_t idx = 0;
    uint32_t frac = 0, step = 0;
    double env = 0, mul = 1, kf = 1, gL = 0, gR = 0, vol = 1;
    double prev[2] = {0, 0}, y[2] = {0, 0};
    double n = 60, off = 0;
  };

  void configure(const PluginState& st, const ChannelAux& aux, const ProcessContext& ctx) override {
    sr_ = ctx.sampleRate > 0 ? ctx.sampleRate : 44100.0;
    const auto* a = dynamic_cast<const KeysAux*>(aux.custom.get());
    set_ = a ? a->set.get() : nullptr;
    for (int i = 0; i < SkCount; ++i) {
      const double raw = i < static_cast<int>(st.params.size()) ? st.params[static_cast<size_t>(i)] : kSkDefault[i];
      p_[i] = (std::clamp(raw, static_cast<double>(kSkMin[i]), static_cast<double>(kSkMax[i])) - kSkMin[i]) / (kSkMax[i] - kSkMin[i]);
    }
    hardOfs_ = std::round(12.0 * p_[SkHardness] - 6.0);
    velHard_ = 0.12 * p_[SkVelHardness];
    velMuf_ = 5.0 * p_[SkVelMuffle] * p_[SkVelMuffle];
    const double p6 = p_[SkVelSens];
    vs_ = 2.0 * p6 + 1.0;
    if (p6 < 0.25) vs_ -= 0.75 - 3.0 * p6;
    const double p7 = p_[SkStereo];
    width_ = p7 * p7, keyGain_ = 1.0 - 0.53 * p7 * p7, keySpread_ = std::min(0.04 * p7, 0.03);
    tune_ = 2.0 * p_[SkTune] - 1.0;
    detune_ = 0.077 * p_[SkDetune] * p_[SkDetune];
    stretch_ = 0.000434 * (p_[SkStretch] - 0.5);
    trebAmt_ = 4.0 * p_[SkTreble] * p_[SkTreble] - 1.0;
    trebK_ = p_[SkTreble] <= 0.5 ? 0.1071 : 0.27201;
    const double m = 2.0 * p_[SkPanTrem] - 1.0;
    modL_ = m, modR_ = std::fabs(m);
    omega_ = 6.283 * std::exp(9.22 * p_[SkLfoRate] - 4.61) / sr_;
    od_ = 1.8 * p_[SkOverdrive];
  }

  void onNoteOn(uint32_t id, double pitch, float velocity) override {
    if (!set_ || set_->samples.empty()) return;
    const KeySet& k = *set_;
    const double n = std::clamp(pitch, 0.0, 127.0);
    const double Vol = std::max(0.0, static_cast<double>(velocity) * 127.0 / 100.0);
    const int vel = static_cast<int>(std::min(127.0, std::round(100.0 * std::log(1.0 + 10.0 * Vol) / std::log(11.0))));
    // Polyphony: with every voice alive, the quietest one goes.
    Voice* v = nullptr;
    for (auto& x : v_)
      if (!x.on) {
        v = &x;
        break;
      }
    if (!v) {
      v = &v_[0];
      for (auto& x : v_)
        if (x.env < v->env) v = &x;
    }
    // Key group by hardness, then the velocity layer.
    const double shift = hardOfs_ + (vel > 40 ? std::round((vel - 40) * velHard_) : 0.0);
    const int groups = static_cast<int>(k.samples.size()) / std::max(1, k.layers);
    int g = groups - 1;
    for (int i = 0; i < groups; ++i)
      if (k.samples[static_cast<size_t>(i * k.layers)].high + shift >= n) {
        g = i;
        break;
      }
    int layer = 0;
    for (int l = 1; l < k.layers; ++l)
      if (k.threshold[l] < vel) layer = l;
    const size_t si = static_cast<size_t>(g * k.layers + layer);
    if (si >= k.samples.size()) return;
    *v = Voice{};
    v->on = true, v->id = id, v->s = &k.samples[si], v->n = n;
    const double kk = std::round((n - 60.0) * (n - 60.0));
    v->off = tune_ + (std::fmod(kk, 13.0) - 6.5) * detune_;
    if (n > 60) v->off += kk * stretch_;
    setStep(*v, n);
    v->idx = 0, v->frac = 0;
    v->env = k.velocityFactor;
    double fc = (vel - 64) * velMuf_ + 160.0 * p_[SkMuffle] * p_[SkMuffle] + 50.0;
    fc = std::clamp(fc, 0.4 * n + 55.0, 210.0);
    v->kf = fc * fc / sr_;
    const double nk = std::clamp(n, 12.0, 108.0);
    const double gg = 0.2 * keyGain_;
    v->gR = gg + (nk - 60.0) * gg * keySpread_;
    v->gL = 2.0 * gg - v->gR;
    if (p_[SkDecay] == 0.0) {
      v->mul = 1.0;
    } else {
      double d = 2.0 * p_[SkDecay];
      if (d < 1.0) d = 1.5 * p_[SkDecay] + 0.25;
      const double nd = std::max(nk, 44.0);
      v->mul = std::exp(-std::exp(0.033 * nd - 0.6 - d) / sr_);
    }
    v->vol = vs_ == 0 ? 1.0 : std::pow(Vol, vs_);
  }
  void setStep(Voice& v, double n) {
    const double r = std::exp2((n - v.s->root + v.off) / 12.0) * set_->rate / sr_ * 65536.0;
    v.step = static_cast<uint32_t>(std::clamp(std::round(r), 0.0, 4294967295.0));
  }
  void release(Voice& v) {
    const double n = v.n;
    double rate;
    if (!set_->releaseHigh && n < 94) rate = std::exp(0.017 * n + 2.0 - 2.0 * p_[SkRelease]);
    else rate = std::exp(0.01 * n + 6.0 - 5.0 * p_[SkRelease]);
    v.mul = std::exp(-rate / sr_);
  }
  void onNoteOff(uint32_t id) override {
    for (auto& v : v_)
      if (v.on && v.id == id) release(v), v.id = 0;
  }
  void onGlide(uint32_t id, double pitch, double) override {
    for (auto& v : v_)
      if (v.on && v.id == id && set_) v.n = std::clamp(pitch, 0.0, 127.0), setStep(v, v.n);
  }
  void onAllOff() override {
    for (auto& v : v_)
      if (v.on && set_) release(v), v.id = 0;
  }
  void onKill() override {
    for (auto& v : v_) v.on = false;
    std::fill(std::begin(wide_), std::end(wide_), 0.0);
    lp_[0] = lp_[1] = 0, sinS_ = 0, cosS_ = 1, tail_ = 0;
  }

  void render(float* L, float* R, int n) override {
    if (!set_) return;
    const KeySet& k = *set_;
    const int ch = k.channels;
    const int16_t* data = k.data.data();
    bool any = false;
    for (int i = 0; i < n; ++i) {
      double ol = 0, orr = 0;
      for (auto& v : v_) {
        if (!v.on) continue;
        v.frac += v.step;
        v.idx += v.frac >> 16;
        v.frac &= 0xFFFF;
        if (v.idx > v.s->last) {
          if (!v.s->loops) {
            v.on = false;
            continue;
          }
          v.idx -= v.s->loop;
          if (v.idx > v.s->last) v.idx = v.s->last;
        }
        const double t = static_cast<double>(v.frac >> 9) / 128.0;
        double s[2];
        for (int c = 0; c < 2; ++c) {
          const int cc = std::min(c, ch - 1);
          const size_t a = (v.s->start + static_cast<size_t>(v.idx)) * static_cast<size_t>(ch) + static_cast<size_t>(cc);
          const double x0 = data[a], x1 = data[a + static_cast<size_t>(ch)];
          s[c] = (x0 + t * (x1 - x0)) / 32768.0 * v.env;
          if (od_ != 0 && s[c] > 0) {
            s[c] -= od_ * s[c] * s[c];
            if (s[c] < 0) s[c] += 0.5 * s[c] * s[c];
          }
          v.y[c] += v.kf * (s[c] + v.prev[c] - v.y[c]);
          v.prev[c] = s[c];
        }
        v.env *= v.mul;
        ol += v.vol * v.gL * v.y[0];
        orr += v.vol * v.gR * v.y[1];
        if (v.env < 1e-4) v.on = false;
        any = true;
      }
      // Master: treble shelf, LFO, widener.
      double x[2] = {ol, orr};
      for (int c = 0; c < 2; ++c) {
        lp_[c] += (x[c] - lp_[c]) * trebK_;
        x[c] += (x[c] - lp_[c]) * trebAmt_;
      }
      sinS_ += omega_ * cosS_;
      cosS_ -= omega_ * sinS_;
      x[0] *= 1.0 + modL_ * cosS_;
      x[1] *= 1.0 + modR_ * cosS_;
      const double dl = width_ * wide_[wi_];
      wide_[wi_] = x[0] + x[1];
      wi_ = (wi_ + 1) & 127;
      L[i] += static_cast<float>(x[0] + dl);
      R[i] += static_cast<float>(x[1] - dl);
    }
    if (std::fabs(lp_[0]) < 1e-10) lp_[0] = 0;
    if (std::fabs(lp_[1]) < 1e-10) lp_[1] = 0;
    if (any) tail_ = 256;
    else if (tail_ > 0) tail_ = std::max(0, tail_ - n);
  }

 private:
  double sr_ = 44100;
  const KeySet* set_ = nullptr;
  double p_[SkCount] = {};
  double hardOfs_ = 0, velHard_ = 0, velMuf_ = 0, vs_ = 1, width_ = 0, keyGain_ = 1, keySpread_ = 0, tune_ = 0, detune_ = 0, stretch_ = 0;
  double trebAmt_ = 0, trebK_ = 0.1071, modL_ = 0, modR_ = 0, omega_ = 0, od_ = 0;
  double lp_[2] = {0, 0}, sinS_ = 0, cosS_ = 1;
  double wide_[128] = {};
  int wi_ = 0;
  int tail_ = 0;
  Voice v_[kVoices];
};

}  // namespace

std::unique_ptr<Generator> makeSampleKeys() { return std::make_unique<SampleKeysGen>(); }

bool prepareSampleKeysAux(const PluginState& state, ChannelAux& aux) {
  if (state.type != "gl.samplekeys") return false;
  aux = ChannelAux{};
  auto a = std::make_shared<KeysAux>();
  a->set = keySet(state.data);
  aux.custom = a;
  return true;
}

}  // namespace gl::packs::synths
