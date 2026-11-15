// Gridline - the menu tree read from the menu definitions (K-112, core/MenuTree.h, MenuDef.h).
//
// build() reads the menu bar of a menu style from its definitions (each entry reports to a walker,
// nothing is drawn; providers list their items the same way) and returns it as a tree. run() reads
// the same definitions again and lets only the chosen item answer true, so the entry's own action
// does the work. The in-window menu bar, the palette, the system menu bars and the tests use these.
#pragma once
#include <string>
#include <vector>
#include "../core/LayoutPreset.h"
#include "../core/MenuTree.h"

namespace gl {
class App;
}

namespace gl::menutreeui {

// The menu bar of `style` as it is now (call during a frame).
menutree::Tree build(App& a, layout::Preset style);
// The menu bar in use (the active layout's menu style).
menutree::Tree build(App& a);

enum class RunResult { Ran, Gone, Disabled };
// Runs the item at root / raw path / raw label / occurrence of `style`'s menu bar (during a frame).
RunResult run(App& a, layout::Preset style, int root, const std::vector<std::string>& rawPath, const std::string& rawLabel, int occ);
RunResult run(App& a, layout::Preset style, const menutree::Ref& item);

// While build() walks: the description of the item just walked (ui::Hint while noting).
// True when the text was taken.
bool noteHint(const char* text);

// Dev check (--dev-input "menutree dump [studio|live|logic|tracker]"): the tree as text between
// MENUTREE BEGIN / END lines.
bool devCommand(App& a, const std::vector<std::string>& args, int frame);
void devFrame(App& a, int frame);  // during the frame: prints the dumps asked for

}  // namespace gl::menutreeui
