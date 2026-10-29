// Gridline — J-173: automation clips inside patterns, the menus shared by the
// channel rack, the pattern menus, the playlist and the browser.
#pragma once
#include "../../third_party/imgui/imgui.h"
#include <cstddef>
#include <cstdint>
#include "../core/Params.h"
#include "../core/Song.h"

namespace gl {

class App;

namespace patautoui {

// The colour a pattern's k-th automation clip is drawn in (its own, or one of
// a few told apart by order while it has the default colour), 0xRRGGBB.
uint32_t curveRgb(const AutomationClip& c, size_t k);
// Menu body for one pattern automation clip: edit, show in the piano roll,
// rename, colour, move to the playlist, delete. The pattern may be replaced by
// an item (callers must not keep pointers into the song across it).
void clipMenu(App& a, ObjectId pattern, ParamId target);
// Menu body: add an automation clip to the pattern for a control (the
// selected channel's, the song's, its mixer track's).
void addMenu(App& a, ObjectId pattern);
// Menu body: the pattern's automation clips (open each, a submenu per clip
// with clipMenu), then addMenu as a submenu.
void listMenu(App& a, ObjectId pattern);
// The pattern's automation clips as channel rack rows (below the channels):
// name button and the curve across the step area. Row geometry from the rack.
struct RackRows {
  float rowW, rowH, rowGap, cellH;
  float xLed, nameX, nameW;  // left section (row-relative)
  float stepsX, stepsW, scrollX, viewX0, viewX1;
  double playTick = -1;  // the pattern's playhead (pattern ticks), < 0: not playing it
};
void drawRackRows(App& a, ObjectId pattern, const RackRows& g);
// While an automation clip from the playlist's clip sources is dragged: a
// row that takes it into the pattern (drawn at the cursor, full width).
void rackDropRow(App& a, ObjectId pattern, float rowW, float rowH);

}  // namespace patautoui
}  // namespace gl
