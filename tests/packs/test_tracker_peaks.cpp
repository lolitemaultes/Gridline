// Gridline - K-106 cp18 (K-316 plan 7.3): the Sample editor's waveform peaks (tracker_peaks.h).
//   * the pyramid's lowest / highest value over any span equals a brute-force scan (mono and
//     stereo, random spans, spans on and off block edges, the file's ragged end);
//   * the cost stays small: raw frames only at the ragged ends (< 2 blocks), a handful of blocks;
//   * the column cache equals a brute-force scan per pixel, rebuilds only when the file, the view,
//     the width or 8-bit play changes, and shows 8-bit play rounded as the instrument plays it.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>
#include "../../src/packs/tracker/tracker.h"
#include "../../src/packs/tracker/tracker_peaks.h"
#include "../Check.h"

using namespace gl;
namespace tp = gl::packs::trackerpack;

namespace {

void expect(bool ok, const std::string& name) { expect(ok, name.c_str()); }

bool brute(const std::vector<float>& L, const std::vector<float>* R, int64_t a, int64_t b, float* lo, float* hi) {
  a = std::max<int64_t>(0, a);
  b = std::min<int64_t>(static_cast<int64_t>(L.size()), b);
  float l = 1e30f, h = -1e30f;
  for (int64_t i = a; i < b; ++i) {
    const float v = R ? 0.5f * (L[static_cast<size_t>(i)] + (*R)[static_cast<size_t>(i)]) : L[static_cast<size_t>(i)];
    l = std::min(l, v);
    h = std::max(h, v);
  }
  *lo = l, *hi = h;
  return l <= h;
}

std::vector<float> noise(size_t n, unsigned seed) {
  std::mt19937 g(seed);
  std::uniform_real_distribution<float> d(-1.0f, 1.0f);
  std::vector<float> v(n);
  for (auto& x : v) x = d(g);
  return v;
}

void testRanges(bool stereo) {
  const char* tag = stereo ? "stereo" : "mono";
  for (const size_t n : {size_t{1}, size_t{63}, size_t{64}, size_t{65}, size_t{1000}, size_t{200003}}) {
    const std::vector<float> L = noise(n, 7 + static_cast<unsigned>(n)), Rv = noise(n, 99 + static_cast<unsigned>(n));
    const std::vector<float>* R = stereo ? &Rv : nullptr;
    tp::PeakPyramid p;
    p.build(L.data(), R ? R->data() : nullptr, static_cast<int32_t>(n));
    expect(p.frames() == static_cast<int32_t>(n), std::string(tag) + ": frames kept");
    if (n >= 64 * 2048) expect(p.levels() >= 2, std::string(tag) + ": a long file has more than one level");
    for (int lv = 0; lv + 1 < p.levels(); ++lv) expect(p.entries(lv + 1) == (p.entries(lv) + 1) / 2, std::string(tag) + ": each level halves the one below");
    std::mt19937 g(1234 + static_cast<unsigned>(n));
    int bad = 0, tries = 0;
    long long worstRaw = 0;
    for (int t = 0; t < 400; ++t) {
      std::uniform_int_distribution<int64_t> d(-10, static_cast<int64_t>(n) + 10);
      int64_t a = d(g), b = d(g);
      if (t % 5 == 0) a = (a / 64) * 64;  // on block edges too
      if (t % 7 == 0) b = (b / 64) * 64;
      if (a > b) std::swap(a, b);
      float l1 = 0, h1 = 0, l2 = 0, h2 = 0;
      const bool ok1 = brute(L, R, a, b, &l1, &h1);
      p.rawReads = 0;
      const bool ok2 = p.range(L.data(), R ? R->data() : nullptr, a, b, &l2, &h2);
      worstRaw = std::max(worstRaw, p.rawReads);
      ++tries;
      if (ok1 != ok2 || (ok1 && (l1 != l2 || h1 != h2))) ++bad;
    }
    expect(bad == 0, std::string(tag) + ": pyramid equals the brute-force scan, n=" + std::to_string(n) + " (" + std::to_string(bad) + " of " +
                         std::to_string(tries) + " differ)");
    expect(worstRaw < 2 * tp::PeakPyramid::kBlock, std::string(tag) + ": raw frames only at the ragged ends, n=" + std::to_string(n));
  }
  // The whole of a long file costs its top level's blocks (under 2048), not its frames; one column
  // of the whole file at 1440 px costs a block or two of the levels it spans.
  const std::vector<float> L = noise(1 << 21, 5);
  tp::PeakPyramid p;
  p.build(L.data(), nullptr, static_cast<int32_t>(L.size()));
  p.rawReads = p.blockReads = 0;
  float lo, hi;
  p.range(L.data(), nullptr, 0, static_cast<int64_t>(L.size()), &lo, &hi);
  expect(p.rawReads == 0 && p.blockReads <= p.entries(p.levels() - 1) && p.entries(p.levels() - 1) < 2048,
         "a whole 2M-frame file: its top level's blocks, no raw frames (" + std::to_string(p.blockReads) + " blocks)");
  tp::ColumnCache cc;
  p.rawReads = p.blockReads = 0;
  cc.update(p, L.data(), nullptr, tp::ColumnKey{&p, 0.0, static_cast<double>(L.size()), 1440, false});
  expect(p.blockReads <= 1440LL * 2 * p.levels() && p.rawReads <= 1440LL * 2 * tp::PeakPyramid::kBlock,
         "1440 columns of a 2M-frame file: a few blocks each (" + std::to_string(p.blockReads) + " blocks, " + std::to_string(p.rawReads) + " raw frames)");
}

void testColumns() {
  const size_t n = 100000;
  std::vector<float> L = noise(n, 31);
  for (size_t i = 0; i < 3000; ++i) L[i] = 0.004f * static_cast<float>(i % 7);  // quiet values the 8-bit rounding moves
  tp::PeakPyramid p;
  p.build(L.data(), nullptr, static_cast<int32_t>(n));
  tp::ColumnCache c;
  const tp::ColumnKey k{&p, 1234.5, 50000.0, 1430, false};
  expect(c.update(p, L.data(), nullptr, k), "columns: built the first time");
  expect(!c.update(p, L.data(), nullptr, k) && !c.update(p, L.data(), nullptr, k), "columns: the same view does not rebuild");
  expect(c.builds == 1, "columns: one build for three frames of the same view");
  int bad = 0;
  const double per = k.len / k.width;
  for (int px = 0; px < k.width; ++px) {
    const double fa = k.start + per * px, fb = k.start + per * (px + 1);
    int64_t ia = static_cast<int64_t>(std::floor(fa)), ib = static_cast<int64_t>(std::ceil(fb));
    if (ib <= ia) ib = ia + 1;
    float l, h;
    brute(L, nullptr, ia, ib, &l, &h);
    if (c.lo[static_cast<size_t>(px)] != l || c.hi[static_cast<size_t>(px)] != h) ++bad;
  }
  expect(bad == 0, "columns: each equals a brute-force scan of its frames");
  tp::ColumnKey k2 = k;
  k2.start += 1;
  expect(c.update(p, L.data(), nullptr, k2), "columns: a scrolled view rebuilds");
  k2.width = 900;
  expect(c.update(p, L.data(), nullptr, k2) && c.lo.size() == 900, "columns: another width rebuilds");
  k2.len = 5000;
  expect(c.update(p, L.data(), nullptr, k2), "columns: a zoom rebuilds");
  tp::ColumnKey k3{&p, 0, 2000, 400, true};
  expect(c.update(p, L.data(), nullptr, k3), "columns: 8-bit play rebuilds");
  bool rounded = true;
  for (size_t px = 0; px < c.lo.size(); ++px) rounded = rounded && c.lo[px] == tp::eightBitValue(c.lo[px]) && c.hi[px] == tp::eightBitValue(c.hi[px]);
  expect(rounded, "columns: 8-bit play shows the values rounded to 256 steps");
  // Zoomed in past one frame a pixel: every column still has the frame under it.
  tp::ColumnKey k4{&p, 10, 50, 500, false};
  c.update(p, L.data(), nullptr, k4);
  bool all = true;
  for (size_t px = 0; px < c.lo.size(); ++px) all = all && c.lo[px] <= c.hi[px];
  expect(all, "columns: zoomed in, each pixel shows its frame");
  // Past the file's end: empty columns, nothing read out of bounds.
  tp::ColumnKey k5{&p, static_cast<double>(n) - 100, 400, 400, false};
  c.update(p, L.data(), nullptr, k5);
  expect(c.lo.back() > c.hi.back() && c.lo.front() <= c.hi.front(), "columns: past the end, empty");
}

}  // namespace

int main() {
  testRanges(false);
  testRanges(true);
  testColumns();
  return finish();
}
