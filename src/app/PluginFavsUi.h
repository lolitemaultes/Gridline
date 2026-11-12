// Gridline: K-16: favourite (starred) plugins in the pickers. One list per
// user (host::PluginPrefs favourites, keyed by plugin type), toggled from any
// picker, the browser or the plugin picker (F8) and in sync at once.
#pragma once
#include <functional>
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../core/PluginInfo.h"

namespace gl::favui {

// One plugin row of a picker list or menu: the name, a dim detail (format /
// category) and a star at the right end. Hovering the row shows the star, a
// favourite keeps it lit; a click on the star toggles the favourite and keeps
// the list open, a click elsewhere picks the plugin (the popups close).
// Returns true when picked.
bool pickerRow(const PluginInfo& p, bool current, const char* detail = nullptr);

// The plugins of `kind` a picker lists besides the built-ins: installed
// (hosted) plugins the browser lists (enabled formats, one format per plugin,
// none in a collaborative project), by name.
std::vector<const PluginInfo*> installedOfKind(PluginKind kind);
// The favourites of `kind` (built-in and installed), by name.
std::vector<const PluginInfo*> favouritesOfKind(PluginKind kind);

// Menu body of a plugin picker (Insert / Replace effect, Add / Replace
// instrument): the favourites first (their own group at the top), then one
// submenu per built-in category, then the installed plugins. Every row has the
// star toggle.
void pluginMenu(PluginKind kind, const std::function<void(const std::string&)>& pick, const std::string& current);
// The search field at the top of a picker (pluginMenu and the mixer's effect list): while it holds
// text it lists the matching plugins of `kind`, built-in and installed, best first (Enter picks the
// first) and returns true; the caller then skips its own rows. Empty when the picker opens.
bool searchField(PluginKind kind, const std::function<void(const std::string&)>& pick, const std::string& current);
// Dev check: the last search field drawn ("plugsearch frame F x0 .. y0 .. x1 .. y1 .. active A rows Y",
// Y the top of the picker's own first row below it), so tests hover real rows, not fixed offsets.
std::string devSearchReport();

// The star glyph of a row at `center` (the in-house five-point star).
void drawRowStar(ImDrawList* dl, ImVec2 center, bool favourite, bool rowHovered, bool starHovered);

}  // namespace gl::favui
