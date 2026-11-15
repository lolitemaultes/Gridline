// Gridline - the menu tree (K-112): the menu bar of a layout as data.
//
// Every layout's menu bar (its menu style: Studio, Live, Logic, Tracker) is one tree: top menus,
// submenus and items, each item with its shortcut, the command behind it (the keymap id), its
// check and enabled state and its description. The menus are defined as data (app/MenuDef.h);
// the tree is read from those definitions (app/MenuTreeUi.h), the in-window menu bar draws the
// same definitions, so the in-window menu bar, the command palette, the system menu bar (macOS,
// the Linux global menu) and the tests all see exactly the same items. Providers (recent files,
// plugins, arrangements, layouts, templates) are entries whose items are listed from the project
// and settings at that moment.
//
// An item is addressed by its path (the submenu labels as the menu code passes them), its label
// and its occurrence among items of the same path and label (two "Empty project" templates).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace gl::menutree {

struct Node {
  std::string raw;       // the label as the menu code passes it (may carry an ImGui "##id")
  std::string shown;     // as shown (translated, without the id)
  std::string en;        // in English (the source string)
  std::string shortcut;  // "Ctrl+S" or ""
  std::string command;   // the keymap command behind the shortcut ("" = none)
  std::string hint;      // the item's description (its tooltip)
  bool submenu = false;
  bool separatorBefore = false;
  bool enabled = true;
  bool checked = false;
  bool paletteSkip = false;  // listed by the menu for itself (the Add menu's plugin lists): not a palette row
  int occ = 0;  // occurrence among items of the same path and raw label
  std::vector<Node> children;
};

struct Tree {
  std::string style;        // the menu style ("studio", "live", "logic", "tracker")
  std::vector<Node> roots;  // the top menus (each a submenu)
};

// An item with where it lives.
struct Ref {
  int root = -1;
  std::vector<const Node*> path;  // the submenus from the root (the root included)
  const Node* item = nullptr;
  std::vector<std::string> rawPath() const;  // the submenus' raw labels below the root
};

// Every item (not submenus), in menu order.
std::vector<Ref> items(const Tree& t);
// Number of items and submenus.
int countItems(const Tree& t);
int countSubmenus(const Tree& t);
// The item at root / raw path / raw label / occurrence (nullptr when gone).
const Node* find(const Tree& t, int root, const std::vector<std::string>& rawPath, const std::string& rawLabel, int occ);
// Every item that runs `command`.
std::vector<Ref> byCommand(const Tree& t, const std::string& command);
// One line per item: "Root > Sub > Item <TAB> shortcut <TAB> command <TAB> flags" (English,
// flags: e enabled, c checked). Stable across runs: the tests compare it.
std::string dump(const Tree& t);

}  // namespace gl::menutree
