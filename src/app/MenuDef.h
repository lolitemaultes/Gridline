// Gridline - the menus as data (K-112).
//
// A menu is a list of entries: items (a label, the keymap command whose chord is shown, what it
// runs, when it is enabled or checked), on/off items bound to a flag, separators, titled
// separators, submenus and providers. A provider lists items the menu cannot know ahead (recent
// files, templates, the project's patterns, plugin lists) or another module's own rows; it is
// menu code run in place.
//
// The same definition is drawn by the in-window menu bars (emit with no walker), read into the
// menu tree (core/MenuTree.h: the command palette, the system menu bars, the tests) and run when
// an item is chosen there (emit with a walker: nothing is drawn, the walker sees every entry and
// answers which item runs).
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace gl {
class App;
}
namespace gl::imhook {
struct MenuWalker;
}

namespace gl::menudef {

using Fn = void (*)(App&);
using Pred = bool (*)(App&);
using Text = std::string (*)(App&);      // a label made when shown (already translated)
using Shortcut = const char* (*)(App&);  // a shortcut text chosen when shown
using Flag = bool* (*)(App&);

enum class Kind { Item, Toggle, Separator, Heading, Submenu, Provider };

struct Entry {
  Kind kind = Kind::Item;
  const char* label = nullptr;  // English (translated when shown)
  const char* key = nullptr;    // keymap command: its chord is the shortcut shown
  Fn run = nullptr;             // Item: what it does; Provider: the menu code it runs
  Pred enabled = nullptr;       // nullptr: enabled
  Pred checked = nullptr;       // nullptr: not checked
  Pred shown = nullptr;         // nullptr: always there
  Flag flag = nullptr;          // Toggle: the flag it flips
  Text dynLabel = nullptr;      // when set, the label (instead of `label`)
  Shortcut dynKey = nullptr;    // when set, the shortcut text (instead of `key`'s chord)
  const char* hint = nullptr;   // the item's description (English)
  const char* devRect = nullptr;   // Submenu: its header as a scripted target (devRackItem)
  const char* devNote = nullptr;   // Submenu: its header as an "sdlat" target (devNoteRect)
  std::vector<Entry> children;     // Submenu

  Entry when(Pred p) && { enabled = p; return std::move(*this); }
  Entry check(Pred p) && { checked = p; return std::move(*this); }
  Entry only(Pred p) && { shown = p; return std::move(*this); }
  Entry tip(const char* h) && { hint = h; return std::move(*this); }
  Entry text(Text t) && { dynLabel = t; return std::move(*this); }
  Entry keyText(Shortcut s) && { dynKey = s; return std::move(*this); }
  Entry rect(const char* id) && { devRect = id; return std::move(*this); }
  Entry note(const char* id) && { devNote = id; return std::move(*this); }
};
using Menu = std::vector<Entry>;

Entry item(const char* label, const char* key, Fn run);
Entry item(const char* label, Fn run);
Entry toggle(const char* label, const char* key, Flag flag);
Entry separator();
Entry heading(const char* label);
Entry submenu(const char* label, Menu children);
Entry provider(Fn code);

// Draws `m` (walker nullptr), or reports it to `w` without drawing: providers then run with `w`
// installed as ImGui's menu walker, so their items report the same way.
void emit(App& a, const Menu& m, imhook::MenuWalker* w = nullptr);

// The Studio menus (Menus.cpp): 0 File, 1 Edit, 2 Add, 3 Patterns, 4 View, 5 Options, 6 Tools,
// 7 Help.
const Menu& studio(int which);
// K-112b: Edit in the Tracks layout's grouping.
const Menu& tracksEdit();
// Checkpoint 49 (N47-13): File and Edit in the Clips layout (Studio's rows, regrouped and reworded).
const Menu& clipsFile();
const Menu& clipsEdit();
// The gathered menus of the other menu styles (MenuStyles.cpp): 0 Navigate, 1 Record, 2 Mix,
// 3 Program.
const Menu& gathered(int which);

// Counts, for the tests: entries of each kind in `m` and its submenus.
int count(const Menu& m, Kind k);

}  // namespace gl::menudef
