// Gridline: a sample played through its warp markers. See SampleWarpRender.h.
#include "SampleWarpRender.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include "TimeStretch.h"

namespace gl {

SamplePtr renderWarpedSample(const Sample& in, const std::vector<WarpMarker>& m) {
  const int64_t n = static_cast<int64_t>(in.frames());
  const double rate = in.sampleRate;
  if (n < 2 || rate <= 0) return nullptr;
  const double fileSec = static_cast<double>(n) / rate;
  const std::vector<WarpPoint> p = warpPoints(m, false, 0.0);
  const double base = p.empty() ? 0.0 : warpTimeAt(p, 0.0);
  const double outSec = std::max(1.0 / rate, warpTimeAt(p, fileSec) - base);
  const int64_t outLen = std::max<int64_t>(2, static_cast<int64_t>(std::llround(outSec * rate)));
  if (outLen > static_cast<int64_t>(rate * 60 * 20)) return nullptr;  // 20 minutes: refuse
  auto out = std::make_shared<Sample>();
  out->path = in.path;
  out->sampleRate = rate;
  out->left.assign(static_cast<size_t>(outLen), 0.0f);
  const bool mono = in.mono();
  const float* src[2] = {in.left.data(), in.right.data()};
  std::vector<float> rbuf;
  if (!mono) rbuf.assign(static_cast<size_t>(outLen), 0.0f);
  float* dst[2] = {out->left.data(), mono ? nullptr : rbuf.data()};
  stretch::Settings st;
  st.mode = stretch::kStretch;  // the one stretch engine (pitch kept)
  st.pitch = 1.0;
  stretch::processMapped(src, mono ? 1 : 2, n, dst, outLen, rate, st, [&](int64_t j) {
    const double w = base + static_cast<double>(j) / rate;
    return std::clamp(warpSourceAt(p, w) * rate, 0.0, static_cast<double>(n - 1));
  });
  if (mono) out->setMono();
  else out->right = std::move(rbuf);
  return out;
}

SamplePtr warpedSample(const SamplePtr& in, const SampleWarp& w) {
  if (!in || in->frames() < 2 || !w.active()) return in;
  static std::mutex mu;
  static std::map<std::string, std::pair<std::weak_ptr<const Sample>, SamplePtr>> cache;
  char k[48];
  std::snprintf(k, sizeof k, "%p|", static_cast<const void*>(in.get()));
  std::string key = k;
  for (const WarpMarker& m : w.markers) {
    char b[64];
    std::snprintf(b, sizeof b, "%.9g:%.9g;", m.at, m.to);  // (ids do not change the sound)
    key += b;
  }
  std::lock_guard<std::mutex> lock(mu);
  auto it = cache.find(key);
  if (it != cache.end() && it->second.first.lock() == in) return it->second.second;
  SamplePtr out = renderWarpedSample(*in, w.markers);
  if (!out) out = in;
  cache[key] = {in, out};
  if (cache.size() > 48)  // drop entries whose source sample is gone
    for (auto i = cache.begin(); i != cache.end();) i = i->second.first.expired() ? cache.erase(i) : std::next(i);
  return out;
}

std::vector<double> sampleOnsetsSec(const Sample& in, double sensitivity) {
  std::vector<double> out;
  const int64_t n = static_cast<int64_t>(in.frames());
  if (n < 2 || in.sampleRate <= 0) return out;
  std::vector<float> mono(static_cast<size_t>(n));
  for (int64_t i = 0; i < n; ++i) mono[static_cast<size_t>(i)] = 0.5f * (in.left[static_cast<size_t>(i)] + in.right[static_cast<size_t>(i)]);
  for (int64_t f : stretch::detectOnsets(mono.data(), n, in.sampleRate, sensitivity)) out.push_back(static_cast<double>(f) / in.sampleRate);
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace gl
