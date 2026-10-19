// Gridline — "synths" pack, J-22: Finger Bass (gl.fingerbass), a plucked
// bass-guitar model with a three-band tone stack.
//
// Each voice is a digital waveguide string: a fractional delay line tuned to
// the note, a loss filter whose brightness falls with time (fingertip
// damping), a finger excitation (soft half-cosine pulse plus a little
// filtered noise for the attack) and a pickup comb (the pickup sits at 1/5 of
// the string). Note-off damps the string over ~70 ms. The tone stack follows
// the reference's three knobs (after a 5.2 kHz pickup roll-off): low shelf at 90 Hz, mid peak at 700 Hz, high
// shelf at 2.8 kHz, each ±12 dB around the centre position.
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>
#include "../Packs.h"
#include "va.h"
#include "va_fx.h"

namespace gl::packs::synths::va {

namespace {

constexpr int kVoices = 8;
constexpr int kLineLen = 16384;  // one period of 20 Hz up to 192 kHz

struct BassVoice {
  std::vector<float> line = std::vector<float>(kLineLen, 0.f);
  int w = 0;
  double period = 100;  // samples
  double loss = 0.995, relLoss = 0.9, bright = 0.5, lp = 0;
  bool on = false, held = false;
  uint32_t id = 0;
  double age = 0, level = 0, amp = 1;
  int excite = 0, exciteLen = 1;
  dsp::Noise noise;
  dsp::Allpass1 tune;  // fractional delay
  float out = 0;

  void start(uint32_t nid, double pitch, float vel, double sr) {
    id = nid;
    on = held = true;
    const double hz = std::clamp(dsp::noteToHz(pitch), 20.0, 2000.0);
    period = std::clamp(sr / hz, 4.0, kLineLen - 8.0);
    amp = 0.35 + 0.65 * vel;
    bright = 0.25 + 0.55 * vel;  // harder plucks are brighter
    exciteLen = std::max(4, static_cast<int>(sr * (0.0045 - 0.002 * vel)));
    excite = exciteLen;
    age = 0;
    lp = 0;
    noise.seed(nid * 2654435761u + 7u);
    // Frequency-dependent loss: low notes ring ~4 s, high notes ~1.2 s (T60).
    const double t60 = std::clamp(5.0 - 0.0018 * hz * 2.0, 1.2, 5.0);
    loss = std::pow(10.0, -3.0 / (t60 * hz));
    relLoss = std::pow(10.0, -3.0 / (0.07 * hz));  // fingertip damping after note-off: T60 70 ms
    const double frac = period - std::floor(period);
    tune.a = (1.0 - frac) / (1.0 + frac);
  }
  double next(double sr) {
    if (!on) return 0;
    double ex = 0;
    if (excite > 0) {
      const double ph = 1.0 - static_cast<double>(excite) / exciteLen;
      ex = amp * (0.5 - 0.5 * std::cos(dsp::kTwoPi * ph)) + amp * 0.12 * noise.next() * (1.0 - ph);
      --excite;
    }
    const int ip = static_cast<int>(std::floor(period));
    const float d = line[static_cast<size_t>((w - ip + kLineLen) & (kLineLen - 1))];
    // Loss filter: one-pole low pass whose cutoff closes as the note ages.
    const double coef = std::clamp(bright * std::exp(-age * 1.6) + 0.18, 0.05, 0.95);
    lp = lp + (d - lp) * coef;
    double y = tune.process(lp) * (held ? loss : relLoss);
    if (std::fabs(y) < 1e-20) y = 0;
    line[static_cast<size_t>(w)] = static_cast<float>(y + ex);
    // Pickup comb at 1/5 of the string.
    const int pk = std::max(1, ip / 5);
    const float pick = line[static_cast<size_t>((w - pk + kLineLen) & (kLineLen - 1))];
    w = (w + 1) & (kLineLen - 1);
    age += 1.0 / sr;
    level = level * 0.999 + std::fabs(y) * 0.001;
    if (excite == 0 && age > 0.02 && level < 2e-6) {
      on = false;
      std::fill(line.begin(), line.end(), 0.f);
    }
    return (y - pick) * 0.9;
  }
};

class FingerBass final : public Generator {
 public:
  FingerBass() = default;
  bool active() const override {
    for (const auto& v : v_)
      if (v.on) return true;
    return false;
  }

 protected:
  void onNoteOn(uint32_t id, double pitch, float vel) override {
    BassVoice* use = nullptr;
    for (auto& v : v_)
      if (!v.on) {
        use = &v;
        break;
      }
    if (!use) {  // steal the oldest
      use = &v_[0];
      for (auto& v : v_)
        if (v.age > use->age) use = &v;
    }
    use->start(id, pitch, vel, sr_);
  }
  void onNoteOff(uint32_t id) override {
    for (auto& v : v_)
      if (v.on && v.id == id) v.held = false;
  }
  void onAllOff() override {
    for (auto& v : v_) v.held = false;
  }
  void onKill() override {
    for (auto& v : v_) {
      v.on = false;
      std::fill(v.line.begin(), v.line.end(), 0.f);
    }
    for (auto& b : eq_) b.clear();
    pickupLp_.z = 0;
    dc_.reset();
  }
  void configure(const PluginState& s, const ChannelAux&, const ProcessContext& ctx) override {
    sr_ = ctx.sampleRate;
    auto db = [&](int p) { return (static_cast<double>(prm(s, p, 0.5f)) - 0.5) * 24.0; };
    eq_[0].set(dsp::Biquad::LowShelf, sr_, 90.0, 0.7, db(BbBass));
    eq_[1].set(dsp::Biquad::Peak, sr_, 700.0, 0.8, db(BbMid));
    eq_[2].set(dsp::Biquad::HighShelf, sr_, 2800.0, 0.7, db(BbTreble));
    pickupLp_.setFreq(sr_, 5200.0);  // pickup / cable roll-off
    dc_.setFreq(sr_, 20.0);
  }
  void render(float* L, float* R, int n) override {
    for (int i = 0; i < n; ++i) {
      double x = 0;
      for (auto& v : v_) x += v.next(sr_);
      x = dc_.process(pickupLp_.process(x));
      for (auto& b : eq_) x = b.process(x);
      x = softClip(x * 0.9);
      L[i] += static_cast<float>(x);
      R[i] += static_cast<float>(x);
    }
  }

 private:
  std::array<BassVoice, kVoices> v_{};
  std::array<dsp::Biquad, 3> eq_{};
  dsp::OnePole pickupLp_;
  dsp::DcBlock dc_;
  double sr_ = 44100;
};

}  // namespace

std::unique_ptr<Generator> makeFingerBass() { return std::make_unique<FingerBass>(); }

}  // namespace gl::packs::synths::va
