// Gridline — pieces of the main window shared by the application (main.cpp)
// and the plugin build (PH-09).
#pragma once
#include <vector>
#include "../../third_party/imgui/imgui.h"
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
// A floating window keeps its title bar inside the dock space.
void keepFloatingInView(const char* name, ImVec2 pos, ImVec2 size);
// Side panels scale with the dock space and the central node keeps half of every split.
void fitDockTree(ImGuiDockNode* node, ImVec2 scale, ImVec2 avail);

}  // namespace gl::shell
