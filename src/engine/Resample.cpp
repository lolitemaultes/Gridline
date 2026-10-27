// Gridline — sample rate conversion. See Resample.h.
#include "Resample.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include "pitch/Kernel.h"

namespace gl {

namespace {

constexpr double kPi = 3.14159265358979323846;

double blackman(double u) {  // u in [-1, 1]
  const double x = (u + 1.0) * 0.5;
  return 0.42 - 0.5 * std::cos(2 * kPi * x) + 0.08 * std::cos(4 * kPi * x);
}

double sinc(double x) { return std::fabs(x) < 1e-12 ? 1.0 : std::sin(kPi * x) / (kPi * x); }

}  // namespace

// J-140: one kernel family for every conversion (engine/pitch/Kernel.h):
// up to 16 half taps the realtime Kaiser kernel, above it the offline one
// (48 zero crossings, beta 10: about 100 dB of image / alias rejection; the
// Blackman kernel this replaced reached about 75 dB).
std::vector<float> resampleChannel(const float* in, size_t frames, double fromRate, double toRate, int halfTaps) {
  return pitch::convert(in, frames, fromRate, toRate, halfTaps <= 16 ? pitch::Quality::Realtime : pitch::Quality::Offline);
}

AudioBuffer resampleBuffer(const AudioBuffer& in, double fromRate, double toRate, int halfTaps) {
  AudioBuffer out;
  if (in.channels == 0) return out;
  std::vector<std::vector<float>> ch;
  for (size_t c = 0; c < in.channels; ++c) ch.push_back(resampleChannel(in.plane(c), in.frames, fromRate, toRate, halfTaps));
  out.resize(in.channels, ch[0].size());
  for (size_t c = 0; c < in.channels; ++c) std::copy(ch[c].begin(), ch[c].end(), out.plane(c));
  return out;
}

const SincKernel& sincKernel(int points) {
  static constexpr int kSizes[] = {16, 24, 32, 64, 128, 256, 512};
  static std::array<SincKernel, 7> kernels;
  static std::array<std::once_flag, 7> once;
  int slot = 0;
  while (slot < 6 && kSizes[slot] < points) ++slot;
  std::call_once(once[static_cast<size_t>(slot)], [slot]() {
    SincKernel& k = kernels[static_cast<size_t>(slot)];
    k.points = kSizes[slot];
    k.phases = k.points >= 256 ? 256 : 512;
    k.table.assign(static_cast<size_t>(k.phases + 1) * static_cast<size_t>(k.points), 0.0f);
    const double half = k.points / 2.0;
    for (int r = 0; r <= k.phases; ++r) {
      const double fr = static_cast<double>(r) / k.phases;
      double sum = 0;
      std::vector<double> w(static_cast<size_t>(k.points));
      for (int t = 0; t < k.points; ++t) {
        // Tap t reads sample floor(pos) - points/2 + 1 + t; distance from pos:
        const double x = static_cast<double>(t - k.points / 2 + 1) - fr;
        w[static_cast<size_t>(t)] = sinc(x) * blackman(std::clamp(x / half, -1.0, 1.0));
        sum += w[static_cast<size_t>(t)];
      }
      for (int t = 0; t < k.points; ++t)
        k.table[static_cast<size_t>(r) * static_cast<size_t>(k.points) + static_cast<size_t>(t)] =
            static_cast<float>(w[static_cast<size_t>(t)] / sum);
    }
  });
  return kernels[static_cast<size_t>(slot)];
}

}  // namespace gl
