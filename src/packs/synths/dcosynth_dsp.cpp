// Gridline — "synths" pack, J-22: DCO Synth (gl.dcosynth) runtime.
//
// Two digitally-controlled oscillators per voice (each: pulse with PWM from
// LFO 1 / manual / envelope, saw, square sub one octave down with its phase
// offset), DCO 2 with semitone / detune offset, hard sync to DCO 1 and
// two-way cross modulation (6 modulator shapes), DCO 1/2 AM, noise; then a
// high pass and a resonant 4-stage ladder low pass with 6/12/18/24 dB taps,
// key follow, audio-rate filter FM, envelope (with polarity) and LFO amounts;
// VCA from envelope 1, gate or envelope 2. Two ADSR envelopes, two LFOs
// (LFO 2 with 6 shapes, both with delay, tempo sync, key retrigger phase),
// an 8-slot matrix (24 sources → any control or the DCO pitches), mono /
// legato / portamento, hold, octave, arpeggiator (9 modes), the classic
// bucket-brigade chorus (I / II / I+II with hiss) and the FX chain
// saturation → EQ → ensemble → delay → reverb (→ saturation when post).
//
// Value laws (the controls store 0..1 positions; see dcoLaw below):
//   attack 1 ms·3000^x, decay / release 2 ms·6000^x, sustain x
//   cutoff 20 Hz·1000^x (key follow: 1 octave / octave at 1), resonance x
//   high pass 20 Hz·400^x (off at 0 or when switched off)
//   LFO rate 0.05 Hz·600^x (≈ 0.05 … 30 Hz) or, synced, a cycle of
//   (round(6143·(2^(7(1−x)) − 1) / 127) + 1) / 192 beats; delay 0..5 s fade in
//   DCO LFO depth x²·2 st (x²·24 st with the range switch), env 2 → DCO x²·24 st
//   pulse width 50 % → 5 %, DCO 2 −48..+48 st, detune ±50 ct
//   env → cutoff x·8 octaves, LFO → cutoff x·4 octaves, filter FM x·4 octaves
//   portamento x²·3 s, volume (x / 0.6311)², EQ ±18 dB / 20 Hz·1000^x
// "One DCO" panel: the classic voicing (the PWM envelope is envelope 1).
#include <algorithm>
#include <array>
#include <cmath>
#include "../Packs.h"
#include "../../engine/Dsp.h"
#include "dco.h"
#include "dcosynth_fx.h"
#include "synths_common.h"

namespace gl::packs::synths {

namespace {

using namespace dco;

constexpr int kDcVoices = 16;
constexpr int kCtl = 16;  // control-rate step (samples)

double expMap(double x, double lo, double ratio) { return lo * std::pow(ratio, std::clamp(x, 0.0, 1.0)); }
double bip(double x) { return std::clamp(x * 2.0 - 1.0, -1.0, 1.0); }
int idx(double x, int n) { return std::clamp(static_cast<int>(std::lround(x * (n - 1))), 0, n - 1); }

// Exponential-segment ADSR with times in seconds (attack linear in level).
struct Env {
  int stage = 0;  // 0 idle, 1 attack, 2 decay, 3 sustain, 4 release
  double v = 0, ai = 0, dc = 0, rc = 0, s = 0;
  void set(double sr, double a, double d, double sus, double r) {
    ai = 1.0 / std::max(1.0, a * sr);
    dc = std::exp(-1.0 / std::max(1.0, d * sr / 4.0));
    rc = std::exp(-1.0 / std::max(1.0, r * sr / 4.0));
    s = sus;
  }
  void on() { stage = 1; }
  void off() {
    if (stage != 0) stage = 4;
  }
  double tick(int n) {
    for (int i = 0; i < n; ++i) {
      switch (stage) {
        case 1:
          v += ai;
          if (v >= 1) {
            v = 1;
            stage = 2;
          }
          break;
        case 2:
          v = s + (v - s) * dc;
          if (std::fabs(v - s) < 1e-4) stage = 3;
          break;
        case 3: v = s; break;
        case 4:
          v *= rc;
          if (v < 1e-5) {
            v = 0;
            stage = 0;
          }
          break;
        default: v = 0; break;
      }
    }
    return v;
  }
};

class DcoSynth final : public Generator {
 public:
  DcoSynth() {
    bufL_.assign(kCtl, 0.f);
    bufR_.assign(kCtl, 0.f);
  }
  bool active() const override {
    if (tail_ > 0) return true;
    for (const auto& v : voices_)
      if (v.on) return true;
    return false;
  }
  bool supportsGlide() const override { return true; }

 protected:
  struct Voice {
    bool on = false, held = false, sustained = false;
    uint32_t id = 0;
    double pitch = 60, target = 60, glideFrom = 60;
    double glideT = 1, glideLen = 0;
    float vel = 1;
    uint64_t age = 0;
    double ph1 = 0, ph2 = 0, sub1 = 0, sub2 = 0;
    double xm1 = 0, xm2 = 0;  // last osc outputs (cross mod)
    Env e1, e2;
    double gate = 0;
    double hp = 0, hpx = 0;
    double st[4] = {0, 0, 0, 0};
    double rnd = 0, alt = 1;
    uint32_t noise = 1;
    double vcaSmooth = 0;
    double modOff[kDcTags] = {};  // matrix offsets of this voice (control rate)
    double p1Mod = 0, p2Mod = 0;
    bool ctlValid = false;        // control values computed for the current period (J-30)
    double ce1 = 0, ce2 = 0;      // envelope values of the current period
  };

  double P(int i) const { return static_cast<double>(p_[static_cast<size_t>(i)]); }
  // A switch / selector's stored choice index.
  int ci(int i, int n) const { return std::clamp(static_cast<int>(std::lround(P(i))), 0, n - 1); }

  void configure(const PluginState& s, const ChannelAux&, const ProcessContext& ctx) override {
    sr_ = ctx.sampleRate > 0 ? ctx.sampleRate : 44100.0;
    tempo_ = ctx.tempo > 0 ? ctx.tempo : 120.0;
    for (int i = 0; i < DcCount; ++i) p_[static_cast<size_t>(i)] = prm(s, i, 0.f);
    panelOne_ = prmi(s, DcPanel) == PanelOne;
    for (int r = 0; r < kDcMatrix; ++r) {
      mxSrc_[r] = std::clamp(prmi(s, DcMxSrc1 + r), 0, kDcSources);
      mxDst_[r] = std::clamp(prmi(s, DcMxDst1 + r), 0, kDcDestCount - 1);
      mxAmt_[r] = bip(P(DcMxAmt1 + r));
    }
    // Global effects (modulated per control step through gfx()).
    chorus_.configure(sr_, ci(DcChorus, 4), P(DcChorusNoise), P(DcChorusGate) >= 0.5);
    ens_.sr = sr_;
    dly_.configure(sr_);
    const bool arp = P(DcArpOn) >= 0.5;
    if (!arp && arpOn_) arpStop();
    arpOn_ = arp;
  }

  // Effective (matrix-modulated) global value of a control.
  double gv(int tag) const { return std::clamp(P(tag) + globalMod_[static_cast<size_t>(tag)], 0.0, 1.0); }

  void setupFx() {
    const double gains[3] = {bip(gv(DcEqGainLo)) * 18.0, bip(gv(DcEqGainMid)) * 18.0, bip(gv(DcEqGainHi)) * 18.0};
    const double hz[3] = {expMap(gv(DcEqFreqLo), 20, 1000), expMap(gv(DcEqFreqMid), 20, 1000), expMap(gv(DcEqFreqHi), 20, 1000)};
    const double q[3] = {expMap(gv(DcEqQLo), 0.3, 20), expMap(gv(DcEqQMid), 0.3, 20), expMap(gv(DcEqQHi), 0.3, 20)};
    eq_.configure(sr_, P(DcEqOn) >= 0.5, gains, hz, q);
    dly_.on = P(DcDlyOn) >= 0.5;
    dly_.input = gv(DcDlyInput);
    double t;
    if (P(DcDlySync) >= 0.5) {
      static const double beats[8] = {0.125, 0.25, 1.0 / 3.0, 0.5, 0.75, 1.0, 1.5, 2.0};
      t = beats[idx(gv(DcDlyTime), 8)] * 60.0 / tempo_;
    } else {
      t = expMap(gv(DcDlyTime), 0.005, 400.0);
    }
    t = std::min(t, 2.0);
    dly_.tL = t;
    dly_.tR = std::clamp(t * (0.5 + gv(DcDlyRightTap)), 0.001, 4.0);
    dly_.fb = gv(DcDlyFb) * 0.95;
    dly_.fbHz = expMap(gv(DcDlyFbCut), 100, 200);
    dly_.fbType = ci(DcDlyFbType, 4);
    dly_.stereo = gv(DcDlyStereo) * 0.5;
    dly_.blur = gv(DcDlyBlur);
    dly_.tone = gv(DcDlyTone);
    plate_.on = P(DcRevOn) >= 0.5;
    plate_.lowCut = expMap(gv(DcRevLowCut), 20, 50);
    plate_.highCut = expMap(gv(DcRevHighCut), 1000, 20);
    plate_.predelay = gv(DcRevPredelay) * 0.25;
    plate_.preFb = gv(DcRevPreFb) * 0.7;
    plate_.size = gv(DcRevSize);
    plate_.diffusion = gv(DcRevDiffusion) * 0.9;
    plate_.decay = expMap(gv(DcRevDecay), 0.2, 100);
    plate_.dampAmt = gv(DcRevDamp);
    plate_.width = gv(DcRevWidth) * 2.0;
    plate_.dry = std::min(1.0, gv(DcRevDry) * 1.25);
    plate_.early = gv(DcRevEarly);
    plate_.wet = gv(DcRevWet);
    // J-30: the plate's filters and line lengths follow the values just set
    // (configured once per process call, they used the previous chunk's values).
    plate_.configure(sr_);
    ens_.on = P(DcEnsOn) >= 0.5;
    ens_.delayMs = 2.0 + 30.0 * gv(DcEnsDelay);
    ens_.fb = gv(DcEnsFb) * 0.8;
    ens_.rate = expMap(gv(DcEnsRate), 0.05, 200);
    ens_.depth = gv(DcEnsDepth);
    ens_.move = gv(DcEnsMove);
    ens_.moveRate = expMap(gv(DcEnsMoveRate), 0.05, 100);
    ens_.lines = ci(DcEnsLines, 2) == 1 ? 4 : 2;
    ens_.lpHz = expMap(gv(DcEnsLowPass), 200, 100);
    ens_.hpHz = expMap(gv(DcEnsHighPass), 20, 100);
    ens_.mix = gv(DcEnsMix);
    ens_.phase = gv(DcEnsPhase);
  }

  // ---------------------------------------------------------------- notes
  void onNoteOn(uint32_t id, double pitch, float vel) override {
    if (arpOn_) {
      arpAdd(id, pitch, vel);
      return;
    }
    startNote(id, pitch, vel);
  }
  void onNoteOff(uint32_t id) override {
    if (arpOn_) arpRemove(id);
    stopNote(id);
  }
  void onGlide(uint32_t id, double pitch, double seconds) override {
    for (auto& v : voices_)
      if (v.on && v.id == id) {
        v.glideFrom = v.pitch;
        v.target = pitch;
        v.glideT = 0;
        v.glideLen = std::max(1e-3, seconds);
      }
  }
  void onAllOff() override {
    arpCount_ = 0;
    arpStop();
    heldCount_ = 0;
    for (auto& v : voices_) {
      v.held = v.sustained = false;
      v.e1.off();
      v.e2.off();
    }
  }
  void onKill() override {
    for (auto& v : voices_) v = Voice{};
    heldCount_ = 0;
    arpCount_ = 0;
    arpNoteOn_ = false;
    chorus_.clear();
    eq_.clear();
    dly_.clear();
    plate_.clear();
    ens_.clear();
    tail_ = 0;
  }

  void triggerLfos() {
    // Delay / phase restart when a note starts with no key held.
    for (int l = 0; l < 2; ++l) {
      const int trig = l == 0 ? DcLfo1Trig : DcLfo2Trig;
      const int phOn = l == 0 ? DcLfo1PhaseOn : DcLfo2PhaseOn;
      const int phV = l == 0 ? DcLfo1Phase : DcLfo2Phase;
      if (P(trig) >= 0.5 || heldCount_ == 0) {
        lfoFade_[l] = 0;
        if (P(phOn) >= 0.5) lfoPh_[l] = P(phV);
      }
    }
  }

  void startNote(uint32_t id, double pitch, float vel) {
    ++clock_;
    altSign_ = -altSign_;
    triggerLfos();
    const bool mono = P(DcMono) >= 0.5;
    const double porta = P(DcPorta);
    const double portaSec = porta * porta * 3.0;
    if (mono) {
      stack_.push(id, pitch, vel);
      Voice& v = voices_[0];
      const bool legato = P(DcLegato) >= 0.5 && heldCount_ > 0 && v.on;
      const bool glide = portaSec > 0.002 && v.on;
      v.id = id;
      v.glideFrom = v.on ? v.pitch : pitch;
      v.target = pitch;
      v.glideT = glide ? 0.0 : 1.0;
      v.glideLen = portaSec;
      if (!glide) v.pitch = pitch;
      v.vel = vel;
      v.held = true;
      v.age = clock_;
      if (!v.on || !legato || P(DcRetrig) >= 0.5) trigger(v);
      v.on = true;
      ++heldCount_;
      return;
    }
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
    const bool glide = portaSec > 0.002 && lastPitch_ >= 0;
    v = Voice{};
    v.on = v.held = true;
    v.id = id;
    v.vel = vel;
    v.age = clock_;
    v.target = pitch;
    v.glideFrom = glide ? lastPitch_ : pitch;
    v.pitch = v.glideFrom;
    v.glideT = glide ? 0.0 : 1.0;
    v.glideLen = portaSec;
    v.noise = 0x9E3779B9u * static_cast<uint32_t>(clock_ + 1);
    v.rnd = hashUnit(static_cast<uint32_t>(clock_) * 2654435761u);
    v.alt = altSign_;
    trigger(v);
    lastPitch_ = pitch;
    ++heldCount_;
  }
  void trigger(Voice& v) {
    v.ctlValid = false;  // fresh control values from this sample on (J-30)
    applyEnvs(v);
    v.e1.on();
    v.e2.on();
    v.gate = 1;
    // A small kick so a self-oscillating filter starts without oscillators.
    if (std::fabs(v.st[3]) < 1e-3) v.st[3] = 0.02;
    // Sub phase offsets.
    v.sub1 = P(DcDco1SubPhase) * 0.5;
    v.sub2 = P(DcDco2SubPhase) * 0.5;
  }
  void stopNote(uint32_t id) {
    const bool hold = P(DcHold) >= 0.5;
    if (P(DcMono) >= 0.5) {
      stack_.remove(id);
      Voice& v = voices_[0];
      if (heldCount_ > 0) --heldCount_;
      if (stack_.count > 0 && v.id == id) {
        const auto& top = stack_.notes[static_cast<size_t>(stack_.count - 1)];
        v.id = top.id;
        v.glideFrom = v.pitch;
        v.target = top.pitch;
        v.glideT = P(DcPorta) > 0.03 ? 0.0 : 1.0;
        v.glideLen = P(DcPorta) * P(DcPorta) * 3.0;
        if (v.glideT >= 1) v.pitch = top.pitch;
        if (P(DcRelease) >= 0.5) trigger(v);
        return;
      }
      if (stack_.count == 0 && v.on) {
        if (hold) {
          v.sustained = true;
        } else {
          v.held = false;
          v.e1.off();
          v.e2.off();
          v.gate = 0;
        }
      }
      return;
    }
    for (auto& v : voices_)
      if (v.on && v.held && v.id == id) {
        if (heldCount_ > 0) --heldCount_;
        if (hold) {
          v.sustained = true;
          continue;
        }
        v.held = false;
        v.e1.off();
        v.e2.off();
        v.gate = 0;
      }
  }

  void applyEnvs(Voice& v) {
    auto envSet = [&](Env& e, int base) {
      e.set(sr_, expMap(P(base) + v.modOff[base], 0.001, 3000), expMap(P(base + 1) + v.modOff[base + 1], 0.002, 6000),
            std::clamp(P(base + 2) + v.modOff[base + 2], 0.0, 1.0), expMap(P(base + 3) + v.modOff[base + 3], 0.002, 6000));
    };
    envSet(v.e1, DcEnv1A);
    envSet(v.e2, DcEnv2A);
  }

  // ------------------------------------------------------------- sources
  // Matrix source value (choice index 1..24).
  double source(int src, const Voice* v, double lfo1Tri, double lfo1Sin, double lfo2) const {
    const double gate = v && v->held ? 1.0 : 0.0;
    switch (src) {
      case 1: return lfo1Tri;
      case 2: return lfo2;
      case 3: return v ? v->e1.v : 0.0;
      case 4: return v ? v->e2.v : 0.0;
      case 5: return v ? v->vel : 0.0;
      case 6: return v ? 1.0 - v->vel : 0.0;
      case 7: return bip(P(DcXOffset));
      case 8: return bip(P(DcYOffset));
      case 9: return P(DcXOffset);
      case 10: return P(DcYOffset);
      case 11: return gate;
      case 12: return 1.0 - gate;
      case 13: return lfo1Sin;
      case 14: return 0.5 + 0.5 * lfo1Tri;
      case 15: return 0.5 + 0.5 * lfo2;
      case 16: return v ? std::clamp((v->pitch - 60.0) / 48.0, -1.0, 1.0) : 0.0;
      case 17: return std::clamp(lfo1Tri * 4.0, -4.0, 4.0);
      case 18: return std::clamp(lfo2 * 4.0, -4.0, 4.0);
      case 19: return lfo1Sin * 4.0;
      case 20: return (0.5 + 0.5 * lfo1Tri) * 4.0;
      case 21: return (0.5 + 0.5 * lfo2) * 4.0;
      case 22: return v ? v->alt : 1.0;
      case 23: return v ? std::clamp(v->pitch / 127.0, 0.0, 1.0) : 0.0;
      case 24: return v ? v->rnd : 0.0;
      default: return 0.0;
    }
  }

  double lfoShape2(double ph, uint64_t cyc) const {
    switch (ci(DcLfo2Shape, 6)) {
      case 0: return ph < 0.25 ? 4 * ph : ph < 0.75 ? 2 - 4 * ph : 4 * ph - 4;
      case 1: return std::sin(dsp::kTwoPi * ph);
      case 2: return ph < 0.5 ? 1.0 : -1.0;
      case 3: return 2.0 * ph - 1.0;
      case 4: return 1.0 - 2.0 * ph;
      default: return hashUnit(static_cast<uint32_t>(cyc) * 2654435761u ^ 0x5bd1e995u);
    }
  }
  double lfoRate(int l) const {
    const int rate = l == 0 ? DcLfo1Rate : DcLfo2Rate, sync = l == 0 ? DcLfo1Sync : DcLfo2Sync;
    if (P(sync) >= 0.5) {
      // Synced: cycle length (1/192 beat units) on a 2^(7(1 − r)) scale, 1/192 … 32 beats.
      const double u = (std::exp2(7.0 * (1.0 - gv(rate))) - 1.0) / 127.0;
      const double beats = (std::round(u * 6143.0) + 1.0) / 192.0;
      return tempo_ / 60.0 / std::max(1.0 / 192.0, beats);
    }
    return expMap(gv(rate), 0.05, 600);
  }

  // ------------------------------------------------------------- render
  void render(float* L, float* R, int n) override {
    int pos = 0;
    while (pos < n) {
      // Control periods of kCtl samples on the instrument's own clock (J-30:
      // periods counted from each render call made the sound depend on the
      // host block size).
      int m = std::min(n - pos, kCtl - ctlPhase_);
      if (arpOn_) {
        if (arpCount_ > 0 && arpCounter_ <= 0) {
          arpFire();
          arpCounter_ += std::max(1.0, arpStep());
        }
        if (arpNoteOn_ && arpGateLeft_ <= 0) {
          stopNote(arpVoiceId_);
          arpNoteOn_ = false;
        }
        if (arpCount_ > 0) m = std::min(m, std::max(1, static_cast<int>(std::ceil(arpCounter_))));
        if (arpNoteOn_) m = std::min(m, std::max(1, static_cast<int>(std::ceil(arpGateLeft_))));
      }
      renderChunk(L + pos, R + pos, m);
      ctlPhase_ = (ctlPhase_ + m) % kCtl;
      if (arpOn_) {
        if (arpCount_ > 0) arpCounter_ -= m;
        if (arpNoteOn_) arpGateLeft_ -= m;
      }
      pos += m;
    }
  }

  void renderChunk(float* L, float* R, int n) {
    // Control values are computed at the start of each control period (for
    // the whole period) and held for its later chunks (J-30).
    const bool fresh = ctlPhase_ == 0;
    if (fresh) {
      // LFOs (global).
      for (int l = 0; l < 2; ++l) {
        lfoPh_[l] += lfoRate(l) / sr_ * kCtl;
        while (lfoPh_[l] >= 1.0) {
          lfoPh_[l] -= 1.0;
          ++lfoCycle_[l];
        }
        const double delay = (l == 0 ? gv(DcLfo1Delay) : gv(DcLfo2Delay)) * 5.0;
        lfoFade_[l] = delay < 1e-3 ? 1.0 : std::min(1.0, lfoFade_[l] + kCtl / (delay * sr_));
      }
      const double ph = lfoPh_[0];
      lfoVal_[0] = (ph < 0.25 ? 4 * ph : ph < 0.75 ? 2 - 4 * ph : 4 * ph - 4) * lfoFade_[0];
      lfo1Sin_ = std::sin(dsp::kTwoPi * ph) * lfoFade_[0];
      lfoVal_[1] = lfoShape2(lfoPh_[1], lfoCycle_[1]) * lfoFade_[1];
      // Global matrix (FX destinations follow the newest voice's envelopes).
      globalMod_.fill(0.0);
      const Voice* newest = nullptr;
      for (const auto& v : voices_)
        if (v.on && (!newest || v.age > newest->age)) newest = &v;
      for (int r = 0; r < kDcMatrix; ++r) {
        if (mxSrc_[r] == 0 || mxDst_[r] == 0 || mxDst_[r] > kDcTags) continue;
        globalMod_[static_cast<size_t>(mxDst_[r] - 1)] += mxAmt_[r] * source(mxSrc_[r], newest, lfoVal_[0], lfo1Sin_, lfoVal_[1]);
      }
    }
    const double* lfoVal = lfoVal_;
    const double lfo1Sin = lfo1Sin_;
    setupFx();
    std::fill(bufL_.begin(), bufL_.begin() + n, 0.f);
    std::fill(bufR_.begin(), bufR_.begin() + n, 0.f);
    bool any = false;
    for (auto& v : voices_)
      if (v.on) {
        // Voices compute their controls at period starts, and when they start mid-period
        // (for the rest of it).
        const bool ctl = fresh || !v.ctlValid;
        renderVoice(v, n, lfoVal[0], lfo1Sin, lfoVal[1], ctl, kCtl - ctlPhase_);
        any = true;
      }
    float* bl = bufL_.data();
    float* br = bufR_.data();
    chorus_.process(bl, br, n, any);
    const bool satOn = P(DcSatOn) >= 0.5, satPost = ci(DcSatPost, 2) == 1;
    const double drive = 1.0 + gv(DcSatAmount) * 15.0;
    auto sat = [&]() {
      const double mk = 1.0 / softClip(drive * 0.5) * 0.5;
      for (int i = 0; i < n; ++i) {
        bl[i] = static_cast<float>(softClip(bl[i] * drive) * mk);
        br[i] = static_cast<float>(softClip(br[i] * drive) * mk);
      }
    };
    if (satOn && !satPost) sat();
    eq_.process(bl, br, n);
    ens_.process(bl, br, n);
    dly_.process(bl, br, n);
    plate_.process(bl, br, n);
    if (satOn && satPost) sat();
    const double vol = gv(DcVolume) / 0.6311, gain = vol * vol;
    const double pan = bip(gv(DcPan));
    const double pl = pan > 0 ? 1.0 - pan : 1.0, pr = pan < 0 ? 1.0 + pan : 1.0;
    double peak = 0;
    for (int i = 0; i < n; ++i) {
      const double l = bl[i] * gain * pl, r = br[i] * gain * pr;
      L[i] += static_cast<float>(l);
      R[i] += static_cast<float>(r);
      peak = std::max({peak, std::fabs(l), std::fabs(r)});
    }
    const bool fxTail = dly_.on || plate_.on || ens_.on || P(DcChorus) > 0.01;
    if (any) tail_ = fxTail ? static_cast<int>(sr_ * 4) : 0;
    else if (tail_ > 0) tail_ = peak < 1e-5 ? std::max(0, tail_ - n * 8) : tail_ - n;
  }

  double vp(const Voice& v, int tag) const { return std::clamp(P(tag) + v.modOff[tag], 0.0, 1.0); }

  // Modulator waveform for the cross mod / filter FM shapes.
  static double xShape(int shape, double ph, double osc) {
    switch (shape) {
      case 0: return std::sin(dsp::kTwoPi * ph);
      case 1: return ph < 0.25 ? 4 * ph : ph < 0.75 ? 2 - 4 * ph : 4 * ph - 4;
      case 2: return 2 * ph - 1;
      case 3: return 1 - 2 * ph;
      case 4: return ph < 0.5 ? 1.0 : -1.0;
      default: return osc;
    }
  }

  // `ctl`: compute the voice's control values (matrix, glide, envelopes) for
  // the next `adv` samples; otherwise the chunk reuses them (J-30).
  void renderVoice(Voice& v, int n, double lfo1, double lfo1Sin, double lfo2, bool ctl, int adv) {
    const int vca = panelOne_ ? (P(DcVcaSrc) >= 0.5 ? 1 : 0) : ci(DcVcaSrc, 3);
    if (ctl) {
      v.ctlValid = true;
      // Per-voice matrix.
      std::fill(std::begin(v.modOff), std::end(v.modOff), 0.0);
      v.p1Mod = v.p2Mod = 0;
      for (int r = 0; r < kDcMatrix; ++r) {
        const int d = mxDst_[r];
        if (mxSrc_[r] == 0 || d == 0) continue;
        const double val = mxAmt_[r] * source(mxSrc_[r], &v, lfo1, lfo1Sin, lfo2);
        if (d == kDcDestDco1Pitch) v.p1Mod += val * 12.0;
        else if (d == kDcDestDco2Pitch) v.p2Mod += val * 12.0;
        else if (d >= 1 && d <= kDcTags) v.modOff[d - 1] += val;
      }
      // Glide.
      if (v.glideT < 1.0) {
        v.glideT = std::min(1.0, v.glideT + adv / std::max(1.0, v.glideLen * sr_));
        if (ci(DcPortaMode, 2) == 0) {  // exponential approach
          const double k = 1.0 - std::exp(-5.0 * adv / std::max(1.0, v.glideLen * sr_));
          v.pitch += (v.target - v.pitch) * k;
          if (v.glideT >= 1.0) v.pitch = v.target;
        } else {
          v.pitch = v.glideFrom + (v.target - v.glideFrom) * v.glideT;
        }
      } else {
        v.pitch = v.target;
      }
      applyEnvs(v);
      v.ce1 = v.e1.tick(adv);
      v.ce2 = v.e2.tick(adv);
      const bool envDone = vca == 0 ? v.e1.stage == 0 : vca == 2 ? v.e2.stage == 0 : (!v.held && !v.sustained && v.vcaSmooth < 1e-4);
      if (!v.held && !v.sustained && envDone) {
        v.on = false;
        return;
      }
    }
    const double e1 = v.ce1, e2 = v.ce2;
    const double lfoA = lfo1, lfoB = lfo2;
    const double oct = (ci(DcOctave, 3) - 1) * 12.0 + bip(P(DcMasterTune));
    auto dcoDepth = [&](int amt, int wide) {
      const double a = vp(v, amt);
      return a * a * (P(wide) >= 0.5 ? 24.0 : 2.0);
    };
    const double ep1 = vp(v, DcEnv2ToDco1), ep2 = vp(v, DcEnv2ToDco2);
    const double env2Pitch1 = ep1 * ep1 * 24.0 * e2;
    const double env2Pitch2 = ep2 * ep2 * 24.0 * e2;
    const double pitch1 = v.pitch + oct + dcoDepth(DcLfo1ToDco1, DcLfo1ToDco1Wide) * lfoA +
                          dcoDepth(DcLfo2ToDco1, DcLfo2ToDco1Wide) * lfoB + env2Pitch1 + v.p1Mod;
    const bool dco2 = P(DcDco2On) >= 0.5 && !panelOne_;
    const double semi2 = vp(v, DcDco2Semi) * 96.0 - 48.0;
    const double det2 = bip(vp(v, DcDco2Detune)) * 0.5;
    const double pitch2 = v.pitch + oct + semi2 + det2 + dcoDepth(DcLfo1ToDco2, DcLfo1ToDco2Wide) * lfoA +
                          dcoDepth(DcLfo2ToDco2, DcLfo2ToDco2Wide) * lfoB + env2Pitch2 + v.p2Mod;
    const double inc1 = std::min(0.45, dsp::noteToHz(pitch1) / sr_);
    const double inc2 = std::min(0.45, dsp::noteToHz(pitch2) / sr_);
    // Pulse widths.
    auto pwOf = [&](int amt, int src) {
      const double a = vp(v, amt);
      const int s = ci(src, 3);
      const double envPw = panelOne_ ? e1 : e2;
      const double m = s == 1 ? a : s == 0 ? a * (0.5 + 0.5 * lfoA) : a * envPw;
      return std::clamp(0.5 - 0.45 * m, 0.03, 0.97);
    };
    const double pw1 = pwOf(DcDco1Pw, DcDco1PwSrc), pw2 = pwOf(DcDco2Pw, DcDco2PwSrc);
    const bool pul1 = P(DcDco1Pulse) >= 0.5, saw1 = P(DcDco1Saw) >= 0.5, sub1 = P(DcDco1Sub) >= 0.5;
    const bool pul2 = P(DcDco2Pulse) >= 0.5, saw2 = P(DcDco2Saw) >= 0.5, sub2 = P(DcDco2Sub) >= 0.5;
    const double subLv1 = vp(v, DcDco1SubLevel), subLv2 = vp(v, DcDco2SubLevel);
    const double lv1 = panelOne_ ? 1.0 : vp(v, DcDco1Level), lv2 = vp(v, DcDco2Level);
    const double noiseLv = vp(v, DcNoise);
    const double am = vp(v, DcAm);
    const bool sync = P(DcSync) >= 0.5;
    const double x12 = vp(v, DcXmod12), x21 = vp(v, DcXmod21);
    const int x12s = ci(DcXmod12Shape, 6), x21s = ci(DcXmod21Shape, 6);
    // Filter.
    const double kf = vp(v, DcKeyFollow);
    const double envAmt = vp(v, DcEnv1ToCut) * (ci(DcEnvPolarity, 2) == 1 ? -1.0 : 1.0);
    double cutOct = std::log2(expMap(vp(v, DcCutoff), 20, 1000)) + kf * (v.pitch - 60.0) / 12.0 + envAmt * 8.0 * e1 +
                    vp(v, DcLfo1ToCut) * 4.0 * lfoA + vp(v, DcLfo2ToCut) * 4.0 * lfoB;
    const double fm = vp(v, DcFilterFm) * 4.0;
    const int fmShape = ci(DcFilterFmShape, 6);
    const double reso = vp(v, DcReso);
    const int slope = panelOne_ ? 3 : ci(DcSlope, 4);
    const bool hpOn = P(DcHpfOn) >= 0.5 && vp(v, DcHpf) > 0.001;
    const double hpHz = expMap(vp(v, DcHpf), 20, 400);
    const double hpA = hpOn ? 1.0 - std::exp(-dsp::kTwoPi * hpHz / sr_) : 0.0;
    const double velVol = vp(v, DcVelVol);
    const double vgain = (1.0 - velVol + velVol * v.vel) * 0.35;
    const bool smooth = P(DcVcaSmooth) >= 0.5;
    const double gateTarget = (v.held || v.sustained) ? 1.0 : 0.0;
    const double gateCoef = 1.0 - std::exp(-1.0 / ((smooth ? 0.006 : 0.0015) * sr_));
    const double vcaEnv = vca == 0 ? e1 : vca == 2 ? e2 : -1.0;
    for (int i = 0; i < n; ++i) {
      // DCO 1.
      double m1 = 0;
      if (x21 > 0 && dco2) m1 = xShape(x21s, v.ph2, v.xm2) * x21 * x21 * 4.0;
      double i1 = inc1 * std::max(0.0, 1.0 + m1);
      v.ph1 += i1;
      bool wrapped = false;
      if (v.ph1 >= 1.0) {
        v.ph1 -= std::floor(v.ph1);
        wrapped = true;
        v.sub1 += 0.5;
        if (v.sub1 >= 1.0) v.sub1 -= 1.0;
      }
      double o1 = 0;
      if (saw1) o1 += (2.0 * v.ph1 - 1.0) - dsp::polyBlep(v.ph1, i1);
      if (pul1) {
        double sq = v.ph1 < pw1 ? 1.0 : -1.0;
        sq += dsp::polyBlep(v.ph1, i1);
        sq -= dsp::polyBlep(std::fmod(v.ph1 - pw1 + 1.0, 1.0), i1);
        o1 += sq;
      }
      if (sub1) o1 += (v.sub1 < 0.5 ? 1.0 : -1.0) * subLv1;
      v.xm1 = o1;
      // DCO 2.
      double o2 = 0;
      if (dco2) {
        double m2 = x12 > 0 ? xShape(x12s, v.ph1, o1 * 0.5) * x12 * x12 * 4.0 : 0.0;
        const double i2 = inc2 * std::max(0.0, 1.0 + m2);
        v.ph2 += i2;
        if (sync && wrapped) {
          v.ph2 = v.ph1 * (inc2 / std::max(1e-9, inc1));
          v.ph2 -= std::floor(v.ph2);
        } else if (v.ph2 >= 1.0) {
          v.ph2 -= std::floor(v.ph2);
          v.sub2 += 0.5;
          if (v.sub2 >= 1.0) v.sub2 -= 1.0;
        }
        if (saw2) o2 += (2.0 * v.ph2 - 1.0) - (sync ? 0.0 : dsp::polyBlep(v.ph2, i2));
        if (pul2) {
          double sq = v.ph2 < pw2 ? 1.0 : -1.0;
          if (!sync) {
            sq += dsp::polyBlep(v.ph2, i2);
            sq -= dsp::polyBlep(std::fmod(v.ph2 - pw2 + 1.0, 1.0), i2);
          }
          o2 += sq;
        }
        if (sub2) o2 += (v.sub2 < 0.5 ? 1.0 : -1.0) * subLv2;
        v.xm2 = o2;
        o2 *= 1.0 - am + am * o1;
      }
      v.noise = v.noise * 1664525u + 1013904223u;
      const double nz = (static_cast<double>(v.noise >> 9) / 4194304.0 - 1.0) * noiseLv;
      double x = (o1 * lv1 + o2 * lv2) * 0.5 + nz;
      // High pass.
      if (hpOn) {
        v.hp += hpA * (x - v.hp);
        x -= v.hp;
      }
      // Ladder low pass with slope taps.
      double co = cutOct;
      if (fm > 0) co += fm * xShape(fmShape, v.ph1, o1 * 0.5);
      const double hz = std::clamp(std::exp2(co), 15.0, sr_ * 0.45);
      const double g = 1.0 - std::exp(-dsp::kTwoPi * hz / sr_);
      const double k = reso * 4.1;
      const double in = std::tanh(x - k * v.st[3]);
      v.st[0] += g * (in - v.st[0]);
      v.st[1] += g * (v.st[0] - v.st[1]);
      v.st[2] += g * (v.st[1] - v.st[2]);
      v.st[3] += g * (v.st[2] - v.st[3]);
      double y = v.st[slope] * (1.0 + 0.5 * reso);
      if (!std::isfinite(y)) {
        v.st[0] = v.st[1] = v.st[2] = v.st[3] = 0;
        y = 0;
      }
      // VCA.
      double amp;
      if (vcaEnv >= 0) {
        amp = vcaEnv;
      } else {
        v.vcaSmooth += gateCoef * (gateTarget - v.vcaSmooth);
        amp = v.vcaSmooth;
      }
      const double out = y * amp * vgain;
      bufL_[static_cast<size_t>(i)] += static_cast<float>(out);
      bufR_[static_cast<size_t>(i)] += static_cast<float>(out);
    }
  }

  // ------------------------------------------------------------- arpeggiator
  double arpStep() const {
    if (P(DcArpSync) >= 0.5) {
      static const double beats[5] = {1.0, 0.5, 0.25, 0.125, 0.0625};
      return beats[idx(P(DcArpRate), 5)] * 60.0 / tempo_ * sr_;
    }
    return sr_ / expMap(P(DcArpRate), 0.5, 60);
  }
  void arpAdd(uint32_t id, double pitch, float vel) {
    if (arpCount_ >= kArpMax) return;
    if (arpCount_ == 0) {
      arpIndex_ = -1;
      arpDir_ = 1;
      arpCounter_ = 0;
    }
    arpNotes_[static_cast<size_t>(arpCount_++)] = {id, pitch, vel, ++arpSeq_};
  }
  void arpRemove(uint32_t id) {
    if (P(DcHold) >= 0.5) return;
    for (int i = 0; i < arpCount_; ++i)
      if (arpNotes_[static_cast<size_t>(i)].id == id) {
        for (int j = i; j + 1 < arpCount_; ++j) arpNotes_[static_cast<size_t>(j)] = arpNotes_[static_cast<size_t>(j + 1)];
        --arpCount_;
        break;
      }
  }
  void arpStop() {
    if (arpNoteOn_) stopNote(arpVoiceId_);
    arpNoteOn_ = false;
  }
  void arpFire() {
    if (arpNoteOn_) {
      stopNote(arpVoiceId_);
      arpNoteOn_ = false;
    }
    if (arpCount_ == 0) return;
    // Sorted (pitch) or played order, expanded over the octave range.
    const int mode = panelOne_ ? std::clamp(static_cast<int>(std::lround(P(DcArpMode) / 4.0)), 0, 2) : ci(DcArpMode, 9);
    const int range = ci(DcArpRange, 3) + 1;
    std::array<ArpNote, kArpMax> base{};
    for (int i = 0; i < arpCount_; ++i) base[static_cast<size_t>(i)] = arpNotes_[static_cast<size_t>(i)];
    const bool played = mode >= 6;
    if (played)
      std::sort(base.begin(), base.begin() + arpCount_, [](const ArpNote& a, const ArpNote& b) { return a.seq < b.seq; });
    else
      std::sort(base.begin(), base.begin() + arpCount_, [](const ArpNote& a, const ArpNote& b) { return a.pitch < b.pitch; });
    const int total = arpCount_ * range;
    auto noteAt = [&](int k) {
      const ArpNote& a = base[static_cast<size_t>(k % arpCount_)];
      return ArpNote{a.id, a.pitch + 12.0 * (k / arpCount_), a.vel, a.seq};
    };
    int k;
    const int dirMode = played ? mode - 6 : mode;
    if (dirMode == 0) {
      k = (arpIndex_ + 1) % total;
    } else if (dirMode == 1) {
      k = arpIndex_ <= 0 ? total - 1 : arpIndex_ - 1;
    } else if (dirMode == 2) {
      if (total == 1) {
        k = 0;
      } else {
        k = arpIndex_ + arpDir_;
        if (k >= total) {
          arpDir_ = -1;
          k = total - (P(DcArpVariant) >= 0.5 ? 1 : 2);
        } else if (k < 0) {
          arpDir_ = 1;
          k = P(DcArpVariant) >= 0.5 ? 0 : 1;
        }
        k = std::clamp(k, 0, total - 1);
      }
    } else {  // random 1..3: different seeds
      arpRnd_ = arpRnd_ * 1664525u + 1013904223u + static_cast<uint32_t>(dirMode);
      k = static_cast<int>((arpRnd_ >> 8) % static_cast<uint32_t>(total));
    }
    arpIndex_ = k;
    const ArpNote nn = noteAt(k);
    arpVoiceId_ = 0x80000000u | (arpSerial_++ & 0x7fffffffu);
    startNote(arpVoiceId_, nn.pitch, nn.vel);
    arpNoteOn_ = true;
    arpGateLeft_ = std::max(1.0, arpStep() * std::clamp(P(DcArpGate), 0.05, 1.0));
  }

 private:
  static constexpr int kArpMax = 32;
  struct ArpNote {
    uint32_t id;
    double pitch;
    float vel;
    uint64_t seq;
  };
  std::array<float, DcCount> p_{};
  std::array<Voice, kDcVoices> voices_{};
  std::array<double, kDcTags> globalMod_{};
  int mxSrc_[kDcMatrix] = {}, mxDst_[kDcMatrix] = {};
  double mxAmt_[kDcMatrix] = {};
  bool panelOne_ = false;
  double sr_ = 44100, tempo_ = 120;
  double lfoPh_[2] = {0, 0}, lfoFade_[2] = {1, 1};
  double lfoVal_[2] = {0, 0}, lfo1Sin_ = 0;  // LFO values of the current control period
  int ctlPhase_ = 0;                         // position in the control period (own clock, J-30)
  uint64_t lfoCycle_[2] = {0, 0};
  double lastPitch_ = -1, altSign_ = 1;
  int heldCount_ = 0;
  uint64_t clock_ = 0;
  NoteStack stack_;
  std::vector<float> bufL_, bufR_;
  dcofx::BbdChorus chorus_;
  dcofx::ThreeBandEq eq_;
  dcofx::StereoDelay dly_;
  dcofx::Plate plate_;
  dcofx::Ensemble ens_;
  int tail_ = 0;
  // Arpeggiator.
  bool arpOn_ = false, arpNoteOn_ = false;
  std::array<ArpNote, kArpMax> arpNotes_{};
  int arpCount_ = 0, arpIndex_ = -1, arpDir_ = 1;
  uint64_t arpSeq_ = 0;
  uint32_t arpSerial_ = 1, arpVoiceId_ = 0, arpRnd_ = 99;
  double arpCounter_ = 0, arpGateLeft_ = 0;
};

}  // namespace

std::unique_ptr<Generator> makeDcoSynth() { return std::make_unique<DcoSynth>(); }

}  // namespace gl::packs::synths
