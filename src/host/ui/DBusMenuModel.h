// Gridline - the menus as a "dbusmenu" layout (K-112), the model behind the Linux global menu.
//
// Desktops with a global menu bar (KDE Plasma's application menu, the Unity / Budgie / Xfce /
// MATE appmenu applets) show a program's menus from a com.canonical.dbusmenu object on the
// session bus. This is the transport-free part: the menu tree as numbered entries with the
// dbusmenu properties (label with its mnemonic escaped, enabled, separator, check mark,
// shortcut, submenu), ids that stay the same while an item stays where it is (so an open menu
// does not jump), and a revision that moves when anything changed. DBusMenuBus.cpp puts it on
// the bus; the tests check it without one.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "NativeMenu.h"

namespace gl::host::dbusmenu {

struct Entry {
  int id = 0;
  int parent = -1;
  std::string label;  // dbusmenu "label": underscores doubled (a single one marks a mnemonic)
  bool enabled = true;
  bool separator = false;
  bool checkable = false;
  bool checked = false;
  bool submenu = false;                             // "children-display": "submenu"
  std::vector<std::vector<std::string>> shortcut;   // [["Control", "Shift", "s"]]
  std::string appId;                                // the app's item id (chosen on "clicked")
  std::vector<int> children;
};

class Model {
 public:
  // Replaces the tree (the app menu first, then the menus). Returns true when anything changed;
  // the revision then moved.
  bool set(const std::vector<nativemenu::Item>& appMenu, const std::vector<nativemenu::Item>& menus);
  uint32_t revision() const { return revision_; }
  // Entry 0 is the root (its children are the top menus).
  const Entry* entry(int id) const;
  const std::vector<Entry>& entries() const { return entries_; }
  // The app id of an item chosen by its dbusmenu id ("" when it is a submenu, separator or gone).
  std::string appIdOf(int id) const;

 private:
  int idFor(const std::string& key);
  void add(const nativemenu::Item& it, int parent, const std::string& parentKey, std::map<std::string, int>& seen);
  std::vector<Entry> entries_;     // by position; entry(id) maps through byId_
  std::map<int, size_t> byId_;
  std::map<std::string, int> ids_;  // path key -> id, kept across rebuilds
  int nextId_ = 1;
  uint32_t revision_ = 0;
};

// "Ctrl+Shift+S" -> {{"Control", "Shift", "S"}}; "F9" -> {{"F9"}}; "" -> {}.
std::vector<std::vector<std::string>> shortcutKeys(const std::string& shortcut);
// Underscores doubled ("Save_as" -> "Save__as"): dbusmenu reads a single one as a mnemonic.
std::string escapeMnemonic(const std::string& label);
// The text as valid UTF-8 (libdbus refuses anything else, and ends the program on it): every
// byte that does not start a well-formed character (overlong, a surrogate, past U+10FFFF, cut
// short) becomes U+FFFD; NUL bytes are dropped.
std::string validUtf8(const std::string& s);

}  // namespace gl::host::dbusmenu
