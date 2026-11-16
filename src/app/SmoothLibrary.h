// Gridline - K-112D: the Smooth layout's Library pane, as its reference's sound browser on the left of
// the main window (refs 111, 158, 233, 252, 248, 264).
//
//   Header      the selected track's sound: its icon and patch name
//   Search      over every patch and effect
//   Navigation  All, Instruments, Effects, then the kinds of sound (Bass, Drum kit, Keys, ...)
//   Results     one row per patch (a built-in instrument's preset, or an instrument as it starts),
//               or per effect. A click loads it: a patch replaces the selected track's sound (its
//               instrument and settings, in one undo step; a track named after its old patch takes
//               the new name), on an audio or empty track a new instrument track is made for it; an
//               effect goes into the selected track's first free slot.
//   Footer      how many items the list holds
//
// The pane is open on a new project and the demo song, as the reference opens a new project with
// its Library showing. Whether it is open is saved with the project (Song::smooth), so each project
// opens with the Library as it was left; B shows or hides it.
#pragma once
#include <string>
#include <vector>

namespace gl {
class App;
}

namespace gl::smoothlib {

// Per frame (any layout): in Smooth, a project just opened or made shows the Library as it was saved;
// showing or hiding it afterwards is noted in the project.
void frame(App& a);
// The Library pane's body (Smooth only; LayoutParts draws the other layouts' library).
void draw(App& a);

// One result row, for tests and the dev command.
struct Item {
  std::string type;    // plugin type
  int preset = -1;     // factory preset index (-1: the plugin as it starts)
  std::string name;    // what the row shows
  std::string detail;  // the instrument or the category, dim at the right
  std::string kind;    // Bass, Keys, ... (instruments), the category (effects)
  bool effect = false;
};
// The rows of a view: nav 0 All, 1 Instruments, 2 Effects, 3 + k the k-th kind of sound.
std::vector<Item> items(const App& a, int nav, const std::string& filter);
// The kinds of sound in navigation order.
const std::vector<std::string>& kinds();
// Loads a row as a click does. Returns false when nothing changed.
bool load(App& a, const Item& it);

// Dev ("smoothlib ..."): nav <n> | rows | load <row> | state.
bool devCommand(App& a, const std::vector<std::string>& args);

}  // namespace gl::smoothlib
