// Gridline — "synths" pack, J-22: Saw Swarm (gl.sawswarm).
//
// Signal flow per voice: a stack of 1 / 2 / 4 / 8 band-limited saws (detune
// and stereo spread, free-running or restarted phases), optionally hard-synced
// to a master at the note pitch (the stack then plays at the sync frequency);
// a "sub-saw" at a free octave offset (−2..+2 octaves, continuous) added with
// a phase offset — or subtracted (invert), or folded into a square — so the
// pair works like pulse-width / octave tricks of a dual-saw; noise; a 4-mode
// filter (LP24 / LP12 / BP / HP) with key / velocity tracking and its own
// ADSR (bipolar amount to cutoff, sync frequency, sub phase or master pitch);
// an LFO (triangle / saw / square / random, free or tempo synced, fade-in /
// fade-out, retrigger, invert) to one of seven destinations; the amplifier
// ADSR, level and pan. A 6-slot MIDI matrix adds velocity, aftertouch, pitch
// wheel or any controller to any parameter (normalized offset). Effects:
// chorus → phaser → delay → reverb. Voice modes: poly (polyphony list), mono
// with glide (always / held notes, constant time or rate), octaver, unison
// retrigger, arpeggiator.
// Control laws from the reference's value readouts: level ±24 dB, envelope
// times max(50·v⁴ s, 1 ms) (amp release ×0.35 + 20 ms), LFO 10·v³ Hz or ten
// tempo steps, LFO fades 10·v² s, delay times 1..8 sixteenths, sub-saw in nine
// half-octave steps, selectors truncated, switches on at any non-zero value;
// cutoff / resonance / effect depths are Gridline's.
#include <algorithm>
#include <array>
#include <cmath>
#include "../Packs.h"
#include "va.h"
#include "va_fx.h"
#include "va_voice.h"

namespace gl::packs::synths::va {

namespace {

constexpr int kVoices = 24;
constexpr int kStack = 8;
// LFO tempo divisions (in 16th notes): 1/16, 2/16, 3/16, 1/4, 2/4, 3/4, 4/4, 8/4, 16/4, 32/4.
constexpr double kLfoDivs[10] = {1, 2, 3, 4, 8, 12, 16, 32, 64, 128};

inline int idx(float v, int n) { return std::clamp(static_cast<int>(v * static_cast<float>(n - 1)), 0, n - 1); }  // normalized knob → step (truncated)
// Reference time law: max(50·v⁴, 1 ms) seconds.
inline double quart50(double v) {
  v = std::clamp(v, 0.0, 1.0);
  return std::max(50.0 * v * v * v * v, 0.00101);
}
inline int pick(float v, int n) { return std::clamp(static_cast<int>(std::lround(v)), 0, n - 1); }        // choice index

struct Voice {
  bool on = false, held = false, octave = false;
  uint32_t id = 0;
  double pitch = 60;
  PitchGlide glide;
  float vel = 1;
  Env amp, eg;
  std::array<double, kStack> ph{};
  double master = 0, sub = 0;
  dsp::Svf f1[2], f2[2];
  dsp::Noise noise;
  double lfoPh = 0, lfoFade = 0, lfoRel = 1;
  uint32_t lfoCycle = 0;
  uint64_t age = 0;
};

class SawSwarm final : public Generator {
 public:
  SawSwarm() {
    paramRanges("gl.sawswarm", lo_, hi_);
    ccSet_.fill(false);
  }
  bool supportsGlide() const override { return true; }
  bool active() const override {
    if (tail_ > 0) return true;
    for (const auto& v : v_)
      if (v.on) return true;
    return false;
  }

 protected:
  void onNoteOn(uint32_t id, double pitch, float vel) override {
    held_.add(id, pitch, vel);
    if (arpOn_) {
      if (held_.count == 1) arp_.reset();
      return;
    }
    play(id, pitch, vel, held_.count > 1);
  }
  void onNoteOff(uint32_t id) override {
    held_.remove(id);
    if (arpOn_) return;
    if (mono_) {
      if (const Held::N* n = held_.last()) {
        Voice& v = v_[0];
        if (v.on && v.held) {
          v.id = n->id;
          glideTo(v, n->pitch, true);
        }
        return;
      }
    }
    release(id);
  }
  void onGlide(uint32_t id, double pitch, double seconds) override {
    for (auto& v : v_)
      if (v.on && v.id == id) v.glide.start(v.pitch, pitch, std::max(seconds, 0.001), sr_);
  }
  void onMidi(uint8_t status, uint8_t d1, uint8_t d2) override {
    switch (status & 0xF0) {
      case 0xB0: cc_[d1 & 127u] = d2 / 127.0f, ccSet_[d1 & 127u] = true; break;
      case 0xD0: after_ = d1 / 127.0f; break;
      case 0xE0: bend_ = ((d2 << 7 | d1) - 8192) / 8192.0f; break;
      default: break;
    }
  }
  void onAllOff() override {
    held_.count = 0;
    for (auto& v : v_) v.held = false, v.amp.gate(false), v.eg.gate(false);
  }
  void onKill() override {
    held_.count = 0;
    for (auto& v : v_) v.on = false;
    dly_.clear();
    rev_.clear();
  }

  void configure(const PluginState& s, const ChannelAux& aux, const ProcessContext& ctx) override {
    sr_ = ctx.sampleRate;
    tempo_ = ctx.tempo;
    for (size_t k = 0; k < base_.size(); ++k) {
      const float x = k < s.params.size() ? s.params[k] : 0.f;
      base_[k] = std::isfinite(x) ? x : 0.f;
    }
    if (const auto* cl = dynamic_cast<const CcLinks*>(aux.custom.get()))
      for (int k = 0; k < cl->n; ++k) {
        const auto& l = cl->l[static_cast<size_t>(k)];
        if (l.tag < SwCount && ccSet_[l.cc]) base_[l.tag] = lo_[l.tag] + cc_[l.cc] * (hi_[l.tag] - lo_[l.tag]);
      }
    // Global MIDI-matrix sources → effective parameters.
    const float cc1 = std::max(cc_[1], base_[SwModWheel]);
    eff_ = base_;
    for (int m = 0; m < kSwModSlots; ++m) {
      const int src = static_cast<int>(std::lround(base_[static_cast<size_t>(SwModSrc1 + m)]));
      const int dst = static_cast<int>(std::lround(base_[static_cast<size_t>(SwModDest1 + m)]));
      if (src <= 1 || dst < 0 || dst >= SwModAmt1) continue;  // none / velocity (per voice)
      const double amt = base_[static_cast<size_t>(SwModAmt1 + m)];
      double val = 0;
      if (src == 2) val = after_;
      else if (src == 3) val = bend_;
      else if (src - 4 < kSwCcCount) val = kSwCc[src - 4] == 1 ? cc1 : cc_[static_cast<size_t>(kSwCc[src - 4])];
      const size_t d = static_cast<size_t>(dst);
      eff_[d] = static_cast<float>(std::clamp(eff_[d] + amt * val * (hi_[d] - lo_[d]), static_cast<double>(lo_[d]), static_cast<double>(hi_[d])));
    }
    mono_ = P(SwMono) >= 0.5f;
    const bool arp = P(SwArpOn) >= 0.5f;
    if (!arp && arpOn_ && arp_.playing) release(kArpId), arp_.playing = false;
    arpOn_ = arp;
    stack_ = 1 << pick(P(SwUnison), 4);
    poly_ = kSwPoly[pick(P(SwPoly), kSwPolyChoices)];
    const double aA = quart50(P(SwAmpA)), aD = quart50(P(SwAmpD)), aR = quart50(P(SwAmpR)) * 0.35 + 0.02;
    const double eA = quart50(P(SwEgA)), eD = quart50(P(SwEgD)), eR = quart50(P(SwEgR));
    for (auto& v : v_) {
      v.amp.set(sr_, aA, aD, P(SwAmpS), aR);
      v.eg.set(sr_, eA, eD, P(SwEgS), eR);
    }
    cho_.set(sr_, P(SwChoOn) >= 0.5f, 0.05 * std::pow(100.0, P(SwChoRate)), P(SwChoDepth), P(SwChoMix), 1.0);
    ph_.set(sr_, P(SwPhOn) >= 0.5f, 100.0 * std::pow(60.0, P(SwPhCenter)), 0.02 * std::pow(500.0, P(SwPhRate)), P(SwPhDepth),
            0.9 * P(SwPhFb), P(SwPhMix));
    const double sixteenth = 15.0 / std::max(20.0, tempo_);  // delay times: 1..8 sixteenths
    dly_.set(sr_, P(SwDlyOn) >= 0.5f, (idx(P(SwDlyL), 8) + 1) * sixteenth, (idx(P(SwDlyR), 8) + 1) * sixteenth, 0.95 * P(SwDlyFb),
             P(SwDlyDepth),
             20.0 * std::pow(100.0, P(SwDlyLoCut)), 500.0 * std::pow(40.0, P(SwDlyHiCut)), P(SwDlyMix));
    rev_.set(sr_, P(SwRevOn) >= 0.5f, P(SwRevDecay), 1000.0 * std::pow(18.0, P(SwRevHiCut)), P(SwRevDamp), P(SwRevMix));
    if (dly_.on() || rev_.on()) tail_ = static_cast<int>(sr_ * 5);
  }

  void render(float* L, float* R, int n) override {
    const double level = dsp::dbToGain(P(SwLevel)) * 0.5;  // ±24 dB around the reference's centre
    const double pan = P(SwPan);
    const double master = std::trunc(P(SwTranspose)) + P(SwFine) / 100.0;
    const double bendSt = static_cast<double>(bend_) * kSwBend[pick(P(SwBendRange), kSwBendSteps)];
    const double detune = 0.6 * P(SwUniDetune), uniPan = P(SwUniPan);
    const bool syncOn = P(SwSyncOn) >= 0.5f, subInv = P(SwSubInvert) >= 0.5f, subSq = P(SwSubSquare) >= 0.5f;
    const double subLvl = P(SwSubLevel), noise = P(SwNoise);
    const int fMode = pick(P(SwFltMode), 4);
    const double egAmt = P(SwEgAmt);
    const int egDest = pick(P(SwEgDest), 4);
    const int lfoShape = pick(P(SwLfoShape), 4), lfoDest = pick(P(SwLfoDest), 7);
    const bool lfoSync = P(SwLfoSync) >= 0.5f, lfoRetrig = P(SwLfoRetrig) >= 0.5f, lfoInv = P(SwLfoInvert) >= 0.5f;
    const double lfoHz = lfoSync ? sixteenthsToHz(kLfoDivs[idx(P(SwLfoSpeed), 10)], tempo_)
                                 : 10.0 * P(SwLfoSpeed) * P(SwLfoSpeed) * P(SwLfoSpeed);  // free: 10·v³ Hz
    const double lfoAmt = P(SwLfoAmt) * (lfoInv ? -1.0 : 1.0);
    // LFO attack / release: 10·v² seconds.
    const double lfoFadeIn = 1.0 / std::max(1.0, 10.0 * P(SwLfoAttack) * P(SwLfoAttack) * sr_);
    const double lfoFadeOut = 1.0 / std::max(1.0, 10.0 * P(SwLfoRelease) * P(SwLfoRelease) * sr_);
    const double beatInc = tempo_ / 60.0 / sr_;
    const int arpMode = pick(P(SwArpMode), 4), arpOct = pick(P(SwArpRange), 4) + 1;
    const double arpStep = kSwArpStep16[pick(P(SwArpSync), kSwArpSteps)] / 4.0;  // 16ths → beats
    const double lfoInc = lfoHz / sr_;

    for (int i = 0; i < n; ++i) {
      if (arpOn_) {
        if (held_.count > 0) {
          arp_.phase += beatInc / arpStep;
          if (arp_.phase >= 1.0) {
            arp_.phase -= std::floor(arp_.phase);
            if (arp_.next(held_, arpMode, arpOct)) {
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
      gLfo_ += lfoInc;
      if (gLfo_ >= 1) gLfo_ -= 1, ++gCycle_;
      double oL = 0, oR = 0;
      for (auto& v : v_) {
        if (!v.on) continue;
        ++v.age;
        v.glide.advance(v.pitch);
        const double a = v.amp.next(), e = v.eg.next();
        if (!v.amp.active()) {
          v.on = false;
          continue;
        }
        // LFO (per voice when retriggered).
        double ph = gLfo_;
        uint32_t cyc = gCycle_;
        if (lfoRetrig) {
          v.lfoPh += lfoInc;
          if (v.lfoPh >= 1) v.lfoPh -= 1, ++v.lfoCycle;
          ph = v.lfoPh;
          cyc = v.lfoCycle;
        }
        double lfo;
        switch (lfoShape) {
          case SsSaw: lfo = 2.0 * ph - 1.0; break;
          case SsSquare: lfo = ph < 0.5 ? 1.0 : -1.0; break;
          case SsRandom: lfo = hashUnit(cyc * 2654435761u ^ 0x77u); break;
          default: lfo = ph < 0.25 ? 4 * ph : ph < 0.75 ? 2 - 4 * ph : 4 * ph - 4; break;
        }
        v.lfoFade = std::min(1.0, v.lfoFade + lfoFadeIn);
        if (!v.held) v.lfoRel = std::max(0.0, v.lfoRel - lfoFadeOut);
        lfo *= lfoAmt * v.lfoFade * v.lfoRel;
        double modE[SeCount] = {0, 0, 0, 0};
        modE[egDest] = egAmt * e;
        double modL[SlCount] = {0, 0, 0, 0, 0, 0, 0};
        modL[lfoDest] = lfo;
        const double vp = v.pitch + master + bendSt + (v.octave ? -12.0 : 0.0) + 24.0 * (modE[SeMasterFreq] + modL[SlMasterFreq]);
        const double hz = dsp::noteToHz(vp);
        const double dtM = std::min(0.45, hz / sr_);
        const double syncRatio = std::pow(2.0, 4.0 * std::clamp(VP(v, SwSyncFreq) + modE[SeSyncFreq] + modL[SlSyncFreq], 0.0, 1.0));
        double stackL = 0, stackR = 0;
        bool reset = false;
        if (syncOn) {
          v.master += dtM;
          if (v.master >= 1.0) v.master -= 1.0, reset = true;
        }
        for (int u = 0; u < stack_; ++u) {
          const double off = stack_ == 1 ? 0.0 : 2.0 * u / (stack_ - 1) - 1.0;
          const double dt = std::min(0.45, dtM * (syncOn ? syncRatio : 1.0) * std::pow(2.0, off * detune / 12.0));
          double& p = v.ph[static_cast<size_t>(u)];
          if (reset) p = v.master * dt / dtM;
          const double s = 2.0 * p - 1.0 - dsp::polyBlep(p, dt);
          p += dt;
          if (p >= 1.0) p -= std::floor(p);
          const double pn = off * uniPan;
          stackL += s * (pn > 0 ? 1.0 - pn : 1.0);
          stackR += s * (pn < 0 ? 1.0 + pn : 1.0);
        }
        const double norm = 1.0 / std::sqrt(static_cast<double>(stack_));
        // Sub-saw: its own pitch; the phase offset compares it with a shifted copy.
        const double subOct = -2.0 + 0.5 * std::round(VP(v, SwSubOct));  // nine half-octave steps
        const double dtS = std::min(0.45, dtM * std::pow(2.0, subOct + VP(v, SwSubDetune) * 0.5 / 12.0));
        const double sphase = std::clamp(VP(v, SwSubPhase) + modE[SeSubPhase] + modL[SlSubPhase], 0.0, 1.0);
        auto sawAt = [&](double p) {
          p -= std::floor(p);
          return 2.0 * p - 1.0 - dsp::polyBlep(p, dtS);
        };
        double sub = sawAt(v.sub);
        const double shifted = sawAt(v.sub + sphase);
        sub = subSq ? 0.5 * (sub - shifted) : subInv ? -shifted : shifted;
        v.sub += dtS;
        if (v.sub >= 1.0) v.sub -= std::floor(v.sub);
        const double nz = v.noise.next() * std::clamp(noise + modL[SlNoise], 0.0, 1.0);
        double sL = stackL * norm + sub * subLvl * 0.8 + nz;
        double sR = stackR * norm + sub * subLvl * 0.8 + nz;
        const double c = std::clamp(VP(v, SwCutoff) + modE[SeCutoff] + modL[SlCutoff] + VP(v, SwKeyTrk) * v.pitch / 120.0 /* key tracking from C-1, ~1 oct / oct at 100% */ +
                                        VP(v, SwVelTrk) * (v.vel - 1.0) * 0.5,
                                    0.0, 1.0);
        const double fc = cutoffHz(c), rs = VP(v, SwReso);
        for (int ch = 0; ch < 2; ++ch) {
          double x = ch ? sR : sL, bp = 0, hp = 0, y;
          switch (fMode) {
            case SfLp24:
              v.f1[ch].set(sr_, fc, rs * 0.6);
              v.f2[ch].set(sr_, fc, rs * 0.97);
              y = v.f2[ch].process(v.f1[ch].process(x));
              break;
            case SfLp12:
              v.f1[ch].set(sr_, fc, rs * 0.97);
              y = v.f1[ch].process(x);
              break;
            case SfBp:
              v.f1[ch].set(sr_, fc, rs * 0.97);
              v.f1[ch].process(x, &bp);
              y = bp;
              break;
            default:
              v.f1[ch].set(sr_, fc, rs * 0.97);
              v.f1[ch].process(x, nullptr, &hp);
              y = hp;
              break;
          }
          (ch ? sR : sL) = y;
        }
        const double lvl = std::clamp(1.0 + modL[SlLevel], 0.0, 2.0) * a * v.vel * 0.3;
        const double pv2 = std::clamp(pan + modL[SlPan], -1.0, 1.0);
        oL += sL * lvl * (pv2 > 0 ? 1.0 - pv2 : 1.0);
        oR += sR * lvl * (pv2 < 0 ? 1.0 + pv2 : 1.0);
      }
      L[i] += static_cast<float>(oL * level);
      R[i] += static_cast<float>(oR * level);
    }
    cho_.process(L, R, n);
    ph_.process(L, R, n);
    dly_.process(L, R, n);
    rev_.process(L, R, n);
    if (tail_ > 0) tail_ -= n;
  }

 private:
  static constexpr uint32_t kArpId = 0xA8A8A8A8u;
  float P(int i) const { return eff_[static_cast<size_t>(i)]; }
  // Parameter with this voice's velocity routes added.
  float VP(const Voice& v, int i) const {
    float x = eff_[static_cast<size_t>(i)];
    for (int m = 0; m < kSwModSlots; ++m)
      if (std::lround(base_[static_cast<size_t>(SwModSrc1 + m)]) == 1 && std::lround(base_[static_cast<size_t>(SwModDest1 + m)]) == i)
        x += static_cast<float>(base_[static_cast<size_t>(SwModAmt1 + m)] * v.vel * (hi_[static_cast<size_t>(i)] - lo_[static_cast<size_t>(i)]));
    return std::clamp(x, lo_[static_cast<size_t>(i)], hi_[static_cast<size_t>(i)]);
  }

  void glideTo(Voice& v, double pitch, bool legato) {
    const bool heldOnly = P(SwGlideHeld) >= 0.5f, constTime = P(SwGlideStatic) >= 0.5f;
    const double t = 2.0 * P(SwGlide) * P(SwGlide);
    if (t > 0.0005 && (!heldOnly || legato))
      v.glide.start(v.pitch, pitch, constTime ? t : t * std::max(1.0, std::fabs(pitch - v.pitch)) / 12.0, sr_);
    else
      v.pitch = pitch, v.glide.left = 0;
  }

  void start(Voice& v, uint32_t id, double pitch, float vel, bool octave) {
    const bool was = v.on;
    v.on = v.held = true;
    v.id = id;
    v.vel = vel;
    v.octave = octave;
    v.age = 0;
    v.lfoFade = P(SwLfoAttack) < 0.001f ? 1.0 : 0.0;
    v.lfoRel = 1.0;
    if (!was) {
      v.pitch = pitch;
      v.glide.left = 0;
      for (int c = 0; c < 2; ++c) v.f1[c].reset(), v.f2[c].reset();
    } else {  // J-92: a reused (releasing / stolen) voice took the new note at its OLD pitch
      v.pitch = pitch;
      v.glide.left = 0;
    }
    const bool retrig = P(SwUniRetrig) >= 0.5f;
    if (!was || retrig)
      for (int u = 0; u < kStack; ++u) v.ph[static_cast<size_t>(u)] = retrig ? 0.0 : 0.5 + 0.5 * hashUnit(id * 131u + static_cast<uint32_t>(u));
    if (!was || P(SwSubRetrig) >= 0.5f) v.sub = 0;
    v.master = 0;
    v.lfoPh = 0;
    v.lfoCycle = 0;
    v.noise.seed(id * 2891336453u + 3u);
    v.amp.gate(true);
    v.eg.gate(true);
  }

  void play(uint32_t id, double pitch, float vel, bool legato) {
    const bool oct = P(SwOctaver) >= 0.5f;
    if (mono_) {
      Voice& v = v_[0];
      if (v.on && v.held) {
        v.id = id;
        v.vel = vel;
        glideTo(v, pitch, legato);
        if (!legato) v.amp.gate(true), v.eg.gate(true);
      } else {
        const bool releasing = v.on;
        const double from = v.pitch;
        start(v, id, pitch, vel, false);
        if (releasing) v.pitch = from, glideTo(v, pitch, false);  // glides unless "Glide held notes" only
      }
      if (oct) start(v_[1], id, pitch, vel * 0.7f, true);
      return;
    }
    start(alloc(), id, pitch, vel, false);
    if (oct) start(alloc(), id, pitch, vel * 0.7f, true);
  }

  Voice& alloc() {
    int heldCount = 0;
    for (auto& v : v_) heldCount += v.on && v.held ? 1 : 0;
    if (heldCount >= poly_) {
      Voice* old = nullptr;
      for (auto& v : v_)
        if (v.on && v.held && (!old || v.age > old->age)) old = &v;
      if (old) old->held = false, old->amp.gate(false), old->eg.gate(false);
    }
    for (auto& v : v_)
      if (!v.on) return v;
    Voice* q = &v_[0];
    for (auto& v : v_)
      if (v.age > q->age) q = &v;
    q->on = false;
    return *q;
  }

  void release(uint32_t id) {
    for (auto& v : v_)
      if (v.on && v.held && v.id == id) v.held = false, v.amp.gate(false), v.eg.gate(false);
  }

  std::array<Voice, kVoices> v_{};
  std::array<float, SwCount> base_{}, eff_{}, lo_{}, hi_{};
  std::array<float, 128> cc_{};
  std::array<bool, 128> ccSet_{};
  float after_ = 0, bend_ = 0;
  Held held_;
  Arp arp_;
  Chorus cho_;
  Phaser ph_;
  StereoDelay dly_;
  Reverb rev_;
  double sr_ = 44100, tempo_ = 120, gLfo_ = 0;
  uint32_t gCycle_ = 0;
  bool mono_ = false, arpOn_ = false;
  int stack_ = 1, poly_ = 16, tail_ = 0;
};

}  // namespace

std::unique_ptr<Generator> makeSawSwarm() { return std::make_unique<SawSwarm>(); }

}  // namespace gl::packs::synths::va
