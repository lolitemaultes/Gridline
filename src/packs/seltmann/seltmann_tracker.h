// Gridline - "seltmann" pack: Seltmann's own screens in the Tracker layout (0.7.8, K-347, the
// "Sampler twin"). When a Seltmann channel is the current instrument in the Tracker, the lower band
// shows these screens instead of the Channel frame, the way it shows the Tracker Sampler's:
//   Chop        (where the Sample editor sits) the waveform with its lettered regions and the keys
//               that play them, the sequence as rows at the right, and under them the band's buttons:
//               play, chop sizes, the tool, the region edits, the region's or step's values, drags and
//               sends, tempo and follow, the way out;
//   Instrument  (where the Instrument editor sits) Sound, Layouts and Grid boxes side by side and the
//               key map on a piano along the bottom;
//   Chop page   a page beside the Tracker's other pages: the layout library, the waveform with the
//               sequence on the same time line, the region's values, the key map, the sequence rows,
//               takes and loops, all on one page.
// Drawn in the Tracker's own look (TrackerLook.h). Every edit is Seltmann's own model action
// (seltmann_ui.h: commit, applyLayout, setChopSize, splitRegionUi, sendToProject, useTake ...) or a
// parameter edit: the same undo steps, saved bytes and collaboration ops as the Ribbon in Studio,
// Clips and Smooth. Which screen shows, the region and step cursor, the tool, the layout cursor and
// the scroll are view state, never saved. Tracker only: Studio, Clips and Smooth keep the Ribbon.
// UI thread only.
#pragma once
#include <string>
#include <vector>
#include "../../core/Types.h"
#include "../../../third_party/imgui/imgui.h"

namespace gl {
class App;
}

namespace gl::packs::seltmann::tracker {

enum Screen { ScreenChop = 0, ScreenInstrument = 1 };

// The channel plays Seltmann (gl.seltmann).
bool isSeltmann(App& a, ObjectId channel);
// The band's screens in [p0, p1] (the band's window is current). `keys`: the band has the keyboard.
void chopScreen(App& a, ObjectId channel, ImVec2 p0, ImVec2 p1, bool keys);
void instrumentScreen(App& a, ObjectId channel, ImVec2 p0, ImVec2 p1, bool keys);
// The height a screen wants at this font and work area (the band's rule: as the Sampler's screens).
float need(int screen, float fontSize, float workH);

// Keys (SDL scancodes, keycodes and modifiers, as main.cpp hands them to the band): true when the
// screen takes this key (it is then queued and run on the next draw); the note keys are not among
// them (the band plays the channel on them, so they play the regions).
bool takesKey(int screen, int scancode, int keycode, int mods);
void queueKey(int scancode, int keycode, int mods);
// Esc and Cmd/Ctrl+W: an open box closes first. True when one was open (the screen stays).
bool closeInner(App& a, ObjectId channel);
// The band closes or another screen opens: the auditions and the preview stop.
void leave(App& a, ObjectId channel);

// The Chop page (the "Chop" window's body in tracker.pages): the current instrument when it is a
// Seltmann, else the project's first.
void chopPage(App& a);
// The page tab shows while the project has a Seltmann channel.
bool projectHasSeltmann(App& a);
// The region letter a note of a Seltmann channel plays (the pattern's cells, K-347 T7): "A".."Z",
// "AA".., the pattern octave "P" (its own key) or "+1".."+11"; "" for a key that plays nothing.
std::string noteLabel(App& a, ObjectId channel, int midiNote);

// Dev (--dev-input "tracker editors selt ..."):
//   dump               the screen, the regions and steps, the cursor, the tool, the undo position, the
//                      layout cursor and the controls drawn (name and centre) as one line each
//   at <control>       the pointer to that control's centre (then "sdldown 0" / "sdlup 0")
//   region <i> · step <i> · tool play|cut|grid · group <g> · layout <name> (the list cursor on it)
void devCommand(App& a, const std::vector<std::string>& args, int frame);
// The point "at" asked for (taken once by the band's dev pointer).
bool takeDevPointer(float* x, float* y);

}  // namespace gl::packs::seltmann::tracker
