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
  Track,     // K-112b (Tracks layout): new, duplicate and delete tracks, the track's own menu
  Window,    // K-112b (Tracks layout): the editors and windows, plugin windows shown or hidden
  TracksEdit,    // K-112b (Tracks layout): Edit in that layout's grouping (Studio's Edit stays as it is)
  TracksRecord,  // K-112b (Tracks layout): Record in that layout's grouping (Live keeps Record)
  Pages,         // K-106 (Tracker layout): the pages and header screens, named as the header names them
  TracksView,    // R04-09 (Tracks layout): the panes the View menu shows (inspector, editors, lists, ...)
  TrackerEdit,     // K-106: Edit with the pattern editor's block and row edits (Studio's Edit commands, regrouped)
  TrackerPattern,  // K-106: the pattern editor's settings (edit mode, edit step, follow, columns)
  TrackerView,     // K-106: Studio's View less the pages and header screens the Tracker names otherwise
  ClipsCreate,   // K-112c (Clips layout): new audio / MIDI tracks, a marker, imports (then Add in a submenu)
  ClipsNavigate, // K-112c (Clips layout): the views and bands to go to
  ClipsRecord,   // K-112c (Clips layout): Arrangement and Session record, the click, the record options
  TracksFile,    // R04-09 (Tracks layout): File's close items and the Bounce submenu, after the studio's File
  ClipsFile,     // checkpoint 49 (Clips layout): File as the reference's (Studio's File rows, regrouped)
  ClipsEdit,     // checkpoint 49 (Clips layout): Edit as the reference's (Studio's Edit, and the clip rows of Patterns)
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
// True when `block` appears in the style (a menu or a submenu of one), or a regrouping of it
// (K-112b: the Tracks layout's Edit and Record hold the same commands).
bool reaches(layout::Preset style, Block block);

}  // namespace gl::menustyle
