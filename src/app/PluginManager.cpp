// Gridline — Options > Plugin manager: scan folders, formats, the plugin
// list with favourites / categories / "Run in sandbox", and failed plugins
// with retry. Layout follows the usual plugin-manager arrangement (folders
// and scan controls on the left, the plugin table on the right, details of
// the selected plugin below it), drawn in Gridline's own style.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include "../host/HostedPlugins.h"
#include "../host/PluginPrefs.h"
#include "../host/PluginScan.h"
#include "App.h"
#include "HostedUi.h"
#include "PluginUI.h"
#include "Theme.h"
#include "Widgets.h"
#include "Windows.h"

namespace fs = std::filesystem;

namespace gl {

using theme::pal;

namespace {

struct ManagerState {
  char search[96] = "";
  char folder[512] = "";
  char category[64] = "";
  int view = 0;  // 0 all, 1 instruments, 2 effects, 3 favourites, 4 failed
  std::string selected;  // plugin type
  std::string categoryFor;
};
ManagerState& state() {
  static ManagerState s;
  return s;
}

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

void sectionLabel(const char* text) {
  ImGui::Dummy(ImVec2(0, 4));
  ui::Label(text);
}

// Left column: scan controls, folders, formats.
void leftPanel(App& a, host::PluginPrefs& prefs, bool& changed) {
  const auto& P = pal();
  const host::ScanStatus ss = host::scanStatus();
  sectionLabel("SCAN");
  if (ss.running) {
    ImGui::TextColored(theme::vec(P.text), "Scanning %d / %d", std::min(ss.done + 1, std::max(1, ss.total)), ss.total);
    ImGui::PushFont(a.fontMono, 10.5f);
    ImGui::TextColored(theme::vec(P.textFaint), "%s", theme::fitText(fs::path(ss.current).filename().string().c_str(), 230));
    ImGui::PopFont();
  } else {
    if (ui::TextButton("Find plugins", false, 110, "Scans new and updated plugins in the folders below (each in its own process)"))
      { host::startScan(host::selfExecutablePath(), false); a.scanByUser = true; }
    ImGui::SameLine(0, 6);
    if (ui::TextButton("Rescan all", false, 110, "Scans every plugin again, including failed ones")) { host::startScan(host::selfExecutablePath(), true); a.scanByUser = true; }
  }
  ImGui::TextColored(theme::vec(P.textDim), "%d plugins  \xC2\xB7  %d failed", ss.classes, ss.failed);

  sectionLabel("FORMATS");
  for (const host::Format& f : host::formats()) {
    bool on = !prefs.disabledFormats.count(f.id);
    if (ui::TextButton(f.label, on, 54, on ? "Listed and scanned (click to switch off)" : "Not scanned, hidden from the browser (click to switch on)")) {
      if (on)
        prefs.disabledFormats.insert(f.id);
      else
        prefs.disabledFormats.erase(f.id);
      changed = true;
    }
    ImGui::SameLine(0, 4);
  }
  ImGui::NewLine();

  sectionLabel("SEARCH FOLDERS");
  ImGui::BeginChild("##folders", ImVec2(0, 230), ImGuiChildFlags_Borders);
  ImGui::PushFont(a.fontMono, 10.5f);
  for (const host::Format& f : host::formats()) {
    if (prefs.disabledFormats.count(f.id)) continue;
    for (const auto& p : f.defaultPaths()) {
      std::error_code ec;
      const bool exists = fs::is_directory(p, ec);
      ImGui::TextColored(theme::vec(exists ? P.textDim : P.textFaint), "%-5s %s", f.label, theme::fitText(p.c_str(), 200));
      if (ImGui::IsItemHovered()) ui::Tooltip((p + (exists ? "" : "  (does not exist)")).c_str());
    }
  }
  ImGui::PopFont();
  int removeAt = -1;
  for (size_t i = 0; i < prefs.extraFolders.size(); ++i) {
    ImGui::PushID(static_cast<int>(i));
    if (ui::IconButton("rm", ui::Icon::Close, false, 18, "Remove this folder")) removeAt = static_cast<int>(i);
    ImGui::SameLine(0, 4);
    ImGui::PushFont(a.fontMono, 10.5f);
    ImGui::TextColored(theme::vec(P.text), "%s", theme::fitText(prefs.extraFolders[i].c_str(), 200));
    ImGui::PopFont();
    ImGui::PopID();
  }
  if (removeAt >= 0) {
    prefs.extraFolders.erase(prefs.extraFolders.begin() + removeAt);
    changed = true;
  }
  ImGui::EndChild();
  ManagerState& ms = state();
  ImGui::SetNextItemWidth(-1);
  const bool enter = ImGui::InputTextWithHint("##addfolder", "Add a folder (path)", ms.folder, sizeof ms.folder, ImGuiInputTextFlags_EnterReturnsTrue);
  if ((ui::TextButton("Add folder", false, 0, "Also search this folder for VST3, CLAP and VST2 plugins") || enter) && ms.folder[0]) {
    std::string f = ms.folder;
    std::error_code ec;
    if (!fs::is_directory(f, ec)) {
      a.notify("Not a folder: " + f, true);
    } else if (std::find(prefs.extraFolders.begin(), prefs.extraFolders.end(), f) == prefs.extraFolders.end()) {
      prefs.extraFolders.push_back(f);
      changed = true;
      ms.folder[0] = 0;
    }
  }
}

void detailPanel(App& a, const host::HostedClass& c, host::PluginPrefs& prefs, bool& changed) {
  const auto& P = pal();
  ManagerState& ms = state();
  ImGui::PushFont(a.fontBold, 14.0f);
  ImGui::TextUnformatted(c.name.c_str());
  ImGui::PopFont();
  ImGui::SameLine(0, 10);
  ImGui::TextColored(theme::vec(P.textDim), "%s  \xC2\xB7  %s  %s", c.vendor.c_str(), c.formatLabel(), c.version.c_str());
  ImGui::PushFont(a.fontMono, 10.5f);
  ImGui::TextColored(theme::vec(P.textFaint), "%s", theme::fitText(c.type.c_str(), ImGui::GetContentRegionAvail().x));
  ImGui::TextColored(theme::vec(P.textFaint), "%s", theme::fitText(c.bundle.c_str(), ImGui::GetContentRegionAvail().x));
  ImGui::PopFont();
  ImGui::TextColored(theme::vec(P.textDim), "%s  \xC2\xB7  %zu parameters  \xC2\xB7  %zu programs  \xC2\xB7  %s%s",
                     c.instrument ? "Instrument" : "Effect", c.params.size(), c.programs.size(),
                     c.auxOutputs.empty() ? "stereo out" : (std::to_string(c.auxOutputs.size() + 1) + " outputs").c_str(),
                     c.sidechain ? "  \xC2\xB7  sidechain input" : "");
  const bool fav = prefs.favourites.count(c.type) > 0;
  if (ui::TextButton(fav ? "Favourite" : "Add to favourites", fav, 0, "Favourites are listed first in the browser")) {
    if (fav)
      prefs.favourites.erase(c.type);
    else
      prefs.favourites.insert(c.type);
    changed = true;
  }
  ImGui::SameLine(0, 6);
  const bool sb = prefs.sandbox.count(c.type) > 0;
  if (ui::TextButton("Run in sandbox", sb, 0, "Runs this plugin in its own process: a crash silences it instead of closing Gridline"))
    setHostedSandbox(a, c.type, !sb);  // saves the setting itself
  ImGui::SameLine(0, 6);
  if (ui::TextButton("Rescan", false, 0, "Scans this plugin again")) host::rescanEntry(host::selfExecutablePath(), c.format, c.bundle);
  ImGui::SameLine(0, 12);
  if (ms.categoryFor != c.type) {
    ms.categoryFor = c.type;
    auto it = prefs.categories.find(c.type);
    std::snprintf(ms.category, sizeof ms.category, "%s", it == prefs.categories.end() ? "" : it->second.c_str());
  }
  ImGui::SetNextItemWidth(170);
  if (ImGui::InputTextWithHint("##cat", "Category (e.g. Bass, Drums)", ms.category, sizeof ms.category)) {
    if (ms.category[0])
      prefs.categories[c.type] = ms.category;
    else
      prefs.categories.erase(c.type);
    changed = true;
  }
}

}  // namespace

void drawPluginManager(App& a) {
  if (!a.showPluginManager) return;
  const auto& P = pal();
  ManagerState& ms = state();
  ui::PlaceNextDialog(ImVec2(1060, 640), ImGuiCond_Appearing, ImVec2(720, 400));  // J-54: fits short / scaled screens
  if (!ImGui::Begin("Plugin manager", &a.showPluginManager, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    return;
  }
  ui::KeepWindowInViewport();
  host::PluginPrefs prefs = host::pluginPrefs();
  bool changed = false;

  // ---- left: scan, formats, folders ----
  ImGui::BeginChild("##left", ImVec2(270, 0));
  leftPanel(a, prefs, changed);
  ImGui::EndChild();
  ImGui::SameLine(0, 12);

  // ---- right: filter bar, table, details ----
  ImGui::BeginChild("##right", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  static const char* const views[] = {"All", "Instruments", "Effects", "Favourites", "Failed"};
  ui::Segmented("##view", views, 5, &ms.view, 92);
  ImGui::SameLine(0, 12);
  ImGui::SetNextItemWidth(std::max(100.0f, std::min(220.0f, ImGui::GetContentRegionAvail().x)));  // J-54: shrinks on narrow screens
  ImGui::InputTextWithHint("##search", "Search name, vendor, category", ms.search, sizeof ms.search);
  const std::string q = lower(ms.search);

  const float detailH = ms.view == 4 ? 0.0f : 132.0f;  // J-54: name, type, bundle, summary and the button row fit without scrolling
  // J-54: table + gap + detail box fill the panel exactly (the box's lower edge stays visible).
  const float gapY = ImGui::GetStyle().ItemSpacing.y;
  const float tableH = std::max(120.0f, ImGui::GetContentRegionAvail().y - (detailH > 0 ? detailH + gapY : 0.0f));
  if (ms.view == 4) {  // failed / blacklisted entries with their reason
    if (ImGui::BeginTable("##failed", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable,
                          ImVec2(0, tableH))) {
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthFixed, 56);
      ImGui::TableSetupColumn("Plugin", ImGuiTableColumnFlags_WidthStretch, 1.0f);
      ImGui::TableSetupColumn("Reason", ImGuiTableColumnFlags_WidthStretch, 1.4f);
      ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 64);
      ImGui::TableHeadersRow();
      int row = 0;
      const bool busy = host::scanStatus().running;
      for (const auto& e : host::cacheEntries()) {
        if (e.ok) continue;
        if (!q.empty() && lower(e.entry).find(q) == std::string::npos) continue;
        const host::Format* f = host::findFormat(e.format);
        ImGui::TableNextRow();
        ImGui::PushID(row++);
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(f ? f->label : e.format.c_str());
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(fs::path(e.entry).filename().string().c_str());
        if (ImGui::IsItemHovered()) ui::Tooltip(e.entry.c_str());
        ImGui::TableSetColumnIndex(2);
        ImGui::TextColored(theme::vec(P.danger), "%s", e.error.c_str());
        ImGui::TableSetColumnIndex(3);
        if (!busy && ui::TextButton("Retry", false, 56, "Scan this plugin again")) host::rescanEntry(host::selfExecutablePath(), e.format, e.entry);
        ImGui::PopID();
      }
      if (row == 0) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(1);
        ImGui::TextColored(theme::vec(P.textFaint), "No failed plugins.");
      }
      ImGui::EndTable();
    }
  } else {
    std::vector<std::shared_ptr<const host::HostedClass>> list;
    for (const auto& c : host::hostedClasses()) {
      if (prefs.disabledFormats.count(c->format)) continue;
      if (ms.view == 1 && !c->instrument) continue;
      if (ms.view == 2 && c->instrument) continue;
      if (ms.view == 3 && !prefs.favourites.count(c->type)) continue;
      auto cat = prefs.categories.find(c->type);
      const std::string userCat = cat == prefs.categories.end() ? std::string() : cat->second;
      if (!q.empty() && lower(c->name).find(q) == std::string::npos && lower(c->vendor).find(q) == std::string::npos &&
          lower(userCat).find(q) == std::string::npos && lower(c->subCategories).find(q) == std::string::npos)
        continue;
      list.push_back(c);
    }
    std::sort(list.begin(), list.end(), [](const auto& x, const auto& y) {
      std::string a1 = lower(x->name), b1 = lower(y->name);
      return a1 != b1 ? a1 < b1 : std::string(x->formatLabel()) < y->formatLabel();
    });
    const host::HostedClass* sel = nullptr;
    if (ImGui::BeginTable("##plugins", 7,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable,
                          ImVec2(0, tableH))) {
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("Fav", ImGuiTableColumnFlags_WidthFixed, 30);
      ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1.4f);
      ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthFixed, 52);
      ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 78);
      ImGui::TableSetupColumn("Vendor", ImGuiTableColumnFlags_WidthStretch, 1.0f);
      ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthStretch, 0.8f);
      ImGui::TableSetupColumn("Sandbox", ImGuiTableColumnFlags_WidthFixed, 60);
      ImGui::TableHeadersRow();
      ImGuiListClipper clip;
      clip.Begin(static_cast<int>(list.size()));
      while (clip.Step())
        for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
          const host::HostedClass& c = *list[static_cast<size_t>(r)];
          ImGui::TableNextRow();
          ImGui::PushID(c.type.c_str());
          ImGui::TableSetColumnIndex(0);
          const bool fav = prefs.favourites.count(c.type) > 0;
          ImVec2 p = ImGui::GetCursorScreenPos();
          if (ImGui::InvisibleButton("##fav", ImVec2(22, 18))) {
            if (fav)
              prefs.favourites.erase(c.type);
            else
              prefs.favourites.insert(c.type);
            changed = true;
          }
          ui::Led(ImGui::GetWindowDrawList(), ImVec2(p.x + 10, p.y + 9), 4, fav);
          ImGui::TableSetColumnIndex(1);
          if (ImGui::Selectable(c.name.c_str(), ms.selected == c.type, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
            ms.selected = c.type;
          ImGui::TableSetColumnIndex(2);
          ImGui::TextColored(theme::vec(P.textDim), "%s", c.formatLabel());
          ImGui::TableSetColumnIndex(3);
          ImGui::TextColored(theme::vec(P.textDim), "%s", c.instrument ? "Instrument" : "Effect");
          ImGui::TableSetColumnIndex(4);
          ImGui::TextUnformatted(c.vendor.c_str());
          ImGui::TableSetColumnIndex(5);
          auto cat = prefs.categories.find(c.type);
          ImGui::TextColored(theme::vec(cat == prefs.categories.end() ? P.textFaint : P.text), "%s",
                             cat == prefs.categories.end() ? theme::fitText(c.subCategories.c_str(), 140) : cat->second.c_str());
          ImGui::TableSetColumnIndex(6);
          const bool sb = prefs.sandbox.count(c.type) > 0;
          ImVec2 q2 = ImGui::GetCursorScreenPos();
          if (ImGui::InvisibleButton("##sb", ImVec2(40, 18))) setHostedSandbox(a, c.type, !sb);
          if (ImGui::IsItemHovered()) ui::Tooltip("Run in sandbox (own process)");
          ui::Led(ImGui::GetWindowDrawList(), ImVec2(q2.x + 10, q2.y + 9), 4, sb);
          ImGui::PopID();
        }
      ImGui::EndTable();
    }
    for (const auto& c : list)
      if (c->type == ms.selected) sel = c.get();
    ImGui::BeginChild("##detail", ImVec2(0, detailH), ImGuiChildFlags_Borders);
    if (sel)
      detailPanel(a, *sel, prefs, changed);
    else
      ImGui::TextColored(theme::vec(P.textFaint), list.empty() ? "No plugins match." : "Select a plugin to see its details.");
    ImGui::EndChild();
  }
  ImGui::EndChild();
  if (changed) {
    // Sandbox switches were saved by setHostedSandbox: keep them.
    host::PluginPrefs now = host::pluginPrefs();
    prefs.sandbox = now.sandbox;
    host::setPluginPrefs(prefs);
  }
  ImGui::End();
}

}  // namespace gl
