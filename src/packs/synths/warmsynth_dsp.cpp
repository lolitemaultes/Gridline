// Gridline — "synths" pack, J-22: Warm Trio (gl.warmsynth) runtime.
//
// Signal flow and laws follow the reference subtractive synth's engine
// (studied with REA for behaviour; the code here is Gridline's own):
//   oscillators  a 4096-point single cycle per oscillator, rebuilt when the
//                shape, width or octave buttons change. Shapes (selector
//                order): charged pulse with the width knob (2..98 %), rounded
//                saw, triangle, noise, and a sine that morphs into sine⁴ on
//                the width knob. The "+1" / "+2" buttons ADD the cycle one /
//                two octaves up (layering, not transposition). "Warm" layers
//                a copy of the oscillator 20 cents up.
//                Level law (level·100 / 100)²; coarse ±24 st, fine ±100 ct;
//                LFO and envelope pitch depth ±12 st (bipolar knobs); osc 2 /
//                osc 3 carry a built-in −4 / +4 ct detune. Ring 1×2 multiplies
//                oscillator 1 by twice oscillator 2 instead of adding it.
//   LFO          half period max(64, (1 − r)³·4·g) samples (r = rate knob,
//                g = the sample rate); shapes
//                square (0/1), ramp (1 → 0 per half), triangle (bipolar for
//                pitch, unipolar for the filter), random step; delay = fade-in
//                over 8 s·delay knob after a retriggered note. Retrigger
//                restarts it per note, else it runs from the song position.
//   envelopes    linear ADSR in ms: attack 8000·a³, decay 8000·d², sustain s,
//                release 8000·r². Amplitude = env during the attack, env²
//                afterwards, squared again and smoothed (0.03 / sample).
//                "Track amp" makes the filter follow the amp envelope instead
//                of its own.
//   filter       two cascaded state-variable stages (coefficients rateDiv·c²
//                and rateDiv·c, c = cut + env·amt + LFO·amt + key·(note+24)/127),
//                damping (1 − emphasis)² + 0.1; outputs blended by the High /
//                Band knobs on quarter-circle laws (both 0 = low pass).
//   output       velocity × velocity-knob level; chorus = a vibrato delay
//                (read rate wandering ±0.72 %) added to the left channel.
// Realtime-safe: fixed arrays; cycle rebuilds use preallocated buffers.
#include <algorithm>
#include <array>
#include <cmath>
#include "../Packs.h"
#include "../../engine/Dsp.h"
#include "dco.h"
#include "synths_common.h"

namespace gl::packs::synths {

namespace {

using namespace dco;

constexpr int kCycle = 4096;
constexpr int kWarmVoices = 12;

// One cycle of a shape (before octave layering) at phase x (0..1); `knob` is
// the width knob position (0..1).
double warmBase(int shape, double x, double knob) {
  switch (shape) {
    case WsSaw: return std::sin((1.0 - 2.0 * x) * dsp::kPi * 0.5);  // falling ramp with rounded ends
    case WsTriangle: return x < 0.25 ? 4.0 * x : x < 0.75 ? 2.0 - 4.0 * x : 4.0 * x - 4.0;
    case WsSine: {  // sine morphing into sine^4 on the width knob
      const double m = std::clamp((std::floor(knob * 96.0) + 4.0) / 96.0, 0.0, 1.0);
      const double s = std::sin(dsp::kTwoPi * x);
      return s + (s * s * s * s - s) * m;
    }
    default: return 0.0;
  }
}

struct Cycle {
  std::array<float, kCycle> t{};
  int shape = -1;
  float knob = -1;
  bool o1 = false, o2 = false;
  void build(int sh, float kn, bool oct1, bool oct2) {
    shape = sh;
    knob = kn;
    o1 = oct1;
    o2 = oct2;
    std::array<float, kCycle> base{};
    if (sh == WsPulse) {
      // Charged pulse: rises towards +0.65 for the duty (2..98 %), falls
      // towards −0.65 after it and relaxes to 0 over the last 8 points.
      const int duty = static_cast<int>((std::floor(kn * 96.0) + 2.0) * 40.96);
      double f = 0;
      for (int i = 0; i < kCycle; ++i) {
        if (i < duty) f = f * 0.65 + 0.2275;
        else if (i < kCycle - 8) f = f * 0.65 - 0.2275;
        else f *= 0.65;
        base[static_cast<size_t>(i)] = static_cast<float>(f);
      }
    } else {
      for (int i = 0; i < kCycle; ++i) base[static_cast<size_t>(i)] = static_cast<float>(warmBase(sh, static_cast<double>(i) / kCycle, kn));
    }
    // "+1" / "+2": the cycle added one / two octaves up.
    for (int i = 0; i < kCycle; ++i) {
      double v = base[static_cast<size_t>(i)];
      if (oct1) v += base[static_cast<size_t>((2 * i) & (kCycle - 1))];
      if (oct2) v += base[static_cast<size_t>((4 * i) & (kCycle - 1))];
      t[static_cast<size_t>(i)] = static_cast<float>(v);
    }
    // Light two-point smoothing (the reference's standard-quality tables).
    float prev = t[0];
    for (int i = 1; i < kCycle; ++i) {
      const float cur = t[static_cast<size_t>(i)];
      t[static_cast<size_t>(i - 1)] = 0.5f * (prev + cur);
      prev = cur;
    }
  }
  float at(double ph) const {
    const double p = ph * kCycle;
    const int i0 = static_cast<int>(p) & (kCycle - 1);
    const double f = p - std::floor(p);
    const float a = t[static_cast<size_t>(i0)], b = t[static_cast<size_t>((i0 + 1) & (kCycle - 1))];
    return static_cast<float>(a + (b - a) * f);
  }
};

// Linear ADSR in samples (the reference's segment envelope).
struct LinEnv {
  int stage = 4;  // 0 attack, 1 decay, 2 sustain, 3 release, 4 off
  double v = 0;
  double att = 1, dec = 1, sus = 0.5, rel = 1;  // samples / level
  void set(double sr, double a, double d, double s, double r) {
    att = std::max(1.0, 8.0 * a * a * a * sr);
    dec = std::max(1.0, 8.0 * d * d * sr);
    sus = std::clamp(s, 0.0, 1.0);
    rel = std::max(1.0, 8.0 * r * r * sr);
  }
  void trigger() {
    stage = 0;
    v = 0;
  }
  void release() {
    if (stage < 3) stage = 3;
  }
  double tick() {
    switch (stage) {
      case 0:
        v += 1.0 / att;
        if (v >= 1.0) {
          v = 1.0;
          stage = 1;
        }
        break;
      case 1:
        v -= (1.0 - sus) / dec;
        if (v <= sus) {
          v = sus;
          stage = 2;
        }
        break;
      case 2: v = sus; break;
      case 3:
        v -= 1.0 / rel;
        if (v <= 0) {
          v = 0;
          stage = 4;
        }
        break;
      default: v = 0; break;
    }
    return v;
  }
};

class WarmGen final : public Generator {
 public:
  bool active() const override {
    for (const auto& v : voices_)
      if (v.on) return true;
    return false;
  }

 protected:
  struct Voice {
    bool on = false, held = false;
    uint32_t id = 0;
    double pitch = 60;
    float vel = 1;
    uint64_t age = 0;
    double ph[3] = {0, 0, 0}, ph2[3] = {0, 0, 0};
    LinEnv amp, flt;
    double ampSmooth = 0;
    double l1 = 0, b1 = 0, l2 = 0, b2 = 0;  // filter stages
    double lfoPh = 0, lfoFade = 1, lfoFadeInc = 0;
    double lfoRnd = 0;
    uint32_t noise = 22222;
    uint32_t lfoCycle = 0;
  };

  void configure(const PluginState& s, const ChannelAux&, const ProcessContext& ctx) override {
    sr_ = ctx.sampleRate > 0 ? ctx.sampleRate : 44100.0;
    auto p = [&](int i, float d) { return static_cast<double>(std::clamp(prm(s, i, d), 0.f, 1.f)); };
    for (int k = 0; k < 3; ++k) {
      on_[k] = prmb(s, WmOsc1On + k);
      const double lv = p(WmOsc1Level + 3 * k, 0.5f);
      level_[k] = static_cast<double>(static_cast<int>(lv * 100.0)) / 100.0;
      level_[k] *= level_[k];
      lfoAmt_[k] = 12.0 * (p(WmOsc1Lfo + 3 * k, 0.5f) * 2.0 - 1.0);
      envAmt_[k] = 12.0 * (p(WmOsc1Env + 3 * k, 0.5f) * 2.0 - 1.0);
      tune_[k] = std::clamp(prm(s, WmOsc1Coarse + 3 * k, 0.f), -24.f, 24.f) + (p(WmOsc1Fine + 3 * k, 0.5f) * 2.0 - 1.0) +
                 (k == 1 ? -0.04 : k == 2 ? 0.04 : 0.0);
      const int sh = std::clamp(prmi(s, WmOsc1Shape + k, 1), 0, WsCount - 1);
      const float knob = static_cast<float>(p(WmOsc1Pw + 3 * k, 0.5f));
      shape_[k] = sh;
      const bool o1 = prmb(s, WmOsc1Oct1 + 3 * k), o2 = prmb(s, WmOsc1Oct2 + 3 * k);
      warm_[k] = prmb(s, WmOsc1Warm + 3 * k);
      Cycle& c = cyc_[k];
      if (sh != WsNoise && (c.shape != sh || c.knob != knob || c.o1 != o1 || c.o2 != o2)) c.build(sh, knob, o1, o2);
    }
    ring_ = prmb(s, WmRing);
    lfoOn_ = prmb(s, WmLfoOn);
    retrig_ = prmb(s, WmRetrig);
    lfoShape_ = std::clamp(prmi(s, WmLfoShape, WlTriangle), 0, WlCount - 1);
    const double r = std::floor(p(WmLfoRate, 0.5f) * 999.0) / 999.0;
    const double g = sr_;
    lfoHalf_ = std::max(64.0, (1.0 - r) * (1.0 - r) * (1.0 - r) * 4.0 * g);
    lfoDelay_ = p(WmLfoDelay, 0) * 8.0 * sr_;
    svfOn_ = prmb(s, WmSvfOn);
    cut_ = std::floor(p(WmCut, 0.5f) * 255.0) / 255.0;
    const double e = std::floor(p(WmEmph, 0) * 255.0) / 255.0;
    damp_ = (1.0 - e) * (1.0 - e) + 0.1;
    fEnv_ = (std::floor(p(WmSvfEnv, 0.5f) * 255.0 - 127.5)) / 128.0;
    fLfo_ = (std::floor(p(WmSvfLfo, 0.5f) * 255.0 - 127.5)) / 128.0;
    kb_ = p(WmSvfKb, 0);
    const double th = std::floor(p(WmSvfHigh, 0) * 255.0) * dsp::kPi / 510.0;
    const double tb = std::floor(p(WmSvfBand, 0) * 255.0) * dsp::kPi / 510.0;
    mixH_ = std::sin(th);
    mixL_ = std::cos(th) * std::cos(tb);
    mixB_ = std::cos(th) * std::sin(tb);
    trackAmp_ = prmb(s, WmTrackAmp);
    velLevel_ = p(WmVcaVel, 1);
    chorus_ = prmb(s, WmChorus);
    rateDiv_ = 44100.0 / sr_;
    for (auto& v : voices_) {
      v.amp.set(sr_, p(WmAmpA, 0), p(WmAmpD, 0.5f), std::floor(p(WmAmpS, 0.5f) * 255.0) / 255.0,
                p(WmAmpR, 0.15f));
      v.flt.set(sr_, p(WmSvfA, 0), p(WmSvfD, 0.5f), std::floor(p(WmSvfS, 0.5f) * 255.0) / 255.0, p(WmSvfR, 0.15f));
    }
  }

  void onNoteOn(uint32_t id, double pitch, float vel) override {
    Voice* pick = nullptr;
    for (auto& v : voices_)
      if (!v.on) {
        pick = &v;
        break;
      }
    if (!pick) {
      pick = &voices_[0];
      for (auto& v : voices_)
        if ((!v.held && pick->held) || (v.held == pick->held && v.age < pick->age)) pick = &v;
    }
    Voice& v = *pick;
    const LinEnv a = v.amp, f = v.flt;
    v = Voice{};
    v.amp = a;
    v.flt = f;
    v.on = v.held = true;
    v.id = id;
    v.pitch = pitch;
    v.vel = std::clamp(vel, 0.f, 1.f);
    v.age = ++clock_;
    v.noise = 22222u + static_cast<uint32_t>(clock_ * 7919u);
    v.amp.trigger();
    v.flt.trigger();
    if (retrig_) {
      v.lfoPh = 0;
      v.lfoFade = lfoDelay_ > 1 ? 0.0 : 1.0;
      v.lfoFadeInc = lfoDelay_ > 1 ? 1.0 / lfoDelay_ : 0.0;
    } else {
      v.lfoPh = std::fmod(songPos_ / (2.0 * lfoHalf_), 1.0);
    }
  }
  void onNoteOff(uint32_t id) override {
    for (auto& v : voices_)
      if (v.on && v.held && v.id == id) {
        v.held = false;
        v.amp.release();
        v.flt.release();
      }
  }
  void onAllOff() override {
    for (auto& v : voices_) {
      v.held = false;
      v.amp.release();
      v.flt.release();
    }
  }
  void onKill() override {
    for (auto& v : voices_) v.on = false;
    chorusBuf_.fill(0.f);
  }

  // LFO outputs (bipolar for pitch, unipolar for the filter) at phase ph.
  static void lfoAt(int shape, double ph, double rnd, double& bi, double& uni) {
    const bool second = ph >= 0.5;
    const double f = second ? (ph - 0.5) * 2.0 : ph * 2.0;  // 0..1 inside a half
    switch (shape) {
      case WlSquare: bi = uni = second ? 1.0 : 0.0; break;
      case WlRamp: bi = uni = 1.0 - f; break;
      case WlTriangle: {
        const double fold = std::min(f, 1.0 - f) * 2.0;  // 0..1..0 over the half
        bi = second ? fold : -fold;
        uni = second ? f : 1.0 - f;
        break;
      }
      default: bi = uni = rnd; break;
    }
  }

  void render(float* L, float* R, int n) override {
    const double lfoInc = 1.0 / (2.0 * lfoHalf_);
    for (int i = 0; i < n; ++i) {
      double out = 0;
      for (auto& v : voices_) {
        if (!v.on) continue;
        const double ae = v.amp.tick();
        const double fe = v.flt.tick();
        if (v.amp.stage == 4) {
          v.on = false;
          continue;
        }
        double lfoBi = 0, lfoUni = 0;
        if (lfoOn_) {
          const double before = v.lfoPh;
          v.lfoPh += lfoInc;
          if (v.lfoPh >= 1.0) v.lfoPh -= 1.0;
          if ((before < 0.5) != (v.lfoPh < 0.5)) {
            v.lfoRnd = hashUnit(static_cast<uint32_t>(++v.lfoCycle) * 2654435761u ^ v.noise) * 0.5 + 0.5;
          }
          lfoAt(lfoShape_, v.lfoPh, v.lfoRnd, lfoBi, lfoUni);
          v.lfoFade = std::min(1.0, v.lfoFade + v.lfoFadeInc);
          lfoBi *= v.lfoFade;
          lfoUni *= v.lfoFade;
        }
        const double a = v.amp.stage == 0 ? ae : ae * ae;
        double osc[3] = {0, 0, 0};
        for (int k = 0; k < 3; ++k) {
          if (!on_[k]) continue;
          const double semis = v.pitch + tune_[k] + lfoAmt_[k] * lfoBi + envAmt_[k] * a;
          const double inc = std::min(0.45, dsp::noteToHz(semis) / sr_);
          double s;
          if (shape_[k] == WsNoise) {
            v.noise = v.noise * 1664525u + 1013904223u;
            s = (static_cast<double>(v.noise >> 9) / 4194304.0 - 1.0) * 0.6;
          } else {
            s = cyc_[k].at(v.ph[k]);
            if (warm_[k]) {  // warm: a copy 20 cents up layered on the cycle
              s += cyc_[k].at(v.ph2[k]);
              v.ph2[k] += inc * 1.0116194;
              v.ph2[k] -= std::floor(v.ph2[k]);
            }
          }
          v.ph[k] += inc;
          v.ph[k] -= std::floor(v.ph[k]);
          osc[k] = s * level_[k];
        }
        double sig = (ring_ && on_[0] && on_[1]) ? osc[0] * 2.0 * osc[1] : osc[0] + osc[1];
        sig += osc[2];
        if (svfOn_) {
          const double envSrc = trackAmp_ ? a : fe;
          double c = cut_ + fEnv_ * envSrc + fLfo_ * lfoUni + kb_ * (v.pitch + 24.0) / 127.0;
          c = std::clamp(c, 0.0001, 1.0);
          const double f1 = std::min(1.0, rateDiv_ * c * c), f2 = std::min(1.0, rateDiv_ * c);
          v.l1 += f1 * v.b1;
          const double h1 = sig - damp_ * v.b1 - v.l1;
          v.b1 += f1 * h1;
          const double in2 = mixL_ * v.l1 + mixB_ * v.b1 + mixH_ * h1;
          v.l2 += f2 * v.b2;
          const double h2 = in2 - damp_ * v.b2 - v.l2;
          v.b2 += f2 * h2;
          sig = mixL_ * v.l2 + mixB_ * v.b2 + mixH_ * h2;
          if (!std::isfinite(sig)) {
            v.l1 = v.b1 = v.l2 = v.b2 = 0;
            sig = 0;
          }
        }
        v.ampSmooth += (a * a - v.ampSmooth) * 0.03 * 44100.0 / sr_;
        if (v.ampSmooth < 1e-9) v.ampSmooth = 0;
        out += sig * v.ampSmooth * velLevel_ * v.vel;
      }
      out *= 0.7;
      double left = out;
      if (chorus_) {
        chorusBuf_[static_cast<size_t>(chW_)] = static_cast<float>(out);
        chRate_ += chDir_ * 0.0000008 * 44100.0 / sr_;
        if (chRate_ > 1.0072 || chRate_ < 0.9928) chDir_ = -chDir_;
        chRead_ += chRate_;
        if (chRead_ >= kChorus) chRead_ -= kChorus;
        const int i0 = static_cast<int>(chRead_);
        const double fr = chRead_ - i0;
        const float x0 = chorusBuf_[static_cast<size_t>(i0)], x1 = chorusBuf_[static_cast<size_t>((i0 + 1) % kChorus)];
        left += x0 + (x1 - x0) * fr;
        chW_ = (chW_ + 1) % kChorus;
      }
      L[i] += static_cast<float>(left);
      R[i] += static_cast<float>(out);
    }
    songPos_ += n;
  }

 private:
  static constexpr int kChorus = 4096;
  std::array<Voice, kWarmVoices> voices_{};
  Cycle cyc_[3];
  double sr_ = 44100;
  bool on_[3] = {true, false, false}, warm_[3] = {false, false, false};
  int shape_[3] = {1, 1, 1};
  double level_[3] = {0.25, 0.25, 0.25}, lfoAmt_[3] = {0, 0, 0}, envAmt_[3] = {0, 0, 0}, tune_[3] = {0, 0, 0};
  bool ring_ = false, lfoOn_ = false, retrig_ = false, svfOn_ = false, trackAmp_ = false, chorus_ = false;
  int lfoShape_ = WlTriangle;
  double lfoHalf_ = 22050, lfoDelay_ = 0;
  double cut_ = 0.5, damp_ = 1.1, fEnv_ = 0, fLfo_ = 0, kb_ = 0, mixL_ = 1, mixB_ = 0, mixH_ = 0, velLevel_ = 1;
  double rateDiv_ = 1;
  double songPos_ = 0;
  std::array<float, kChorus> chorusBuf_{};
  int chW_ = 0;
  double chRead_ = kChorus / 2, chRate_ = 1.0, chDir_ = 1.0;
  uint64_t clock_ = 0;
};

}  // namespace

std::unique_ptr<Generator> makeWarmSynth() { return std::make_unique<WarmGen>(); }

}  // namespace gl::packs::synths
