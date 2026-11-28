// Gridline — K-450 plugin icons: every built-in plugin's own icon, drawn wherever the
// plugin is shown (browser, picker, channel rack, plugin windows, mixer slots, the Clips
// device band and chain, Smooth, the Tracker, the command palette, the plugin menus, drag previews).
//
// The icon is the plugin's Tile: a rounded square in its family colour with its own
// pictogram cut out, the same drawing as the tile of the editor's wordmark
// (tools/wordmarks/make_wordmarks.py writes both from motifs.py and plugins.py). Its SVG is
// assets/icons/ui/plugin-<id>.svg for type gl.<id>, rasterised by the icon atlas at the exact
// device size (crisp tile edges, pictogram strokes on the icon set's stroke rule).
// Hosted plugins (VST3, CLAP, AU, ...) have no Gridline icon: callers draw their generic one.
#pragma once
#include <string>
#include "../../third_party/imgui/imgui.h"

namespace gl::ui {

// The surfaces that show plugin icons (counted per frame for the tests).
enum class PluginIconSurface {
  Browser, Picker, Rack, WindowTitle, MixerSlot, DeviceBand, Smooth, Tracker, Palette, Drag, Menu, Count
};
const char* pluginIconSurfaceName(PluginIconSurface s);

// File stem of the type's icon ("plugin-<id>"), nullptr when it has none (hosted plugins,
// unknown types).
const char* pluginIconName(const std::string& type);
inline bool hasPluginIcon(const std::string& type) { return pluginIconName(type) != nullptr; }
// The icon's tile colour on the current theme (its family colour, deepened on the light
// faces as every lit role); 0 when the type has none.
ImU32 pluginIconColour(const std::string& type);
// Draws the type's icon: a tile `size` logical px square centred at `c` (snapped to whole
// device pixels). `alpha` fades it (a bypassed slot); `tint` (not 0) draws the tile in that
// colour instead of the family colour (an icon on a coloured pill: the pill's text colour, the
// pictogram cut through to the pill). False, and nothing drawn, when the type has no icon: the
// caller then draws its generic glyph and should call notePluginIconFallback.
bool drawPluginIcon(ImDrawList* dl, const std::string& type, ImVec2 c, float size, PluginIconSurface s, float alpha = 1.0f, ImU32 tint = 0);
// Records that `s` drew a generic glyph for `type` (a hosted plugin). A built-in type that has
// its own icon counts as a mistake (pluginIconMistakes).
void notePluginIconFallback(const std::string& type, PluginIconSurface s);

// Plugin windows (an instrument's settings, an effect's editor): `title` with room before it for
// the plugin's icon when the type has one (leading spaces; the window's ### id is unchanged), and,
// right after that window's Begin, the icon drawn in that room on its title bar or, docked, on
// its tab.
std::string pluginWindowTitle(const std::string& type, const std::string& title);
void drawPluginWindowTitleIcon(const std::string& type);

// A text button (ui::TextButton) naming a plugin, with the plugin's icon in front of its name
// (the Device chain's chips). `id` is the button's "##..." id suffix.
bool pluginTextButton(const std::string& type, const std::string& name, const char* id, bool active, const char* hint, PluginIconSurface s);

// Per-surface counts since the last reset (dev command "plugicons", tests).
struct PluginIconCounts {
  int own[static_cast<int>(PluginIconSurface::Count)] = {};
  int generic[static_cast<int>(PluginIconSurface::Count)] = {};
  int mistakes[static_cast<int>(PluginIconSurface::Count)] = {};  // generic glyph for a type with its own icon
};
const PluginIconCounts& pluginIconCounts();
void resetPluginIconCounts();

}  // namespace gl::ui
