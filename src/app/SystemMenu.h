// Gridline - a layout's menus in the operating system's menu bar (K-112).
//
// The menu tree of the layout's menu style (core/MenuTree.h, read from MenuDef.h) is handed
// to the system menu bar (host/ui/NativeMenu.h): the macOS menu bar, or a Linux global menu
// over D-Bus where the desktop shows one. It holds exactly the items, states and shortcuts of the
// in-window menus, and a chosen item runs the same definition (menutreeui::run).
//
// When: the setting ui.systemMenuBar is on (the default), the system has such a bar, and the
// active layout puts its menus in a row of their own (Live, Logic, Tracker). Studio keeps its
// menus in its top bar. The welcome screen keeps the system's default. Hidden runs (tests) keep
// the in-window row unless --system-menu asks.
#pragma once
#include <string>
#include <vector>

namespace gl {
class App;
}

namespace gl::sysmenu {

// True while the system menu bar holds the menus (the top bar then draws no menu buttons).
bool active();
// Per frame, after the windows: installs, refreshes (when the menus changed) or removes the
// system menu bar, and runs the item chosen there.
void frame(App& a);
// Dev: a hidden run fills the system menu bar too (--system-menu).
void setDevForce(bool on);
// Hidden runs (tests) keep the in-window menu row unless forced.
void setHiddenRun(bool hidden);
bool hiddenRun();
// The X11 window the menus belong to (Linux global menu; 0 = none).
void setWindow(unsigned long xid);
// Dev: the installed tree as text, one item per line, indented by depth ("Undo|Ctrl+Z|on").
std::string dump();
// Dev: runs an item by its shown path ("Edit/Undo") on the next frame, as choosing it would.
bool choose(const std::string& path);
// Dev checks (--dev-input "sysmenu dump | choose <path> | status").
bool devCommand(App& a, const std::vector<std::string>& args, int frame);

}  // namespace gl::sysmenu
