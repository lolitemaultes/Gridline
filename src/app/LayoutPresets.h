// Gridline - window layout presets in the interface (K-79, core/LayoutPreset.h):
// which preset is on screen, choosing one (welcome screen, Settings >
// Interface, View > Layout), the shortcut set that goes with it, and the
// behaviour the presets add to the existing windows (the logic preset's
// exclusive bottom and right panes, the live preset's main view
// switch).
#pragma once
#include <string>
#include "../../third_party/imgui/imgui.h"
#include "../core/LayoutPreset.h"
#include "../core/MenuStyle.h"

namespace gl {
class App;
}

namespace gl::layoutui {

// The preset whose arrangement is on screen (main.cpp keeps it current).
layout::Preset active();
void setActive(layout::Preset p);
// --layout <id>: this run shows the preset (with its keys and menus) and saves none of it.
void setDevOverride(layout::Preset p);
bool hasDevOverride();
// The menu bar style in use (ui.menuStyle, or the dev override).
layout::Preset menuStyle(const App& a);

// Choose a preset: the settings and shortcut set change at once (never the theme), the window
// arrangement at the start of the next frame (main.cpp: takeSwitch).
void choose(App& a, layout::Preset p, const layout::Linked& linked);
bool takeSwitch(layout::Preset* out);

// The shortcut set of the settings (or the override) with its own overrides loaded.
void applyKeyProfile(App& a);
// The current set's overrides into its own settings key.
void saveKeymap(App& a);

// View > Window layout: the presets (each with its glyph, the one in use checked) and the layout commands.
void viewLayoutMenu(App& a);

// ---- the chooser (K-130): one card component for the welcome screen and Settings > Interface ----
// A faithful miniature of a preset's window: its control bar, panes and status bar drawn from the
// same geometry the dock builder uses (core/LayoutPreset.h, core/Workspace.h), with believable
// content (clips, mixer strips, step buttons, notes, list rows) in the theme's own colours.
void drawPreview(ImDrawList* dl, ImVec2 p0, ImVec2 p1, layout::Preset preset);
// The preset as a small line glyph (menus, the welcome screen's rail): `size` px wide, in `col`.
void drawGlyph(ImDrawList* dl, ImVec2 centre, float size, layout::Preset preset, ImU32 col);
// Card metrics for `count` cards across `width` (fewer columns, more rows when they do not fit).
struct CardGrid {
  int cols = 1;
  float cardW = 0, cardH = 0, thumbH = 0, gap = 0;
  float height(int count) const;
  float width(int count) const;
};
CardGrid cardGrid(float width, int count, float maxCardW, float minCardW);
// One card at `pos` (the miniature, name, preview badge and one line; the layout in use has the
// accent ring and a check badge; `focused` + `keyNav` draws the keyboard ring). True when clicked.
bool card(App& a, int index, ImVec2 pos, const CardGrid& g, bool focused, bool keyNav);
// Every preset's card from the cursor, at most `maxCardW` wide each. A click chooses the preset
// at once (with the options below). Returns the preset index picked this frame, or -1.
int cards(App& a, float width, float maxCardW, int focus = -1, bool keyNav = false);
// The two options of a choice (matching shortcuts, matching menus) as switches with a hint each,
// side by side when `width` has room.
void linkedOptions(App& a, float width);
// Dev report: the cards drawn on the last frame ("layoutcard <i> <id> x= y= w= h= in=<window>")
// and the layout in use and in the settings.
std::string devCards(const App& a);

// The windows a preset opens with (its default arrangement, the first time it is built).
void showPresetWindows(App& a, layout::Preset p);
// After a switch: the open windows stay open; only what the preset itself needs is added
// (the live layout's pads view behind the playlist).
void enterPreset(App& a, layout::Preset p);
// A short fade over the main window after a switch (call before ImGui::Render).
void noteSwitched(double now);
void drawSwitchFade(double now);
// Dev check (--switch-check): every open main window is drawn this frame, or is a docked tab
// behind another one, at a usable size inside the main window. "" when so, else what is wrong.
std::string switchCheck(App& a);
// The open main windows, as short ids ("playlist,mixer,..."), for the same check.
std::string openWindows(App& a);
// Per frame, before the windows draw: the logic preset keeps one window open in each pane
// group (bottom: mixer, piano roll, channel rack; right: undo history), with the mixer and
// the editors keeping separate pane heights.
void paneGroupsFrame(App& a);
// view.editors: the editors in the bottom pane on / off (the last one used comes back).
void toggleEditors(App& a);
// view.toggleMain: the main view between the playlist and the live pads.
void toggleMainView(App& a);
// K-124 view.backToPlaylist: the playlist shown, in front and focused; docked in a pane, its tab is
// the active one (the live pads step back behind it in the Live layout).
void showPlaylist(App& a);

}  // namespace gl::layoutui

namespace gl {
// MenuStyles.cpp: the items of one menu of a menu bar style.
void drawStyleMenu(App& a, const menustyle::Menu& m);
}  // namespace gl
