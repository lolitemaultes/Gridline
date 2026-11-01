// Gridline: track icons on screen. The icons are Gridline originals
// (assets/icons/ui/track-*.svg, 24-unit grid, 2-unit round stroke); which icon
// is which, and its category and FL number, is core/TrackIconSet.cpp.
#include "TrackIcons.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include "../core/I18n.h"
#include "../core/TrackIconSet.h"
#include "Theme.h"
#include "Widgets.h"
#include "icons/IconTexture.h"

namespace gl::trackicons {

const char* name(int icon) {
  const trackicon::Info* i = trackicon::info(icon);
  return i ? i18n::tr(i->name) : i18n::tr("None");
}

void draw(ImDrawList* dl, int icon, ImVec2 c, float box, ImU32 col) {
  const trackicon::Info* i = trackicon::info(icon);
  if (!i || !dl) return;
  icons::draw(dl, i->svg, c, icons::standardSize(box), col);
}

float boxFor(float rowH, float maxBox) {
  // The largest standard box that fits the row with a pixel to spare.
  static const float kBoxes[] = {20, 16, 14, 12, 10};
  for (float b : kBoxes)
    if (b <= maxBox && b <= rowH - 1.0f) return b;
  return 0.0f;
}

namespace {

std::string lower(const char* s) {
  std::string o(s ? s : "");
  for (char& ch : o) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return o;
}

bool matches(const trackicon::Info& i, const std::string& q) {
  if (q.empty()) return true;
  for (const char* s : {i.name, i18n::tr(i.name), i.keywords, trackicon::categoryName(i.category), i18n::tr(trackicon::categoryName(i.category))})
    if (lower(s).find(q) != std::string::npos) return true;
  return false;
}

}  // namespace

namespace {
std::string gDevSearch;
int gDevCategory = -2;  // -2: none pending
}  // namespace

void devPreset(const char* search, int category) {
  gDevSearch = search ? search : "";
  gDevCategory = category;
}

bool picker(int current, int* out) {
  const auto& P = theme::pal();
  static char search[64] = "";
  static int category = -1;  // -1: every icon
  static int hovered = 0;
  if (ImGui::IsWindowAppearing()) {  // opens on the current icon's category, search empty
    search[0] = 0;
    const trackicon::Info* ci = trackicon::info(current);
    category = ci ? static_cast<int>(ci->category) : -1;
    if (!gDevSearch.empty()) std::snprintf(search, sizeof search, "%s", gDevSearch.c_str());
    if (gDevCategory >= -1) category = std::min(gDevCategory, trackicon::CategoryCount - 1);
    gDevSearch.clear(), gDevCategory = -2;
  }
  bool picked = false;
  auto pick = [&](int id) {
    *out = id;
    picked = true;
    ImGui::CloseCurrentPopup();
  };
  const float cell = 30.0f, gap = 2.0f;
  const int perRow = 8;
  const float listW = 150.0f, gridW = perRow * (cell + gap) + ImGui::GetStyle().ScrollbarSize;
  // Tall enough for the whole category list (no scrolling there).
  const float bodyH = std::max(8 * (cell + gap), static_cast<float>(trackicon::CategoryCount + 1) * ImGui::GetTextLineHeightWithSpacing() + 4.0f);

  // Search (every category) and "no icon".
  const float noneW = ImGui::CalcTextSize(i18n::tr("No icon")).x + ImGui::GetStyle().FramePadding.x * 2;
  ImGui::SetNextItemWidth(listW + gridW - noneW);
  if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
  ImGui::InputTextWithHint("##iconsearch", i18n::tr("Search icons"), search, sizeof search);
  ImGui::SameLine(0, ImGui::GetStyle().ItemSpacing.x);
  if (ImGui::Button(i18n::tr("No icon"), ImVec2(noneW, 0))) pick(0);
  const std::string q = lower(search);

  // Category list.
  ImGui::BeginChild("##iconcats", ImVec2(listW, bodyH), ImGuiChildFlags_None);
  if (ImGui::Selectable(i18n::tr("All icons"), q.empty() && category < 0)) category = -1, search[0] = 0;
  for (int k = 0; k < trackicon::CategoryCount; ++k)
    if (ImGui::Selectable(i18n::tr(trackicon::categoryName(k)), q.empty() && category == k)) category = k, search[0] = 0;
  ImGui::EndChild();
  ImGui::SameLine(0, 4);

  // Icon grid: the chosen category, or every match of the search.
  ImGui::BeginChild("##icongrid", ImVec2(gridW, bodyH), ImGuiChildFlags_None);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  int n = 0, shown = 0;
  const trackicon::Info* set = trackicon::all(&n);
  int hov = 0;
  for (int k = 0; k < n; ++k) {
    const trackicon::Info& i = set[k];
    if (!q.empty() ? !matches(i, q) : category >= 0 && static_cast<int>(i.category) != category) continue;
    if (shown % perRow) ImGui::SameLine(0, gap);
    ++shown;
    ImGui::PushID(i.id);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("ic", ImVec2(cell, cell))) pick(i.id);
    const bool h = ImGui::IsItemHovered();
    if (h) hov = i.id;
    dl->AddRectFilled(p, ImVec2(p.x + cell, p.y + cell), i.id == current ? P.surface600 : h ? P.surface700 : P.field, theme::kRadiusSm);
    if (i.id == current) dl->AddRect(p, ImVec2(p.x + cell, p.y + cell), P.hot, theme::kRadiusSm);
    draw(dl, i.id, ImVec2(p.x + cell * 0.5f, p.y + cell * 0.5f), 20.0f, h || i.id == current ? P.text : P.textDim);
    ImGui::PopID();
  }
  if (!shown) ImGui::TextColored(theme::vec(P.textFaint), "%s", i18n::tr("No icon matches"));
  ImGui::EndChild();
  if (hov) hovered = hov;
  else if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) hovered = 0;

  // Footer: the icon under the pointer (else the current one) and its category.
  const int show = hovered ? hovered : current;
  const trackicon::Info* si = trackicon::info(show);
  char line[160];
  if (si)
    std::snprintf(line, sizeof line, "%s  \xC2\xB7  %s", i18n::tr(si->name), i18n::tr(trackicon::categoryName(si->category)));
  else
    std::snprintf(line, sizeof line, "%s", i18n::tr("No icon"));
  ImGui::TextColored(theme::vec(hovered ? P.text : P.textDim), "%s", line);
  return picked;
}

}  // namespace gl::trackicons
