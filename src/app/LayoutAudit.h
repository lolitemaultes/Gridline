// Gridline — editor layout audit (Q-7). While a collection is open the
// editor building blocks (skin panels / wells / graph boxes, pui panels and
// sections, every bound control through pui::dragParamItem, skin labels and
// buttons) report their screen rectangles here; end() checks them with
// inset::check. Off (the normal case) every call is one branch.
//
// `gridline-app --layout-audit <dir>` draws every built-in editor at three
// widths (form scale 1, 1.5, 2), writes <dir>/layout_audit.tsv and a contact
// sheet <dir>/sheet.png with the problem rectangles outlined, and exits with
// the number of editors that have issues (0 = clean).
#pragma once
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "Inset.h"

namespace gl::audit {

bool on();
void begin();
void frame(ImVec2 p0, ImVec2 p1, const char* what);
void control(ImVec2 p0, ImVec2 p1, const char* what);
// The last ImGui item as a control.
void lastItem(const char* what);

struct Report {
  std::vector<inset::Rect> frames, controls;
  std::vector<std::string> frameWhat, controlWhat;
  std::vector<inset::Issue> issues;
};
// Closes the collection and checks it (min inset in px).
Report end(float min = inset::kMinInset);

}  // namespace gl::audit
