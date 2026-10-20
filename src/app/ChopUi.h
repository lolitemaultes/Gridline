// Gridline — playlist chop / stutter / region editing UI (PL-13). The "Chop
// and stutter" window (slice at the grid, into equal parts or at transients;
// stutter with rate, decay, pitch and speed ramps, reverse and gate; chop
// presets) previews its result live in the playlist (one undo step that is
// redone on every change) and keeps it on Apply. Menus: the clip menus'
// "Chop" submenu and the playlist's time selection items. Operations live
// in engine/Chop.h.
#pragma once
#include "../core/Types.h"

namespace gl {

class App;

namespace chopui {

// Opens the window on the current target: the time selection (its clips on
// the selected clips' tracks, else on every track), else the selected clips
// (or `clip`). mode: -1 keep, 0 slice, 1 stutter; preset: -1 keep, else a chop::Preset.
void open(App& a, ObjectId clip = kNoId, int mode = -1, int preset = -1);
bool isOpen();
void draw(App& a);  // every frame (after the playlist)
// "Chop" submenu for a clip's context menu (the clip and the selection).
void clipMenu(App& a, ObjectId clip);
void clipMenuItems(App& a, ObjectId clip);  // the submenu's contents
// Time selection menu items (duplicate, insert / delete space, consolidate, chop it, stutter).
void timeSelectionItems(App& a);
// Playlist shortcuts (call while the playlist has focus).
void shortcuts(App& a);
// Region actions shared by menus and shortcuts.
void duplicateTime(App& a);
void insertSpace(App& a);
void deleteSpace(App& a);
void consolidateTime(App& a);
void chopTimeToSlicer(App& a, Tick grid);
void sliceSelectedAtGrid(App& a, Tick grid);
// GRIDLINE_PL13_DEMO=stutter|slice|chop: screenshot set-ups (once per run).
void devDemo(App& a);

}  // namespace chopui
}  // namespace gl
