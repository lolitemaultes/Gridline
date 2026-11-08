// Gridline - menu bar styles of the layout presets (K-79). Each style is a
// list of top-level menus; each menu is made of blocks. The studio blocks are
// the eight studio menus, whole; the other blocks add items that gather
// existing commands under the names another arrangement uses (Navigate,
// Record, Mix, the program menu). Pure data, so tests can check that every
// studio menu is reachable from every style.
#pragma once
#include <vector>
#include "LayoutPreset.h"

namespace gl::menustyle {

enum class Block : int {
  // The studio menus (Menus.cpp), whole.
  File, Edit, Add, Patterns, View, Options, Tools, Help,
  // Gathered items (MenuStyles.cpp).
  Navigate,  // song position, zoom, follow, song / live mode
  Record,    // record, metronome, count-in, typing keyboard, recording options
  Mix,       // mixer windows and mixer layout
  Program,   // the program menu on the mark: about, settings, shortcuts, welcome screen, quit
  Count
};

struct Item {
  Block block;
  const char* submenu = nullptr;  // nullptr: the block's items in the menu itself; else in this submenu
};

struct Menu {
  const char* name;  // English, translatable (nullptr: the program menu on the mark)
  std::vector<Item> items;
};

// The menu bar of a style, left to right. The tracks style's first entry is
// the program menu (name nullptr), drawn on the mark.
const std::vector<Menu>& menus(layout::Preset style);
// The studio menus every style must reach.
const std::vector<Block>& studioBlocks();
// True when `block` appears in the style (a menu or a submenu of one).
bool reaches(layout::Preset style, Block block);

}  // namespace gl::menustyle
