// Gridline - K-237: the workspace strip and menu (core/Workspaces.h).
//
// A slim strip under the top bar names the active layout's workspaces; a click (or Ctrl+1..9)
// switches. "+" adds one from the current arrangement, a double-click renames, a drag reorders,
// and the right-click menu duplicates, moves, saves, reverts and closes (with an Undo for a few
// seconds). The strip only shows once a layout has a second workspace, so the default look of
// every layout stays as it was. View > Workspaces has the same commands.
#pragma once
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"

namespace gl {
class App;
}

namespace gl::workspacesui {

// Draws the strip at `pos`, `width` wide; returns the height it took (0 when it does not show).
float drawStrip(App& a, ImVec2 pos, float width);
// The View > Workspaces submenu's items.
void menu(App& a);
// Per frame: the switch keys (view.workspace1 .. 9, next, previous).
void frameKeys(App& a);
// K-237: the opt-in copy of the workspaces in the project (Song::workspaceCopy). On: the project carries
// every layout's set, refreshed at each save; opening it gives a layout the copy's set only when the
// person has no workspaces of their own there (one, or none). Off: nothing is written.
bool projectCopyOn(const App& a);
void setProjectCopy(App& a, bool on);
void refreshProjectCopy(App& a);  // before a save
void projectOpened(App& a);       // after a project opened
// Dev checks (--dev-input "workspace add <name>|select <n>|close <n>|starters|dump").
void devCommand(App& a, const std::vector<std::string>& args);

}  // namespace gl::workspacesui
