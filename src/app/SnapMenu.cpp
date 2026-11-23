// Gridline — the shared snap choices (see SnapMenu.h).
#include "SnapMenu.h"
#include "../../third_party/imgui/imgui.h"
#include "../core/I18n.h"
#include "Widgets.h"

namespace gl::snapmenu {

namespace {
struct Entry {
  Snap snap;
  const char* name;  // English, as snapName
  const char* hint;
};
// Group breaks are kNone entries (a separator).
constexpr Snap kBreak = static_cast<Snap>(-1);
const Entry kEntries[] = {
    {Snap::Main, i18n::N_("Main"), i18n::N_("Follows the playlist's snap, so every editor shares one grid")},
    {Snap::Line, i18n::N_("Line"), i18n::N_("Snaps to the nearest grid line drawn at this zoom (free at the deepest zoom)")},
    {Snap::Cell, i18n::N_("Cell"), i18n::N_("Snaps to the start of the grid cell under the pointer (follows the zoom)")},
    {Snap::None, i18n::N_("None"), i18n::N_("No snap: things go where they are put")},
    {kBreak, nullptr, nullptr},
    {Snap::Sixth, i18n::N_("1/6 step"), i18n::N_("A sixth of a step")},
    {Snap::Quarter, i18n::N_("1/4 step"), i18n::N_("A quarter of a step")},
    {Snap::Third, i18n::N_("1/3 step"), i18n::N_("A third of a step (step triplets)")},
    {Snap::Half, i18n::N_("1/2 step"), i18n::N_("Half a step")},
    {Snap::Step, i18n::N_("Step"), i18n::N_("One step of the pattern (its own steps per bar)")},
    {kBreak, nullptr, nullptr},
    {Snap::BeatSixth, i18n::N_("1/6 beat"), i18n::N_("A sixth of a beat (sixteenth note triplets in 4/4)")},
    {Snap::BeatQuarter, i18n::N_("1/4 beat"), i18n::N_("A quarter of a beat")},
    {Snap::BeatThird, i18n::N_("1/3 beat"), i18n::N_("A third of a beat (eighth note triplets in 4/4)")},
    {Snap::BeatHalf, i18n::N_("1/2 beat"), i18n::N_("Half a beat")},
    {Snap::Beat, i18n::N_("Beat"), i18n::N_("One beat")},
    {Snap::Bar, i18n::N_("Bar"), i18n::N_("One bar")},
    {kBreak, nullptr, nullptr},
    {Snap::StepDotted, i18n::N_("Dotted step"), i18n::N_("A step and a half")},
    {Snap::BeatHalfDotted, i18n::N_("Dotted 1/2 beat"), i18n::N_("Three quarters of a beat (a dotted eighth in 4/4)")},
    {Snap::BeatDotted, i18n::N_("Dotted beat"), i18n::N_("A beat and a half")},
    {kBreak, nullptr, nullptr},
    {Snap::BeatFifth, i18n::N_("1/5 beat"), i18n::N_("Five to a beat (quintuplets)")},
    {Snap::BeatSeventh, i18n::N_("1/7 beat"), i18n::N_("Seven to a beat (septuplets)")},
    {kBreak, nullptr, nullptr},
    {Snap::RowGrid, i18n::N_("Row grid"), i18n::N_("One step of the edited row's own grid (where it is not on the pattern's)")},
};
std::string gRowGrid;  // K-255: the offered Row grid's name ("" not offered)
}  // namespace

void offerRowGrid(const std::string& text) { gRowGrid = text; }
bool rowGridOffered() { return !gRowGrid.empty(); }

const char* label(Snap s, bool smart) {
  if (smart && s == Snap::Line) return i18n::tr("Smart");
  if (s == Snap::RowGrid && !gRowGrid.empty()) return gRowGrid.c_str();
  for (const Entry& e : kEntries)
    if (e.name && e.snap == s) return i18n::tr(e.name);
  return snapName(s);
}

const char* hint(Snap s) {
  for (const Entry& e : kEntries)
    if (e.name && e.snap == s) return i18n::tr(e.hint);
  return "";
}

bool items(Snap& cur, bool offerMain, bool asMenuItems, bool smart) {
  bool picked = false;
  bool pendingBreak = false;
  for (const Entry& e : kEntries) {
    if (!e.name) {
      pendingBreak = true;
      continue;
    }
    if (e.snap == Snap::Main && !offerMain) continue;
    if (e.snap == Snap::RowGrid && gRowGrid.empty() && cur != Snap::RowGrid) {  // K-255: only for a row on its own grid
      pendingBreak = false;
      continue;
    }
    if (pendingBreak) ImGui::Separator();
    pendingBreak = false;
    ImGui::PushID(static_cast<int>(e.snap));
    const char* l = label(e.snap, smart);
    const bool on = cur == e.snap;
    const bool hit = asMenuItems ? ImGui::MenuItem(l, nullptr, on) : ImGui::Selectable(l, on);
    if (on && !asMenuItems && ImGui::IsWindowAppearing()) ImGui::SetItemDefaultFocus();
    ui::Hint(e.hint);
    ImGui::PopID();
    if (hit) {
      cur = e.snap;
      picked = true;
    }
  }
  return picked;
}

}  // namespace gl::snapmenu
