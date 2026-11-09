// Gridline - K-93 command palette (Ctrl+K, Cmd+K on macOS; Ctrl+Shift+P too).
//
// One search over everything the app can do, built from what is already there rather than
// a second list:
//   - the menus of the menu bar in use, walked as they are drawn (imgui_hooks: BeginMenu /
//     MenuItem report instead of drawing), each item with where it lives, its shortcut, its
//     on / off state and whether it can run now (with the menu's own hint as the reason);
//   - the central keymap: a menu item's shortcut names its command, so the two are one row;
//     commands without a menu item get rows of their own;
//   - the windows (and each channel's and effect's plugin window);
//   - every setting of the settings pages (drawn once unseen, their rows noted);
//   - every plugin the plugin picker lists.
// Running a menu row walks the menus again and lets only that item answer true, so the
// menu's own code does the work. Recent rows are a per-user setting (ui.paletteRecent).
#pragma once
#include <string>
#include <vector>
#include "../core/CommandPalette.h"

namespace gl {
class App;
}

namespace gl::cmdpalette {

// ---- API for toolbars and other callers (K-108: pinning commands) ----
struct CommandInfo {
  std::string id;        // stable row id (cmdpal::Entry::id)
  std::string label;     // as shown
  std::string path;      // where it lives ("Options > Recording options")
  std::string shortcut;  // "Ctrl+S" or ""
  bool enabled = true;
  cmdpal::Kind kind = cmdpal::Kind::Command;
};
// Every row the palette offers right now (call during a frame: the menus are walked).
std::vector<CommandInfo> commandList(App& a);
// Runs a row by id (as Enter on it does), the next frame. False when there is no such row
// or it cannot run now.
bool run(App& a, const std::string& id);

// ---- the palette ----
void open(App& a);
void close();
void toggle(App& a);
bool isOpen();
// Per frame (main loop, with the dialogs): the palette when open, and the commands it ran.
void draw(App& a);

// Inside a menu body: its items are not palette rows while this lives (the plugin lists of
// the Add menu: plugins are rows of their own).
struct SkipRows {
  SkipRows();
  ~SkipRows();
  SkipRows(const SkipRows&) = delete;
  SkipRows& operator=(const SkipRows&) = delete;
};

// Notes from the shared widgets while the palette reads the menus and settings pages or
// highlights a setting (cheap test first: noting()).
// Header-only so the shared widgets need no link to the palette (some tests build them alone);
// the palette points these at its own note takers while it notes.
namespace detail {
inline bool noting = false;
inline void (*formLabelFn)(const char*) = nullptr;
inline void (*sectionFn)(const char*) = nullptr;
inline void (*hintFn)(const char*) = nullptr;
}  // namespace detail
inline bool noting() { return detail::noting; }
inline void noteFormLabel(const char* shown) {  // ui::FormLabelFit: a settings row
  if (detail::formLabelFn) detail::formLabelFn(shown);
}
inline void noteSection(const char* captionEn) {  // ui::BeginSection: a settings group
  if (detail::sectionFn) detail::sectionFn(captionEn);
}
inline void noteHint(const char* text) {  // ui::Hint: the last item's description
  if (detail::hintFn) detail::hintFn(text);
}

// Dev checks (--dev-input "palette ..."): open, close, dump, state, run <id>.
bool devCommand(App& a, const std::vector<std::string>& args, int frame);

}  // namespace gl::cmdpalette
