// Gridline — "synths" pack, J-33 group "hex": Hex Synth (gl.hexsynth) runtime.
//
// Voice engine (the reference instrument's behaviour, written from its
// analysis; laws in hex_laws.h):
// * Every voice runs a 128-sample control tick: glide, its own copy of the two
//   LFOs, envelope segment logic, oscillator frequencies / levels, filter
//   cutoff. Inside a tick oscillator increments ramp linearly, oscillator
//   levels follow a 4-point cubic through the last tick values (≈ two ticks of
//   lag), the cutoff ramps linearly.
// * Envelopes are one-pole curves chasing targets: attack chases 5.0 (clipped
//   at 1, ≈ the attack time), decay / release fall with time constant T / 8.82.
//   Segment timers advance 128 / 44100 s per tick at any sample rate.
//   Oscillator envelopes start from and release to their "init" level.
// * Oscillators read 256-sample band-limited tables (the level keeps every
//   harmonic below Nyquist) with cubic interpolation, a 32-bit phase, U
//   unison copies (ratio (1 + D / 12U)^(j − (U−1)/2), spread pan), and phase-
//   modulate each other through the 6 × 6 matrix with one sample of delay
//   (index 2·amount cycles per unit of the source's output).
// * Filter: 4 one-pole stages with resonance feedback and a soft clip on the
//   input, low / band / high taps, cutoff index 65535·(cut + key + LFO 1 +
//   velocity terms + env) into a 2^(5 + 9u) Hz table calibrated at 44.1 kHz.
// * Voice allocation, mono legato, two glide modes (rate / time) from the last
//   or the nearest held note, the chord-following step sequencer, the
//   8-slot modulation matrix (velocity, LFOs, controllers at their resting
//   values) and the post chain (hexfx_dsp.cpp) follow the reference.
// Realtime-safe: fixed voice array; tables are prepared on the UI thread.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include "../Packs.h"
#include "hex_fx.h"
#include "hex_laws.h"
#include "hex_rt.h"

namespace gl::packs::synths {

namespace {

using namespace hex;

constexpr int kTick = 128;
constexpr int kMaxVoices = 32;
constexpr int kMaxCopies = 8;
constexpr double kTickSec = 128.0 / 44100.0;

struct Lcg {
  uint32_t s = 0x8AE;
  uint32_t next() { return s = s * 0x0BB38435u + 0x3619636Bu; }
  double unit() { return next() * (1.0 / 4294967296.0); }
};

// Chord tables of the sequencer (degree → semitones; [12] = octave-shifted root).
constexpr int kChordT[8][13] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 0},   // none (chromatic)
    {0, 1, 2, 3, 3, 5, 6, 7, 8, 8, 10, 10, 0},   // minor
    {0, 1, 2, 4, 4, 5, 6, 7, 9, 9, 11, 11, 0},   // major
    {0, 1, 2, 3, 3, 5, 6, 7, 8, 10, 10, 10, -2}, // minor 7
    {0, 1, 2, 4, 4, 5, 6, 7, 8, 10, 10, 10, -2}, // dominant 7
    {0, 1, 2, 3, 3, 5, 6, 7, 8, 11, 11, 11, -1}, // minor / major 7
    {0, 1, 2, 4, 4, 5, 6, 7, 8, 11, 11, 11, -1}, // major 7
    {0, 1, 2, 3, 3, 5, 6, 6, 8, 8, 10, 10, 0},   // diminished
};

class HexSynthGen final : public Generator {
 public:
  HexSynthGen() {
    for (int i = 0; i < HxCount; ++i) x_[i] = e_[i] = 0;
    cc_[7] = 1.f, cc_[10] = 0.5f;
    for (auto& h : held_) h = 0;
    for (auto& s : seqSound_) s = -1;
  }
  bool active() const override {
    if (postAwake_) return true;
    for (const auto& v : voice_)
      if (v.active) return true;
    return false;
  }

 protected:
  struct Copy {
    uint32_t ph = 0, inc = 0;
    int64_t incStep = 0;
    float gL = 0, gR = 0, gTL = 0, gTR = 0, out = 0, acc = 0;
  };
  struct Osc {
    bool on = false;
    float env = 0, envTgt = 0, envRate = 0;
    float a = 0.001f, d = 0.001f, s = 1, r = 0.001f, init = 0, vsens = 1, pan = 0.5f;
    float hist[4] = {};
    float lc[4] = {};  // cubic coefficients of the level across the tick
    int level = 0;
    uint32_t incTarget[kMaxCopies] = {};
    Copy c[kMaxCopies];
  };
  struct Voice {
    bool active = false, released = false, first = true, fresh = true;
    uint32_t id = 0;
    int key = -1;
    double frac = 0;  // fractional part of the note (channel detune)
    uint64_t age = 0;
    float gv = 1, vs = 1;
    double glidePos = 60, glideTarget = 60, glideRate = 0;
    double t = 0, tr = 0;
    float amp = 0, ampTgt = 0, ampRate = 0, A = 0.001f, D = 0.001f, S = 1, R = 0.02f;
    float fenv = 0, fTgt = 0, fRate = 0, fA = 0.001f, fD = 0.001f, fS = 1, fR = 0.001f;
    int tickLeft = 0, tickPos = 0;
    int fadeTicks = 0;
    int pendKey = -1;
    float pendVel = 0;
    double pendStart = 60, pendRate = 0;
    double lfoPh[2] = {0, 0};
    float lfo[2] = {0, 0};
    float ibPrev = 0, ibTgt = 0;
    int uni = 1;
    double ratio[kMaxCopies] = {};
    float spread[kMaxCopies] = {};
    Osc osc[kOscs];
    float st[2][4] = {}, inPrev[2][4] = {};
    float w[4] = {0, 0, 0, 1};
    bool wInit = false;
  };

  // ---------------------------------------------------------------- params
  void configure(const PluginState& s, const ChannelAux& aux, const ProcessContext& ctx) override {
    const double sr = ctx.sampleRate > 0 ? ctx.sampleRate : 44100.0;
    if (sr != sr_) {
      sr_ = sr;
      post_.configure(sr_);
      smoothInit_ = false;
    }
    bpm_ = ctx.tempo > 0 ? ctx.tempo : 120.0;
    aux_ = dynamic_cast<const HexAux*>(aux.custom.get());
    for (int i = 0; i < HxCount; ++i) {
      const float v = i < static_cast<int>(s.params.size()) ? s.params[static_cast<size_t>(i)] : 0.f;
      x_[i] = std::isfinite(v) ? v : 0.f;
    }
    const bool seqOn = x_[HxSeqPlay] >= 0.5f;
    if (seqOn != seqOn_) {
      // Switching the sequencer clears every held key and voice.
      for (int k = 0; k < 128; ++k) held_[k] = 0;
      for (auto& v : voice_) releaseVoice(v);
      seqStop();
      seqOn_ = seqOn;
    }
    const int poly = std::clamp(static_cast<int>(std::lround(x_[HxVoices])), 0, kMaxVoices);
    if ((poly == 0) != (poly_ == 0) && poly_ >= 0)
      for (auto& v : voice_) releaseVoice(v);
    poly_ = poly;
    seqDirty_ = true;
  }

  // Effective value (modulation matrix applied) of parameter p.
  float e(int p) const { return e_[p]; }

  void applyMods() {
    std::memcpy(e_, x_, sizeof e_);
    float src[kModSources] = {};
    src[1] = lastVel_;
    src[2] = glfoVal_[0];
    src[3] = glfoVal_[1];
    for (int c = 0; c < 128; ++c) src[7 + c] = cc_[c];
    for (int k = 0; k < kModSlots; ++k) {
      const int code = static_cast<int>(std::lround(x_[HxModSource + k]));
      const int tgt = static_cast<int>(std::lround(x_[HxModTarget + k])) - 1;
      if (code <= 0 || code >= kModSources || tgt < 0 || tgt >= kAutomatable) continue;
      e_[tgt] += src[code] * static_cast<float>(law::modAmount(x_[HxModAmount + k]));
    }
    for (int k = 0; k < kModSlots; ++k) {
      const int tgt = static_cast<int>(std::lround(x_[HxModTarget + k])) - 1;
      if (tgt >= 0 && tgt < kAutomatable) e_[tgt] = std::clamp(e_[tgt], 0.f, 1.f);
    }
  }

  // ------------------------------------------------------------- notes
  void onNoteOn(uint32_t id, double pitch, float velocity) override {
    const int key = std::clamp(static_cast<int>(std::lround(pitch)), 0, 127);
    noteFrac_ = pitch - key;
    if (idCount_ < static_cast<int>(ids_.size())) ids_[static_cast<size_t>(idCount_++)] = {id, key};
    const int vel = std::clamp(static_cast<int>(velocity * 127.f), 1, 127);
    held_[key] = static_cast<uint8_t>(vel);
    heldOrder_[key] = ++heldClock_;
    if (seqOn_) {
      seqDirty_ = true;
      return;
    }
    internalOn(key, vel / 127.f);
  }
  void onNoteOff(uint32_t id) override {
    int key = -1;
    for (int i = 0; i < idCount_; ++i)
      if (ids_[static_cast<size_t>(i)].first == id) {
        key = ids_[static_cast<size_t>(i)].second;
        ids_[static_cast<size_t>(i)] = ids_[static_cast<size_t>(--idCount_)];
        break;
      }
    if (key < 0) return;
    held_[key] = 0;
    if (seqOn_) {
      seqDirty_ = true;
      return;
    }
    internalOff(key);
  }
  void onAllOff() override {
    for (int k = 0; k < 128; ++k) held_[k] = 0;
    idCount_ = 0;
    for (auto& v : voice_) releaseVoice(v);
    seqStop();
  }
  void onKill() override {
    for (auto& v : voice_) killVoice(v);
    for (int k = 0; k < 128; ++k) held_[k] = 0;
    idCount_ = 0;
    seqStop();
    post_.reset();
  }

  double glideSeconds() const { return law::glideTime(e(HxGlide)); }

  int nearestHeld(int key, bool excludeSelf) const {
    int best = -1, bd = 1000;
    for (int k = 0; k < 128; ++k) {
      if (!held_[k] || (excludeSelf && k == key)) continue;
      const int d = std::abs(k - key);
      if (d < bd) bd = d, best = k;  // ties: the lower key (scanned first)
    }
    return best;
  }

  void internalOn(int key, float vel) {
    rng_.next();
    const int mode = std::clamp(static_cast<int>(std::lround(x_[HxGlideMode])), 0, 3);
    const double tg = glideSeconds();
    const bool glide = tg >= 0.001;
    const bool bend = mode == 0 || mode == 2, sameTime = mode >= 2;
    if (poly_ == 0) {  // mono
      Voice& v = voice_[0];
      if (v.active && !v.released && v.fadeTicks == 0) {
        v.key = key;
        v.frac = noteFrac_;
        v.glideTarget = key;
        if (!glide) v.glidePos = key, v.glideRate = 0;
        else v.glideRate = sameTime ? std::fabs(key - v.glidePos) / (tg * sr_) : 12.0 / (tg * sr_);
        lastNote_ = key;
        return;
      }
      double start = key;
      if (glide && bend && lastNote_ >= 0) start = lastNote_;
      const double rate = !glide ? 0.0 : (sameTime ? std::fabs(key - start) / (tg * sr_) : 12.0 / (tg * sr_));
      startOrSteal(v, key, vel, start, rate);
      lastNote_ = key;
      return;
    }
    double start = key;
    if (glide) {
      if (bend) {
        if (lastNote_ >= 0) start = lastNote_;
      } else {
        const int h = nearestHeld(key, true);
        if (h >= 0) start = h;
      }
    }
    const double rate = !glide ? 0.0 : (sameTime ? std::fabs(key - start) / (tg * sr_) : 12.0 / (tg * sr_));
    // Allocation among the first `poly` voices: a free one, else the oldest
    // released one older than the list head, else the oldest held one.
    const int n = std::max(1, poly_);
    Voice* pick = nullptr;
    for (int i = 0; i < n; ++i)
      if (!voice_[i].active) {
        pick = &voice_[i];
        break;
      }
    if (!pick) {
      Voice* cand = &voice_[0];
      for (int i = 0; i < n; ++i)
        if (voice_[i].released && voice_[i].age < cand->age) cand = &voice_[i];
      if (!cand->released) {
        cand = &voice_[0];
        for (int i = 0; i < n; ++i)
          if (voice_[i].age < cand->age) cand = &voice_[i];
      }
      pick = cand;
    }
    startOrSteal(*pick, key, vel, start, rate);
    lastNote_ = key;
  }

  void internalOff(int key) {
    if (poly_ == 0) {
      Voice& v = voice_[0];
      if (!v.active || v.released) return;
      const int h = nearestHeld(key, false);
      if (h >= 0) {  // legato to the nearest held key, rate-based glide
        const double tg = glideSeconds();
        v.key = h;
        v.glideTarget = h;
        if (tg < 0.001) v.glidePos = h, v.glideRate = 0;
        else v.glideRate = 12.0 / (tg * sr_);
        return;
      }
      releaseVoice(v);
      return;
    }
    for (auto& v : voice_)
      if (v.active && !v.released && v.key == key) releaseVoice(v);
  }

  void startOrSteal(Voice& v, int key, float vel, double start, double rate) {
    if (v.active && (v.t != 0 || v.tr != 0)) {  // fast fade before the restart
      v.fadeTicks = std::max(1, static_cast<int>(sr_ * 4.0 / 44100.0));
      v.pendKey = key, v.pendVel = vel, v.pendStart = start, v.pendRate = rate;
      v.age = ++ageClock_;
      return;
    }
    if (!v.active) v.fresh = true;
    beginNote(v, key, vel, start, rate);
  }

  void beginNote(Voice& v, int key, float vel, double start, double rate) {
    v.active = true;
    v.released = false;
    v.key = key;
    v.frac = noteFrac_;
    v.age = ++ageClock_;
    v.gv = static_cast<float>(std::pow(static_cast<double>(vel), law::velExponent(e(HxVelCurve))));
    lastVel_ = v.gv;
    applyMods();  // velocity-driven modulation takes effect for this note's latched values
    v.glidePos = start;
    v.glideTarget = key;
    v.glideRate = rate;
    v.t = v.tr = 0;
    v.fadeTicks = 0;
    v.tickLeft = 0;
    v.lfoPh[0] = glfoPh_[0], v.lfoPh[1] = glfoPh_[1];
    v.A = static_cast<float>(std::max(law::time(e(HxAmpA)), 0.001));
    v.D = static_cast<float>(std::max(law::time(e(HxAmpD)), 0.001));
    v.S = static_cast<float>(law::squared(e(HxAmpS)));
    v.fA = static_cast<float>(std::max(law::time(e(HxFltA)), 0.001));
    v.fD = static_cast<float>(std::max(law::time(e(HxFltD)), 0.001));
    v.fS = static_cast<float>(law::squared(e(HxFltS)));
    v.uni = law::unisonCopies(static_cast<int>(std::lround(x_[HxUnison])));
    const double D = e(HxUniDetune), P = std::clamp(static_cast<double>(e(HxUniPan)), 0.0, 1.0);
    const double r = 1.0 + D / (12.0 * v.uni);
    for (int j = 0; j < v.uni; ++j) {
      v.ratio[j] = std::pow(r, j - (v.uni - 1) * 0.5);
      v.spread[j] = v.uni > 1 ? static_cast<float>((2.0 * j / (v.uni - 1) - 1.0) * P) : 0.f;
    }
    bool any = false;
    for (int k = 0; k < kOscs; ++k) {
      Osc& o = v.osc[k];
      o.on = e(HxOscOn + k) > 0.5f;
      any = any || o.on;
      o.a = static_cast<float>(std::max(law::time(e(HxOscA + k)), 0.001));
      o.d = static_cast<float>(std::max(law::time(e(HxOscD + k)), 0.001));
      o.s = static_cast<float>(law::squared(e(HxOscS + k)));
      o.init = static_cast<float>(law::squared(e(HxInit + k)));
      o.vsens = e(HxVelSens + k);
      o.pan = e(HxPan + k);
      o.env = std::max(o.init, 1e-7f);
      o.envTgt = o.env, o.envRate = 0;
    }
    if (!any) {
      killVoice(v);
      return;
    }
    v.first = true;
  }

  void releaseVoice(Voice& v) {
    if (!v.active || v.released) return;
    if (v.fadeTicks > 0) {  // the pending note never started
      v.fadeTicks = 0;
    }
    v.released = true;
    v.tr = 0;
    v.R = static_cast<float>(law::releaseTime(e(HxAmpR)));
    v.fR = static_cast<float>(std::max(law::time(e(HxFltR)), 0.001));
    for (int k = 0; k < kOscs; ++k) v.osc[k].r = static_cast<float>(std::max(law::time(e(HxOscR + k)), 0.001));
  }

  void killVoice(Voice& v) {
    v.active = false;
    v.released = false;
    v.key = -1;
    v.amp = 0, v.fenv = 0;
    v.t = v.tr = 0;
    v.fadeTicks = 0;
    v.fresh = true;
    v.wInit = false;
    for (auto& s : v.st)
      for (float& y : s) y = 0;
    for (auto& s : v.inPrev)
      for (float& y : s) y = 0;
    for (auto& o : v.osc)
      for (auto& c : o.c) c.acc = 0, c.out = 0, c.gL = c.gR = 0;
  }

  // -------------------------------------------------------------- LFOs
  float lfoValue(int which, double ph) const {
    if (!aux_ || !aux_->slot[kOscs + which]) return 0.f;
    const float* t = aux_->slot[kOscs + which]->t[0];
    const double p = ph * 255.0;
    const int i = std::clamp(static_cast<int>(std::floor(p)), 0, 255);
    return cubicAt(t, i, static_cast<float>(p - i));
  }
  double lfoHz(int which) const {
    const int sync = static_cast<int>(std::lround(x_[which == 0 ? HxLfo1Sync : HxLfo2Sync]));
    if (sync > 0) return bpm_ / law::syncDivisor(sync);
    return law::lfoHz(e(which == 0 ? HxLfo1Speed : HxLfo2Speed));
  }

  // ----------------------------------------------------------- per block
  void blockStart() {
    // Smoothed controls (move a step per 128-sample block).
    const double c = std::min(1.0, 4410.0 / sr_);
    auto target = [&](int i) -> double {
      switch (i) {
        case 0: return std::max(1e-7, static_cast<double>(e(HxCutoff)));
        case 1: return std::max(1e-7, static_cast<double>(e(HxReso)));
        case 2: return std::max(1e-7, static_cast<double>(e(HxKeyTrack)));
        case 3: return std::max(1e-7, static_cast<double>(e(HxVelTrack)));
        case 4: return law::fltEnvAmount(e(HxFltEnvAmt));
        case 5: return std::max(1e-7, law::fltLfo1(e(HxFltLfo1)));
        default: return law::shiftHz(e(HxShift + (i - 6)));
      }
    };
    for (int i = 0; i < 12; ++i) {
      const double t = target(i);
      if (!smoothInit_) sm_[i] = t;
      else {
        sm_[i] += (t - sm_[i]) * c;
        if (std::fabs(sm_[i]) < 1e-9) sm_[i] = 0;
      }
    }
    smoothInit_ = true;
    // Global LFOs.
    bool anyVoice = false;
    for (const auto& v : voice_) anyVoice = anyVoice || v.active;
    for (int k = 0; k < 2; ++k) {
      if (x_[k == 0 ? HxLfo1Free : HxLfo2Free] == 0.f && !anyVoice) glfoPh_[k] = 0;
      glfoInc_[k] = lfoHz(k) / sr_;
      glfoPh_[k] += glfoInc_[k] * kTick;
      if (glfoPh_[k] > 1.0) glfoPh_[k] -= 1.0;
      glfoVal_[k] = lfoValue(k, glfoPh_[k]);
    }
    applyMods();
    if (seqOn_) seqBlock();
    // Post chain controls.
    post_.setDrive(law::drive(e(HxDrive)));
    float eq[kEqBands];
    for (int b = 0; b < kEqBands; ++b) eq[b] = static_cast<float>(law::eqDb(e(HxEqBand + b)));
    post_.setEq(e(HxEqOn) != 0.f, eq);
    for (int s = 0; s < 2; ++s) {
      post_.slot[s].setType(std::clamp(static_cast<int>(std::lround(x_[HxFxType + s])), 0, FxKinds - 1));
      post_.slot[s].setEnabled(e(HxFxOn + s) != 0.f);
      post_.slot[s].setParams(e_, s);
    }
    gainTarget_ = static_cast<float>(law::gain(e(HxGain)) * 0.5 * cc_[7]);
  }

  // ------------------------------------------------------------ sequencer
  void seqStop() {
    for (int s = 0; s < 8; ++s)
      if (seqSound_[s] >= 0) internalOff(seqSound_[s]), seqSound_[s] = -1;
    seqStopped_ = true;
    seqIndex_ = 0;
    seqPhase_ = 0;
    seqParity_ = false;
  }

  void seqBuild() {
    seqDirty_ = false;
    int low = -1;
    bool pc[12] = {};
    for (int k = 0; k < 128; ++k)
      if (held_[k]) {
        if (low < 0) low = k;
        pc[k % 12] = true;
      }
    seqVel_ = low >= 0 ? held_[low] / 127.f : 0.f;
    seqRootKey_ = low;
    // Chord detection (later rules override earlier ones; within a rule the
    // highest matching root wins).
    int type = 0, root = 0;
    auto has = [&](int i, int d) { return pc[(i + d) % 12]; };
    struct Rule {
      int a, b, c, t;
    };
    static const Rule rules[] = {{3, -1, -1, 1}, {4, -1, -1, 2}, {3, 7, -1, 1},  {4, 7, -1, 2},  {3, 10, -1, 3},
                                 {4, 10, -1, 4}, {3, 7, 10, 3},  {4, 7, 10, 4},  {3, 11, -1, 5}, {4, 11, -1, 6},
                                 {3, 7, 11, 5},  {4, 7, 11, 6},  {3, 6, -1, 7}};
    for (const Rule& r : rules)
      for (int i = 0; i < 12; ++i)
        if (pc[i] && has(i, r.a) && (r.b < 0 || has(i, r.b)) && (r.c < 0 || has(i, r.c))) type = r.t, root = i;
    const int steps = law::seqSteps(e(HxSeqSteps));
    const bool dual = e(HxSeqDual) >= 0.5f;
    seqCount_ = dual ? std::min(steps, 32) : steps;
    auto stepNote = [&](int i) {
      const int v = law::seqStep(e(HxSeqStep + i));
      if (v == -26) return -1;
      if (v == -25) return -2;
      if (low < 0) return -1;
      const int o = static_cast<int>(std::floor(v / 12.0));
      const int d = v - 12 * o;
      const int idx = o == 0 ? d : d + 12;
      const int t = idx == 12 ? kChordT[type][12] : kChordT[type][idx >= 12 ? idx - 12 : idx];
      int note = 12 * (o + low / 12) + root + t;
      while (note < 0) note += 12;
      if (note >= 121) note = note % 12 + 108;
      return note;
    };
    for (int i = 0; i < 64; ++i) {
      seqNote_[i][0] = stepNote(i);
      seqNote_[i][1] = dual && i + 32 < 64 ? stepNote(i + 32) : -1;
    }
  }

  void seqBlock() {
    if (seqDirty_) seqBuild();
    if (seqRootKey_ < 0) {  // no key held: stop
      if (!seqStopped_) seqStop();
      return;
    }
    if (seqStopped_) {
      seqStopped_ = false;
      seqIndex_ = 0, seqPhase_ = 0, seqParity_ = false, seqTrig_ = true, seqDir_ = 1;
    }
    if (seqTrig_) {
      seqTrig_ = false;
      const int n = std::max(1, seqCount_);
      if (seqIndex_ >= n || seqIndex_ < 0) seqIndex_ = 0;
      for (int s = 0; s < 2; ++s) {
        const int note = seqNote_[seqIndex_][s];
        if (note == -2) continue;
        if (seqSound_[s] >= 0) internalOff(seqSound_[s]);
        seqSound_[s] = -1;
        if (note >= 0) {
          internalOn(std::min(note, 127), seqVel_);
          seqSound_[s] = std::min(note, 127);
        }
      }
      // Next step.
      const bool rnd = e(HxSeqRandom) >= 0.5f, pp = e(HxSeqPingPong) >= 0.5f;
      if (rnd) {
        seqIndex_ = std::min(n - 1, static_cast<int>(n * rng_.unit()));
      } else if (pp && n > 1) {
        if (seqIndex_ + seqDir_ >= n || seqIndex_ + seqDir_ < 0) seqDir_ = -seqDir_;
        seqIndex_ += seqDir_;
      } else {
        seqIndex_ = seqIndex_ + 1 >= n ? 0 : seqIndex_ + 1;
      }
    }
    seqPhase_ += kTick * bpm_ / (sr_ * 15.0);
    const bool swing = e(HxSeqSwing) >= 0.5f;
    const double len = swing ? (seqParity_ ? 0.75 : 1.25) : 1.0;
    if (seqPhase_ >= len) {
      seqPhase_ -= len;
      seqParity_ = !seqParity_;
      seqTrig_ = true;
    }
  }

  // ------------------------------------------------------------- voices
  void controlTick(Voice& v) {
    if (v.fadeTicks > 0) {
      if (--v.fadeTicks == 0) {
        noteFrac_ = v.frac;
        beginNote(v, v.pendKey, v.pendVel, v.pendStart, v.pendRate);
        if (!v.active) return;
      } else {
        v.ampTgt = 0, v.ampRate = static_cast<float>(147.0 / sr_);
        v.fTgt = 0, v.fRate = v.ampRate;
        for (auto& o : v.osc) o.envTgt = o.init, o.envRate = v.ampRate;
      }
    }
    if (v.released && v.tr > v.R) {
      killVoice(v);
      return;
    }
    // Glide.
    if (v.glidePos != v.glideTarget) {
      const double step = kTick * v.glideRate;
      if (v.glideRate <= 0 || std::fabs(v.glideTarget - v.glidePos) <= step) v.glidePos = v.glideTarget;
      else v.glidePos += v.glideTarget > v.glidePos ? step : -step;
    }
    // Voice LFOs.
    for (int k = 0; k < 2; ++k) {
      v.lfoPh[k] += glfoInc_[k] * kTick;
      if (v.lfoPh[k] >= 1.0) v.lfoPh[k] -= 1.0;
      v.lfo[k] = lfoValue(k, v.lfoPh[k]);
    }
    v.vs = v.first ? v.gv : v.vs + (v.gv - v.vs) * 0.05f;
    const double S = v.glidePos + v.frac + law::transpose(e(HxTranspose));
    const double F = 220.0 * std::exp2(S / 12.0 - 4.75);
    if (v.fadeTicks == 0) {
      const double ar = 0.2236212 / sr_;
      if (!v.released) {
        if (v.t < v.A) v.ampTgt = 5.f, v.ampRate = static_cast<float>(ar / v.A);
        else v.ampTgt = std::max(v.S, 1e-7f), v.ampRate = static_cast<float>(8.82 / (sr_ * v.D));
        if (v.t < v.fA) v.fTgt = 5.f, v.fRate = static_cast<float>(ar / v.fA);
        else v.fTgt = v.fS + 1e-7f, v.fRate = static_cast<float>(8.82 / (sr_ * v.fD));
        for (auto& o : v.osc) {
          if (v.t < o.a) o.envTgt = 5.f, o.envRate = static_cast<float>(ar / o.a);
          else o.envTgt = o.s, o.envRate = static_cast<float>(8.82 / (sr_ * o.d));
        }
      } else {
        v.ampTgt = 1e-7f, v.ampRate = static_cast<float>(8.82 / (sr_ * v.R));
        v.fTgt = 0, v.fRate = static_cast<float>(8.82 / (sr_ * v.fR));
        for (auto& o : v.osc) o.envTgt = o.init, o.envRate = static_cast<float>(8.82 / (sr_ * o.r));
      }
    }
    // Oscillators.
    const double egf = law::transpose(e(HxEnvToPitch)) / 12.0;
    const double l2p = law::lfo2Pitch(e(HxLfo2ToPitch)) / 12.0;
    for (int k = 0; k < kOscs; ++k) {
      Osc& o = v.osc[k];
      if (!o.on) continue;
      const double mult = std::exp2(egf * v.fenv + l2p * v.lfo[1] + (law::coarse(e(HxCoarse + k)) + law::fine(e(HxFine + k))) / 12.0);
      const double shift = sm_[6 + k];
      double f0 = 0;
      for (int j = 0; j < v.uni; ++j) {
        double f = F * v.ratio[j] * mult + shift;
        if (j == 0) f0 = f;
        f = std::clamp(f, 0.0, 0.49 * sr_);
        o.incTarget[j] = static_cast<uint32_t>(f * 4294967295.0 / sr_);
        if (v.first) {
          o.c[j].inc = o.incTarget[j];
          o.c[j].incStep = 0;
        } else {
          o.c[j].incStep = (static_cast<int64_t>(o.incTarget[j]) - static_cast<int64_t>(o.c[j].inc)) / kTick;
        }
      }
      // Wave table level for copy 0's frequency.
      if (f0 <= sr_ / 256.0) o.level = 0;
      else {
        const int harm = std::clamp(static_cast<int>(std::floor(sr_ / (2.0 * f0))), 1, 128);
        o.level = 128 - harm;
      }
      // Level (tremolo × velocity) and its cubic across the tick.
      float L = static_cast<float>((1.0 + law::lfoOsc(e(HxLfo1Osc + k), k) * v.lfo[0] + law::lfoOsc(e(HxLfo2Osc + k), k) * v.lfo[1]) *
                                   ((v.vs - 0.5) * o.vsens + 0.5));
      if (e(HxOscOn + k) <= 0.5f) L = 1e-5f;
      if (v.first) o.hist[0] = 0, o.hist[1] = 0, o.hist[2] = L, o.hist[3] = L;
      else o.hist[0] = o.hist[1], o.hist[1] = o.hist[2], o.hist[2] = o.hist[3], o.hist[3] = L;
      const float p = o.hist[0], y0 = o.hist[1], y1 = o.hist[2], y2 = o.hist[3];
      o.lc[1] = (y1 - p) * 0.5f;
      o.lc[3] = (y2 - p) * 0.5f + 1.5f * (y0 - y1);
      o.lc[2] = p + o.lc[1] - y0 - o.lc[3];
      o.lc[0] = y0;
      // Pan gain targets (pan latched at note start, level live).
      const float mix = static_cast<float>(law::squared(e(HxMix + k)));
      for (int j = 0; j < v.uni; ++j) {
        const float sp = v.spread[j];
        o.c[j].gTL = std::max(1e-7f, 0.5f * std::clamp(1.f + sp, 0.f, 1.f) * 2.f * (1.f - o.pan) * mix);
        o.c[j].gTR = std::max(1e-7f, 0.5f * std::clamp(1.f - sp, 0.f, 1.f) * 2.f * o.pan * mix);
      }
    }
    // Phases of a voice that was idle.
    if (v.first && v.fresh) {
      for (int k = 0; k < kOscs; ++k) {
        const bool free = e(HxFreeRun + k) != 0.f;
        const uint32_t ph = static_cast<uint32_t>(static_cast<double>(e(HxPhase + k)) * 4294967296.0);
        for (int j = 0; j < v.uni; ++j) v.osc[k].c[j].ph = free ? rng_.next() : ph;
      }
      v.fresh = false;
    }
    // Filter cutoff index.
    const float ib = static_cast<float>(65535.0 * (sm_[0] + sm_[2] * (S * 0.01 - 0.6) + sm_[5] * v.lfo[0] + sm_[3] * (v.vs * 0.5 - 0.5)));
    v.ibPrev = v.first ? ib : v.ibTgt;
    v.ibTgt = ib;
    // Timers.
    if (!v.released) v.t += kTickSec;
    else v.tr += kTickSec;
    v.first = false;
    v.tickLeft = kTick;
    v.tickPos = 0;
  }

  void renderVoice(Voice& v, float* outL, float* outR, int n) {
    const WaveTable* tab[kOscs] = {};
    for (int k = 0; k < kOscs; ++k) tab[k] = aux_ && aux_->slot[k] ? aux_->slot[k].get() : nullptr;
    const FilterCoef* fct = filterTable();
    const int ftype = std::clamp(static_cast<int>(std::lround(x_[HxFilterType])), 0, 3);
    static const float kW[4][4] = {{0, 0, 0, 1}, {0, 0, 1, 1e-7f}, {0, 1, -1, 1e-7f}, {1, 0, -1, 1e-7f}};
    const float* wt = kW[ftype];
    const float res = static_cast<float>(std::clamp(sm_[1], 0.0, 1.0));
    const float ea = static_cast<float>(sm_[4]);
    const float gc = static_cast<float>(0.001 * 44100.0 / sr_);
    float fm[kOscs][kOscs];
    for (int d = 0; d < kOscs; ++d)
      for (int s = 0; s < kOscs; ++s) fm[d][s] = static_cast<float>(2.0 * law::fm(e(HxFm + 6 * d + s)));
    int i = 0;
    while (i < n && v.active) {
      if (v.tickLeft <= 0) {
        controlTick(v);
        if (!v.active) break;
      }
      const int len = std::min(n - i, v.tickLeft);
      for (int q = 0; q < len; ++q, ++i) {
        const float tf = static_cast<float>(v.tickPos) / kTick;
        float sumL = 0, sumR = 0;
        for (int k = 0; k < kOscs; ++k) {
          Osc& o = v.osc[k];
          if (!o.on || !tab[k]) continue;
          const float lvl = ((o.lc[3] * tf + o.lc[2]) * tf + o.lc[1]) * tf + o.lc[0];
          const float env = o.env;
          o.env += (o.envTgt - o.env) * o.envRate;
          if (o.env > 1.f) o.env = 1.f;
          const float* row = tab[k]->t[o.level];
          for (int j = 0; j < v.uni; ++j) {
            Copy& c = o.c[j];
            const uint32_t pe = c.ph + static_cast<uint32_t>(static_cast<int32_t>(c.acc * 16777215.0f)) * 256u;
            c.acc = 0;
            const float s = cubicAt(row, static_cast<int>(pe >> 24), static_cast<float>(pe & 0xFFFFFF) * (1.0f / 16777216.0f));
            const float out = s * lvl * env;
            c.out = out;
            const float gl = c.gL, gr = c.gR;
            sumL += out * gl;
            sumR += out * gr;
            c.gL = c.gL < 1e-7f ? c.gTL : c.gL + (c.gTL - c.gL) * gc;
            c.gR = c.gR < 1e-7f ? c.gTR : c.gR + (c.gTR - c.gR) * gc;
            c.ph += c.inc;
            c.inc = static_cast<uint32_t>(static_cast<int64_t>(c.inc) + c.incStep);
          }
        }
        // Phase modulation for the next sample.
        for (int d = 0; d < kOscs; ++d) {
          Osc& od = v.osc[d];
          if (!od.on) continue;
          for (int s = 0; s < kOscs; ++s) {
            const float a = fm[d][s];
            if (a <= 1e-5f || !v.osc[s].on) continue;
            for (int j = 0; j < v.uni; ++j) od.c[j].acc += a * v.osc[s].c[j].out;
          }
        }
        // Filter.
        const float fenv = v.fenv;
        v.fenv += (v.fTgt - v.fenv) * v.fRate;
        if (v.fenv > 1.f) v.fenv = 1.f;
        const float ibase = v.ibPrev + (v.ibTgt - v.ibPrev) * tf;
        const float I = ibase + 65535.f * ea * fenv;
        const FilterCoef& fc = fct[std::clamp(static_cast<int>(I), 0, 65535)];
        if (!v.wInit) {
          for (int w = 0; w < 4; ++w) v.w[w] = wt[w];
          v.wInit = true;
        } else {
          for (int w = 0; w < 4; ++w) v.w[w] += (wt[w] - v.w[w]) * 0.005f;
        }
        float in2[2] = {sumL, sumR};
        if (v.w[3] < 0.999999f) {
          for (int ch = 0; ch < 2; ++ch) {
            const float x = in2[ch];
            float u = x - fc.feedback * res * v.st[ch][3];
            const float dd = std::min(std::fabs(u) / 15.f, 1.f);
            u *= 1.f - 3.f * dd + 4.f * dd * dd - 2.f * dd * dd * dd;
            float in = u;
            for (int s = 0; s < 4; ++s) {
              const float y = (in + v.inPrev[ch][s]) * fc.halfG + fc.oneMinusG * v.st[ch][s];
              v.inPrev[ch][s] = in;
              v.st[ch][s] = (y < 1e-15f && y > -1e-15f) ? 0.f : y;
              in = v.st[ch][s];
            }
            const float out = v.w[0] * u + v.w[1] * v.st[ch][2] + v.w[2] * v.st[ch][3];
            in2[ch] = (x - out) * v.w[3] + out;
          }
        }
        const float amp = v.amp;
        v.amp += (v.ampTgt - v.amp) * v.ampRate;
        if (v.amp > 1.f) v.amp = 1.f;
        outL[i] += in2[0] * amp;
        outR[i] += in2[1] * amp;
        ++v.tickPos;
      }
      v.tickLeft -= len;
    }
  }

  void render(float* L, float* R, int n) override {
    int i = 0;
    while (i < n) {
      if (grid_ == 0) {
        blockStart();
        bool awake = false;
        for (const auto& v : voice_) awake = awake || v.active;
        postAwake_ = awake || post_.awake();
      }
      const int len = std::min(n - i, kTick - grid_);
      float bl[kTick], br[kTick];
      std::fill(bl, bl + len, 0.f);
      std::fill(br, br + len, 0.f);
      for (auto& v : voice_)
        if (v.active) renderVoice(v, bl, br, len);
      post_.process(bl, br, len, bpm_);
      for (int q = 0; q < len; ++q) {
        L[i + q] += bl[q] * gainL_;
        R[i + q] += br[q] * gainR_;
        gainL_ += (gainTarget_ - gainL_) * 0.005f;
        gainR_ += (gainTarget_ - gainR_) * 0.005f;
      }
      i += len;
      grid_ = (grid_ + len) % kTick;
    }
  }

 private:
  double sr_ = 0, bpm_ = 120;
  const HexAux* aux_ = nullptr;
  float x_[HxCount], e_[HxCount];
  float cc_[128] = {};
  uint8_t held_[128];
  uint64_t heldOrder_[128] = {};
  uint64_t heldClock_ = 0, ageClock_ = 0;
  std::array<std::pair<uint32_t, int>, 256> ids_{};
  int idCount_ = 0;
  double noteFrac_ = 0;
  int poly_ = -1, lastNote_ = -1;
  float lastVel_ = 0;
  Lcg rng_;
  Voice voice_[kMaxVoices];
  double sm_[12] = {};
  bool smoothInit_ = false;
  double glfoPh_[2] = {0, 0}, glfoInc_[2] = {0, 0};
  float glfoVal_[2] = {0, 0};
  int grid_ = 0;
  float gainTarget_ = 0.5f, gainL_ = 0, gainR_ = 0;
  bool postAwake_ = false;
  PostChain post_;
  // Sequencer.
  bool seqOn_ = false, seqDirty_ = true, seqStopped_ = true, seqTrig_ = false, seqParity_ = false;
  int seqIndex_ = 0, seqDir_ = 1, seqCount_ = 16, seqRootKey_ = -1;
  double seqPhase_ = 0;
  float seqVel_ = 0;
  int seqNote_[64][2] = {};
  int seqSound_[8];
};

}  // namespace

std::unique_ptr<Generator> makeHexSynth() { return std::make_unique<HexSynthGen>(); }

bool prepareHexAux(const PluginState& state, ChannelAux& aux) {
  if (state.type != "gl.hexsynth") return false;
  aux = ChannelAux{};
  aux.custom = buildHexAux(state.data, state.params);
  return true;
}

}  // namespace gl::packs::synths
