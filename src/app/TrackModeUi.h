// Gridline — playlist track modes UI (PL-08): the track menu's mode and
// recording items, the audio / instrument track header controls, and the
// mixer and channel menu entries. Edits go through core/TrackModes (undoable).
#pragma once
#include "../../third_party/imgui/imgui.h"
#include "../core/Types.h"

namespace gl {

class App;

namespace trackui {

// Track menu block: "Track mode" submenu and the audio track options. Draws
// inside an open menu.
void trackMenuItems(App& a, int track);
// Header controls of an audio / instrument track on the bottom line of the
// header [x0, x1] at `lineY` (the space between the drag handle and the mute
// switch). Returns the hovered control (0 = none, see headerClick).
int drawHeader(App& a, ImDrawList* dl, int track, float x0, float x1, float lineY, bool hovHead, bool roomy);
// A click on a control drawHeader returned. The input control opens the
// popup "##trackinput" (draw it with inputPopup in the same window).
void headerClick(App& a, int track, int control);
void inputPopup(App& a, int track);
// Glyph before the track name (audio: a wave, instrument: keys). Returns the
// width it takes (0 for ordinary tracks).
float drawNameGlyph(App& a, ImDrawList* dl, int track, ImVec2 pos);
// Status bar hint for a hovered header of a track with a mode ("" = none).
const char* headerHint(App& a, int track, int control);
// Mixer track menu / channel menu items.
void mixerMenuItems(App& a, ObjectId mixer);
void channelMenuItems(App& a, ObjectId channel);
// Screenshot set-ups (GRIDLINE_PL08_DEMO / GRIDLINE_MASTER_DEMO), once per run.
void devDemo(App& a);

}  // namespace trackui
}  // namespace gl
