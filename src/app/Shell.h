// Gridline — pieces of the main window shared by the application (main.cpp)
// and the plugin build (PH-09).
#pragma once
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../core/LayoutPreset.h"
#include "../core/Workspace.h"

struct ImGuiDockNode;

namespace gl::shell {

// Tile mark coverage, size*size alpha values (supersampled).
std::vector<unsigned char> markCoverage(int size);
// Mark texture (white + coverage alpha) of `px` pixels, cached per size, for
// theme::setMarkTextureProvider. Needs a current GL context (shared between
// all windows that use it).
ImTextureID markTexture(int px);
// FL-style default workspace in `dockspace`; returns the floating rack rectangle.
// `alsoLeft`: more windows docked as tabs beside the browser (plugin build: "Host link").
workspace::Rect buildDefaultLayout(ImGuiID dockspace, ImVec2 size, int channels, const char* alsoLeft = nullptr);
// K-79: the default dock tree of a layout preset (Studio = buildDefaultLayout). Returns the
// floating rack rectangle for Studio; the other presets dock the rack (empty rectangle).
workspace::Rect buildLayout(layout::Preset preset, ImGuiID dockspace, ImVec2 size, int channels, const char* alsoLeft = nullptr);
// Dock node ids of the last build (0 when the preset has no such node). The main view is the
// central node; the bottom pane hosts the editors (and in Logic the mixer).
struct BuiltNodes {
  ImGuiID main = 0, bottom = 0, right = 0, mixer = 0, left = 0;
};
const BuiltNodes& builtNodes();
// (J-117: floating windows are kept inside the work area by ui::ClampTrackedWindows, before
// they are laid out; the rack's old clamp after it was drawn moved it away from its title bar.)
// Side panels scale with the dock space and the central node keeps half of every split.
void fitDockTree(ImGuiDockNode* node, ImVec2 scale, ImVec2 avail);

}  // namespace gl::shell
