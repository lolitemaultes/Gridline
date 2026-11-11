// Gridline — timeline maths (see Timeline.h).
#include "Timeline.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace gl {

const char* snapName(Snap s) {
  switch (s) {
    case Snap::Line: return "Line";
    case Snap::Cell: return "Cell";
    case Snap::None: return "None";
    case Snap::Sixth: return "1/6 step";
    case Snap::Quarter: return "1/4 step";
    case Snap::Third: return "1/3 step";
    case Snap::Half: return "1/2 step";
    case Snap::Step: return "Step";
    case Snap::Beat: return "Beat";
    case Snap::Bar: return "Bar";
  }
  return "?";
}

namespace {
struct Sizes {
  Tick bar, beat, step;
};
Sizes sizesOf(const Song& s) {
  return {std::max<Tick>(1, s.barTicks()), std::max<Tick>(1, s.beatTicks()), std::max<Tick>(1, s.ppq / 4)};
}
Sizes sizesOf(const MeterMap& m) { return {m.baseBar(), m.baseBeat(), std::max<Tick>(1, m.ppq() / 4)}; }

Tick snapTicksZ(const Sizes& z, Snap snap) {
  const Tick step = z.step;
  switch (snap) {
    case Snap::None: return 1;
    case Snap::Sixth: return std::max<Tick>(1, step / 6);
    case Snap::Quarter: return std::max<Tick>(1, step / 4);
    case Snap::Third: return std::max<Tick>(1, step / 3);
    case Snap::Half: return std::max<Tick>(1, step / 2);
    case Snap::Line:
    case Snap::Cell:
    case Snap::Step: return step;
    case Snap::Beat: return z.beat;
    case Snap::Bar: return z.bar;
  }
  return 1;
}
}  // namespace

Tick snapTicks(const Song& s, Snap snap) { return snapTicksZ(sizesOf(s), snap); }
Tick snapTicks(const MeterMap& m, Snap snap) { return snapTicksZ(sizesOf(m), snap); }

Tick snapDown(Tick t, Tick g) { return g <= 1 ? t : (t >= 0 ? t / g * g : -((-t + g - 1) / g * g)); }
Tick snapNearest(Tick t, Tick g) { return g <= 1 ? t : snapDown(t + g / 2, g); }

namespace timeline {

namespace {

// Grid sizes from coarsest to finest, each dividing the previous where possible.
std::vector<Tick> ladder(const Sizes& z) {
  const Tick bar = z.bar, beat = z.beat, step = z.step;
  std::vector<Tick> v;
  for (Tick k : {64, 32, 16, 8, 4, 2}) v.push_back(bar * k);
  v.push_back(bar);
  if (beat < bar) v.push_back(beat);
  if (step < beat) v.push_back(step);
  Tick d = std::min({bar, beat, step});
  while (d > 1) {
    if (d % 2 == 0) d /= 2;
    else if (d % 3 == 0) d /= 3;
    else d = 1;
    v.push_back(d);
  }
  if (v.back() != 1) v.push_back(1);
  return v;
}

Tick visibleGridZ(const Sizes& z, double pxPerTick, double minPx) {
  const std::vector<Tick> v = ladder(z);
  // Finest first: the first size whose lines are far enough apart.
  for (auto it = v.rbegin(); it != v.rend(); ++it)
    if (static_cast<double>(*it) * pxPerTick >= minPx) return *it;
  return v.front();
}

bool atMaxZoomZ(const Sizes& z, double pxPerTick) {
  return pxPerTick >= std::max(16.0, kMaxBeatPx / static_cast<double>(z.beat)) * 0.98;
}

Tick snapGridZ(const Sizes& z, Snap mode, double pxPerTick, bool altHeld) {
  const bool adaptive = mode == Snap::Line || mode == Snap::Cell;
  if (altHeld) return mode == Snap::None ? visibleGridZ(z, pxPerTick, 16.0) : 1;  // Alt inverts snapping
  if (!adaptive) return snapTicksZ(z, mode);
  return atMaxZoomZ(z, pxPerTick) ? 1 : visibleGridZ(z, pxPerTick, 16.0);
}

std::string gridNameZ(const Sizes& z, Tick grid) {
  const Tick bar = z.bar, beat = z.beat, step = z.step;
  if (grid <= 1) return "free";
  if (grid >= bar && grid % bar == 0) return grid == bar ? "bar" : std::to_string(grid / bar) + " bars";
  if (grid == beat) return "beat";
  if (grid == step) return "step";
  if (grid < step && step % grid == 0) return "1/" + std::to_string(step / grid) + " step";
  return std::to_string(grid) + " ticks";
}

}  // namespace

double minPxPerTick(const Song& s) { return kMinBeatPx / static_cast<double>(std::max<Tick>(1, s.beatTicks())); }
// Deep enough that a single tick is at least 16 px wide at every PPQ.
double maxPxPerTick(const Song& s) {
  return std::max(16.0, kMaxBeatPx / static_cast<double>(std::max<Tick>(1, s.beatTicks())));
}
bool atMaxZoom(const Song& s, double pxPerTick) { return pxPerTick >= maxPxPerTick(s) * 0.98; }

Tick visibleGrid(const Song& s, double pxPerTick, double minPx) { return visibleGridZ(sizesOf(s), pxPerTick, minPx); }
Tick visibleGrid(const MeterMap& m, double pxPerTick, double minPx) { return visibleGridZ(sizesOf(m), pxPerTick, minPx); }

Tick snapGrid(const Song& s, Snap mode, double pxPerTick, bool altHeld) { return snapGridZ(sizesOf(s), mode, pxPerTick, altHeld); }
Tick snapGrid(const MeterMap& m, Snap mode, double pxPerTick, bool altHeld) { return snapGridZ(sizesOf(m), mode, pxPerTick, altHeld); }

Tick snapTick(Tick t, Tick grid, Snap mode) { return mode == Snap::Cell ? snapDown(t, grid) : snapNearest(t, grid); }

Tick moveDistance(Tick dragged, Tick grid, Snap mode, Tick earliest) {
  const Tick d = snapTick(dragged, grid, mode);
  if (earliest + d >= 0) return d;
  (void)grid;
  return -std::max<Tick>(0, earliest);  // K-82: the wall at 0 (always a grid line)
}

Tick tickAtPx(float px, float pxPerTick) {
  constexpr double kLim = 4.0e15;  // far past any song, well inside Tick
  const double t = static_cast<double>(std::floor(px / pxPerTick));
  if (std::isnan(t)) return 0;
  return static_cast<Tick>(std::clamp(t, -kLim, kLim));
}

double clampZoom(const Song& s, double pxPerTick) { return std::clamp(pxPerTick, minPxPerTick(s), maxPxPerTick(s)); }

void zoomAt(const Song& s, float& pxPerTick, float& scrollX, double factor, float anchorPx) {
  const double tickAtAnchor = (static_cast<double>(anchorPx) + scrollX) / pxPerTick;
  const double z = clampZoom(s, pxPerTick * factor);
  pxPerTick = static_cast<float>(z);
  scrollX = static_cast<float>(std::max(0.0, tickAtAnchor * z - anchorPx));
}

double fitZoom(const Song& s, Tick ticks, float widthPx) {
  return clampZoom(s, static_cast<double>(std::max(1.0f, widthPx)) / static_cast<double>(std::max<Tick>(1, ticks)));
}

std::string rulerLabel(const Song& s, Tick t, Tick grid) {
  MeterMap m;
  m.setConstant(s.timeSigNum, s.timeSigDen, s.ppq);
  return rulerLabel(m, t, grid);
}

std::string gridName(const Song& s, Tick grid) { return gridNameZ(sizesOf(s), grid); }
std::string gridName(const MeterMap& m, Tick grid) { return gridNameZ(sizesOf(m), grid); }

// --- K-179: song time with meter changes -----------------------------------------------------

GridLevel gridLevel(const MeterMap& m, Tick grid) {
  GridLevel lv;
  const Tick bar = m.baseBar(), beat = m.baseBeat();
  if (grid >= bar && grid % bar == 0) {
    lv.kind = GridLevel::Bars;
    lv.bars = grid / bar;
  } else if (grid == beat) {
    lv.kind = GridLevel::Beat;
  } else {
    lv.ticks = std::max<Tick>(1, grid);
  }
  return lv;
}

namespace {
// The k-bar line nearest t (Line) or at or before it (Cell).
Tick snapBars(const MeterMap& m, Tick t, int64_t k, bool down) {
  auto floorK = [k](int64_t b) { return b >= 0 ? b / k * k : -((-b + k - 1) / k * k); };
  const int64_t b0 = floorK(m.barIndex(t));
  const Tick a = m.barStart(b0);
  if (down) return a;
  const Tick b = m.barStart(b0 + k);
  return t - a < b - t ? a : b;
}
}  // namespace

Tick snapTick(const MeterMap& m, Tick t, Tick grid, Snap mode) {
  if (m.constant()) return snapTick(t, grid, mode);
  const GridLevel lv = gridLevel(m, grid);
  const bool down = mode == Snap::Cell;
  switch (lv.kind) {
    case GridLevel::Bars: return snapBars(m, t, lv.bars, down);
    case GridLevel::Beat: return down ? m.beatStartAt(t) : m.nearestBeat(t);
    case GridLevel::Ticks: break;
  }
  return snapTick(t, lv.ticks, mode);
}

Tick moveDistance(const MeterMap& m, Tick dragged, Tick grid, Snap mode, Tick earliest) {
  if (m.constant()) return moveDistance(dragged, grid, mode, earliest);
  const GridLevel lv = gridLevel(m, grid);
  Tick d = 0;
  const Tick target = earliest + dragged;
  if (lv.kind == GridLevel::Bars) {
    // Whole bars from where the earliest one sits, k at a time: the count whose landing is nearest.
    const int64_t k = std::max<int64_t>(1, lv.bars);
    const int64_t n0 = (m.barIndex(target) - m.barIndex(earliest)) / k * k;
    Tick best = 0;
    bool have = false;
    for (int64_t n = n0 - 2 * k; n <= n0 + 2 * k; n += k) {
      const Tick c = m.addBars(earliest, n) - earliest;
      if (!have || std::llabs(c - dragged) < std::llabs(best - dragged)) best = c, have = true;
    }
    d = best;
  } else if (lv.kind == GridLevel::Beat) {
    const Tick off = earliest - m.beatStartAt(earliest);  // keeps its offset from the beat
    d = m.nearestBeat(target - off) + off - earliest;
    if (mode == Snap::Cell) d = m.beatStartAt(target - off) + off - earliest;
  } else {
    d = snapTick(dragged, lv.ticks, mode);
  }
  if (earliest + d >= 0) return d;
  return -std::max<Tick>(0, earliest);  // K-82: the wall at 0
}

std::string rulerLabel(const MeterMap& m, Tick t, Tick grid) {
  t = std::max<Tick>(0, t);
  const Tick step = std::max<Tick>(1, m.ppq() / 4);
  const MeterMap::Bbt p = m.toBbt(t);
  const long long b = p.bar + 1, bt = p.beat + 1, st = p.tick / step + 1, tk = p.tick % step;
  const Tick bar = m.baseBar(), beat = m.baseBeat();
  char buf[96];
  if (grid >= bar) std::snprintf(buf, sizeof buf, "%lld", b);
  else if (grid >= beat) std::snprintf(buf, sizeof buf, "%lld.%lld", b, bt);
  else if (grid >= step || tk == 0) std::snprintf(buf, sizeof buf, "%lld.%lld.%lld", b, bt, st);
  else std::snprintf(buf, sizeof buf, "%lld.%lld.%lld:%lld", b, bt, st, tk);
  return buf;
}

int lineStrength(const MeterMap& m, Tick t) {
  if (t == m.barStartAt(t)) return kLineBar;
  if (t != m.beatStartAt(t)) return kLineFine;
  return m.accentAt(t) == 1 ? kLineGroup : kLineBeat;
}

}  // namespace timeline
}  // namespace gl
