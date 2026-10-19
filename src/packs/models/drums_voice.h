// Gridline — "models" pack, J-22 drums: the membrane drum voice shared by
// Membrane Pad and Membrane Kit. Audio thread only; no allocation.
//
// Signal flow of one strike (all controls 0..1, see drums.h MdParam). Control
// laws marked [ref] follow the reference instrument's own parameter mapping
// (worked out from its engine, behaviour only); the membrane itself is
// Gridline's modal model.
//   mallet:   every strike excites the membrane; mallet level sets the strike
//             amplitude on a 80 dB log scale [ref], mallet noise cross-fades the
//             impulse into a noise burst [ref], the noise pattern control seeds
//             that noise [ref], mallet decay sets the burst's decay rate [ref];
//             mallet retrigger adds a second, softer hit;
//   membrane: 24 modes of a circular membrane (complex one-pole rotators);
//             size + tension set the fundamental, pitch transposes ±1 octave
//             [ref], material stretches the mode ratios towards metal, shape
//             morphs membrane → bar ratios, strike phase weights the modes,
//             membrane decay sets the ring time, membrane cut-off damps upper
//             modes and dulls the excitation; a loose membrane (low tension)
//             starts sharp and settles (tension glide);
//   sine:     an extra sine (61 Hz … 1.5 kHz [ref]) with its own decay and an
//             8th-power level curve [ref];
//   tone:     LOW = resonant high-pass 4 Hz … 8 kHz, Q 0.5 … 8.5, off at 0 [ref];
//             MID = peak EQ 32 Hz … 16 kHz, ±16 dB, width 4.5 … 0.5 oct [ref],
//             whose frequency (±16 semitones) and gain (±24 dB) follow an
//             attack / decay envelope [ref]; the repeat selector re-fires that
//             envelope 1 … 3 more times (clap) [ref: 4 positions];
//             HIGH = high shelf 256 Hz … 4.4 kHz, ±18 dB [ref];
//   output:   lo-fi = amplitude quantisation to (1−v)^5·65536 + 4 steps [ref],
//             hold = gate of v³·3 s before a fast fade [ref], output level.
// Velocity routes add amount · velocity to their destination (presets with a
// zero mallet level and a velocity route to it rely on this).
#pragma once
#include <algorithm>
#include <cmath>
#include "../../engine/Dsp.h"
#include "drums.h"

namespace gl::packs::models::dr {

using namespace gl::dsp;

inline constexpr int kMembraneModes = 24;
inline constexpr float kMakeup = 0.25f;
// Circular-membrane mode ratios (Bessel zeros relative to the fundamental).
inline const float* membraneRatios() {
  static const float r[kMembraneModes] = {1.000f, 1.594f, 2.136f, 2.296f, 2.653f, 2.918f, 3.156f, 3.501f,
                                          3.600f, 3.652f, 4.060f, 4.154f, 4.230f, 4.601f, 4.832f, 4.903f,
                                          5.131f, 5.404f, 5.412f, 5.552f, 5.693f, 5.936f, 6.155f, 6.208f};
  return r;
}
// Free bar ratios (stiff, sparse) for the shape morph.
inline float barRatio(int k) { return static_cast<float>(std::pow((2.0 * k + 1.0) / 3.0, 2.0) * (k == 0 ? 1.0 : 1.0)); }

// Curves of the drum model (0..1 control → physical value). Shared with the editor readouts.
inline double mdFundamental(const float* p) {
  return 28.0 * std::pow(2.0, (1.0 - p[MdSize]) * 4.5) * std::pow(2.0, (p[MdTension] - 0.5) * 3.0) * std::pow(2.0, p[MdPitch] * 2.0 - 1.0);
}
inline double mdRingSeconds(const float* p) { return 0.02 * std::pow(2.0, p[MdMemDecay] * 8.0); }
// Mallet decay: per-sample decay rate (at 44.1 kHz) (1 − v)^4 · 0.05 + 0.0001.
inline double mdMalletRate(const float* p) {
  const double a = (1.0 - p[MdMalletDecay]) * (1.0 - p[MdMalletDecay]);
  return a * a * 0.05 + 0.0001;
}
inline double mdMalletSeconds(const float* p) { return 1.0 / (mdMalletRate(p) * 44100.0); }
inline double mdMalletGain(const float* p) { return std::pow(10.0, p[MdMalletAmp] * 4.0 - 4.0); }
inline double mdLowHz(const float* p) { return std::pow(2.0, p[MdLowFreq] * 11.0 + 2.0); }
inline double mdMidHz(const float* p) { return std::pow(2.0, p[MdMidFreq] * 9.0 + 5.0); }
inline double mdMidDb(const float* p) { return p[MdMidLevel] * 32.0 - 16.0; }
inline double mdHighHz(const float* p) { return std::pow(2.0, p[MdHighFreq] * 4.1 + 8.0); }
inline double mdHighDb(const float* p) { return p[MdHighLevel] * 36.0 - 18.0; }
inline double mdSineHz(const float* p) { return std::pow(2.0, p[MdSofFreq] * 4.65 + 5.93); }
inline double mdHoldSeconds(const float* p) { return p[MdHold] * p[MdHold] * p[MdHold] * 3.0 + 0.001; }

// Applies the four velocity routes to a copy of the model values.
inline void applyVelocity(const float* in, float* out, float velocity) {
  std::copy(in, in + MdCount, out);
  for (int r = 0; r < 4; ++r) {
    const int dest = velDestParam(static_cast<int>(std::lround(in[MdVelSrc1 + r])));
    if (dest < 0 || dest >= MdCount) continue;
    const float amt = std::clamp(in[MdVelAmt1 + r], 0.0f, 1.0f);
    out[dest] = std::clamp(out[dest] + amt * velocity, 0.0f, 1.0f);
  }
}

class MembraneVoice {
 public:
  bool on = false;
  int tag = 0;        // owner tag (pad / channel index)
  uint32_t id = 0;    // note id
  uint64_t age = 0;

  // Starts a strike. `p` = 38 model values (velocity already applied), `semis` = keyboard offset.
  void strike(const float* p, float velocity, double semis, double sr, uint32_t seed) {
    on = true;
    sr_ = sr;
    t_ = 0;
    const float* R = membraneRatios();
    const double f0 = mdFundamental(p) * std::pow(2.0, semis / 12.0);
    const double ring = mdRingSeconds(p);
    const double mat = p[MdMaterial], shape = p[MdShape], cut = p[MdMemCutoff];
    const double phase = 0.1 + 0.8 * p[MdPhase];
    modes_ = 0;
    double norm = 0;
    for (int k = 0; k < kMembraneModes; ++k) {
      double r = shape * R[k] + (1.0 - shape) * barRatio(k);
      r = std::pow(r, 0.7 + 1.3 * mat);
      const double hz = f0 * r;
      if (hz >= sr * 0.45) break;
      const double w = kTwoPi * hz / sr;
      const double t60 = ring / (1.0 + (r - 1.0) * (1.0 - cut) * 3.0);
      w_[k] = static_cast<float>(w);
      c_[k] = static_cast<float>(std::cos(w));
      s_[k] = static_cast<float>(std::sin(w));
      rad_[k] = static_cast<float>(std::exp(-6.907755 / std::max(1e-4, t60 * sr)));
      const double weight = (0.25 + 0.75 * std::fabs(std::sin(kPi * ((k % 6) + 1) * phase))) / (1.0 + 0.12 * k);
      gain_[k] = static_cast<float>(weight);
      norm += weight;
      re_[k] = im_[k] = 0;
      ++modes_;
    }
    for (int k = 0; k < modes_; ++k) gain_[k] = static_cast<float>(gain_[k] / std::max(1e-3, norm) * 2.5);
    // Tension glide: a slack membrane starts sharp and settles.
    glide_ = static_cast<float>(0.35 * (1.0 - p[MdTension]) * p[MdMalletAmp]);
    glideDec_ = static_cast<float>(std::exp(-1.0 / std::max(1.0, ring * 0.12 * sr)));
    glideBase_ = 1.0f;
    glideDec32_ = std::pow(glideDec_, 32.0f);
    // Mallet.
    (void)velocity;
    malletAmp_ = static_cast<float>(mdMalletGain(p));
    malletDec_ = static_cast<float>(std::pow(1.0 - std::min(0.9, mdMalletRate(p)), 44100.0 / sr));
    malletNoise_ = p[MdMalletNoise];
    noise_.seed(static_cast<uint32_t>(static_cast<double>(p[MdMalletNoiseRp]) * 4294967295.0) ^ seed);
    excLp_.setFreq(sr, 400.0 * std::pow(2.0, cut * 6.0));
    env_ = 1.0f;
    repeats_ = std::clamp(static_cast<int>(std::lround(p[MdRetrig])), 0, 3);
    repeatGap_ = static_cast<int>(0.011 * sr);
    retrig_ = p[MdMalletRetrig] >= 0.5f ? static_cast<int>(std::max(0.004, mdMalletSeconds(p) * 4.0) * sr) : -1;
    // Sine.
    sineInc_ = static_cast<float>(kTwoPi * mdSineHz(p) / sr);
    sinePh_ = 0;
    {
      const float l = p[MdSofLevel], l2 = l * l, l4 = l2 * l2;
      sineAmp_ = l4 * l4 * 0.5f;
      const double d = 0.25 * p[MdSofDecay] * p[MdSofDecay];
      sineDec_ = static_cast<float>(std::pow(1.0 - (d * d * 0.01 + 1e-5), 44100.0 / sr));
    }
    // Tone.
    lowOn_ = p[MdLowFreq] > 0.0f;
    low_.clear();
    if (lowOn_) low_.set(Biquad::HighPass, sr, mdLowHz(p), 0.5 + p[MdLowReso] * 8.0, 0);
    midHz_ = mdMidHz(p);
    midDb_ = mdMidDb(p);
    midBw_ = (1.0 - p[MdMidRes]) * 4.0 + 0.5;  // octaves
    midFm_ = p[MdMidFreqMod] * 32.0 - 16.0;    // semitones at full envelope
    midLm_ = p[MdMidLevelMod] * 48.0 - 24.0;   // dB at full envelope
    midOn_ = p[MdMidLevel] != 0.5f || p[MdMidLevelMod] != 0.5f;
    {
      const double a = (1.0 - p[MdMidAttack]) * (1.0 - p[MdMidAttack]);
      midAtt_ = static_cast<float>((a * a * 0.01 + 1e-5) * 44100.0 / sr);
    }
    midDec_ = static_cast<float>(std::exp(-1.0 / std::max(1.0, ring * 0.3 * sr)));
    midEnv_ = 0;
    midRise_ = true;
    mid_.clear();
    updateMid();
    high_.clear();
    highOn_ = std::fabs(mdHighDb(p)) > 0.01;
    high_.set(Biquad::HighShelf, sr, mdHighHz(p), 1.0 / ((1.0 - p[MdHighRes]) * 2.0 + 0.5), mdHighDb(p));
    // Output.
    const float lf = p[MdLofi];
    if (lf >= 0.01f) {
      const float q = 1.0f - lf;
      lofiSteps_ = q * q * q * q * q * 65536.0f + 4.0f;
    } else {
      lofiSteps_ = 0;
    }
    holdLeft_ = static_cast<int64_t>(mdHoldSeconds(p) * sr);
    fade_ = 1.0f;
    fadeStep_ = static_cast<float>(1.0 / (0.03 * sr));
    peak_ = 0;
    quietFor_ = 0;
  }

  // One sample (mono).
  float tick() {
    // Excitation.
    // Mallet noise cross-fades the impulse into a noise burst; both decay with
    // the mallet. The membrane always gets a small strike, the mallet level
    // (log scale) adds the rest and the dry click.
    const float n = noise_.next();
    const float imp = t_ == 0 ? 1.0f : 0.15f;
    const float burst = (1.0f - malletNoise_) * imp + malletNoise_ * n * 1.6f;
    float x = env_ * (0.1f + 0.9f * malletAmp_) * burst;
    const float dry = env_ * malletAmp_ * burst;
    env_ *= malletDec_;
    if (repeats_ > 0 && t_ > 0 && t_ % repeatGap_ == 0) {
      midEnv_ = 0.0f;
      midRise_ = true;
      --repeats_;
    }
    if (retrig_ > 0 && t_ == retrig_) env_ = 0.5f;
    x = static_cast<float>(excLp_.process(x));
    // Membrane modes; the tension glide re-tunes them every 32 samples.
    if (glide_ > 1e-4f && (t_ & 31) == 0) {
      const float g = glideBase_ + glide_;
      for (int k = 0; k < modes_; ++k) {
        const float w = std::min(w_[k] * g, 3.0f);
        c_[k] = std::cos(w);
        s_[k] = std::sin(w);
      }
      if (glide_ * glideDec32_ <= 1e-4f)  // settle on the exact tuning
        for (int k = 0; k < modes_; ++k) {
          c_[k] = std::cos(w_[k]);
          s_[k] = std::sin(w_[k]);
        }
    }
    glide_ *= glideDec_;
    float y = 0;
    for (int k = 0; k < modes_; ++k) {
      const float c = c_[k], s = s_[k];
      float re = rad_[k] * (re_[k] * c - im_[k] * s) + x;
      float im = rad_[k] * (re_[k] * s + im_[k] * c);
      re_[k] = re;
      im_[k] = im;
      y += im * gain_[k];
    }
    y += dry * 0.35f;  // direct mallet / noise
    // Sine.
    y += sineAmp_ * std::sin(sinePh_);
    sinePh_ += sineInc_;
    if (sinePh_ > kTwoPi) sinePh_ -= static_cast<float>(kTwoPi);
    sineAmp_ *= sineDec_;
    // Tone.
    double v = lowOn_ ? low_.process(y) : y;
    if (midRise_) {
      midEnv_ += midAtt_;
      if (midEnv_ >= 1.0f) {
        midEnv_ = 1.0f;
        midRise_ = false;
      }
    } else {
      midEnv_ *= midDec_;
    }
    if (midOn_ || midFm_ != 0.0) {
      if ((t_ & 15) == 0) updateMid();
      v = mid_.process(v);
    }
    if (highOn_) v = high_.process(v);
    float out = static_cast<float>(v);
    // Lo-fi.
    if (lofiSteps_ > 0) out = std::round(out * lofiSteps_) / lofiSteps_;
    // Make-up gain and a soft ceiling (resonant bands can stack up > +40 dB).
    out *= kMakeup;
    out = out / std::sqrt(1.0f + out * out);
    // Hold gate.
    if (holdLeft_ > 0) {
      --holdLeft_;
    } else {
      fade_ = std::max(0.0f, fade_ - fadeStep_);
    }
    out *= fade_;
    ++t_;
    // End of voice: faded or quiet for 50 ms after the mallet.
    float a = std::fabs(out);
    if (a > 1e-4f) quietFor_ = 0;
    else ++quietFor_;
    if (fade_ <= 0.0f || (t_ > 2000 && quietFor_ > static_cast<int64_t>(0.05 * sr_) && repeats_ == 0 && (retrig_ < 0 || t_ > retrig_)))
      on = false;
    if (!std::isfinite(out)) {
      on = false;
      return 0.0f;
    }
    return out;
  }

 private:
  void updateMid() {
    const double hz = midHz_ * std::pow(2.0, midFm_ * midEnv_ / 12.0);
    const double bw = std::pow(2.0, midBw_);  // bandwidth in octaves → Q
    mid_.set(Biquad::Peak, sr_, hz, std::sqrt(bw) / (bw - 1.0), midDb_ + midLm_ * midEnv_);
  }

  double sr_ = 44100;
  int64_t t_ = 0;
  int modes_ = 0;
  float w_[kMembraneModes] = {}, c_[kMembraneModes] = {}, s_[kMembraneModes] = {}, rad_[kMembraneModes] = {}, gain_[kMembraneModes] = {};
  float re_[kMembraneModes] = {}, im_[kMembraneModes] = {};
  float glide_ = 0, glideDec_ = 1, glideBase_ = 1, glideDec32_ = 1;
  float malletAmp_ = 0, malletDec_ = 0, malletNoise_ = 0, env_ = 0;
  int repeats_ = 0, repeatGap_ = 400, retrig_ = -1;
  Noise noise_;
  OnePole excLp_;
  float sineInc_ = 0, sinePh_ = 0, sineAmp_ = 0, sineDec_ = 0;
  Biquad low_, mid_, high_;
  bool lowOn_ = false, midOn_ = false, highOn_ = false;
  double midHz_ = 1000, midDb_ = 0, midBw_ = 1, midFm_ = 0, midLm_ = 0;
  float midEnv_ = 0, midAtt_ = 1, midDec_ = 0.999f;
  bool midRise_ = true;
  float lofiSteps_ = 0;
  int64_t holdLeft_ = 0, quietFor_ = 0;
  float fade_ = 1, fadeStep_ = 0.001f, peak_ = 0;
};

}  // namespace gl::packs::models::dr
