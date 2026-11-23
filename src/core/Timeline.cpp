// Gridline — timeline maths (see Timeline.h).
#include "Timeline.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <numeric>
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
    case Snap::Main: return "Main";
    case Snap::BeatSixth: return "1/6 beat";
    case Snap::BeatQuarter: return "1/4 beat";
    case Snap::BeatThird: return "1/3 beat";
    case Snap::BeatHalf: return "1/2 beat";
    case Snap::StepDotted: return "Dotted step";
    case Snap::BeatHalfDotted: return "Dotted 1/2 beat";
    case Snap::BeatDotted: return "Dotted beat";
    case Snap::BeatFifth: return "1/5 beat";
    case Snap::BeatSeventh: return "1/7 beat";
    case Snap::RowGrid: return "Row grid";
  }
  return "?";
}

// ---- SnapGrid: a size that may be a fraction of a tick ----
namespace {
Tick floorDivT(Tick a, Tick b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
}  // namespace

SnapGrid::SnapGrid(Tick n, Tick d) {
  if (d < 1) d = 1;
  if (n < d) n = d;  // never finer than one tick
  const Tick g = std::gcd(n, d);
  num = n / g;
  den = d / g;
}

SnapGrid::operator Tick() const {
  if (den <= 1) return num;
  return std::max<Tick>(1, (num + den / 2) / den);
}

Tick SnapGrid::at(int64_t k) const {
  if (den <= 1) return k * num;
  return floorDivT(2 * k * num + den, 2 * den);  // round(k * num / den), halves up
}

int64_t SnapGrid::indexDown(Tick t) const {
  if (num <= 0) return 0;
  if (den <= 1) return floorDivT(t, num);
  int64_t k = floorDivT(t * den, num);
  while (at(k) > t) --k;
  while (at(k + 1) <= t) ++k;
  return k;
}

Tick snapDown(Tick t, const SnapGrid& g) {
  if (!g.fractional()) return snapDown(t, g.num);
  return g.at(g.indexDown(t));
}

Tick snapNearest(Tick t, const SnapGrid& g) {
  if (!g.fractional()) return snapNearest(t, g.num);
  const int64_t k = g.indexDown(t);
  const Tick a = g.at(k), b = g.at(k + 1);
  return t - a < b - t ? a : b;  // ties go to the later line, as snapNearest does
}

namespace {
std::atomic<int> gMainSnap{static_cast<int>(Snap::Line)};
}  // namespace

void setMainSnap(Snap s) { gMainSnap.store(static_cast<int>(s), std::memory_order_relaxed); }
Snap mainSnap() { return static_cast<Snap>(gMainSnap.load(std::memory_order_relaxed)); }
Snap resolveSnap(Snap s) {
  if (s != Snap::Main) return s;
  const Snap m = mainSnap();
  return m == Snap::Main ? Snap::Line : m;
}
bool adaptiveSnap(Snap s) {
  s = resolveSnap(s);
  return s == Snap::Line || s == Snap::Cell;
}

namespace {
struct Sizes {
  Tick bar, beat, step;
  Tick rowNum = 0, rowDen = 1;  // K-255: the edited row's own step (bar / grid; 0: the step)
};
Sizes sizesOf(const Song& s) {
  return {std::max<Tick>(1, s.barTicks()), std::max<Tick>(1, s.beatTicks()), std::max<Tick>(1, s.ppq / 4)};
}
Sizes sizesOf(const MeterMap& m) { return {m.baseBar(), m.baseBeat(), m.stepTicks(), m.rowGridNum(), m.rowGridDen()}; }

SnapGrid snapSizeZ(const Sizes& z, Snap snap) {
  const Tick step = z.step, beat = z.beat;
  switch (resolveSnap(snap)) {
    case Snap::None: return SnapGrid(1);
    case Snap::Sixth: return SnapGrid(step, 6);
    case Snap::Quarter: return SnapGrid(step, 4);
    case Snap::Third: return SnapGrid(step, 3);
    case Snap::Half: return SnapGrid(step, 2);
    case Snap::Main:  // (resolved above)
    case Snap::Line:
    case Snap::Cell:
    case Snap::Step: return SnapGrid(step);
    case Snap::Beat: return SnapGrid(beat);
    case Snap::Bar: return SnapGrid(z.bar);
    case Snap::BeatSixth: return SnapGrid(beat, 6);
    case Snap::BeatQuarter: return SnapGrid(beat, 4);
    case Snap::BeatThird: return SnapGrid(beat, 3);
    case Snap::BeatHalf: return SnapGrid(beat, 2);
    case Snap::StepDotted: return SnapGrid(step * 3, 2);
    case Snap::BeatHalfDotted: return SnapGrid(beat * 3, 4);
    case Snap::BeatDotted: return SnapGrid(beat * 3, 2);
    case Snap::BeatFifth: return SnapGrid(beat, 5);
    case Snap::BeatSeventh: return SnapGrid(beat, 7);
    case Snap::RowGrid: return z.rowNum > 0 ? SnapGrid(z.rowNum, z.rowDen) : SnapGrid(step);
  }
  return SnapGrid(1);
}
Tick snapTicksZ(const Sizes& z, Snap snap) { return snapSizeZ(z, snap); }
}  // namespace

Tick snapTicks(const Song& s, Snap snap) { return snapTicksZ(sizesOf(s), snap); }
Tick snapTicks(const MeterMap& m, Snap snap) { return snapTicksZ(sizesOf(m), snap); }
SnapGrid snapSize(const Song& s, Snap snap) { return snapSizeZ(sizesOf(s), snap); }
SnapGrid snapSize(const MeterMap& m, Snap snap) { return snapSizeZ(sizesOf(m), snap); }

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

SnapGrid snapGridZ(const Sizes& z, Snap mode, double pxPerTick, bool altHeld) {
  mode = resolveSnap(mode);
  const bool adaptive = mode == Snap::Line || mode == Snap::Cell;
  if (altHeld) return SnapGrid(mode == Snap::None ? visibleGridZ(z, pxPerTick, 16.0) : 1);  // Alt inverts snapping
  if (!adaptive) return snapSizeZ(z, mode);
  return SnapGrid(atMaxZoomZ(z, pxPerTick) ? 1 : visibleGridZ(z, pxPerTick, 16.0));
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

SnapGrid snapGrid(const Song& s, Snap mode, double pxPerTick, bool altHeld) { return snapGridZ(sizesOf(s), mode, pxPerTick, altHeld); }
SnapGrid snapGrid(const MeterMap& m, Snap mode, double pxPerTick, bool altHeld) { return snapGridZ(sizesOf(m), mode, pxPerTick, altHeld); }

Tick snapTick(Tick t, Tick grid, Snap mode) { return resolveSnap(mode) == Snap::Cell ? snapDown(t, grid) : snapNearest(t, grid); }
Tick snapTick(Tick t, const SnapGrid& grid, Snap mode) { return resolveSnap(mode) == Snap::Cell ? snapDown(t, grid) : snapNearest(t, grid); }

Tick moveDistance(Tick dragged, Tick grid, Snap mode, Tick earliest) {
  const Tick d = snapTick(dragged, grid, mode);
  if (earliest + d >= 0) return d;
  return -std::max<Tick>(0, earliest);  // K-82: the wall at 0 (always a grid line)
}

Tick moveDistance(Tick dragged, const SnapGrid& grid, Snap mode, Tick earliest) {
  if (!grid.fractional()) return moveDistance(dragged, grid.num, mode, earliest);
  // A fractional grid is not evenly spaced in ticks: move the earliest one's own grid line to the
  // line nearest where it is dragged (or the cell under it), keeping its offset from that line.
  const Tick from = snapDown(earliest, grid);
  const Tick d = snapTick(from + dragged, grid, mode) - from;
  if (earliest + d >= 0) return d;
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
std::string gridName(const MeterMap& m, const SnapGrid& grid) {
  if (!grid.fractional()) return gridName(m, grid.num);
  const Sizes z = sizesOf(m);
  if (z.rowNum > 0 && SnapGrid(z.rowNum, z.rowDen) == grid) return snapName(Snap::RowGrid);  // K-255
  for (Snap sn : {Snap::BeatFifth, Snap::BeatSeventh, Snap::Sixth, Snap::Third, Snap::Quarter, Snap::Half, Snap::BeatSixth, Snap::BeatThird,
                  Snap::StepDotted, Snap::BeatHalfDotted, Snap::BeatDotted, Snap::BeatQuarter, Snap::BeatHalf})
    if (snapSizeZ(z, sn) == grid) {
      std::string n = snapName(sn);
      if (!n.empty() && n[0] == 'D') n[0] = 'd';  // "dotted step", as the other names read
      return n;
    }
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.2f ticks", static_cast<double>(grid.num) / static_cast<double>(grid.den));
  return buf;
}

bool drawsSnapLines(const MeterMap& m, Snap mode, double pxPerTick, double minPx) {
  mode = resolveSnap(mode);
  if (mode == Snap::Line || mode == Snap::Cell || mode == Snap::None) return false;
  const SnapGrid g = snapSizeZ(sizesOf(m), mode);
  const double size = static_cast<double>(g.num) / static_cast<double>(g.den);
  return size < static_cast<double>(m.baseBeat()) && size * pxPerTick >= minPx;
}

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

Tick snapTick(const MeterMap& m, Tick t, const SnapGrid& grid, Snap mode) {
  if (grid.fractional()) return snapTick(t, grid, mode);  // the same absolute grid in every section
  return snapTick(m, t, grid.num, mode);
}

Tick snapTick(const MeterMap& m, Tick t, Tick grid, Snap mode) {
  mode = resolveSnap(mode);
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

Tick moveDistance(const MeterMap& m, Tick dragged, const SnapGrid& grid, Snap mode, Tick earliest) {
  if (grid.fractional()) return moveDistance(dragged, grid, mode, earliest);
  return moveDistance(m, dragged, grid.num, mode, earliest);
}

Tick moveDistance(const MeterMap& m, Tick dragged, Tick grid, Snap mode, Tick earliest) {
  mode = resolveSnap(mode);
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
  const Tick step = m.stepTicks();
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
