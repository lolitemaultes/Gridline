// Gridline - K-12: the playlist track "+". Hovering a track header shows a
// small "+" left of the mute switch, on the switch's line (every track
// height); its menu gives the track an instrument (a favourite, any
// instrument, or a rack channel) and starts patterns for it on the track,
// opened in the piano roll. Shown when View > "Track + button" is on
// (setting ui.playlistTrackPlus, default on). Edits: core/TrackPlus.
#pragma once
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../core/Types.h"

namespace gl {

class App;
struct Song;

namespace trackplusui {

// The View options switch.
bool enabled(const App& a);
void setEnabled(App& a, bool on);
// Whether a track header gets the "+" (not on audio tracks or locked tracks).
bool offeredOn(const Song& s, int track);
// Width the "+" takes on the header line (button and gap), logical px.
constexpr float kSlotW = 18.0f;
// Half the button's side.
constexpr float kHalf = 6.0f;

// Draws the "+" of `track` centred at `c` with its hover fade `alpha` (0..1;
// it is not drawn at 0). `hot`: the pointer is on it.
void drawButton(App& a, ImDrawList* dl, int track, ImVec2 c, float alpha, bool hot);
// Opens the menu for `track` (call in the playlist window). `at`: the tick a
// new pattern goes to; `anchor`: the menu's top left corner.
void open(App& a, int track, Tick at, ImVec2 anchor);
// Track whose menu is open (-1 = none).
int menuTrack();
// The menu (call every frame in the playlist window, after open()).
void menu(App& a);

// Dev geometry (--dev-input plgeom, "tplus" lines): the "+" drawn last frame
// on the hovered header and what sits beside it, and the open menu's rows.
struct DevGeom {
  bool on = false;      // the View switch
  int track = -1;       // header showing the "+" (-1 = none)
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;            // the button
  float nameX0 = 0, nameX1 = 0, nameY0 = 0, nameY1 = 0;  // the name's box (0 0: not drawn)
  float handleX0 = 0, handleX1 = 0;                 // the "..." handle's span on the line
  float ledX0 = 0, ledX1 = 0;                       // the mute switch's span
  float ctlX0 = 0, ctlX1 = 0;                       // the mode controls' span (audio / instrument row)
  float iconX1 = 0;                                 // right end of the track icon / glyph (0: none)
  struct Row {
    std::string key;
    float x, y;
  };
  std::vector<Row> rows;  // menu rows of the open menu (and its open submenu)
  int menu = -1;          // track whose menu is open
};
DevGeom& devGeom();

}  // namespace trackplusui
}  // namespace gl
