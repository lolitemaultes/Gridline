// Gridline — "models" pack, J-22 group "drums": runtimes.
//   Membrane Pad  — one keyboard-pitched membrane drum (drums_voice.h), 8 voices.
//   Membrane Kit  — 16 channels of the same voice (24 shared voices), per-channel
//                   volume / pan / mute / solo / key, a step sequencer whose 16
//                   patterns start while their trigger key is held (tempo-synced
//                   16ths with swing), master level, 8-band EQ and a limiter.
//   Kick Sculptor — pitch-swept sine (base / peak / slide), sub shelf, drive,
//                   attack, per-hit variation, a band-passed click and a sampled
//                   noise layer (ADS envelope, start, pitch, low-pass).
//   Sweep Kick    — sine sweeping from the max to the min frequency, amp decay,
//                   click and distortion.
#include <array>
#include <atomic>
#include <cmath>
#include "../../engine/ChannelIns.h"
#include "../../engine/pitch/Kernel.h"
#include "../Packs.h"
#include "../dynamics/dynamics.h"
#include "drums.h"
#include "drums_ref.h"
#include "drums_voice.h"

namespace gl::packs::models {

namespace {

using namespace dr;
using namespace gl::dsp;

inline float pv(const PluginState& s, int i, float d = 0.f) {
  return i >= 0 && static_cast<size_t>(i) < s.params.size() ? s.params[static_cast<size_t>(i)] : d;
}

// ---------------------------------------------------------------- Membrane Pad
class MembranePadGen final : public Generator {
 public:
  bool active() const override {
    for (const auto& v : voices_)
      if (v.on) return true;
    for (const auto& v : ref_)
      if (v.on) return true;
    return false;
  }

 protected:
  static constexpr int kVoices = 8, kRefVoices = 16;
  void configure(const PluginState& s, const ChannelAux&, const ProcessContext& ctx) override {
    sr_ = ctx.sampleRate;
    for (int k = 0; k < MdCount; ++k) model_[k] = pv(s, PdModel + k);
    out_ = pv(s, PdOutput, 1.0f);
    engine_ = pv(s, PdEngine, 1.0f) >= 0.5f;
    if (engine_ && !refReady_) {
      for (int k = 0; k < kRefVoices; ++k) ref_[static_cast<size_t>(k)].allocate(0x9E3779B9u * static_cast<uint32_t>(k + 1));
      refReady_ = true;
    }
  }
  void onNoteOn(uint32_t id, double pitch, float velocity) override {
    if (engine_ && refReady_) {
      // First voice whose note slot is free (it may still ring), else the oldest.
      RefMembraneVoice* v = nullptr;
      for (auto& x : ref_)
        if (!x.held) {
          v = &x;
          break;
        }
      if (!v) {
        v = &ref_[0];
        for (auto& x : ref_)
          if (x.age < v->age) v = &x;
      }
      float p[MdCount];
      RefMembraneVoice::routes(model_, p, velocity);
      v->id = id;
      v->age = ++clock_;
      v->strike(p, pitch - std::round(pitch), sr_);  // the key does not transpose; fine pitch does
      return;
    }
    MembraneVoice* v = &voices_[0];
    for (auto& x : voices_)
      if (!x.on) {
        v = &x;
        break;
      } else if (x.age < v->age) {
        v = &x;
      }
    float p[MdCount];
    applyVelocity(model_, p, velocity);
    v->id = id;
    v->age = ++clock_;
    v->strike(p, velocity, pitch - 60.0, sr_, static_cast<uint32_t>(clock_));
  }
  void onNoteOff(uint32_t id) override {  // one-shot: the hold control gates the ring
    for (auto& v : ref_)
      if (v.held && v.id == id) v.held = false;
  }
  void onAllOff() override {
    for (auto& v : ref_) v.held = false;
  }
  void onKill() override {
    for (auto& v : voices_) v.on = false;
    for (auto& v : ref_) v.held = false, v.on = false;
  }
  void render(float* L, float* R, int n) override {
    for (auto& v : ref_) {
      if (!v.on) continue;
      for (int i = 0; i < n; ++i) {
        const float y = v.next() * out_;
        L[i] += y;
        R[i] += y;
      }
    }
    const float g = out_ * 0.7f;
    for (auto& v : voices_) {
      if (!v.on) continue;
      for (int i = 0; i < n && v.on; ++i) {
        float y = v.tick() * g;
        L[i] += y;
        R[i] += y;
      }
    }
  }

 private:
  std::array<MembraneVoice, kVoices> voices_;
  std::array<RefMembraneVoice, kRefVoices> ref_;
  bool engine_ = true, refReady_ = false;
  float model_[MdCount] = {};
  float out_ = 1;
  double sr_ = 44100;
  uint64_t clock_ = 0;
};

// ---------------------------------------------------------------- Membrane Kit
struct KitAux final : AuxData {
  KitData d;
};

class MembraneKitGen final : public Generator {
 public:
  bool active() const override {
    if (seq_.on) return true;
    for (const auto& v : voices_)
      if (v.on) return true;
    return false;
  }

 protected:
  static constexpr int kVoices = 24;
  void configure(const PluginState& s, const ChannelAux& aux, const ProcessContext& ctx) override {
    const bool srChanged = sr_ != ctx.sampleRate;
    sr_ = ctx.sampleRate;
    tempo_ = ctx.tempo > 1 ? ctx.tempo : 120.0;
    state_ = &s;
    data_ = dynamic_cast<const KitAux*>(aux.custom.get());
    bool anySolo = false;
    for (int c = 0; c < kKitPads; ++c) anySolo = anySolo || pv(s, kitPad(c, KpSolo)) >= 0.5f;
    for (int c = 0; c < kKitPads; ++c) {
      const bool audible = pv(s, kitPad(c, KpMute)) < 0.5f && (!anySolo || pv(s, kitPad(c, KpSolo)) >= 0.5f);
      const float vol = pv(s, kitPad(c, KpVolume), 0.7f);
      // Reference laws (J-33): pad gain 2·v², linear pan L = g·(1 − p), R = g·p (p = 0..1).
      const float pan01 = std::clamp(pv(s, kitPad(c, KpPan), 0.5f), 0.0f, 1.0f);
      const float g = audible ? 2.0f * vol * vol : 0.0f;
      gl_[c] = g * (1.0f - pan01);
      gr_[c] = g * pan01;
      key_[c] = static_cast<int>(std::lround(pv(s, kitPad(c, KpKey), 60.0f + c)));
    }
    master_ = std::pow(10.0f, 1.6f * pv(s, KmLevel, 0.5f)) / 8.0f;  // reference: 10^(1.6x)·0.25 × output 0.5
    swing_ = pv(s, KmSwing, 0);
    eqOn_ = pv(s, KmEqOn) >= 0.5f;
    float eq[8];
    bool eqChanged = srChanged;
    for (int k = 0; k < 8; ++k) {
      eq[k] = pv(s, KmEq1 + k, 0.5f);
      eqChanged = eqChanged || eq[k] != eqVals_[k];
    }
    if (eqChanged) {
      static const double hz[8] = {60, 150, 400, 1000, 2400, 5000, 9000, 14000};
      for (int k = 0; k < 8; ++k) {
        eqVals_[k] = eq[k];
        for (int c = 0; c < 2; ++c) eqF_[c][k].set(Biquad::Peak, sr_, hz[k], 1.1, (eq[k] - 0.5) * 24.0);
      }
    }
    limOn_ = pv(s, KmLimiter) >= 0.5f;
    limGain_ = static_cast<float>(std::pow(10.0, 1.5 * pv(s, KmLimGain)));  // reference: 10^(1.5x)
    limRel_ = static_cast<float>(std::exp(-1.0 / std::max(1.0, 0.01 * std::pow(100.0, pv(s, KmLimRelease, 0.5f)) * sr_)));
  }
  void strikeChannel(int c, float vel) {
    const PluginState& s = *state_;
    MembraneVoice* v = &voices_[0];
    for (auto& x : voices_)
      if (!x.on) {
        v = &x;
        break;
      } else if (x.age < v->age) {
        v = &x;
      }
    float in[MdCount], p[MdCount];
    for (int k = 0; k < MdCount; ++k) in[k] = pv(s, kitPad(c, KpModel + k));
    applyVelocity(in, p, vel);
    v->tag = c;
    v->age = ++clock_;
    v->strike(p, vel, 0.0, sr_, static_cast<uint32_t>(clock_ * 7919u + static_cast<uint64_t>(c)));
  }
  // J-180: kit pieces play on the note's written key (fine / channel pitch never pick another piece).
  void onNoteOnEx(uint32_t id, double pitch, float velocity, const NoteProps& np) override { onNoteOn(id, pitch - np.detune, velocity); }
  void onNoteOn(uint32_t id, double pitch, float velocity) override {
    const int note = static_cast<int>(std::lround(pitch));
    if (data_)
      for (int k = 0; k < kKitPatterns; ++k)
        if (data_->d.pattern[k].length > 0 && data_->d.pattern[k].key == note) {
          seq_ = Seq{true, id, k, 0, 0.0, velocity};
          return;
        }
    for (int c = 0; c < kKitPads; ++c)
      if (key_[c] == note) strikeChannel(c, velocity);
  }
  void onNoteOff(uint32_t id) override {
    if (seq_.on && seq_.id == id) seq_.on = false, uiStep_.store(-1, std::memory_order_relaxed);
  }
  void onAllOff() override { seq_.on = false, uiStep_.store(-1, std::memory_order_relaxed); }
  int displayStep() const override { return uiStep_.load(std::memory_order_relaxed); }
  void onKill() override {
    seq_.on = false, uiStep_.store(-1, std::memory_order_relaxed);
    for (auto& v : voices_) v.on = false;
  }
  void render(float* L, float* R, int n) override {
    float bl[512], br[512];
    for (int at = 0; at < n; at += 512) {
      const int m = std::min(512, n - at);
      std::fill(bl, bl + m, 0.0f);
      std::fill(br, br + m, 0.0f);
      for (int i = 0; i < m; ++i) {
        if (seq_.on && data_) stepSequencer();
        for (auto& v : voices_) {
          if (!v.on) continue;
          float y = v.tick();
          bl[i] += y * gl_[v.tag];
          br[i] += y * gr_[v.tag];
        }
      }
      for (int i = 0; i < m; ++i) {
        double l = bl[i] * master_, r = br[i] * master_;
        if (eqOn_)
          for (int k = 0; k < 8; ++k) {
            l = eqF_[0][k].process(l);
            r = eqF_[1][k].process(r);
          }
        if (limOn_) {
          l *= limGain_;
          r *= limGain_;
          const float pk = static_cast<float>(std::max(std::fabs(l), std::fabs(r)));
          env_ = pk > env_ ? pk : env_ * limRel_ + pk * (1.0f - limRel_);
          if (env_ > 1.0f) {  // ceiling at full scale
            const double g = 1.0 / env_;
            l *= g;
            r *= g;
          }
        }
        L[at + i] += static_cast<float>(l);
        R[at + i] += static_cast<float>(r);
      }
    }
  }

 private:
  struct Seq {
    bool on = false;
    uint32_t id = 0;
    int pattern = 0, step = 0;  // step: the next step to play
    double pos = 0;  // 16ths since the start of the current pass of the pattern
    float vel = 1;
  };
  // Advances the sequencer by one sample; fires a step's cells at its start.
  // Steps keep to a 16th grid that starts at the trigger key (positions are
  // kept as fractions, so the pattern never drifts from the tempo); swing
  // delays the second step of each pair, counted from the pattern start (the
  // last step of an odd-length pattern has no partner and stays on the grid).
  void stepSequencer() {
    const KitPattern& P = data_->d.pattern[seq_.pattern];
    const int len = std::max(1, P.length);
    if (seq_.step >= len) {  // the pattern got shorter while playing: start its next pass
      seq_.pos -= seq_.step;
      seq_.step = 0;
    }
    const double at = seq_.step + ((seq_.step & 1) ? swing_ * 0.5 : 0.0);
    if (seq_.pos >= at - 1e-9) {
      uiStep_.store(seq_.step, std::memory_order_relaxed);
      for (int c = 0; c < kKitPads; ++c) {
        const uint8_t cell = P.cell[c][seq_.step % kKitSteps];
        if (cell & 0x80) strikeChannel(c, seq_.vel * static_cast<float>(cell & 0x7F) / 127.0f);
      }
      if (++seq_.step >= len) {
        seq_.step = 0;
        seq_.pos -= len;
      }
    }
    seq_.pos += tempo_ / (sr_ * 15.0);
  }

  std::array<MembraneVoice, kVoices> voices_;
  const PluginState* state_ = nullptr;
  const KitAux* data_ = nullptr;
  Seq seq_;
  std::atomic<int> uiStep_{-1};  // J-68: running step for the editor
  float gl_[kKitPads] = {}, gr_[kKitPads] = {};
  int key_[kKitPads] = {};
  float master_ = 1, swing_ = 0, limGain_ = 1, limRel_ = 0.999f, env_ = 0;
  bool eqOn_ = false, limOn_ = false;
  float eqVals_[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
  Biquad eqF_[2][8];
  double sr_ = 0, tempo_ = 120;
  uint64_t clock_ = 0;
};

// ---------------------------------------------------------------- Kick Sculptor
// Control laws [ref] follow the reference kick's own parameter mapping
// (behaviour only): level 10^(2v−1) (±20 dB), sampling ratio 2^(4v−2) (body
// pitch ×¼ … ×4), duration v³ s (linear fade), sub v², attack time ≈ v⁸·10⁵
// samples, base 2^(4v+4) Hz (16 … 256 Hz), peak v³·100 semitones above the base,
// slide time constant (v³·20 + 0.001)·5000 samples, variations ±100 %
// (2v−1), click level v⁵, noise attack / decay v⁶ s, noise pitch 2^(4v−2),
// noise mix 2v², noise filter v². The drive curve, click shape and filters
// are Gridline's own.
class KickSculptGen final : public Generator {
 public:
  bool active() const override {
    for (const auto& v : voices_)
      if (v.on) return true;
    return false;
  }

 protected:
  static constexpr int kVoices = 4;
  struct Voice {
    bool on = false;
    uint64_t age = 0;
    int64_t t = 0, len = 0;
    double ph = 0, base = 50, peak = 200, slideK = 0.999, sweep = 1, pitchMul = 1;
    float vel = 1, amp = 1, ampStep = 0, attack = 1, attackK = 0;
    float click = 0, clickK = 0, nzPos = 0, nzRate = 1, nzEnv = 0, nzA = 1, nzD = 0.999f, nzS = 0.5f;
    bool nzRise = true;
    Biquad clickBp;
    OnePole nzLp;
    Biquad sub;
    Noise rng;
  };
  void configure(const PluginState& s, const ChannelAux& aux, const ProcessContext& ctx) override {
    sr_ = ctx.sampleRate;
    for (int k = 0; k < KsCount; ++k) p_[k] = pv(s, k);
    const int idx = std::clamp(static_cast<int>(std::lround(p_[KsNoiseSample])), 0, kickNoiseCount() - 1);
    noise_ = aux.sample(static_cast<size_t>(idx)).get();
  }
  void onNoteOn(uint32_t, double pitch, float velocity) override {
    Voice* v = &voices_[0];
    for (auto& x : voices_)
      if (!x.on) {
        v = &x;
        break;
      } else if (x.age < v->age) {
        v = &x;
      }
    const float* p = p_;
    *v = Voice{};
    v->on = true;
    v->age = ++clock_;
    v->rng.seed(static_cast<uint32_t>(clock_ * 2654435761u));
    const double sr44 = sr_ / 44100.0;
    auto vary = [&](float ctl, double range) { return 1.0 + 0.5 * (v->rng.next() + 1.0) * (2.0 * ctl - 1.0) * range; };
    v->pitchMul = std::pow(2.0, (pitch - 60.0) / 12.0) * std::pow(2.0, 4.0 * p[KsSampling] - 2.0);
    v->base = std::pow(2.0, 4.0 * p[KsBase] + 4.0) * vary(p[KsVarBase], 0.25);
    const double peakSt = 100.0 * p[KsPeak] * p[KsPeak] * p[KsPeak] * vary(p[KsVarPeak], 0.5);
    v->peak = v->base * std::pow(2.0, std::max(0.0, peakSt) / 12.0);
    const double slide = (20.0 * p[KsSlide] * p[KsSlide] * p[KsSlide] + 0.001) * vary(p[KsVarSlide], 0.5);
    v->slideK = std::exp(-1.0 / std::max(1.0, slide * 5000.0 * sr44));
    v->sweep = 1.0;
    v->ph = 0.5 * (v->rng.next() + 1.0) * p[KsVarPhase] * kPi;
    const double dur = std::max(0.005, static_cast<double>(p[KsDuration] * p[KsDuration] * p[KsDuration]));
    v->len = static_cast<int64_t>(dur * sr_) + 1;
    v->amp = 1.0f;
    v->ampStep = static_cast<float>(1.0 / (dur * sr_));
    v->vel = 0.3f + 0.7f * velocity;
    const float a = p[KsAttack], a2 = a * a, a4 = a2 * a2;
    v->attackK = static_cast<float>(1.0 / (a4 * a4 * 100000.0 * sr44 + 1.0));
    v->attack = a4 * a4 > 1e-6f ? 0.0f : 1.0f;
    const float cl = p[KsClickLevel], cl2 = cl * cl;
    v->click = cl2 * cl2 * cl;
    v->clickK = static_cast<float>(std::exp(-1.0 / std::max(1.0, 0.0004 * std::pow(2.0, (1.0 - p[KsClickCut]) * 4.0) * sr_)));
    v->clickBp.set(Biquad::BandPass, sr_, 200.0 * std::pow(2.0, p[KsClickFreq] * 6.5), 0.9, 0);
    v->sub.set(Biquad::LowShelf, sr_, 90, 0.7, 12.0 * p[KsSub] * p[KsSub]);
    v->nzPos = static_cast<float>(p[KsNoiseStart] * 0.5 * (noise_ ? static_cast<double>(noise_->frames()) : 0.0));
    v->nzRate = static_cast<float>(std::pow(2.0, 4.0 * p[KsNoisePitch] - 2.0) * (noise_ ? noise_->sampleRate / sr_ : 1.0));
    const double na = std::pow(static_cast<double>(p[KsNoiseA]), 6.0), nd = std::pow(static_cast<double>(p[KsNoiseD]), 6.0);
    v->nzA = static_cast<float>(1.0 / std::max(1.0, na * sr_));
    v->nzD = static_cast<float>(std::exp(-1.0 / std::max(1.0, (nd + 0.001) * sr_)));
    v->nzS = p[KsNoiseS];
    v->nzLp.setFreq(sr_, 100.0 * std::pow(200.0, static_cast<double>(p[KsNoiseFilter] * p[KsNoiseFilter])));
  }
  void onNoteOff(uint32_t) override {}
  void onAllOff() override {}
  void onKill() override {
    for (auto& v : voices_) v.on = false;
  }
  void render(float* L, float* R, int n) override {
    const float* p = p_;
    const float level = 0.5f * std::pow(10.0f, 2.0f * p[KsLevel] - 1.0f);
    const float d = p[KsDrive], d2 = d * d;
    const float drive = 1.0f + 40.0f * d2 * d2;
    const float driveNorm = 1.0f / std::tanh(drive);
    const float mix = 2.0f * p[KsNoiseMix] * p[KsNoiseMix];
    for (auto& v : voices_) {
      if (!v.on) continue;
      for (int i = 0; i < n; ++i) {
        const double f = (v.base + (v.peak - v.base) * v.sweep) * v.pitchMul;
        v.sweep *= v.slideK;
        v.ph += kTwoPi * f / sr_;
        if (v.ph > kTwoPi) v.ph -= kTwoPi;
        const double body = std::sin(v.ph);
        const double cl = v.clickBp.process(v.rng.next()) * v.click * 3.0;
        v.click *= v.clickK;
        double nz = 0;
        if (noise_ && mix > 0) {
          const size_t at = static_cast<size_t>(v.nzPos);
          if (at + 1 < noise_->frames()) {
            // J-140: band-limited read at the noise pitch (was linear).
            nz = pitch::read(noise_->left.data(), static_cast<int64_t>(noise_->frames()), v.nzPos, v.nzRate,
                             pitch::table(pitch::Quality::Realtime));
            v.nzPos += v.nzRate;
          }
          if (v.nzRise) {
            v.nzEnv += v.nzA;
            if (v.nzEnv >= 1) {
              v.nzEnv = 1;
              v.nzRise = false;
            }
          } else {
            v.nzEnv = v.nzS + (v.nzEnv - v.nzS) * v.nzD;
          }
          nz = v.nzLp.process(nz) * v.nzEnv * mix;
        }
        double x = (v.sub.process(body) + cl + nz) * v.amp;
        x = std::tanh(x * drive) * driveNorm;
        if (v.attack < 1.0f) {
          x *= v.attack;
          v.attack += (1.0f - v.attack) * v.attackK + 1e-7f;
        }
        v.amp = std::max(0.0f, v.amp - v.ampStep);
        const float y = static_cast<float>(x) * level * v.vel;
        L[i] += y;
        R[i] += y;
        if (++v.t >= v.len) {
          v.on = false;
          break;
        }
      }
    }
  }

 private:
  std::array<Voice, kVoices> voices_;
  float p_[KsCount] = {};
  const Sample* noise_ = nullptr;
  double sr_ = 44100;
  uint64_t clock_ = 0;
};

// ---------------------------------------------------------------- Sweep Kick
// Engine 1 (SkModel): a 32-bit phase reads a
// 16384-point sine; the click is the start phase (click x 2^24), start level 1.5; the sweep and
// the amp fall per sample by factors of the decay knobs; the start pitch follows the key at half
// rate. Then an optional distortion and the channel envelopes and filter (engine/ChannelIns.h).
// The channel's pitch envelope and LFO do not reach this instrument's oscillator.
class SweepKickGen final : public Generator {
 public:
  SweepKickGen() {
    for (int i = 0; i < 16384; ++i) sine_[static_cast<size_t>(i)] = static_cast<float>(std::sin(kTwoPi * i / 16384.0));
    for (int k = 0; k <= 8191; ++k) shape_[static_cast<size_t>(k)] = static_cast<float>(gl::packs::dynamics::fastDistRef(k / 8191.0, 0, 10));
    shape_[8192] = shape_[8191];
  }
  bool active() const override {
    for (const auto& v : voices_)
      if (v.on) return true;
    for (const auto& v : refs_)
      if (v.on) return true;
    return false;
  }
  bool handlesNotePan() const override { return model_; }

 protected:
  struct Voice {
    bool on = false;
    uint64_t age = 0;
    int64_t t = 0, len = 0;
    double ph = 0, f0 = 100, f1 = 50, fk = 0.999;
    float amp = 1, ampK = 0.999f, click = 0, vel = 1;
    uint32_t id = 0;
    bool held = false;
    InsVoice ins;  // switched on in the envelope page; older states have none
  };
  struct RefVoice {
    bool on = false, held = false, ending = false;
    uint32_t id = 0, phase = 0;
    uint64_t age = 0;
    float step = 0, amp = 1.5f, hostL = 0, hostR = 0;
    int fade = -1;
    InsVoice ins;
  };
  void configure(const PluginState& s, const ChannelAux& aux, const ProcessContext& ctx) override {
    const bool rateChanged = ctx.sampleRate != sr_ || !configured_;
    sr_ = ctx.sampleRate;
    bool changed = rateChanged;
    for (int k = 0; k < SkCount; ++k) {
      const float x = pv(s, k);
      changed = changed || x != p_[k];
      p_[k] = x;
    }
    configured_ = true;
    model_ = p_[SkModel] >= 0.5f;
    ins_ = aux.ins;
    tempo_ = ctx.tempo, ppq_ = ctx.ppq, tick0_ = ctx.tick, blockPos_ = 0;
    if (!changed || !model_) return;
    // The reference's single-precision laws.
    const float rate = 44100.0f / static_cast<float>(sr_);
    stepBase_ = 2.2473466e12f / static_cast<float>(sr_);
    auto fall = [rate](float knob, double top, double range) {
      const float inner = 1.0f - knob * 0.00390625f;
      const double e = static_cast<double>(static_cast<float>(std::exp(static_cast<double>(inner) * 6.90875477931522) - 1.0)) * range;
      const float x = static_cast<float>(top - e);
      if (rate == 1.0f) return x;
      return static_cast<float>(std::exp(std::log(static_cast<double>(x)) * static_cast<double>(rate)));
    };
    sweepK_ = fall(std::round(p_[SkFreqDecay]), 0.9997, 9.700000000000043e-06);
    ampK_ = fall(std::round(p_[SkAmpDecay]), 0.99998, 4.9799999999999845e-06);
    minStep_ = centsToStep(static_cast<float>(std::round(p_[SkMinFreq]) - 3600.0f));
    const float d = std::round(p_[SkDist]) * 0.0078125f;
    dry_ = 1.0f - d, wet_ = d * 3.3f;
    dist_ = std::round(p_[SkDist]) > 0.0f;
  }
  void startVoice(uint32_t id, double pitch, float velocity, float pan, double modX, double modY) {
    RefVoice* v = &refs_[0];
    for (auto& x : refs_)
      if (!x.on) {
        v = &x;
        break;
      } else if (x.age < v->age) {
        v = &x;
      }
    v->on = true, v->held = true, v->ending = false, v->id = id, v->age = ++clock_, v->fade = -1;
    v->phase = static_cast<uint32_t>(std::lround(p_[SkClick])) << 24;
    const float lo = std::round(p_[SkMinFreq]) - 3600.0f;
    float cents = static_cast<float>((pitch - 60.0) * 100.0) * 0.5f + std::round(p_[SkMaxFreq]) + 1200.0f;
    cents = std::min(std::max(cents, lo), 4800.0f);
    v->step = static_cast<float>(centsToStep(cents));
    v->amp = 1.5f;
    // The host's note level and constant-power pan.
    const double vol = (std::pow(11.0, std::nearbyint(static_cast<double>(velocity) * 128.0) / 100.0) - 1.0) / 10.0;
    const float p = std::clamp(pan, -1.0f, 1.0f);
    const double a = (1.0 + static_cast<double>(p)) * 0.7853981633974483;
    v->hostL = static_cast<float>(std::cos(a) * vol), v->hostR = static_cast<float>(std::sin(a) * vol);
    v->ins.start(ins_.get(), sr_, tempo_, ppq_, modX, modY, tick0_ + static_cast<double>(blockPos_) * tempo_ * ppq_ / (60.0 * sr_));
    if (v->ins.active()) v->ins.setHostGains(p, static_cast<float>(vol)), v->ins.setNote(pitch, velocity);
  }
  void onNoteOn(uint32_t id, double pitch, float velocity) override {
    if (model_) return startVoice(id, pitch, velocity, 0.f, 0.0, 0.0);
    Voice* v = &voices_[0];
    for (auto& x : voices_)
      if (!x.on) {
        v = &x;
        break;
      } else if (x.age < v->age) {
        v = &x;
      }
    *v = Voice{};
    v->on = true;
    v->age = ++clock_;
    const double ref = 130.8128 * std::pow(2.0, (pitch - 60.0) / 12.0);
    v->f0 = ref * std::pow(2.0, p_[SkMaxFreq] / 1200.0);
    v->f1 = ref * std::pow(2.0, p_[SkMinFreq] / 1200.0) * 0.5;
    v->fk = std::exp(-1.0 / std::max(1.0, 0.001 * std::pow(2.0, p_[SkFreqDecay] / 256.0 * 8.0) * sr_));
    const double dec = 0.02 * std::pow(2.0, p_[SkAmpDecay] / 256.0 * 7.0);
    v->ampK = static_cast<float>(std::exp(-6.907755 / std::max(1.0, dec * sr_)));
    v->len = static_cast<int64_t>(dec * sr_ * 1.2) + 64;
    v->click = p_[SkClick] / 64.0f;
    v->vel = velocity;
    v->id = id, v->held = true;
    v->ins.start(ins_.get(), sr_, tempo_, ppq_, 0, 0, tick0_ + static_cast<double>(blockPos_) * tempo_ * ppq_ / (60.0 * sr_));
    if (v->ins.active()) v->ins.setNote(pitch, velocity);
  }
  void onNoteOnEx(uint32_t id, double pitch, float velocity, const NoteProps& np) override {
    if (model_) return startVoice(id, pitch, velocity, np.pan, (np.modX - 0.5) * 2.0, (np.modY - 0.5) * 2.0);
    onNoteOn(id, pitch, velocity);
  }
  void endNote(RefVoice& v) {
    v.held = false;
    // The volume envelope releases the voice; without one the note end fades it out at once.
    if (v.ins.active()) v.ins.release();
    if (!(v.ins.active() && ins_ && ins_->volumeEnvelope()) && v.fade < 0) v.fade = 0;
  }
  void onNoteOff(uint32_t id) override {
    for (auto& v : refs_)
      if (v.on && v.held && v.id == id) endNote(v);
    for (auto& v : voices_)
      if (v.on && v.held && v.id == id) v.held = false, v.ins.release();
  }
  void onAllOff() override {
    for (auto& v : refs_)
      if (v.on && v.held) endNote(v);
    for (auto& v : voices_)
      if (v.on && v.held) v.held = false, v.ins.release();
  }
  void onKill() override {
    for (auto& v : voices_) v.on = false;
    for (auto& v : refs_) v.on = false;
  }
  void render(float* L, float* R, int n) override {
    blockPos_ += n;
    const float dist = p_[SkDist] / 128.0f;
    const float drive = 1.0f + 20.0f * dist * dist, norm = 1.0f / std::tanh(drive);
    for (auto& v : voices_) {
      if (!v.on) continue;
      for (int i = 0; i < n; ++i) {
        const double f = v.f1 + (v.f0 - v.f1) * std::pow(v.fk, static_cast<double>(v.t));
        v.ph += f / sr_;
        v.ph -= std::floor(v.ph);
        // Click: the start of the sweep with a squared-off edge.
        double x = std::sin(kTwoPi * v.ph);
        x += v.click * (x >= 0 ? 1.0 : -1.0) * std::exp(-static_cast<double>(v.t) / (0.002 * sr_));
        x *= v.amp;
        v.amp *= v.ampK;
        if (dist > 0) x = std::tanh(x * drive) * norm;
        const float y = static_cast<float>(x) * 0.8f * v.vel;
        float yl = y, yr = y;
        if (v.ins.active()) {
          v.ins.process(yl, yr);
          if (!v.held && v.ins.done()) v.t = v.len;
        }
        L[i] += yl;
        R[i] += yr;
        if (++v.t >= v.len) {
          v.on = false;
          break;
        }
      }
    }
    for (auto& v : refs_) {
      if (!v.on) continue;
      const bool ins = v.ins.active();
      const int fadeLen = static_cast<int>(sr_ * 0.01);
      for (int i = 0; i < n; ++i) {
        float x = sine_[v.phase >> 18] * v.amp;
        v.phase += static_cast<uint32_t>(static_cast<int64_t>(std::nearbyint(v.step)));
        v.amp *= ampK_;
        if (minStep_ < v.step) v.step *= sweepK_;
        else v.step = minStep_;
        if (dist_) {
          const float u = std::min(std::fabs(x * 0.1f) * 8191.0f, 8191.0f);
          const int k = static_cast<int>(u);
          const float y = shape_[static_cast<size_t>(k)] + (shape_[static_cast<size_t>(k) + 1] - shape_[static_cast<size_t>(k)]) * (u - static_cast<float>(k));
          x = (x < 0 ? -y : y) * wet_ + x * dry_;
        }
        float dk = 1.0f;
        if (v.fade >= 0) {
          dk = v.fade >= fadeLen ? 0.0f : 0.5f + 0.5f * std::cos(static_cast<float>(kTwoPi) * 0.5f * static_cast<float>(v.fade) / static_cast<float>(fadeLen));
          if (++v.fade >= fadeLen) v.on = false;
        }
        float l = x, r = x;
        if (ins) {
          v.ins.processWith(l, r, dk);
          if (v.ins.done() && v.fade < 0) v.fade = 0;
        } else {
          l *= dk * v.hostL, r *= dk * v.hostR;
        }
        L[i] += l;
        R[i] += r;
        if (!v.on) break;
      }
      // The reference ends a voice at the end of the block that falls under this level.
      if (v.amp < 0.0002f) v.on = false;
    }
  }

 private:
  float centsToStep(float cents) const {
    return static_cast<float>(static_cast<int32_t>(std::nearbyint(std::exp(static_cast<double>(cents) * 0.00057762265046662109) * static_cast<double>(stepBase_))));
  }
  std::array<Voice, 4> voices_;
  std::array<RefVoice, 8> refs_;
  std::array<float, 16384> sine_{};
  std::array<float, 8193> shape_{};
  std::shared_ptr<const InsSettings> ins_;
  float p_[SkCount] = {};
  bool configured_ = false, model_ = false, dist_ = false;
  float stepBase_ = 0, sweepK_ = 1, ampK_ = 1, minStep_ = 0, dry_ = 1, wet_ = 0;
  double sr_ = 44100, tempo_ = 120, tick0_ = 0;
  int ppq_ = 96;
  int64_t blockPos_ = 0;
  uint64_t clock_ = 0;
};

}  // namespace

std::unique_ptr<Generator> createDrumsGenerator(const std::string& type) {
  if (type == "gl.membranepad") return std::make_unique<MembranePadGen>();
  if (type == "gl.membranekit") return std::make_unique<MembraneKitGen>();
  if (type == "gl.kicksculpt") return std::make_unique<KickSculptGen>();
  if (type == "gl.sweepkick") return std::make_unique<SweepKickGen>();
  return nullptr;
}

bool prepareDrumsAux(const PluginState& state, ChannelAux& aux) {
  if (state.type == "gl.membranekit") {
    aux = ChannelAux{};
    auto k = std::make_shared<KitAux>();
    decodeKitData(state.data, k->d);
    aux.custom = k;
    return true;
  }
  if (state.type == "gl.kicksculpt") {
    aux = ChannelAux{};
    for (int k = 0; k < kickNoiseCount(); ++k) aux.samples.push_back(kickNoiseSample(k));
    return true;
  }
  if (state.type == "gl.membranepad" || state.type == "gl.sweepkick") {
    aux = ChannelAux{};
    return true;
  }
  return false;
}

}  // namespace gl::packs::models
