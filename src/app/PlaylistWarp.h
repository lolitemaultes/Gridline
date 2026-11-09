// Gridline — K-85: warp markers in the playlist (the warp tool).
//
// Every tool shows an audio clip's markers as faint lines in its body; the
// warp tool shows them as handles with the speed of each changed section:
// click adds a marker (on the nearest transient when that option is on),
// dragging a marker stretches the sections on both sides (on the grid unless
// Alt is held), Shift+drag slides the audio under a marker that stays,
// double-click or right-click removes one. The edits are clip edits (one undo
// step per drag, merged and shared like every other clip change).
#pragma once
#include <cstdint>
#include "../core/Song.h"
#include "../core/Automation.h"
#include "../../third_party/imgui/imgui.h"

namespace gl {
class App;

namespace plwarp {

// Draws the clip's markers inside its body [b0, b1] (tool: the warp tool is active).
void drawMarkers(ImDrawList* dl, const Song& s, const Clip& c, const TempoMap& tm, ImVec2 b0, ImVec2 b1, float clipX0, float pxPerTick,
                 float visX0, float visX1, bool tool, ImU32 ink);
// The marker under the pointer on clip `clip` (as last drawn; 0: none).
uint32_t hovered(ObjectId clip);

// A click with the warp tool on an audio clip: grabs the marker under the
// pointer or adds one at `tick` and grabs it. False: nothing to drag.
bool begin(App& a, const Clip& c, const TempoMap& tm, double tick, bool slide, uint64_t key);
// The grabbed marker follows the pointer as a clip move does: it keeps its
// offset from the pointer and lands on the nearest line of `grid` (1: free).
void drag(App& a, const TempoMap& tm, Tick pointer, Tick grid);
void end();
// The clip as drawn this frame: while its marker is dragged on a grid with
// smooth snapping, the marker and the waveform around it glide to where the
// document has it (as a dragged clip glides between cells); otherwise `c`.
const Clip& shown(const Song& s, const Clip& c, const TempoMap& tm, bool glide, double eps);
// Removes the marker under the pointer on `c` (double-click, right-click). False: none there.
bool removeHovered(App& a, const Clip& c);

// The audio clip menu's warp items (transients, quantize with its strength, clear).
void clipMenu(App& a, ObjectId clip, const TempoMap& tm, Tick grid);

}  // namespace plwarp
}  // namespace gl
