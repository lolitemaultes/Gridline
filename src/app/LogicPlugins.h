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
#include <set>
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
// Checkpoint 47 (R07-10): Join for audio regions that are not pieces of one file, or not end to end
// (the reference's Join): the regions of one track are mixed, at their own gain and fades but before
// the strip (unity, centre, no effects; gaps silent), into a new file in `dir`. One region replaces
// them on that track, named after the first, playing through the first one's strip. kNoId: nothing.
ObjectId joinAudioRegions(Song& s, const std::set<ObjectId>& clips, const std::string& dir, double sampleRate);

// K-112B (checkpoint 46): automation-only tracks as lanes. The reference has no automation regions:
// a parameter's automation is a curve on the track it belongs to. In this layout only, and as a
// view (the project and the other layouts are unchanged), a track holding nothing but automation
// regions whose parameters each belong to one track takes no room: its curves show on that track's
// automation lane (laneClip is true for them, so they are never regions here). The automation-only
// tracks whose parameters belong to no single track (a bus several tracks play into) stay together
// in one compact Automation folder row, closed by default (ui.logicAutoFolderOpen opens it: its
// tracks show again as rows of regions). ui.logicAutoAsLanes (default on) turns the view off.
struct AutoFold {
  bool on = false;
  bool open = false;                 // the folder is open
  std::vector<char> hidden;          // per track: an automation-only track shown on its targets' lanes
  std::vector<char> member;          // per track: in the Automation folder
  std::vector<int> folder;           // the folder's tracks, in order (the first one is its row)
  std::vector<int> clipTarget;       // per playlist clip: the track its curve shows on (hidden tracks), else -1
  std::vector<ObjectId> busStrip;    // checkpoint 48: per track: the bus whose automation it shows (a bus track), else kNoId
  int head() const { return folder.empty() ? -1 : folder.front(); }
  bool isHidden(int t) const { return on && t >= 0 && t < static_cast<int>(hidden.size()) && hidden[static_cast<size_t>(t)]; }
  bool isBus(int t) const { return on && t >= 0 && t < static_cast<int>(busStrip.size()) && busStrip[static_cast<size_t>(t)] != kNoId; }
  bool isMember(int t) const { return on && t >= 0 && t < static_cast<int>(member.size()) && member[static_cast<size_t>(t)]; }
  // R09-01: a bus track and the automation-only tracks take no regions here (the reference's aux
  // track holds no MIDI or audio regions): only their automation.
  bool takesNoRegions(int t) const { return isBus(t) || isHidden(t) || isMember(t); }
  // The row takes no room: a hidden track, or a folder track under the closed folder's row.
  bool takesNoRoom(int t) const { return isHidden(t) || (isMember(t) && !open && t != head()); }
};
// Worked out once per document revision (and when the layout or the setting changes).
const AutoFold& autoFold(const App& a);
void setAutoFolderOpen(App& a, bool open);
// K-112B (checkpoint 46): the Project End marker: the end of the last region, rounded up to a bar
// line (bar 129 in an empty project). Without a cycle, playback stops there (recording runs on).
Tick projectEnd(const App& a);
// The one track a parameter owner (a sound, a mixer track) belongs to, else -1: the track that plays
// the sound or has it as its channel strip; else the one track whose sounds play into that mixer
// track. Automation-only tracks never count.
int ownerTrack(const Song& s, ObjectId owner);

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
// R08-07: before regions go (Delete, the Eraser, the region menu), a track that loses its last
// region keeps the sound they played (PlaylistTrack::sound), so its strip, its header controls and
// its automation lane stay, as the reference's track does. Only in this layout; call it inside the
// deletion's undo group.
void keepTrackSounds(App& a, const std::set<ObjectId>& clips);
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
