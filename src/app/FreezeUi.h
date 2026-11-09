// Gridline - K-87: freezing and hibernating channels and mixer tracks in the app (engine/Freeze.h).
//
//   freeze     the unit's sound is rendered once (a worker, with progress on its marker) into
//              <project>/Frozen/ and its plugins (and those that play only into a frozen track)
//              unload; fader, pan and sends stay live. One undo step each: freeze, unfreeze,
//              a refreeze asked for. An edit the render depends on marks it out of date: it plays
//              live and is rendered again a second after the last edit (an option keeps the old
//              audio, marked amber, instead). In a shared project only the computer that froze
//              it renders again by itself. An edit of a frozen plugin asks "Unfreeze to edit?".
//   hibernate  the unit's plugins unload without a render, on this computer only (not an undo
//              step, never shared). It wakes when its editor opens, a little before the song
//              plays its notes (also while stopped, near the song position), when it is the
//              selected channel or is played by hand, and in pattern or live mode; it sleeps
//              again after a while without any of that (a setting; never is allowed).
// A frozen unit played in pattern mode, in live mode or by hand plays live the same way.
#pragma once
#include <string>
#include <vector>
#include "../core/Song.h"
#include "../engine/Freeze.h"

struct ImDrawList;
struct ImVec2;

namespace gl {
class App;
struct PluginTarget;
}

namespace gl::freezeui {

using freeze::Unit;

// Per frame (App::tick): render jobs, out-of-date checks and refreezes, wake and sleep, the
// guard on frozen plugins' editors and edits.
void tick(App& a);
// App::syncEngine: the song the program is built from (frozen units play their renders, plugins
// left out are not loaded); hands the decoded renders to a.builder.
Song playSong(App& a, const Song& s);

// ---- actions ----
void freezeUnits(App& a, const std::vector<Unit>& units);
void unfreezeUnits(App& a, const std::vector<Unit>& units);
void refreezeUnits(App& a, const std::vector<Unit>& units);
void setHibernate(App& a, const std::vector<Unit>& units, bool on);
void unfreezeAll(App& a);
int hibernateUnused(App& a);  // how many units went to sleep
void wakeAll(App& a);         // every hibernating unit stops hibernating
// A plugin editor is about to open (hosted window or Gridline's panel): a hibernating unit wakes
// at once; a frozen one asks to be unfrozen and false is returned.
bool beforeEditorOpens(App& a, const PluginTarget& t);

// ---- state for the interface ----
bool rendering(const App& a, Unit u, float* progress = nullptr);
bool outOfDate(const App& a, Unit u);
bool asleep(const App& a, Unit u);   // hibernating and not woken now
bool coveredNow(const App& a, Unit u);  // its plugins are left out for a frozen track
std::string statusText(const App& a, Unit u);  // "" = nothing to say
// The playlist track's linked unit (instrument track: its channel; audio track: its mixer track).
bool trackUnit(const Song& s, int track, Unit& out);

// Marker (snowflake / moon, amber dot when out of date, progress ring while rendering), centred
// on `c` in a `size` box. Returns false (nothing drawn) when the unit has nothing to show.
bool drawMarker(App& a, ImDrawList* dl, Unit u, ImVec2 c, float size);
bool hasMarker(const App& a, Unit u);
// Covered units (and asleep ones) draw their row at half strength: a shade over [p0, p1].
void drawShade(App& a, ImDrawList* dl, Unit u, ImVec2 p0, ImVec2 p1);

// ---- menus (K-77 places) ----
void channelMenuItems(App& a, ObjectId channel);                  // before "Render to sampled instrument..."
void rackMenuItems(App& a, const std::vector<ObjectId>& channels);  // after "Route selected to free mixer tracks"
void mixerMenuItems(App& a, const std::vector<ObjectId>& tracks);   // after "Disable track"
void trackMenuItems(App& a, int track);                           // after "Lock"
void toolsMacroItems(App& a);                                     // Tools > Macros
void optionsItems(App& a);                                        // Options, after delay compensation
void exportItems(App& a);                                         // File > Export, after the zipped project
// Zipped project writers: the song as it goes in the zip ("Leave out frozen audio": unfrozen).
Song zipSong(const App& a, Song s);

// dev-input "freeze ...": "state" prints every unit's state; "freeze <c|m> <index>",
// "unfreeze <c|m> <index>", "hibernate <c|m> <index> <0|1>", "wait" (until no render runs).
bool devCommand(App& a, const std::vector<std::string>& args);

}  // namespace gl::freezeui
