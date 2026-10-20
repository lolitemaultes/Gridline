// Gridline — offline loudness (RN-09). See Loudness.h.
#include "Loudness.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace gl {

namespace {

struct Biquad {
  double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
  double run(double x) {
    const double y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
};

// The two K-weighting stages for sample rate `sr` (bilinear designs of the
// BS.1770 analog prototype: +4 dB shelf above ~1.7 kHz, high pass at ~38 Hz).
void kWeighting(double sr, Biquad& shelf, Biquad& hp) {
  {
    const double f0 = 1681.974450955533, gainDb = 3.999843853973347, q = 0.7071752369554196;
    const double k = std::tan(M_PI * f0 / sr), vh = std::pow(10.0, gainDb / 20.0), vb = std::pow(vh, 0.4996667741545416);
    const double a0 = 1.0 + k / q + k * k;
    shelf.b0 = (vh + vb * k / q + k * k) / a0;
    shelf.b1 = 2.0 * (k * k - vh) / a0;
    shelf.b2 = (vh - vb * k / q + k * k) / a0;
    shelf.a1 = 2.0 * (k * k - 1.0) / a0;
    shelf.a2 = (1.0 - k / q + k * k) / a0;
  }
  {
    const double f0 = 38.13547087602444, q = 0.5003270373238773;
    const double k = std::tan(M_PI * f0 / sr), a0 = 1.0 + k / q + k * k;
    hp.b0 = 1.0, hp.b1 = -2.0, hp.b2 = 1.0;
    hp.a1 = 2.0 * (k * k - 1.0) / a0;
    hp.a2 = (1.0 - k / q + k * k) / a0;
  }
}

double toLufs(double energy) { return energy > 1e-20 ? -0.691 + 10.0 * std::log10(energy) : kSilentLufs; }

}  // namespace

double integratedLoudness(const AudioBuffer& buf, int sampleRate) {
  if (buf.channels == 0 || buf.frames == 0 || sampleRate <= 0) return kSilentLufs;
  const double sr = sampleRate;
  const size_t sub = std::max<size_t>(1, static_cast<size_t>(std::llround(sr * 0.1)));  // 100 ms
  const size_t nSub = buf.frames / sub;
  if (nSub < 4) return kSilentLufs;
  // Mean square of the K-weighted signal per 100 ms sub-block, summed over channels.
  std::vector<double> subE(nSub, 0.0);
  const size_t chans = std::min<size_t>(buf.channels, 2);
  for (size_t c = 0; c < chans; ++c) {
    Biquad shelf, hp;
    kWeighting(sr, shelf, hp);
    const float* x = buf.plane(c);
    for (size_t b = 0; b < nSub; ++b) {
      double acc = 0;
      for (size_t i = b * sub, e = i + sub; i < e; ++i) {
        const double y = hp.run(shelf.run(x[i]));
        acc += y * y;
      }
      subE[b] += acc / static_cast<double>(sub) * (buf.channels == 1 ? 2.0 : 1.0);
    }
  }
  // 400 ms blocks every 100 ms.
  std::vector<double> blocks;
  blocks.reserve(nSub - 3);
  for (size_t b = 0; b + 4 <= nSub; ++b) blocks.push_back((subE[b] + subE[b + 1] + subE[b + 2] + subE[b + 3]) * 0.25);
  double sum = 0;
  size_t n = 0;
  for (double e : blocks)
    if (toLufs(e) > -70.0) sum += e, ++n;
  if (n == 0) return kSilentLufs;
  const double relGate = toLufs(sum / static_cast<double>(n)) - 10.0;
  sum = 0, n = 0;
  for (double e : blocks)
    if (toLufs(e) > -70.0 && toLufs(e) > relGate) sum += e, ++n;
  return n ? toLufs(sum / static_cast<double>(n)) : kSilentLufs;
}

double samplePeakDb(const AudioBuffer& buf) {
  const float p = buf.peak();
  return p > 1e-9f ? 20.0 * std::log10(static_cast<double>(p)) : kSilentLufs;
}

LoudnessNormalization normalizeLoudness(AudioBuffer& buf, int sampleRate, double targetLufs, double ceilingDb) {
  LoudnessNormalization r;
  r.measured = integratedLoudness(buf, sampleRate);
  if (r.measured <= kSilentLufs + 1) return r;
  double gainDb = targetLufs - r.measured;
  const double peak = samplePeakDb(buf);
  if (peak + gainDb > ceilingDb) {
    gainDb = ceilingDb - peak;
    r.ceilingLimited = true;
  }
  const float g = static_cast<float>(std::pow(10.0, gainDb / 20.0));
  for (float& v : buf.data) v *= g;
  r.gainDb = gainDb;
  r.result = r.measured + gainDb;  // loudness scales exactly with gain
  return r;
}

}  // namespace gl
