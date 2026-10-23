#include "../packs/Packs.h"
#include "../packs/synths/trioref.h"
#include "../host/HostedPlugins.h"
#include "Generators.h"
#include "ChannelIns.h"
#include <atomic>
#include <cmath>
#include <memory>
#include <vector>
#include "../core/PluginParams.h"
#include "Dsp.h"
#include "Resample.h"
#include "TimeStretch.h"

namespace gl {

using namespace dsp;

void Generator::fire(const Ev& e) {
  switch (e.kind) {
    case Ev::On:
      if (e.hasProps) {
        const bool silent = !active();  // nothing sounding: the pan can jump without a click
        onNoteOnEx(e.id, e.pitch, e.velocity, e.props);
        if (!handlesNotePan()) {
          panTarget_ = e.props.pan;
          panUsed_ = panUsed_ || e.props.pan != 0;
          if (silent) panCur_ = panTarget_;
        }
      } else {
        const bool silent = panUsed_ && !active();
        onNoteOn(e.id, e.pitch, e.velocity);
        panTarget_ = 0;
        if (silent) panCur_ = 0;
      }
      break;
    case Ev::Off: onNoteOff(e.id); break;
    case Ev::AllOff: onAllOff(); break;
    case Ev::Glide: onGlide(e.id, e.pitch, e.time); break;
    case Ev::Cut: onCut(e.id); break;
    case Ev::Midi:
      onMidi(static_cast<uint8_t>(e.id & 0xFF), static_cast<uint8_t>((e.id >> 8) & 0xFF), static_cast<uint8_t>((e.id >> 16) & 0xFF));
      break;
  }
}

void Generator::process(const PluginState& state, const ChannelAux& aux, const ProcessContext& ctx, float* L,
                        float* R, int n) {
  configure(state, aux, ctx);
  sampleRate_ = ctx.sampleRate;
  // Stable insertion sort by offset (event lists are short).
  for (size_t i = 1; i < count_; ++i) {
    Ev e = events_[i];
    size_t j = i;
    while (j > 0 && events_[j - 1].offset > e.offset) {
      events_[j] = events_[j - 1];
      --j;
    }
    events_[j] = e;
  }
  // Output pan for instruments that do not pan their voices: follows the most
  // recent note's pan with a ~5 ms per-sample smoother (block-size independent).
  const float panCoef = static_cast<float>(1.0 - std::exp(-1.0 / (0.005 * sampleRate_)));
  auto applyPan = [&](int from, int to) {
    if (!panUsed_) return;
    for (int i = from; i < to; ++i) {
      panCur_ += (panTarget_ - panCur_) * panCoef;
      float l = panCur_ > 0 ? 1.0f - panCur_ : 1.0f, r = panCur_ < 0 ? 1.0f + panCur_ : 1.0f;
      L[i] *= l;
      R[i] *= r;
    }
    if (panTarget_ == 0 && std::fabs(panCur_) < 1e-5f) panUsed_ = false, panCur_ = 0;
  };
  if (wholeBlock()) {
    for (size_t i = 0; i < count_; ++i) {
      evOffset_ = std::clamp(events_[i].offset, 0, std::max(0, n - 1));
      fire(events_[i]);
    }
    evOffset_ = 0;
    count_ = 0;
    render(L, R, n);
    applyPan(0, n);
    return;
  }
  int pos = 0;
  size_t ei = 0;
  while (pos < n) {
    while (ei < count_ && events_[ei].offset <= pos) fire(events_[ei++]);
    int end = n;
    if (ei < count_) end = std::min(n, std::max(pos + 1, events_[ei].offset));
    render(L + pos, R + pos, end - pos);
    applyPan(pos, end);
    pos = end;
  }
  // Events at or beyond n (should not happen) fire at the block end.
  while (ei < count_) fire(events_[ei++]);
  count_ = 0;
}

namespace {

float param(const PluginState& s, int i, float dflt = 0.f) {
  return i < static_cast<int>(s.params.size()) ? s.params[static_cast<size_t>(i)] : dflt;
}

// ---------------------------------------------------------------------------
// Trio Synth: 3 oscillators (+ noise), each with tuning, pan, invert, stereo
// phase and stereo detune; osc 3 can amplitude/ring/frequency/phase-modulate
// oscillators 1+2 and osc 2 can modulate osc 1 → per-side resonant filter
// (with envelope, key tracking, velocity) → amp ADSR; one LFO per voice.
class SynthGen final : public Generator {
 public:
  bool active() const override {
    for (const auto& v : voices_)
      if (v.amp.active() || v.rOn) return true;
    return false;
  }

 protected:
  struct Voice {
    uint32_t id = 0;
    bool held = false;
    // J-65 reference oscillators: phases, release declick position, host note gains.
    packs::synths::trioref::Voice rv;
    bool rOn = false;
    int relK = -1;
    float hostL = 0.70710678f, hostR = 0.70710678f;
    double pitch = 60, target = 60;
    float vel = 0;
    uint64_t age = 0;
    Osc osc[2][3];  // [side][osc]
    Noise noise;
    Adsr amp, fenv;
    Svf filt[2][2];  // [side][stage]
    uint32_t ctr = 0;
    double dt[2][3] = {};
    double lfoPh = 0, lfo = 0;
    float rnd = 0;
    double npL = 1, npR = 1, cutOct = 0, resoAdd = 0;  // per-note pan / mod X / mod Y
    PitchGlide slide;
    InsVoice ins;            // channel instrument envelopes / filter (J-33)
    double insX = 0, insY = 0;  // note mod X / Y for them
  };

  void configure(const PluginState& s, const ChannelAux& aux, const ProcessContext& ctx) override {
    using namespace pp;
    sr_ = ctx.sampleRate;
    insSet_ = aux.ins;
    tempo_ = ctx.tempo, ppq_ = ctx.ppq;
    tick0_ = ctx.tick, blockPos_ = 0;
    const int shapeP[3] = {SynOsc1Shape, SynOsc2Shape, SynOsc3Shape};
    custom_ = aux.sample(0);
    if (custom_ && custom_->frames() < 2) custom_ = nullptr;
    hq_ = param(s, SynHQ, 1) >= 0.5f;
    for (int i = 0; i < 3; ++i) {
      shapeI_[i] = std::clamp(static_cast<int>(param(s, shapeP[i]) + 0.5f), 0, kCustomShape);
      shape_[i] = static_cast<Shape>(std::min(shapeI_[i], 6));
    }
    level_[0] = param(s, SynOsc1Level);
    level_[1] = param(s, SynOsc2Level);
    level_[2] = param(s, SynOsc3Level);
    tune_[0] = param(s, SynOsc1Coarse) + param(s, SynOsc1Fine) / 100.0;
    tune_[1] = param(s, SynOsc2Coarse) + param(s, SynOsc2Fine) / 100.0;
    tune_[2] = param(s, SynOsc3Coarse) + param(s, SynOsc3Fine) / 100.0;
    stereo_ = false;
    for (int k = 0; k < 3; ++k) {
      double pan = param(s, SynOsc1Pan + k);
      panGains(pan, panL_[k], panR_[k]);
      sign_[k] = param(s, SynOsc1Invert + k) >= 0.5f ? -1.0 : 1.0;
      phaseOfs_[k] = param(s, SynOsc1Phase + k);
      detune_[k] = param(s, SynOsc1Detune + k) / 200.0;  // ± half the spread, in semitones
      if (pan != 0 || phaseOfs_[k] != 0 || detune_[k] != 0) stereo_ = true;
    }
    phaseRand_ = param(s, SynPhaseRand);
    if (phaseRand_ > 0) stereo_ = true;
    osc3Mode_ = std::clamp(static_cast<int>(param(s, SynOsc3Mode) + 0.5f), 0, 4);
    osc2Mod_ = std::clamp(static_cast<int>(param(s, SynOsc2Mod) + 0.5f), 0, 4);
    noise_ = param(s, SynNoise);
    atk_ = param(s, SynAttack), dec_ = param(s, SynDecay), sus_ = param(s, SynSustain), rel_ = param(s, SynRelease);
    cutoff_ = param(s, SynCutoff), reso_ = param(s, SynResonance), fenvAmt_ = param(s, SynFilterEnv);
    fdec_ = param(s, SynFilterDecay);
    ftype_ = std::clamp(static_cast<int>(param(s, SynFilterType) + 0.5f), 0, 4);
    velCut_ = param(s, SynVelCutoff), keyTrack_ = param(s, SynKeyTrack);
    lfoTarget_ = std::clamp(static_cast<int>(param(s, SynLfoTarget) + 0.5f), 0, 4);
    lfoShape_ = std::clamp(static_cast<int>(param(s, SynLfoShape) + 0.5f), 0, 4);
    lfoRate_ = param(s, SynLfoRate, 4), lfoAmt_ = param(s, SynLfoAmount, 0.2f);
    if (lfoTarget_ == 4 && lfoAmt_ > 0) stereo_ = true;
    glide_ = param(s, SynGlide);
    maxVoices_ = std::clamp(static_cast<int>(param(s, SynVoices, 16) + 0.5f), 1, kMaxVoices);
    volume_ = param(s, SynVolume);
    // J-65: the reference oscillator model (tables prepared by the synths pack on the UI thread).
    refTab_ = std::dynamic_pointer_cast<const packs::synths::trioref::Tables>(aux.custom);
    refSetup_ = packs::synths::trioref::setup(s, sr_);
    const bool ref = param(s, SynModel) >= 0.5f && refTab_ && !refSetup_.custom;
    if (ref != ref_)
      for (auto& v : voices_) v.amp.stage = Adsr::Idle, v.amp.level = 0, v.rOn = false;
    ref_ = ref;
    for (auto& v : voices_) {
      v.amp.set(sr_, atk_, dec_, sus_, rel_);
      v.fenv.set(sr_, 0.001, fdec_, 0.0, fdec_);
    }
  }

  void startOscs(Voice& v, uint32_t id) {
    for (int sd = 0; sd < 2; ++sd)
      for (int k = 0; k < 3; ++k) {
        Osc& o = v.osc[sd][k];
        o.triState = 0;
        o.phase = sd == 1 ? phaseOfs_[k] : 0.0;
        if (phaseRand_ > 0) {
          rng_.seed(id * 2246822519u + static_cast<uint32_t>(sd * 3 + k) * 97u + 11u);
          o.phase += (rng_.next() * 0.5 + 0.5) * phaseRand_;
        }
        o.phase -= std::floor(o.phase);
      }
  }

  // J-65: reference oscillators — one voice per note (the oldest is taken over
  // when all are busy), the host's note volume (11^(v/100) − 1) / 10 for
  // velocity v/128 and constant-power pan.
  void refNoteOn(uint32_t id, double pitch, float vel, float pan, double modX = 0, double modY = 0) {
    ++clock_;
    Voice* pick = nullptr;
    for (auto& v : voices_)
      if (!v.rOn) {
        pick = &v;
        break;
      }
    if (!pick)
      for (auto& v : voices_)
        if (!pick || v.age < pick->age) pick = &v;
    Voice& v = *pick;
    v.id = id, v.pitch = v.target = pitch, v.vel = vel, v.held = true, v.age = clock_;
    v.rOn = true, v.relK = -1, v.ctr = 0;
    packs::synths::trioref::start(v.rv, refSetup_);
    const double vol = (std::pow(11.0, std::nearbyint(static_cast<double>(vel) * 128.0) / 100.0) - 1.0) / 10.0;
    const long double a = (1.0L + static_cast<long double>(std::clamp(pan, -1.0f, 1.0f))) * static_cast<long double>(0.7853981633974483);
    v.hostL = static_cast<float>(std::cos(a) * static_cast<long double>(vol));
    v.hostR = static_cast<float>(std::sin(a) * static_cast<long double>(vol));
    v.slide = PitchGlide{};
    v.ins.start(insSet_.get(), sr_, tempo_, ppq_, modX, modY, songTick());
    // J-72: the channel envelopes take over the host note gains (constant-power
    // law of note pan + pan modulation, note volume × volume envelope).
    if (v.ins.active()) v.ins.setHostGains(std::clamp(pan, -1.0f, 1.0f), static_cast<float>(vol)), v.ins.setNote(pitch, vel);
  }

  void onNoteOn(uint32_t id, double pitch, float vel) override {
    if (ref_) return refNoteOn(id, pitch, vel, 0.f);
    ++clock_;
    if (maxVoices_ == 1) {  // mono / legato with glide
      Voice& v = voices_[0];
      bool legato = v.held && v.amp.active();
      v.id = id;
      v.target = pitch;
      if (!legato && !v.amp.active()) v.pitch = pitch;
      if (!legato) {
        if (v.amp.level <= 0.0) startOscs(v, id);
        v.amp.gate(true);
        v.fenv.gate(true);
        v.lfoPh = 0;
      }
      v.vel = vel;
      v.held = true;
      v.age = clock_;
      v.npL = v.npR = 1, v.cutOct = v.resoAdd = 0;
      v.slide = PitchGlide{};
      if (!legato) v.ins.start(insSet_.get(), sr_, tempo_, ppq_, 0, 0, songTick()), v.ins.setNote(pitch, vel);
      return;
    }
    Voice* pick = nullptr;
    for (int i = 0; i < maxVoices_; ++i)
      if (!voices_[static_cast<size_t>(i)].amp.active()) {
        pick = &voices_[static_cast<size_t>(i)];
        break;
      }
    if (!pick) {  // steal: prefer released voices, then the oldest
      for (int i = 0; i < maxVoices_; ++i) {
        Voice& v = voices_[static_cast<size_t>(i)];
        if (!pick || (!v.held && pick->held) || (v.held == pick->held && v.age < pick->age)) pick = &v;
      }
    }
    Voice& v = *pick;
    v.id = id;
    v.target = pitch;
    v.pitch = glide_ > 0.002 && lastPitch_ >= 0 ? lastPitch_ : pitch;
    v.vel = vel;
    v.held = true;
    v.age = clock_;
    if (v.amp.level <= 0.0) {
      startOscs(v, id);
      for (auto& sd : v.filt)
        for (auto& f : sd) f.reset();
    }
    v.noise.seed(id * 2654435761u + 1);
    v.npL = v.npR = 1, v.cutOct = v.resoAdd = 0;
    v.slide = PitchGlide{};
    v.ctr = 0;
    v.lfoPh = 0;
    v.amp.gate(true);
    v.fenv.level = 0;
    v.fenv.gate(true);
    v.ins.start(insSet_.get(), sr_, tempo_, ppq_, 0, 0, songTick());
    v.ins.setNote(pitch, vel);
    lastPitch_ = pitch;
  }

  bool supportsGlide() const override { return true; }
  bool handlesNotePan() const override { return true; }
  void onNoteOnEx(uint32_t id, double pitch, float vel, const NoteProps& np) override {
    // Note mod X / Y reach the channel filter as the reference host passes them: (value − ½) · 2, −1..1.
    if (ref_) return refNoteOn(id, pitch, vel, np.pan, (np.modX - 0.5) * 2.0, (np.modY - 0.5) * 2.0);
    onNoteOn(id, pitch, vel);
    for (auto& v : voices_)
      if (v.id == id && v.held) {
        panGains(std::clamp(static_cast<double>(np.pan), -1.0, 1.0), v.npL, v.npR);
        v.cutOct = (np.modX - 0.5) * 8.0;
        v.resoAdd = np.modY - 0.5;
        if (insSet_) {  // the channel filter takes the note's mod X / Y instead
          v.cutOct = v.resoAdd = 0;
          v.ins.start(insSet_.get(), sr_, tempo_, ppq_, (np.modX - 0.5) * 2.0, (np.modY - 0.5) * 2.0, songTick());
          v.ins.setNote(v.target, v.vel);
        }
      }
  }
  void onGlide(uint32_t id, double pitch, double seconds) override {
    for (auto& v : voices_)
      if (v.id == id && v.amp.active()) {
        v.slide.start(v.pitch, pitch, seconds, sr_);
        v.target = pitch;
      }
  }

  void releaseVoice(Voice& v) {
    v.held = false;
    if (v.rOn) {  // reference: the channel volume envelope ends the voice, else the 10 ms declick
      if (v.ins.active()) v.ins.release();
      if (!(v.ins.active() && insSet_ && insSet_->volumeEnvelope())) v.relK = 0;
      return;
    }
    if (v.ins.active()) {
      v.ins.release();
      if (insSet_ && insSet_->volumeEnvelope()) return;  // the channel volume envelope ends the voice
    }
    v.amp.gate(false);
  }
  void onNoteOff(uint32_t id) override {
    for (auto& v : voices_)
      if (v.held && v.id == id) releaseVoice(v);
  }
  void onAllOff() override {
    for (auto& v : voices_)
      if (v.held) releaseVoice(v);
  }
  void onKill() override {
    for (auto& v : voices_) v = Voice{};
    lastPitch_ = -1;
    refSeed_ = 0;
  }

  double lfoValue(Voice& v) {
    double ph = v.lfoPh;
    switch (lfoShape_) {
      case 1: return ph < 0.5 ? 4 * ph - 1 : 3 - 4 * ph;
      case 2: return ph < 0.5 ? 1 : -1;
      case 3: return 2 * ph - 1;
      case 4: return v.rnd;
      default: return std::sin(kTwoPi * ph);
    }
  }

  // One oscillator sample: band-limited shapes (HQ), the classic naive shapes, or
  // one cycle of the channel sample (custom wave, linear interpolation).
  double oscNext(Osc& o, int k, double dt, Noise& nz) const {
    const int sh = shapeI_[k];
    if (sh == kCustomShape) {
      double out = 0;
      if (custom_) {
        const size_t n = custom_->frames();
        double pos = o.phase * static_cast<double>(n);
        size_t i0 = std::min(n - 1, static_cast<size_t>(pos));
        size_t i1 = i0 + 1 >= n ? 0 : i0 + 1;
        double f = pos - static_cast<double>(i0);
        double a = 0.5 * (custom_->left[i0] + custom_->right[i0]), b = 0.5 * (custom_->left[i1] + custom_->right[i1]);
        out = a + (b - a) * f;
      }
      o.phase += dt;
      if (o.phase >= 1.0) o.phase -= std::floor(o.phase);
      return out;
    }
    if (hq_ || sh == static_cast<int>(Shape::Sine) || sh == static_cast<int>(Shape::Noise)) return o.next(shape_[k], dt, nz);
    const double ph = o.phase;
    double out;
    switch (shape_[k]) {
      case Shape::Saw: out = 2.0 * ph - 1.0; break;
      case Shape::Square: out = ph < 0.5 ? 1.0 : -1.0; break;
      case Shape::Triangle: out = ph < 0.25 ? 4.0 * ph : ph < 0.75 ? 2.0 - 4.0 * ph : 4.0 * ph - 4.0; break;
      case Shape::RoundSaw: {
        constexpr double kRise = 0.4638671875, kFall = 5.0 / 6.0, kK = 22.834;  // fast charge, flat top, low last sixth
        out = ph < kRise ? -1.0 + 2.0 * (1.0 - std::exp(-kK * ph / kRise)) / (1.0 - std::exp(-kK)) : ph < kFall ? 1.0 : -1.0;
        break;
      }
      case Shape::Pulse25: out = (ph < 0.25 ? 1.0 : -1.0) + 0.5; break;
      default: out = 0; break;
    }
    o.phase += dt;
    if (o.phase >= 1.0) o.phase -= std::floor(o.phase);
    return out;
  }

  // One side of one voice: oscillators → modulation → mix. Returns the dry mix.
  double renderSide(Voice& v, int sd) {
    Osc* o = v.osc[sd];
    const double* dt = v.dt[sd];
    double gains[3];
    for (int k = 0; k < 3; ++k) gains[k] = level_[k] * sign_[k] * (sd == 0 ? panL_[k] : panR_[k]);
    // Osc 3 first (it can modulate 1 and 2), then osc 2 (it can modulate osc 1), then osc 1.
    double o3 = (level_[2] > 0 || osc3Mode_ != pp::SynModeNormal) ? oscNext(o[2], 2, dt[2], v.noise) : 0.0;
    double m3 = o3 * level_[2];
    double d1 = dt[0], d2 = dt[1], pm = 0;
    if (osc3Mode_ == pp::SynModeFM) {
      d1 *= std::max(0.0, 1.0 + m3 * 2.0);
      d2 *= std::max(0.0, 1.0 + m3 * 2.0);
    } else if (osc3Mode_ == pp::SynModePM) {
      pm = m3 * 0.5;
    }
    auto runOsc = [&](Osc& osc, int k, double d, double phaseMod) {
      if (phaseMod == 0) return oscNext(osc, k, d, v.noise);
      double keep = osc.phase;
      osc.phase = keep + phaseMod;
      osc.phase -= std::floor(osc.phase);
      double r = oscNext(osc, k, d, v.noise);
      osc.phase = keep + d;
      osc.phase -= std::floor(osc.phase);
      return r;
    };
    double o2 = (level_[1] > 0 || osc2Mod_ != 0) ? runOsc(o[1], 1, d2, pm) : 0.0;
    double m2 = o2 * level_[1];
    double pm1 = pm;
    if (osc2Mod_ == 2) d1 *= std::max(0.0, 1.0 + m2 * 2.0);
    if (osc2Mod_ == 3) pm1 += m2 * 0.5;
    double before = o[0].phase;
    double o1 = level_[0] > 0 || osc2Mod_ != 0 ? runOsc(o[0], 0, d1, pm1) : 0.0;
    if (osc2Mod_ == 4 && o[0].phase < before) o[1].phase = 0;  // hard sync: osc 1 restarts osc 2
    double sum;
    if (osc2Mod_ == 1)
      sum = o1 * gains[0] * (1.0 - level_[1] + level_[1] * o2);  // ring: osc 2 multiplies osc 1
    else if (osc2Mod_ != 0)
      sum = o1 * gains[0];  // osc 2 is a modulator only
    else
      sum = o1 * gains[0] + o2 * gains[1];
    switch (osc3Mode_) {
      case pp::SynModeAM: sum *= 1.0 - m3; break;
      case pp::SynModeRing: sum *= 1.0 - level_[2] + m3; break;
      case pp::SynModeFM:
      case pp::SynModePM: break;
      default: sum += o3 * gains[2]; break;
    }
    return sum;
  }

  // J-65: reference voices — oscillators in chunks on the voice's own 64-frame
  // clock (pitch re-read per chunk), the channel instrument envelopes, the
  // release declick, then the host note gains.
  void renderRef(float* L, float* R, int n) {
    float buf[2 * 64];
    for (auto& v : voices_) {
      if (!v.rOn) continue;
      const bool ins = v.ins.active();
      for (int i = 0; i < n && v.rOn;) {
        int m = std::min(n - i, 64 - static_cast<int>(v.ctr & 63));
        float cents;
        if (ins) {  // a pending tick first: it can move the pitch and end the volume envelope
          v.ins.sync();
          if (v.ins.done() && v.relK < 0) v.relK = 0;  // the volume envelope ended: release declick
          m = std::min(m, v.ins.pitchHold());
          cents = static_cast<float>((v.pitch - 60.0) * 100.0) + v.ins.pitchCents();
        } else {
          cents = static_cast<float>((v.pitch - 60.0) * 100.0);
        }
        packs::synths::trioref::render(v.rv, refSetup_, *refTab_, cents, buf, m, refSeed_);
        for (int j = 0; j < m; ++j) {
          float l = buf[2 * j], r = buf[2 * j + 1];
          float dk = 1.0f;
          if (v.relK >= 0) {
            dk = packs::synths::trioref::declick(v.relK++, sr_);
            if (v.relK >= packs::synths::trioref::declickLength(sr_)) v.rOn = false;
          }
          if (ins) {
            v.ins.processWith(l, r, dk);
            L[i + j] += static_cast<float>(static_cast<double>(l) * volume_);
            R[i + j] += static_cast<float>(static_cast<double>(r) * volume_);
          } else {
            l *= dk, r *= dk;
            L[i + j] += static_cast<float>(static_cast<double>(l * v.hostL) * volume_);
            R[i + j] += static_cast<float>(static_cast<double>(r * v.hostR) * volume_);
          }
          if (v.slide.active()) v.slide.advance(v.pitch);
          if (!v.rOn) break;
        }
        v.ctr += static_cast<uint32_t>(m);
        i += m;
      }
    }
  }

  void render(float* L, float* R, int n) override {
    blockPos_ += n;  // (notes fired before this call already read the position)
    if (ref_) return renderRef(L, R, n);
    const double glideCoef = std::exp(-1.0 / std::max(1.0, glide_ * sr_ * 0.3));
    for (auto& v : voices_) {
      if (!v.amp.active()) continue;
      for (int i = 0; i < n; ++i) {
        // Every 16 samples of the voice's own clock (independent of block size):
        // LFO, pitch glide, oscillator increments, filter coefficients.
        if ((v.ctr++ & 15) == 0) {
          double lfo = 0;
          if (lfoTarget_ > 0 && lfoAmt_ > 0) {
            double before = v.lfoPh;
            v.lfoPh += lfoRate_ * 16.0 / sr_;
            v.lfoPh -= std::floor(v.lfoPh);
            if (v.lfoPh < before) v.rnd = v.noise.next();
            lfo = lfoValue(v) * lfoAmt_;
          }
          v.lfo = lfo;
          double pitchMod = lfoTarget_ == 1 ? lfo * lfo * (lfo < 0 ? -12.0 : 12.0) : 0.0;
          if (v.ins.active()) pitchMod += v.ins.pitchSemis();
          for (int sd = 0; sd < (stereo_ ? 2 : 1); ++sd)
            for (int k = 0; k < 3; ++k)
              v.dt[sd][k] = noteToHz(v.pitch + tune_[k] + pitchMod + (sd == 0 ? -detune_[k] : detune_[k])) / sr_;
          double cut = cutoff_ * std::pow(2.0, fenvAmt_ * v.fenv.level * 6.0 + keyTrack_ * (v.pitch - 60.0) / 12.0 +
                                                    velCut_ * (v.vel - 0.75) * 4.0 + (lfoTarget_ == 2 ? lfo * 4.0 : 0.0) + v.cutOct);
          double rs = std::clamp(reso_ + v.resoAdd, 0.0, 1.0);
          for (auto& sd : v.filt)
            for (auto& f : sd) f.set(sr_, cut, ftype_ == 1 ? rs * 0.7 : rs);
        }
        double a = v.amp.next();
        v.fenv.next();
        double g = a * v.vel * volume_;
        if (lfoTarget_ == 3) g *= 1.0 - (0.5 + 0.5 * v.lfo) * std::min(1.0, lfoAmt_);
        double nz = noise_ > 0 ? noise_ * v.noise.next() : 0.0;
        double outs[2];
        for (int sd = 0; sd < (stereo_ ? 2 : 1); ++sd) {
          double x = (renderSide(v, sd) + nz) * 0.5;
          double band = 0, high = 0;
          double low = v.filt[sd][0].process(x, &band, &high);
          double y;
          switch (ftype_) {
            case 1: y = v.filt[sd][1].process(low); break;
            case 2: y = band; break;
            case 3: y = high; break;
            case 4: y = low + high; break;
            default: y = low; break;
          }
          outs[sd] = y * g;
        }
        if (!stereo_) outs[1] = outs[0];
        if (lfoTarget_ == 4) {
          double pl, pr;
          panGains(v.lfo, pl, pr);
          outs[0] *= pl, outs[1] *= pr;
        }
        if (v.ins.active()) {
          float il = static_cast<float>(outs[0]), ir = static_cast<float>(outs[1]);
          v.ins.process(il, ir);
          outs[0] = il, outs[1] = ir;
          if (!v.held && v.ins.done()) {  // channel volume envelope finished
            v.amp.stage = Adsr::Idle;
            v.amp.level = 0;
          }
        }
        L[i] += static_cast<float>(outs[0] * v.npL);
        R[i] += static_cast<float>(outs[1] * v.npR);
        if (v.slide.active())
          v.slide.advance(v.pitch);
        else
          v.pitch = v.target + (v.pitch - v.target) * glideCoef;
        if (!v.amp.active()) break;
      }
    }
  }

 private:
  static constexpr int kMaxVoices = 32;
  std::array<Voice, kMaxVoices> voices_{};
  double sr_ = 44100;
  static constexpr int kCustomShape = 7;
  Shape shape_[3] = {Shape::Saw, Shape::Square, Shape::Saw};
  int shapeI_[3] = {2, 3, 2};
  bool hq_ = true;
  SamplePtr custom_;
  double level_[3] = {0.8, 0.4, 0}, tune_[3] = {0, 0, 0};
  double panL_[3] = {1, 1, 1}, panR_[3] = {1, 1, 1}, sign_[3] = {1, 1, 1}, phaseOfs_[3] = {0, 0, 0}, detune_[3] = {0, 0, 0};
  double phaseRand_ = 0;
  bool stereo_ = false;
  int osc3Mode_ = 0, osc2Mod_ = 0, ftype_ = 0, lfoTarget_ = 0, lfoShape_ = 0;
  double noise_ = 0, atk_ = 0.005, dec_ = 0.3, sus_ = 0.7, rel_ = 0.25;
  double cutoff_ = 6000, reso_ = 0.2, fenvAmt_ = 0.3, fdec_ = 0.4, glide_ = 0.001, volume_ = 0.7;
  double velCut_ = 0, keyTrack_ = 0, lfoRate_ = 4, lfoAmt_ = 0.2;
  int maxVoices_ = 16;
  uint64_t clock_ = 0;
  double lastPitch_ = -1;
  Noise rng_;
  std::shared_ptr<const InsSettings> insSet_;
  bool ref_ = false;
  packs::synths::trioref::Setup refSetup_;
  std::shared_ptr<const packs::synths::trioref::Tables> refTab_;
  uint32_t refSeed_ = 0;
  double tempo_ = 120, tick0_ = 0;
  int ppq_ = 96;
  int64_t blockPos_ = 0;
  double songTick() const { return tick0_ + blockPos_ * tempo_ * ppq_ / (60.0 * sr_); }
};

// ---------------------------------------------------------------------------
// Sampler: pitched sample playback. Region (start/end), loop points with
// forward or ping-pong looping and loop crossfade, render-time sample
// processing (normalize, DC, polarity, swap, fades), declick modes, time
// modes (J-87: Resample reads through a band-limited windowed-sinc kernel;
// the stretch modes give every voice its own engine/TimeStretch voice, pitch
// independent of speed; a voice without one falls back to the two-head grain
// reader), AHDSR with delay/hold, per-voice filter with its own envelope, LFO,
// pan, glide.
class SamplerGen final : public Generator {
 public:
  ~SamplerGen() override {
    delete pending_.exchange(nullptr);
    delete retired_.exchange(nullptr);
    delete active_;
  }

  // J-87: the stretch voices are built here (UI thread) when the time mode,
  // the voice count or the sample rate change, and handed to the audio thread.
  void setClipOnly(bool clipOnly) override { clipOnly_ = clipOnly; }
  void prepareHost(const PluginState& s, double sampleRate) override {
    delete retired_.exchange(nullptr, std::memory_order_acq_rel);
    const stretch::Mode m = stretch::modeOf(param(s, pp::SmpStretchMode));
    if (clipOnly_) return;  // playlist clips render offline (engine/TimeStretch); no notes to stretch
    const int count = std::clamp(static_cast<int>(param(s, pp::SmpVoices, 32) + 0.5f), 1, kStretchVoices);  // more notes: grains
    if (m == stretch::Mode::Resample) {
      uiMode_ = m;
      return;  // the pool (if any) stays until a stretch mode needs another one
    }
    if (m == uiMode_ && sampleRate == uiRate_ && count == uiCount_) return;
    auto* pool = new StretchPool;
    pool->mode = m;
    pool->rate = sampleRate;
    pool->voices.resize(static_cast<size_t>(count));
    for (auto& v : pool->voices) {
      v = std::make_unique<stretch::Voice>();
      v->prepare(2, sampleRate, m, 8.0, 8.0);
    }
    delete pending_.exchange(pool, std::memory_order_acq_rel);  // a pool never picked up is dropped
    uiMode_ = m, uiRate_ = sampleRate, uiCount_ = count;
  }

  bool active() const override {
    for (const auto& v : voices_)
      if (v.on) return true;
    return false;
  }

 protected:
  struct Voice {
    bool on = false, held = false;
    uint32_t id = 0;
    double pos = 0, rate = 1, dir = 1;
    double note = 60, target = 60;
    float vel = 0;
    uint64_t age = 0;
    Adsr env, fenv;
    int64_t delayLeft = 0, holdLeft = 0;
    Svf filt[2][2];
    uint32_t ctr = 0;
    double lfoPh = 0, lfo = 0, lfoAge = 0;
    // stretch mode
    double src = 0, gph = 0, anchor[2] = {0, 0};
    // J-87: stretch voice (index into the pool, -1 = grains), its source
    // cursor, its last rendered frames, and the tail left after the source ends.
    int sv = -1;
    int64_t si = 0;
    int sdir = 1;
    bool srcDone = false, stopped = false;
    size_t nextOn = 0;  // next onset (index into onsets_)
    int64_t tailLeft = 0;
    float sb[2][16] = {};
    int sbPos = 16;
    float fadeOut = 1, fadeStep = 0;
    // Per-note properties: pan gains, cutoff factor (octaves) and resonance offset.
    double npL = 1, npR = 1, cutOct = 0, resoAdd = 0;
    PitchGlide slide;  // slide notes / porta
  };

  void configure(const PluginState& s, const ChannelAux& aux, const ProcessContext& ctx) override {
    using namespace pp;
    sr_ = ctx.sampleRate;
    interp_ = ctx.interp;
    kernel_ = interp_ >= 2 ? &sincKernel(samplerSincPoints(interp_)) : nullptr;
    if (aux.sample().get() != sample_.get()) {  // new sample: restart the amortised analysis scan
      sample_ = aux.sample();
      scan_ = 0, peak_ = 0, sumL_ = sumR_ = 0, analysed_ = false;
    }
    smpL_ = sample_ ? sample_->left.data() : nullptr;
    smpR_ = sample_ ? sample_->right.data() : nullptr;
    if (sample_ && !analysed_) {  // peak / mean, at most 65536 frames per block
      size_t nf = sample_->frames(), end = std::min(nf, scan_ + 65536);
      for (size_t i = scan_; i < end; ++i) {
        peak_ = std::max(peak_, static_cast<double>(std::max(std::fabs(sample_->left[i]), std::fabs(sample_->right[i]))));
        sumL_ += sample_->left[i], sumR_ += sample_->right[i];
      }
      scan_ = end;
      analysed_ = scan_ >= nf;
    }
    root_ = param(s, SmpRoot, 60);
    fine_ = param(s, SmpFine);
    start_ = param(s, SmpStart);
    end_ = std::max(static_cast<double>(param(s, SmpEnd, 1)), start_ + 0.001);
    reverse_ = param(s, SmpReverse) >= 0.5f;
    loop_ = param(s, SmpLoop) >= 0.5f;
    oneShot_ = param(s, SmpOneShot) >= 0.5f;
    volume_ = param(s, SmpVolume, 0.8f);
    loopStart_ = param(s, SmpLoopStart), loopEnd_ = std::max(static_cast<double>(param(s, SmpLoopEnd, 1)), loopStart_ + 0.001);
    pingPong_ = param(s, SmpLoopMode) >= 0.5f;
    double gainN = param(s, SmpNormalize) >= 0.5f && analysed_ && peak_ > 1e-6 ? 1.0 / peak_ : 1.0;
    gain_ = gainN * (param(s, SmpPolarity) >= 0.5f ? -1.0 : 1.0);
    dcL_ = dcR_ = 0;
    if (param(s, SmpRemoveDc) >= 0.5f && analysed_ && sample_ && sample_->frames())
      dcL_ = sumL_ / static_cast<double>(sample_->frames()), dcR_ = sumR_ / static_cast<double>(sample_->frames());
    swap_ = param(s, SmpSwapStereo) >= 0.5f;
    fadeIn_ = param(s, SmpFadeIn), fadeOutAmt_ = param(s, SmpFadeOut), xfade_ = param(s, SmpLoopXfade);
    declick_ = std::clamp(static_cast<int>(param(s, SmpDeclick) + 0.5f), 0, 4);
    stretchMode_ = stretch::modeOf(param(s, SmpStretchMode));
    onsets_ = aux.slices.data();  // J-87: the sample's onsets (prepared with the aux in Auto / Transient)
    onsetN_ = stretchMode_ == stretch::Mode::Auto || stretchMode_ == stretch::Mode::Transient ? aux.slices.size() : 0;
    stretch_ = stretchMode_ != stretch::Mode::Resample;
    if (StretchPool* p = pending_.load(std::memory_order_acquire)) {  // J-87: a new pool from prepareHost
      if (!active_ || retired_.load(std::memory_order_acquire) == nullptr) {
        pending_.store(nullptr, std::memory_order_release);
        StretchPool* old = active_;
        active_ = p;
        for (auto& v : voices_) v.sv = -1;  // sounding voices finish on the grains
        if (old) retired_.store(old, std::memory_order_release);
      }
    }
    pitchShift_ = param(s, SmpPitchShift);
    timeStretch_ = param(s, SmpTimeStretch, 1);
    delay_ = param(s, SmpEnvDelay), hold_ = param(s, SmpEnvHold);
    ftype_ = std::clamp(static_cast<int>(param(s, SmpFilterType) + 0.5f), 0, 5);
    cutoff_ = param(s, SmpCutoff, 20000), reso_ = param(s, SmpReso), fenvAmt_ = param(s, SmpFiltEnv);
    velCut_ = param(s, SmpVelCutoff), keyCut_ = param(s, SmpKeyCutoff);
    lfoTarget_ = std::clamp(static_cast<int>(param(s, SmpLfoTarget) + 0.5f), 0, 4);
    lfoShape_ = std::clamp(static_cast<int>(param(s, SmpLfoShape) + 0.5f), 0, 2);
    lfoRate_ = param(s, SmpLfoRate, 2), lfoAmt_ = param(s, SmpLfoAmount), lfoDelay_ = param(s, SmpLfoDelay);
    double pl, pr;
    panGains(param(s, SmpPan), pl, pr);
    panL_ = pl, panR_ = pr;
    velVol_ = param(s, SmpVelVolume, 1);
    voices_n_ = std::clamp(static_cast<int>(param(s, SmpVoices, 32) + 0.5f), 1, 32);
    glide_ = param(s, SmpGlide);
    // Declick: minimum attack / release times by mode.
    static const double minAtk[5] = {0, 0, 0, 0.001, 0.005}, minRel[5] = {0, 0.002, 0.005, 0.005, 0.02};
    double atk = std::max(static_cast<double>(param(s, SmpAttack)), minAtk[declick_]);
    double rel = std::max(static_cast<double>(param(s, SmpRelease, 0.02f)), minRel[declick_]);
    // J-30: the envelope coefficients (eight exp()) are designed once and copied
    // to the voices (designed per voice, every block, they were a hot spot of
    // projects with hundreds of samplers); identical values.
    Adsr e, f;
    e.set(sr_, atk, param(s, SmpDecay, 10), param(s, SmpSustain, 1), rel);
    f.set(sr_, param(s, SmpFiltAttack, 0.001f), param(s, SmpFiltDecay, 0.3f), param(s, SmpFiltSustain), param(s, SmpFiltRelease, 0.3f));
    for (auto& v : voices_) {
      v.env.atkInc = e.atkInc, v.env.decCoef = e.decCoef, v.env.relCoef = e.relCoef, v.env.sustain = e.sustain;
      v.fenv.atkInc = f.atkInc, v.fenv.decCoef = f.decCoef, v.fenv.relCoef = f.relCoef, v.fenv.sustain = f.sustain;
    }
  }

  double regionLo() const { return start_ * static_cast<double>(sample_->frames() - 1); }
  double regionHi() const { return std::min(end_, 1.0) * static_cast<double>(sample_->frames() - 1); }

  bool supportsGlide() const override { return true; }
  bool handlesNotePan() const override { return true; }

  void onNoteOnEx(uint32_t id, double pitch, float vel, const NoteProps& np) override {
    onNoteOn(id, pitch, vel);
    if (Voice* v = find(id)) {
      dsp::panGains(std::clamp(static_cast<double>(np.pan), -1.0, 1.0), v->npL, v->npR);
      v->cutOct = (np.modX - 0.5) * 8.0;  // mod X: cutoff ±4 octaves
      v->resoAdd = (np.modY - 0.5) * 1.0;  // mod Y: resonance ±0.5
    }
  }
  Voice* find(uint32_t id) {
    for (auto& v : voices_)
      if (v.on && v.id == id) return &v;
    return nullptr;
  }
  void onGlide(uint32_t id, double pitch, double seconds) override {
    if (Voice* v = find(id)) {
      v->slide.start(v->note, pitch, seconds, sr_);
      v->target = pitch;
    }
  }
  void onCut(uint32_t id) override {
    for (auto& v : voices_)
      if (v.on && v.id == id && v.fadeStep == 0) {
        v.held = false;
        v.fadeStep = static_cast<float>(1.0 / std::max(1.0, 0.004 * sr_));  // 4 ms choke
      }
  }

  void onNoteOn(uint32_t id, double pitch, float vel) override {
    if (!sample_ || sample_->frames() < 4) return;
    ++clock_;
    Voice* pick = nullptr;
    if (voices_n_ == 1) {
      Voice& v = voices_[0];
      if (v.on && v.held && glide_ > 0.002) {  // mono legato: glide, no retrigger
        v.target = pitch;
        v.id = id;
        return;
      }
      pick = &v;
    } else {
      for (int i = 0; i < voices_n_; ++i)
        if (!voices_[static_cast<size_t>(i)].on) {
          pick = &voices_[static_cast<size_t>(i)];
          break;
        }
      if (!pick)
        for (int i = 0; i < voices_n_; ++i)
          if (!pick || voices_[static_cast<size_t>(i)].age < pick->age) pick = &voices_[static_cast<size_t>(i)];
    }
    Voice& v = *pick;
    v.on = true;
    v.held = true;
    v.id = id;
    v.vel = vel;
    v.age = clock_;
    v.note = v.target = pitch;
    v.dir = reverse_ ? -1 : 1;
    v.pos = reverse_ ? regionHi() : regionLo();
    v.src = v.pos;
    v.gph = 0;
    v.anchor[0] = v.anchor[1] = v.pos;
    v.env.level = 0;
    v.delayLeft = static_cast<int64_t>(delay_ * sr_);
    v.holdLeft = static_cast<int64_t>(hold_ * sr_);
    if (v.delayLeft <= 0) v.env.gate(true);
    v.fenv.level = 0;
    v.fenv.gate(true);
    for (auto& sd : v.filt)
      for (auto& f : sd) f.reset();
    v.ctr = 0;
    v.lfoPh = 0, v.lfoAge = 0;
    v.fadeOut = 1, v.fadeStep = 0;
    v.npL = v.npR = 1, v.cutOct = v.resoAdd = 0;
    v.slide = PitchGlide{};
    // J-87: stretch voice for this note (same slot in the pool).
    v.sv = -1;
    v.srcDone = false;
    v.tailLeft = 0;
    v.sbPos = 16;
    const auto slot = static_cast<size_t>(pick - voices_.data());
    if (stretch_ && active_ && active_->mode == stretchMode_ && active_->rate == sr_ && slot < active_->voices.size()) {
      v.sv = static_cast<int>(slot);
      v.si = static_cast<int64_t>(std::llround(v.pos));
      v.sdir = reverse_ ? -1 : 1;
      v.stopped = false;
      v.nextOn = 0;
      while (v.nextOn < onsetN_ && onsets_[v.nextOn] <= v.si) ++v.nextOn;
      ReadCtx rc{this, &v};
      active_->voices[slot]->start(&SamplerGen::readSource, &rc, stretchSpeed(), stretchPitch(v));
    }
  }
  void onNoteOff(uint32_t id) override {
    if (oneShot_) return;
    for (auto& v : voices_)
      if (v.on && v.held && v.id == id) {
        v.held = false;
        v.env.gate(false);
        v.fenv.gate(false);
        if (v.delayLeft > 0) v.on = false;
      }
  }
  void onAllOff() override {
    for (auto& v : voices_)
      if (v.on) {
        v.held = false;
        v.env.gate(false);
        v.fenv.gate(false);
      }
  }
  void onKill() override {
    for (auto& v : voices_) v = Voice{};
  }

  // Reads the sample at a fractional frame with the render-time processing applied.
  void read(double p, double& l, double& r) const {
    const float* sl = smpL_;  // raw channel pointers, set in configure (J-30: no per-read
    const float* sr = smpR_;  // shared-channel test)
    double len = static_cast<double>(sample_->left.size());
    p = std::clamp(p, 0.0, len - 2);
    size_t p0 = static_cast<size_t>(p);
    double fr = p - static_cast<double>(p0);
    if (fr < 1e-9 && rate_ <= 1.000001) {  // on a sample, not reading faster: exact
      l = (sl[p0] - dcL_) * gain_;
      r = (sr[p0] - dcR_) * gain_;
    } else if (interp_ == 0) {
      // J-87: band-limited windowed sinc, cutoff following the read rate
      // (pitching up never aliases); was linear interpolation.
      float a, b;
      stretch::readSinc2(sl, sr, static_cast<int64_t>(sample_->left.size()), p, rate_, *bl_, a, b);
      l = (a - dcL_) * gain_;
      r = (b - dcR_) * gain_;
    } else if (interp_ >= 2 && rate_ > 1.0) {  // offline sinc qualities: band-limited when reading faster
      float a, b;
      stretch::readSinc2(sl, sr, static_cast<int64_t>(sample_->left.size()), p, rate_, *blOff_, a, b);
      l = (a - dcL_) * gain_;
      r = (b - dcR_) * gain_;
    } else {
      interpolate(p0, fr, l, r);
      l = (l - dcL_) * gain_;
      r = (r - dcR_) * gain_;
    }
    if (swap_) std::swap(l, r);
    if (fadeIn_ <= 0 && fadeOutAmt_ <= 0) return;  // J-30: no region fades, no per-sample division (gain 1)
    double lo = regionLo(), hi = regionHi(), span = std::max(1.0, hi - lo);
    double rel = (p - lo) / span;
    double g = 1;
    if (fadeIn_ > 0 && rel < fadeIn_) g *= std::max(0.0, rel / fadeIn_);
    if (fadeOutAmt_ > 0 && rel > 1 - fadeOutAmt_) g *= std::max(0.0, (1 - rel) / fadeOutAmt_);
    l *= g, r *= g;
  }

  // High-quality reads for offline renders (RenderInterp): 4-point Hermite,
  // or a windowed-sinc kernel (sample ends clamp).
  void interpolate(size_t p0, double fr, double& l, double& r) const {
    const auto& sl = sample_->left;
    const auto& sr = sample_->right;
    const auto last = static_cast<int64_t>(sl.size()) - 1;
    auto at = [last](int64_t i) { return static_cast<size_t>(std::clamp<int64_t>(i, 0, last)); };
    const auto i0 = static_cast<int64_t>(p0);
    if (!kernel_) {  // Hermite (Catmull-Rom)
      auto herm = [fr](double xm1, double x0, double x1, double x2) {
        const double c1 = 0.5 * (x1 - xm1), c2 = xm1 - 2.5 * x0 + 2 * x1 - 0.5 * x2, c3 = 0.5 * (x2 - xm1) + 1.5 * (x0 - x1);
        return ((c3 * fr + c2) * fr + c1) * fr + x0;
      };
      l = herm(sl[at(i0 - 1)], sl[at(i0)], sl[at(i0 + 1)], sl[at(i0 + 2)]);
      r = herm(sr[at(i0 - 1)], sr[at(i0)], sr[at(i0 + 1)], sr[at(i0 + 2)]);
      return;
    }
    const SincKernel& k = *kernel_;
    const double ph = fr * k.phases;
    const int r0 = std::min(k.phases - 1, static_cast<int>(ph));
    const float w1 = static_cast<float>(ph - r0);
    const float* a = k.row(r0);
    const float* b = k.row(r0 + 1);
    const int64_t first = i0 - k.points / 2 + 1;
    double accL = 0, accR = 0;
    if (first >= 0 && first + k.points - 1 <= last) {
      for (int t = 0; t < k.points; ++t) {
        const double w = a[t] + (b[t] - a[t]) * w1;
        accL += w * sl[static_cast<size_t>(first + t)];
        accR += w * sr[static_cast<size_t>(first + t)];
      }
    } else {
      for (int t = 0; t < k.points; ++t) {
        const double w = a[t] + (b[t] - a[t]) * w1;
        accL += w * sl[at(first + t)];
        accR += w * sr[at(first + t)];
      }
    }
    l = accL;
    r = accR;
  }

  // Read with the loop crossfade applied (forward loops only).
  void readLooped(double p, double& l, double& r) const {
    read(p, l, r);
    if (!loop_ || pingPong_ || xfade_ <= 0) return;
    double ls = loopStart_ * static_cast<double>(sample_->frames() - 1), le = loopEnd_ * static_cast<double>(sample_->frames() - 1);
    double x = xfade_ * (le - ls);
    if (x < 2 || p < le - x || p >= le) return;
    double w = (p - (le - x)) / x, l2, r2;
    read(p - (le - ls), l2, r2);
    l = l * (1 - w) + l2 * w;
    r = r * (1 - w) + r2 * w;
  }

  void render(float* L, float* R, int n) override {
    if (!sample_) return;
    const double base = sample_->sampleRate / sr_;
    const double len = static_cast<double>(sample_->frames());
    const double lo = regionLo(), hi = std::max(lo + 1, regionHi());  // start offset at (or past) the end: keep lo <= hi
    const double ls = std::clamp(loopStart_ * (len - 1), lo, hi - 1), le = std::clamp(loopEnd_ * (len - 1), ls + 1, hi);
    const double glideCoef = std::exp(-1.0 / std::max(1.0, glide_ * sr_ * 0.3));
    const double grainLen = 0.05 * sr_;
    static const double minEnd[5] = {0, 0.002, 0.005, 0.005, 0.02};
    const double endFade = minEnd[declick_] * sr_;
    for (auto& v : voices_) {
      if (!v.on) continue;
      for (int i = 0; i < n; ++i) {
        if (v.delayLeft > 0) {
          if (--v.delayLeft == 0) v.env.gate(v.held || oneShot_);
          continue;
        }
        if ((v.ctr++ & 15) == 0) {
          if (lfoTarget_ > 0 && lfoAmt_ != 0) {
            v.lfoAge += 16.0 / sr_;
            v.lfoPh += lfoRate_ * 16.0 / sr_;
            v.lfoPh -= std::floor(v.lfoPh);
            double sh = lfoShape_ == 1 ? (v.lfoPh < 0.5 ? 4 * v.lfoPh - 1 : 3 - 4 * v.lfoPh)
                        : lfoShape_ == 2 ? (v.lfoPh < 0.5 ? 1.0 : -1.0)
                                         : std::sin(kTwoPi * v.lfoPh);
            double fadeIn = lfoDelay_ > 0 ? std::clamp((v.lfoAge - lfoDelay_) / std::max(0.05, lfoDelay_ * 0.5), 0.0, 1.0) : 1.0;
            v.lfo = sh * lfoAmt_ * fadeIn;
          } else {
            v.lfo = 0;
          }
          double semis = v.note - root_ + fine_ / 100.0 + (lfoTarget_ == 4 ? v.lfo * 12.0 : 0.0);
          v.rate = std::pow(2.0, semis / 12.0) * base;
          if (ftype_ > 0) {
            double cut = cutoff_ * std::pow(2.0, fenvAmt_ * v.fenv.level * 6.0 + velCut_ * (v.vel - 0.75) * 4.0 +
                                                      keyCut_ * (v.note - root_) / 12.0 + (lfoTarget_ == 3 ? v.lfo * 4.0 : 0.0) + v.cutOct);
            double rs = std::clamp(reso_ + v.resoAdd, 0.0, 1.0);
            for (auto& sd : v.filt)
              for (auto& f : sd) f.set(sr_, cut, ftype_ == 2 ? rs * 0.7 : rs);
          }
        }
        double l, r;
        if (stretch_ && v.sv >= 0 && active_ && static_cast<size_t>(v.sv) < active_->voices.size()) {
          // J-87: the voice's stretcher, 16 frames at a time (the LFO / glide rate).
          if (v.sbPos >= 16) {
            float* o[2] = {v.sb[0], v.sb[1]};
            ReadCtx rc{this, &v};
            active_->voices[static_cast<size_t>(v.sv)]->render(o, 16, stretchSpeed(), stretchPitch(v), 0.0, &SamplerGen::readSource, &rc);
            v.sbPos = 0;
          }
          l = v.sb[0][v.sbPos];
          r = v.sb[1][v.sbPos];
          ++v.sbPos;
          if (v.srcDone && !loop_) {
            // The stretched tail: as long as the engine still holds input.
            if (v.tailLeft == 0) {
              const double sp = std::max(1e-3, stretchSpeed());
              v.tailLeft = static_cast<int64_t>(active_->voices[static_cast<size_t>(v.sv)]->lookahead(sp, stretchPitch(v)) / sp) + 1;
            }
            if (--v.tailLeft <= 0) {
              v.on = false;
              break;
            }
          }
        } else if (stretch_) {
          // Source advances at the stretched speed; two grains read at the pitch ratio.
          double pitchRatio = std::pow(2.0, (v.note - root_ + fine_ / 100.0 + pitchShift_ / 100.0 + (lfoTarget_ == 4 ? v.lfo * 12.0 : 0.0)) / 12.0);
          v.src += v.dir * base / timeStretch_;
          if (loop_ && v.dir > 0 && v.src >= le) v.src -= le - ls;
          if ((!loop_) && (v.src >= hi || v.src < lo)) {
            v.on = false;
            break;
          }
          double prev = v.gph;
          v.gph += 1.0 / grainLen;
          v.gph -= std::floor(v.gph);
          for (int h = 0; h < 2; ++h) {
            double t = v.gph + 0.5 * h;
            t -= std::floor(t);
            double tp = prev + 0.5 * h;
            tp -= std::floor(tp);
            if (t < tp) v.anchor[h] = v.src;  // grain restarts at the current source position
          }
          l = r = 0;
          for (int h = 0; h < 2; ++h) {
            double t = v.gph + 0.5 * h;
            t -= std::floor(t);
            double w = std::sin(kPi * t);
            w *= w;
            double gl, gr;
            readLooped(v.anchor[h] + v.dir * t * grainLen * pitchRatio * base, gl, gr);
            l += gl * w, r += gr * w;
          }
        } else {
          if (v.dir > 0 && v.pos >= (loop_ ? le : hi)) {
            if (loop_ && pingPong_) {
              v.dir = -1, v.pos = 2 * le - v.pos;
            } else if (loop_) {
              v.pos -= le - ls;
            } else {
              v.on = false;
              break;
            }
          } else if (v.dir < 0 && v.pos <= (loop_ ? ls : lo)) {
            if (loop_ && (pingPong_ || reverse_)) {
              if (pingPong_)
                v.dir = 1, v.pos = 2 * ls - v.pos;
              else
                v.pos += le - ls;
            } else {
              v.on = false;
              break;
            }
          }
          rate_ = std::fabs(v.rate);
          readLooped(v.pos, l, r);
          v.pos += v.dir * v.rate;
        }
        // Declick near the end of a non-looping region.
        if (!loop_ && endFade > 0 && v.sv < 0) {
          double left = v.dir > 0 ? hi - (stretch_ ? v.src : v.pos) : (stretch_ ? v.src : v.pos) - lo;
          if (left < endFade) {
            double g = std::max(0.0, left / endFade);
            l *= g, r *= g;
          }
        }
        double a;
        if (v.holdLeft > 0 && v.env.stage == Adsr::Decay) {  // AHDSR: hold the peak
          --v.holdLeft;
          a = v.env.level;
        } else {
          a = v.env.next();
        }
        v.fenv.next();
        if (!v.env.active()) {
          v.on = false;
          break;
        }
        if (ftype_ > 0) {
          double io[2] = {l, r};
          for (int c = 0; c < 2; ++c) {
            double band = 0, high = 0, low = v.filt[c][0].process(io[c], &band, &high), y;
            switch (ftype_) {
              case 2: y = v.filt[c][1].process(low); break;
              case 3: y = band; break;
              case 4: y = high; break;
              case 5: y = low + high; break;
              default: y = low; break;
            }
            io[c] = y;
          }
          l = io[0], r = io[1];
        }
        double g = a * (1.0 - velVol_ + velVol_ * v.vel) * volume_;
        if (v.fadeStep > 0) {  // cut: short fade, then the voice ends
          g *= v.fadeOut;
          v.fadeOut -= v.fadeStep;
          if (v.fadeOut <= 0) {
            v.on = false;
            break;
          }
        }
        double pl = panL_ * v.npL, pr = panR_ * v.npR;
        if (lfoTarget_ == 1) g *= 1.0 - 0.5 * std::fabs(v.lfo) - 0.5 * v.lfo;
        if (lfoTarget_ == 2) {
          panGains(std::clamp(v.lfo, -1.0, 1.0), pl, pr);
          pl *= v.npL, pr *= v.npR;
        }
        L[i] += static_cast<float>(l * g * pl);
        R[i] += static_cast<float>(r * g * pr);
        if (v.slide.active())
          v.slide.advance(v.note);
        else
          v.note = v.target + (v.note - v.target) * glideCoef;
      }
    }
  }

 private:
  // ---- J-87 stretch voices
  static constexpr int kStretchVoices = 8;
  struct StretchPool {
    stretch::Mode mode = stretch::Mode::Auto;
    double rate = 0;
    std::vector<std::unique_ptr<stretch::Voice>> voices;
  };
  std::atomic<StretchPool*> pending_{nullptr};  // UI → audio thread
  std::atomic<StretchPool*> retired_{nullptr};  // audio thread → UI (freed by the next prepareHost)
  StretchPool* active_ = nullptr;               // audio thread
  stretch::Mode uiMode_ = stretch::Mode::Resample, stretchMode_ = stretch::Mode::Resample;
  bool clipOnly_ = false;  // UI thread
  const int64_t* onsets_ = nullptr;
  size_t onsetN_ = 0;
  double uiRate_ = 0;
  int uiCount_ = 0;
  struct ReadCtx {
    SamplerGen* g;
    Voice* v;
  };
  // Source frames per output frame (time stretch) and pitch ratio (note,
  // fine tune, pitch shift, pitch LFO; the sample rate is folded in).
  double stretchSpeed() const { return sample_ ? sample_->sampleRate / sr_ / std::max(0.01, timeStretch_) : 1.0; }
  double stretchPitch(const Voice& v) const {
    const double base = sample_ ? sample_->sampleRate / sr_ : 1.0;
    return std::pow(2.0, (v.note - root_ + fine_ / 100.0 + pitchShift_ / 100.0 + (lfoTarget_ == 4 ? v.lfo * 12.0 : 0.0)) / 12.0) * base;
  }
  // Reader for the stretch voice: the region frame by frame, loops (forward,
  // ping-pong), reverse, the render-time processing of read(); silence at the end.
  // Transients (Auto / Transient modes, no loop, not reversed): the reader
  // stops at each onset of the sample (aux.slices) so the voice restarts its
  // engine there; resume continues from it.
  static int readSource(void* ctx, float* const* d, int frames, bool resume) {
    auto* rc = static_cast<ReadCtx*>(ctx);
    SamplerGen& g = *rc->g;
    Voice& v = *rc->v;
    const bool onsets = g.onsetN_ > 0 && !g.loop_ && v.sdir > 0;
    if (resume) {
      v.stopped = false;
      while (v.nextOn < g.onsetN_ && g.onsets_[v.nextOn] <= v.si) ++v.nextOn;
    }
    int hit = -1;
    const double len = static_cast<double>(g.sample_->frames());
    const double lo = g.regionLo(), hi = std::max(lo + 1, g.regionHi());  // keep lo <= hi (start offset at the end)
    const auto ls = static_cast<int64_t>(std::clamp(g.loopStart_ * (len - 1), lo, hi - 1));
    const auto le = std::max(ls + 1, static_cast<int64_t>(std::clamp(g.loopEnd_ * (len - 1), static_cast<double>(ls) + 1, hi)));
    const auto ilo = static_cast<int64_t>(std::ceil(lo)), ihi = static_cast<int64_t>(std::floor(hi));
    g.rate_ = 1.0;
    for (int k = 0; k < frames; ++k) {
      if (v.srcDone || v.stopped) {
        d[0][k] = d[1][k] = 0.0f;
        continue;
      }
      if (onsets && v.nextOn < g.onsetN_ && v.si >= g.onsets_[v.nextOn]) {
        v.stopped = true;
        hit = k;
        d[0][k] = d[1][k] = 0.0f;
        continue;
      }
      if (g.loop_) {
        if (v.sdir > 0 && v.si >= le) {
          if (g.pingPong_)
            v.sdir = -1, v.si = 2 * le - v.si;
          else
            v.si -= le - ls;
        } else if (v.sdir < 0 && v.si <= ls && (g.pingPong_ || g.reverse_)) {
          if (g.pingPong_)
            v.sdir = 1, v.si = 2 * ls - v.si;
          else
            v.si += le - ls;
        }
      }
      if (v.si > ihi || v.si < ilo) {
        v.srcDone = true;
        d[0][k] = d[1][k] = 0.0f;
        continue;
      }
      double l, r;
      g.readLooped(static_cast<double>(v.si), l, r);
      d[0][k] = static_cast<float>(l);
      d[1][k] = static_cast<float>(r);
      v.src = static_cast<double>(v.si);
      v.si += v.sdir;
    }
    return hit;
  }

  std::array<Voice, 32> voices_{};
  SamplePtr sample_;
  const float* smpL_ = nullptr;  // sample_ channel data (read)
  const float* smpR_ = nullptr;
  size_t scan_ = 0;
  double peak_ = 0, sumL_ = 0, sumR_ = 0;
  bool analysed_ = false;
  double sr_ = 44100, root_ = 60, fine_ = 0, start_ = 0, end_ = 1, volume_ = 0.8;
  double loopStart_ = 0, loopEnd_ = 1, gain_ = 1, dcL_ = 0, dcR_ = 0, fadeIn_ = 0, fadeOutAmt_ = 0, xfade_ = 0;
  int interp_ = 0;                        // offline render interpolation (ProcessContext::interp)
  double rate_ = 1;                       // J-87: current read rate (resample reads; cutoff)
  const stretch::SincTable* bl_ = &stretch::sincTable(stretch::Quality::Realtime);
  const stretch::SincTable* blOff_ = &stretch::sincTable(stretch::Quality::Offline);
  const SincKernel* kernel_ = nullptr;    // sinc qualities
  double pitchShift_ = 0, timeStretch_ = 1, delay_ = 0, hold_ = 0;
  double cutoff_ = 20000, reso_ = 0, fenvAmt_ = 0, velCut_ = 0, keyCut_ = 0, lfoRate_ = 2, lfoAmt_ = 0, lfoDelay_ = 0;
  double panL_ = 1, panR_ = 1, velVol_ = 1, glide_ = 0.001;
  int declick_ = 0, ftype_ = 0, lfoTarget_ = 0, lfoShape_ = 0, voices_n_ = 32;
  bool reverse_ = false, loop_ = false, oneShot_ = true, pingPong_ = false, swap_ = false, stretch_ = false;
  uint64_t clock_ = 0;
};

// ---------------------------------------------------------------------------
// Drum synths share a one-shot voice pool; note-offs are ignored.
template <typename Voice>
class DrumGen : public Generator {
 public:
  bool active() const override {
    for (const auto& v : voices_)
      if (v.on) return true;
    return false;
  }

 protected:
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
        if (v.t > pick->t) pick = &v;
    }
    *pick = Voice{};
    pick->on = true;
    pick->vel = vel;
    pick->tune = std::pow(2.0, (pitch - 60.0) / 12.0);
    pick->noise.seed(id * 747796405u + 2891336453u);
    pick->rnd1 = pick->noise.next();
    pick->rnd2 = pick->noise.next();
    started(*pick);
  }
  virtual void started(Voice&) {}
  void onNoteOff(uint32_t) override {}
  void onAllOff() override {}
  void onKill() override {
    for (auto& v : voices_) v = Voice{};
  }
  void out(float* L, float* R, int i, double s) const {
    L[i] += static_cast<float>(s * panL_);
    R[i] += static_cast<float>(s * panR_);
  }
  void setPan(double pan) { panGains(pan, panL_, panR_); }
  std::array<Voice, 8> voices_{};
  double sr_ = 44100, panL_ = 1, panR_ = 1;
};

struct DrumVoiceBase {
  bool on = false;
  float vel = 0;
  double t = 0, tune = 1, phase = 0;
  double rnd1 = 0, rnd2 = 0;
  Noise noise;
};

// ---------------------------------------------------------------------------
struct KickVoice : DrumVoiceBase {
  double phase2 = 0;
  Svf clickF;
  OnePole tone;
};
class KickGen final : public DrumGen<KickVoice> {
 protected:
  void configure(const PluginState& s, const ChannelAux&, const ProcessContext& ctx) override {
    using namespace pp;
    sr_ = ctx.sampleRate;
    p0_ = param(s, KickPitch), p1_ = param(s, KickEndPitch), sweep_ = param(s, KickSweep);
    decay_ = param(s, KickDecay), click_ = param(s, KickClick), drive_ = param(s, KickDrive), vol_ = param(s, KickVolume);
    keyTrack_ = param(s, KickKeyTrack, 1) >= 0.5f;
    sub_ = param(s, KickSub), clickTone_ = param(s, KickClickTone, 4000), clickLen_ = param(s, KickClickLen, 0.004f);
    attack_ = param(s, KickAttack), tone_ = param(s, KickTone, 20000), variation_ = param(s, KickVariation);
    noiseLvl_ = param(s, KickNoise), noiseDec_ = param(s, KickNoiseDecay, 0.08f);
    linear_ = param(s, KickSweepCurve) >= 0.5f;
  }
  void render(float* L, float* R, int n) override {
    const double dt = 1.0 / sr_;
    for (auto& v : voices_) {
      if (!v.on) continue;
      if (v.t == 0) {
        v.clickF.set(sr_, clickTone_, 0.1);
        v.tone.setFreq(sr_, tone_);
      }
      double tune = keyTrack_ ? v.tune : 1.0;
      double p0 = p0_ * (1 + 0.1 * variation_ * v.rnd1), sweep = sweep_ * (1 + 0.3 * variation_ * v.rnd2);
      for (int i = 0; i < n; ++i) {
        double env = linear_ ? std::max(0.0, 1.0 - v.t / (sweep * 3.0)) : std::exp(-v.t / sweep);
        double f = (p1_ + (p0 - p1_) * env) * tune;
        v.phase += f * dt;
        v.phase2 += p1_ * tune * 0.5 * dt;
        double amp = std::exp(-v.t / (decay_ * 0.35));
        if (attack_ > 0 && v.t < attack_) amp *= v.t / attack_;
        double s = std::sin(kTwoPi * v.phase) * amp;
        if (sub_ > 0) s += sub_ * std::sin(kTwoPi * v.phase2) * amp;
        double nz = v.noise.next();
        if (v.t < clickLen_) {
          double band = 0;
          v.clickF.process(nz, &band);
          s += click_ * (nz * 0.5 + band) * (1.0 - v.t / clickLen_);
        }
        if (noiseLvl_ > 0) s += noiseLvl_ * nz * 0.5 * std::exp(-v.t / noiseDec_);
        double d = 1.0 + drive_ * 8.0;
        s = std::tanh(s * d) / std::tanh(d);
        if (tone_ < 19900) s = v.tone.process(s);
        out(L, R, i, s * v.vel * vol_);
        v.t += dt;
        if (amp < 1e-4 && v.t > attack_) {
          v.on = false;
          break;
        }
      }
    }
  }
  double p0_ = 220, p1_ = 48, sweep_ = 0.06, decay_ = 0.45, click_ = 0.3, drive_ = 0.2, vol_ = 0.9;
  double sub_ = 0, clickTone_ = 4000, clickLen_ = 0.004, attack_ = 0, tone_ = 20000, variation_ = 0, noiseLvl_ = 0, noiseDec_ = 0.08;
  bool keyTrack_ = true, linear_ = false;
};

// ---------------------------------------------------------------------------
struct SnareVoice : DrumVoiceBase {
  double phase2 = 0;
  Svf hp;
  Biquad tilt;
};
class SnareGen final : public DrumGen<SnareVoice> {
 protected:
  void configure(const PluginState& s, const ChannelAux&, const ProcessContext& ctx) override {
    using namespace pp;
    sr_ = ctx.sampleRate;
    tone_ = param(s, SnTone), snappy_ = param(s, SnSnappy), body_ = param(s, SnDecay);
    ndec_ = param(s, SnNoiseDecay), tuneHz_ = param(s, SnTune), vol_ = param(s, SnVolume);
    sweep_ = param(s, SnSweep), sweepT_ = param(s, SnSweepTime, 0.02f), band_ = param(s, SnNoiseBand, 1800), width_ = param(s, SnNoiseWidth, 1);
    ring_ = param(s, SnRing), harm_ = param(s, SnHarmonics), click_ = param(s, SnClick), tilt_ = param(s, SnTilt), velTone_ = param(s, SnVelTone);
    setPan(param(s, SnPan));
  }
  void render(float* L, float* R, int n) override {
    const double dt = 1.0 / sr_;
    for (auto& v : voices_) {
      if (!v.on) continue;
      if (v.t == 0) {
        double vt = std::pow(2.0, velTone_ * (v.vel - 0.75) * 2.0);
        v.hp.set(sr_, band_ * vt, (1.0 - width_) * 0.85 + 0.1 * width_);
        v.tilt.set(Biquad::HighShelf, sr_, 1000, 0.6, tilt_ + velTone_ * (v.vel - 0.75) * 6.0);
      }
      for (int i = 0; i < n; ++i) {
        double f = tuneHz_ * v.tune * (sweep_ > 0 ? std::pow(2.0, sweep_ / 12.0 * std::exp(-v.t / sweepT_)) : 1.0);
        v.phase += f * dt;
        v.phase2 += f * 1.47 * dt;
        double ab = std::exp(-v.t / (body_ * 0.4));
        double an = std::exp(-v.t / (ndec_ * 0.4));
        double bodyS = (std::sin(kTwoPi * v.phase) * (1 - tone_ * 0.5) + std::sin(kTwoPi * v.phase2) * tone_ * 0.6) * ab;
        if (harm_ > 0) bodyS = bodyS * (1 - harm_) + harm_ * (std::fabs(bodyS) * 2 - ab * 0.6);
        if (harm_ < 0) bodyS = bodyS * (1 + harm_) - harm_ * std::tanh(bodyS * 4);
        double nz = v.noise.next(), band = 0, high = 0;
        v.hp.process(nz, &band, &high);
        double wire = width_ >= 0.999 ? high : high * width_ + band * (1 - width_) * 1.5;
        double s = bodyS * (1.0 - snappy_ * 0.5) + wire * an * snappy_ * 1.2;
        if (ring_ > 0) s += ring_ * bodyS * nz * 1.5;
        if (click_ > 0 && v.t < 0.0015) s += click_ * (1.0 - v.t / 0.0015);
        if (std::fabs(tilt_) > 0.01 || velTone_ != 0) s = v.tilt.process(s);
        out(L, R, i, s * v.vel * vol_);
        v.t += dt;
        if (ab < 1e-4 && an < 1e-4) {
          v.on = false;
          break;
        }
      }
    }
  }
  double tone_ = 0.5, snappy_ = 0.6, body_ = 0.12, ndec_ = 0.2, tuneHz_ = 190, vol_ = 0.8;
  double sweep_ = 0, sweepT_ = 0.02, band_ = 1800, width_ = 1, ring_ = 0, harm_ = 0, click_ = 0, tilt_ = 0, velTone_ = 0;
};

// ---------------------------------------------------------------------------
struct HatVoice : DrumVoiceBase {
  double ph[6] = {0, 0, 0, 0, 0, 0};
  Svf hp, bp;
};
class HatGen final : public DrumGen<HatVoice> {
 protected:
  void configure(const PluginState& s, const ChannelAux&, const ProcessContext& ctx) override {
    using namespace pp;
    sr_ = ctx.sampleRate;
    decay_ = param(s, HatDecay), tone_ = param(s, HatTone), open_ = param(s, HatOpen) >= 0.5f, vol_ = param(s, HatVolume);
    metal_ = param(s, HatMetal, 0.17f), base_ = param(s, HatBase, 205), width_ = param(s, HatWidth, 0.5f);
    choke_ = param(s, HatChoke, 1) >= 0.5f, attack_ = param(s, HatAttack), click_ = param(s, HatClick);
    setPan(param(s, HatPan));
  }
  void onNoteOn(uint32_t id, double pitch, float vel) override {
    // A new hit chokes ringing hats (open/closed share one group).
    if (choke_)
      for (auto& v : voices_) v.on = false;
    DrumGen<HatVoice>::onNoteOn(id, pitch, vel);
  }
  void render(float* L, float* R, int n) override {
    static const double ratios[6] = {2.0, 3.0, 4.16, 5.43, 6.79, 8.21};
    const double dt = 1.0 / sr_;
    const double dec = open_ ? decay_ * 6.0 : decay_;
    for (auto& v : voices_) {
      if (!v.on) continue;
      if (v.t == 0) {
        v.hp.set(sr_, tone_ * 0.9, 0.2);
        v.bp.set(sr_, tone_, 1.0 - width_);
      }
      for (int i = 0; i < n; ++i) {
        double metal = 0;
        for (int k = 0; k < 6; ++k) {
          v.ph[k] += base_ * ratios[k] * v.tune * dt;
          v.ph[k] -= std::floor(v.ph[k]);
          metal += v.ph[k] < 0.5 ? 1.0 : -1.0;
        }
        double x = metal * 0.7 * metal_ + v.noise.next() * 0.72 * (1 - metal_);
        if (click_ > 0 && v.t < 0.001) x += click_ * 4.0 * (1 - v.t / 0.001);
        double band = 0, high = 0;
        v.bp.process(x, &band);
        v.hp.process(band, nullptr, &high);
        double amp = std::exp(-v.t / (dec * 0.35));
        if (attack_ > 0 && v.t < attack_) amp *= v.t / attack_;
        out(L, R, i, high * amp * v.vel * vol_ * 1.6);
        v.t += dt;
        if (amp < 1e-4 && v.t > attack_) {
          v.on = false;
          break;
        }
      }
    }
  }
  double decay_ = 0.06, tone_ = 8000, vol_ = 0.5, metal_ = 0.17, base_ = 205, width_ = 0.5, attack_ = 0, click_ = 0;
  bool open_ = false, choke_ = true;
};

// ---------------------------------------------------------------------------
struct ClapVoice : DrumVoiceBase {
  Svf bp, bp2;
  Noise noise2;
  double jit[8] = {};
};
class ClapGen final : public DrumGen<ClapVoice> {
 protected:
  void configure(const PluginState& s, const ChannelAux&, const ProcessContext& ctx) override {
    using namespace pp;
    sr_ = ctx.sampleRate;
    decay_ = param(s, ClapDecay), spread_ = param(s, ClapSpread), tone_ = param(s, ClapTone), vol_ = param(s, ClapVolume);
    hits_ = std::clamp(static_cast<int>(param(s, ClapHits, 3) + 0.5f), 1, 8);
    hitDecay_ = param(s, ClapHitDecay, 0.0035f), falloff_ = param(s, ClapFalloff), width_ = param(s, ClapWidth, 0.5f);
    stereo_ = param(s, ClapStereo), humanize_ = param(s, ClapHumanize);
    setPan(param(s, ClapPan));
  }
  void started(ClapVoice& v) override {
    v.noise2.seed(static_cast<uint32_t>(static_cast<int32_t>(v.rnd1 * 1e6)) + 77u);
    for (int b = 0; b < 8; ++b) v.jit[b] = v.noise.next() * 0.3 * humanize_;
  }
  void render(float* L, float* R, int n) override {
    const double dt = 1.0 / sr_;
    for (auto& v : voices_) {
      if (!v.on) continue;
      if (v.t == 0) {
        v.bp.set(sr_, tone_ * v.tune, std::clamp(1.1 * (1 - width_), 0.0, 1.0));
        v.bp2.set(sr_, tone_ * v.tune * 1.07, std::clamp(1.1 * (1 - width_), 0.0, 1.0));
      }
      double tailStart = 0;
      for (int b = 0; b < hits_; ++b) tailStart = std::max(tailStart, b * spread_ * (1 + v.jit[b]));
      for (int i = 0; i < n; ++i) {
        // Bursts, then a decaying tail.
        double envL = 0, envR = 0;
        for (int b = 0; b < hits_; ++b) {
          double tb = v.t - b * spread_ * (1 + v.jit[b]);
          if (tb < 0) continue;
          double e = std::exp(-tb / hitDecay_) * std::pow(1.0 - falloff_, b);
          double side = (b % 2 ? 1.0 : -1.0) * stereo_;
          envL = std::max(envL, e * (1 - std::max(0.0, side)));
          envR = std::max(envR, e * (1 + std::min(0.0, side)));
        }
        double tt = v.t - tailStart;
        double tail = tt >= 0 ? std::exp(-tt / (decay_ * 0.35)) : 0.0;
        envL = std::max(envL, tail * 0.85);
        envR = std::max(envR, tail * 0.85);
        double band = 0, band2 = 0;
        v.bp.process(v.noise.next(), &band);
        double bandR = band;
        if (stereo_ > 0) {
          v.bp2.process(v.noise2.next(), &band2);
          bandR = band * (1 - stereo_) + band2 * stereo_;
        }
        double g = v.vel * vol_ * 2.2;
        L[i] += static_cast<float>(band * envL * g * panL_);
        R[i] += static_cast<float>(bandR * envR * g * panR_);
        v.t += dt;
        if (tt > 0 && tail < 1e-4) {
          v.on = false;
          break;
        }
      }
    }
  }
  double decay_ = 0.25, spread_ = 0.011, tone_ = 1300, vol_ = 0.7, hitDecay_ = 0.0035, falloff_ = 0, width_ = 0.5, stereo_ = 0, humanize_ = 0;
  int hits_ = 3;
};

// ---------------------------------------------------------------------------
// Pluck: Karplus-Strong string, two slightly detuned strings for stereo.
class PluckGen final : public Generator {
 public:
  // Strings and scratch are sized for 20 Hz at 192 kHz up front: no
  // allocation ever happens on the audio thread.
  static constexpr size_t kMaxLen = 192000 / 20 + 4;
  PluckGen() {
    for (auto& v : voices_)
      for (auto& s : v.s) s.buf.reserve(kMaxLen);
    scratch_.reserve(kMaxLen);
  }
  bool active() const override {
    for (const auto& v : voices_)
      if (v.on) return true;
    return false;
  }

 protected:
  struct Str {
    std::vector<float> buf;
    double len = 100, pos = 0;
    float last = 0;
  };
  struct Voice {
    bool on = false, held = false;
    uint32_t id = 0;
    uint64_t age = 0;
    float vel = 0, amp = 1;
    double hz = 220;
    uint32_t clock = 0;
    Str s[2];
  };

  void configure(const PluginState& s, const ChannelAux&, const ProcessContext& ctx) override {
    using namespace pp;
    sr_ = ctx.sampleRate;
    decay_ = param(s, PlkDecay, 2.5f);
    damp_ = param(s, PlkDamping, 0.35f);
    bright_ = param(s, PlkBrightness, 0.7f);
    pos_ = param(s, PlkPosition, 0.18f);
    width_ = param(s, PlkWidth, 0.5f);
    rel_ = param(s, PlkRelease, 0.08f);
    vol_ = param(s, PlkVolume, 0.8f);
    normalize_ = param(s, PlkNormalize, 1) >= 0.5f;
    gate_ = param(s, PlkGate, 1) >= 0.5f;
  }

  void onNoteOn(uint32_t id, double pitch, float vel) override {
    ++clock_;
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
    v.on = true;
    v.held = true;
    v.id = id;
    v.age = clock_;
    v.vel = vel;
    v.amp = 1;
    v.clock = 0;
    v.hz = noteToHz(pitch);
    Noise nz;
    nz.seed(id * 2246822519u + 3);
    for (int c = 0; c < 2; ++c) {
      double hz = v.hz * std::pow(2.0, (c ? 1 : -1) * width_ * 4.0 / 1200.0);
      Str& st = v.s[c];
      st.len = std::max(2.0, sr_ / hz);
      size_t n = std::min(kMaxLen, static_cast<size_t>(std::ceil(st.len)) + 2);
      st.len = std::min(st.len, static_cast<double>(n - 2));
      st.buf.assign(n, 0.f);  // within reserved capacity
      // Excitation: noise, low-passed by brightness, comb-filtered at the pick position.
      double lp = 0, a = 1.0 - 0.9 * (1.0 - bright_);
      std::vector<float>& ex = scratch_;
      ex.assign(n, 0.f);
      for (size_t i = 0; i < n; ++i) {
        lp += (nz.next() - lp) * a;
        ex[i] = static_cast<float>(lp);
      }
      size_t p = std::max<size_t>(1, static_cast<size_t>(pos_ * st.len));
      for (size_t i = 0; i < n; ++i) st.buf[i] = ex[i] - (i >= p ? ex[i - p] : 0.f);
      st.pos = 0;
      st.last = 0;
    }
  }
  void onNoteOff(uint32_t id) override {
    for (auto& v : voices_)
      if (v.on && v.id == id && gate_) v.held = false;
  }
  void onAllOff() override {
    for (auto& v : voices_) v.held = false;
  }
  void onKill() override {
    for (auto& v : voices_) v.on = false;
  }

  void render(float* L, float* R, int n) override {
    for (auto& v : voices_) {
      if (!v.on) continue;
      // Loop gain per pass so the string decays 60 dB in `decay` seconds.
      // Normalised: every note rings for `decay`; otherwise higher notes die faster (fixed loop gain).
      double g = std::pow(10.0, -3.0 / (std::max(0.05f, decay_) * (normalize_ ? v.hz : 220.0)));
      double relMul = std::exp(-1.0 / std::max(1.0, rel_ * sr_ * 0.25));
      for (int i = 0; i < n; ++i) {
        float out[2];
        for (int c = 0; c < 2; ++c) {
          Str& st = v.s[c];
          size_t N = st.buf.size();
          size_t i0 = static_cast<size_t>(st.pos) % N;
          size_t i1 = (i0 + 1) % N;
          float x0 = st.buf[i0], x1 = st.buf[i1];
          float y = static_cast<float>(((1 - damp_) * x0 + damp_ * 0.5 * (x0 + x1)) * g);
          st.buf[i0] = y;
          out[c] = x0;
          st.pos += 1.0;
          if (st.pos >= st.len) st.pos -= st.len;
        }
        if (!v.held) v.amp *= static_cast<float>(relMul);
        float gain = v.vel * vol_ * v.amp;
        L[i] += out[0] * gain;
        R[i] += out[1] * gain;
        if ((++v.clock & 255) == 0) {  // end-of-note checks every 256 samples of the voice's own clock
          float e = 0;
          for (float x : v.s[0].buf) e = std::max(e, std::fabs(x));
          if (v.amp < 1e-4f || e * v.amp < 1e-5f) {
            v.on = false;
            break;
          }
        }
      }
    }
  }

 private:
  std::array<Voice, 16> voices_{};
  std::vector<float> scratch_;
  double sr_ = 44100;
  float decay_ = 2.5f, damp_ = 0.35f, bright_ = 0.7f, pos_ = 0.18f, width_ = 0.5f, rel_ = 0.08f, vol_ = 0.8f;
  bool normalize_ = true, gate_ = true;
  uint64_t clock_ = 0;
};

}  // namespace

std::unique_ptr<Generator> createGeneratorW2(const std::string& type);  // Generators2.cpp

std::unique_ptr<Generator> createGenerator(const std::string& type) {
  if (isHostedType(type)) return host::createHostedGenerator(type);
  if (auto g = createGeneratorW2(type)) return g;
  if (auto g = packs::createGenerator(type)) return g;
  if (type == "gl.pluck") return std::make_unique<PluckGen>();
  if (type == "gl.synth") return std::make_unique<SynthGen>();
  if (type == "gl.sampler") return std::make_unique<SamplerGen>();
  if (type == "gl.kick") return std::make_unique<KickGen>();
  if (type == "gl.snare") return std::make_unique<SnareGen>();
  if (type == "gl.hat") return std::make_unique<HatGen>();
  if (type == "gl.clap") return std::make_unique<ClapGen>();
  return nullptr;
}

}  // namespace gl
