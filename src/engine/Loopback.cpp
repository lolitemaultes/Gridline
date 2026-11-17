// Gridline - K-123 (0.7.7): the loopback latency check. See Loopback.h.
#include "Loopback.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace gl::loopback {

std::vector<float> probeSignal(double sampleRate) {
  const double sr = sampleRate > 0 ? sampleRate : 48000.0;
  const auto n = static_cast<size_t>(std::max(256.0, std::round(sr * 0.043)));
  const size_t edge = std::max<size_t>(16, n / 16);
  std::vector<float> s(n);
  uint32_t x = 0x1234567u;
  for (size_t i = 0; i < n; ++i) {
    x = x * 1664525u + 1013904223u;  // a fixed noise: every measurement plays the same burst
    const float v = static_cast<float>(static_cast<int32_t>(x >> 8) - (1 << 23)) / static_cast<float>(1 << 23);
    double g = 1.0;
    if (i < edge) g = 0.5 - 0.5 * std::cos(M_PI * static_cast<double>(i) / static_cast<double>(edge));
    if (n - 1 - i < edge) g = std::min(g, 0.5 - 0.5 * std::cos(M_PI * static_cast<double>(n - 1 - i) / static_cast<double>(edge)));
    s[i] = static_cast<float>(0.35 * g) * v;
  }
  return s;
}

size_t listenFrames(double sampleRate) {
  const double sr = sampleRate > 0 ? sampleRate : 48000.0;
  return static_cast<size_t>(std::ceil(sr * 1.1));
}

Lag findLag(const std::vector<float>& sent, const std::vector<float>& got) {
  Lag r;
  const size_t n = sent.size();
  if (n == 0 || got.size() < n) return r;
  double es = 0;
  for (float v : sent) {
    es += static_cast<double>(v) * v;
  }
  for (float v : got) {
    r.level = std::max(r.level, std::fabs(v));
  }
  if (es <= 0 || r.level < 1e-5f) return r;
  // Energy of each window of `got` (running sum), then the correlation at every lag.
  const size_t lags = got.size() - n + 1;
  double eg = 0;
  for (size_t i = 0; i < n; ++i) {
    eg += static_cast<double>(got[i]) * got[i];
  }
  double best = 0;
  size_t bestLag = 0;
  bool bestNeg = false;
  for (size_t lag = 0; lag < lags; ++lag) {
    if (lag > 0) {
      const double out = got[lag - 1], in = got[lag + n - 1];
      eg = std::max(0.0, eg - out * out + in * in);
    }
    if (eg <= 1e-12) continue;
    double c = 0;
    const float* g = got.data() + lag;
    for (size_t i = 0; i < n; ++i) {
      c += static_cast<double>(sent[i]) * g[i];
    }
    const double m = std::fabs(c) / std::sqrt(es * eg);
    if (m > best) {
      best = m;
      bestLag = lag;
      bestNeg = c < 0;
    }
  }
  r.match = static_cast<float>(std::min(1.0, best));
  r.found = best >= 0.5;
  r.frames = static_cast<int>(bestLag);
  r.inverted = bestNeg;
  return r;
}

Summary summarise(const std::vector<Lag>& runs) {
  Summary s;
  s.total = static_cast<int>(runs.size());
  std::vector<int> v;
  for (const Lag& l : runs) {
    if (l.found) v.push_back(l.frames);
  }
  if (v.empty()) return s;
  std::sort(v.begin(), v.end());
  // The largest group within 2 frames of each other.
  size_t bestA = 0, bestN = 0;
  for (size_t a = 0; a < v.size(); ++a) {
    size_t b = a;
    while (b < v.size() && v[b] - v[a] <= 2) ++b;
    if (b - a > bestN) bestN = b - a, bestA = a;
  }
  s.agree = static_cast<int>(bestN);
  s.ok = bestN >= 2 || (s.total == 1 && bestN == 1);
  s.frames = v[bestA + (bestN - 1) / 2];
  return s;
}

std::string setupKey(const std::string& output, const std::string& input, double sampleRate, int buffer) {
  return output + "|" + input + "|" + std::to_string(static_cast<long long>(std::llround(sampleRate))) + "|" + std::to_string(buffer);
}

int recordLatency(int automaticRoundTrip, int measuredRoundTrip, int pluginDelay, int manualOffset) {
  const int dev = measuredRoundTrip > 0 ? measuredRoundTrip : automaticRoundTrip;
  return std::max(0, dev + std::max(0, pluginDelay) - manualOffset);
}

}  // namespace gl::loopback
