// Gridline - "tracker" pack, K-106 cp18: the waveform peaks pyramid and column cache (tracker_peaks.h).
#include "tracker_peaks.h"

#include <algorithm>
#include <cmath>
#include <map>
#include "../../engine/Sample.h"
#include "tracker.h"

namespace gl::packs::trackerpack {

namespace {
inline float valueAt(const float* L, const float* R, int64_t i) { return R ? 0.5f * (L[i] + R[i]) : L[i]; }
}  // namespace

void PeakPyramid::build(const float* L, const float* R, int32_t frames) {
  frames_ = std::max<int32_t>(0, frames);
  mn_.clear();
  mx_.clear();
  if (frames_ <= 0 || !L) return;
  // Level 0 from the frames.
  const size_t n0 = (static_cast<size_t>(frames_) + kBlock - 1) / kBlock;
  std::vector<float> lo(n0), hi(n0);
  for (size_t b = 0; b < n0; ++b) {
    float l = 1e30f, h = -1e30f;
    const int64_t e = std::min<int64_t>(frames_, static_cast<int64_t>((b + 1) * kBlock));
    for (int64_t i = static_cast<int64_t>(b * kBlock); i < e; ++i) {
      const float v = valueAt(L, R, i);
      l = std::min(l, v);
      h = std::max(h, v);
    }
    lo[b] = l, hi[b] = h;
  }
  mn_.push_back(std::move(lo));
  mx_.push_back(std::move(hi));
  // Each further level halves the one below, while that one has 2048 entries or more.
  while (mn_.back().size() >= 2048) {
    const std::vector<float>& pl = mn_.back();
    const std::vector<float>& ph = mx_.back();
    const size_t n = (pl.size() + 1) / 2;
    std::vector<float> l(n), h(n);
    for (size_t b = 0; b < n; ++b) {
      const size_t j = 2 * b, k = std::min(pl.size() - 1, j + 1);
      l[b] = std::min(pl[j], pl[k]);
      h[b] = std::max(ph[j], ph[k]);
    }
    mn_.push_back(std::move(l));
    mx_.push_back(std::move(h));
  }
}

bool PeakPyramid::range(const float* L, const float* R, int64_t a, int64_t b, float* lo, float* hi) const {
  a = std::max<int64_t>(0, a);
  b = std::min<int64_t>(frames_, b);
  if (b <= a || !L) return false;
  float l = 1e30f, h = -1e30f;
  int64_t i = a;
  const int nl = levels();
  while (i < b) {
    if (i % kBlock != 0 || i + kBlock > b) {  // a ragged end: raw frames up to the next block edge
      const int64_t e = std::min<int64_t>(b, (i / kBlock + 1) * kBlock);
      rawReads += e - i;
      for (; i < e; ++i) {
        const float v = valueAt(L, R, i);
        l = std::min(l, v);
        h = std::max(h, v);
      }
      continue;
    }
    // The deepest level whose block starts here and ends inside the span.
    int lv = 0;
    while (lv + 1 < nl) {
      const int64_t size = static_cast<int64_t>(kBlock) << (lv + 1);
      if (i % size != 0 || i + size > b) break;
      ++lv;
    }
    const int64_t size = static_cast<int64_t>(kBlock) << lv;
    const auto idx = static_cast<size_t>(i / size);
    l = std::min(l, mn_[static_cast<size_t>(lv)][idx]);
    h = std::max(h, mx_[static_cast<size_t>(lv)][idx]);
    ++blockReads;
    i += size;
  }
  *lo = l, *hi = h;
  return l <= h;
}

const PeakPyramid& peaksFor(const std::shared_ptr<const Sample>& sp) {
  struct Entry {
    std::weak_ptr<const Sample> owner;
    PeakPyramid p;
  };
  static std::map<const Sample*, Entry> cache;
  static const PeakPyramid none;
  if (!sp) return none;
  Entry& en = cache[sp.get()];
  if (en.owner.expired() || en.owner.lock() != sp) {
    // (A file let go: its entry goes, so a new file at the same address is built afresh.)
    for (auto it = cache.begin(); it != cache.end();) it = it->first != sp.get() && it->second.owner.expired() ? cache.erase(it) : std::next(it);
    en.owner = sp;
    const auto n = static_cast<int32_t>(std::min<size_t>(sp->frames(), 0x3FFFFFFF));
    en.p.build(sp->left.data(), sp->mono() ? nullptr : sp->right.data(), n);
  }
  return en.p;
}

bool ColumnCache::update(const PeakPyramid& p, const float* L, const float* R, const ColumnKey& k) {
  if (k == key && static_cast<int>(lo.size()) == k.width) return false;
  key = k;
  ++builds;
  const int w = std::max(0, k.width);
  lo.assign(static_cast<size_t>(w), 1.0f);
  hi.assign(static_cast<size_t>(w), -1.0f);
  if (w == 0 || k.len <= 0) return true;
  const double per = k.len / w;
  for (int px = 0; px < w; ++px) {
    const double fa = k.start + per * px, fb = k.start + per * (px + 1);
    int64_t ia = static_cast<int64_t>(std::floor(fa)), ib = static_cast<int64_t>(std::ceil(fb));
    if (ib <= ia) ib = ia + 1;
    float l, h;
    if (!p.range(L, R, ia, ib, &l, &h)) continue;
    if (k.eightBit) l = eightBitValue(l), h = eightBitValue(h);
    lo[static_cast<size_t>(px)] = l;
    hi[static_cast<size_t>(px)] = h;
  }
  return true;
}

}  // namespace gl::packs::trackerpack
