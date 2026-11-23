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
#include "../core/LayoutPreset.h"

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
// K-256: the active layout's workspaces replaced by the project's copy of them (false: it keeps none).
bool importProjectCopy(App& a);
// ---- K-256: what a workspace keeps, the drawer and the settings page ----
// The active workspace's own control bar and pins (toolbar::barJson / pinsJson text, "" = the layout's).
std::string activeOwnBar(App& a, layout::Preset p);
std::string activeOwnPins(App& a, layout::Preset p);
// Sets them (nullptr leaves one as it is; "" = back to the layout's).
void setActiveOwn(App& a, layout::Preset p, const std::string* bar, const std::string* pins);

struct Info {
  std::string name, key;
  bool active = false, changed = false;
  std::vector<std::string> open;  // the windows it opens
  bool keepWindows = true, keepPanes = true, ownBar = false, ownPins = false;
  std::string regionState, dock;  // for its miniature
};
// A layout's workspaces (the active layout's with today's arrangement when it has only the first).
std::vector<Info> list(App& a, layout::Preset p);
// The active layout's workspaces: switch, close (with its Undo), add from now, starters, rename, duplicate,
// move, save now, revert, auto-save.
void select(App& a, int i);
void close(App& a, int i);
void addFromNow(App& a);
void starters(App& a);
bool rename(App& a, int i, const std::string& name);
void duplicate(App& a, int i);
void move(App& a, int from, int to);
void saveNow(App& a);
void revertNow(App& a);
bool autoSave(App& a);
void setAutoSave(App& a, bool on);
enum Keep { kKeepWindows = 0, kKeepPanes = 1, kKeepBar = 2, kKeepPins = 3 };
void setKeeps(App& a, int i, int what, bool on);

// The drawer of workspace cards (the layout pill on the strip, View > Workspaces > Show all): a card per
// workspace with a miniature of its arrangement, its key and what it keeps; Edit turns a card over.
void setDrawerOpen(bool on);
bool drawerOpen();
// Draws the drawer under the strip at `pos` when open; returns its height (0 when closed).
float drawDrawer(App& a, ImVec2 pos, float width);

// Dev checks (--dev-input "workspace add <name>|select <n>|close <n>|starters|dump").
void devCommand(App& a, const std::vector<std::string>& args);

}  // namespace gl::workspacesui
