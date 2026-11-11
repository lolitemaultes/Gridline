// Gridline — meter changes in the interface: the playlist ruler's meter lane (shown when the song
// has meter changes), its menu, and the meter change editor (a small floating window). Every edit
// is one undo step (core/MeterEdit.h).
#pragma once
#include "../../third_party/imgui/imgui.h"
#include "../core/Meter.h"

namespace gl {

class App;

namespace meterui {

// Height of the meter lane at the top of the playlist ruler (0 when the song has no meter changes).
constexpr float kLaneH = 14.0f;
float laneHeight(const Song& s);

// Draws the lane in [p0, p1] (tick 0 at x = `originX`) and handles its pointer and Delete key.
// `hovered`: the playlist canvas is hovered. Returns true while the pointer is in the lane.
bool lane(App& a, ImDrawList* dl, const MeterMap& map, ImVec2 p0, ImVec2 p1, float originX, float pxPerTick, bool hovered);

// Opens the editor for the meter change at marker `index` (-1: the song's meter at the start).
void openEditor(App& a, int index);
// Adds a meter change at the bar line at or before `pos` (the meter there) and opens its editor.
void addAndEdit(App& a, Tick pos);
// The editor window (from the dialogs pass).
void drawEditor(App& a);

// The "Meter" submenu of a pattern's menus: follows the song, or a meter of its own.
void patternMeterMenu(App& a, ObjectId pattern);
// Playlist clip menu items for a pattern clip: make the pattern's meter match the section it starts in.
void clipMeterItems(App& a, ObjectId clip);

// Common meters offered in menus and the editor.
inline constexpr int kCommon[][2] = {{2, 4}, {3, 4}, {4, 4}, {5, 4}, {6, 8}, {7, 8}, {9, 8}, {12, 8}};

}  // namespace meterui
}  // namespace gl
