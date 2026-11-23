// Gridline - Settings > Layouts: each layout's control bar, panels, workspaces, pinned commands and
// switching, and Studio's zone keys. The full editor behind the drawer of workspace cards and the bar
// list (a right-click on an empty bit of a bar); every change here is the same data those edit.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../core/CommandPalette.h"
#include "../core/I18n.h"
#include "../core/LayoutHomes.h"
#include "../core/LayoutShell.h"
#include "../core/Settings.h"
#include "../core/Toolbar.h"
#include "../core/ZoneKeys.h"
#include "App.h"
#include "CommandPaletteUi.h"
#include "LayoutPresets.h"
#include "LayoutShellUi.h"
#include "SettingsPages.h"
#include "Theme.h"
#include "ToolbarUi.h"
#include "Widgets.h"
#include "WorkspacesUi.h"
#include "ZoneKeysUi.h"

namespace gl {

using layout::Preset;
namespace tb = toolbar;

namespace {

int gLayout = -1;  // the layout the page shows (-1: the active one)
char gPinFind[96] = "";
char gRename[64] = "";
int gRenaming = -1;
toolbarui::PartDrag gBarDrag;  // a part dragged in the control bar list

const char* tr(const char* s) { return i18n::tr(s); }

// ---- the control bar ----
void barSection(App& a, Preset p) {
  const auto& P = theme::pal();
  ui::BeginSection(tr("CONTROL BAR"));
  tb::Bar bar = toolbarui::modelFor(a, p).bar;
  bool edit = false;
  if (toolbarui::barIsOwn(a, p) && p == layoutui::active())
    ImGui::TextColored(theme::vec(P.textDim), "%s", tr("The workspace in use keeps a bar of its own: changes here go to it."));
  const float lx0 = ImGui::GetCursorScreenPos().x, lx1 = lx0 + ImGui::GetContentRegionAvail().x;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  for (const tb::SectionInfo& sec : tb::sections(p)) {
    const float hy0 = ImGui::GetCursorScreenPos().y;
    const bool lit = !gBarDrag.dragging().empty() && gBarDrag.target() == sec.id && tb::sectionOf(p, bar, gBarDrag.dragging()) != sec.id;
    ImGui::TextColored(theme::vec(lit ? P.hot : P.textFaint), "%s", tr(sec.label));
    const std::vector<const tb::Part*> parts = tb::sectionParts(p, bar, sec.id);
    if (parts.empty())  // a free side with nothing left on it: a part can be dragged back onto it
      ImGui::TextColored(theme::vec(P.textFaint), "  %s", tr("Nothing here: drag a part onto this side"));
    gBarDrag.heading(sec.id, tr(sec.label), ImVec2(lx0, hy0), ImVec2(lx1, ImGui::GetCursorScreenPos().y));
    if (ImGui::BeginTable(sec.id, 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
      ImGui::TableSetupColumn("##on", ImGuiTableColumnFlags_WidthFixed, 30);
      ImGui::TableSetupColumn("##part", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("##order", ImGuiTableColumnFlags_WidthFixed, 60);
      ImGui::TableSetupColumn("##always", ImGuiTableColumnFlags_WidthFixed, 150);
      for (const tb::Part* part : parts) {
        const tb::PartInfo* info = tb::part(p, part->id);
        if (!info) continue;
        ImGui::PushID(part->id.c_str());
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        bool on = part->shown;
        if (ImGui::Checkbox("##on", &on)) edit = tb::setShown(bar, part->id, on) || edit;
        ImGui::TableNextColumn();
        {  // its name (with a grip when it can move): drag it to move the part
          const ImVec2 c0 = ImGui::GetCursorScreenPos();
          const float h = ImGui::GetFrameHeight(), pad = ImGui::GetStyle().CellPadding.y;
          ImGui::InvisibleButton("##name", ImVec2(std::max(1.0f, ImGui::GetContentRegionAvail().x), h));
          const bool hov = ImGui::IsItemHovered();
          if (info->movable && hov) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
          if (info->movable && hov && gBarDrag.dragging().empty())
            ImGui::SetTooltip("%s", sec.open ? tr("Drag to move it in the bar, also to another side")
                                                                                                                  : tr("Drag to move it in the bar"));
          gBarDrag.row(sec.id, part->id, tr(info->label), info->movable, ImVec2(lx0, c0.y - pad), ImVec2(lx1, c0.y + h + pad));
          ImDrawList* cl = ImGui::GetWindowDrawList();
          if (info->movable) ui::drawIcon(cl, ui::Icon::GripDots, ImVec2(c0.x + 6, c0.y + h * 0.5f), 12, hov ? P.textDim : P.textFaint);
          cl->AddText(ImVec2(c0.x + 16, c0.y + ImGui::GetStyle().FramePadding.y), on ? P.text : P.textDim, tr(info->label));
        }
        ImGui::TableNextColumn();
        if (info->movable) {
          if (ui::IconButton("##up", ui::Icon::ChevronUp, false, 22, "Earlier in the bar")) edit = tb::step(p, bar, part->id, -1) || edit;
          ImGui::SameLine(0, 2);
          if (ui::IconButton("##down", ui::Icon::ChevronDown, false, 22, "Later in the bar")) edit = tb::step(p, bar, part->id, +1) || edit;
        }
        ImGui::TableNextColumn();
        if (info->fitLevel > 0) {
          bool alw = part->always;
          if (ImGui::Checkbox(tr("Always shown"), &alw)) edit = tb::setAlways(bar, part->id, alw) || edit;
        }
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
  }
  edit = gBarDrag.finish(p, bar, dl, lx0, lx1) || edit;
  // When the window is narrow: the order the parts leave in.
  {
    std::string order;
    for (const std::vector<std::string>& level : tb::leaveOrder(p, bar)) {
      std::string group;
      for (const std::string& id : level)
        if (const tb::PartInfo* in = tb::part(p, id)) group += (group.empty() ? "" : " + ") + std::string(tr(in->label));
      if (!group.empty()) order += (order.empty() ? "" : ", then ") + group;
    }
    ImGui::Spacing();
    ImGui::TextColored(theme::vec(P.textFaint), "%s", tr("When the window is narrow, parts leave in this order:"));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::max(200.0f, ImGui::GetContentRegionAvail().x));
    ImGui::TextColored(theme::vec(P.textDim), "%s", order.empty() ? tr("Nothing leaves.") : order.c_str());
    ImGui::PopTextWrapPos();
  }
  if (ui::TextButton("Reset the bar", false, 0, "Puts this bar back as it was designed (the pinned commands stay)")) bar = tb::defaults(p), edit = true;
  if (edit) toolbarui::setBar(a, p, bar);
  ui::EndSection();
}

// ---- panels: which pane each window lives in ----
void panelsSection(App& a, Preset p) {
  const auto& P = theme::pal();
  ui::BeginSection(tr("PANELS"));
  if (p == Preset::Studio) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(theme::vec(P.textDim), "%s",
                       tr("Studio's windows dock freely: drag a window's tab to another pane, or out to float. Each workspace keeps where they are."));
    ImGui::PopTextWrapPos();
    ui::EndSection();
    return;
  }
  layout::State st = layoutui::regionState(a, p);
  std::vector<const layout::Region*> panes;
  for (const layout::Region& r : layout::shell(p).regions)
    if (layout::hasPaneTabs(r)) panes.push_back(&r);
  bool changed = false;
  if (ImGui::BeginTable("##panels", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
    ImGui::TableSetupColumn(tr("Window"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn(tr("Pane"), ImGuiTableColumnFlags_WidthFixed, 180);
    ImGui::TableSetupColumn(tr("Open at start"), ImGuiTableColumnFlags_WidthFixed, 100);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 90);
    ImGui::TableHeadersRow();
    const int k = static_cast<int>(p);
    for (const layout::Item& it : layout::items()) {
      if (!it.window || !*it.window) continue;
      const layout::Home& h = it.home[k];
      if (h.kind != layout::HomeKind::Dock && h.kind != layout::HomeKind::Part) continue;
      const std::string here = layout::tabRegion(p, st, it.window);
      if (here.empty()) continue;
      bool movable = false;
      for (const layout::Region* r : panes) movable = movable || layout::canMoveTab(p, it.window, r->id);
      if (!movable) continue;
      ImGui::PushID(it.window);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(tr(it.name));
      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(-1);
      auto paneName = [&](const std::string& id) {
        std::string n = id.substr(id.find('.') + 1);
        if (!n.empty()) n[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(n[0])));
        return n;
      };
      if (ImGui::BeginCombo("##pane", paneName(here).c_str())) {
        for (const layout::Region* r : panes) {
          if (!layout::canMoveTab(p, it.window, r->id)) continue;
          if (ImGui::Selectable(paneName(r->id).c_str(), here == r->id)) changed = layout::moveTab(p, st, it.window, r->id) || changed;
        }
        ImGui::EndCombo();
      }
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      ImGui::TextColored(theme::vec(P.textDim), "%s", h.openByDefault ? tr("yes") : tr("no"));
      ImGui::TableNextColumn();
      const bool home = !st.moved.count(it.window);
      ImGui::BeginDisabled(home);
      if (ui::TextButton("Back home", false, 0, "Back to the pane it was designed for")) changed = layout::moveTab(p, st, it.window, h.region) || changed;
      ImGui::EndDisabled();
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  ImGui::TextColored(theme::vec(P.textFaint), "%s", tr("The main area keeps its own view; dragging a tab between panes does the same."));
  if (changed) {
    layoutui::setRegionState(a, p, st);
    if (p == layoutui::active()) layoutui::requestShellRebuild();
  }
  ui::EndSection();
}

// ---- workspaces ----
void workspacesSection(App& a, Preset p) {
  const auto& P = theme::pal();
  ui::BeginSection(tr("WORKSPACES"));
  const bool here = p == layoutui::active();
  const std::vector<workspacesui::Info> list = workspacesui::list(a, p);
  if (!here) {
    char line[160];
    std::snprintf(line, sizeof line, tr("Switch to the %s layout to change its workspaces."), tr(layout::displayName(p)));
    ImGui::TextColored(theme::vec(P.textDim), "%s", line);
  }
  ImGui::BeginDisabled(!here);
  if (ImGui::BeginTable("##ws", 8, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
    ImGui::TableSetupColumn(tr("Name"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn(tr("Key"), ImGuiTableColumnFlags_WidthFixed, 110);
    ImGui::TableSetupColumn(tr("Opens"), ImGuiTableColumnFlags_WidthFixed, 70);
    ImGui::TableSetupColumn(tr("Windows"), ImGuiTableColumnFlags_WidthFixed, 64);
    ImGui::TableSetupColumn(tr("Pane sizes"), ImGuiTableColumnFlags_WidthFixed, 74);
    ImGui::TableSetupColumn(tr("Own bar"), ImGuiTableColumnFlags_WidthFixed, 64);
    ImGui::TableSetupColumn(tr("Own pins"), ImGuiTableColumnFlags_WidthFixed, 64);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 150);
    ImGui::TableHeadersRow();
    int dup = -1, close = -1;
    for (int i = 0; i < static_cast<int>(list.size()); ++i) {
      const workspacesui::Info& w = list[static_cast<size_t>(i)];
      ImGui::PushID(i);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      if (gRenaming == i) {
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##name", gRename, sizeof gRename, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll))
          workspacesui::rename(a, i, gRename), gRenaming = -1;
        if (ImGui::IsItemDeactivated()) gRenaming = -1;
      } else {
        if (ImGui::Selectable(w.name.c_str(), w.active, ImGuiSelectableFlags_AllowDoubleClick) && here) {
          if (ImGui::IsMouseDoubleClicked(0)) gRenaming = i, std::snprintf(gRename, sizeof gRename, "%s", w.name.c_str());
          else if (!w.active) workspacesui::select(a, i);
        }
      }
      ImGui::TableNextColumn();
      ImGui::TextColored(theme::vec(P.textDim), "%s", w.key.empty() ? "-" : w.key.c_str());
      ImGui::TableNextColumn();
      ImGui::Text("%d", static_cast<int>(w.open.size()));
      const bool keeps[4] = {w.keepWindows, w.keepPanes, w.ownBar, w.ownPins};
      for (int k = 0; k < 4; ++k) {
        ImGui::TableNextColumn();
        bool on = keeps[k];
        ImGui::PushID(k);
        if (ImGui::Checkbox("##keep", &on)) workspacesui::setKeeps(a, i, k, on);
        ImGui::PopID();
      }
      ImGui::TableNextColumn();
      if (ui::TextButton("Duplicate", false, 0)) dup = i;
      ImGui::SameLine(0, 4);
      ImGui::BeginDisabled(list.size() <= 1);
      if (ui::TextButton("Close", false, 0)) close = i;
      ImGui::EndDisabled();
      ImGui::PopID();
    }
    ImGui::EndTable();
    if (dup >= 0) workspacesui::duplicate(a, dup);
    if (close >= 0) workspacesui::close(a, close);
  }
  if (ui::TextButton("New from the current arrangement", false, 0)) workspacesui::addFromNow(a);
  ImGui::SameLine(0, theme::kS2);
  if (ui::TextButton("Add starter workspaces", false, 0)) workspacesui::starters(a);
  ImGui::SameLine(0, theme::kS2);
  if (ui::TextButton("Import from the project copy", false, 0, "This layout's workspaces as the project's copy keeps them"))
    if (!workspacesui::importProjectCopy(a)) a.notify(i18n::trMessage("This project keeps no copy of this layout's workspaces"));
  bool autoSave = workspacesui::autoSave(a);
  if (ImGui::Checkbox(tr("Save workspace changes automatically"), &autoSave)) workspacesui::setAutoSave(a, autoSave);
  ImGui::EndDisabled();
  bool copy = workspacesui::projectCopyOn(a);
  if (ImGui::Checkbox(tr("Keep a copy in the project"), &copy)) workspacesui::setProjectCopy(a, copy);
  ImGui::SameLine();
  ImGui::TextColored(theme::vec(P.textFaint), "%s", tr("(for another computer; your own workspaces are never shared)"));
  ui::EndSection();
}

// ---- pinned commands ----
void pinsSection(App& a, Preset p) {
  const auto& P = theme::pal();
  ui::BeginSection(tr("PINNED COMMANDS"));
  std::vector<tb::Pin> pins = toolbarui::modelFor(a, p).pins;
  bool edit = false;
  if (pins.empty()) ImGui::TextColored(theme::vec(P.textDim), "%s", tr("Nothing pinned yet. Find a command below, or press Ctrl+Enter on one in the command palette."));
  if (!pins.empty() && ImGui::BeginTable("##pins", 5, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
    ImGui::TableSetupColumn(tr("Command"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn(tr("Shows on"), ImGuiTableColumnFlags_WidthFixed, 150);
    ImGui::TableSetupColumn(tr("Look"), ImGuiTableColumnFlags_WidthFixed, 90);
    ImGui::TableSetupColumn(tr("Order"), ImGuiTableColumnFlags_WidthFixed, 60);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 30);
    ImGui::TableHeadersRow();
    int remove = -1;
    for (int i = 0; i < static_cast<int>(pins.size()); ++i) {
      tb::Pin& pn = pins[static_cast<size_t>(i)];
      ImGui::PushID(i);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(toolbarui::pinLabel(pn).c_str());
      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(-1);
      if (ImGui::BeginCombo("##where", tr(tb::whereName(pn.where)))) {
        for (tb::Where w : {tb::Where::Both, tb::Where::Bar, tb::Where::Palette})
          if (ImGui::Selectable(tr(tb::whereName(w)), pn.where == w)) pn.where = w, edit = true;
        ImGui::EndCombo();
      }
      ImGui::TableNextColumn();
      int look = pn.icon ? 1 : 0;
      const char* looks[] = {"Label", "Icon"};
      if (ui::Segmented("##look", looks, 2, &look, 42, ImGui::GetFrameHeight())) pn.icon = look == 1, edit = true;
      ImGui::TableNextColumn();
      if (ui::IconButton("##up", ui::Icon::ChevronUp, false, 22, "Earlier")) edit = tb::movePin(pins, i, i - 1) || edit;
      ImGui::SameLine(0, 2);
      if (ui::IconButton("##down", ui::Icon::ChevronDown, false, 22, "Later")) edit = tb::movePin(pins, i, i + 1) || edit;
      ImGui::TableNextColumn();
      if (ui::IconButton("##x", ui::Icon::Close, false, 22, "Unpin")) remove = i;
      ImGui::PopID();
    }
    ImGui::EndTable();
    if (remove >= 0) edit = tb::unpin(pins, pins[static_cast<size_t>(remove)].command) || edit;
  }
  // Pin a command: find it by name.
  ImGui::SetNextItemWidth(std::min(360.0f, ImGui::GetContentRegionAvail().x));
  ImGui::InputTextWithHint("##pinfind", tr("Pin a command: type its name"), gPinFind, sizeof gPinFind);
  if (gPinFind[0]) {
    std::vector<cmdpal::Entry> rows;
    for (const cmdpalette::CommandInfo& c : cmdpalette::commandList(a)) {
      cmdpal::Entry e;
      e.id = c.id, e.kind = c.kind, e.label = c.label, e.path = c.path, e.order = static_cast<int>(rows.size());
      if (cmdpal::pinnableRow(e)) rows.push_back(e);
    }
    int shown = 0;
    for (const cmdpal::Ranked& r : cmdpal::rank(rows, gPinFind, {})) {
      const cmdpal::Entry& e = rows[static_cast<size_t>(r.index)];
      const bool on = tb::pinned(pins, e.id);
      ImGui::PushID(e.id.c_str());
      std::string line = e.label + (e.path.empty() ? std::string() : "   " + e.path);
      if (ImGui::Selectable(line.c_str(), on)) {
        if (on) edit = tb::unpin(pins, e.id) || edit;
        else if (tb::pin(pins, tb::Pin{e.id, e.label})) edit = true;
        else a.notify(i18n::trMessage("The pinned list is full"), true);
      }
      ImGui::PopID();
      if (++shown >= 8) break;
    }
  }
  if (edit) toolbarui::setPins(a, p, pins);
  ui::EndSection();
}

// ---- switching ----
void switchingSection(App& a) {
  ui::BeginSection(tr("SWITCHING"));
  int when = std::clamp(static_cast<int>(a.settings.getNumber("ui.workspaceStrip")), 0, 2);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(tr("Show the workspace strip:"));
  const char* opts[] = {"Always", "With two or more", "Never"};
  for (int i = 0; i < 3; ++i) {
    ImGui::SameLine(0, theme::kS3);
    if (ImGui::RadioButton(tr(opts[i]), when == i)) a.settings.set("ui.workspaceStrip", i);
  }
  ImGui::TextColored(theme::vec(theme::pal().textFaint), "%s", tr("The keys and View > Workspaces switch whatever is chosen here."));
  ui::EndSection();
}

// ---- Studio: zone keys ----
void zonesSection(App& a) {
  const auto& P = theme::pal();
  ui::BeginSection(tr("ZONE KEYS"));
  const bool taken = zonekeysui::keySetTakesTab();
  ImGui::BeginDisabled(taken);
  bool on = zonekeysui::enabled(a);
  if (ui::ToggleRow(tr("Zone keys: hold Tab and press a digit"), &on)) a.settings.set("studio.zoneKeys", on);
  ImGui::EndDisabled();
  ImGui::PushTextWrapPos(0);
  if (taken)
    ImGui::TextColored(theme::vec(P.textDim), "%s", tr("The key set in use gives plain Tab its own work (the Clips and Tracker sets), so zone keys are off."));
  else
    ImGui::TextColored(theme::vec(P.textFaint), "%s",
                       tr("Tab and a digit shows that zone's window; the same digit again swaps a zone's two windows. Tab alone goes back to the zone before."));
  ImGui::PopTextWrapPos();
  zones::Zones z = zonekeysui::load(a);
  bool edit = false;
  ImGui::BeginDisabled(taken || !on);
  if (ImGui::BeginTable("##zones", 3, ImGuiTableFlags_SizingFixedFit)) {
    ImGui::TableSetupColumn(tr("Key"), ImGuiTableColumnFlags_WidthFixed, 70);
    ImGui::TableSetupColumn(tr("Window"), ImGuiTableColumnFlags_WidthFixed, 170);
    ImGui::TableSetupColumn(tr("Then (the same key again)"), ImGuiTableColumnFlags_WidthFixed, 170);
    ImGui::TableHeadersRow();
    for (int k = 0; k < zones::kZones; ++k) {
      ImGui::PushID(k);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      ImGui::Text("Tab+%d", k + 1);
      for (int s = 0; s < 2; ++s) {
        ImGui::TableNextColumn();
        std::string& w = z[static_cast<size_t>(k)].windows[static_cast<size_t>(s)];
        ImGui::PushID(s);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##w", w.empty() ? tr("(none)") : tr(w.c_str()))) {
          if (ImGui::Selectable(tr("(none)"), w.empty())) w.clear(), edit = true;
          for (const char* c : zonekeysui::choices())
            if (ImGui::Selectable(tr(c), w == c)) w = c, edit = true;
          ImGui::EndCombo();
        }
        ImGui::PopID();
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  if (ui::TextButton("Reset the zones", false, 0)) z = zones::defaults(), edit = true;
  ImGui::EndDisabled();
  if (edit) zonekeysui::store(a, z);
  ui::EndSection();
}

}  // namespace

void showLayoutsPageFor(int l) { gLayout = l; }

void layoutsSettingsPage(App& a, float fieldW) {
  (void)fieldW;
  if (gLayout < 0 || gLayout >= layout::kLayoutCount) gLayout = static_cast<int>(layoutui::active());
  {  // the layout this page shows
    const char* names[layout::kLayoutCount];
    for (int k = 0; k < layout::kLayoutCount; ++k) names[k] = layout::displayName(static_cast<Preset>(k));
    ui::Segmented("##layoutpick", names, layout::kLayoutCount, &gLayout, 90, ImGui::GetFrameHeight());
    ImGui::SameLine(0, theme::kS3);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::vec(theme::pal().textFaint), "%s", tr("Applies to this layout and its workspaces; nothing here goes into a project."));
  }
  const Preset p = static_cast<Preset>(gLayout);
  barSection(a, p);
  panelsSection(a, p);
  workspacesSection(a, p);
  pinsSection(a, p);
  switchingSection(a);
  if (p == Preset::Studio) zonesSection(a);
}

}  // namespace gl
