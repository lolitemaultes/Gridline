// Gridline — Drum Model: 16 pads, each a modal drum. A mallet pulse (width
// from hardness) plus a click excites up to 12 resonant modes (complex
// one-pole rotators, so pitch bends glide smoothly); mode ratios come from the
// shape (membrane, tom, bar, plate, bell, tabla), higher modes decay faster
// with "material". A band-passed noise layer with its own decay, optionally
// following the drum's level (snare wires), adds the rattle. Master: 3-band
// EQ, lo-fi and a peak limiter.
#include <array>
#include <cmath>
#include "models_rt.h"

namespace gl::packs::models {

namespace {

using namespace dsp;
constexpr int kCtl = 16;
constexpr int kVoices = 24;

class DrumGen final : public Generator {
 public:
  bool active() const override {
    for (const auto& v : voices_)
      if (v.on) return true;
    return false;
  }

 protected:
  struct Voice {
    bool on = false;
    int pad = 0, group = 0;
    uint64_t age = 0;
    int64_t n = 0;  // samples since strike
    float re[kDrumModes] = {}, im[kDrumModes] = {};
    float c[kDrumModes] = {}, s[kDrumModes] = {}, r[kDrumModes] = {}, g[kDrumModes] = {};
    int modes = 0;
    double f0 = 100;
    int pulseLen = 1;
    float amp = 1, gl = 1, gr = 1, click = 0, noise = 0, noiseEnv = 0, noiseDec = 0.999f, wires = 0;
    float follow = 0, peak = 0, fade = 1, fadeStep = 0, drive = 0;
    float level = 1, velGain = 1;  // pad level and velocity gain (amp = level x velGain)
    Svf nf;
    Noise rng;
  };

  void configure(const PluginState& s, const ChannelAux&, const ProcessContext& ctx) override {
    bool srChanged = sr_ != ctx.sampleRate;
    state_ = &s;
    sr_ = ctx.sampleRate;
    float lo = prmf(s, DmLow, 0), mid = prmf(s, DmMid, 0), hi = prmf(s, DmHigh, 0);
    if (srChanged || lo != eq_[0] || mid != eq_[1] || hi != eq_[2]) {
      eq_[0] = lo, eq_[1] = mid, eq_[2] = hi;
      for (int c = 0; c < 2; ++c) {
        eqF_[c][0].set(Biquad::LowShelf, sr_, 110, 0.7, lo);
        eqF_[c][1].set(Biquad::Peak, sr_, 1200, 0.8, mid);
        eqF_[c][2].set(Biquad::HighShelf, sr_, 6500, 0.7, hi);
      }
    }
    limiter_ = prmf(s, DmLimiter, 1) >= 0.5f;
    lofi_ = prmf(s, DmLofi, 0);
    vol_ = prmf(s, DmVolume, 0.8f);
  }

  void setModes(Voice& v, double semis) {
    const PluginState& s = *state_;
    int P = v.pad;
    int shape = std::clamp(prmi(s, drumPad(P, DpShape), 0), 0, DsCount - 1);
    const float* ratios = drumRatios(shape);
    float spread = prmf(s, drumPad(P, DpSpread), 1);
    float decay = prmf(s, drumPad(P, DpDecay), 0.4f);
    float material = prmf(s, drumPad(P, DpMaterial), 0.4f);
    double bendMul = std::pow(2.0, semis / 12.0);
    v.modes = 0;
    for (int k = 0; k < kDrumModes; ++k) {
      double ratio = 1.0 + (ratios[k] - 1.0) * spread;
      double hz = v.f0 * ratio * bendMul;
      if (hz >= sr_ * 0.45 || ratio <= 0) break;
      double w = kTwoPi * hz / sr_;
      double t60 = decay / (1.0 + material * (ratio - 1.0) * 2.0);
      v.c[k] = static_cast<float>(std::cos(w));
      v.s[k] = static_cast<float>(std::sin(w));
      v.r[k] = static_cast<float>(std::pow(10.0, -3.0 / (std::max(0.005, t60) * sr_)));
      v.modes = k + 1;
    }
  }

  void onNoteOn(uint32_t, double pitch, float vel) override {
    const PluginState& s = *state_;
    int pad = static_cast<int>(std::lround(pitch)) - prmi(s, DmBaseNote, 60);
    double keyShift = 0;  // semitones (pitched key mode)
    if (prmi(s, DmKeyMode, 0) == 1) keyShift = pad, pad = 0;
    if (pad < 0 || pad >= kDrumPads) return;
    if (prmf(s, drumPad(pad, DpMute), 0) >= 0.5f) return;
    int group = prmi(s, drumPad(pad, DpChoke), 0);
    if (group > 0)
      for (auto& v : voices_)
        if (v.on && v.group == group) v.fadeStep = static_cast<float>(1.0 / std::max(1.0, 0.006 * sr_));
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
    v.on = true;
    v.pad = pad;
    v.group = group;
    v.age = ++clock_;
    v.rng.seed(static_cast<uint32_t>(clock_ * 2654435761u + 7u));
    float sens = prmf(s, DmVelocity, 0.7f);
    float velGain = 1 - sens + sens * vel;
    v.f0 = prmf(s, drumPad(pad, DpFreq), 100) * std::pow(2.0, (prmf(s, drumPad(pad, DpTune), 0) + keyShift) / 12.0);
    float hard = std::clamp(prmf(s, drumPad(pad, DpMallet), 0.5f) + (vel - 0.7f) * 0.3f * sens, 0.0f, 1.0f);
    v.pulseLen = std::max(1, static_cast<int>((0.15 + 6.0 * (1.0 - hard) * (1.0 - hard)) * 1e-3 * sr_));
    setModes(v, prmf(s, drumPad(pad, DpBend), 0));
    const float* gains = drumGains(std::clamp(prmi(s, drumPad(pad, DpShape), 0), 0, DsCount - 1));
    float pos = prmf(s, drumPad(pad, DpPosition), 0.3f);
    float norm = 0;
    for (int k = 0; k < v.modes; ++k) {
      v.g[k] = gains[k] / (1.0f + (1.0f - pos) * static_cast<float>(k) * 0.6f);
      norm += v.g[k];
    }
    for (int k = 0; k < v.modes; ++k) v.g[k] *= 1.6f / std::max(1.0f, norm * 0.6f);
    v.level = prmf(s, drumPad(pad, DpLevel), 0.8f);
    v.velGain = velGain;
    v.amp = v.level * velGain;
    double l, r;
    panGains(prmf(s, drumPad(pad, DpPan), 0), l, r);
    v.gl = static_cast<float>(l);
    v.gr = static_cast<float>(r);
    v.click = prmf(s, drumPad(pad, DpClick), 0.2f) * (0.5f + 0.5f * hard);
    v.noise = prmf(s, drumPad(pad, DpNoise), 0);
    v.noiseEnv = 1;
    v.noiseDec = static_cast<float>(std::pow(10.0, -3.0 / (std::max(0.005f, prmf(s, drumPad(pad, DpNoiseDecay), 0.2f)) * sr_)));
    v.wires = prmf(s, drumPad(pad, DpWires), 0);
    v.nf.set(sr_, prmf(s, drumPad(pad, DpNoiseTone), 4000), 0.35);
    v.drive = prmf(s, drumPad(pad, DpDrive), 0);
    v.fade = 1;
  }
  void onNoteOff(uint32_t) override {}
  void onAllOff() override {}
  void onKill() override {
    for (auto& v : voices_) v.on = false;
    limEnv_ = 1;
  }

  void controlTick() {
    const PluginState& s = *state_;
    for (auto& v : voices_) {
      if (!v.on) continue;
      float bend = prmf(s, drumPad(v.pad, DpBend), 0);
      if (bend != 0) {
        double t = static_cast<double>(v.n) / sr_;
        setModes(v, bend * std::exp(-t / std::max(0.003f, prmf(s, drumPad(v.pad, DpBendTime), 0.05f))));
      }
    }
  }

  void renderVoice(Voice& v, float* bl, float* br, int n) {
    const float pulseNorm = static_cast<float>(kPi / (2.0 * v.pulseLen));
    for (int j = 0; j < n; ++j) {
      float x = 0;
      if (v.n < v.pulseLen) x = pulseNorm * static_cast<float>(std::sin(kPi * (static_cast<double>(v.n) + 0.5) / v.pulseLen));
      int clickLen = static_cast<int>(sr_ * 0.0015);
      float clk = v.n < clickLen ? v.rng.next() * v.click * (1.0f - static_cast<float>(v.n) / clickLen) : 0.0f;
      float y = 0;
      for (int k = 0; k < v.modes; ++k) {
        float re = v.r[k] * (v.c[k] * v.re[k] - v.s[k] * v.im[k]) + x * v.g[k];
        float im = v.r[k] * (v.s[k] * v.re[k] + v.c[k] * v.im[k]);
        v.re[k] = re;
        v.im[k] = im;
        y += im;
      }
      y *= 3.0f;
      v.follow += (std::fabs(y) - v.follow) * 0.01f;
      float nz = 0;
      if (v.noise > 0) {
        double band = 0;
        v.nf.process(v.rng.next(), &band);
        float env = v.noiseEnv * (1 - v.wires) + std::min(1.0f, v.follow * 3.0f) * v.wires;
        nz = static_cast<float>(band) * env * v.noise * 1.5f;
        v.noiseEnv *= v.noiseDec;
      }
      float o;
      if (v.drive > 0) {
        // J-138: the drive saturates at the pad level, then velocity sets the
        // loudness (applied before the drive, it was flattened back to full level).
        float k = 1 + v.drive * 10;
        o = std::tanh((y + nz + clk) * v.level * k) / std::tanh(k) * (0.6f + 0.4f / (1 + v.drive)) * v.velGain;
      } else {
        o = (y + nz + clk) * v.amp;
      }
      o *= v.fade;
      v.fade = std::max(0.0f, v.fade - v.fadeStep);
      bl[j] += o * v.gl;
      br[j] += o * v.gr;
      float a = std::fabs(o);
      v.peak = a > v.peak ? a : v.peak * 0.9993f;
      ++v.n;
      // Ends on the voice's own clock, not at a block end (J-30).
      if (v.fade <= 0 || (v.n > v.pulseLen + static_cast<int64_t>(sr_ * 0.02) && v.peak < 2e-5f)) {
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
      for (auto& v : voices_)
        if (v.on) renderVoice(v, bl, br, len);
      for (int j = 0; j < len; ++j) {
        float x[2] = {bl[j] * vol_, br[j] * vol_};
        for (int c = 0; c < 2; ++c)
          for (int b = 0; b < 3; ++b)
            if (eq_[b] != 0) x[c] = static_cast<float>(eqF_[c][b].process(x[c]));
        if (lofi_ > 0) {
          int hold = 1 + static_cast<int>(lofi_ * 7);
          if (++holdCount_ >= hold) {
            holdCount_ = 0;
            float q = std::pow(2.0f, 15.0f - lofi_ * 11.0f);
            held_[0] = std::round(x[0] * q) / q;
            held_[1] = std::round(x[1] * q) / q;
          }
          x[0] = held_[0];
          x[1] = held_[1];
        }
        if (limiter_) {
          float pk = std::max(std::fabs(x[0]), std::fabs(x[1]));
          float want = pk > 0.95f ? 0.95f / pk : 1.0f;
          limEnv_ = want < limEnv_ ? want : limEnv_ + (1.0f - limEnv_) * limRel_;
          x[0] *= limEnv_;
          x[1] *= limEnv_;
        }
        L[pos + j] += x[0];
        R[pos + j] += x[1];
      }
      ctr_ -= len;
      pos += len;
    }
  }

 private:
  std::array<Voice, kVoices> voices_{};
  const PluginState* state_ = nullptr;
  double sr_ = 0;
  float eq_[3] = {99, 99, 99}, vol_ = 0.8f, lofi_ = 0, limEnv_ = 1, limRel_ = 0.0005f, held_[2] = {0, 0};
  bool limiter_ = true;
  int ctr_ = 0, holdCount_ = 0;
  Biquad eqF_[2][3];
  uint64_t clock_ = 0;
};

}  // namespace

std::unique_ptr<Generator> makeDrumKit() { return std::make_unique<DrumGen>(); }

}  // namespace gl::packs::models
