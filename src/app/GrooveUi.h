// Gridline — K-151: swing presets in the interface (core/Groove.h): the preset
// chip beside the channel rack's Swing knob, the preset menu, the Groove
// editor window, saving / importing / exporting presets and taking a groove
// from a pattern or an audio clip. Every change to the song is one undo step.
#pragma once
#include <string>
#include <vector>
#include "../core/Groove.h"

namespace gl {

class App;
class TempoMap;

namespace grooveui {

// Factory and user presets (the user's re-read every few seconds and after a save).
const std::vector<groove::Preset>& presets(App& a);
void refresh();
// The preset's swing amount and groove, one undo step.
void apply(App& a, const groove::Preset& p);
// The same at the start of the next frame (App::later): for menus drawn inside windows that keep
// pointers into the song while they draw (the channel rack's chip).
void applyLater(App& a, const groove::Preset& p);
// "Swing piano roll notes" switched at the start of the next frame (one undo step).
void toggleSwingNotesLater(App& a);
// The preset list and actions, inside an open popup or menu.
void presetMenuItems(App& a);
// The chip itself (a combo of `width`), at the cursor.
void chip(App& a, float width);
void savePrompt(App& a);
void importDialog(App& a);
void exportDialog(App& a);
// A groove taken from the notes and steps of `channel` in `pattern`, applied (one undo step).
bool fromPattern(App& a, ObjectId pattern, ObjectId channel);
// A groove taken from an audio clip's transients, applied (one undo step).
bool fromClip(App& a, ObjectId clip, const TempoMap& tm);
// The song's groove applied to an audio clip through warp markers (one undo step).
bool swingClip(App& a, ObjectId clip, const TempoMap& tm, double strength = 1.0);
// The Groove editor window (App::showGroove).
void drawEditor(App& a);
// Dev check ("groove" in --dev-input): the song's swing and where the chip, menu rows, lanes and buttons were drawn.
void devCommand(App& a, int frame);

}  // namespace grooveui
}  // namespace gl
