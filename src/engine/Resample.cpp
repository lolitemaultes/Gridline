// Gridline — sample rate conversion. See Resample.h.
#include "Resample.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>

namespace gl {

namespace {

constexpr double kPi = 3.14159265358979323846;

double blackman(double u) {  // u in [-1, 1]
  const double x = (u + 1.0) * 0.5;
  return 0.42 - 0.5 * std::cos(2 * kPi * x) + 0.08 * std::cos(4 * kPi * x);
}

double sinc(double x) { return std::fabs(x) < 1e-12 ? 1.0 : std::sin(kPi * x) / (kPi * x); }

}  // namespace

std::vector<float> resampleChannel(const float* in, size_t frames, double fromRate, double toRate, int halfTaps) {
  std::vector<float> out;
  if (frames == 0 || fromRate <= 0 || toRate <= 0) return out;
  const double ratio = toRate / fromRate;
  const size_t outN = static_cast<size_t>(std::llround(static_cast<double>(frames) * ratio));
  out.assign(outN, 0.0f);
  if (std::fabs(ratio - 1.0) < 1e-12) {
    std::copy(in, in + std::min(frames, outN), out.begin());
    return out;
  }
  halfTaps = std::clamp(halfTaps, 4, 256);
  // Cutoff (in input-rate cycles): the lower Nyquist, with a small guard band.
  const double cut = std::min(1.0, ratio) * (1.0 - 1.5 / halfTaps);
  const double width = std::ceil(halfTaps / std::min(1.0, ratio));  // kernel half-width, input samples
  constexpr int kRes = 512;                                         // table steps per input sample
  const size_t tn = static_cast<size_t>(width * kRes) + 2;
  std::vector<double> kern(tn);
  for (size_t i = 0; i < tn; ++i) {
    const double x = static_cast<double>(i) / kRes;
    kern[i] = x >= width ? 0.0 : cut * sinc(cut * x) * blackman(x / width);
  }
  const auto W = static_cast<int64_t>(width);
  const auto N = static_cast<int64_t>(frames);
  for (size_t j = 0; j < outN; ++j) {
    const double t = static_cast<double>(j) / ratio;
    const auto i0 = static_cast<int64_t>(std::floor(t));
    double acc = 0, wsum = 0;
    for (int64_t k = i0 - W + 1; k <= i0 + W; ++k) {
      const double x = std::fabs(t - static_cast<double>(k)) * kRes;
      const auto xi = static_cast<size_t>(x);
      if (xi + 1 >= tn) continue;
      const double fr = x - static_cast<double>(xi);
      const double w = kern[xi] + (kern[xi + 1] - kern[xi]) * fr;
      wsum += w;
      if (k >= 0 && k < N) acc += w * in[k];
    }
    // Normalised by the kernel's DC gain at this phase (all taps, also those
    // hanging over the ends), so constant signals pass at exactly unity.
    out[j] = static_cast<float>(wsum > 1e-9 ? acc / wsum : acc);
  }
  return out;
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
