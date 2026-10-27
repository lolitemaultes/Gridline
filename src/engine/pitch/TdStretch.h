// Gridline: time-domain time stretching, WSOLA (J-140).
//
// Waveform-similarity overlap-add (Verhelst & Roelands, 1993): the output is
// built from Hann grains of N frames placed every N / 2 output frames; each
// grain is taken from the input near the position the time map gives, shifted
// by up to ±tolerance frames to the offset whose waveform best continues the
// previous grain (normalised cross-correlation on a decimated mix, refined at
// full rate). Nothing is re-synthesised: transients and noise are copied, so
// there is no phasiness and no smearing, which suits drums, percussive loops
// and single voices. Pitch is not changed here (callers resample).
//
// The interface mirrors the spectral engine's streaming calls (outputSeek,
// process, flush), so the realtime voice and the offline renderers host
// either. Zero latency semantics: after outputSeek(len) output frame 0 is
// input frame 0, and the engine keeps exactly len input frames of lookahead.
// configure() allocates; everything else is realtime safe.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace gl::pitch {

class TdStretch {
 public:
  struct Preset {
    double grainSec = 0.046;      // grain length (output), 2 hops
    double toleranceSec = 0.0116;  // search range each side
  };
  static Preset beats() { return {0.035, 0.010}; }
  static Preset general() { return {0.046, 0.0116}; }
  static Preset light() { return {0.040, 0.010}; }

  void configure(int channels, double sampleRate, Preset p);
  void reset();
  int channels() const { return channels_; }
  // Input frames to give outputSeek() (the lookahead), any rate.
  int outputSeekLength(float /*rate*/) const { return seek_; }
  int inputLatency() const { return seek_; }
  int outputLatency() const { return 0; }
  int blockSamples() const { return n_; }

  template <class In>
  void outputSeek(const In& in, int length) {
    reset();
    push(in, 0, length);
    ana_ = 0;  // output frame 0 = input frame 0
  }
  template <class In, class Out>
  void process(const In& in, int inSamples, Out&& out, int outSamples) {
    if (outSamples <= 0) {
      push(in, 0, inSamples);
      ana_ += inSamples;
      return;
    }
    const double r = static_cast<double>(inSamples) / outSamples;
    // Sub-steps of at most one hop of output; input given in proportion
    // (cumulative rounding), so the lookahead stays exactly seek_.
    int inDone = 0;
    for (int o = 0; o < outSamples;) {
      const int m = std::min(outSamples - o, hop_);
      const int inTo = static_cast<int>(static_cast<int64_t>(inSamples) * (o + m) / outSamples);
      push(in, inDone, inTo - inDone);
      const double anaEnd = ana_ + (inTo - inDone);
      render(m, r, anaEnd);
      for (int c = 0; c < channels_; ++c)
        for (int j = 0; j < m; ++j) out[c][o + j] = outBuf_[static_cast<size_t>(c)][static_cast<size_t>(j)];
      inDone = inTo;
      ana_ = anaEnd;
      o += m;
    }
  }
  // The last frames: the input runs out (zeros), at `rate`.
  template <class Out>
  void flush(Out&& out, int outSamples, float rate) {
    Zeros z;
    process(z, static_cast<int>(std::lround(static_cast<double>(outSamples) * rate)), out, outSamples);
  }

 private:
  struct Zeros {
    struct Ch {
      float operator[](int) const { return 0.0f; }
    };
    Ch operator[](int) const { return {}; }
  };
  template <class In>
  void push(const In& in, int from, int count) {
    if (count <= 0) return;
    // Only the last cap frames can ever be read.
    const int skip = count > cap_ ? count - cap_ : 0;
    for (int c = 0; c < channels_; ++c) {
      float* rb = ring_[static_cast<size_t>(c)].data();
      const auto ch = in[c];
      for (int i = skip; i < count; ++i) rb[static_cast<size_t>((written_ + i) & mask_)] = ch[from + i];
    }
    written_ += count;
  }
  // Produces m output frames into outBuf_ (analysis position runs from ana_ to anaEnd).
  void render(int m, double rate, double anaEnd);
  void addGrain(int64_t at, int64_t src);
  int64_t search(int64_t nominal, int64_t ref) const;
  float mono(int64_t i) const;

  int channels_ = 1;
  int n_ = 2048, hop_ = 1024, tol_ = 512, seek_ = 0, cap_ = 0;
  int64_t mask_ = 0;
  std::vector<float> ring_[2];
  std::vector<float> acc_[2];      // overlap-add accumulator, ring of 2 N
  std::vector<float> outBuf_[2];   // one hop of finished output
  std::vector<float> win_;
  int64_t written_ = 0;   // input frames received since reset
  double ana_ = 0;        // input position of output frame outPos_
  int64_t outPos_ = 0;    // output frames produced
  int64_t nextGrain_ = 0; // output index of the next grain
  int64_t prevSrc_ = 0;   // source start of the previous grain
  bool first_ = true;
};

}  // namespace gl::pitch
