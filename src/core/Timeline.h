// Gridline — timeline maths shared by the playlist and the piano roll: snap
// modes (including the zoom-adaptive "Line" and "Cell"), zoom limits, the
// grid ladder used for drawing and snapping, and zoom-around-a-point.
#pragma once
#include <string>
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

// Grid a drag snaps to: the mode's size, or the visible grid for Line/Cell
// (1 tick = free at maximum zoom). `altHeld` inverts snapping like FL: a
// snapped mode becomes free, None snaps to the visible grid.
Tick snapGrid(const Song& s, Snap mode, double pxPerTick, bool altHeld);
// Snaps a position: Cell snaps to the start of the cell under the pointer,
// every other mode to the nearest grid line.
Tick snapTick(Tick t, Tick grid, Snap mode);

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

}  // namespace timeline
}  // namespace gl
