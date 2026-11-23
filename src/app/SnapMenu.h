// Gridline — the snap choices every editor offers (GitHub #58): one list, in one order, with
// the same groups in the piano roll (each layout's), the playlist, the automation editor and
// the Tracker's roll page, so a grid picked in one reads the same in the others.
#pragma once
#include <string>
#include "../core/Timeline.h"

namespace gl::snapmenu {

// The snap's name in the interface language ("1/5 beat"); `smart` names Line "Smart" (the calm
// toolbars call the zoom-following grid that).
const char* label(Snap s, bool smart = false);
// A short line on what the snap does, for a hover hint ("Five notes to a beat (quintuplets)").
const char* hint(Snap s);

// Draws the choices as selectable rows (inside a combo) or menu items (inside a menu), in
// groups: the zoom-following ones, step fractions, beat fractions, dotted values, tuplets.
// `offerMain` adds "Main" (follow the main snap), which the main snap itself leaves out.
// Returns true and sets `cur` when one was picked.
bool items(Snap& cur, bool offerMain, bool asMenuItems, bool smart = false);

// K-255: the editor drawing the next menus edits a row on its own grid: "Row grid" is offered,
// named with `text` ("Row grid: 5 per beat"). Empty: not offered (call it again after the menus).
void offerRowGrid(const std::string& text);
bool rowGridOffered();

// The editors' Main follows the playlist's snap: the app calls this once a frame.
inline void publishMain(Snap playlistSnap) { setMainSnap(playlistSnap); }

}  // namespace gl::snapmenu
