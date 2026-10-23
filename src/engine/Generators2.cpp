// Gridline — Wave 2 instruments: Drum Pads, FM Synth, Acid Bass, Keys,
// Slicer, Granular, MIDI Out. Audio-thread only; no allocation after creation.
#include "TimeStretch.h"
#include <algorithm>
#include <array>
#include <cmath>
#include "SlicexEngine.h"
#include "../core/PluginParams.h"
#include "Dsp.h"
#include "Generators.h"
#include "Slicing.h"

namespace gl {

using namespace dsp;

namespace {

float param(const PluginState& s, int i, float dflt = 0.f) {
  return i >= 0 && i < static_cast<int>(s.params.size()) ? s.params[static_cast<size_t>(i)] : dflt;
}

inline float interp(const std::vector<float>& v, double pos) {
  size_t i0 = static_cast<size_t>(pos);
  if (i0 + 1 >= v.size()) return i0 < v.size() ? v[i0] : 0.f;
  float fr = static_cast<float>(pos - static_cast<double>(i0));
  return v[i0] + (v[i0 + 1] - v[i0]) * fr;
}

// J-87: band-limited read (windowed sinc, cutoff following the read rate) for
// pitched sample playback: no aliasing when pitched up, no dulling between
// samples. Exact on whole frames.
inline void interpBl(const Sample& s, double pos, double rate, float& l, float& r) {
  const double fl = std::floor(pos);
  if (std::fabs(rate) <= 1.000001 && pos - fl < 1e-9 && pos >= 0 && fl < static_cast<double>(s.frames())) {  // exact unless reading faster
    const auto i = static_cast<size_t>(fl);
    l = s.left[i];
    r = s.right[i];
    return;
  }
  stretch::readSinc2(s.left.data(), s.right.data(), static_cast<int64_t>(s.frames()), pos, std::fabs(rate),
                     stretch::sincTable(stretch::Quality::Realtime), l, r);
}

// ============================================================ Drum Pads
// 32 pads (banks A and B), each with any number of velocity layers (stacked or
// round-robin / random), a MIDI note (or base note + index), cut / cut-by
// groups, mute / solo, an AHDSR envelope (one-shot or note-off release) or a
// multi-point volume envelope, a pan envelope and a play region.
class PadsGen final : public Generator {
 public:
  bool active() const override {
    for (const auto& v : voices_)
      if (v.on) return true;
    return false;
  }

 protected:
  // Multi-point envelope runner (see PadEnvelope).
  struct EnvRun {
    int seg = 1;          // segment being run: from point seg-1 to seg
    double t = 0;         // seconds into the segment
    double value = 0;
    bool done = false;
    void start(const PadEnvelope& e) {
      seg = 1, t = 0, done = e.n <= 1;
      value = e.n > 0 ? e.v[0] : 1.0;
    }
    // Advances by dt seconds; `held` keeps the sustain point / loop.
    void step(const PadEnvelope& e, double dt, bool held) {
      if (done || e.n <= 1) return;
      if (held && e.sustain >= 0 && seg - 1 == e.sustain) {  // holding at the sustain point
        value = e.v[e.sustain];
        return;
      }
      t += dt;
      while (!done) {
        double len = e.t[seg];
        if (t < len) break;
        t -= len;
        ++seg;
        if (held && e.loopEnd >= 0 && seg - 1 == e.loopEnd) seg = e.loopStart + 1;
        if (seg >= e.n) {
          done = true;
          value = e.v[e.n - 1];
          return;
        }
        if (held && e.sustain >= 0 && seg - 1 == e.sustain) {
          value = e.v[e.sustain];
          t = 0;
          return;
        }
      }
      double len = e.t[seg];
      double x = len > 0 ? t / len : 1.0;
      value = e.v[seg - 1] + (e.v[seg] - e.v[seg - 1]) * padEnvCurve(e.ten[seg], x);
    }
  };
  struct Voice {
    bool on = false, held = false;
    uint32_t id = 0;
    int pad = 0, group = 0;
    double pos = 0, rate = 1, endPos = 0, startPos = 0;
    float gl = 0, gr = 0, fade = 1, fadeStep = 0;
    float level = 0, pan = 0;
    bool reverse = false;
    const Sample* smp = nullptr;  // owned by the sample cache (never freed while running)
    uint64_t age = 0;
    // AHDSR envelope
    int stage = 0;  // 0 attack, 1 hold, 2 decay/sustain, 3 release
    double env = 0, holdLeft = 0;
    // multi-point envelopes (when the pad has them)
    bool useVolEnv = false, usePanEnv = false;
    EnvRun volEnv, panEnv;
    int ctr = 0;
  };
  void configure(const PluginState& s, const ChannelAux& aux, const ProcessContext& ctx) override {
    using namespace pp;
    state_ = &s;
    aux_ = &aux;
    view_ = padsAuxView(padsAuxOf(aux));
    sr_ = ctx.sampleRate;
    anySolo_ = false;
    for (int p = 0; p < kPadsAll; ++p)
      if (param(s, padE(p, PadSolo)) >= 0.5f) anySolo_ = true;
  }
  int padNote(int p) const {
    using namespace pp;
    int n = static_cast<int>(std::lround(param(*state_, padE(p, PadNote), -1)));
    return n >= 0 ? n : static_cast<int>(std::lround(param(*state_, PadsBaseNote, 60))) + p;
  }
  void onNoteOn(uint32_t id, double pitch, float vel) override {
    int note = static_cast<int>(std::lround(pitch));
    for (int p = 0; p < pp::kPadsAll; ++p)
      if (padNote(p) == note) triggerPad(id, p, vel);
  }
  struct LayerSel {
    int layer;
    float level, pan, tune;
    bool reverse;
    double start, end;
  };
  void triggerPad(uint32_t id, int pad, float vel) {
    using namespace pp;
    const PluginState& s = *state_;
    if (param(s, padE(pad, PadMute)) >= 0.5f) return;
    if (anySolo_ && param(s, padE(pad, PadSolo)) < 0.5f) return;
    // Cut groups: this pad chokes its cut-by group (legacy: its own group).
    int group = static_cast<int>(param(s, padP(pad, PadCut)) + 0.5f);
    int cutBy = static_cast<int>(param(s, padE(pad, PadCutBy)) + 0.5f);
    int choke = cutBy > 0 ? cutBy : group;
    if (choke > 0)
      for (auto& v : voices_)
        if (v.on && v.group == choke) v.fadeStep = static_cast<float>(1.0 / std::max(1.0, 0.005 * sr_));
    // Layers whose velocity range holds this hit (parameter layers 1..4, data layers 5..).
    int vel127 = static_cast<int>(std::lround(vel * 127));
    const int nLayers = view_.layers ? std::clamp(view_.layers[pad], kPadLayers, kPadMaxLayers) : kPadLayers;
    LayerSel cand[kPadMaxLayers];
    int nc = 0;
    for (int l = 0; l < nLayers; ++l) {
      LayerSel c{l, 1, 0, 0, false, 0, 1};
      int lo, hi;
      if (l < kPadLayers) {
        if (param(s, padL(pad, l, LayEnabled), l == 0 ? 1.f : 0.f) < 0.5f) continue;
        lo = static_cast<int>(param(s, padL(pad, l, LayVelLow))), hi = static_cast<int>(param(s, padL(pad, l, LayVelHigh), 127));
        c.level = param(s, padL(pad, l, LayLevel), 1), c.pan = param(s, padL(pad, l, LayPan)), c.tune = param(s, padL(pad, l, LayTune));
        c.reverse = param(s, padL(pad, l, LayReverse)) >= 0.5f;
        c.start = param(s, padL(pad, l, LayStart)), c.end = param(s, padL(pad, l, LayEnd), 1);
      } else {
        if (!view_.extra) continue;
        const PadLayerX& x = view_.extra[pad * kPadMaxLayers + l];
        lo = x.lo, hi = x.hi;
        c.level = x.level, c.pan = x.pan, c.tune = x.tune, c.reverse = x.reverse;
      }
      if (vel127 < lo || vel127 > hi) continue;
      const Sample* smp = aux_->sample(static_cast<size_t>(padLayerSampleX(pad, l))).get();
      if (!smp || smp->frames() < 2) continue;
      cand[nc++] = c;
    }
    if (nc == 0) return;
    int mode = static_cast<int>(param(s, padE(pad, PadLayerMode)) + 0.5f);
    int first = 0, count = nc;
    if (mode != LayerStack && nc > 1) {
      int& last = lastLayer_[pad];
      int pick;
      switch (mode) {
        case LayerCycle: pick = (last + 1) % nc; break;
        case LayerCycleBack: pick = (last - 1 + nc) % nc; break;
        case LayerRandomNoRepeat:
          pick = static_cast<int>((rng_.next() * 0.5 + 0.5) * (nc - 1)) % std::max(1, nc - 1);
          if (pick >= last) ++pick;
          pick = std::min(pick, nc - 1);
          break;
        default: pick = static_cast<int>((rng_.next() * 0.5 + 0.5) * nc) % nc; break;
      }
      last = pick;
      first = pick, count = 1;
    }
    bool velScale = param(s, padE(pad, PadVelScale), 1) >= 0.5f;
    float velAmt = velScale ? param(s, PadsVelocity, 0.8f) : 0.f;
    const PadEnvelope* ve = view_.vol && view_.vol[pad].on && view_.vol[pad].n > 0 ? &view_.vol[pad] : nullptr;
    const PadEnvelope* pe = view_.pan && view_.pan[pad].on && view_.pan[pad].n > 0 ? &view_.pan[pad] : nullptr;
    for (int c = first; c < first + count; ++c) {
      const LayerSel& L = cand[c];
      const Sample* smp = aux_->sample(static_cast<size_t>(padLayerSampleX(pad, L.layer))).get();
      Voice* pick = nullptr;
      for (auto& v : voices_)
        if (!v.on) {
          pick = &v;
          break;
        }
      if (!pick)
        for (auto& v : voices_)
          if (!pick || v.age < pick->age) pick = &v;
      Voice& v = *pick;
      v = Voice{};
      v.on = v.held = true;
      v.id = id;
      v.pad = pad;
      v.group = group;
      v.smp = smp;
      v.age = ++clock_;
      double len = static_cast<double>(smp->frames()) - 2;
      double st = std::clamp(static_cast<double>(param(s, padE(pad, PadStart))) + L.start, 0.0, 1.0);
      double en = std::clamp(L.end, st + 0.001, 1.0);
      v.startPos = st * len, v.endPos = en * len;
      v.reverse = (param(s, padP(pad, PadReverse)) >= 0.5f) != L.reverse;
      v.rate = std::pow(2.0, (param(s, padP(pad, PadTune)) + L.tune) / 12.0) * smp->sampleRate / sr_;
      v.pos = v.reverse ? v.endPos : v.startPos;
      v.level = param(s, padP(pad, PadLevel), 0.8f) * L.level * (1 - velAmt + velAmt * vel) * param(s, PadsVolume, 0.8f);
      v.pan = static_cast<float>(std::clamp(static_cast<double>(param(s, padP(pad, PadPan))) + L.pan, -1.0, 1.0));
      setGains(v, 0.0);
      double atk = param(s, padE(pad, PadAttack));
      v.stage = atk > 0.0005 ? 0 : 1;
      v.env = atk > 0.0005 ? 0.0 : 1.0;
      v.holdLeft = param(s, padE(pad, PadHold), 10) * sr_;
      if (ve) v.useVolEnv = true, v.volEnv.start(*ve);
      if (pe) v.usePanEnv = true, v.panEnv.start(*pe);
    }
  }
  static void setGains(Voice& v, double panMod) {
    double pl, pr;
    panGains(std::clamp(static_cast<double>(v.pan) + panMod, -1.0, 1.0), pl, pr);
    v.gl = static_cast<float>(v.level * pl);
    v.gr = static_cast<float>(v.level * pr);
  }
  void onNoteOff(uint32_t id) override {
    for (auto& v : voices_)
      if (v.on && v.held && v.id == id) {
        v.held = false;
        if (param(*state_, pp::padE(v.pad, pp::PadOneShot), 1) < 0.5f) v.stage = 3;
      }
  }
  void onAllOff() override {
    for (auto& v : voices_) v.held = false;
  }
  void onKill() override {
    for (auto& v : voices_) v = Voice{};
  }
  void render(float* L, float* R, int n) override {
    using namespace pp;
    const double dt = 1.0 / sr_;
    for (auto& v : voices_) {
      if (!v.on) continue;
      const Sample& sm = *v.smp;
      const PluginState& s = *state_;
      double atkInc = 1.0 / std::max(1.0, param(s, padE(v.pad, PadAttack)) * sr_);
      double decC = std::exp(-1.0 / std::max(1.0, param(s, padE(v.pad, PadDecay), 10) * sr_ * 0.25));
      double sus = param(s, padE(v.pad, PadSustain), 1);
      double relC = std::exp(-1.0 / std::max(1.0, param(s, padE(v.pad, PadRelease), 0.05f) * sr_ * 0.25));
      const PadEnvelope* ve = v.useVolEnv && view_.vol ? &view_.vol[v.pad] : nullptr;
      const PadEnvelope* pe = v.usePanEnv && view_.pan ? &view_.pan[v.pad] : nullptr;
      for (int i = 0; i < n; ++i) {
        if (v.pos < v.startPos - 1 || v.pos > v.endPos || v.fade <= 0) {
          v.on = false;
          break;
        }
        double a;
        if (ve) {  // multi-point volume envelope (the note-off releases it unless one-shot)
          v.volEnv.step(*ve, dt, v.held);
          a = v.volEnv.value;
          if (v.volEnv.done && a < 1e-4) v.fade = 0;
        } else {
          switch (v.stage) {
            case 0:
              v.env += atkInc;
              if (v.env >= 1) v.env = 1, v.stage = 1;
              break;
            case 1:
              if (--v.holdLeft <= 0) v.stage = 2;
              break;
            case 2: v.env = sus + (v.env - sus) * decC; break;
            default:
              v.env *= relC;
              if (v.env < 1e-4) v.fade = 0;
              break;
          }
          a = v.env;
        }
        if (pe && (v.ctr++ & 15) == 0) {
          v.panEnv.step(*pe, dt * 16, v.held);
          setGains(v, (v.panEnv.value - 0.5) * 2.0);
        }
        float g = v.fade * static_cast<float>(a);
        float sl, sr;
        interpBl(sm, v.pos, v.rate, sl, sr);
        L[i] += sl * v.gl * g;
        R[i] += sr * v.gr * g;
        v.pos += v.reverse ? -v.rate : v.rate;
        v.fade -= v.fadeStep;
      }
    }
  }

 private:
  std::array<Voice, 64> voices_{};
  const PluginState* state_ = nullptr;
  const ChannelAux* aux_ = nullptr;
  PadsAuxView view_;
  double sr_ = 44100;
  uint64_t clock_ = 0;
  bool anySolo_ = false;
  int lastLayer_[pp::kPadsAll] = {};
  Noise rng_;
};

// ============================================================ FM Synth
// Modulators of each operator per algorithm (bit k set = op k modulates), and carriers.
struct FmAlgo {
  uint8_t mods[4];
  uint8_t carriers;
};
const FmAlgo kAlgos[8] = {
    {{0b0010, 0b0100, 0b1000, 0}, 0b0001},  // 4→3→2→1
    {{0b0010, 0b1100, 0, 0}, 0b0001},       // (3+4)→2→1
    {{0b1010, 0b0100, 0, 0}, 0b0001},       // 3→2→1 + 4→1
    {{0b0110, 0, 0b1000, 0}, 0b0001},       // 4→3→1 + 2→1
    {{0b0010, 0, 0b1000, 0}, 0b0101},       // 4→3, 2→1
    {{0b1000, 0b1000, 0b1000, 0}, 0b0111},  // 4→(1,2,3)
    {{0, 0, 0b1000, 0}, 0b0111},            // 4→3 + 2 + 1
    {{0, 0, 0, 0}, 0b1111},                 // all carriers
};

class FmGen final : public Generator {
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
    double note = 60, target = 60;
    float vel = 0;
    uint64_t age = 0;
    double ph[4] = {0, 0, 0, 0};
    double last[4] = {0, 0, 0, 0};
    Adsr env[4];
    double lfoPh = 0;
    float gain[4] = {0, 0, 0, 0};  // operator gains of the current control period
    uint32_t ctl = 0;              // samples rendered (control grid)
  };
  void configure(const PluginState& s, const ChannelAux&, const ProcessContext& ctx) override {
    using namespace pp;
    sr_ = ctx.sampleRate;
    for (int o = 0; o < kFmOps; ++o) {
      ratio_[o] = param(s, fmParam(o, FmRatio), 1) * std::pow(2.0, param(s, fmParam(o, FmFine)) / 1200.0);
      level_[o] = param(s, fmParam(o, FmLevel));
      velSens_[o] = param(s, fmParam(o, FmVel), 0.5f);
      wave_[o] = param(s, fmExt(o, FmWave));
      fixed_[o] = param(s, fmExt(o, FmFixed)) >= 0.5f;
      offset_[o] = param(s, fmExt(o, FmOffset));
      thru_[o] = param(s, fmExt(o, FmThru));
      panGains(param(s, fmExt(o, FmPan)), panL_[o], panR_[o]);
      startPhase_[o] = param(s, fmExt(o, FmPhase));
      keyScale_[o] = param(s, fmExt(o, FmKeyScale));
      init_[o] = param(s, fmExt(o, FmInit));
      for (auto& v : voices_)
        v.env[o].set(sr_, param(s, fmParam(o, FmAttack), 0.002f), param(s, fmParam(o, FmDecay), 1),
                     param(s, fmParam(o, FmSustain)), param(s, fmParam(o, FmRelease), 0.4f));
    }
    algo_ = std::clamp(static_cast<int>(param(s, FmAlgorithm) + 0.5f), 0, 7);
    fb_ = param(s, FmFeedback);
    voices_n_ = std::clamp(static_cast<int>(param(s, FmVoices, 8) + 0.5f), 1, 16);
    vol_ = param(s, FmVolume, 0.6f);
    octave_ = std::round(param(s, FmOctave));
    lfoRate_ = param(s, FmLfoRate, 5), vibrato_ = param(s, FmVibrato), lfoIndex_ = param(s, FmLfoIndex);
    glide_ = param(s, FmGlide, 0.001f), pitchEnv_ = param(s, FmPitchEnv);
  }
  void onNoteOn(uint32_t id, double pitch, float vel) override {
    Voice* pick = nullptr;
    if (voices_n_ == 1 && voices_[0].on && voices_[0].held && glide_ > 0.002) {  // mono legato glide
      voices_[0].target = pitch;
      voices_[0].id = id;
      return;
    }
    for (int i = 0; i < voices_n_; ++i)
      if (!voices_[static_cast<size_t>(i)].on) {
        pick = &voices_[static_cast<size_t>(i)];
        break;
      }
    if (!pick)
      for (int i = 0; i < voices_n_; ++i)
        if (!pick || voices_[static_cast<size_t>(i)].age < pick->age) pick = &voices_[static_cast<size_t>(i)];
    Voice& v = *pick;
    if (!v.on)
      for (int o = 0; o < 4; ++o) v.ph[o] = startPhase_[o], v.last[o] = 0;
    v.on = v.held = true;
    v.id = id;
    v.note = v.target = pitch;
    v.vel = vel;
    v.age = ++clock_;
    v.lfoPh = 0;
    v.ctl = 0;  // operator gains for the new note at its first sample
    for (int o = 0; o < 4; ++o) {
      if (init_[o] > 0) v.env[o].level = init_[o];
      v.env[o].gate(true);
    }
  }
  void onNoteOff(uint32_t id) override {
    for (auto& v : voices_)
      if (v.on && v.held && v.id == id) {
        v.held = false;
        for (auto& e : v.env) e.gate(false);
      }
  }
  void onAllOff() override {
    for (auto& v : voices_) {
      v.held = false;
      for (auto& e : v.env) e.gate(false);
    }
  }
  void onKill() override {
    for (auto& v : voices_) v = Voice{};
  }
  // Operator waveform: sine bent toward a brighter shape by phase distortion.
  static double opWave(double ph, double w) {
    if (w <= 0) return std::sin(kTwoPi * ph);
    double knee = 0.5 - 0.45 * w;  // compress the first half of the cycle
    double p = ph < knee ? ph / knee * 0.5 : 0.5 + (ph - knee) / (1 - knee) * 0.5;
    return std::sin(kTwoPi * p);
  }
  void render(float* L, float* R, int n) override {
    const FmAlgo& al = kAlgos[algo_];
    const double glideCoef = std::exp(-1.0 / std::max(1.0, glide_ * sr_ * 0.3));
    for (auto& v : voices_) {
      if (!v.on) continue;
      float* gain = v.gain;
      for (int i = 0; i < n; ++i) {
        // Operator gains (key scaling follows the gliding pitch) every 16
        // samples of the voice's own clock (J-30: once per host block made the
        // sound depend on the block size).
        if ((v.ctl++ & 15u) == 0)
          for (int o = 0; o < 4; ++o) {
            double ks = keyScale_[o] != 0 ? dbToGain(keyScale_[o] * 6.0 * (v.note - 60.0) / 12.0) : 1.0;
            gain[o] = static_cast<float>(level_[o] * (1 - velSens_[o] + velSens_[o] * v.vel) * ks);
          }
        double lfo = 0;
        if (vibrato_ > 0 || lfoIndex_ > 0) {
          lfo = std::sin(kTwoPi * v.lfoPh);
          v.lfoPh += lfoRate_ / sr_;
          v.lfoPh -= std::floor(v.lfoPh);
        }
        double semis = v.note + octave_ * 12.0 + lfo * vibrato_ / 100.0 + pitchEnv_ * v.env[0].level;
        double hz = noteToHz(semis);
        double out[4] = {};
        double modScale = 1.0 - lfoIndex_ * (0.5 + 0.5 * lfo);
        for (int o = 3; o >= 0; --o) {  // modulators always have higher indices
          double mod = 0;
          for (int m = 0; m < 4; ++m)
            if (al.mods[o] & (1 << m)) mod += out[m] * modScale;
          if (o == 3) mod += fb_ * v.last[3] * 1.5;
          double e = v.env[o].next();
          double pph = v.ph[o] + mod * 4.0 / kTwoPi;
          pph -= std::floor(pph);
          out[o] = opWave(pph, wave_[o]) * e * gain[o];
          v.last[o] = out[o];
          double f = (fixed_[o] ? ratio_[o] * 100.0 : hz * ratio_[o]) + offset_[o];
          v.ph[o] += f / sr_;
          v.ph[o] -= std::floor(v.ph[o]);
        }
        double sl = 0, sr = 0;
        int nc = 0;
        for (int o = 0; o < 4; ++o) {
          if (al.carriers & (1 << o)) {
            sl += out[o] * panL_[o], sr += out[o] * panR_[o], ++nc;
          } else if (thru_[o] > 0) {
            sl += out[o] * thru_[o] * panL_[o], sr += out[o] * thru_[o] * panR_[o];
          }
        }
        double k = vol_ / std::max(1, nc);
        L[i] += static_cast<float>(sl * k);
        R[i] += static_cast<float>(sr * k);
        v.note = v.target + (v.note - v.target) * glideCoef;
        // Ends when its carriers' envelopes have, on its own clock (J-30: not at a block end).
        bool any = false;
        for (int o = 0; o < 4; ++o)
          if ((al.carriers & (1 << o)) && v.env[o].active()) any = true;
        if (!any) {
          v.on = false;
          break;
        }
      }
    }
  }

 private:
  std::array<Voice, 16> voices_{};
  double sr_ = 44100, ratio_[4] = {1, 1, 1, 1}, fb_ = 0;
  float level_[4] = {1, 0, 0, 0}, velSens_[4] = {0, 0, 0, 0}, vol_ = 0.6f;
  double wave_[4] = {}, offset_[4] = {}, thru_[4] = {}, panL_[4] = {1, 1, 1, 1}, panR_[4] = {1, 1, 1, 1}, startPhase_[4] = {}, keyScale_[4] = {},
         init_[4] = {};
  bool fixed_[4] = {};
  double octave_ = 0, lfoRate_ = 5, vibrato_ = 0, lfoIndex_ = 0, glide_ = 0.001, pitchEnv_ = 0;
  int algo_ = 0, voices_n_ = 8;
  uint64_t clock_ = 0;
};

// ============================================================ Acid Bass
class AcidGen final : public Generator {
 public:
  bool active() const override { return amp_ > 1e-5 || gate_ || (seqOn_ && playing_); }

 protected:
  void configure(const PluginState& s, const ChannelAux&, const ProcessContext& ctx) override {
    using namespace pp;
    sr_ = ctx.sampleRate;
    square_ = param(s, AcWave) >= 0.5f;
    cutoff_ = param(s, AcCutoff, 500);
    reso_ = param(s, AcReso, 0.7f);
    envMod_ = param(s, AcEnvMod, 0.55f);
    decay_ = param(s, AcDecay, 0.25f);
    accent_ = param(s, AcAccent, 0.6f);
    slide_ = param(s, AcSlide, 0.06f);
    drive_ = param(s, AcDrive, 0.2f);
    vol_ = param(s, AcVolume, 0.7f);
    tuning_ = param(s, AcTuning), blend_ = param(s, AcBlend), vintage_ = param(s, AcVintage) >= 0.5f;
    pw_ = param(s, AcPulseWidth, 0.5f), pwmRate_ = param(s, AcPwmRate, 1), pwmDepth_ = param(s, AcPwmDepth);
    hp_ = param(s, AcHighPass, 10), accDecay_ = param(s, AcAccentDecay, 0.25f), keyFollow_ = param(s, AcKeyFollow);
    clicks_ = param(s, AcClicks), driveTone_ = param(s, AcDriveTone, 20000), accThresh_ = param(s, AcAccentThresh, 0.9f);
    hpF_.setFreq(sr_, hp_);
    toneF_.setFreq(sr_, driveTone_);
    sqDc_.setFreq(sr_, 20);
    seqOn_ = param(s, AcSeqOn) >= 0.5f;
    seqLen_ = std::clamp(static_cast<int>(param(s, AcSeqLength, 16) + 0.5f), 1, kAcSteps);
    shuffle_ = param(s, AcSeqShuffle), seqGate_ = param(s, AcSeqGate, 0.5f);
    for (int k = 0; k < kAcSteps; ++k) {
      stepNote_[k] = static_cast<int>(param(s, acStep(k, AcStepNote)) + 0.5f);
      stepOct_[k] = static_cast<int>(param(s, acStep(k, AcStepOctave), 1) + 0.5f) - 1;
      stepAcc_[k] = param(s, acStep(k, AcStepAccent)) >= 0.5f;
      stepSlide_[k] = param(s, acStep(k, AcStepSlide)) >= 0.5f;
      stepGate_[k] = static_cast<int>(param(s, acStep(k, AcStepGate)) + 0.5f);
    }
    playing_ = ctx.playing;
    tick0_ = ctx.tick / std::max(1, ctx.ppq);
    spb_ = 60.0 / std::max(1.0, ctx.tempo) * sr_;
    blockPos_ = 0;
    if (!playing_) lastStep_ = -1;
  }
  void trigger(double pitch, bool accent, bool legato) {
    target_ = pitch;
    if (!legato) {
      pitch_ = pitch;
      fenv_ = 1;
      acc_ = accent ? 1.0f : 0.0f;
      click_ = clicks_;
    }
    gate_ = true;
  }
  void onNoteOn(uint32_t id, double pitch, float vel) override {
    held_[heldN_ < 16 ? heldN_++ : 15] = {id, pitch};
    if (seqOn_) {  // keys transpose the sequence
      transpose_ = pitch - 60;
      return;
    }
    trigger(pitch, vel >= accThresh_, gate_);  // overlapping notes slide instead of retriggering
    id_ = id;
  }
  void onNoteOff(uint32_t id) override {
    for (int i = 0; i < heldN_; ++i)
      if (held_[i].id == id) {
        held_[i] = held_[--heldN_];
        break;
      }
    if (seqOn_) {
      if (heldN_ > 0) transpose_ = held_[heldN_ - 1].pitch - 60;
      return;
    }
    if (heldN_ > 0) {  // fall back to the most recent held note (slide)
      target_ = held_[heldN_ - 1].pitch;
      id_ = held_[heldN_ - 1].id;
    } else {
      gate_ = false;
    }
  }
  void onAllOff() override {
    heldN_ = 0;
    gate_ = false;
  }
  void onKill() override {
    heldN_ = 0;
    gate_ = false;
    amp_ = 0;
    f1_.reset();
    f2_.reset();
    lastStep_ = -1;
  }
  // Internal 16-step sequencer, locked to the song position.
  void runSequencer(int i) {
    double beat = tick0_ + (blockPos_ + i) / spb_;
    double stepF = beat * 4.0;
    long k = static_cast<long>(std::floor(stepF));
    double local = stepF - static_cast<double>(k);
    if (k % 2 != 0) {  // shuffle delays the off-beat 16ths
      double delay = shuffle_ * 0.5;
      if (local < delay) {
        --k;
        local += 1.0;
      } else {
        local -= delay;
      }
    }
    int step = static_cast<int>(((k % seqLen_) + seqLen_) % seqLen_);
    if (k != lastStep_) {
      int prev = lastStep_ >= 0 ? static_cast<int>(((lastStep_ % seqLen_) + seqLen_) % seqLen_) : -1;
      lastStep_ = k;
      bool prevSlide = prev >= 0 && stepSlide_[prev] && stepGate_[prev] != 0;
      if (stepGate_[step] == 1) {
        double pitch = 48 + stepNote_[step] + stepOct_[step] * 12 + transpose_;
        trigger(pitch, stepAcc_[step], prevSlide && gate_);
      } else if (stepGate_[step] == 0 && !prevSlide) {
        gate_ = false;
      }
    }
    // Gate length: release inside the step unless it slides or the next step ties.
    int next = (step + 1) % seqLen_;
    if (stepGate_[step] != 0 && !stepSlide_[step] && stepGate_[next] != 2 && local > seqGate_) gate_ = false;
  }
  void render(float* L, float* R, int n) override {
    const double slideCoef = std::exp(-1.0 / std::max(1.0, slide_ * sr_ * 0.3));
    const double fdec = std::exp(-1.0 / std::max(1.0, decay_ * sr_ * 0.3));
    const double fdecAcc = std::exp(-1.0 / std::max(1.0, accDecay_ * sr_ * 0.3));
    const double atk = 1.0 / (0.003 * sr_), rel = std::exp(-1.0 / (0.008 * sr_));
    const double clickDec = std::exp(-1.0 / (0.002 * sr_));
    for (int i = 0; i < n; ++i) {
      if (seqOn_ && playing_) runSequencer(i);
      pitch_ = target_ + (pitch_ - target_) * slideCoef;
      double dt = noteToHz(pitch_ + tuning_) / sr_;
      double x;
      if (blend_ > 0.0005 || pwmDepth_ > 0 || std::fabs(pw_ - 0.5) > 0.001 || vintage_) {
        double saw = osc_.next(Shape::Saw, dt, noise_);
        double sq;
        if (vintage_) {  // square shaped from the saw by an asymmetric transistor-like stage
          sq = std::tanh((saw + 0.25) * 6.0);
          sq = sq - sqDc_.process(sq);
        } else {
          double width = std::clamp(pw_ + pwmDepth_ * std::sin(kTwoPi * pwmPh_), 0.05, 0.95);
          pwmPh_ += pwmRate_ / sr_;
          pwmPh_ -= std::floor(pwmPh_);
          double ph = osc_.phase;
          sq = ph < width ? 1.0 : -1.0;
          sq += polyBlep(ph, dt);
          sq -= polyBlep(std::fmod(ph + 1.0 - width, 1.0), dt);
        }
        double b = blend_ > 0.0005 ? blend_ : (square_ ? 1.0 : 0.0);
        x = saw * (1 - b) + sq * b;
      } else {
        x = osc_.next(square_ ? Shape::Square : Shape::Saw, dt, noise_);
      }
      if (hp_ > 10.5) x -= hpF_.process(x);
      if (click_ > 1e-4) {
        x += click_ * noise_.next();
        click_ *= clickDec;
      }
      if ((ctr_++ & 7) == 0) {
        double env = fenv_ * (1 + acc_ * accent_);
        double kf = keyFollow_ > 0 ? std::pow(2.0, keyFollow_ * (pitch_ - 48.0) / 12.0) : 1.0;
        double fc = std::clamp(cutoff_ * kf * std::pow(2.0, envMod_ * env * 4.5), 30.0, sr_ * 0.45);
        double rs = std::min(0.98, reso_ * (1 + acc_ * accent_ * 0.3));
        f1_.set(sr_, fc, rs * 0.6);
        f2_.set(sr_, fc, rs);
      }
      fenv_ *= acc_ > 0 ? fdecAcc : fdec;
      double y = f2_.process(f1_.process(x));
      amp_ = gate_ ? std::min(1.0, amp_ + atk) : amp_ * rel;
      double d = 1 + drive_ * 6;
      y = std::tanh(y * d) / std::tanh(d);
      if (driveTone_ < 19900) y = toneF_.process(y);
      y *= amp_ * (0.8 + acc_ * accent_ * 0.5) * vol_;
      L[i] += static_cast<float>(y);
      R[i] += static_cast<float>(y);
    }
    blockPos_ += n;
  }

 private:
  struct Held {
    uint32_t id;
    double pitch;
  };
  Held held_[16] = {};
  int heldN_ = 0;
  bool gate_ = false, square_ = false, vintage_ = false, seqOn_ = false, playing_ = false;
  uint32_t id_ = 0;
  double sr_ = 44100, pitch_ = 48, target_ = 48, fenv_ = 0, acc_ = 0, amp_ = 0, click_ = 0;
  double cutoff_ = 500, reso_ = 0.7, envMod_ = 0.5, decay_ = 0.25, accent_ = 0.6, slide_ = 0.06, drive_ = 0.2, vol_ = 0.7;
  double tuning_ = 0, blend_ = 0, pw_ = 0.5, pwmRate_ = 1, pwmDepth_ = 0, pwmPh_ = 0, hp_ = 10, accDecay_ = 0.25, keyFollow_ = 0;
  double clicks_ = 0, driveTone_ = 20000, accThresh_ = 0.9, shuffle_ = 0, seqGate_ = 0.5, transpose_ = 0;
  double tick0_ = 0, spb_ = 1;
  int64_t blockPos_ = 0;
  long lastStep_ = -1;
  int seqLen_ = 16;
  int stepNote_[pp::kAcSteps] = {}, stepOct_[pp::kAcSteps] = {}, stepGate_[pp::kAcSteps] = {};
  bool stepAcc_[pp::kAcSteps] = {}, stepSlide_[pp::kAcSteps] = {};
  Osc osc_;
  Noise noise_;
  Svf f1_, f2_;
  OnePole hpF_, toneF_, sqDc_;
  uint32_t ctr_ = 0;
};

// ============================================================ Keys
class KeysGen final : public Generator {
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
    double hz = 440, ph[2] = {0, 0}, mph = 0, bph = 0, amp = 0, modEnv = 0, note = 60;
    float vel = 0;
    uint64_t age = 0;
    OnePole lp[2];
    double panL = 1, panR = 1;
    // Piano models: hammered-string partials as rotating phasors per side
    // (two slightly detuned strings), each with its own decay.
    static constexpr int kPartials = 14;
    double re[2][kPartials] = {}, im[2][kPartials] = {}, cr[2][kPartials] = {}, ci[2][kPartials] = {};
    double pa[kPartials] = {}, pdec[kPartials] = {}, prel = 1;
    int parts = 0;
    double knock = 0, knockDec = 0, knockLp = 0;
    uint32_t seed = 1;
    uint32_t ticks = 0;  // samples rendered (piano renormalisation grid)
  };
  static bool pianoModel(int m) { return m >= 4; }
  // Starts the partials of a piano voice (model 4 grand, 5 upright).
  void startPiano(Voice& v) const {
    const bool upright = model_ == 5;
    const double f0 = v.hz;
    const double hard = std::clamp(hard_ + velHard_ * (v.vel - 0.5) + (upright ? 0.15 : 0.0), 0.0, 1.0);
    // Inharmonicity grows towards the treble; brighter hammers excite higher partials.
    const double B = (upright ? 0.0004 : 0.00025) * std::pow(2.0, (v.note - 60.0) / 18.0);
    const double tilt = 2.2 - 1.6 * hard * (0.5 + 0.5 * v.vel);
    const double strike = upright ? 0.14 : 0.12;  // hammer position along the string (comb notches)
    const double pitchScale = std::pow(2.0, -(v.note - 60.0) / 24.0);
    v.parts = 0;
    double norm = 0;
    for (int n = 1; n <= Voice::kPartials; ++n) {
      double fn = n * f0 * std::sqrt(1.0 + B * n * n);
      if (fn > sr_ * 0.45) break;
      const int i = n - 1;
      double a = std::pow(static_cast<double>(n), -tilt) * std::fabs(std::sin(kPi * n * strike)) + 1e-4;
      v.pa[i] = a;
      norm += a;
      double tau = decay_ * pitchScale * (upright ? 0.7 : 1.0) / (1.0 + 0.45 * (n - 1));
      v.pdec[i] = std::exp(-1.0 / std::max(1.0, tau * sr_ * 0.25));
      for (int sd = 0; sd < 2; ++sd) {
        double cents = (sd == 0 ? -1.0 : 1.0) * detune_ * 0.5;
        double w = kTwoPi * fn * std::pow(2.0, cents / 1200.0) / sr_;
        v.cr[sd][i] = std::cos(w), v.ci[sd][i] = std::sin(w);
        v.re[sd][i] = 0, v.im[sd][i] = 1;  // start at sin phase 0 (no click)
      }
      v.parts = n;
    }
    for (int i = 0; i < v.parts; ++i) v.pa[i] /= std::max(1e-6, norm) * 0.85;
    v.prel = std::exp(-1.0 / std::max(1.0, release_ * sr_ * 0.25));
    v.knock = 0.25 * hard * v.vel;
    v.knockDec = std::exp(-1.0 / (0.006 * sr_));
    v.knockLp = 0;
  }
  void configure(const PluginState& s, const ChannelAux&, const ProcessContext& ctx) override {
    using namespace pp;
    sr_ = ctx.sampleRate;
    decay_ = param(s, KyDecay, 3.5f);
    release_ = param(s, KyRelease, 0.25f);
    hard_ = param(s, KyHardness, 0.45f);
    trem_ = param(s, KyTremDepth, 0.15f);
    tremRate_ = param(s, KyTremRate, 4.5f);
    stereo_ = param(s, KyStereo, 0.5f);
    detune_ = param(s, KyDetune, 4);
    drive_ = param(s, KyDrive, 0.1f);
    velSens_ = param(s, KyVelocity, 0.7f);
    vol_ = param(s, KyVolume, 0.7f);
    velHard_ = param(s, KyVelHardness), velMuffle_ = param(s, KyVelMuffle), tune_ = param(s, KyTune), stretch_ = param(s, KyStretch);
    autoPan_ = param(s, KyModMode) >= 0.5f;
    model_ = std::clamp(static_cast<int>(param(s, KyModel) + 0.5f), 0, 5);
    velCurve_ = std::clamp(static_cast<int>(param(s, KyVelCurve) + 0.5f), 0, 2);
    keyPan_ = param(s, KyKeyPan);
    muffle_ = param(s, KyMuffle, 9000);
    if (sr_ != lastSr_ || muffle_ != lastMuffle_ || param(s, KyTreble) != lastTreble_) {
      lastSr_ = sr_;
      lastMuffle_ = static_cast<float>(muffle_);
      lastTreble_ = param(s, KyTreble);
      for (int c = 0; c < 2; ++c) {
        lp_[c].set(Biquad::LowPass, sr_, lastMuffle_, 0.707, 0);
        shelf_[c].set(Biquad::HighShelf, sr_, 3000, 0.707, lastTreble_);
      }
    }
  }
  void onNoteOn(uint32_t id, double pitch, float vel) override {
    Voice* pick = nullptr;
    for (auto& v : voices_)
      if (!v.on) {
        pick = &v;
        break;
      }
    if (!pick)
      for (auto& v : voices_)
        if (!pick || v.age < pick->age) pick = &v;
    Voice& v = *pick;
    v = Voice{};
    v.on = v.held = true;
    v.id = id;
    v.note = pitch;
    v.hz = noteToHz(pitch + tune_ / 100.0 + stretch_ / 100.0 * (pitch - 60.0) / 12.0);
    if (velCurve_ == 1) vel = std::sqrt(vel);
    if (velCurve_ == 2) vel = vel * vel;
    v.vel = vel;
    v.age = ++clock_;
    v.amp = 1;
    v.modEnv = 1;
    if (velMuffle_ > 0) {
      double fc = std::min(sr_ * 0.45, muffle_ * std::pow(2.0, velMuffle_ * vel * 4.0));
      v.lp[0].setFreq(sr_, fc);
      v.lp[1].a = v.lp[0].a;
    }
    panGains(std::clamp(keyPan_ * (pitch - 60.0) / 30.0, -1.0, 1.0), v.panL, v.panR);
    v.seed = id * 2654435761u + 7u;
    if (pianoModel(model_)) startPiano(v);
  }
  void onNoteOff(uint32_t id) override {
    for (auto& v : voices_)
      if (v.on && v.held && v.id == id) v.held = false;
  }
  void onAllOff() override {
    for (auto& v : voices_) v.held = false;
  }
  void onKill() override {
    for (auto& v : voices_) v = Voice{};
  }
  void render(float* L, float* R, int n) override {
    // Model: modulator ratio, bell partial ratio and bell weight.
    static const double modRatio[4] = {1.0, 2.0, 1.0, 1.0}, bellRatio[4] = {14.0, 7.0, 19.0, 14.0}, bellAmt[4] = {0.25, 0.1, 0.45, 0.05};
    static const double hardScale[4] = {1.0, 0.8, 1.35, 0.55};
    const int mdl = std::min(model_, 3);  // tine models (voices started before a model change)
    if (n > 1024) {  // the mix buffers hold 1024 frames
      render(L, R, 1024);
      render(L + 1024, R + 1024, n - 1024);
      return;
    }
    for (int i = 0; i < n; ++i) bufL_[static_cast<size_t>(i)] = bufR_[static_cast<size_t>(i)] = 0;
    for (auto& v : voices_) {
      if (!v.on) continue;
      if (v.parts > 0) {  // piano model
        const double velG = 1 - velSens_ + velSens_ * v.vel;
        for (int i = 0; i < n; ++i) {
          double l = 0, r = 0, peak = 0;
          for (int p = 0; p < v.parts; ++p) {
            for (int sd = 0; sd < 2; ++sd) {  // rotate the phasor (recursive sine)
              double nr = v.re[sd][p] * v.cr[sd][p] - v.im[sd][p] * v.ci[sd][p];
              double ni = v.re[sd][p] * v.ci[sd][p] + v.im[sd][p] * v.cr[sd][p];
              v.re[sd][p] = nr, v.im[sd][p] = ni;
            }
            l += v.re[0][p] * v.pa[p];
            r += v.re[1][p] * v.pa[p];
            v.pa[p] *= v.held ? v.pdec[p] : v.prel * v.pdec[p];
            peak = std::max(peak, v.pa[p]);
          }
          if (v.knock > 1e-5) {  // hammer knock: short low-passed noise burst
            v.seed = v.seed * 1664525u + 1013904223u;
            double nz = (static_cast<double>(v.seed >> 9) / 4194304.0 - 1.0) * v.knock;
            v.knockLp += (nz - v.knockLp) * 0.35;
            l += v.knockLp, r += v.knockLp;
            v.knock *= v.knockDec;
          }
          double g = velG * 0.9;
          l *= g * v.panL, r *= g * v.panR;
          if (velMuffle_ > 0) l = v.lp[0].process(l), r = v.lp[1].process(r);
          bufL_[static_cast<size_t>(i)] += static_cast<float>(l);
          bufR_[static_cast<size_t>(i)] += static_cast<float>(r);
          // Every 256 samples of the voice's life (not per block, J-30):
          // end check, and renormalise the phasors (keeps the recursive sines stable).
          if ((++v.ticks & 255u) == 0) {
            if (peak < 1e-4 && v.knock < 1e-5) {
              v.on = false;
              break;
            }
            for (int p = 0; p < v.parts; ++p)
              for (int sd = 0; sd < 2; ++sd) {
                double m = std::sqrt(v.re[sd][p] * v.re[sd][p] + v.im[sd][p] * v.im[sd][p]);
                if (m > 0) v.re[sd][p] /= m, v.im[sd][p] /= m;
              }
          }
        }
        continue;
      }
      double pitchScale = std::pow(2.0, -(12.0 * std::log2(v.hz / 261.6)) / 24.0);  // higher keys decay faster
      double dec = std::exp(-1.0 / std::max(1.0, decay_ * pitchScale * sr_ * 0.25));
      double rel = std::exp(-1.0 / std::max(1.0, release_ * sr_ * 0.25));
      double modDec = std::exp(-1.0 / std::max(1.0, 0.25 * sr_ * 0.25));
      double velG = 1 - velSens_ + velSens_ * v.vel;
      double hard = std::clamp(hard_ * hardScale[mdl] + velHard_ * (v.vel - 0.5), 0.0, 2.0);
      double index = hard * (0.4 + 1.6 * v.vel);
      double dL = v.hz * std::pow(2.0, -detune_ / 1200.0) / sr_, dR = v.hz * std::pow(2.0, detune_ / 1200.0) / sr_;
      for (int i = 0; i < n; ++i) {
        double mod = std::sin(kTwoPi * v.mph) * index * v.modEnv + std::sin(kTwoPi * v.bph) * index * bellAmt[mdl] * v.modEnv * v.modEnv;
        double l = std::sin(kTwoPi * v.ph[0] + mod), r = std::sin(kTwoPi * v.ph[1] + mod);
        double a = v.amp * velG;
        l *= a * v.panL, r *= a * v.panR;
        if (velMuffle_ > 0) l = v.lp[0].process(l), r = v.lp[1].process(r);
        bufL_[static_cast<size_t>(i)] += static_cast<float>(l);
        bufR_[static_cast<size_t>(i)] += static_cast<float>(r);
        v.ph[0] += dL;
        v.ph[1] += dR;
        v.mph += v.hz * modRatio[mdl] / sr_;
        v.bph += v.hz * bellRatio[mdl] / sr_;
        v.ph[0] -= std::floor(v.ph[0]);
        v.ph[1] -= std::floor(v.ph[1]);
        v.mph -= std::floor(v.mph);
        v.bph -= std::floor(v.bph);
        v.amp *= v.held ? dec : rel;
        v.modEnv *= modDec;
        if (v.amp < 1e-4) {  // ends on its own clock, not at a block end (J-30)
          v.on = false;
          break;
        }
      }
    }
    for (int i = 0; i < n; ++i) {
      double t = std::sin(kTwoPi * tph_);
      tph_ += tremRate_ / sr_;
      tph_ -= std::floor(tph_);
      double gl, gr;
      if (autoPan_) {  // auto-pan: move the image between the sides
        panGains(t * trem_, gl, gr);
      } else {
        gl = 1 - trem_ * (0.5 + 0.5 * t * stereo_ + 0.5 * (1 - stereo_)), gr = 1 - trem_ * (0.5 - 0.5 * t * stereo_ + 0.5 * (1 - stereo_));
      }
      double l = bufL_[static_cast<size_t>(i)], r = bufR_[static_cast<size_t>(i)];
      double m = (l + r) * 0.5;
      l = m + (l - m) * stereo_;
      r = m + (r - m) * stereo_;
      l = shelf_[0].process(lp_[0].process(l));
      r = shelf_[1].process(lp_[1].process(r));
      double d = 1 + drive_ * 5;
      l = std::tanh(l * 0.5 * d) / std::tanh(d * 0.5);
      r = std::tanh(r * 0.5 * d) / std::tanh(d * 0.5);
      L[i] += static_cast<float>(l * gl * vol_ * 0.6);
      R[i] += static_cast<float>(r * gr * vol_ * 0.6);
    }
  }

 private:
  std::array<Voice, 24> voices_{};
  std::array<float, 1024> bufL_{}, bufR_{};
  Biquad lp_[2], shelf_[2];
  double sr_ = 44100, lastSr_ = 0, tph_ = 0, muffle_ = 9000;
  float lastMuffle_ = -1, lastTreble_ = -99;
  double decay_ = 3.5, release_ = 0.25, hard_ = 0.45, trem_ = 0.15, tremRate_ = 4.5, stereo_ = 0.5, detune_ = 4, drive_ = 0.1, velSens_ = 0.7, vol_ = 0.7;
  double velHard_ = 0, velMuffle_ = 0, tune_ = 0, stretch_ = 0, keyPan_ = 0;
  bool autoPan_ = false;
  int model_ = 0, velCurve_ = 0;
  uint64_t clock_ = 0;
};

// ============================================================ Slicer
class SlicerGen final : public Generator {
 public:
  bool active() const override {
    if (sx_) return sxe_.active();
    for (const auto& v : voices_)
      if (v.on) return true;
    return false;
  }
  bool handlesNotePan() const override { return sx_; }

 protected:
  struct Voice {
    bool on = false, held = false;
    uint32_t id = 0;
    int slice = 0, cut = 0;
    double pos = 0, end = 0, start = 0, rate = 1;
    float vel = 0, gl = 1, gr = 1, fade = 1, fadeStep = 0;
    bool reverse = false;
    int64_t delayLeft = 0;  // region start delay (samples)
    Adsr env;
    Svf filt[2];
  };
  void configure(const PluginState& s, const ChannelAux& aux, const ProcessContext& ctx) override {
    using namespace pp;
    // J-41: Slicex region engine.
    sx_ = param(s, SlEngine, 0) >= 0.5f && param(s, SlEngine, 0) < SlEngineProcessed - 0.5f && dynamic_cast<const slicex::Aux*>(aux.custom.get()) != nullptr;
    proc_ = param(s, SlEngine, 0) >= SlEngineProcessed - 0.5f;  // J-41: processed slices (aux sample + starts)
    if (sx_) {
      sxe_.configure(s, aux, ctx);
      return;
    }
    sr_ = ctx.sampleRate;
    aux_ = &aux;
    state_ = &s;
    base_ = static_cast<int>(param(s, SlBaseNote, 60) + 0.5f);
    pitch_ = param(s, SlPitch) + param(s, SlFine) / 100.0;
    reverse_ = param(s, SlReverse) >= 0.5f;
    vol_ = param(s, SlVolume, 0.8f);
    playToEnd_ = param(s, SlPlayToEnd) >= 0.5f;
    hold_ = param(s, SlHold) >= 0.5f;
    mono_ = param(s, SlMono) >= 0.5f;
    fadeIn_ = param(s, SlFadeIn), fadeOut_ = param(s, SlFadeOut);
    declick_ = param(s, SlDeclick, 1) >= 0.5f;
    ftype_ = std::clamp(static_cast<int>(param(s, SlFilterType) + 0.5f), 0, 3);
    cutoff_ = param(s, SlCutoff, 20000), reso_ = param(s, SlReso);
    panGains(param(s, SlPan), panL_, panR_);
    velAmt_ = param(s, SlVelocity, 1);
    chromatic_ = static_cast<int>(std::lround(param(s, SlChromatic, -1)));
    modX_ = param(s, SlModX, 0.5f), modY_ = param(s, SlModY, 0.5f);
    speed_ = 1.0;
    SamplePtr smp = aux.sample();
    if (smp && param(s, SlFitTempo) >= 0.5f && smp->seconds() > 0) {
      double loopSec = param(s, SlLoopBeats, 4) * 60.0 / std::max(20.0, ctx.tempo);
      speed_ = smp->seconds() / loopSec;
    }
    // Slice starts: manual markers or the prepared automatic slices.
    nSlices_ = 0;
    if (proc_) {
      for (size_t k = 0; k < aux.slices.size() && nSlices_ < kMaxSlices; ++k) starts_[nSlices_++] = aux.slices[k];
    } else if (static_cast<int>(param(s, SlMode) + 0.5f) == SlModeManual && smp) {
      for (int k = 0; k < kMaxSlices; ++k) {
        float m = param(s, slMarker(k), -1);
        if (m >= 0) starts_[nSlices_++] = static_cast<int64_t>(m * static_cast<double>(smp->frames()));
      }
      std::sort(starts_, starts_ + nSlices_);
    } else {
      for (size_t k = 0; k < aux.slices.size() && nSlices_ < kMaxSlices; ++k) starts_[nSlices_++] = aux.slices[k];
    }
    for (auto& v : voices_) v.env.set(sr_, std::max(declick_ ? 0.001 : 0.0, static_cast<double>(param(s, SlAttack, 0.001f))), 30, 1,
                                    std::max(declick_ ? 0.002 : 0.0, static_cast<double>(param(s, SlRelease, 0.01f))));
  }
  void onNoteOnEx(uint32_t id, double pitch, float vel, const NoteProps& np) override {
    if (sx_) {
      sxe_.noteOn(id, pitch, vel, np);
      return;
    }
    noteModX_ = np.modX - 0.5, noteModY_ = np.modY - 0.5;
    onNoteOn(id, pitch, vel);
    noteModX_ = noteModY_ = 0;
  }
  void onNoteOn(uint32_t id, double pitch, float vel) override {
    using namespace pp;
    if (sx_) {
      sxe_.noteOn(id, pitch, vel, NoteProps{});
      return;
    }
    SamplePtr smp = aux_ ? aux_->sample() : nullptr;
    if (!smp || nSlices_ == 0) return;
    const PluginState& s = *state_;
    int note = static_cast<int>(std::lround(pitch));
    int k = -1;
    for (int j = 0; j < nSlices_ && k < 0; ++j)  // regions assigned to a key
      if (static_cast<int>(std::lround(param(s, slParam2(j, SliceKey), -1))) == note) k = j;
    double semis = 0;
    if (k < 0) {
      k = note - base_;
      if (chromatic_ >= 0 && chromatic_ < nSlices_) {  // one slice played across the keyboard
        semis = k;
        k = chromatic_;
      }
    }
    if (k < 0 || k >= nSlices_) return;
    if (param(s, slParam(k, SliceMute)) >= 0.5f) return;
    semis += pitch - note;  // J-74: the note's fine pitch (the rest after the key) still tunes the slice
    const int cut = static_cast<int>(std::lround(param(s, slParam2(k, SliceCut))));
    for (auto& v : voices_)
      if (v.on && (mono_ || (cut > 0 && v.cut == cut))) v.fadeStep = static_cast<float>(1.0 / std::max(1.0, 0.003 * sr_));
    Voice* pick = &voices_[next_++ % voices_.size()];
    for (auto& v : voices_)
      if (!v.on) {
        pick = &v;
        break;
      }
    Voice& v = *pick;
    v.on = v.held = true;
    v.id = id;
    v.vel = vel;
    v.slice = k;
    v.cut = cut;
    v.fade = 1, v.fadeStep = 0;
    v.delayLeft = static_cast<int64_t>(param(s, slParam2(k, SliceDelay)) * sr_);
    double sliceStart = static_cast<double>(starts_[k]);
    double sliceEnd = !playToEnd_ && k + 1 < nSlices_ ? static_cast<double>(starts_[k + 1]) : static_cast<double>(smp->frames() - 1);
    const double endPos = param(s, slParam2(k, SliceEnd), -1);
    if (endPos >= 0 && !playToEnd_) sliceEnd = std::clamp(endPos * static_cast<double>(smp->frames()), sliceStart + 1, static_cast<double>(smp->frames() - 1));
    v.start = sliceStart + param(s, slParam(k, SliceStart)) * (sliceEnd - sliceStart);
    v.end = sliceEnd;
    v.reverse = reverse_ != (!proc_ && param(s, slParam(k, SliceReverse)) >= 0.5f);  // (processed: reversed offline)
    v.pos = v.reverse ? v.end - 1 : v.start;
    v.rate = std::pow(2.0, (pitch_ + semis + param(s, slParam(k, SlicePitch))) / 12.0) * smp->sampleRate / sr_ * speed_;
    double g = dbToGain(param(s, slParam(k, SliceGain))) * (1 - velAmt_ + velAmt_ * vel);
    double pl, pr;
    panGains(param(s, slParam(k, SlicePan)), pl, pr);
    v.gl = static_cast<float>(g * pl * panL_), v.gr = static_cast<float>(g * pr * panR_);
    if (ftype_ > 0) {
      double fc = cutoff_ * std::pow(2.0, param(s, slParam(k, SliceCutoff)) * 5.0 + ((modX_ - 0.5) + noteModX_) * 8.0);
      double rs = std::clamp(reso_ + param(s, slParam2(k, SliceReso)) * 0.5 + ((modY_ - 0.5) + noteModY_), 0.0, 1.0);
      fc = std::clamp(fc, 20.0, sr_ * 0.45);
      v.filt[0].set(sr_, fc, rs), v.filt[1].set(sr_, fc, rs);
      v.filt[0].reset(), v.filt[1].reset();
    }
    v.env.level = 0;
    v.env.gate(true);
  }
  void onNoteOff(uint32_t id) override {
    if (sx_) {
      sxe_.noteOff(id);
      return;
    }
    if (hold_) return;
    for (auto& v : voices_)
      if (v.on && v.held && v.id == id) {
        v.held = false;
        v.env.gate(false);
      }
  }
  void onAllOff() override {
    sxe_.allOff();
    for (auto& v : voices_) v.env.gate(false), v.held = false;
  }
  void onKill() override {
    sxe_.kill();
    for (auto& v : voices_) v = Voice{};
  }
  void render(float* L, float* R, int n) override {
    if (sx_) {
      sxe_.render(L, R, n);
      return;
    }
    SamplePtr smp = aux_ ? aux_->sample() : nullptr;
    if (!smp) return;
    for (auto& v : voices_) {
      if (!v.on) continue;
      double span = std::max(1.0, v.end - v.start);
      double edge = declick_ ? 0.001 * sr_ : 0.0;
      for (int i = 0; i < n; ++i) {
        if (v.delayLeft > 0) {  // region delay: silent until it starts
          --v.delayLeft;
          v.fade -= v.fadeStep;
          if (v.fade <= 0) v.on = false;
          if (!v.on) break;
          continue;
        }
        if ((!v.reverse && v.pos >= v.end) || (v.reverse && v.pos < v.start) || v.fade <= 0) {
          v.on = false;
          break;
        }
        double a = v.env.next();
        if (!v.env.active()) {
          v.on = false;
          break;
        }
        double rel = (v.pos - v.start) / span;  // position in the slice, 0..1
        double f = 1;
        if (fadeIn_ > 0 && rel < fadeIn_) f *= rel / fadeIn_;
        if (fadeOut_ > 0 && rel > 1 - fadeOut_) f *= (1 - rel) / fadeOut_;
        double toEnd = v.reverse ? v.pos - v.start : v.end - v.pos;
        if (edge > 0 && toEnd < edge) f *= toEnd / edge;
        double g = a * vol_ * f * v.fade;
        float bl, br;
        interpBl(*smp, v.pos, v.rate, bl, br);
        double l = bl, r = br;
        if (ftype_ > 0) {
          double io[2] = {l, r};
          for (int c = 0; c < 2; ++c) {
            double band = 0, high = 0, low = v.filt[c].process(io[c], &band, &high);
            io[c] = ftype_ == 1 ? low : ftype_ == 2 ? high : band;
          }
          l = io[0], r = io[1];
        }
        L[i] += static_cast<float>(l * g * v.gl);
        R[i] += static_cast<float>(r * g * v.gr);
        v.pos += v.reverse ? -v.rate : v.rate;
        v.fade -= v.fadeStep;
      }
    }
  }

 private:
  std::array<Voice, 16> voices_{};
  const ChannelAux* aux_ = nullptr;
  const PluginState* state_ = nullptr;
  int64_t starts_[pp::kMaxSlices] = {};
  int nSlices_ = 0;
  double sr_ = 44100, pitch_ = 0, speed_ = 1, fadeIn_ = 0, fadeOut_ = 0, cutoff_ = 20000, reso_ = 0, panL_ = 1, panR_ = 1, velAmt_ = 1;
  int base_ = 60, ftype_ = 0, chromatic_ = -1;
  double modX_ = 0.5, modY_ = 0.5, noteModX_ = 0, noteModY_ = 0;
  bool reverse_ = false, playToEnd_ = false, hold_ = false, mono_ = false, declick_ = true;
  float vol_ = 0.8f;
  size_t next_ = 0;
  bool sx_ = false, proc_ = false;
  slicex::Engine sxe_;
};

// ============================================================ Granular
class GranularGen final : public Generator {
 public:
  bool active() const override {
    for (const auto& v : notes_)
      if (v.on) return true;
    return false;
  }

 protected:
  struct Grain {
    bool on = false;
    double pos = 0, rate = 1, t = 0, len = 1;
    float gl = 0, gr = 0;
    int note = -1;
  };
  struct NoteV {
    bool on = false, held = false;
    uint32_t id = 0;
    double pitch = 60, nextGrain = 0, scan = 0, dwell = 0;
    float vel = 0;
    Adsr env;
  };
  void configure(const PluginState& s, const ChannelAux& aux, const ProcessContext& ctx) override {
    using namespace pp;
    sr_ = ctx.sampleRate;
    aux_ = &aux;
    pos_ = param(s, GrPosition, 0.3f);
    spread_ = param(s, GrSpread, 0.1f);
    size_ = param(s, GrSize, 0.08f);
    density_ = param(s, GrDensity, 25);
    prand_ = param(s, GrPitchRand, 0.1f);
    stereo_ = param(s, GrStereo, 0.6f);
    freeze_ = param(s, GrFreeze, 1) >= 0.5f;
    vol_ = param(s, GrVolume, 0.7f);
    speed_ = param(s, GrSpeed, 1), shape_ = param(s, GrShape, 0.5f);
    loop_ = param(s, GrLoop, 1) >= 0.5f;
    keyMode_ = std::clamp(static_cast<int>(param(s, GrKeyMode) + 0.5f), 0, 2);
    baseNote_ = param(s, GrBaseNote, 60);
    if (param(s, GrTempoSync) >= 0.5f) density_ = density_ * std::max(20.0, ctx.tempo) / 60.0 / 4.0;  // grains per 1/16 beat → Hz
    wobble_ = param(s, GrWobble), wobbleRate_ = param(s, GrWobbleRate, 4), jitter_ = param(s, GrJitter);
    stepHold_ = param(s, GrTransientHold, 0.25f), pitchOfs_ = param(s, GrPitch);
    for (auto& v : notes_) v.env.set(sr_, param(s, GrAttack, 0.05f), 30, 1, param(s, GrRelease, 0.6f));
  }
  void onNoteOn(uint32_t id, double pitch, float vel) override {
    for (auto& v : notes_)
      if (!v.on) {
        v.on = v.held = true;
        v.id = id;
        v.pitch = pitch;
        v.vel = vel;
        v.nextGrain = 0;
        v.scan = 0;
        v.dwell = keyMode_ == 2 ? stepHold_ * sr_ : 0;
        v.env.level = 0;
        v.env.gate(true);
        rng_.seed(id * 2654435761u + 17);
        return;
      }
  }
  void onNoteOff(uint32_t id) override {
    for (auto& v : notes_)
      if (v.on && v.held && v.id == id) v.held = false, v.env.gate(false);
  }
  void onAllOff() override {
    for (auto& v : notes_) v.held = false, v.env.gate(false);
  }
  void onKill() override {
    for (auto& v : notes_) v = NoteV{};
    for (auto& g : grains_) g = Grain{};
  }
  void render(float* L, float* R, int n) override {
    SamplePtr smp = aux_ ? aux_->sample() : nullptr;
    if (!smp || smp->frames() < 16) return;
    const Sample& sm = *smp;
    const double len = static_cast<double>(sm.frames());
    for (int i = 0; i < n; ++i) {
      for (int ni = 0; ni < static_cast<int>(notes_.size()); ++ni) {  // spawn grains
        NoteV& v = notes_[static_cast<size_t>(ni)];
        if (!v.on) continue;
        v.nextGrain -= 1;
        if (v.dwell > 0)
          v.dwell -= 1;
        else if (!freeze_)
          v.scan += speed_;
        if (!loop_ && !freeze_ && (pos_ * len + v.scan * sm.sampleRate / sr_ >= len || pos_ * len + v.scan * sm.sampleRate / sr_ < 0)) {
          v.env.gate(false);
          v.held = false;
        }
        if (v.nextGrain <= 0) {
          v.nextGrain += sr_ / density_ * (1.0 + jitter_ * rng_.next() * 0.9);
          for (auto& g : grains_)
            if (!g.on) {
              double r = rng_.next();
              double basePos = pos_;
              double semis = v.pitch - 60;
              if (keyMode_ == 1) basePos += (v.pitch - baseNote_) / 24.0, semis = 0;              // keys pick the position
              if (keyMode_ == 2) basePos = std::floor(v.pitch - baseNote_) / 16.0, semis = 0;  // keys pick 1/16 steps
              basePos -= std::floor(basePos);
              double start = (basePos + r * spread_) * len + v.scan * sm.sampleRate / sr_;
              g.on = true;
              g.note = ni;
              g.pos = std::fmod(std::fmod(start, len) + len, len);
              double wob = wobble_ > 0 ? wobble_ * std::sin(kTwoPi * wobPh_) : 0.0;
              g.rate = std::pow(2.0, (semis + pitchOfs_ + wob + rng_.next() * prand_) / 12.0) * sm.sampleRate / sr_;
              g.t = 0;
              g.len = std::max(16.0, size_ * sr_);
              double pan = rng_.next() * stereo_, l, rr;
              panGains(pan, l, rr);
              g.gl = static_cast<float>(l);
              g.gr = static_cast<float>(rr);
              break;
            }
        }
      }
      double outL = 0, outR = 0;
      for (auto& g : grains_) {
        if (!g.on) continue;
        NoteV& v = notes_[static_cast<size_t>(g.note)];
        double ph = g.t / g.len, edge = std::clamp(shape_, 0.02, 0.5);
        double w = ph < edge ? 0.5 - 0.5 * std::cos(kPi * ph / edge) : ph > 1 - edge ? 0.5 - 0.5 * std::cos(kPi * (1 - ph) / edge) : 1.0;
        double a = w * v.env.level * v.vel * vol_ * 0.35;
        if (g.pos >= len - 1) g.pos = 0;
        outL += interp(sm.left, g.pos) * a * g.gl;
        outR += interp(sm.right, g.pos) * a * g.gr;
        g.pos += g.rate;
        g.t += 1;
        if (g.t >= g.len) g.on = false;
      }
      for (auto& v : notes_)
        if (v.on) {
          v.env.next();
          if (!v.env.active()) v.on = false;
        }
      L[i] += static_cast<float>(outL);
      R[i] += static_cast<float>(outR);
      wobPh_ += wobbleRate_ / sr_;
      wobPh_ -= std::floor(wobPh_);
    }
  }

 private:
  std::array<NoteV, 8> notes_{};
  std::array<Grain, 96> grains_{};
  const ChannelAux* aux_ = nullptr;
  Noise rng_;
  double sr_ = 44100, pos_ = 0.3, spread_ = 0.1, size_ = 0.08, density_ = 25, prand_ = 0.1, stereo_ = 0.6;
  bool freeze_ = true, loop_ = true;
  float vol_ = 0.7f;
  double speed_ = 1, shape_ = 0.5, baseNote_ = 60, wobble_ = 0, wobbleRate_ = 4, jitter_ = 0, stepHold_ = 0.25, pitchOfs_ = 0, wobPh_ = 0;
  int keyMode_ = 0;
};

// ============================================================ MIDI Out
class MidiOutGen final : public Generator {
 public:
  bool active() const override { return false; }
  void setMidiOut(MidiOutRing* r) override { out_ = r; }

 protected:
  void configure(const PluginState& s, const ChannelAux&, const ProcessContext& ctx) override {
    using namespace pp;
    ch_ = std::clamp(static_cast<int>(param(s, MoChannel, 1) + 0.5f), 1, 16) - 1;
    port_ = std::clamp(static_cast<int>(param(s, MoPort) + 0.5f), 0, MidiPortBus::kPorts - 1);
    bus_ = ctx.ports;
    if (bus_)  // the bus is cleared every block: re-announce the held notes
      for (auto& h : held_)
        if (h.id) bus_->set(port_, h.pitch, h.vel);
    int prog = static_cast<int>(param(s, MoProgram) + 0.5f);
    int msb = static_cast<int>(std::lround(param(s, MoBankMsb, -1))), lsb = static_cast<int>(std::lround(param(s, MoBankLsb, -1)));
    if ((prog != prog_ || msb != msb_ || lsb != lsb_) && out_ && prog > 0) {  // bank select before the program change
      if (msb >= 0) out_->push(static_cast<uint8_t>(0xB0 | ch_), 0, static_cast<uint8_t>(msb));
      if (lsb >= 0) out_->push(static_cast<uint8_t>(0xB0 | ch_), 32, static_cast<uint8_t>(lsb));
      out_->push(static_cast<uint8_t>(0xC0 | ch_), static_cast<uint8_t>(prog - 1), 0);
    }
    prog_ = prog, msb_ = msb, lsb_ = lsb;
    // J-92: the pitch bend range reaches the receiver as RPN 0 (it was never sent).
    const int range = std::clamp(static_cast<int>(std::lround(param(s, MoBendRange, 2))), 0, 127);
    if (range != bendRange_ && out_) {
      const uint8_t cc = static_cast<uint8_t>(0xB0 | ch_);
      out_->push(cc, 101, 0), out_->push(cc, 100, 0), out_->push(cc, 6, static_cast<uint8_t>(range)), out_->push(cc, 38, 0);
      out_->push(cc, 101, 127), out_->push(cc, 100, 127);  // RPN null
    }
    bendRange_ = range;
    velScale_ = param(s, MoVelocity, 1);
    velCurve_ = static_cast<int>(param(s, MoVelCurve) + 0.5f);
    // Controller knobs: send when the value changes.
    for (int k = 0; k < kMoKnobs; ++k) {
      int value = static_cast<int>(std::lround(param(s, moKnob(k, MoKnobValue))));
      if (value == knobLast_[k]) continue;
      knobLast_[k] = value;
      if (!out_) continue;
      int type = static_cast<int>(param(s, moKnob(k, MoKnobType)) + 0.5f);
      int num = static_cast<int>(std::lround(param(s, moKnob(k, MoKnobNumber), 20 + k)));
      double lo = param(s, moKnob(k, MoKnobMin)), hi = param(s, moKnob(k, MoKnobMax), 127);
      int scaled = static_cast<int>(std::lround(lo + value / 127.0 * (hi - lo)));
      int chn = static_cast<int>(std::lround(param(s, moKnob(k, MoKnobChannel))));
      uint8_t c = static_cast<uint8_t>(chn > 0 ? chn - 1 : ch_);
      switch (type) {
        case 1:  // RPN
        case 2: {  // NRPN
          out_->push(static_cast<uint8_t>(0xB0 | c), type == 1 ? 101 : 99, static_cast<uint8_t>((num >> 7) & 127));
          out_->push(static_cast<uint8_t>(0xB0 | c), type == 1 ? 100 : 98, static_cast<uint8_t>(num & 127));
          out_->push(static_cast<uint8_t>(0xB0 | c), 6, static_cast<uint8_t>(std::clamp(scaled >> 7, 0, 127)));
          out_->push(static_cast<uint8_t>(0xB0 | c), 38, static_cast<uint8_t>(scaled & 127));
          break;
        }
        case 3: out_->push(static_cast<uint8_t>(0xD0 | c), static_cast<uint8_t>(std::clamp(scaled, 0, 127)), 0); break;
        case 4:
          if (scaled > 0)
            out_->push(static_cast<uint8_t>(0x90 | c), static_cast<uint8_t>(num & 127), static_cast<uint8_t>(std::clamp(scaled, 1, 127)));
          else
            out_->push(static_cast<uint8_t>(0x80 | c), static_cast<uint8_t>(num & 127), 0);
          break;
        default: out_->push(static_cast<uint8_t>(0xB0 | c), static_cast<uint8_t>(num & 127), static_cast<uint8_t>(std::clamp(scaled, 0, 127)));
      }
    }
  }
  void onNoteOn(uint32_t id, double pitch, float vel) override {
    int p = std::clamp(static_cast<int>(std::lround(pitch)), 0, 127);
    double v = velCurve_ == 1 ? std::sqrt(vel) : velCurve_ == 2 ? vel * vel : velCurve_ == 3 ? 100.0 / 127.0 : vel;
    const int v7 = std::clamp(static_cast<int>(v * 127 * velScale_), 1, 127);
    for (auto& h : held_)
      if (h.id == 0) {
        h = {id, p, v7};
        break;
      }
    if (bus_) bus_->set(port_, p, v7);
    if (out_) out_->push(static_cast<uint8_t>(0x90 | ch_), static_cast<uint8_t>(p), static_cast<uint8_t>(std::clamp(static_cast<int>(v * 127 * velScale_), 1, 127)));
  }
  void onNoteOff(uint32_t id) override {
    for (auto& h : held_)
      if (h.id == id && id != 0) {
        if (bus_) bus_->set(port_, h.pitch, 0);
        if (out_) out_->push(static_cast<uint8_t>(0x80 | ch_), static_cast<uint8_t>(h.pitch), 0);
        h = {};
      }
  }
  void onAllOff() override {
    for (auto& h : held_)
      if (h.id) {
        if (bus_) bus_->set(port_, h.pitch, 0);
        if (out_) out_->push(static_cast<uint8_t>(0x80 | ch_), static_cast<uint8_t>(h.pitch), 0);
        h = {};
      }
  }
  void onKill() override {
    onAllOff();
    if (out_) out_->push(static_cast<uint8_t>(0xB0 | ch_), 123, 0);
  }
  void render(float*, float*, int) override {}
  // J-92: pitch bend and channel pressure from the keyboard go out on this channel.
  void onMidi(uint8_t status, uint8_t d1, uint8_t d2) override {
    const uint8_t kind = status & 0xF0;
    if (out_ && (kind == 0xE0 || kind == 0xD0)) out_->push(static_cast<uint8_t>(kind | ch_), d1, d2);
  }

 private:
  struct Held {
    uint32_t id = 0;
    int pitch = 0;
    int vel = 100;
  };
  std::array<Held, 128> held_{};
  int bendRange_ = 2;  // the receiver's default (General MIDI): announced only when it differs
  MidiOutRing* out_ = nullptr;
  MidiPortBus* bus_ = nullptr;
  int port_ = 0;
  int ch_ = 0, prog_ = 0, msb_ = -1, lsb_ = -1, velCurve_ = 0;
  int knobLast_[pp::kMoKnobs] = {};
  float velScale_ = 1;
};

}  // namespace

std::unique_ptr<Generator> createGeneratorW2(const std::string& type) {
  if (type == "gl.pads") return std::make_unique<PadsGen>();
  if (type == "gl.fm") return std::make_unique<FmGen>();
  if (type == "gl.acid") return std::make_unique<AcidGen>();
  if (type == "gl.keys") return std::make_unique<KeysGen>();
  if (type == "gl.slicer") return std::make_unique<SlicerGen>();
  if (type == "gl.granular") return std::make_unique<GranularGen>();
  if (type == "gl.midiout") return std::make_unique<MidiOutGen>();
  return nullptr;
}

}  // namespace gl
