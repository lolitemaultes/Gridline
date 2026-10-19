// Gridline — "synths" pack: Analog runtime.
// Signal flow per voice: 3 band-limited oscillators (polyBLEP saw/pulse,
// polyBLAMP triangle, sine, noise; sync / ring / FM from the previous
// oscillator) × up to 8 unison copies + sub + noise → drive → multimode filter
// (stereo when unison is spread) → amp envelope. Two LFOs, a modulation
// envelope and a 4-slot matrix are evaluated at control rate (every 16 samples
// of the voice's own clock, so the output does not depend on block size).
// Voices are summed, then run through the FX strip (drive, chorus, delay, reverb).
#include "synths_common.h"

namespace gl::packs::synths {

namespace {

using dsp::noteToHz;
using dsp::polyBlep;

constexpr int kMaxVoices = 32;
constexpr int kMaxUni = 8;
constexpr int kChunk = 256;
constexpr uint32_t kArpIdBase = 0xA7000000u;

double naiveWave(int wave, double p, double pw) {
  switch (wave) {
    case WaveSine: return std::sin(dsp::kTwoPi * p);
    case WaveTriangle: return 1.0 - 4.0 * std::fabs(p - 0.5);
    case WaveSaw: return 2.0 * p - 1.0;
    case WavePulse: return (p < pw ? 1.0 : -1.0) - (2.0 * pw - 1.0);
    default: return 0.0;
  }
}

// One oscillator sample. `reset` hard-syncs the phase (resetFrac = samples
// since the master's wrap, 0..1); `pm` is a phase offset in cycles (FM).
double oscStep(double& ph, double dt, int wave, double pw, bool reset, double resetFrac, double pm, dsp::Noise& nz, bool& wrapped,
               double& wfrac) {
  wrapped = false;
  ph += dt;
  if (ph >= 1.0) {
    ph -= std::floor(ph);
    wrapped = true;
    wfrac = ph / dt;
  }
  double syncFrom = 0;
  bool did = false;
  if (reset && wave != WaveNoise) {
    double np = std::min(resetFrac * dt, 0.999999);
    double old = ph - np;
    if (old < 0) old += 1.0;
    syncFrom = naiveWave(wave, old, pw);
    ph = np;
    did = wrapped = true;
    wfrac = resetFrac;
  }
  double p = ph;
  if (pm != 0.0) {
    p += pm;
    p -= std::floor(p);
  }
  double out = 0;
  switch (wave) {
    case WaveSine: out = std::sin(dsp::kTwoPi * p); break;
    case WaveTriangle:
      out = 1.0 - 4.0 * std::fabs(p - 0.5);
      out += 4.0 * dt * (polyBlamp(p, dt) - polyBlamp(std::fmod(p + 0.5, 1.0), dt));
      break;
    case WaveSaw: out = 2.0 * p - 1.0 - polyBlep(p, dt); break;
    case WavePulse: {
      double q = p - pw;
      if (q < 0) q += 1.0;
      out = (p < pw ? 1.0 : -1.0) + polyBlep(p, dt) - polyBlep(q, dt) - (2.0 * pw - 1.0);
      break;
    }
    default: out = nz.next(); break;
  }
  if (did) {
    // The standard residual assumes the natural jump at phase 0; correct it
    // to the actual jump of the sync reset (after-side half of the BLEP).
    double jump = naiveWave(wave, p, pw) - syncFrom;
    double natural = wave == WaveSaw ? -2.0 : wave == WavePulse ? 2.0 : 0.0;
    out += 0.5 * (jump - natural) * polyBlep(p, dt);
  }
  return out;
}

class AnalogGen final : public Generator {
 public:
  AnalogGen() = default;

  bool active() const override {
    if (tail_ > 0) return true;
    for (const auto& v : voices_)
      if (v.on) return true;
    return false;
  }

 protected:
  struct Voice {
    bool on = false, held = false;
    uint32_t id = 0;
    double pitch = 60, target = 60;
    float vel = 1;
    uint64_t age = 0;
    uint32_t ctr = 0;
    double ph[kMaxUni][3] = {};
    double last[kMaxUni][3] = {};
    bool wrap[kMaxUni][3] = {};
    double wfrac[kMaxUni][3] = {};
    double dt[kMaxUni][3] = {};
    double subPh = 0, subDt = 0;
    dsp::Adsr amp, fenv, menv;
    MultiFilter fl, fr;
    dsp::Noise noise;
    LfoPhase lfo[2];
    double lfoInc[2] = {0, 0};
    double ageSec = 0, rnd = 0;
    // control-rate results
    double pw[3] = {0.5, 0.5, 0.5}, depth[3] = {0, 0, 0};
    double gl[kMaxUni] = {}, gr[kMaxUni] = {};
    double noiseLvl = 0, gain = 0, panL = 1, panR = 1;
    bool stereo = false;
  };

  void configure(const PluginState& s, const ChannelAux&, const ProcessContext& ctx) override {
    sr_ = ctx.sampleRate > 0 ? ctx.sampleRate : 44100.0;
    tempo_ = ctx.tempo;
    volume_ = prm(s, AnVolume, 0.7f);
    pan_ = prm(s, AnPan);
    transpose_ = prm(s, AnTranspose) + prm(s, AnFine) / 100.0;
    maxVoices_ = std::clamp(prmi(s, AnVoices, 16), 1, kMaxVoices);
    playMode_ = std::clamp(prmi(s, AnPlayMode), 0, 2);
    glide_ = prm(s, AnGlide, 0.001f);
    glideOverlap_ = prmi(s, AnGlideMode) == 1;
    velAmp_ = prm(s, AnVelAmp, 0.6f);
    velFlt_ = prm(s, AnVelFilter, 0.2f);
    uni_ = std::clamp(prmi(s, AnUniVoices, 1), 1, kMaxUni);
    uniDetune_ = prm(s, AnUniDetune, 0.25f);
    uniSpread_ = prm(s, AnUniSpread, 0.7f);
    for (int k = 0; k < 3; ++k) {
      oscOn_[k] = prmb(s, anOsc(k, OscOn));
      wave_[k] = std::clamp(prmi(s, anOsc(k, OscWave)), 0, WaveCount - 1);
      tune_[k] = prm(s, anOsc(k, OscCoarse)) + prm(s, anOsc(k, OscFine)) / 100.0;
      level_[k] = prm(s, anOsc(k, OscLevel));
      pw_[k] = prm(s, anOsc(k, OscPw), 0.5f);
      modKind_[k] = std::clamp(prmi(s, anOsc(k, OscMod)), 0, ModKindCount - 1);
      modAmt_[k] = prm(s, anOsc(k, OscModAmt), 0.5f);
    }
    subLevel_ = prm(s, AnSubLevel);
    subOct_ = prmi(s, AnSubOct) == 1 ? -24.0 : -12.0;
    subSquare_ = prmi(s, AnSubWave, 1) == 1;
    noise_ = prm(s, AnNoise);
    fltMode_ = std::clamp(prmi(s, AnFltMode, FltLp24), 0, FltModeCount - 1);
    cutoff_ = prm(s, AnCutoff, 4000);
    reso_ = prm(s, AnReso);
    drive_ = prm(s, AnDrive);
    keyTrack_ = prm(s, AnKeyTrack);
    fltEnv_ = prm(s, AnFltEnv);
    modDest_ = std::clamp(prmi(s, AnModDest), 0, AdDestCount - 1);
    modEnvAmt_ = prm(s, AnModAmt);
    for (int l = 0; l < 2; ++l) {
      lfoShape_[l] = std::clamp(prmi(s, anLfo(l, LfoShapeP)), 0, LfoShapeCount - 1);
      lfoHz_[l] = lfoHz(prmb(s, anLfo(l, LfoSync)), prmi(s, anLfo(l, LfoDiv)), prm(s, anLfo(l, LfoRate), 1), tempo_);
      lfoRetrig_[l] = prmb(s, anLfo(l, LfoRetrig));
      lfoDelay_[l] = prm(s, anLfo(l, LfoDelay), 0.001f);
      lfoDest_[l] = std::clamp(prmi(s, anLfo(l, LfoDest)), 0, AdDestCount - 1);
      lfoAmt_[l] = prm(s, anLfo(l, LfoAmt));
    }
    for (int r = 0; r < kAnMatrixRows; ++r) {
      mxSrc_[r] = std::clamp(prmi(s, anMx(r, 0)), 0, AsSourceCount - 1);
      mxDst_[r] = std::clamp(prmi(s, anMx(r, 1)), 0, AdDestCount - 1);
      mxAmt_[r] = prm(s, anMx(r, 2));
    }
    bool arp = prmb(s, AnArpOn);
    if (!arp && arpOn_) arpStop();
    arpOn_ = arp;
    arpMode_ = std::clamp(prmi(s, AnArpMode), 0, ArpModeCount - 1);
    arpOct_ = std::clamp(prmi(s, AnArpOct, 1), 1, 4);
    arpStep_ = kDivBeats[std::clamp(prmi(s, AnArpDiv, Div1_16), 0, DivCount - 1)] * 60.0 / std::max(20.0, tempo_) * sr_;
    arpGate_ = prm(s, AnArpGate, 0.6f);
    for (auto& v : voices_) {
      v.amp.set(sr_, prm(s, AnAmpA), prm(s, AnAmpD), prm(s, AnAmpS), prm(s, AnAmpR));
      v.fenv.set(sr_, prm(s, AnFenA), prm(s, AnFenD), prm(s, AnFenS), prm(s, AnFenR));
      v.menv.set(sr_, prm(s, AnModA), prm(s, AnModD), prm(s, AnModS), prm(s, AnModR));
    }
    fx_.configure(s, AnFxDrive, sr_, tempo_);
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
    stopNote(id);  // also releases notes started before the arpeggiator was switched on
  }
  void onAllOff() override {
    arpCount_ = 0;
    arpStop();
    stack_.clear();
    for (auto& v : voices_) release(v);
  }
  void onKill() override {
    for (auto& v : voices_) v = Voice{};
    stack_.clear();
    arpCount_ = 0;
    arpNoteOn_ = false;
    lastPitch_ = -1;
    tail_ = 0;
  }

  void startNote(uint32_t id, double pitch, float vel) {
    ++clock_;
    if (playMode_ != PlayPoly) {
      bool hadHeld = stack_.count > 0;
      stack_.push(id, pitch, vel);
      Voice& v = voices_[0];
      bool legato = playMode_ == PlayLegato && hadHeld && v.on;
      bool glide = glide_ > 0.002 && v.on && (!glideOverlap_ || hadHeld);
      v.id = id;
      v.target = pitch;
      if (!glide) v.pitch = pitch;
      v.vel = vel;
      v.held = true;
      v.age = clock_;
      if (!legato) trigger(v, glide);
      lastPitch_ = pitch;
      return;
    }
    bool anyHeld = false;
    Voice* pick = nullptr;
    for (int i = 0; i < maxVoices_; ++i) {
      Voice& v = voices_[static_cast<size_t>(i)];
      anyHeld |= v.on && v.held;
      if (!pick && !v.on) pick = &v;
    }
    if (!pick) {  // steal: released voices first, then the oldest
      for (int i = 0; i < maxVoices_; ++i) {
        Voice& v = voices_[static_cast<size_t>(i)];
        if (!pick || (!v.held && pick->held) || (v.held == pick->held && v.age < pick->age)) pick = &v;
      }
    }
    Voice& v = *pick;
    bool glide = glide_ > 0.002 && lastPitch_ >= 0 && (!glideOverlap_ || anyHeld);
    v.id = id;
    v.target = pitch;
    v.pitch = glide ? lastPitch_ : pitch;
    v.vel = vel;
    v.held = true;
    v.age = clock_;
    trigger(v, glide);
    lastPitch_ = pitch;
  }

  void stopNote(uint32_t id) {
    if (playMode_ != PlayPoly) {
      bool wasTop = stack_.top() && stack_.top()->id == id;
      stack_.remove(id);
      Voice& v = voices_[0];
      if (!wasTop || !v.on) return;
      if (const auto* t = stack_.top()) {  // fall back to the previous held note
        v.id = t->id;
        v.target = t->pitch;
        if (glide_ <= 0.002) v.pitch = t->pitch;
        if (playMode_ == PlayMono) trigger(v, true);
        lastPitch_ = t->pitch;
      } else {
        release(v);
      }
      return;
    }
    for (auto& v : voices_)
      if (v.on && v.held && v.id == id) release(v);
  }

  void trigger(Voice& v, bool keepPhase) {
    bool fresh = !v.on || v.amp.level <= 1e-4;
    if (fresh && !keepPhase) {
      for (int u = 0; u < kMaxUni; ++u)
        for (int k = 0; k < 3; ++k) {
          v.ph[u][k] = uni_ > 1 ? 0.5 + 0.5 * hashUnit(static_cast<uint32_t>(clock_ * 131u + static_cast<uint64_t>(u) * 7u + static_cast<uint64_t>(k))) : 0.0;
          v.last[u][k] = 0;
          v.wrap[u][k] = false;
        }
      v.subPh = 0;
      v.fl.reset();
      v.fr.reset();
    }
    v.noise.seed(static_cast<uint32_t>(clock_ * 2654435761u + 1u));
    v.rnd = hashUnit(static_cast<uint32_t>(clock_ * 40503u + 17u));
    v.on = true;
    v.ctr = 0;
    v.ageSec = 0;
    for (int l = 0; l < 2; ++l)
      if (lfoRetrig_[l]) v.lfo[l].reset();
    v.amp.gate(true);
    v.fenv.gate(true);
    v.menv.gate(true);
  }

  void release(Voice& v) {
    v.held = false;
    v.amp.gate(false);
    v.fenv.gate(false);
    v.menv.gate(false);
  }

  // ------------------------------------------------------------ arpeggio
  struct ArpNote {
    uint32_t id;
    double pitch;
    float vel;
  };
  void arpAdd(uint32_t id, double pitch, float vel) {
    for (int i = 0; i < arpCount_; ++i)
      if (arp_[static_cast<size_t>(i)].id == id) return;
    if (arpCount_ >= static_cast<int>(arp_.size())) return;
    if (arpCount_ == 0) {
      arpCounter_ = 0;  // first step now
      arpIndex_ = -1;
      arpDir_ = 1;
    }
    arp_[static_cast<size_t>(arpCount_++)] = {id, pitch, vel};
  }
  void arpRemove(uint32_t id) {
    for (int i = 0; i < arpCount_; ++i)
      if (arp_[static_cast<size_t>(i)].id == id) {
        for (int j = i + 1; j < arpCount_; ++j) arp_[static_cast<size_t>(j - 1)] = arp_[static_cast<size_t>(j)];
        --arpCount_;
        break;
      }
    if (arpCount_ == 0) arpStop();
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
    // Order: by pitch for the directional modes, as played otherwise.
    std::array<int, 32> order{};
    for (int i = 0; i < arpCount_; ++i) order[static_cast<size_t>(i)] = i;
    if (arpMode_ != ArpPlayed)
      std::sort(order.begin(), order.begin() + arpCount_,
                [&](int a, int b) { return arp_[static_cast<size_t>(a)].pitch < arp_[static_cast<size_t>(b)].pitch; });
    int len = arpCount_ * arpOct_;
    switch (arpMode_) {
      case ArpDown: arpIndex_ = arpIndex_ <= 0 ? len - 1 : arpIndex_ - 1; break;
      case ArpUpDown:
        if (len == 1) {
          arpIndex_ = 0;
        } else {
          int nx = arpIndex_ + arpDir_;
          if (nx >= len) arpDir_ = -1, nx = len - 2;
          if (nx < 0) arpDir_ = 1, nx = std::min(1, len - 1);
          arpIndex_ = nx;
        }
        break;
      case ArpRandom:
        arpIndex_ = static_cast<int>((hashUnit(static_cast<uint32_t>(++arpSeq_ * 7919u)) * 0.5 + 0.5) * len) % len;
        break;
      default: arpIndex_ = (arpIndex_ + 1) % len; break;
    }
    arpIndex_ = std::clamp(arpIndex_, 0, len - 1);
    const ArpNote& nt = arp_[static_cast<size_t>(order[static_cast<size_t>(arpIndex_ % arpCount_)])];
    arpVoiceId_ = kArpIdBase + (++arpSeq_ & 0xFFFFFu);
    startNote(arpVoiceId_, nt.pitch + 12.0 * (arpIndex_ / arpCount_), nt.vel);
    arpNoteOn_ = true;
    arpGateLeft_ = std::max(1.0, arpGate_ * arpStep_);
  }

  // -------------------------------------------------------------- render
  void render(float* L, float* R, int n) override {
    int pos = 0;
    while (pos < n) {
      int m = std::min(n - pos, kChunk);
      if (arpOn_) {
        if (arpCount_ > 0 && arpCounter_ <= 0) {
          arpFire();
          arpCounter_ += std::max(1.0, arpStep_);
        }
        if (arpNoteOn_ && arpGateLeft_ <= 0) {
          stopNote(arpVoiceId_);
          arpNoteOn_ = false;
        }
        if (arpCount_ > 0) m = std::min(m, std::max(1, static_cast<int>(std::ceil(arpCounter_))));
        if (arpNoteOn_) m = std::min(m, std::max(1, static_cast<int>(std::ceil(arpGateLeft_))));
      }
      renderChunk(L + pos, R + pos, m);
      if (arpOn_) {
        if (arpCount_ > 0) arpCounter_ -= m;
        if (arpNoteOn_) arpGateLeft_ -= m;
      }
      pos += m;
    }
  }

  void renderChunk(float* L, float* R, int n) {
    std::fill(bufL_.begin(), bufL_.begin() + n, 0.f);
    std::fill(bufR_.begin(), bufR_.begin() + n, 0.f);
    bool any = false;
    for (auto& v : voices_)
      if (v.on) {
        renderVoice(v, n);
        any = true;
      }
    fx_.process(bufL_.data(), bufR_.data(), n);
    double pl = pan_ > 0 ? 1.0 - pan_ : 1.0, pr = pan_ < 0 ? 1.0 + pan_ : 1.0;
    for (int i = 0; i < n; ++i) {
      L[i] += static_cast<float>(bufL_[static_cast<size_t>(i)] * pl);
      R[i] += static_cast<float>(bufR_[static_cast<size_t>(i)] * pr);
    }
    for (int l = 0; l < 2; ++l) globalLfo_[l].advance(lfoHz_[l] / sr_ * n);
    if (any)
      tail_ = fx_.tailActive() ? static_cast<int64_t>(sr_ * 6) : 0;
    else if (tail_ > 0)
      tail_ = fx_.tailActive() ? tail_ - n : 0;
  }

  void control(Voice& v, int i) {
    double src[AsSourceCount] = {};
    for (int l = 0; l < 2; ++l) {
      double val;
      if (lfoRetrig_[l]) {
        val = lfoValue(lfoShape_[l], v.lfo[l].phase, v.lfo[l].cycle, static_cast<uint32_t>(l * 977 + 3) ^ v.id);
      } else {
        LfoPhase g = globalLfo_[l];
        g.advance(lfoHz_[l] / sr_ * i);
        val = lfoValue(lfoShape_[l], g.phase, g.cycle, static_cast<uint32_t>(l * 977 + 3));
      }
      if (lfoDelay_[l] > 0.002) val *= std::min(1.0, v.ageSec / lfoDelay_[l]);
      src[AsLfo1 + l] = val;
    }
    src[AsModEnv] = v.menv.level;
    src[AsFilterEnv] = v.fenv.level;
    src[AsAmpEnv] = v.amp.level;
    src[AsVelocity] = v.vel;
    src[AsKey] = (v.target - 60.0) / 60.0;
    src[AsRandom] = v.rnd;
    double d[AdDestCount] = {};
    for (int l = 0; l < 2; ++l) d[lfoDest_[l]] += lfoAmt_[l] * src[AsLfo1 + l];
    d[modDest_] += modEnvAmt_ * v.menv.level;
    for (int r = 0; r < kAnMatrixRows; ++r)
      if (mxSrc_[r] != AsNone) d[mxDst_[r]] += mxAmt_[r] * src[mxSrc_[r]];
    for (int l = 0; l < 2; ++l) v.lfoInc[l] = lfoHz_[l] * std::pow(2.0, std::clamp(d[AdLfo1Rate + l], -1.0, 1.0) * 3.0) / sr_;

    const double base = v.pitch + transpose_ + d[AdPitch] * 24.0;
    const double det = std::clamp(uniDetune_ + d[AdDetune], 0.0, 1.0) * 0.6;
    const int N = uni_;
    const double norm = 1.0 / std::sqrt(static_cast<double>(N));
    v.stereo = N > 1 && uniSpread_ > 0.001;
    for (int k = 0; k < 3; ++k) {
      double pk = base + tune_[k] + d[AdOsc1Pitch + k] * 24.0;
      for (int u = 0; u < N; ++u) {
        double off = N > 1 ? (2.0 * u / (N - 1) - 1.0) * det : 0.0;
        v.dt[u][k] = std::min(0.45, noteToHz(pk + off) / sr_);
      }
      v.pw[k] = std::clamp(pw_[k] + d[AdPulseWidth] * 0.45, 0.02, 0.98);
      v.depth[k] = std::clamp(modAmt_[k] + d[AdOscMod], 0.0, 1.0);
    }
    for (int u = 0; u < N; ++u) {
      double p = N > 1 ? uniSpread_ * (2.0 * u / (N - 1) - 1.0) : 0.0;
      v.gl[u] = norm * std::sqrt(1.0 - p);
      v.gr[u] = norm * std::sqrt(1.0 + p);
    }
    v.subDt = std::min(0.45, noteToHz(base + subOct_) / sr_);
    v.noiseLvl = std::clamp(noise_ + d[AdNoise], 0.0, 1.0);
    double oct = keyTrack_ * (v.target - 60.0) / 12.0 + fltEnv_ * v.fenv.level * 7.0 + velFlt_ * (v.vel - 1.0) * 3.0 + d[AdCutoff] * 6.0;
    double cut = std::clamp(cutoff_ * std::pow(2.0, oct), 16.0, std::min(20000.0, sr_ * 0.45));
    double res = std::clamp(reso_ + d[AdResonance], 0.0, 1.0);
    v.fl.set(sr_, cut, res, fltMode_);
    if (v.stereo) v.fr.set(sr_, cut, res, fltMode_);
    v.gain = volume_ * (1.0 - velAmp_ * (1.0 - v.vel)) * std::clamp(1.0 + d[AdVolume], 0.0, 2.0) * 0.5;
    double pan = std::clamp(d[AdPan], -1.0, 1.0);
    v.panL = pan > 0 ? 1.0 - pan : 1.0;
    v.panR = pan < 0 ? 1.0 + pan : 1.0;
  }

  void renderVoice(Voice& v, int n) {
    const double glideCoef = std::pow(std::exp(-1.0 / std::max(1.0, glide_ * sr_ * 0.3)), 16.0);
    const int N = uni_;
    const double drive = 1.0 + drive_ * 8.0, driveMakeup = 1.0 / (1.0 + drive_ * 2.0);
    for (int i = 0; i < n; ++i) {
      if ((v.ctr++ & 15u) == 0) {
        v.pitch = v.target + (v.pitch - v.target) * glideCoef;
        control(v, i);
      }
      double sl = 0, sr = 0;
      for (int u = 0; u < N; ++u) {
        double s = 0;
        for (int k = 0; k < 3; ++k) {
          if (!oscOn_[k]) {
            v.last[u][k] = 0;
            v.wrap[u][k] = false;
            continue;
          }
          int src = (k + 2) % 3;
          bool reset = false;
          double frac = 0, pm = 0;
          int kind = modKind_[k];
          if (kind == ModSync && oscOn_[src] && v.wrap[u][src]) {
            reset = true;
            frac = v.wfrac[u][src];
          } else if (kind == ModFm) {
            pm = v.last[u][src] * v.depth[k] * 1.2;
          }
          double o = oscStep(v.ph[u][k], v.dt[u][k], wave_[k], v.pw[k], reset, frac, pm, v.noise, v.wrap[u][k], v.wfrac[u][k]);
          if (kind == ModRing) o *= (1.0 - v.depth[k]) + v.depth[k] * v.last[u][src];
          v.last[u][k] = o;
          s += o * level_[k];
        }
        sl += s * v.gl[u];
        sr += s * v.gr[u];
      }
      double mono = 0;
      if (subLevel_ > 0) {
        double sp = v.subPh;
        double sub = subSquare_ ? (sp < 0.5 ? 1.0 : -1.0) + polyBlep(sp, v.subDt) - polyBlep(std::fmod(sp + 0.5, 1.0), v.subDt)
                                : std::sin(dsp::kTwoPi * sp);
        mono += sub * subLevel_;
        v.subPh += v.subDt;
        if (v.subPh >= 1.0) v.subPh -= 1.0;
      }
      if (v.noiseLvl > 0) mono += v.noise.next() * v.noiseLvl * 0.7;
      sl += mono;
      sr += mono;
      if (drive_ > 0.001) {
        sl = softClip(sl * drive) * driveMakeup * 2.0;
        sr = softClip(sr * drive) * driveMakeup * 2.0;
      }
      double yl = v.fl.process(sl);
      double yr = v.stereo ? v.fr.process(sr) : yl;
      double a = v.amp.next() * v.gain;
      v.fenv.next();
      v.menv.next();
      bufL_[static_cast<size_t>(i)] += static_cast<float>(yl * a * v.panL);
      bufR_[static_cast<size_t>(i)] += static_cast<float>(yr * a * v.panR);
      v.ageSec += 1.0 / sr_;
      for (int l = 0; l < 2; ++l)
        if (lfoRetrig_[l]) v.lfo[l].advance(v.lfoInc[l]);
      if (!v.amp.active()) {
        v.on = false;
        v.held = false;
        break;
      }
    }
  }

 private:
  std::array<Voice, kMaxVoices> voices_{};
  std::array<float, kChunk> bufL_{}, bufR_{};
  FxRack fx_;
  NoteStack stack_;
  LfoPhase globalLfo_[2];
  double sr_ = 44100, tempo_ = 140;
  double volume_ = 0.7, pan_ = 0, transpose_ = 0, glide_ = 0.001, velAmp_ = 0.6, velFlt_ = 0.2;
  int maxVoices_ = 16, playMode_ = PlayPoly, uni_ = 1;
  bool glideOverlap_ = false;
  double uniDetune_ = 0.25, uniSpread_ = 0.7;
  bool oscOn_[3] = {true, true, false};
  int wave_[3] = {WaveSaw, WaveSaw, WavePulse}, modKind_[3] = {0, 0, 0};
  double tune_[3] = {0, 0, 0}, level_[3] = {0.8, 0.6, 0.6}, pw_[3] = {0.5, 0.5, 0.5}, modAmt_[3] = {0.5, 0.5, 0.5};
  double subLevel_ = 0, subOct_ = -12, noise_ = 0;
  bool subSquare_ = true;
  int fltMode_ = FltLp24;
  double cutoff_ = 4000, reso_ = 0.15, drive_ = 0, keyTrack_ = 0.3, fltEnv_ = 0.3;
  int modDest_ = AdNone;
  double modEnvAmt_ = 0;
  int lfoShape_[2] = {0, 0}, lfoDest_[2] = {0, 0};
  double lfoHz_[2] = {5, 0.5}, lfoDelay_[2] = {0, 0}, lfoAmt_[2] = {0, 0};
  bool lfoRetrig_[2] = {true, false};
  int mxSrc_[kAnMatrixRows] = {}, mxDst_[kAnMatrixRows] = {};
  double mxAmt_[kAnMatrixRows] = {};
  // arpeggiator
  bool arpOn_ = false, arpNoteOn_ = false;
  int arpMode_ = ArpUp, arpOct_ = 1, arpCount_ = 0, arpIndex_ = -1, arpDir_ = 1;
  double arpStep_ = 5000, arpGate_ = 0.6, arpCounter_ = 0, arpGateLeft_ = 0;
  uint32_t arpVoiceId_ = 0, arpSeq_ = 0;
  std::array<ArpNote, 32> arp_{};
  uint64_t clock_ = 0;
  double lastPitch_ = -1;
  int64_t tail_ = 0;
};

}  // namespace

std::unique_ptr<Generator> makeAnalog() { return std::make_unique<AnalogGen>(); }

}  // namespace gl::packs::synths
