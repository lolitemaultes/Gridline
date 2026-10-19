// Gridline — "synths" pack, J-22: Twin VA (gl.twinva).
//
// Signal flow per voice: oscillator A (saw / pulse) and oscillator B (saw /
// pulse, pitch + detune, hard-synced to A and / or ring-modulated by A), each
// with up to seven unison copies spread in pitch and stereo; A/B balance, then
// the noise balance; a resonant low / band / high-pass filter (key and
// velocity tracking); the amplifier envelope. A second envelope and an LFO
// each modulate one destination (cutoff, A pitch, A pulse width, A+B pulse
// width, A+B pitch, LFO only: level); the mod wheel a third. After the voices:
// the 16-step trance gate (tempo synced, mix + smoothing), chorus, then the
// stereo delay. Voice modes: poly (polyphony limit), mono with glide (always /
// held-notes only, constant time or constant rate), octaver (adds the note an
// octave below), arpeggiator (up / down / up-down / random over 1..4 octaves).
// Control laws: the reference's value readouts give the envelope times
// (position⁴ scaled: envelope 5 ms..50 s, amp attack 1 ms..5 s, decay
// 1 ms..20 s, release 15 ms..10 s), LFO rate (position⁵ · 30 Hz), gain
// (−20..+10 dB), stepped pitches (osc B / transpose: whole semitones,
// truncated), osc B detune (0..50 ct), tempo delays (1..8 sixteenths) and
// chorus rate (0.1..5.1 Hz); cutoff / resonance / glide / gate laws are
// Gridline's (see the helpers below and va_voice.h).
#include <algorithm>
#include <array>
#include <cmath>
#include "../Packs.h"
#include "va.h"
#include "va_fx.h"
#include "va_voice.h"

namespace gl::packs::synths::va {

namespace {

constexpr int kVoices = 16;
constexpr int kUni = 7;

// ------------------------------------------------------------------ laws
// Pulse width 0..1 → duty 0.5 .. 0.02 mirrored (1 = square).
inline double duty(double v) { return std::clamp(0.5 * v, 0.02, 0.5); }
// Unison detune 0..1 → spread of ±50 cents at the outer voices.
inline double uniSpreadSt(double v) { return 0.5 * v; }
// Glide 0..1 → 0..2 s.
inline double glideS(double v) { return 2.0 * v * v; }
// position⁴ time law (ms) clamped to [lo, hi].
inline double quartMs(double v, double span, double lo, double hi) {
  v = std::clamp(v, 0.0, 1.0);
  return std::clamp(v * v * v * v * span + lo, lo, hi) / 1000.0;
}
// Tempo delay: 1..8 sixteenths.
inline double delaySixteenths(double v) { return std::floor(std::clamp(v, 0.0, 1.0) * 7.0) + 1.0; }
inline double chorusHz(double v) { return std::clamp(v, 0.0, 1.0) * 5.0 + 0.1; }
inline double lfoHzLaw(double v) {
  v = std::clamp(v, 0.0, 1.0);
  return std::min(30.0, v * v * v * v * v * 30.0);
}
// Sync step lengths (16ths) of the gate / arpeggiator digit wheels (0..6) and
// the LFO sync wheel (1..10; 0 = free).
constexpr double kGateSteps[7] = {1, 2, 4, 0.5, 3, 6, 8};
constexpr double kArpSteps[7] = {1, 2, 4, 0.5, 3, 6, 8};
constexpr double kLfoSync[11] = {0, 1, 2, 3, 4, 6, 8, 12, 16, 32, 64};

double lfoWave(int shape, double p, uint32_t cycle, uint32_t seed) {
  switch (shape) {
    case TlSaw: return 1.0 - 2.0 * p;
    case TlPulse: return p < 0.5 ? 1.0 : -1.0;
    case TlSine: return std::sin(dsp::kTwoPi * p);
    case TlRandom: return hashUnit(cycle * 2654435761u ^ seed);
    default: return p < 0.25 ? 4 * p : p < 0.75 ? 2 - 4 * p : 4 * p - 4;  // triangle
  }
}

struct Voice {
  bool on = false, held = false;
  uint32_t id = 0;
  double pitch = 60, target = 60;
  PitchGlide glide;
  float vel = 1;
  Env amp, eg;
  std::array<double, kUni> pa{}, pb{};
  dsp::Svf f1[2], f2[2];
  dsp::Noise noise;
  double lfoPh = 0;
  uint32_t lfoCycle = 0;
  uint64_t age = 0;
  bool octave = false;  // octaver copy
};

class TwinVa final : public Generator {
 public:
  TwinVa() {
    paramRanges("gl.twinva", lo_, hi_);
    ccVal_.fill(-1.f);
  }
  bool supportsGlide() const override { return true; }
  bool active() const override {
    if (dly_.on() && tail_ > 0) return true;
    for (const auto& v : v_)
      if (v.on) return true;
    return false;
  }

 protected:
  void onNoteOn(uint32_t id, double pitch, float vel) override {
    held_.add(id, pitch, vel);
    if (arpMode_ > 0) {
      if (held_.count == 1) arp_.reset();
      return;
    }
    play(id, pitch, vel, held_.count > 1);
  }
  void onNoteOff(uint32_t id) override {
    held_.remove(id);
    if (arpMode_ > 0) return;
    if (mono_) {
      if (const Held::N* n = held_.last()) {
        for (auto& v : v_)
          if (v.on && v.held) {
            v.id = n->id;
            setTarget(v, n->pitch, true);
          }
        return;
      }
    }
    release(id);
  }
  void onGlide(uint32_t id, double pitch, double seconds) override {
    for (auto& v : v_)
      if (v.on && v.id == id) {
        v.target = pitch;
        v.glide.start(v.pitch, pitch, std::max(seconds, 0.001), sr_);
      }
  }
  void onMidi(uint8_t status, uint8_t d1, uint8_t d2) override {
    if ((status & 0xF0) == 0xB0) {
      ccVal_[d1 & 127u] = d2 / 127.0f;
      if (d1 == 1) midiWheel_ = d2 / 127.0;
    }
    if ((status & 0xF0) == 0xE0) bend_ = ((d2 << 7 | d1) - 8192) / 8192.0;
  }
  void onAllOff() override {
    held_.count = 0;
    for (auto& v : v_) v.held = false, v.amp.gate(false), v.eg.gate(false);
  }
  void onKill() override {
    held_.count = 0;
    for (auto& v : v_) v.on = false;
    dly_.clear();
  }

  void configure(const PluginState& s, const ChannelAux& aux, const ProcessContext& ctx) override {
    sr_ = ctx.sampleRate;
    tempo_ = ctx.tempo;
    if (ctx.playing) songBeat_ = ctx.tick / std::max(1, ctx.ppq);
    playing_ = ctx.playing;
    for (size_t k = 0; k < p_.size(); ++k) p_[k] = k < s.params.size() && std::isfinite(s.params[k]) ? s.params[k] : 0.f;
    if (const auto* cl = dynamic_cast<const CcLinks*>(aux.custom.get()))
      for (int k = 0; k < cl->n; ++k) {
        const auto& l = cl->l[static_cast<size_t>(k)];
        if (l.tag < TwCount && ccVal_[l.cc] >= 0) p_[l.tag] = lo_[l.tag] + ccVal_[l.cc] * (hi_[l.tag] - lo_[l.tag]);
      }
    mono_ = P(TwMono) >= 0.5f;
    arpMode_ = std::clamp(static_cast<int>(std::lround(P(TwArpMode))), 0, 4);
    if (arpMode_ == 0 && arp_.playing) {
      for (auto& v : v_)
        if (v.on && v.id == kArpId) v.amp.gate(false), v.eg.gate(false), v.held = false;
      arp_.playing = false;
    }
    unison_ = std::clamp(static_cast<int>(std::lround(P(TwUnison))) + 1, 1, kUni);
    poly_ = std::clamp(static_cast<int>(std::lround(P(TwPoly))) + 1, 1, kVoices);
    const double ampA = quartMs(P(TwAmpA), 4999, 1, 5000), ampD = quartMs(P(TwAmpD), 19999, 1, 20000),
                 ampR = quartMs(P(TwAmpR), 9985, 15, 10000);
    const double egA = quartMs(P(TwEgA), 49995, 5, 50000), egD = quartMs(P(TwEgD), 49995, 5, 50000), egR = quartMs(P(TwEgR), 49995, 5, 50000);
    for (auto& v : v_) {
      v.amp.set(sr_, ampA, ampD, P(TwAmpS), ampR);
      v.eg.set(sr_, egA, egD, P(TwEgS), egR);
    }
    const double sixteenth = 15.0 / std::max(20.0, tempo_);
    dly_.set(sr_, P(TwDlyOn) >= 0.5f, delaySixteenths(P(TwDlyL)) * sixteenth, delaySixteenths(P(TwDlyR)) * sixteenth, 0.95 * P(TwDlyFb),
             P(TwDlyDepth),
             20.0 * std::pow(100.0, P(TwDlyLoCut)), 500.0 * std::pow(40.0, P(TwDlyHiCut)), P(TwDlyWet));
    cho_.set(sr_, P(TwChoOn) >= 0.5f, chorusHz(P(TwChoRate)), P(TwChoDepth), P(TwChoWet), 1.0);
    if (dly_.on()) tail_ = static_cast<int>(sr_ * 4);
  }

  void render(float* L, float* R, int n) override {
    const double gain = dsp::dbToGain(P(TwGain) + 5.0);  // the reference's default (−5 dB) plays at unity
    const double balance = P(TwBalance), noiseBal = P(TwNoise);
    const double egAmt = P(TwEgAmt), lfoAmt = P(TwLfoAmt), mwAmt = P(TwMwAmt);
    const int egDest = std::clamp(static_cast<int>(std::lround(P(TwEgDest))), 0, 4);
    const int lfoDest = std::clamp(static_cast<int>(std::lround(P(TwLfoDest))), 0, 5);
    const int mwDest = std::clamp(static_cast<int>(std::lround(P(TwMwDest))), 0, 4);
    const int lfoShape = std::clamp(static_cast<int>(std::lround(P(TwLfoShape))), 0, 4);
    const int lfoSync = std::clamp(static_cast<int>(std::lround(P(TwLfoSync))), 0, 10);
    const double lfoHz = lfoSync > 0 ? sixteenthsToHz(kLfoSync[lfoSync], tempo_) : lfoHzLaw(P(TwLfoRate));
    const bool lfoRetrig = P(TwLfoRetrig) >= 0.5f;
    const double mw = std::max(static_cast<double>(P(TwModWheel)), midiWheel_);
    const int filterMode = std::clamp(static_cast<int>(std::lround(P(TwFltMode))), 0, 2);
    const double cut = P(TwCutoff), reso = P(TwReso), key = P(TwKeyTrk), velTrk = P(TwVelTrk);
    const double master =
        std::floor(P(TwTranspose) + 1e-4) + P(TwFine) / 100.0 + bend_ * static_cast<double>(std::lround(P(TwBendRange)) + 1);
    const double bOff = std::floor(P(TwBPitch) + 1e-4) + P(TwBDetune) / 100.0;
    const bool aPulse = P(TwAShape) >= 0.5f, bPulse = P(TwBShape) >= 0.5f, sync = P(TwSync) >= 0.5f, ring = P(TwRing) >= 0.5f;
    const double spread = uniSpreadSt(P(TwUniDetune)), uniPan = P(TwUniPan);
    // Gate.
    const double gateMix = P(TwGateMix);
    const int gateSync = std::clamp(static_cast<int>(std::lround(P(TwGateSync))), 0, 6);
    const double gateStepBeats = kGateSteps[gateSync] / 4.0;
    const double gateSmooth = 0.0005 + 0.05 * P(TwGateSmooth);
    const double gateCoef = 1.0 - std::exp(-1.0 / (gateSmooth * sr_));
    const double beatInc = tempo_ / 60.0 / sr_;
    // Arpeggiator.
    const int arpSync = std::clamp(static_cast<int>(std::lround(P(TwArpSync))), 0, 6);
    const double arpStepBeats = kArpSteps[arpSync] / 4.0;
    const int arpOct = std::clamp(static_cast<int>(std::lround(P(TwArpRange))) + 1, 1, 4);
    const double lfoInc = lfoHz / sr_;

    for (int i = 0; i < n; ++i) {
      // Arpeggiator clock.
      if (arpMode_ > 0) {
        if (held_.count > 0) {
          arp_.phase += beatInc / arpStepBeats;
          if (arp_.phase >= 1.0) {
            arp_.phase -= std::floor(arp_.phase);
            if (arp_.next(held_, arpMode_ - 1, arpOct)) {
              release(kArpId);
              play(kArpId, arp_.curPitch, arp_.curVel, false);
              arp_.playing = true;
            }
          }
        } else if (arp_.playing) {
          release(kArpId);
          arp_.playing = false;
          arp_.phase = 1.0;
        }
      }
      // Global LFO (when not retriggered per voice).
      gLfoPh_ += lfoInc;
      if (gLfoPh_ >= 1) gLfoPh_ -= 1, ++gLfoCycle_;
      // Gate.
      double beat = playing_ ? songBeat_ : freeBeat_;
      const int step = static_cast<int>(std::floor(beat / gateStepBeats)) & 15;
      const double gTarget = P(TwGate1 + step) >= 0.5f ? 1.0 : 0.0;
      gate_ += (gTarget - gate_) * gateCoef;
      songBeat_ += beatInc;
      freeBeat_ += beatInc;

      double outL = 0, outR = 0;
      for (auto& v : v_) {
        if (!v.on) continue;
        ++v.age;
        v.glide.advance(v.pitch);
        const double ampEnv = v.amp.next(), egEnv = v.eg.next();
        if (!v.amp.active()) {
          v.on = false;
          continue;
        }
        double lfo;
        if (lfoRetrig) {
          v.lfoPh += lfoInc;
          if (v.lfoPh >= 1) v.lfoPh -= 1, ++v.lfoCycle;
          lfo = lfoWave(lfoShape, v.lfoPh, v.lfoCycle, v.id);
        } else {
          lfo = lfoWave(lfoShape, gLfoPh_, gLfoCycle_, 0x51);
        }
        // Modulation sums per destination (normalized units). The wheel can
        // also scale the LFO amount.
        double mod[TdCount] = {0, 0, 0, 0, 0, 0};
        static constexpr int kWheelToDest[TmCount] = {TdCutoff, -1, TdFreqA, TdPwA, TdPwAB};
        mod[egDest] += egAmt * egEnv;
        mod[lfoDest] += (lfoAmt + (mwDest == TmLfo ? mwAmt * mw : 0.0)) * lfo;
        if (kWheelToDest[mwDest] >= 0) mod[kWheelToDest[mwDest]] += mwAmt * mw;
        const double base = v.pitch + master + (v.octave ? -12.0 : 0.0);
        const double pitchA = base + 24.0 * (mod[TdFreqA] + mod[TdFreqAB]);
        const double pitchB = base + bOff + 24.0 * mod[TdFreqAB];
        const double dA = duty(std::clamp(P(TwAPw) + mod[TdPwA] + mod[TdPwAB], 0.0, 1.0));
        const double dB = duty(std::clamp(P(TwBPw) + mod[TdPwAB], 0.0, 1.0));
        const double hzA = dsp::noteToHz(pitchA), hzB = dsp::noteToHz(pitchB);
        double sL = 0, sR = 0;
        for (int u = 0; u < unison_; ++u) {
          const double off = unison_ == 1 ? 0.0 : (2.0 * u / (unison_ - 1) - 1.0);
          const double ratio = std::pow(2.0, off * spread / 12.0);
          const double dtA = std::min(0.45, hzA * ratio / sr_), dtB = std::min(0.45, hzB * ratio / sr_);
          double& a = v.pa[static_cast<size_t>(u)];
          double& b = v.pb[static_cast<size_t>(u)];
          double oa = aPulse ? (a < dA ? 1.0 : -1.0) + dsp::polyBlep(a, dtA) - dsp::polyBlep(std::fmod(a + 1.0 - dA, 1.0), dtA)
                             : 2.0 * a - 1.0 - dsp::polyBlep(a, dtA);
          double ob = bPulse ? (b < dB ? 1.0 : -1.0) + dsp::polyBlep(b, dtB) - dsp::polyBlep(std::fmod(b + 1.0 - dB, 1.0), dtB)
                             : 2.0 * b - 1.0 - dsp::polyBlep(b, dtB);
          if (ring) ob *= oa;
          a += dtA;
          b += dtB;
          if (a >= 1.0) {
            a -= 1.0;
            if (sync) b = a * dtB / dtA;
          }
          if (b >= 1.0) b -= std::floor(b);
          const double mix = oa * (1.0 - balance) + ob * balance;
          const double pan = off * uniPan;
          sL += mix * (pan > 0 ? 1.0 - pan : 1.0);
          sR += mix * (pan < 0 ? 1.0 + pan : 1.0);
        }
        const double norm = 1.0 / std::sqrt(static_cast<double>(unison_));
        const double nz = v.noise.next();
        sL = sL * norm * (1.0 - noiseBal) + nz * noiseBal;
        sR = sR * norm * (1.0 - noiseBal) + nz * noiseBal;
        // Filter.
        const double c = std::clamp(cut + mod[TdCutoff] + key * v.pitch / 120.0 /* key tracking from C-1, ~1 oct / oct at 100% */ + velTrk * (v.vel - 1.0) * 0.5, 0.0, 1.0);
        const double hz = cutoffHz(c);
        for (int ch = 0; ch < 2; ++ch) {
          v.f1[ch].set(sr_, hz, reso * 0.97);
          v.f2[ch].set(sr_, hz, reso * 0.6);
          double x = ch ? sR : sL, bp = 0, hp = 0;
          double lp = v.f1[ch].process(x, &bp, &hp);
          double y = filterMode == TfLow ? v.f2[ch].process(lp) : filterMode == TfBand ? bp : hp;
          (ch ? sR : sL) = y;
        }
        const double g = ampEnv * v.vel * 0.35, pan = std::clamp(mod[TdPan], -1.0, 1.0);
        outL += sL * g * (pan > 0 ? 1.0 - pan : 1.0);
        outR += sR * g * (pan < 0 ? 1.0 + pan : 1.0);
      }
      const double gateGain = 1.0 - gateMix * (1.0 - gate_);
      L[i] += static_cast<float>(outL * gain * gateGain);
      R[i] += static_cast<float>(outR * gain * gateGain);
    }
    cho_.process(L, R, n);
    dly_.process(L, R, n);
    if (tail_ > 0) tail_ -= n;
  }

 private:
  static constexpr uint32_t kArpId = 0xA9A9A9A9u;
  float P(int i) const { return i < static_cast<int>(p_.size()) ? p_[static_cast<size_t>(i)] : 0.f; }

  void setTarget(Voice& v, double pitch, bool legato) {
    const bool heldOnly = P(TwGlideHeld) >= 0.5f, constTime = P(TwGlideStatic) >= 0.5f;
    const double t = glideS(P(TwGlide));
    if (t > 0.0005 && (!heldOnly || legato)) {
      const double dist = std::fabs(pitch - v.pitch);
      v.glide.start(v.pitch, pitch, constTime ? t : t * std::max(1.0, dist) / 12.0, sr_);
    } else {
      v.pitch = pitch;
      v.glide.left = 0;
    }
    v.target = pitch;
  }

  void startVoice(Voice& v, uint32_t id, double pitch, float vel, bool octave) {
    const bool wasOn = v.on;
    v.on = v.held = true;
    v.id = id;
    v.vel = vel;
    v.octave = octave;
    v.age = 0;
    if (!wasOn) {
      v.pitch = pitch;
      v.glide.left = 0;
      for (int u = 0; u < kUni; ++u) {
        v.pa[static_cast<size_t>(u)] = 0.5 + 0.5 * hashUnit(id * 31u + static_cast<uint32_t>(u));
        v.pb[static_cast<size_t>(u)] = 0.5 + 0.5 * hashUnit(id * 17u + static_cast<uint32_t>(u) + 99u);
      }
      for (int c = 0; c < 2; ++c) v.f1[c].reset(), v.f2[c].reset();
    }
    v.lfoPh = 0;
    v.lfoCycle = 0;
    v.noise.seed(id * 747796405u + 1u);
    v.amp.gate(true);
    v.eg.gate(true);
  }

  void play(uint32_t id, double pitch, float vel, bool legato) {
    const bool oct = P(TwOctaver) >= 0.5f;
    if (mono_ || arpMode_ > 0) {
      Voice& v = v_[0];
      if (v.on && (v.held || arpMode_ > 0) && mono_) {
        v.id = id;
        v.vel = vel;
        setTarget(v, pitch, legato);
        if (!legato || arpMode_ > 0) v.amp.gate(true), v.eg.gate(true);
        v.held = true;
      } else {
        startVoice(v, id, pitch, vel, false);
      }
      if (oct) startVoice(v_[1], id, pitch, vel * 0.7f, true);
      return;
    }
    startVoice(alloc(), id, pitch, vel, false);
    if (oct) startVoice(alloc(), id, pitch, vel * 0.7f, true);
  }

  Voice& alloc() {
    int active = 0;
    for (auto& v : v_) active += v.on && v.held ? 1 : 0;
    if (active >= poly_) {  // over the polyphony limit: retire the oldest held voice
      Voice* old = nullptr;
      for (auto& v : v_)
        if (v.on && v.held && (!old || v.age > old->age)) old = &v;
      if (old) old->held = false, old->amp.gate(false), old->eg.gate(false);
    }
    for (auto& v : v_)
      if (!v.on) return v;
    Voice* q = &v_[0];
    for (auto& v : v_)
      if (!v.held && (q->held || v.amp.v < q->amp.v)) q = &v;
    if (q->held)
      for (auto& v : v_)
        if (v.age > q->age) q = &v;
    q->on = false;
    return *q;
  }

  void release(uint32_t id) {
    for (auto& v : v_)
      if (v.on && v.held && v.id == id) {
        v.held = false;
        v.amp.gate(false);
        v.eg.gate(false);
      }
  }

  std::array<Voice, kVoices> v_{};
  std::array<float, TwCount> p_{}, lo_{}, hi_{};
  std::array<float, 128> ccVal_{};
  Held held_;
  Arp arp_;
  StereoDelay dly_;
  Chorus cho_;
  double sr_ = 44100, tempo_ = 120, songBeat_ = 0, freeBeat_ = 0, gate_ = 1, gLfoPh_ = 0, midiWheel_ = 0, bend_ = 0;
  uint32_t gLfoCycle_ = 0;
  bool playing_ = false, mono_ = false;
  int arpMode_ = 0, unison_ = 1, poly_ = 16, tail_ = 0;
};

}  // namespace

std::unique_ptr<Generator> makeTwinVa() { return std::make_unique<TwinVa>(); }

}  // namespace gl::packs::synths::va
