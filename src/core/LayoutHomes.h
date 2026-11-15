// Gridline - where every part of the app lives in each layout (K-112, rule R2).
//
// One table, generated from the layout plan, holds every inventory item (windows, bars,
// dialogs, menus, every built-in plugin editor and the new parts) with its home in Studio,
// Live, Logic and Tracker: a region of that layout's shell (core/LayoutShell.h), floating,
// a dialog, hosted inside another part (a plugin editor in a device band), inside another
// window, a menu, a bar or an overlay. Nothing exists in only one layout: every row has a
// home in all four. The app reads it to dock windows (app/LayoutShellUi.cpp) and the
// inventory tests read it to open each item in each layout.
//
// Pure data, no UI. Per person: a layout is never part of a project, so collaborators on
// different layouts see the same project with each item in their own layout's home.
#pragma once
#include <string>
#include <vector>
#include "LayoutPreset.h"

namespace gl::layout {

enum class HomeKind : uint8_t {
  Dock,       // docked into a region of the layout's shell
  Float,      // a floating window
  Modal,      // a dialog over the window
  Host,       // the item's body is drawn inside a region's part (a device band, a page); its window draws nothing there
  Inside,     // drawn inside another item (a page of a window, a section of Settings)
  Overlay,    // over everything (cursors, cards, palette, fades)
  Bar,        // a bar of the shell (top bar, status line)
  Menu,       // a menu, popup or combo
  Unchanged,  // the same in every layout (splash, welcome screen)
  Part,       // a new part (K-112 R4): a window of its own; in Studio it floats, closed by default
};
const char* kindName(HomeKind k);

struct Home {
  HomeKind kind = HomeKind::Float;
  const char* region = "";  // region id of the layout's shell ("" = none: floating, inside, menu, ...)
  bool openByDefault = false;
};

struct Item {
  const char* id;       // inventory id: "B2", "C1", "F58", "N4"
  const char* name;     // English name (reports and tests)
  const char* window;   // stable ImGui window name when the item is a window of its own ("" otherwise)
  const char* plugin;   // plugin type of an editor row ("gl.eq"), else ""
  const char* later;    // the task row that builds its content when it is not built yet ("" = built)
  Home home[kLayoutCount];
};

// Every item, in the plan's order.
const std::vector<Item>& items();
const Item* findItem(const std::string& id);
// The item that owns a window (stable name, e.g. "Mixer", "###channelsettings").
const Item* findWindow(const std::string& stableName);
// The item of a plugin editor ("gl.eq").
const Item* findPlugin(const std::string& type);
// The home of a window in a layout. Windows the table does not list float (as they always have).
Home windowHome(const std::string& stableName, Preset p);
// Region id prefix of a layout ("studio.", "live.", ...).
std::string regionPrefix(Preset p);

}  // namespace gl::layout
