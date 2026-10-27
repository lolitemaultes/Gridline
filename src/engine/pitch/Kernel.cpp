// Gridline: band-limited interpolation kernels (J-140). See Kernel.h.
#include "Kernel.h"
#include <algorithm>

namespace gl::pitch {

namespace {

constexpr double kPi = 3.14159265358979323846;

double besselI0(double x) {
  double s = 1, t = 1;
  const double q = x * x / 4;
  for (int k = 1; k < 64; ++k) {
    t *= q / (static_cast<double>(k) * k);
    s += t;
    if (t < 1e-12 * s) break;
  }
  return s;
}

SincTable build(int zc, int phases, double beta, double guardDown, double guardUp) {
  SincTable t;
  t.zeroCrossings = zc, t.phases = phases, t.guardDown = guardDown, t.guardUp = guardUp;
  const size_t len = static_cast<size_t>(zc * phases) + 2;
  t.table.resize(len);
  const double norm = besselI0(beta);
  for (size_t i = 0; i < len; ++i) {
    const double x = static_cast<double>(i) / phases;
    const double r = x / zc;
    const double win = r >= 1 ? 0.0 : besselI0(beta * std::sqrt(1 - r * r)) / norm;
    const double sinc = x == 0 ? 1.0 : std::sin(kPi * x) / (kPi * x);
    t.table[i] = static_cast<float>(sinc * win);
  }
  return t;
}

}  // namespace

const SincTable& table(Quality q) {
  // Kaiser beta sets the stopband (about 63, 86 and 99 dB); the guards put
  // the transition band (about ±0.24, ±0.11 and ±0.07 of Nyquist) where it
  // does no harm (see Kernel.h). Measured on swept sines (J-140 lab): images
  // and aliases in the audible band under -90 dB for Realtime and Offline.
  static const SincTable compact = build(8, 512, 6.0, 0.80, 0.89);
  static const SincTable rt = [] {
    SincTable t = build(24, 512, 8.6, 0.90, 0.94);
    t.fast = &compact;
    return t;
  }();
  static const SincTable off = build(48, 1024, 10.0, 0.95, 0.97);
  switch (q) {
    case Quality::Offline: return off;
    case Quality::Compact: return compact;
    default: return rt;
  }
}

// Built at start-up (main thread), so no audio thread ever builds one.
namespace {
const bool kTablesBuilt = (table(Quality::Compact), table(Quality::Realtime), table(Quality::Offline), true);
}

void resample(const float* x, int64_t n, double start, double rate, float* out, int64_t outLen, Quality q) {
  const SincTable& t = table(q);
  for (int64_t j = 0; j < outLen; ++j) out[j] = read(x, n, start + static_cast<double>(j) * rate, rate, t);
}

void resample2(const float* l, const float* r, int64_t n, double start, double rate, float* outL, float* outR, int64_t outLen, Quality q) {
  const SincTable& t = table(q);
  for (int64_t j = 0; j < outLen; ++j) read2(l, r, n, start + static_cast<double>(j) * rate, rate, t, outL[j], outR[j]);
}

std::vector<float> convert(const float* x, size_t n, double fromRate, double toRate, Quality q) {
  std::vector<float> out;
  if (n == 0 || !(fromRate > 0) || !(toRate > 0)) return out;
  const double ratio = toRate / fromRate;
  out.assign(static_cast<size_t>(std::llround(static_cast<double>(n) * ratio)), 0.0f);
  if (std::fabs(ratio - 1.0) < 1e-12) {
    std::copy(x, x + std::min(n, out.size()), out.begin());
    return out;
  }
  resample(x, static_cast<int64_t>(n), 0.0, 1.0 / ratio, out.data(), static_cast<int64_t>(out.size()), q);
  return out;
}

}  // namespace gl::pitch
