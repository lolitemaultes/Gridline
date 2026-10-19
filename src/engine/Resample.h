// Gridline — sample rate conversion (RN-03): windowed-sinc resampler for
// offline use (export sample rate conversion, oversampled renders, sample
// export), and the interpolation kernels the sampler uses for high-quality
// renders.
#pragma once
#include <cstddef>
#include <vector>
#include "../core/AudioBuffer.h"

namespace gl {

// Resampler quality: half the kernel length in input samples at unity ratio.
enum class SrcQuality : int { Fast = 8, Good = 16, Best = 64 };

// Converts `in` (any channel count) from `fromRate` to `toRate`. The output
// has round(frames × to / from) frames. Downsampling low-passes at the new
// Nyquist frequency (anti-aliasing). Blackman-windowed sinc, offline only.
AudioBuffer resampleBuffer(const AudioBuffer& in, double fromRate, double toRate, int halfTaps = 32);
std::vector<float> resampleChannel(const float* in, size_t frames, double fromRate, double toRate, int halfTaps = 32);

// Interpolation kernel for reading a sample at a fractional position:
// `points` taps (even: 16 … 512), `phases` fractional steps (+1 guard row).
// Row r holds the weights for fraction r / phases, tap k for sample
// floor(pos) - points/2 + 1 + k. Built once per size (thread-safe); call
// sincKernel() from a non-realtime thread first so the audio thread only reads.
struct SincKernel {
  int points = 0;
  int phases = 0;
  std::vector<float> table;  // (phases + 1) × points
  const float* row(int r) const { return table.data() + static_cast<size_t>(r) * static_cast<size_t>(points); }
};
const SincKernel& sincKernel(int points);
// Sinc taps of a sampler interpolation quality (RenderInterp 2 … 7 → 16 … 512).
inline int samplerSincPoints(int quality) {
  static constexpr int kPts[] = {0, 0, 16, 32, 64, 128, 256, 512};
  return quality >= 2 && quality <= 7 ? kPts[quality] : 0;
}

}  // namespace gl
