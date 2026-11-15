// Gridline - the operating system's own menu bar (K-112), as plain data.
//
// macOS: the menus of a layout move into the system menu bar at the top of the screen
// (NativeMenuMac.mm). Linux: they are exported over D-Bus as a "dbusmenu" for desktops with a
// global menu (a registrar such as KDE's appmenu or the Unity / Budgie / Xfce / MATE applets);
// libdbus is loaded at run time, so Gridline links nothing and runs the same without it
// (NativeMenuLinux.cpp). Where there is no such bar (Windows, GNOME by default, Wayland
// sessions without the KDE protocol, no registrar on the bus) available() is false and Gridline
// keeps its in-window menu row.
//
// The app builds the tree from its own menus (core/MenuTree.h through app/SystemMenu.cpp) and
// installs it here; a chosen item comes back by its id on the next frame, and the app runs it
// through the same menu code the in-window menus use. Shortcuts are shown, never bound: the keys
// stay the app's own (its key sets know the focused editor), so a key never runs twice.
#pragma once
#include <string>
#include <vector>

namespace gl::host::nativemenu {

struct Item {
  std::string label;     // as shown ("" = a separator)
  std::string shortcut;  // shown at the right ("Ctrl+S")
  std::string id;        // handed back when chosen (empty: a submenu)
  bool enabled = true;
  bool checked = false;
  bool checkable = false;  // a check mark column (an on / off item)
  std::vector<Item> children;  // a submenu when not empty
};

// True where the system has a menu bar Gridline can fill (macOS; Linux with a registrar).
bool available();
// What was found ("cocoa", "dbusmenu: registrar com.canonical.AppMenu.Registrar", "no session
// bus", "no libdbus-1", ...), for the settings page and the reports.
std::string status();
// The window the menus belong to (Linux: the X11 window id; 0 = unknown). Call before install.
void setWindow(unsigned long xid);
// Replaces the application's menu bar: `appMenu` goes under the program's name (macOS) or first
// (Linux), then `menus` left to right. Calling it again with a new tree replaces the old one.
void install(const std::vector<Item>& appMenu, const std::vector<Item>& menus);
// macOS: the program menu's own items that get their usual key equivalents (Hide: Command+H,
// Hide others: Command+Alt+H), by name ("hide", "hideOthers"); the app passes only the keys its
// active key set leaves free (keys::systemMenuKeys). Call before install. Elsewhere: ignored.
void setSystemKeys(const std::vector<std::string>& items);
// Puts back the menu bar the program started with.
void uninstall();
bool installed();
// Per frame: handles the system's requests (Linux: the D-Bus messages, without blocking).
void poll();
// The id of the item chosen since the last call ("" when none).
std::string takeChosen();
// Linux: whether the D-Bus worker (its thread, libdbus, the object on the session bus) has been
// started. Only available() and install() start it, so a run whose layout never asks for the
// system menu bar (Studio, test runs) never touches the bus. Elsewhere: false.
bool busStarted();

}  // namespace gl::host::nativemenu
