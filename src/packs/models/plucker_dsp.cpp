// Gridline — Plucker (gl.plucker): a lean plucked string. Each note fills a
// tuned delay loop with one period of noise, low-passed by the colour; the
// loop's two-point average (blended toward a brighter one-zero by the colour)
// sets the tone, the loop gain the ring time (decay knob, T60). "Even decay"
// keeps the ring time the same across the keyboard (otherwise high notes die
// sooner, as on a real string: T60 ∝ 1 / f above A4). "Gate" cuts the note
// shortly after the key is released; "Widen" runs a second, slightly
// detuned loop with its own noise burst on the right channel.
#include <cmath>
#include <cstring>
#include "models_rt.h"
#include "strings.h"
#include "strings_laws.h"

namespace gl::packs::models {

using namespace strs;

namespace {

using namespace dsp;
constexpr int kVoices = 24;
constexpr int kLine = 8192;

class PluckerGen final : public Generator {
 public:
  PluckerGen() : v_(new Voice[kVoices]) {}
  bool active() const override {
    for (int i = 0; i < kVoices; ++i)
      if (v_[i].on) return true;
    return false;
  }

 private:
  struct Loop {
    float buf[kLine];
    int w = 0;
    double len = 100, g = 0.99, b = 0.3;
    int N = 100;          // integer part of the loop delay
    double ap = 0;        // first-order allpass coefficient for the fraction (no level loss)
    double apx = 0, apy = 0;
    float z = 0;
  };
  struct Voice {
    bool on = false, held = false;
    uint32_t id = 0;
    double note = 60;
    float vel = 1, env = 1, rel = 1;
    uint64_t age = 0;
    Loop s[2];
    double silent = 0;
  };

  void configure(const PluginState& st, const ChannelAux&, const ProcessContext& ctx) override {
    sr_ = ctx.sampleRate;
    decay_ = prmf(st, PkDecay, 128);
    color_ = std::clamp(prmf(st, PkColor, 64) / 128.0f, 0.0f, 1.0f);
    norm_ = prmf(st, PkNormalize, 1) >= 0.5f;
    gate_ = prmf(st, PkGate, 0) >= 0.5f;
    widen_ = prmf(st, PkWiden, 1) >= 0.5f;
  }

  void tune(Voice& v, int s) {
    const double f = noteToHz(v.note + (s ? (widen_ ? 0.07 : 0.0) : (widen_ ? -0.07 : 0.0)));
    // Loop filter y = (1-b)·x + b·x[n-1] (phase delay b samples); the colour
    // sets b, lowered when the filter alone would ring shorter than asked.
    double t60 = law::pkT60(decay_);
    if (!norm_ && f > 440.0) t60 *= 440.0 / f;
    const double w = kTwoPi * f / sr_;
    auto mag = [w](double bb) { return std::sqrt(1.0 - 2.0 * bb * (1.0 - bb) * (1.0 - std::cos(w))); };
    const double gper = std::pow(10.0, -3.0 / (t60 * f));
    double b = 0.5 * (1.0 - 0.8 * color_);
    if (mag(b) < gper) {
      const double k = (1.0 - gper * gper) / (2.0 * (1.0 - std::cos(w)));
      b = 4 * k < 1 ? 0.5 * (1.0 - std::sqrt(1.0 - 4.0 * k)) : 0.5;
    }
    v.s[s].b = b;
    // Loop delay = period − filter delay, split into an integer read and a
    // fractional allpass (delay d in [0.5, 1.5)).
    const double len = std::clamp(sr_ / f - b, 2.5, static_cast<double>(kLine - 2));
    int N = static_cast<int>(std::floor(len - 0.5));
    double d = len - N;
    v.s[s].len = len;
    v.s[s].N = N;
    v.s[s].ap = (1.0 - d) / (1.0 + d);
    v.s[s].g = std::min(0.99999, gper / std::max(1e-6, mag(b)));
  }

  void onNoteOn(uint32_t id, double pitch, float velocity) override {
    int slot = 0;
    uint64_t best = UINT64_MAX;
    for (int i = 0; i < kVoices; ++i) {
      if (!v_[i].on) {
        slot = i;
        best = 0;
        break;
      }
      if (v_[i].age < best) best = v_[i].age, slot = i;
    }
    Voice& v = v_[slot];
    v.on = v.held = true;
    v.id = id;
    v.note = pitch;
    v.vel = velocity;
    v.age = ++clock_;
    v.env = 1;
    v.silent = 0;
    for (int s = 0; s < 2; ++s) {
      tune(v, s);
      Loop& lp = v.s[s];
      std::memset(lp.buf, 0, sizeof lp.buf);
      lp.w = 0;
      lp.z = 0;
      lp.apx = lp.apy = 0;
      // One period of colour-filtered noise, behind the write head.
      rng_.seed(0xC0FFEEu + static_cast<uint32_t>(clock_) * 7919u + static_cast<uint32_t>(s) * 104729u);
      const int n = std::min(kLine - 1, static_cast<int>(lp.len) + 1);
      const float a = 0.15f + 0.85f * color_;  // burst low pass
      float z = 0, mean = 0;
      float* tmp = burst_;
      for (int i = 0; i < n; ++i) {
        z += (rng_.next() - z) * a;
        tmp[i] = z;
        mean += z;
      }
      mean /= static_cast<float>(n);
      const float gain = 0.9f / (0.25f + 0.75f * a);
      for (int i = 0; i < n; ++i) lp.buf[(kLine - n + i) & (kLine - 1)] = (tmp[i] - mean) * gain;
    }
  }
  void onNoteOff(uint32_t id) override {
    for (int i = 0; i < kVoices; ++i)
      if (v_[i].on && v_[i].held && v_[i].id == id) v_[i].held = false;
  }
  void onAllOff() override {
    for (int i = 0; i < kVoices; ++i) v_[i].held = false;
  }
  void onKill() override {
    for (int i = 0; i < kVoices; ++i) v_[i].on = false;
  }

  void render(float* L, float* R, int n) override {
    const float relCoef = static_cast<float>(std::exp(-1.0 / (0.012 * sr_)));
    for (int vi = 0; vi < kVoices; ++vi) {
      Voice& v = v_[vi];
      if (!v.on) continue;
      const float amp = 0.5f * (0.25f + 0.75f * v.vel);
      for (int i = 0; i < n; ++i) {
        float out[2];
        for (int s = 0; s < 2; ++s) {
          Loop& lp = v.s[s];
          const double xi = lp.buf[(lp.w - lp.N) & (kLine - 1)];
          const double xa = lp.ap * xi + lp.apx - lp.ap * lp.apy;
          lp.apx = xi;
          lp.apy = std::fabs(xa) < 1e-20 ? 0.0 : xa;
          const float x = static_cast<float>(xa);
          float y = static_cast<float>(((1.0 - lp.b) * x + lp.b * lp.z) * lp.g);
          lp.z = x;
          if (std::fabs(y) < 1e-20f) y = 0;
          lp.buf[lp.w] = y;
          lp.w = (lp.w + 1) & (kLine - 1);
          out[s] = y;
        }
        if (gate_ && !v.held) v.env *= relCoef;
        const float e = v.env * amp;
        L[i] += out[0] * e;
        R[i] += (widen_ ? out[1] : out[0]) * e;
        v.silent = std::fabs(out[0]) * e < 1e-5f ? v.silent + 1 : 0;
        if (v.silent > sr_ * 0.05 || v.env < 1e-4f) {  // on the voice's own clock (J-30: not at a block end)
          v.on = false;
          break;
        }
      }
    }
  }

  std::unique_ptr<Voice[]> v_;
  float burst_[kLine] = {};
  Noise rng_;
  double sr_ = 44100;
  float decay_ = 128, color_ = 0.5f;
  bool norm_ = true, gate_ = false, widen_ = true;
  uint64_t clock_ = 0;
};

}  // namespace

std::unique_ptr<Generator> makePlucker() { return std::make_unique<PluckerGen>(); }

}  // namespace gl::packs::models
