// Gridline — K-85: warp markers (time map). See TimeWarp.h.
#include "TimeWarp.h"
#include <algorithm>
#include <cmath>
#include <set>

namespace gl {

namespace {

// The last point with p[k].w <= w (-1: before the first).
long below(const WarpPoint* p, size_t n, double w) {
  size_t lo = 0, hi = n;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (p[mid].w <= w) lo = mid + 1;
    else hi = mid;
  }
  return static_cast<long>(lo) - 1;
}

long belowSource(const WarpPoint* p, size_t n, double s) {
  size_t lo = 0, hi = n;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (p[mid].s <= s) lo = mid + 1;
    else hi = mid;
  }
  return static_cast<long>(lo) - 1;
}

}  // namespace

double warpSourceAt(const WarpPoint* p, size_t n, double w) {
  if (n == 0) return w;
  const long k = below(p, n, w);
  if (k < 0) return p[0].s + (w - p[0].w);
  if (static_cast<size_t>(k) + 1 >= n) return p[n - 1].s + (w - p[n - 1].w);
  const WarpPoint& a = p[k];
  const WarpPoint& b = p[k + 1];
  const double span = b.w - a.w;
  return span > 0 ? a.s + (w - a.w) * (b.s - a.s) / span : a.s;
}

double warpTimeAt(const WarpPoint* p, size_t n, double s) {
  if (n == 0) return s;
  const long k = belowSource(p, n, s);
  if (k < 0) return p[0].w + (s - p[0].s);
  if (static_cast<size_t>(k) + 1 >= n) return p[n - 1].w + (s - p[n - 1].s);
  const WarpPoint& a = p[k];
  const WarpPoint& b = p[k + 1];
  const double span = b.s - a.s;
  return span > 0 ? a.w + (s - a.s) * (b.w - a.w) / span : a.w;
}

double warpSpeedAt(const WarpPoint* p, size_t n, double w) {
  if (n < 2) return 1.0;
  const long k = below(p, n, w);
  if (k < 0 || static_cast<size_t>(k) + 1 >= n) return 1.0;
  const double span = p[k + 1].w - p[k].w;
  return span > 0 ? (p[k + 1].s - p[k].s) / span : 1.0;
}

std::vector<WarpPoint> warpPoints(const std::vector<WarpMarker>& m, bool reversed, double last) {
  std::vector<WarpPoint> v;
  v.reserve(m.size());
  if (!reversed) {
    for (const WarpMarker& k : m) v.push_back({k.to, k.at});
  } else {
    for (auto it = m.rbegin(); it != m.rend(); ++it) v.push_back({-it->to, last - it->at});
  }
  return v;
}

bool warpIsIdentity(const std::vector<WarpMarker>& m) {
  for (size_t i = 1; i < m.size(); ++i) {
    const double ds = m[i].at - m[i - 1].at, dw = m[i].to - m[i - 1].to;
    if (std::fabs(ds - dw) > 1e-9 * std::max(1.0, std::fabs(dw))) return false;
  }
  return true;
}

bool normalizeWarp(std::vector<WarpMarker>& m, double maxAt) {
  const std::vector<WarpMarker> before = m;
  std::vector<WarpMarker> v;
  v.reserve(m.size());
  for (const WarpMarker& k : m)
    if (std::isfinite(k.at) && std::isfinite(k.to) && std::fabs(k.to) < 1e7) v.push_back(k);
  for (WarpMarker& k : v) k.at = std::clamp(k.at, 0.0, std::max(0.0, maxAt));
  std::stable_sort(v.begin(), v.end(), [](const WarpMarker& a, const WarpMarker& b) { return a.at < b.at; });
  std::vector<WarpMarker> out;
  out.reserve(v.size());
  for (const WarpMarker& k : v) {
    if (out.size() >= kMaxWarpMarkers) break;
    if (!out.empty()) {
      const double ds = k.at - out.back().at, dw = k.to - out.back().to;
      if (!(ds > 0) || !(dw >= kWarpMinSection)) continue;
      const double speed = ds / dw;
      if (speed < kWarpMinSpeed * (1 - 1e-9) || speed > kWarpMaxSpeed * (1 + 1e-9)) continue;
    }
    out.push_back(k);
  }
  std::set<uint32_t> used;
  for (WarpMarker& k : out)
    if (k.id == 0 || !used.insert(k.id).second) k.id = 0;
  for (WarpMarker& k : out)
    if (k.id == 0) {
      uint32_t id = 1;
      while (used.count(id)) ++id;
      k.id = id;
      used.insert(id);
    }
  m = std::move(out);
  return !(m == before);
}

uint32_t nextWarpId(const std::vector<WarpMarker>& m) {
  uint32_t id = 1;
  for (const WarpMarker& k : m) id = std::max(id, k.id + 1);
  return id;
}

}  // namespace gl
