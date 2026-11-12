// Gridline: K-16: favourite plugins in the pickers. See PluginFavsUi.h.
#include "PluginFavsUi.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/CommandPalette.h"
#include "../core/I18n.h"
#include "../host/HostedPlugins.h"
#include "../host/PluginGroups.h"
#include "../host/PluginPrefs.h"
#include "../host/PluginScan.h"
#include "App.h"
#include "BrowserUi.h"
#include "CollabUi.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl::favui {

using theme::pal;

namespace {
bool byName(const PluginInfo* x, const PluginInfo* y) { return x->name < y->name; }
}  // namespace

void drawRowStar(ImDrawList* dl, ImVec2 c, bool favourite, bool rowHovered, bool starHovered) {
  const auto& P = pal();
  if (!favourite && !rowHovered) return;
  if (starHovered) dl->AddCircleFilled(c, 8.0f, theme::withAlpha(P.surface950, 0.55f), 16);
  const ImU32 col = favourite ? P.hot : starHovered ? P.text : P.textFaint;
  browserui::drawStar(dl, c, 5.0f, col, favourite);
}

bool pickerRow(const PluginInfo& p, bool current, const char* detail) {
  const auto& P = pal();
  App& a = app();
  ImGui::PushID(p.type.c_str());
  const float lineH = ImGui::GetTextLineHeight();
  const float starW = std::round(lineH + theme::kS2);
  const bool hasDetail = detail && *detail;
  const float detailFs = theme::kFontLabel;
  const float detW = hasDetail ? a.fontMono->CalcTextSizeA(detailFs, 1e9f, 0, detail).x : 0.0f;
  const float nameW = ImGui::CalcTextSize(p.name.c_str()).x;
  const float minW = nameW + (hasDetail ? detW + theme::kS4 : 0.0f) + starW + theme::kS2;
  ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups, false);
  const bool clicked = ImGui::Selectable("##pick", current, ImGuiSelectableFlags_SpanAvailWidth, ImVec2(minW, 0));
  ImGui::PopItemFlag();
  const bool hov = ImGui::IsItemHovered();
  const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
  const bool fav = host::isFavourite(p.type);
  const bool starHov = hov && ImGui::GetIO().MousePos.x >= r1.x - starW;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float ty = std::round((r0.y + r1.y - lineH) * 0.5f);
  dl->AddText(ImVec2(r0.x, ty), current ? P.hot : P.text, p.name.c_str());
  if (hasDetail) {
    const float dx = std::round(r1.x - starW - detW - theme::kS1);
    if (dx > r0.x + nameW + theme::kS2)
      dl->AddText(a.fontMono, detailFs, ImVec2(dx, std::round((r0.y + r1.y - detailFs) * 0.5f)), P.textFaint, detail);
  }
  drawRowStar(dl, ImVec2(std::round(r1.x - starW * 0.5f), std::round((r0.y + r1.y) * 0.5f)), fav, hov, starHov);
  if (starHov) {
    ui::Hint(fav ? "Remove from favourites" : "Add to favourites (starred plugins are listed first, in every picker and the browser)");
    ui::Tooltip(fav ? "Remove from favourites" : "Add to favourites");
  } else if (hov) {
    ui::Hint(p.name + " \xC2\xB7 " + p.category + "   (click the star to keep it at the top of the pickers)");
  }
  bool picked = false;
  if (clicked) {
    if (starHov) {
      host::setFavourite(p.type, !fav);
    } else {
      picked = true;
      ImGui::ClosePopupsExceptModals();
    }
  }
  ImGui::PopID();
  return picked;
}

std::vector<const PluginInfo*> installedOfKind(PluginKind kind) {
  App& a = app();
  std::vector<const PluginInfo*> out;
  for (const PluginInfo* p : dynamicPlugins()) {
    if (p->kind != kind) continue;
    std::string fmt, uid;
    if (host::splitType(p->type, fmt, uid) && !host::formatEnabled(fmt)) continue;
    if (!host::listedInBrowser(p->type)) continue;  // J-147: one format per plugin
    if (!collabui::pluginAllowed(a, p->type)) continue;  // J-111
    out.push_back(p);
  }
  std::sort(out.begin(), out.end(), byName);
  return out;
}

std::vector<const PluginInfo*> favouritesOfKind(PluginKind kind) {
  const host::PluginPrefs prefs = host::pluginPrefs();
  std::vector<const PluginInfo*> out;
  for (const auto& p : pluginCatalog())
    if (p.kind == kind && !p.hidden && prefs.favourites.count(p.type)) out.push_back(&p);
  for (const PluginInfo* p : installedOfKind(kind))
    if (prefs.favourites.count(p->type)) out.push_back(p);
  std::sort(out.begin(), out.end(), byName);
  return out;
}

namespace {
struct DevSearch {
  int frame = -1;
  ImVec2 min{}, max{};
  bool active = false;
  float rows = 0;
} gDevSearch;
}  // namespace

std::string devSearchReport() {
  char line[200];
  std::snprintf(line, sizeof line, "plugsearch frame %d x0 %.0f y0 %.0f x1 %.0f y1 %.0f active %d rows %.0f\n", gDevSearch.frame,
                gDevSearch.min.x, gDevSearch.min.y, gDevSearch.max.x, gDevSearch.max.y, gDevSearch.active ? 1 : 0, gDevSearch.rows);
  return line;
}

// The search field at the top of a picker: while it holds text, the menu lists the matching plugins
// (built-in and installed, best first) instead of its folders. Enter picks the first one.
bool searchField(PluginKind kind, const std::function<void(const std::string&)>& pick, const std::string& current) {
  static char buf[2][96] = {};
  char* q = buf[kind == PluginKind::Effect ? 1 : 0];
  if (ImGui::IsWindowAppearing()) {
    q[0] = 0;
    ImGui::SetKeyboardFocusHere();
  }
  ImGui::SetNextItemWidth(std::max(220.0f, ImGui::GetContentRegionAvail().x));
  const bool enter = ImGui::InputTextWithHint("##plugsearch", i18n::tr(kind == PluginKind::Effect ? "Search effects" : "Search instruments"), q, sizeof buf[0],
                                              ImGuiInputTextFlags_EnterReturnsTrue);
  if (ImGui::IsItemHovered()) ui::Hint("Type to find a plugin by name, category or format \xC2\xB7 Enter picks the first match");
  // Escape in the field (which holds the keyboard while the picker is open) closes the picker, so it
  // is not a first press that only empties the field.
  if (ImGui::IsItemDeactivated() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
  gDevSearch.frame = ImGui::GetFrameCount();
  gDevSearch.min = ImGui::GetItemRectMin();
  gDevSearch.max = ImGui::GetItemRectMax();
  gDevSearch.active = ImGui::IsItemActive();
  gDevSearch.rows = ImGui::GetCursorScreenPos().y;
  if (!q[0]) return false;
  std::vector<const PluginInfo*> all;
  for (const auto& p : pluginCatalog())
    if (p.kind == kind && !p.hidden) all.push_back(&p);
  for (const PluginInfo* p : installedOfKind(kind)) all.push_back(p);
  std::vector<cmdpal::Entry> rows;
  rows.reserve(all.size());
  for (const PluginInfo* p : all) {
    cmdpal::Entry e;
    e.kind = cmdpal::Kind::Plugin;
    e.label = p->name;
    e.path = p->category;
    std::string fmt, uid;
    if (const host::Format* f = host::splitType(p->type, fmt, uid) ? host::findFormat(fmt) : nullptr) e.alias = f->label;
    e.order = static_cast<int>(rows.size());
    rows.push_back(std::move(e));
  }
  const std::vector<cmdpal::Ranked> found = cmdpal::rank(rows, q, {});
  ImGui::Separator();
  if (found.empty()) ImGui::TextDisabled("%s", i18n::tr("No plugin matches"));
  int shown = 0;
  for (const cmdpal::Ranked& r : found) {
    if (++shown > 60) break;
    const PluginInfo* p = all[static_cast<size_t>(r.index)];
    std::string fmt, uid;
    const host::Format* f = host::splitType(p->type, fmt, uid) ? host::findFormat(fmt) : nullptr;
    if (pickerRow(*p, p->type == current, f ? f->label : p->category.c_str())) pick(p->type);
  }
  if (enter && !found.empty()) {
    pick(all[static_cast<size_t>(found.front().index)]->type);
    ImGui::ClosePopupsExceptModals();
  }
  return true;
}

void pluginMenu(PluginKind kind, const std::function<void(const std::string&)>& pick, const std::string& current) {
  if (searchField(kind, pick, current)) return;
  const auto favs = favouritesOfKind(kind);
  if (!favs.empty()) {
    ui::Label(i18n::tr("Favourites"));
    for (const PluginInfo* p : favs)
      if (pickerRow(*p, p->type == current, isHostedType(p->type) ? nullptr : p->category.c_str())) pick(p->type);
    ImGui::Separator();
  }
  std::map<std::string, std::vector<const PluginInfo*>> cats;
  std::vector<std::string> order;
  for (const auto& p : pluginCatalog()) {
    if (p.kind != kind || p.hidden) continue;
    if (!cats.count(p.category)) order.push_back(p.category);
    cats[p.category].push_back(&p);
  }
  for (const auto& c : order) {
    if (ImGui::BeginMenu(c.c_str())) {
      for (const PluginInfo* p : cats[c])
        if (pickerRow(*p, p->type == current)) pick(p->type);
      ImGui::EndMenu();
    }
  }
  const auto inst = installedOfKind(kind);
  if (!inst.empty() && ImGui::BeginMenu(i18n::tr("Installed plugins"))) {
    for (const PluginInfo* p : inst) {
      std::string fmt, uid;
      const host::Format* f = host::splitType(p->type, fmt, uid) ? host::findFormat(fmt) : nullptr;
      if (pickerRow(*p, p->type == current, f ? f->label : nullptr)) pick(p->type);
    }
    ImGui::EndMenu();
  }
}

}  // namespace gl::favui
