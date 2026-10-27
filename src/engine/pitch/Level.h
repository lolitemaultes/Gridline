// Gridline: loudness matching for formant-keeping pitch shifts (J-140).
//
// Keeping the spectral envelope while the harmonics move changes the energy
// (an octave up has half as many harmonics under the same envelope: about
// -7 dB on a voice; an octave down the opposite). Listeners expect a pitch
// shift to keep its loudness, so the shifted signal is scaled to follow the
// short-time level of the source at the same moment (30 ms windows, gain
// limited to ±12 dB, smoothed).
#pragma once
#include <cstdint>
#include <functional>
#include <vector>

namespace gl::pitch {

// Offline: y[c][0, len) follows the level of x[c][0, n); srcOf(q) is the
// source frame heard at output frame q (non-decreasing). Levels are Hann-
// weighted over 40 ms windows (a rectangular window would ripple with the
// phase of low notes, modulating the gain). Gains stay within ±12 dB and
// never raise a window's peak more than 3 dB over the source's peak there.
// Apply it to the final, band-limited result: content a later resampler
// removes must not count.
void matchLevel(float* const* y, int nch, int64_t len, const float* const* x, int64_t n, const std::function<double(int64_t)>& srcOf,
                double sampleRate);

// Realtime: the mean squares of the source spans the blocks came from and of
// the blocks themselves, each smoothed alike (two 50 ms one-pole stages),
// give the gain, ramped across each block. (Per-block measures would wobble
// with the phase of low notes inside a short block: the gain would ripple,
// an audible amplitude modulation.)
class LevelFollower {
 public:
  void setRate(double sampleRate) {
    const double sr = sampleRate > 0 ? sampleRate : 44100.0;
    tau_ = 0.05 * sr;
    attack_ = 1.0 / (0.001 * sr);  // the peak limit: one unit of gain per ms down
    release_ = 1.0 / (0.02 * sr);  // and per 20 ms back up
  }
  void reset() {
    gain_ = applied_ = 1.0;
    src_[0] = src_[1] = out_[0] = out_[1] = 0.0;
  }
  // Applies the gain to y[c][0, m) (in place). srcMs: mean square of the
  // source span, outMs: of the block before the gain (as it will be heard:
  // band-limited like the output). peakLimit: the source span's peak over
  // the block's peak; a boost stays within 3 dB over that (0: no limit).
  // J-145: the gain is continuous across blocks; the peak limit pulls it down
  // along a short ramp ahead of the frames that need it (a limiter), never
  // as a step at a block's start.
  void apply(float* const* y, int nch, int m, double srcMs, double outMs, double peakLimit = 0);

 private:
  static constexpr int kStep = 256;  // frames per look-ahead pass (a stack buffer: no allocation)
  double gain_ = 1.0, applied_ = 1.0, src_[2] = {0, 0}, out_[2] = {0, 0}, tau_ = 1323.0;
  double attack_ = 1.0 / 44.1, release_ = 1.0 / 882.0;
};

}  // namespace gl::pitch
