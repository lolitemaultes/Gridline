// Gridline — "synths" pack, J-22: FM Duo (gl.fmduo) runtime.
//
// One sine carrier, two modulators in parallel (each with its own frequency
// ratio, init → sustain envelope, release and velocity sensitivity) phase-
// modulating it. Knob laws (all positions 0..1, as stored by the reference):
//   carrier attack   one-pole rise, time constant e^(8p − 8) s
//   carrier decay    exponential fall, time constant e^(8p − 5) s; above 0.98
//                    the carrier holds (no decay)
//   carrier release  time constant e^(5p − 5) s
//   ratio            floor(40.1·coarse²) + fine: below 0.5 a small offset
//                    0.2·fine², above it the steps ¼, ⅓, ½, ⅔, ¾
//   mod time         init → sustain glide, time constant e^(7p − 6) s
//   mod release      time constant e^(8p − 5) s
//   mod depth        index = 12·level² radians, × (1 − vel·(1 − velocity))
//   LFO              25·p² Hz sine; vibrato depth 50·p² cents
//   waveform         carrier phase distortion (0 = pure sine)
//   mod thru         0.25·p² of the modulators added to the output
// The constants are the reference's own (its constant pool holds 40.1, 0.2,
// 8.9, 0.25 … 0.75, 0.98, 25, 628.3); the shaping / scaling is Gridline's.
// Realtime-safe: fixed voice array, no allocation in render().
#include <algorithm>
#include <array>
#include <cmath>
#include "../Packs.h"
#include "../../engine/ChannelIns.h"
#include "../../engine/Dsp.h"
#include "dco.h"
#include "synths_common.h"

namespace gl::packs::synths {

namespace {

using namespace dco;

// Ratio from the coarse / fine positions.
double duoRatio(double coarse, double fine) {
  double r = std::floor(40.1 * coarse * coarse);
  if (fine < 0.5) {
    r += 0.2 * fine * fine;
  } else {
    static const double steps[5] = {0.25, 1.0 / 3.0, 0.5, 2.0 / 3.0, 0.75};
    int k = std::clamp(static_cast<int>(fine * 8.9) - 4, 0, 4);
    r += steps[k];
  }
  return r;
}

// Per-sample coefficient of a one-pole with time constant tau seconds.
double coefFor(double tau, double sr) { return 1.0 - std::exp(-1.0 / std::max(1e-5, tau * sr)); }

class FmDuoGen final : public Generator {
 public:
  bool active() const override {
    for (const auto& v : voices_)
      if (v.on) return true;
    return false;
  }

 protected:
  static constexpr int kVoices = 16;
  struct Mod {
    double ph = 0, env = 0, target = 0, depth = 0;
  };
  struct Voice {
    bool on = false, held = false;
    uint32_t id = 0;
    double pitch = 60;
    float vel = 1;
    uint64_t age = 0;
    double carPh = 0, attEnv = 0, decEnv = 1;
    Mod m[2];
    InsVoice ins;  // channel instrument envelopes / filter (J-33)
  };

  void configure(const PluginState& s, const ChannelAux& aux, const ProcessContext& ctx) override {
    sr_ = ctx.sampleRate > 0 ? ctx.sampleRate : 44100.0;
    ins_ = aux.ins;
    tempo_ = ctx.tempo, ppq_ = ctx.ppq, tick0_ = ctx.tick, blockPos_ = 0;
    auto p = [&](int i, float d) { return static_cast<double>(std::clamp(prm(s, i, d), 0.f, 1.f)); };
    attC_ = coefFor(std::exp(8.0 * p(FdAttack, 0) - 8.0), sr_);
    const double dec = p(FdDecay, 0.65f);
    decMul_ = dec > 0.98 ? 1.0 : std::exp(-1.0 / (std::exp(8.0 * dec - 5.0) * sr_));
    relMul_ = std::exp(-1.0 / (std::exp(5.0 * p(FdRelease, 0.441f) - 5.0) * sr_));
    const int base[2] = {FdM1Coarse, FdM2Coarse};
    for (int k = 0; k < 2; ++k) {
      const int b = base[k];
      ratio_[k] = duoRatio(p(b, 0.85f), p(b + 1, 0.329f));
      init_[k] = p(b + 2, 0.23f);
      timeC_[k] = coefFor(std::exp(7.0 * p(b + 3, 0.8f) - 6.0), sr_);
      sus_[k] = p(b + 4, 0.05f);
      relC_[k] = coefFor(std::exp(8.0 * p(b + 5, 0.8f) - 5.0), sr_);
      velSens_[k] = p(b + 6, 0.9f);
    }
    const double vib = p(FdVibrato, 0);
    vibCents_ = 50.0 * vib * vib;
    const double lr = p(FdLfoRate, 0.414f);
    lfoInc_ = 25.0 * lr * lr / sr_;
    shape_ = p(FdWaveform, 0.447f);
    const double thru = p(FdModThru, 0);
    thru_ = 0.25 * thru * thru;
    octave_ = std::clamp(prmi(s, FdOctave, 0), -3, 3);
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
      for (auto& v : voices_) {
        bool better = (!v.held && pick->held) || (v.held == pick->held && v.age < pick->age);
        if (better) pick = &v;
      }
    }
    Voice& v = *pick;
    v = Voice{};
    v.on = v.held = true;
    v.id = id;
    v.pitch = pitch;
    v.vel = std::clamp(vel, 0.f, 1.f);
    v.age = ++clock_;
    for (int k = 0; k < 2; ++k) {
      const double vs = 1.0 - velSens_[k] * (1.0 - v.vel);
      v.m[k].depth = vs;
      v.m[k].env = init_[k];
      v.m[k].target = sus_[k];
    }
    v.ins.start(ins_.get(), sr_, tempo_, ppq_, 0, 0, tick0_ + blockPos_ * tempo_ * ppq_ / (60.0 * sr_));
    v.ins.setNote(pitch, vel);  // J-72: velocity / key tracking
  }
  void onNoteOff(uint32_t id) override {
    for (auto& v : voices_)
      if (v.on && v.held && v.id == id) v.held = false, v.ins.release();
  }
  void onAllOff() override {
    for (auto& v : voices_)
      if (v.held) v.held = false, v.ins.release();
  }
  void onKill() override {
    for (auto& v : voices_) v = Voice{};
    lfoPh_ = 0;
  }

  void render(float* L, float* R, int n) override {
    blockPos_ += n;
    for (int i = 0; i < n; ++i) {
      lfoPh_ += lfoInc_;
      if (lfoPh_ >= 1.0) lfoPh_ -= 1.0;
      const double vib = std::pow(2.0, vibCents_ * std::sin(dsp::kTwoPi * lfoPh_) / 1200.0);
      double out = 0, outR = 0;
      for (auto& v : voices_) {
        if (!v.on) continue;
        // Envelopes.
        v.attEnv += attC_ * (1.0 - v.attEnv);
        v.decEnv *= v.held ? decMul_ : relMul_;
        const double amp = v.attEnv * v.decEnv;
        if (!v.held && amp < 1e-5) {
          v.on = false;
          continue;
        }
        if (v.held && decMul_ < 1.0 && v.decEnv < 1e-5) {
          v.on = false;
          continue;
        }
        const double hz = dsp::noteToHz(v.pitch + 12.0 * octave_ + (v.ins.active() ? v.ins.pitchSemis() : 0.0)) * vib;
        const double inc = hz / sr_;
        double pm = 0, thru = 0;
        for (int k = 0; k < 2; ++k) {
          Mod& m = v.m[k];
          if (v.held)
            m.env += timeC_[k] * (m.target - m.env);
          else
            m.env += relC_[k] * (0.0 - m.env);
          m.ph += inc * ratio_[k];
          m.ph -= std::floor(m.ph);
          const double s = std::sin(dsp::kTwoPi * m.ph);
          const double lvl = m.env * m.env * m.depth;
          pm += 12.0 * lvl * s;
          thru += lvl * s;
        }
        v.carPh += inc;
        v.carPh -= std::floor(v.carPh);
        const double th = dsp::kTwoPi * v.carPh + pm;
        const double car = std::sin(th + 2.0 * shape_ * std::sin(th));
        const double velAmp = 0.25 + 0.75 * v.vel;
        const double o = 0.3 * velAmp * amp * (car + thru_ * thru);
        if (v.ins.active()) {
          float l = static_cast<float>(o), r = static_cast<float>(o);
          v.ins.process(l, r);
          out += l, outR += r;
          if (!v.held && v.ins.done()) v.on = false;
        } else {
          out += o, outR += o;
        }
      }
      L[i] += static_cast<float>(out);
      R[i] += static_cast<float>(outR);
    }
  }

 private:
  std::array<Voice, kVoices> voices_{};
  double sr_ = 44100;
  double attC_ = 1, decMul_ = 1, relMul_ = 0.99;
  double ratio_[2] = {1, 1}, init_[2] = {0, 0}, timeC_[2] = {0.01, 0.01}, sus_[2] = {0, 0}, relC_[2] = {0.01, 0.01},
         velSens_[2] = {0, 0};
  double vibCents_ = 0, lfoInc_ = 0, lfoPh_ = 0, shape_ = 0, thru_ = 0;
  int octave_ = 0;
  uint64_t clock_ = 0;
  std::shared_ptr<const InsSettings> ins_;
  double tempo_ = 120, tick0_ = 0;
  int ppq_ = 96;
  int64_t blockPos_ = 0;
};

}  // namespace

std::unique_ptr<Generator> makeFmDuo() { return std::make_unique<FmDuoGen>(); }

}  // namespace gl::packs::synths
