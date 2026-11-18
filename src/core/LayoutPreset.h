// Gridline - window layout presets (K-79). A per-user choice (settings.json,
// never a project) of how the main window is arranged: the default studio
// arrangement, a live arrangement (browser left, playlist / live pads
// as one main view with the mixer under it and the editors along the bottom)
// and a logic arrangement (browser left, playlist centre, mixer and editors
// sharing one bottom pane, the undo history on the right, two-row top bar).
//
// Choosing a preset can also switch the shortcut set and the menu bar style to
// the matching ones; each is a tick the user can clear. A preset never changes
// the appearance: the theme, colours, control style, fonts and interface size
// stay what the user chose (K-122). Every preset keeps its own ImGui layout file, so each
// remembers the user's own changes; the studio preset keeps `layout.ini`, so
// existing users see no change at all.
//
// Pure data and geometry (no UI): app/Shell.cpp turns the rectangles into
// dock splits, tests check them for every supported window size.
#pragma once
#include <string>
#include "Workspace.h"

namespace gl {
class Settings;
}

namespace gl::layout {

// K-112: Tracker is the fourth layout (choosable since 2026-11-16, a Preview like Clips and Smooth).
// The chooser, View > Window layout and the settings list the kPresetCount presets; per-layout
// tables (homes, regions, styles, key sets) cover kLayoutCount.
enum class Preset : int { Studio = 0, Live = 1, Logic = 2, Tracker = 3, Count = 4 };
constexpr int kPresetCount = 4;  // the layouts a person can choose
constexpr int kLayoutCount = static_cast<int>(Preset::Count);

// Stable id stored in the settings: "studio", "live", "logic", "tracker".
const char* id(Preset p);
// Unknown or empty text = Studio. The working names of the research notes
// ("performance", "tracks") are read as Live and Logic.
Preset fromId(const std::string& text);
// The name shown in the interface. The ONE place the display names live
// (they are not final yet), every menu, page and card reads them here.
const char* displayName(Preset p);
// One line describing the arrangement (chooser cards and settings).
const char* blurb(Preset p);
// K-112: whether the layout may be shown: every preset (Tracker too, since 2026-11-16).
bool available(Preset p, const Settings& s);
// K-112: whether ui.layoutDev counts: only in a development build (GRIDLINE_DEV_LAYOUTS); a release
// build ignores the setting. It switches on developer commands only, never what people see (K-329).
bool devSetting(const Settings& s);
// --layout-dev for this run: counts as ui.layoutDev (development builds only).
void setDevRun(bool on);
// The layout to show for the settings: ui.layout when available, else Studio.
Preset shown(const Settings& s);

// ImGui layout file of the preset inside the config folder.
std::string iniFileName(Preset p);
// Settings key of the default-layout version the preset's file was built
// with ("ui.layoutVersion" for Studio, as before), and the current version.
std::string versionKey(Preset p);
int defaultVersion(Preset p);
// Settings key of the user's shortcut overrides for the shortcut set.
std::string bindingsKey(Preset p);

// What choosing a preset also switches (the chooser's ticks).
struct Linked {
  bool keys = true, menus = true;
};
Linked linkedFromSettings(const Settings& s);

Preset current(const Settings& s);      // ui.layout
Preset keyProfile(const Settings& s);   // keys.profile
Preset menuStyle(const Settings& s);    // ui.menuStyle

// Sets ui.layout to `p` and, for each ticked item, the matching shortcut
// set and menu style. No theme or colour setting is touched. The ticks are
// remembered for next time.
void choose(Settings& s, Preset p, const Linked& linked);
// Repairs the layout keys of a settings file written by an older or newer
// Gridline (unknown values -> "studio"; working names -> their preset).
// Returns true when something changed.
bool normalize(Settings& s);

// ---- geometry (dock space of w x h logical pixels, origin 0,0) ----

// Live: bottom panel across the full width, browser left of the rest,
// the main view (playlist / live pads) over the mixer.
struct LiveLayout {
  workspace::Rect area, browser, main, mixer, bottom;
  float bottomFrac = 0;   // split of the root (Down)
  float browserFrac = 0;  // split of the top part (Left)
  float mixerFrac = 0;    // split of the rest (Down)
};
LiveLayout liveLayout(float w, float h);

// Logic: browser left, a right column, and the centre column split into the
// playlist over one bottom pane shared by the mixer and the editors.
struct LogicLayout {
  workspace::Rect area, browser, main, bottom, right;
  float browserFrac = 0;  // split of the root (Left)
  float rightFrac = 0;    // split of the rest (Right)
  float bottomFrac = 0;   // split of the centre (Down)
};
LogicLayout logicLayout(float w, float h);

}  // namespace gl::layout
