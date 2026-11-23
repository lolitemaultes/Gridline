// Gridline — timeline maths shared by the playlist and the piano roll: snap
// modes (including the zoom-adaptive "Line" and "Cell"), zoom limits, the
// grid ladder used for drawing and snapping, and zoom-around-a-point.
#pragma once
#include <string>
#include "Meter.h"
#include "Song.h"

namespace gl {

// Line / Cell follow the grid that is drawn at the current zoom (finer as you
// zoom in, free at maximum zoom); the rest are fixed sizes. Main follows the
// main snap (the playlist's), so the editors can share one grid. Step is the
// pattern's own step where the editor knows one (12 or 24 steps a bar), else a
// sixteenth note. New modes are appended: the order is not saved anywhere, but
// dev-input scripts and tests name the modes by snapName.
enum class Snap {
  Line, Cell, None, Sixth, Quarter, Third, Half, Step, Beat, Bar,
  Main,                                            // GitHub #58: the main snap
  BeatSixth, BeatQuarter, BeatThird, BeatHalf,     // beat fractions
  StepDotted, BeatHalfDotted, BeatDotted,          // dotted values
  BeatFifth, BeatSeventh,                          // quintuplets and septuplets (5 or 7 to a beat)
  RowGrid                                          // K-255: the edited row's own grid (a step of it; elsewhere a step)
};
constexpr int kSnapCount = static_cast<int>(Snap::RowGrid) + 1;

const char* snapName(Snap s);

// A grid size in ticks, possibly a fraction of a tick (num / den): a triplet of an odd
// step, a quintuplet or a septuplet. Grid line k sits at round(k * num / den), so the lines
// never drift from the beats they divide even when a tick cannot be split. A plain size
// converts both ways (explicitly from a Tick, implicitly to its rounded size).
struct SnapGrid {
  Tick num = 0, den = 1;
  constexpr SnapGrid() = default;
  explicit constexpr SnapGrid(Tick t) : num(t) {}
  SnapGrid(Tick n, Tick d);           // reduced; never finer than one tick
  operator Tick() const;          // the size rounded to whole ticks (a plain size as it is)
  bool fractional() const { return den > 1; }
  Tick at(int64_t k) const;       // grid line k (line 0 at tick 0)
  int64_t indexDown(Tick t) const;  // the last line at or before t
  bool operator==(const SnapGrid& o) const { return num == o.num && den == o.den; }
  bool operator!=(const SnapGrid& o) const { return !(*this == o); }
};

// Fixed size of a snap mode in ticks (1 for None). Line/Cell have no fixed
// size; they fall back to one step here (use timeline::snapGrid with a zoom).
// A fractional size (a quintuplet) comes back rounded; snapSize keeps it exact.
Tick snapTicks(const Song& song, Snap s);
// K-179: the same in a meter map's meter at the start (a pattern's own meter: a constant map).
Tick snapTicks(const MeterMap& m, Snap s);
SnapGrid snapSize(const Song& song, Snap s);
SnapGrid snapSize(const MeterMap& m, Snap s);
Tick snapDown(Tick t, Tick grid);
Tick snapNearest(Tick t, Tick grid);
Tick snapDown(Tick t, const SnapGrid& grid);
Tick snapNearest(Tick t, const SnapGrid& grid);

// The main snap that Snap::Main follows (the app sets it from the playlist's snap each
// frame); Main itself, or a main snap of Main, reads as Line.
void setMainSnap(Snap s);
Snap mainSnap();
Snap resolveSnap(Snap s);  // Main -> the main snap; every other mode as it is
// True for the modes whose size follows the zoom (Line and Cell, through Main too).
bool adaptiveSnap(Snap s);

namespace timeline {

// Zoom range in pixels per tick: from a whole long song on screen down to
// single ticks several pixels wide. Expressed through beat widths so it
// behaves the same at every PPQ.
constexpr double kMinBeatPx = 1.5;
constexpr double kMaxBeatPx = 1536.0;
double minPxPerTick(const Song& s);
double maxPxPerTick(const Song& s);
// True at (or within a hair of) the deepest zoom: Line/Cell snapping turns free.
bool atMaxZoom(const Song& s, double pxPerTick);

// Finest grid (ticks) from the ladder … 16 bars · 4 bars · 2 bars · bar · beat ·
// step · ½ · ¼ · ⅛ · 1/16 step … 1 tick whose lines are at least `minPx` apart.
Tick visibleGrid(const Song& s, double pxPerTick, double minPx = 16.0);
Tick visibleGrid(const MeterMap& m, double pxPerTick, double minPx = 16.0);  // K-179

// Grid a drag snaps to: the mode's size, or the visible grid for Line/Cell
// (1 tick = free at maximum zoom). `altHeld` inverts snapping like FL: a
// snapped mode becomes free, None snaps to the visible grid.
SnapGrid snapGrid(const Song& s, Snap mode, double pxPerTick, bool altHeld);
SnapGrid snapGrid(const MeterMap& m, Snap mode, double pxPerTick, bool altHeld);  // K-179
// Snaps a position: Cell snaps to the start of the cell under the pointer,
// every other mode to the nearest grid line.
Tick snapTick(Tick t, Tick grid, Snap mode);
Tick snapTick(Tick t, const SnapGrid& grid, Snap mode);
// How far a drag moves clips or notes: whole grid steps from where they are, so what sits off the
// grid keeps its offset. `earliest` is the earliest start among them. K-82: dragged past the song's
// start they stop at the wall: the earliest one lands on 0 (on the beat, its offset gone) and the
// others keep their places relative to it.
Tick moveDistance(Tick dragged, Tick grid, Snap mode, Tick earliest);
Tick moveDistance(Tick dragged, const SnapGrid& grid, Snap mode, Tick earliest);

// Tick under a pixel offset (floor(px / pxPerTick)), defined for any input:
// the pointer position is -FLT_MAX while it is outside the window (J-46: a
// zoom drag released there converted -inf to an integer).
Tick tickAtPx(float px, float pxPerTick);  // (float maths, as the views use)

// Clamps a zoom value to the song's range.
double clampZoom(const Song& s, double pxPerTick);
// Zooms by `factor`, keeping the tick under `anchorPx` (pixels from the left
// edge of the grid) in place. Updates zoom and horizontal scroll (pixels).
void zoomAt(const Song& s, float& pxPerTick, float& scrollX, double factor, float anchorPx);
// Zoom that fits `ticks` into `widthPx` (clamped).
double fitZoom(const Song& s, Tick ticks, float widthPx);

// Ruler label for a grid line at tick t: "5" (bar), "5.3" (beat), "5.3.2"
// (step) or "5.3.2:12" (ticks into the step), depending on the grid spacing.
std::string rulerLabel(const Song& s, Tick t, Tick grid);
// Human description of the effective grid for the snap chip ("1/4 step", "free").
std::string gridName(const Song& s, Tick grid);
std::string gridName(const MeterMap& m, Tick grid);  // K-179
// The same for a size that may be a fraction ("1/5 beat", "dotted step").
std::string gridName(const MeterMap& m, const SnapGrid& grid);
// GitHub #58: true when the editors should draw this fixed snap's own lines (a triplet, dotted
// or tuplet grid, or a pattern step the zoom ladder does not show) rather than the zoom's lines
// below the beat: the mode is fixed, finer than a beat, and its lines sit at least `minPx` apart.
bool drawsSnapLines(const MeterMap& m, Snap mode, double pxPerTick, double minPx = 6.0);

// --- K-179: song time with meter changes -----------------------------------------------------
// Grid sizes from visibleGrid / snapGrid are measured in the song's meter at the start. In a map
// with meter changes they mean a level: whole bars (k bars), the beat, or (finer) a fixed size in
// ticks. With one meter everything below equals the functions above exactly.
struct GridLevel {
  enum Kind { Bars, Beat, Ticks } kind = Ticks;
  int64_t bars = 1;  // Bars: lines every `bars` bars
  Tick ticks = 1;    // Ticks: the size
};
GridLevel gridLevel(const MeterMap& m, Tick grid);
// snapTick in the map: bar levels snap to the map's bar lines (short bars included), the beat to
// the beat lines of the section, finer sizes to the same absolute grid as before.
Tick snapTick(const MeterMap& m, Tick t, Tick grid, Snap mode);
Tick snapTick(const MeterMap& m, Tick t, const SnapGrid& grid, Snap mode);
// moveDistance in the map: at a bar level the distance is whole bars crossed, so a clip on a bar
// line lands on a bar line of the section it is dropped in (its offset into the bar kept).
Tick moveDistance(const MeterMap& m, Tick dragged, Tick grid, Snap mode, Tick earliest);
Tick moveDistance(const MeterMap& m, Tick dragged, const SnapGrid& grid, Snap mode, Tick earliest);
// The ruler label in the map ("9", "9.4", "9.4.2", "9.4.2:12").
std::string rulerLabel(const MeterMap& m, Tick t, Tick grid);
// Grid line strength: 3 bar line, 2 compound group, 1 beat, 0 finer.
enum : int { kLineFine = 0, kLineBeat = 1, kLineGroup = 2, kLineBar = 3 };
int lineStrength(const MeterMap& m, Tick t);
// Calls f(tick, strength) for every line of `grid` in [t0, t1) (t0 >= 0), in order.
template <class F>
void forEachLine(const MeterMap& m, Tick t0, Tick t1, Tick grid, F f) {
  t0 = t0 < 0 ? 0 : t0;
  const GridLevel lv = gridLevel(m, grid);
  if (lv.kind == GridLevel::Bars) {
    const int64_t k = lv.bars < 1 ? 1 : lv.bars;
    int64_t b = m.barIndex(t0);
    if (m.barStart(b) < t0) ++b;
    for (Tick t = m.barStart(b); t < t1; t = m.barStart(++b)) {
      const MeterSeg& g = m.segAt(t);
      if (b % k == 0 || t == g.start) f(t, kLineBar);
    }
    return;
  }
  if (lv.kind == GridLevel::Beat) {
    Tick t = m.beatStartAt(t0);
    if (t < t0) t = m.nextBeat(t);
    for (; t < t1; t = m.nextBeat(t)) f(t, lineStrength(m, t));
    return;
  }
  const Tick g = lv.ticks < 1 ? 1 : lv.ticks;
  const auto& segs = m.segments();
  size_t si = m.segIndex(t0) + 1;  // the next section start that may fall between grid lines
  for (Tick t = snapDown(t0 + g - 1, g); t < t1; t += g) {
    for (; si < segs.size() && segs[si].start <= t; ++si)
      if (segs[si].start < t && segs[si].start >= t0 && segs[si].start % g) f(segs[si].start, kLineBar);
    f(t, lineStrength(m, t));
  }
}

// The same for a size that may be fractional: lines at grid.at(k), each with its strength.
template <class F>
void forEachLine(const MeterMap& m, Tick t0, Tick t1, const SnapGrid& grid, F f) {
  if (!grid.fractional()) return forEachLine(m, t0, t1, static_cast<Tick>(grid), f);
  t0 = t0 < 0 ? 0 : t0;
  int64_t k = grid.indexDown(t0);
  if (grid.at(k) < t0) ++k;
  for (Tick t = grid.at(k); t < t1; t = grid.at(++k)) f(t, lineStrength(m, t));
}

}  // namespace timeline
}  // namespace gl
