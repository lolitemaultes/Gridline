// Gridline: pitch-synchronous overlap-add for voices and speech (J-140).
//
// TD-PSOLA (Moulines & Charpentier, 1990) in the stretch engines' time-only
// domain: the input's pitch period is tracked (YIN on a 4x decimated mix,
// de Cheveigne & Kawahara 2002), pitch marks one period apart are aligned to
// the waveform (cross-correlation with the previous period), and the output
// is built from two-period Hann grains centred on the mark nearest to where
// the time map points, one period apart. Grain contents are read at the
// formant factor (band-limited): the caller resamples the result by the pitch
// ratio, so a formant factor of 1 / pitch gives a pitch shift whose spectral
// envelope (formants) stays where it was. Unvoiced input (breaths,
// fricatives) is overlap-added along the time map in 10 ms grains, so noise
// is neither repeated nor tuned.
//
// Same streaming interface and zero-latency semantics as TdStretch.
// configure() allocates; everything else is realtime safe.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace gl::pitch {

class Psola {
 public:
  void configure(int channels, double sampleRate);
  void reset();
  // Formant factor: grain contents are read this much faster (1 = unchanged).
  void setFormantFactor(double f) { formant_ = std::clamp(f, 0.125, 8.0); }
  int outputSeekLength(float /*rate*/) const { return seek_; }
  int inputLatency() const { return seek_; }
  int outputLatency() const { return 0; }
  int blockSamples() const { return 2 * tMax_; }

  template <class In>
  void outputSeek(const In& in, int length) {
    reset();
    push(in, 0, length);
    ana_ = 0;
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

 private:
  static constexpr int kStep = 256;  // output frames per render step
  struct Zeros {
    struct Ch {
      float operator[](int) const { return 0.0f; }
    };
    Ch operator[](int) const { return {}; }
  };
  struct Mark {
    int64_t pos = 0;
    double period = 0;
    bool voiced = false;
    float tonal = 0;  // 1 = a pure tone (no formants to keep)
    double frac = 0;  // sub-sample offset of the mark
  };
  template <class In>
  void push(const In& in, int from, int count) {
    if (count <= 0) return;
    const int skip = count > cap_ ? count - cap_ : 0;
    for (int c = 0; c < channels_; ++c) {
      float* rb = ring_[static_cast<size_t>(c)].data();
      const auto ch = in[c];
      for (int i = skip; i < count; ++i) rb[static_cast<size_t>((written_ + i) & mask_)] = ch[from + i];
    }
    written_ += count;
    analyse();
  }
  void analyse();
  bool yinAt(int64_t centre, double& period) const;
  float monoAt(int64_t i) const;
  float decAt(int64_t i) const;
  void addMarks();
  const Mark* markNear(double pos) const;
  void addGrain(double centreOut, double centreIn, double halfOut, double f, bool exact);
  float tonalAt(int64_t pos, double period) const;
  void render(int m, double rate);

  int channels_ = 1;
  double sr_ = 44100;
  int tMin_ = 44, tMax_ = 1103;  // period range (frames): 1000 Hz … 40 Hz
  int unv_ = 441;                // unvoiced grain half length (10 ms)
  int yinW_ = 1024;              // YIN window (full-rate frames)
  int frameHop_ = 256;           // pitch analysis hop
  int seek_ = 0, cap_ = 0;
  int64_t mask_ = 0;
  std::vector<float> ring_[2];
  std::vector<float> dec_;       // 4x decimated mono, ring
  int64_t decMask_ = 0, decDone_ = 0;
  std::vector<double> yinD_;     // YIN scratch
  // Pitch analysis frames (every frameHop_ input frames): period, 0 = unvoiced.
  std::vector<double> framePeriod_;
  int64_t frameMask_ = 0, framesDone_ = 0;
  // Marks (ring).
  std::vector<Mark> marks_;
  int64_t markMask_ = 0, marksDone_ = 0;
  // Overlap-add accumulators (signal, window weight), rings.
  std::vector<float> acc_[2];
  std::vector<float> wacc_;
  int64_t accMask_ = 0;
  std::vector<float> outBuf_[2];
  int64_t written_ = 0;
  double ana_ = 0;
  int64_t outPos_ = 0;
  double nextOut_ = 0;  // output position of the next grain centre
  double formant_ = 1;
};

}  // namespace gl::pitch
