// Gridline — String Model: two coupled waveguide strings per voice (a delay
// loop with damping lowpass, dispersion allpasses for stiffness and a soft
// saturator), driven by a pluck / pick / hammer burst or a continuous bow /
// breath exciter. The loop is tuned exactly by subtracting the filters' phase
// delay at the fundamental. Then: per-voice hi-cut with envelope, body
// resonator bank, lo-cut, amp (drive / tone / cabinet) and the effect block.
// A strummer turns each note into a chord played string by string.
#include <array>
#include <cmath>
#include <complex>
#include "models_rt.h"

namespace gl::packs::models {

namespace {

using namespace dsp;
constexpr int kCtl = 32;
constexpr int kVoices = 32;
constexpr int kLine = 8192;
constexpr int kAp = 4;

struct Wg {
  float buf[kLine];
  int w = 0;
  double len = 100;
  float gain = 0.99f, d = 0, lp = 0, apx[kAp] = {}, apy[kAp] = {};
  void clear() {
    std::fill(buf, buf + kLine, 0.0f);
    w = 0;
    lp = 0;
    for (int i = 0; i < kAp; ++i) apx[i] = apy[i] = 0;
  }
  float read(double d) const {
    double rp = static_cast<double>(w) - d;
    while (rp < 0) rp += kLine;
    int i0 = static_cast<int>(rp);
    float f = static_cast<float>(rp - i0);
    int i1 = (i0 + 1) & (kLine - 1);
    i0 &= kLine - 1;
    return buf[i0] + (buf[i1] - buf[i0]) * f;
  }
};

class StringGen final : public Generator {
 public:
  StringGen() : voices_(new Voice[kVoices]) {
    for (int i = 0; i < kVoices; ++i) {
      voices_[i].s[0].clear();
      voices_[i].s[1].clear();
    }
  }
  bool active() const override {
    for (int i = 0; i < kVoices; ++i)
      if (voices_[i].on) return true;
    return fx_.ringing() || tail_ > 0;
  }

 protected:
  struct Voice {
    bool on = false, held = false;
    uint32_t id = 0;
    double pitch = 60;
    float vel = 1;
    uint64_t age = 0;
    int delay = 0;      // strum: samples before the string is struck
    int excPos = 0, excLen = 0;
    float exc[kLine];   // burst excitation (pluck / pick / hammer)
    Wg s[2];
    Adsr amp;
    Svf hicut;
    float hcEnv = 1, panL = 1, panR = 1, quiet = 0;
    double silent = 0;
    float lpBreath = 0;
    Noise rng;
  };

  void configure(const PluginState& s, const ChannelAux&, const ProcessContext& ctx) override {
    bool srChanged = sr_ != ctx.sampleRate;
    sr_ = ctx.sampleRate;
    exciter_ = std::clamp(prmi(s, StExciter, 0), 0, 4);
    excPos_ = prmf(s, StExcPos, 0.18f);
    excHard_ = prmf(s, StExcHard, 0.6f);
    excNoise_ = prmf(s, StExcNoise, 0.1f);
    bowForce_ = prmf(s, StBowForce, 0.5f);
    decay_ = prmf(s, StDecay, 4);
    damp_ = prmf(s, StDamp, 0.35f);
    stiff_ = prmf(s, StStiffness, 0.1f);
    pickup_ = prmf(s, StPickup, 0.25f);
    sat_ = prmf(s, StSaturation, 0.1f);
    ratio2_ = stringRatio(prmi(s, St2Ratio, 1));
    level2_ = prmi(s, St2Ratio, 1) == 0 ? 0.0f : prmf(s, St2Level, 0.5f);
    detune_ = prmf(s, StDetune, 4);
    hiCut_ = prmf(s, StHiCut, 9000);
    hiCutEnv_ = prmf(s, StHiCutEnv, 0);
    hiCutDec_ = prmf(s, StHiCutDec, 0.4f);
    release_ = prmf(s, StRelease, 0.3f);
    strum_ = prmi(s, StStrum, 0);
    strumTime_ = prmf(s, StStrumTime, 0.025f);
    strumDir_ = prmi(s, StStrumDir, 0);
    drive_ = prmf(s, StDrive, 0);
    vibrato_ = prmf(s, StVibrato, 0);
    vibRate_ = prmf(s, StVibRate, 5);
    spread_ = prmf(s, StSpread, 0.3f);
    maxVoices_ = std::clamp(prmi(s, StVoices, 16), 1, kVoices);
    vol_ = prmf(s, StVolume, 0.8f);
    float att = prmf(s, StAttack, 0.002f), sus = prmf(s, StSustain, 1);
    for (int i = 0; i < kVoices; ++i) voices_[i].amp.set(sr_, att, 1.0, sus, release_);
    // Global chain.
    int body = std::clamp(prmi(s, StBody, 1), 0, kBodies - 1);
    bodyMix_ = body == 0 ? 0.0f : prmf(s, StBodyMix, 0.5f);
    float size = prmf(s, StBodySize, 1);
    if (srChanged || body != body_ || size != bodySize_) {
      body_ = body;
      bodySize_ = size;
      const BodyMode* bm = bodyModes(body);
      for (int k = 0; k < kBodyModes; ++k) {
        bodyGain_[k] = bm[k].gain;
        for (int c = 0; c < 2; ++c) bodyF_[c][k].set(Biquad::BandPass, sr_, bm[k].hz / size * (c ? 1.012 : 1.0), bm[k].q, 0);
      }
    }
    float lo = prmf(s, StLoCut, 40);
    float tone = prmf(s, StTone, 0);
    bool cab = prmf(s, StCab, 0) >= 0.5f;
    if (srChanged || lo != loCutHz_ || tone != tone_ || cab != cab_) {
      loCutHz_ = lo;
      tone_ = tone;
      cab_ = cab;
      for (int c = 0; c < 2; ++c) {
        loCut_[c].set(Biquad::HighPass, sr_, lo, 0.707, 0);
        toneF_[c].set(Biquad::HighShelf, sr_, 1800, 0.7, tone * 12.0);
        cabLp_[c].set(Biquad::LowPass, sr_, 4200, 0.9, 0);
        cabHp_[c].set(Biquad::HighPass, sr_, 95, 0.8, 0);
        cabPk_[c].set(Biquad::Peak, sr_, 1700, 1.2, 2.5);
      }
    }
    fx_.set(s, StFx, sr_);
  }

  void retune(Voice& v, double semis) {
    for (int k = 0; k < 2; ++k) {
      Wg& g = v.s[k];
      double hz = noteToHz(v.pitch + semis) * (k ? ratio2_ * std::pow(2.0, detune_ / 1200.0) : 1.0);
      hz = std::clamp(hz, sr_ / (kLine - 8), sr_ * 0.3);
      double w = kTwoPi * hz / sr_;
      std::complex<double> z1 = std::polar(1.0, -w);
      double d = std::clamp(damp_ * 0.92 * (0.25 + 0.75 * std::min(1.0, 300.0 / hz)), 0.0, 0.95);
      double a = -stiff_ * 0.8;
      std::complex<double> hlp = (1.0 - d) / (1.0 - d * z1);
      std::complex<double> hap = (a + z1) / (1.0 + a * z1);
      double ph = std::arg(hlp) + kAp * std::arg(hap);
      double len = sr_ / hz + ph / w;  // ph < 0: filters add delay
      g.len = std::clamp(len, 2.0, static_cast<double>(kLine - 4));
      double t60 = v.held || exciter_ >= ExBow ? decay_ : std::min(static_cast<double>(decay_), std::max(0.01, static_cast<double>(release_)));
      double gain = std::pow(10.0, -3.0 / (t60 * hz)) / std::max(1e-3, std::abs(hlp));
      g.gain = static_cast<float>(std::min(gain, 0.99995));
      g.d = static_cast<float>(d);
    }
    apA_ = static_cast<float>(-stiff_ * 0.8);
  }

  void strike(Voice& v) {
    // Fill the excitation burst for one period of string 1.
    int N = std::clamp(static_cast<int>(v.s[0].len), 8, kLine - 1);
    float lpC = 0.05f + 0.93f * excHard_;  // brighter for harder exciters
    float lp = 0;
    int len = N;
    float amp = 0.9f * (0.3f + 0.7f * v.vel);
    switch (exciter_) {
      case ExPick: {
        len = std::max(8, N / 2);
        lpC = std::min(0.99f, lpC + 0.25f);
        for (int i = 0; i < len; ++i) {
          lp += (v.rng.next() - lp) * lpC;
          v.exc[i] = lp * amp;
        }
        v.exc[0] += amp * 0.6f;
        v.exc[1] -= amp * 0.5f;
        break;
      }
      case ExHammer: {
        double width = (0.4 + 5.0 * (1.0 - excHard_)) * 1e-3 * sr_;
        len = std::clamp(static_cast<int>(width), 4, kLine - 1);
        for (int i = 0; i < len; ++i) v.exc[i] = amp * 1.6f * static_cast<float>(std::sin(kPi * i / len)) + v.rng.next() * excNoise_ * amp * 0.5f;
        break;
      }
      case ExBow:
      case ExBreath: len = 0; break;
      default: {
        for (int i = 0; i < len; ++i) {
          lp += (v.rng.next() - lp) * lpC;
          v.exc[i] = lp * amp * 1.4f;
        }
        break;
      }
    }
    if (len > 0) {
      if (excNoise_ > 0 && exciter_ != ExHammer)
        for (int i = 0; i < len; ++i) v.exc[i] += v.rng.next() * excNoise_ * amp * 0.6f;
      // Striking position: comb (a node at 1/pos of the string).
      int d = std::clamp(static_cast<int>(excPos_ * N), 1, len > 1 ? len - 1 : 1);
      for (int i = len - 1; i >= d; --i) v.exc[i] -= v.exc[i - d];
      // Remove DC so the loop does not drift.
      double m = 0;
      for (int i = 0; i < len; ++i) m += v.exc[i];
      float dc = static_cast<float>(m / len);
      for (int i = 0; i < len; ++i) v.exc[i] -= dc;
    }
    v.excLen = len;
    v.excPos = 0;
  }

  Voice& allocVoice() {
    Voice* pick = nullptr;
    for (int i = 0; i < maxVoices_; ++i)
      if (!voices_[i].on) {
        pick = &voices_[i];
        break;
      }
    if (!pick)
      for (int i = 0; i < maxVoices_; ++i)
        if (!pick || voices_[i].age < pick->age) pick = &voices_[i];
    return *pick;
  }

  void startVoice(uint32_t id, double pitch, float vel, int delay) {
    Voice& v = allocVoice();
    bool wasOn = v.on;
    v.on = v.held = true;
    v.id = id;
    v.pitch = pitch;
    v.vel = vel;
    v.age = ++clock_;
    v.rng.seed(static_cast<uint32_t>(clock_ * 2654435761u + 11u));
    v.delay = delay;
    v.hcEnv = 1;
    v.silent = 0;
    if (!wasOn) {
      v.s[0].clear();
      v.s[1].clear();
      v.hicut.reset();
      v.amp.level = 0;
    }
    double pan = std::clamp(spread_ * (pitch - 60.0) / 24.0, -1.0, 1.0), l, r;
    panGains(pan, l, r);
    v.panL = static_cast<float>(l);
    v.panR = static_cast<float>(r);
    retune(v, 0);
    v.excLen = 0;
    v.excPos = 0;
    if (delay <= 0) {
      v.amp.gate(true);
      strike(v);
    }
  }

  void onNoteOn(uint32_t id, double pitch, float vel) override {
    if (strum_ <= 0) {
      startVoice(id, pitch, vel, 0);
      return;
    }
    int semis[kStrumMax];
    int n = strumChord(strum_, semis);
    bool up = strumDir_ == 1 || (strumDir_ == 2 && alt_);
    alt_ = !alt_;
    for (int i = 0; i < n; ++i) {
      int k = up ? n - 1 - i : i;
      startVoice(id, pitch + semis[k], vel * std::pow(0.94f, static_cast<float>(i)), static_cast<int>(i * strumTime_ * sr_));
    }
  }
  void release(Voice& v) {
    v.held = false;
    if (v.delay > 0) {  // strummed string not yet struck
      v.on = false;
      return;
    }
    v.amp.gate(false);
    retune(v, 0);
  }
  void onNoteOff(uint32_t id) override {
    for (int i = 0; i < kVoices; ++i)
      if (voices_[i].on && voices_[i].held && voices_[i].id == id) release(voices_[i]);
  }
  void onAllOff() override {
    for (int i = 0; i < kVoices; ++i)
      if (voices_[i].on && voices_[i].held) release(voices_[i]);
  }
  void onKill() override {
    for (int i = 0; i < kVoices; ++i) voices_[i].on = false;
    fx_.reset();
    tail_ = 0;
  }

  void controlTick() {
    vibPh_ += vibRate_ * kCtl / sr_;
    if (vibPh_ >= 1) vibPh_ -= std::floor(vibPh_);
    double semis = vibrato_ * 0.5 * std::sin(kTwoPi * vibPh_);
    float hcDec = static_cast<float>(std::exp(-kCtl / (sr_ * std::max(0.005f, hiCutDec_))));
    for (int i = 0; i < kVoices; ++i) {
      Voice& v = voices_[i];
      if (!v.on || v.delay > 0) continue;
      retune(v, semis);
      v.hcEnv *= hcDec;
      double hz = std::clamp(hiCut_ * std::pow(2.0, hiCutEnv_ * v.hcEnv * 4.0), 40.0, sr_ * 0.45);
      v.hicut.set(sr_, hz, 0.15);
    }
  }

  void renderVoice(Voice& v, float* bl, float* br, int n) {
    const float sat = 1.0f + sat_ * 3.0f;
    const float pk = 0.04f + 0.46f * pickup_;
    for (int j = 0; j < n; ++j) {
      if (v.delay > 0) {
        if (--v.delay == 0) {
          v.amp.gate(true);
          retune(v, 0);
          strike(v);
          v.hicut.set(sr_, std::clamp(hiCut_ * std::pow(2.0, hiCutEnv_ * 4.0), 40.0, sr_ * 0.45), 0.15);
        }
        continue;
      }
      v.silent += 1;
      float env = static_cast<float>(v.amp.next());
      float in = 0;
      if (v.excPos < v.excLen) in = v.exc[v.excPos++];
      float sum = 0;
      for (int k = 0; k < 2; ++k) {
        if (k == 1 && level2_ <= 0) break;
        Wg& g = v.s[k];
        float y = g.read(g.len);
        float e = k == 0 ? in : in * 0.8f;
        if (exciter_ == ExBow) {
          float vb = bowForce_ * 0.5f * env * (0.4f + 0.6f * v.vel);
          float dv = vb - y;
          float slope = 5.0f - 4.0f * bowForce_;
          float f = std::pow(std::fabs(dv * slope) + 0.75f, -4.0f);
          e = dv * std::min(1.0f, f) * 0.6f + v.rng.next() * excNoise_ * 0.02f * env;
        } else if (exciter_ == ExBreath) {
          v.lpBreath += (v.rng.next() - v.lpBreath) * (0.05f + 0.6f * excHard_);
          e = v.lpBreath * env * 0.08f * (0.3f + 0.7f * v.vel) * (0.5f + bowForce_);
        }
        // Loop: damping lowpass → dispersion allpasses → gain → saturation.
        g.lp = (1 - g.d) * y + g.d * g.lp;
        float x = g.lp;
        for (int a = 0; a < kAp; ++a) {
          float o = apA_ * x + g.apx[a] - apA_ * g.apy[a];
          g.apx[a] = x;
          g.apy[a] = o;
          x = o;
        }
        x *= g.gain;
        if (sat_ > 0) x = std::tanh(x * sat) / sat;
        if (std::fabs(x) < 1e-15f) x = 0;
        g.buf[g.w] = x + e;
        g.w = (g.w + 1) & (kLine - 1);
        float out = y - 0.8f * g.read(g.len * pk);
        sum += k == 0 ? out : out * level2_;
      }
      float o = static_cast<float>(v.hicut.process(sum)) * (exciter_ >= ExBow ? 1.0f : env);
      bl[j] += o * v.panL;
      br[j] += o * v.panR;
      float a = std::fabs(o);
      v.quiet = a > v.quiet ? a : v.quiet * 0.9995f;
      // Voice ends when released/inaudible, on its own clock (J-30: not at a block end).
      if (v.silent > v.s[0].len * 2 + sr_ * 0.03 &&
          (!v.amp.active() || (!v.held && v.quiet < 1e-5f) || (v.held && exciter_ < ExBow && v.quiet < 2e-6f))) {
        v.on = false;
        break;
      }
    }
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
      bool any = false;
      for (int i = 0; i < kVoices; ++i)
        if (voices_[i].on) {
          renderVoice(voices_[i], bl, br, len);
          any = true;
        }
      tail_ = any ? static_cast<int>(sr_ * 0.5) : std::max(0, tail_ - len);
      for (int j = 0; j < len; ++j) {
        float x[2] = {bl[j], br[j]};
        for (int c = 0; c < 2; ++c) {
          float y = x[c];
          if (bodyMix_ > 0) {
            float b = 0;
            for (int k = 0; k < kBodyModes; ++k) b += static_cast<float>(bodyF_[c][k].process(y)) * bodyGain_[k];
            y = y * (1 - bodyMix_ * 0.7f) + b * bodyMix_ * 1.6f;
          }
          y = static_cast<float>(loCut_[c].process(y));
          if (tone_ != 0) y = static_cast<float>(toneF_[c].process(y));
          if (drive_ > 0) {
            float k = 1.0f + drive_ * 24.0f;
            y = std::tanh(y * k) / std::tanh(k) * 0.8f;
          }
          if (cab_) y = static_cast<float>(cabPk_[c].process(cabHp_[c].process(cabLp_[c].process(y))));
          x[c] = y * vol_;
        }
        bl[j] = x[0];
        br[j] = x[1];
      }
      fx_.process(bl, br, len);
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
  double sr_ = 0, vibPh_ = 0;
  int exciter_ = 0, strum_ = 0, strumDir_ = 0, maxVoices_ = 16, ctr_ = 0, body_ = -1, tail_ = 0;
  float excPos_ = 0.18f, excHard_ = 0.6f, excNoise_ = 0.1f, bowForce_ = 0.5f, decay_ = 4, damp_ = 0.35f, stiff_ = 0.1f;
  float pickup_ = 0.25f, sat_ = 0.1f, level2_ = 0.5f, detune_ = 4, hiCut_ = 9000, hiCutEnv_ = 0, hiCutDec_ = 0.4f;
  float release_ = 0.3f, strumTime_ = 0.025f, drive_ = 0, vibrato_ = 0, vibRate_ = 5, spread_ = 0.3f, vol_ = 0.8f;
  float bodyMix_ = 0, bodySize_ = -1, bodyGain_[kBodyModes] = {}, loCutHz_ = -1, tone_ = -99;
  float apA_ = 0;
  double ratio2_ = 1;
  bool cab_ = false, alt_ = false;
  Biquad bodyF_[2][kBodyModes], loCut_[2], toneF_[2], cabLp_[2], cabHp_[2], cabPk_[2];
  uint64_t clock_ = 0;
  ModelFx fx_;
};

}  // namespace

std::unique_ptr<Generator> makeString() { return std::make_unique<StringGen>(); }

}  // namespace gl::packs::models
