// Gridline - K-112b: plugin windows in the Tracks layout (the Logic preset). Everything here acts
// only while that layout is on screen; the other layouts keep their plugin windows exactly as
// they are.
//
//   Link        Off (the default): each plugin stays in a window of its own, built-in ones too.
//               Single: one window, showing each plugin opened (an installed plugin's window
//               opening closes the others). Follow
//               (right-click): the windows follow the selected strip row by row; a built-in row
//               with nothing there stays open, empty, naming the track and slot; an installed
//               plugin's window closes and comes back with a strip that has one in that row.
//               A per-user setting (ui.logicPluginLink), never in the project.
//   Hide        View > Hide or show the plugin windows (V in the Tracks set): the windows go away
//               and come back where they were; opening a plugin shows them again. Installed
//               plugins' windows use the shared hide / show (wired at the merge with the Live
//               branch, a stub until then).
//   Compare     against the project as last saved (or opened): lit when the plugin's state
//               differs; a click shows the saved state, another click brings the edit back.
//               The Smart controls bar has one for the whole track (every plugin flips together).
//   Copy/paste  a plugin clipboard of its own (whole settings, same plugin type).
//   Controls    a plugin's window shows Gridline's plain parameter list instead of its editor
//               (per plugin type, per user).
#pragma once
#include <string>
#include <vector>
#include "../core/Types.h"

namespace gl {
class App;
struct MixerTrack;
struct PluginTarget;
struct PluginState;
struct Song;
struct Channel;
struct Clip;
}

namespace gl::logicplug {

// The Tracks layout is on screen.
bool active(const App& a);
// R04-01 / R04-05: an automation clip on a track that has an automation lane is the track's own
// automation (drawn in the lane, never a region): it is not selected, moved or deleted as a region.
bool laneClip(const Song& s, const Clip& c);

// The sound a track plays (its linked channel, else its regions' sound; `clip` first when it lies
// on the track), and the track the layout is about (the selected region's or track, else the
// selected channel's, else the first: a track is always selected, as in the reference).
const Channel* trackSound(const Song& s, int track, const Clip* clip);
// R05-13: the track's channel strip (the reference's track Volume, Pan, Mute and Solo): its own mixer
// track, else the mixer track its sounds play into; a kit whose sounds play into several mixer
// tracks gets the one they all send into (its bus). Never the master (nullptr then).
const MixerTrack* trackStrip(const Song& s, int track);
int focusTrack(const App& a);
// R06-06: Delete track, as the reference's: the track, its regions, and the channel strip and the
// sounds only it used (no other track or arrangement plays them, nothing sends into the strip),
// with their automation. One undo step (inside the caller's group when there is one).
bool deleteTrack(App& a, int track);
// R02-14: New track with the same settings (Cmd+D): a track right below `track` with its settings
// and, as the reference's, a channel strip of its own: a copy of the sound the track plays (its
// instrument and settings, no notes) on a mixer insert of its own carrying the same plugins. An
// audio track gets a free mixer track. `withContent` also copies its regions. One undo step.
// Returns the new track (-1 when none was made).
int newTrackSameSettings(App& a, int track, bool withContent);

// Link: 0 off, 1 single, 2 follow the selected strip.
int link(const App& a);
void setLink(App& a, int mode);
// Follow: the effect window's empty state (a row with nothing on the selected strip).
void drawEmptyRow(App& a);
// Off: the built-in effects opened before the one in the effect window, each in its own window.
void drawMoreFx(App& a);
// K-297: closes those windows (Close plugin windows, Close all windows: they stayed open).
void closeMoreFx();

// Per frame, before the plugin windows draw: the saved states, Link following, the hidden state.
void frame(App& a);

// V: hidden windows keep their plugins and come back with the next toggle.
bool hidden();
void toggleHidden(App& a);

// The effect window's title: the track's name (the plugin's name is in its header).
std::string fxWindowTitle(const Song& s, const MixerTrack& t);

// Header row parts (the plugin window's second row).
void compareButton(App& a, const PluginTarget& t);
// Compare for every plugin on a track at once (the Smart controls bar).
void compareTrackButton(App& a, const std::vector<PluginTarget>& ts, float width);
void copyPasteButtons(App& a, const PluginTarget& t);
void viewButton(App& a, const PluginTarget& t);  // size and Controls / Editor
void linkButton(App& a);

// Smart controls (a part homed in the lower pane): the selected track's main controls as knobs,
// grouped by plugin (a group's name opens its window), or its first EQ's own display; a Master
// switch shows the master track's instead. Mapped automatically (no saved mapping yet).
void smartControls(App& a);

// Controls view: the plugin window lists the parameters (built-in plugins).
bool controlsView(const App& a, const std::string& type);
void drawControls(App& a, const PluginTarget& t, float width);

}  // namespace gl::logicplug
