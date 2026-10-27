// Gridline: pitch-synchronous voice engine (J-140, rebuilt in J-144).
//
// TD-PSOLA (Moulines & Charpentier, 1990) for single voices and speech, with
// the pitch changed in the engine itself: grains are cut around each period's
// excitation and laid out one new period apart, so nothing is resampled
// afterwards (the top octave survives a pitch drop and the formants stay
// where they are by construction).
//
// Two streams, crossfaded by the decided voicing (VoiceTrack):
//   voiced  one grain per output period. Near unison a grain is the classic
//           two-period Hann window; as the pitch moves it becomes asymmetric
//           (a short rise before the excitation, a longer decay after), so a
//           grain carries one excitation and its ringing rather than the tail
//           of the previous period: the output's harmonics then follow the
//           spectral envelope instead of keeping a comb at the old pitch.
//           Pure tones are transposed instead (their grains are resampled).
//   noise   breaths, fricatives, silence and anything without a pitch:
//           overlap-add with a waveform-similarity search (WSOLA, Verhelst &
//           Roelands 1993) that continues the previous grain where it can, so
//           no constant offset turns into a comb filter; each crossfade is
//           compensated for the correlation of the two grains it joins (power
//           kept for noise, amplitude kept for tones and DC).
// Grains are added whole, ahead of the output (never cut by a block edge).
//
// Same streaming interface and aligned (zero-latency) semantics as TdStretch:
// output frame j plays the input the time map points to. configure()
// allocates; everything else is realtime safe.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include "VoiceTrack.h"

namespace gl::pitch {

class Psola {
 public:
  void configure(int channels, double sampleRate);
  void reset();
  // pitch: frequency ratio (the engine places grains at the new period);
  // formant: grain contents are read this much faster (1 keeps the formants).
  void setShift(double pitch, double formant) {
    pitch_ = std::clamp(pitch, 1.0 / 16.0, 16.0);
    formant_ = std::clamp(formant, 0.125, 8.0);
  }
  // Input the engine keeps ahead of the frame it plays: the analysis
  // lookahead plus the output horizon (grains are laid out that far ahead) at
  // the read rate.
  int outputSeekLength(float rate) const { return lead_ + static_cast<int>(std::ceil(horizon_ * std::max(0.0f, rate))); }
  int inputLatency() const { return lead_; }
  int outputLatency() const { return horizon_; }
  int blockSamples() const { return kStep; }

  template <class In>
  void outputSeek(const In& in, int length) {
    reset();
    push(in, 0, length);
    ana_ = 0;
    nPrev_ = 0;
  }
  template <class In, class Out>
  void process(const In& in, int inSamples, Out&& out, int outSamples) {
    if (outSamples <= 0) {
      push(in, 0, inSamples);
      ana_ += inSamples;
      return;
    }
    const double r = static_cast<double>(inSamples) / outSamples;
    int inDone = 0;
    for (int o = 0; o < outSamples;) {
      const int m = std::min(outSamples - o, kStep);
      const int inTo = static_cast<int>(static_cast<int64_t>(inSamples) * (o + m) / outSamples);
      push(in, inDone, inTo - inDone);
      render(m, r);
      for (int c = 0; c < channels_; ++c)
        for (int j = 0; j < m; ++j) out[c][o + j] = outBuf_[static_cast<size_t>(c)][static_cast<size_t>(j)];
      ana_ += inTo - inDone;
      inDone = inTo;
      o += m;
    }
  }
  template <class Out>
  void flush(Out&& out, int outSamples, float rate) {
    Zeros z;
    process(z, static_cast<int>(std::lround(static_cast<double>(outSamples) * rate)), out, outSamples);
  }

  // Read access for tests and tools.
  const VoiceTrack& track() const { return track_; }

 private:
  static constexpr int kStep = 256;  // output frames per render step
  struct Zeros {
    struct Ch {
      float operator[](int) const { return 0.0f; }
    };
    Ch operator[](int) const { return {}; }
  };
  template <class In>
  void push(const In& in, int from, int count) {
    if (count <= 0) return;
    for (int i = 0; i < count; ++i) {
      const auto k = static_cast<size_t>((written_ + i) & mask_);
      float mix = 0;
      for (int c = 0; c < channels_; ++c) {
        const float v = in[c][from + i];
        ring_[static_cast<size_t>(c)][k] = v;
        mix += v;
      }
      track_.push(channels_ == 2 ? 0.5f * mix : mix);
    }
    written_ += count;
  }
  float at(int c, int64_t i) const {
    return i < 0 || i >= written_ || i <= written_ - cap_ ? 0.0f : ring_[static_cast<size_t>(c)][static_cast<size_t>(i & mask_)];
  }
  void render(int m, double rate);
  void voicedGrain(double s, double a, double rate);
  void noiseRegion(double rate);
  double noiseSearch(double want, double cont) const;
  void addGrain(double s, double c, double lo, double ro, double f);

  VoiceTrack track_;
  int channels_ = 1;
  double sr_ = 44100;
  int tMax_ = 882;
  int lead_ = 0, horizon_ = 0;
  int64_t cap_ = 0, mask_ = 0;
  std::vector<float> ring_[2];
  int64_t written_ = 0;
  double ana_ = 0;      // input position heard at output frame outPos_
  int64_t outPos_ = 0;
  double pitch_ = 1, formant_ = 1;
  // Voiced stream: overlap-add accumulator (ring) and the next grain's output position.
  std::vector<float> accV_[2];
  int64_t accMask_ = 0;
  double vNext_ = 0;
  bool fresh_ = true;  // the next voiced grain starts a run
  int64_t lastIdx_ = -1;  // the last mark a grain came from
  std::vector<float> kern_;  // per-grain fractional-delay kernel
  // Noise stream: output written region by region (ring), grain centres.
  std::vector<float> accN_[2];
  int hn_ = 441, search_ = 352;
  int64_t nRegion_ = 0;  // output frame where the next noise region starts
  double nPrev_ = 0;     // input centre of the previous noise grain
  // Voicing crossfade.
  double gv_ = 0;
  int ramp_ = 352;
  std::vector<float> outBuf_[2];
};

}  // namespace gl::pitch
