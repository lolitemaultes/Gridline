// Gridline - J-146: the welcome screen's Collaborative view. Every collaborative project this
// computer knows as a tile (its sketch, the members as pictures, my role, what needs
// attention, who is working on it right now), the Join / New / Share tiles, and the
// collaboration settings inline (the same page as Settings > Collaboration). The data is
// collab::buildProjectList (src/collab/ProjectList.h); presence answers come from the
// controller's prober while this view shows (docs/collaboration.md, "Presence on the
// welcome screen"). LauncherUi.cpp owns the window, the rail, the header and the grid;
// this file fills the grid when its view is selected.
#pragma once
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../core/Launcher.h"

namespace gl {
class App;
}

namespace gl::launchercollab {

// The launcher's colours, passed in so the tiles match the other views.
struct Look {
  ImU32 card, cardHover, thumb, line, lineHover, text, dim, faint, accent, danger, fill, fillHover, heart;
};

// Per frame while the view shows (before the layout): the list refreshes, presence checks run.
void update(App& a, const char* search);
// The grid's items (tiles, section rows, the settings panel) and each block's height.
void items(std::vector<launcher::Item>& items, std::vector<float>& blockH);
// Draws item `i` at `p` (its rectangle from the layout). Returns true when a tile was activated.
bool drawItem(App& a, int i, ImVec2 p, const launcher::Rect& r, const launcher::Layout& L, bool focused, bool keyNav, const Look& look);
// A tile chosen with the keyboard (Enter) or a click.
void activate(App& a, int i);
// The tile's menu (keyboard: the Menu key).
void openMenu(int i);
// Favourite on / off for project tile `i` (keyboard: F).
void toggleFavourite(App& a, int i);
// Popups (after the grid).
void menus(App& a);
// Rail count, and how many projects someone is working on right now.
int projectCount(App& a);
int liveCount();
// The header's note next to the title (presence on / off, who is here); returns its width.
float titleNoteWidth(App& a);
void drawTitleNote(App& a, ImDrawList* dl, ImVec2 p, float h, const Look& look);
// Dev check (--dev-input "launcher"): the rows as text.
void devReport(App& a);

}  // namespace gl::launchercollab
