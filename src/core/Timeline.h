// Gridline — timeline maths shared by the playlist and the piano roll: snap
// modes (including the zoom-adaptive "Line" and "Cell"), zoom limits, the
// grid ladder used for drawing and snapping, and zoom-around-a-point.
#pragma once
#include <string>
#include "Meter.h"
#include "Song.h"

namespace gl {

// Line / Cell follow the grid that is drawn at the current zoom (finer as you
// zoom in, free at maximum zoom); the rest are fixed sizes.
enum class Snap { Line, Cell, None, Sixth, Quarter, Third, Half, Step, Beat, Bar };
constexpr int kSnapCount = static_cast<int>(Snap::Bar) + 1;

const char* snapName(Snap s);
// Fixed size of a snap mode in ticks (1 for None). Line/Cell have no fixed
// size; they fall back to one step here (use timeline::snapGrid with a zoom).
Tick snapTicks(const Song& song, Snap s);
// K-179: the same in a meter map's meter at the start (a pattern's own meter: a constant map).
Tick snapTicks(const MeterMap& m, Snap s);
Tick snapDown(Tick t, Tick grid);
Tick snapNearest(Tick t, Tick grid);

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
Tick snapGrid(const Song& s, Snap mode, double pxPerTick, bool altHeld);
Tick snapGrid(const MeterMap& m, Snap mode, double pxPerTick, bool altHeld);  // K-179
// Snaps a position: Cell snaps to the start of the cell under the pointer,
// every other mode to the nearest grid line.
Tick snapTick(Tick t, Tick grid, Snap mode);
// How far a drag moves clips or notes: whole grid steps from where they are, so what sits off the
// grid keeps its offset. `earliest` is the earliest start among them. K-82: dragged past the song's
// start they stop at the wall: the earliest one lands on 0 (on the beat, its offset gone) and the
// others keep their places relative to it.
Tick moveDistance(Tick dragged, Tick grid, Snap mode, Tick earliest);

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
// moveDistance in the map: at a bar level the distance is whole bars crossed, so a clip on a bar
// line lands on a bar line of the section it is dropped in (its offset into the bar kept).
Tick moveDistance(const MeterMap& m, Tick dragged, Tick grid, Snap mode, Tick earliest);
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

}  // namespace timeline
}  // namespace gl
