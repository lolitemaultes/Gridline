// Gridline — Additive: a bank of up to 256 sine partials per voice. The
// timbre (two spectra or a colour / custom table, drawn levels, timbre EQ,
// tilt, odd/even, mask) is shaped over frequency by a harmonic-domain filter
// (shapes + resonance shapes + self-oscillation), a phaser comb, a pluck
// (higher partials fade faster) and a harmonizer that copies partials up the
// series, all evaluated at control rate (every 32 samples) and ramped per
// sample. Prism / scatter / detune / grit move the partial frequencies; blur
// smears level changes; unison copies the bank (up to 9 copies).
#include <array>
#include <cmath>
#include "models_rt.h"

namespace gl::packs::models {

namespace {

using namespace dsp;
constexpr int kCtl = 32;
constexpr int kVoices = 16;
constexpr int kMaxUni = 9;

// Custom timbre table prepared on the UI thread.
struct AdditiveAux final : AuxData {
  AdditiveData d;
};

float hashf(uint32_t a, uint32_t b) {
  uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u;
  h ^= h >> 15;
  h *= 0x2C1B3C6Du;
  h ^= h >> 12;
  h *= 0x297A2D39u;
  h ^= h >> 15;
  return static_cast<float>(h & 0xFFFFFF) / static_cast<float>(0xFFFFFF);
}

// Envelope segment with a slope (-1..1): s = ±(10^(6|x|) - 1); positive bends
// the rise early (fast start), negative late.
double sloped(double t, double slope) {
  t = std::clamp(t, 0.0, 1.0);
  if (std::fabs(slope) < 1e-4) return t;
  const double s = std::pow(10.0, 6.0 * std::fabs(slope)) - 1.0;
  if (slope > 0) return 1.0 - (std::pow(1.0 + s, 1.0 - t) - 1.0) / s;
  return (std::pow(1.0 + s, t) - 1.0) / s;
}

// Attack / decay(→ sustain) / release envelope with sloped segments (control rate).
struct SlopeEnv {
  double att = 0.005, dec = 0.5, sus = 0.3, rel = 0.5, aSlope = 0, dSlope = 0;
  int stage = 0;  // 0 idle, 1 attack, 2 decay, 3 sustain, 4 release
  double t = 0, level = 0, from = 0;
  void gate(bool on) {
    from = level;
    t = 0;
    stage = on ? 1 : (stage == 0 ? 0 : 4);
  }
  double next(double dt) {
    t += dt;
    switch (stage) {
      case 1:
        level = from + (1.0 - from) * sloped(t / std::max(1e-4, att), aSlope);
        if (t >= att) stage = 2, t = 0, level = 1;
        break;
      case 2:
        level = 1.0 + (sus - 1.0) * sloped(t / std::max(1e-4, dec), -dSlope);
        if (t >= dec) stage = 3, level = sus;
        break;
      case 3: level = sus; break;
      case 4:
        level = from * (1.0 - sloped(t / std::max(1e-4, rel), -dSlope));
        if (t >= rel) stage = 0, level = 0;
        break;
      default: level = 0; break;
    }
    return level;
  }
};

class AdditiveGen final : public Generator {
 public:
  AdditiveGen() : voices_(new Voice[kVoices]) {
    (void)sineTable();
    last_.reserve(AdCount + 64);  // parameter snapshot: no allocation on the audio thread
  }
  bool active() const override {
    for (int i = 0; i < kVoices; ++i)
      if (voices_[i].on) return true;
    return fx_.ringing() || compTail_ > 0;
  }

 protected:
  struct Osc {
    uint32_t ph = 0, inc = 0;
    float amp = 0, damp = 0, smooth = 0, grit = 0;
  };
  struct Voice {
    bool on = false, held = false;
    uint32_t id = 0, serial = 0;
    double note = 60, cur = 60, glideFrom = 60;
    double glideT = 1, glideLen = 0;  // portamento progress
    float vel = 1, relVel = 0.5f;
    uint64_t age = 0;
    double t = 0, relT = -1;  // seconds since note-on / since release (-1 while held)
    Adsr amp;
    SlopeEnv fenv;
    float fenvVal = 0, lfo = 0, lfoFade = 0, rand = 0;
    double lfoPhase = 0;
    Osc osc[kAdMaxOsc];
    Osc sub[3];
    Osc self;  // resonance self-oscillation
  };

  void configure(const PluginState& s, const ChannelAux& aux, const ProcessContext& ctx) override {
    bool srChanged = ctx.sampleRate != sr_;
    sr_ = ctx.sampleRate;
    tempo_ = ctx.tempo > 0 ? ctx.tempo : 120.0;
    const AdditiveData* custom = nullptr;
    if (auto* a = dynamic_cast<const AdditiveAux*>(aux.custom.get())) custom = &a->d;
    if (srChanged || s.params != last_ || custom != lastCustom_) {
      last_ = s.params;
      lastCustom_ = custom;
      m_.read(s);
      m_.custom = custom;
      uni_ = std::clamp(prmi(s, AdUniOrder, 1), 1, kMaxUni);
      perCopy_ = std::min(m_.partials, kAdMaxOsc / uni_);
      for (int k = 1; k <= kAdMaxOsc; ++k) {
        tA_[static_cast<size_t>(k - 1)] = m_.timbre(k, 0.0f);
        tB_[static_cast<size_t>(k - 1)] = m_.timbre(k, 1.0f);
        ratio_[static_cast<size_t>(k - 1)] = m_.ratio(k);
        phA_[static_cast<size_t>(k - 1)] = m_.phase(k, 0.0f);
      }
      uniDet_ = prmf(s, AdUniDetune, 12);
      uniSpread_ = prmf(s, AdUniSpread, 0.6f);
      uniType_ = prmi(s, AdUniType, 0);
      uniVar_ = prmf(s, AdUniVar, 0.5f);
      uniPhase_ = prmf(s, AdUniPhase, 0.5f);
      double e = 0;
      for (int k = 1; k <= std::min(m_.partials, 64); ++k) e += tA_[static_cast<size_t>(k - 1)] * tA_[static_cast<size_t>(k - 1)];
      norm_ = static_cast<float>(0.32 / std::sqrt(static_cast<double>(uni_)) / std::max(0.6, std::sqrt(e)));
    }
    morphEnv_ = prmf(s, AdMorphEnv, 0);
    fenvAmt_ = prmf(s, AdFEnvAmt, 0);
    phSpeed_ = prmf(s, AdPhSpeed, 0);
    phKey_ = prmf(s, AdPhKey, 0);
    blur_ = prmf(s, AdBlur, 0);
    phaseRand_ = prmf(s, AdPhaseRand, 0);
    lfoShape_ = prmi(s, AdLfoShape, 0);
    lfoRate_ = prmf(s, AdLfoRate, 5);
    lfoTempo_ = prmi(s, AdLfoTempo, 0) != 0;
    lfoGlobal_ = prmi(s, AdLfoGlobal, 1) != 0;
    lfoFade_ = prmf(s, AdLfoAttack, 0.001f);
    lfoPitch_ = prmf(s, AdLfoPitch, 0);
    lfoCut_ = prmf(s, AdLfoCutoff, 0);
    lfoPh_ = prmf(s, AdLfoPhaser, 0);
    lfoMorph_ = prmf(s, AdLfoMorph, 0);
    lfoMask_ = prmf(s, AdMaskLfo, 0);
    lfoPhW_ = prmf(s, AdPhWidthLfo, 0);
    lfoHz_ = prmf(s, AdHzWidthLfo, 0);
    maskMix_ = prmf(s, AdMaskMix, 0);
    tremolo_ = prmf(s, AdTremolo, 0);
    vibrato_ = prmf(s, AdVibrato, 0);
    grit_ = prmf(s, AdGrit, 0);
    glide_ = prmf(s, AdGlide, 0);
    pitch_ = prmf(s, AdPitch, 0) + prmf(s, AdFine, 0) / 100.0f;
    maxVoices_ = std::clamp(prmi(s, AdVoices, 8), 1, kVoices);
    vol_ = prmf(s, AdVolume, 0.8f);
    sub_[0] = prmf(s, AdSub, 0), sub_[1] = prmf(s, AdSub3, 0), sub_[2] = prmf(s, AdSub4, 0);
    protect_ = prmf(s, AdProtect, 0);
    resOsc_ = prmf(s, AdResOsc, 0);
    resTrack_ = prmi(s, AdResTrack, 0) != 0;
    resAdaptive_ = prmi(s, AdResAdaptive, 0) != 0;
    fenvAdaptive_ = prmi(s, AdFEnvAdaptive, 0) != 0;
    pluckRel_ = prmi(s, AdPluckRel, 0) != 0;
    velAtt_ = prmi(s, AdVelAtt, 0) != 0, velRel_ = prmi(s, AdVelRel, 0) != 0, velVol_ = prmi(s, AdVelVol, 1) != 0;
    velFEnv_ = prmi(s, AdVelFEnv, 0) != 0, velPluck_ = prmi(s, AdVelPluck, 0) != 0, velHarm_ = prmi(s, AdVelHarm, 0) != 0;
    velPorta_ = prmi(s, AdVelPorta, 0) != 0, velUni_ = prmi(s, AdVelUni, 0) != 0;
    // Voice mode.
    const int mode = prmi(s, AdPlayMode, ApAuto);
    switch (mode) {
      case ApPoly: mono_ = false, porta_ = false; break;
      case ApPolyPorta: mono_ = false, porta_ = true; break;
      case ApMono: mono_ = true, porta_ = false; break;
      case ApMonoPorta: mono_ = true, porta_ = true; break;
      default: mono_ = glide_ > 0, porta_ = glide_ > 0; break;  // older states: glide = mono legato
    }
    att_ = prmf(s, AdAttack, 0.005f), dec_ = prmf(s, AdDecay, 0.6f), sus_ = prmf(s, AdSustain, 0.7f), rel_ = prmf(s, AdRelease, 0.4f);
    fAtt_ = prmf(s, AdFEnvAtt, 0.01f), fDec_ = prmf(s, AdFEnvDec, 0.5f), fSus_ = prmf(s, AdFEnvSus, 0.3f), fRel_ = prmf(s, AdFEnvRel, 0.5f);
    fASlope_ = prmf(s, AdFEnvAttSlope, 0), fDSlope_ = prmf(s, AdFEnvDecSlope, 0);
    for (int i = 0; i < kVoices; ++i) {
      Voice& v = voices_[i];
      if (!v.on) continue;
      if (v.amp.stage != Adsr::Release) setAmp(v);
      setFenv(v);
    }
    fx_.set(s, AdFx, sr_);
    fx_.setKinds(prmi(s, AdChType, AcClassic), prmi(s, AdDelType, AdlPingPong), prmi(s, AdRevType, AvClassic), tempo_);
    compType_ = prmi(s, AdCompType, AkOff);
    comp_ = prmf(s, AdComp, 0.5f);
  }

  void setAmp(Voice& v) {
    const double a = velAtt_ ? att_ * (1.6 - 1.2 * v.vel) : att_;
    const double r = velRel_ ? rel_ * (0.4 + 1.2 * v.relVel) : rel_;
    v.amp.set(sr_, a, dec_, sus_, r);
  }
  void setFenv(Voice& v) {
    double k = 1.0;
    if (fenvAdaptive_) k = std::pow(2.0, -(v.cur - 60.0) / 24.0);  // higher notes: shorter sweep
    v.fenv.att = fAtt_ * k, v.fenv.dec = fDec_ * k, v.fenv.sus = fSus_, v.fenv.rel = fRel_ * k;
    v.fenv.aSlope = fASlope_, v.fenv.dSlope = fDSlope_;
  }

  void onNoteOnEx(uint32_t id, double pitch, float vel, const NoteProps& props) override {
    relVelNext_ = props.release;
    onNoteOn(id, pitch, vel);
  }

  void onNoteOn(uint32_t id, double pitch, float vel) override {
    const float relVel = relVelNext_;
    relVelNext_ = 0.5f;
    const double prevNote = lastNote_;
    lastNote_ = pitch;
    if (mono_) {  // legato: retune the held voice
      for (int i = 0; i < kVoices; ++i) {
        Voice& v = voices_[i];
        if (v.on && v.held) {
          startGlide(v, porta_ ? v.cur : pitch, pitch, vel);
          v.note = pitch;
          v.id = id;
          return;
        }
      }
    }
    Voice* pick = nullptr;
    const int limit = mono_ ? 1 : maxVoices_;
    for (int i = 0; i < limit; ++i)
      if (!voices_[i].on) {
        pick = &voices_[i];
        break;
      }
    if (!pick)
      for (int i = 0; i < limit; ++i)
        if (!pick || voices_[i].age < pick->age) pick = &voices_[i];
    Voice& v = *pick;
    const bool wasOn = v.on;
    v.on = v.held = true;
    v.id = id;
    v.serial = ++serial_;
    v.note = pitch;
    v.vel = vel;
    v.relVel = relVel;
    v.age = ++clock_;
    v.t = 0;
    v.relT = -1;
    v.rand = hashf(v.serial, 77u) * 2.0f - 1.0f;
    if (porta_ && prevNote >= 0) {
      v.cur = wasOn ? v.cur : prevNote;
      startGlide(v, v.cur, pitch, vel);
    } else {
      v.cur = pitch;
      v.glideT = 1;
    }
    setAmp(v);
    setFenv(v);
    v.amp.gate(true);
    v.fenv.level = 0;
    v.fenv.gate(true);
    if (!lfoGlobal_) v.lfoPhase = 0;
    v.lfoFade = 0;
    if (!wasOn) {
      for (int c = 0; c < uni_; ++c)
        for (int k = 0; k < perCopy_; ++k) {
          Osc& o = v.osc[c * perCopy_ + k];
          o = Osc{};
          uint32_t h = static_cast<uint32_t>((k + 1) * 2654435761u) ^ static_cast<uint32_t>(c * 40503u + 17u);
          h ^= h >> 13;
          h *= 0x5bd1e995u;
          const double start = phA_[static_cast<size_t>(k)] + uniPhase_ * hashf(static_cast<uint32_t>(c) * 131u + 5u, static_cast<uint32_t>(k)) * (c > 0 ? 1.0 : 0.0);
          o.ph = static_cast<uint32_t>(static_cast<double>(h) * phaseRand_) + static_cast<uint32_t>((start - std::floor(start)) * 4294967296.0);
          o.grit = hashf(v.serial * 7u + static_cast<uint32_t>(c), static_cast<uint32_t>(k)) * 2.0f - 1.0f;
        }
      for (auto& s : v.sub) s = Osc{};
      v.self = Osc{};
    }
    voiceControl(v, true);
  }
  void startGlide(Voice& v, double from, double to, float vel) {
    double len = std::max(0.0, static_cast<double>(glide_));
    if (velPorta_) len *= 1.5 - vel;
    v.glideFrom = from;
    v.glideLen = len;
    v.glideT = len > 1e-4 ? 0.0 : 1.0;
    if (v.glideT >= 1) v.cur = to;
  }
  void release(Voice& v) {
    v.held = false;
    v.relT = 0;
    v.amp.gate(false);
    v.fenv.gate(false);
  }
  void onNoteOff(uint32_t id) override {
    for (int i = 0; i < kVoices; ++i) {
      Voice& v = voices_[i];
      if (v.on && v.held && v.id == id) release(v);
    }
  }
  void onAllOff() override {
    for (int i = 0; i < kVoices; ++i)
      if (voices_[i].on && voices_[i].held) release(voices_[i]);
  }
  void onKill() override {
    for (int i = 0; i < kVoices; ++i) voices_[i].on = false;
    fx_.reset();
    compEnv_ = 0;
    compTail_ = 0;
  }

  // LFO value (-1..1) for a voice (shape or special source).
  float lfoValue(const Voice& v, double ph) const {
    switch (lfoShape_) {
      case AlSmoothSaw: return static_cast<float>(std::tanh(3.0 * (2 * ph - 1)) / std::tanh(3.0));
      case AlSmoothPulse: return static_cast<float>(std::tanh(4.0 * std::sin(kTwoPi * ph)) / std::tanh(4.0));
      case AlStairs: return static_cast<float>(std::floor(ph * 5.0) / 2.0 - 1.0);
      case AlSickle: return static_cast<float>(1.0 - 2.0 * std::sqrt(ph));
      case AlMultiSine: return static_cast<float>(0.6 * std::sin(kTwoPi * ph) + 0.4 * std::sin(kTwoPi * 3 * ph));
      case AlFiltEnv: return v.fenvVal * 2.0f - 1.0f;
      case AlFiltFreq: return static_cast<float>(std::clamp((v.cur - 60.0) / 36.0, -1.0, 1.0));
      case AlVoiceRand: return v.rand;
      case AlVelocity: return v.vel * 2.0f - 1.0f;
      case AlPitch: return static_cast<float>(std::clamp((v.note - 60.0) / 24.0, -1.0, 1.0));
      default: return lfoShape(std::min(lfoShape_, 4), ph, held_);
    }
  }

  // Per-voice partial targets for the next control period.
  void voiceControl(Voice& v, bool fresh) {
    const double dt = kCtl / sr_;
    if (!fresh) {
      v.t += dt;
      if (v.relT >= 0) v.relT += dt;
      if (v.glideT < 1) {
        v.glideT = std::min(1.0, v.glideT + dt / std::max(1e-4, v.glideLen));
        v.cur = v.glideFrom + (v.note - v.glideFrom) * v.glideT;
      } else {
        v.cur = v.note;
      }
      v.fenvVal = static_cast<float>(v.fenv.next(dt));
    } else {
      v.fenvVal = 0;
    }
    // LFO (global phase or per voice) with fade-in.
    if (!lfoGlobal_) {
      v.lfoPhase += lfoHzNow() * dt;
      v.lfoPhase -= std::floor(v.lfoPhase);
    }
    v.lfoFade = lfoFade_ <= 0.002f ? 1.0f : static_cast<float>(std::min(1.0, v.t / lfoFade_));
    v.lfo = lfoValue(v, lfoGlobal_ ? lfoPhase_ : v.lfoPhase) * v.lfoFade;
    const float lfo = v.lfo;
    const double vib = vibrato_ > 0 ? vibrato_ * std::sin(kTwoPi * 5.5 * v.t) * std::min(1.0, v.t / 0.3) : 0.0;
    const double f0 = noteToHz(v.cur + pitch_ + lfo * lfoPitch_ + vib);
    const float envAmt = fenvAmt_ * (velFEnv_ ? v.vel : 1.0f);
    const double keyCut = m_.cutoff * std::pow(2.0, m_.keyTrack * (v.cur - 60.0) / 12.0);
    const double cut = keyCut * std::pow(2.0, envAmt * v.fenvVal + lfo * lfoCut_ * 2.0);
    const float morph = std::clamp(m_.morph + morphEnv_ * v.fenvVal + lfoMorph_ * lfo * 0.5f, 0.0f, 1.0f);
    const double phOff = m_.phOffset + phSpeed_ * v.t + lfo * lfoPh_ * 0.5 + phKey_ * (v.cur - 60.0) / 12.0;
    const double phDen = std::pow(2.0, lfoPhW_ * lfo);
    const double hzW = m_.hzWidth * (velHarm_ ? v.vel : 1.0f) + lfoHz_ * lfo * 0.5;
    const float mask = std::clamp(maskMix_ + lfoMask_ * lfo * 0.5f, 0.0f, 1.0f);
    const double nyq = sr_ * 0.47;
    const float gNorm = norm_ * (velVol_ ? (0.25f + 0.75f * v.vel) : 1.0f);
    const double pluckT = pluckRel_ ? (v.relT >= 0 ? v.relT : 0.0) : v.t;
    const double pluckMul = velPluck_ ? (0.4 + 1.2 * v.vel) : 1.0;
    // Static resonance centre unless it tracks the cutoff motion.
    const double resCut = resTrack_ ? cut : keyCut;
    const double widthMul = 1.0;
    m_.resWidth = resAdaptive_ ? std::clamp(f0 / 92.5, 0.05, 20.0) : 1.0;
    m_.f0ref = f0;
    // Levels for the first copy's partials (shared by every unison copy).
    const int n = perCopy_;
    for (int k = 1; k <= n; ++k) {
      float a = tA_[static_cast<size_t>(k - 1)], b = tB_[static_cast<size_t>(k - 1)];
      float tim = a + (b - a) * morph;
      if (mask != m_.maskMix && m_.maskMix < 1.0f) {
        // Re-apply the mask at the modulated amount (timbre tables hold the static mix).
        const float mk = m_.mask[(k - 1) % kAdMask];
        const float st = 1.0f + (mk - 1.0f) * m_.maskMix, dy = 1.0f + (mk - 1.0f) * mask;
        tim = st > 1e-6f ? tim / st * dy : 0.0f;
      }
      lvl_[k - 1] = tim;
    }
    auto harmonize = [&]() { m_.harmonize(lvl_.data(), n, hzW, tmp_.data()); };
    if (m_.hzPos == AhPreFilter) harmonize();
    for (int k = 1; k <= n; ++k) {
      const double hz = f0 * ratio_[static_cast<size_t>(k - 1)];
      float g = lvl_[k - 1];
      if (g != 0.0f) {
        float fg = m_.filter(hz, cut, -1, widthMul);
        if (!resTrack_ && (m_.reso > 0 || m_.resOsc > 0)) {
          // Resonance at the static centre: filter without it, add it at resCut.
          const float noRes = filterNoRes(hz, cut, widthMul);
          const float withRes = m_.filter(hz, resCut, -1, widthMul) - filterNoRes(hz, resCut, widthMul);
          fg = noRes + withRes;
        }
        g *= std::max(0.0f, fg);
      }
      lvl_[k - 1] = g;
    }
    if (m_.hzPos == AhPrePhaser) harmonize();
    for (int k = 1; k <= n; ++k)
      if (lvl_[k - 1] != 0.0f) lvl_[k - 1] *= m_.phaser(f0 * ratio_[static_cast<size_t>(k - 1)], phOff, phDen);
    if (m_.hzPos == AhPrePluck) harmonize();
    for (int k = 1; k <= n; ++k) {
      if (lvl_[k - 1] == 0.0f) continue;
      const double hz = f0 * ratio_[static_cast<size_t>(k - 1)];
      float p = m_.pluckGain(hz, f0, pluckT, pluckMul);
      if (m_.pluckMasked) p = 1.0f + (p - 1.0f) * std::clamp(m_.mask[(k - 1) % kAdMask], 0.0f, 1.0f);
      lvl_[k - 1] *= p;
    }
    if (m_.hzPos == AhFinal) harmonize();
    // Low-partial protection: partials below a floor frequency fade out (sub-sonic guard).
    const double floorHz = protect_ > 0 ? 16.0 * std::pow(2.0, protect_ * 4.0) : 0.0;
    // Unison detune / pan per copy.
    double det = uniDet_ * (velUni_ ? v.vel : 1.0f);
    for (int c = 0; c < uni_; ++c) {
      double x = uni_ > 1 ? (2.0 * c / (uni_ - 1) - 1.0) : 0.0;
      if (uniType_ == 1 && uni_ > 1) x = (c % 2 ? -1.0 : 1.0) * (0.5 + 0.5 * static_cast<double>(c / 2) / std::max(1, (uni_ - 1) / 2));
      if (uniType_ == 2 && c > 0) x = hashf(v.serial * 13u + static_cast<uint32_t>(c), 3u) * 2.0 - 1.0;
      if (c > 0) x += uniVar_ * 0.3 * (hashf(static_cast<uint32_t>(c) * 977u, 11u) * 2.0 - 1.0);
      detMul_[c] = std::pow(2.0, x * det / 1200.0);
      double pan = std::clamp((uni_ > 1 ? (2.0 * c / (uni_ - 1) - 1.0) : 0.0) * uniSpread_, -1.0, 1.0), l, r;
      panGains(pan, l, r);
      panL_[c] = static_cast<float>(l);
      panR_[c] = static_cast<float>(r);
    }
    // Grit: each partial moves by up to 0.8 × f0 (curve 65^|x|), fixed random pattern per voice.
    const double gritHz = (std::pow(65.0, std::fabs(grit_)) - 1.0) * 0.0125 * f0;
    for (int c = 0; c < uni_; ++c) {
      for (int k = 1; k <= n; ++k) {
        Osc& o = v.osc[c * n + k - 1];
        double hz = f0 * ratio_[static_cast<size_t>(k - 1)] * detMul_[c];
        if (k > 1 && grit_ != 0) hz = std::max(1.0, hz + gritHz * o.grit);
        float target = 0;
        if (hz < nyq) {
          target = lvl_[k - 1] * gNorm;
          if (floorHz > 0 && hz < floorHz) target *= static_cast<float>((hz / floorHz) * (hz / floorHz));
          o.inc = phaseInc(hz, sr_);
        }
        if (blur_ > 0 && !fresh) {
          double tau = blur_ * 0.6 * (0.25 + 0.75 * static_cast<double>(k) / n);
          o.smooth += (target - o.smooth) * static_cast<float>(1.0 - std::exp(-dt / tau));
        } else {
          o.smooth = target;
        }
        o.damp = (o.smooth - o.amp) / kCtl;
      }
    }
    for (int i = 0; i < 3; ++i) {
      // Half-step partials at 0.5, 1.5 and 2.5 × f0, levelled from the fundamental.
      static const double ratio[3] = {0.5, 1.5, 2.5};
      Osc& s = v.sub[i];
      s.inc = phaseInc(f0 * ratio[i], sr_);
      const float t = sub_[i] * tA_[0] * (i == 0 ? 1.0f : 1.0f / (1.0f + static_cast<float>(i))) * gNorm;
      s.damp = (t - s.amp) / kCtl;
    }
    // Self-oscillation: a sine at the resonance centre.
    {
      const double rc = (resTrack_ ? cut : keyCut) * std::pow(2.0, m_.resOffset / 12.0);
      v.self.inc = phaseInc(std::clamp(rc, 20.0, nyq), sr_);
      const float t = resOsc_ > 0 && m_.fltType != AfOff ? resOsc_ * 2.0f * gNorm : 0.0f;
      v.self.damp = (t - v.self.amp) / kCtl;
    }
  }
  float filterNoRes(double hz, double cut, double widthMul) const { return m_.filter(hz, cut, -1, widthMul, false); }

  double lfoHzNow() const { return lfoTempo_ ? lfoRate_ * tempo_ / 60.0 / 4.0 : lfoRate_; }

  void controlTick() {
    lfoPhase_ += lfoHzNow() * kCtl / sr_;
    if (lfoPhase_ >= 1) {
      lfoPhase_ -= std::floor(lfoPhase_);
      held_ = noise_.next();
    }
    for (int i = 0; i < kVoices; ++i)
      if (voices_[i].on) voiceControl(voices_[i], false);
  }

  void renderVoices(float* bl, float* br, int n) {
    const SineTable& st = sineTable();
    float mono[kMaxUni][kCtl];
    for (int i = 0; i < kVoices; ++i) {
      Voice& v = voices_[i];
      if (!v.on) continue;
      for (int c = 0; c < uni_; ++c) {
        float* m = mono[c];
        for (int j = 0; j < n; ++j) m[j] = 0;
        for (int k = 0; k < perCopy_; ++k) {
          Osc& o = v.osc[c * perCopy_ + k];
          if (o.amp == 0 && o.damp == 0) {
            o.ph += o.inc * static_cast<uint32_t>(n);
            continue;
          }
          uint32_t ph = o.ph;
          float a = o.amp;
          for (int j = 0; j < n; ++j) {
            m[j] += st.at(ph) * a;
            ph += o.inc;
            a += o.damp;
          }
          o.ph = ph;
          o.amp = std::fabs(a) < 1e-9f ? 0.0f : a;
        }
        if (c == 0) {
          for (Osc* s : {&v.sub[0], &v.sub[1], &v.sub[2], &v.self}) {
            if (s->amp == 0 && s->damp == 0) continue;
            for (int j = 0; j < n; ++j) {
              m[j] += st.at(s->ph) * s->amp;
              s->ph += s->inc;
              s->amp += s->damp;
            }
            if (std::fabs(s->amp) < 1e-9f) s->amp = 0;
          }
        }
      }
      // Stereo tremolo from the voice's LFO.
      const float tr = tremolo_ * (0.5f + 0.5f * v.lfo);
      const float tl = 1.0f - tr, trr = 1.0f - tremolo_ + tr;
      for (int j = 0; j < n; ++j) {
        float e = static_cast<float>(v.amp.next());
        float l = 0, r = 0;
        for (int c = 0; c < uni_; ++c) {
          l += mono[c][j] * panL_[c];
          r += mono[c][j] * panR_[c];
        }
        bl[j] += l * e * vol_ * (tremolo_ > 0 ? tl : 1.0f);
        br[j] += r * e * vol_ * (tremolo_ > 0 ? trr : 1.0f);
      }
      if (!v.amp.active()) v.on = false;
    }
  }

  // Output compression / saturation stage.
  void compress(float* L, float* R, int n) {
    if (compType_ == AkOff) return;
    const float drive = 0.5f + comp_ * 3.0f;
    const float att = static_cast<float>(1.0 - std::exp(-1.0 / (0.002 * sr_))), rel = static_cast<float>(1.0 - std::exp(-1.0 / (0.15 * sr_)));
    bool loud = false;
    for (int i = 0; i < n; ++i) {
      float l = L[i], r = R[i];
      switch (compType_) {
        case AkLimit: {
          const float pk = std::max(std::fabs(l), std::fabs(r)) * drive;
          compEnv_ += (pk - compEnv_) * (pk > compEnv_ ? att : rel);
          const float g = compEnv_ > 0.9f ? 0.9f / compEnv_ : 1.0f;
          l *= drive * g, r *= drive * g;
          break;
        }
        case AkWarm: l = std::tanh(l * drive) / std::tanh(drive), r = std::tanh(r * drive) / std::tanh(drive); break;
        case AkHeat: l = std::tanh(l * drive * 1.8f) * 0.8f, r = std::tanh(r * drive * 1.8f) * 0.8f; break;
        default: {  // drive: asymmetric clip
          const float d = drive * 2.5f;
          l = std::tanh(l * d + 0.15f) - std::tanh(0.15f), r = std::tanh(r * d + 0.15f) - std::tanh(0.15f);
          break;
        }
      }
      if (std::fabs(l) + std::fabs(r) > 1e-6f) loud = true;
      L[i] = l, R[i] = r;
    }
    compTail_ = loud ? static_cast<int>(sr_ * 0.2) : std::max(0, compTail_ - n);
  }

  void render(float* L, float* R, int n) override {
    int pos = 0;
    while (pos < n) {
      if (ctr_ == 0) {
        controlTick();
        ctr_ = kCtl;
      }
      int len = std::min(ctr_, n - pos);
      float bl[kCtl] = {}, br[kCtl] = {};
      renderVoices(bl, br, len);
      fx_.process(bl, br, len);
      compress(bl, br, len);
      for (int j = 0; j < len; ++j) {
        L[pos + j] += bl[j];
        R[pos + j] += br[j];
      }
      ctr_ -= len;
      pos += len;
    }
  }

 private:
  std::unique_ptr<Voice[]> voices_;
  AdditiveModel m_;
  std::vector<float> last_;
  const AdditiveData* lastCustom_ = nullptr;
  std::array<float, kAdMaxOsc> tA_{}, tB_{}, phA_{}, lvl_{}, tmp_{};
  std::array<double, kAdMaxOsc> ratio_{};
  double detMul_[kMaxUni] = {1, 1, 1, 1, 1, 1, 1, 1, 1};
  float panL_[kMaxUni] = {1, 1, 1, 1, 1, 1, 1, 1, 1}, panR_[kMaxUni] = {1, 1, 1, 1, 1, 1, 1, 1, 1};
  int uni_ = 1, perCopy_ = 128, maxVoices_ = 8, lfoShape_ = 0, ctr_ = 0, uniType_ = 0, compType_ = 0, compTail_ = 0;
  double sr_ = 0, lfoPhase_ = 0, tempo_ = 120, lastNote_ = -1;
  float norm_ = 0.3f, morphEnv_ = 0, fenvAmt_ = 0, phSpeed_ = 0, phKey_ = 0, blur_ = 0, phaseRand_ = 0;
  float lfoRate_ = 5, lfoFade_ = 0, lfoPitch_ = 0, lfoCut_ = 0, lfoPh_ = 0, lfoMorph_ = 0, lfoMask_ = 0, lfoPhW_ = 0, lfoHz_ = 0;
  float held_ = 0, maskMix_ = 0, tremolo_ = 0, vibrato_ = 0, grit_ = 0;
  float glide_ = 0, vol_ = 0.8f, pitch_ = 0, sub_[3] = {0, 0, 0}, protect_ = 0, resOsc_ = 0;
  float uniDet_ = 12, uniSpread_ = 0.6f, uniVar_ = 0.5f, uniPhase_ = 0.5f, comp_ = 0.5f, compEnv_ = 0, relVelNext_ = 0.5f;
  float att_ = 0.005f, dec_ = 0.6f, sus_ = 0.7f, rel_ = 0.4f, fAtt_ = 0.01f, fDec_ = 0.5f, fSus_ = 0.3f, fRel_ = 0.5f, fASlope_ = 0, fDSlope_ = 0;
  bool lfoTempo_ = false, lfoGlobal_ = true, mono_ = false, porta_ = false, resTrack_ = false, resAdaptive_ = false;
  bool fenvAdaptive_ = false, pluckRel_ = false, velAtt_ = false, velRel_ = false, velVol_ = true, velFEnv_ = false;
  bool velPluck_ = false, velHarm_ = false, velPorta_ = false, velUni_ = false;
  Noise noise_;
  uint64_t clock_ = 0;
  uint32_t serial_ = 0;
  ModelFx fx_;
};

}  // namespace

std::unique_ptr<Generator> makeAdditive() { return std::make_unique<AdditiveGen>(); }

std::shared_ptr<const AuxData> buildAdditiveAux(const PluginState& s) {
  if (s.data.empty()) return nullptr;
  auto a = std::make_shared<AdditiveAux>();
  if (!parseAdditiveData(s.data, a->d) || a->d.level.empty()) return nullptr;
  return a;
}

}  // namespace gl::packs::models
