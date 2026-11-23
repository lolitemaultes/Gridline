// Gridline - the Studio layout's dock tree as a workspace keeps it (core/DockPlan.h).
//
// capture() reads the live dock tree (splits and their shares, each pane's windows in tab order and the one
// in front, the main area) and the floating windows; request() asks main.cpp's next dock build to make the
// tree from a plan instead of Studio's default (no dock file is loaded); the floating windows are placed as
// they appear, and each pane's front window comes forward.
#pragma once
#include <string>
#include "../../third_party/imgui/imgui.h"

namespace gl {
class App;
}

namespace gl::dockplanui {

// The Studio dock tree and floating windows now, as dockplan::toJson text ("" outside Studio or before the
// first build).
std::string capture(App& a);
// The next dock build (this frame or the next) makes this plan. "" does nothing.
void request(App& a, const std::string& planText);
// main.cpp, in the dock build: true (and the plan built into `dockspace` of `size`) when a plan was asked for.
bool buildPending(App& a, ImGuiID dockspace, ImVec2 size);
// Per frame, after the windows drew: floating windows of the last plan placed as they appear, fronts set.
void frame(App& a);
// Dev: "workspace dockcheck": the tree now against the last plan built (similar, or what differs).
std::string devCheck(App& a);

}  // namespace gl::dockplanui
