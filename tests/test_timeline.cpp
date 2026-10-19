// Gridline — timeline maths: adaptive snap, zoom range, grid ladder, labels.
#include <cmath>
#include "../src/core/Song.h"
#include "../src/core/Timeline.h"
#include "Check.h"

using namespace gl;
using namespace gl::timeline;

int main() {
  Song s = makeEmptySong();  // 96 PPQ, 4/4
  const Tick bar = s.barTicks(), beat = s.beatTicks(), step = s.ppq / 4;
  expect(bar == 384 && beat == 96 && step == 24, "fixture: 96 PPQ 4/4");

  // Zoom range: whole songs down to single ticks several pixels wide.
  expect(std::fabs(maxPxPerTick(s) - 16.0) < 1e-9, "max zoom: 16 px per tick at 96 PPQ");
  expect(clampZoom(s, 1e6) == maxPxPerTick(s) && clampZoom(s, 0) == minPxPerTick(s), "zoom clamps to the range");

  // Visible grid gets finer as you zoom in.
  expect(visibleGrid(s, 0.05) >= bar, "zoomed out: bar-or-coarser grid");
  expect(visibleGrid(s, 0.2) == beat, "medium zoom: beat grid");
  expect(visibleGrid(s, 1.0) == step, "1 px/tick: step grid");
  expect(visibleGrid(s, 4.0) == 6, "4 px/tick: 1/4 step grid");
  expect(visibleGrid(s, maxPxPerTick(s)) == 1, "max zoom: tick grid");
  bool monotonic = true;
  Tick prev = visibleGrid(s, minPxPerTick(s));
  for (double z = minPxPerTick(s); z <= maxPxPerTick(s); z *= 1.1) {
    Tick g = visibleGrid(s, z);
    monotonic = monotonic && g <= prev;
    prev = g;
  }
  expect(monotonic, "grid never gets coarser while zooming in");

  // Line / Cell follow the zoom; maximum zoom unlocks to free; fixed modes don't.
  expect(snapGrid(s, Snap::Line, 0.2, false) == beat, "Line snaps to the visible beat grid");
  expect(snapGrid(s, Snap::Line, 1.0, false) == step, "Line snaps to steps when zoomed in");
  expect(snapGrid(s, Snap::Line, maxPxPerTick(s), false) == 1, "Line is free at maximum zoom");
  expect(snapGrid(s, Snap::Cell, maxPxPerTick(s), false) == 1, "Cell is free at maximum zoom");
  expect(snapGrid(s, Snap::Beat, maxPxPerTick(s), false) == beat, "fixed Beat stays a beat at any zoom");
  expect(snapGrid(s, Snap::Line, 0.2, true) == 1, "Alt unsnaps a snapped mode");
  expect(snapGrid(s, Snap::None, 0.2, true) == beat, "Alt snaps when snap is None");
  expect(snapGrid(s, Snap::None, 0.2, false) == 1, "None is free");

  // Line snaps to the nearest line, Cell to the start of the cell.
  expect(snapTick(70, beat, Snap::Line) == 96 && snapTick(40, beat, Snap::Line) == 0, "Line: nearest line");
  expect(snapTick(70, beat, Snap::Cell) == 0 && snapTick(100, beat, Snap::Cell) == 96, "Cell: cell start");
  expect(snapTick(77, 1, Snap::Line) == 77, "free grid keeps the exact tick");

  // Zoom around a point keeps the tick under the pointer in place.
  float z = 0.5f, scroll = 200.0f;
  const float anchor = 300.0f;
  const double before = (anchor + scroll) / z;
  zoomAt(s, z, scroll, 3.0, anchor);
  expect(std::fabs((anchor + scroll) / z - before) < 0.01, "zoomAt keeps the anchor tick fixed");
  float z2 = 0.5f, sc2 = 0;
  zoomAt(s, z2, sc2, 1e9, 10);
  expect(std::fabs(z2 - maxPxPerTick(s)) < 1e-5, "zoomAt clamps to max zoom");
  expect(std::fabs(fitZoom(s, 16 * bar, 1536) - 0.25) < 1e-9, "fitZoom: 16 bars in 1536 px");

  // Ruler labels deepen with the grid.
  const Tick t = 4 * bar + 2 * beat + 1 * step + 5;
  expect(rulerLabel(s, 4 * bar, bar) == "5", "bar label");
  expect(rulerLabel(s, 4 * bar + 2 * beat, beat) == "5.3", "beat label");
  expect(rulerLabel(s, 4 * bar + 2 * beat + step, step) == "5.3.2", "step label");
  expect(rulerLabel(s, t, 1) == "5.3.2:5", "tick label");
  expect(rulerLabel(s, 4 * bar + 2 * beat + step, 1) == "5.3.2", "tick grid on a step boundary omits :0");
  expect(gridName(s, 1) == "free" && gridName(s, beat) == "beat" && gridName(s, step / 4) == "1/4 step" &&
             gridName(s, 4 * bar) == "4 bars",
         "grid names");

  // Odd PPQ (e.g. 120 → step 30): ladder still reaches single ticks.
  Song s2 = makeEmptySong();
  s2.ppq = 120;
  expect(visibleGrid(s2, maxPxPerTick(s2)) == 1, "120 PPQ: max zoom reaches ticks");
  expect(snapGrid(s2, Snap::Line, maxPxPerTick(s2), false) == 1, "120 PPQ: Line free at max zoom");
  return finish();
}
