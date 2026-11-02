// Gridline — K-13: Settings > Plugins (see PluginFoldersUi.h).
#include "PluginFoldersUi.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "../../third_party/imgui/imgui.h"
#include "../core/I18n.h"
#include "../core/Platform.h"
#include "../host/HostedPlugins.h"
#include "../host/PluginFolders.h"
#include "../host/PluginPrefs.h"
#include "../host/PluginScan.h"
#include "App.h"
#include "DialogLayout.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl {

using theme::pal;
using i18n::tr;
namespace fs = std::filesystem;

namespace {

// "~/..." for paths below the home folder (the full path is in the tooltip).
std::string shortPath(const std::string& p) {
  const std::string home = platform::homeDir();
  if (!home.empty() && p.size() > home.size() && p.compare(0, home.size(), home) == 0 && (p[home.size()] == '/' || p[home.size()] == '\\'))
    return "~" + p.substr(home.size());
  return p;
}

// The folder chooser (Gridline's own folder dialog); `format` "" adds a folder for every format.
void chooseFolder(App& a, const std::string& format) {
  a.fileDialog = {};
  a.fileDialog.open = true;
  a.fileDialog.title = format.empty() ? i18n::N_("Add a folder for every plugin format") : i18n::N_("Add a plugin folder");
  a.fileDialog.dir = platform::homeDir();
  a.fileDialog.exts = {"/"};  // folder mode
  a.fileDialog.onOk = [format](const std::string& dir) {
    host::PluginPrefs p = host::pluginPrefs();
    std::vector<std::string>& list = format.empty() ? p.extraFolders : p.formatFolders[format];
    for (const auto& x : list)
      if (host::sameFolder(x, dir)) return;  // already listed
    list.push_back(dir);
    p.disabledFolders.erase(host::folderKey(format.empty() ? "*" : format, dir));
    host::setPluginPrefs(p);
    host::requestScan(host::selfExecutablePath());
  };
}

struct RowState {
  const char* text = nullptr;  // English (translated when drawn)
  ImU32 col = 0;
  std::string tip;
};

RowState rowState(const host::FolderRow& r, bool userRow) {
  const auto& P = pal();
  RowState s;
  if (!r.exists) {
    s.text = i18n::N_("Missing");
    s.col = userRow ? P.danger : P.textFaint;
    s.tip = tr("Not found on this computer: nothing to scan here");
  } else if (!r.enabled) {
    s.text = i18n::N_("Off");
    s.col = P.textFaint;
    s.tip = tr("Switched off: not searched; its plugins leave the list (projects keep them as placeholders)");
  } else if (r.duplicate) {
    s.text = i18n::N_("Listed above");
    s.col = P.textFaint;
    s.tip = tr("The same folder as an earlier row: searched once");
  } else if (!r.inside.empty()) {
    s.text = i18n::N_("Inside another");
    s.col = P.textDim;
    s.tip = std::string(tr("Lies inside a searched folder: its plugins are found once")) + "\n" + r.inside;
  }
  return s;
}

enum RowAction { kNone, kToggle, kRescan, kRemove };

// One folder row: [switch] [tag] path ............ state [rescan] [remove]
RowAction folderRow(App& a, const host::FolderRow& r, float indent, bool switchable, bool removable, const char* tag, bool dim, bool scanning) {
  const auto& P = pal();
  const float h = ImGui::GetFrameHeight(), icon = h;
  RowAction act = kNone;
  ImGui::PushID((r.format + "|" + std::to_string(static_cast<int>(r.source)) + "|" + r.path).c_str());
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
  if (switchable) {
    bool on = r.enabled;
    if (ImGui::Checkbox("##on", &on)) act = kToggle;
    if (ImGui::IsItemHovered()) ui::Tooltip(r.enabled ? "Search this folder (click to switch it off)" : "Not searched (click to switch it on)");
  } else {
    ImGui::Dummy(ImVec2(h, h));
  }
  ImGui::SameLine(0, theme::kS2);
  if (tag && *tag) {
    ImGui::PushFont(a.fontMono, theme::kFontLabel + 0.5f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::vec(P.textDim), "%s", tag);
    ImGui::PopFont();
    ImGui::SameLine(0, theme::kS2);
  }
  const RowState st = rowState(r, switchable);
  const char* stText = st.text ? tr(st.text) : "";
  const bool canRescan = r.searched() && host::folderSearchApplies(r.format);
  const int buttons = 2;  // rescan and remove slots on every row, so the columns line up
  const float stW = st.text ? ImGui::CalcTextSize(stText).x : 0.0f;
  const float x0 = ImGui::GetCursorPosX(), avail = ImGui::GetContentRegionAvail().x;
  const float rightW = static_cast<float>(buttons) * (icon + theme::kS1) + (stW > 0 ? stW + theme::kS3 : 0.0f);
  const float pathW = std::max(40.0f, avail - rightW);
  ImGui::PushFont(a.fontMono, theme::kFontLabel + 0.5f);
  ui::PathFit(shortPath(r.path).c_str(), pathW, dim || !r.searched() ? P.textFaint : P.text, true);
  ImGui::PopFont();
  if (ImGui::IsItemHovered() && !st.text) ui::Tooltip(r.path.c_str());
  float x = x0 + pathW + theme::kS3;
  if (st.text) {
    ImGui::SameLine(0, 0);
    ImGui::SetCursorPosX(x);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::vec(st.col), "%s", stText);
    if (ImGui::IsItemHovered() && !st.tip.empty()) ImGui::SetTooltip("%s", st.tip.c_str());
    x += stW;
  }
  x = x0 + avail - static_cast<float>(buttons) * (icon + theme::kS1) + theme::kS1;
  ImGui::SameLine(0, 0);
  ImGui::SetCursorPosX(x);
  if (canRescan) {
    ImGui::BeginDisabled(scanning);
    if (ui::IconButton("##rescan", ui::Icon::LoopArrows, false, icon, "Scan the plugins in this folder again")) act = kRescan;
    ImGui::EndDisabled();
  } else {
    ImGui::Dummy(ImVec2(icon, icon));
  }
  ImGui::SameLine(0, theme::kS1);
  if (removable) {
    if (ui::IconButton("##rm", ui::Icon::Trash, false, icon, "Remove this folder from the list")) act = kRemove;
  } else {
    ImGui::Dummy(ImVec2(icon, icon));
  }
  ImGui::PopID();
  return act;
}

}  // namespace

void openPluginFolderSettings(App& a) {
  a.showSettings = true;
  a.settingsTab = kPluginSettingsTab;
}

void pluginFoldersSettingsPage(App& a, float fieldW) {
  (void)fieldW;
  const auto& P = pal();
  const std::string exe = host::selfExecutablePath();
  const host::ScanStatus ss = host::scanStatus();
  host::PluginPrefs prefs = host::pluginPrefs();
  bool changed = false;

  // ---- scanning ----
  ui::BeginSection(tr("SCANNING"));
  {
    char line[160];
    if (ss.running) {
      std::snprintf(line, sizeof line, tr("Scanning %d / %d"), std::min(ss.done + 1, std::max(1, ss.total)), ss.total);
    } else if (ss.failed > 0) {
      std::snprintf(line, sizeof line, tr("%d plugins found, %d failed to scan"), ss.classes, ss.failed);
    } else {
      std::snprintf(line, sizeof line, tr("%d plugins found"), ss.classes);
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::vec(ss.running ? P.text : P.textDim), "%s", line);
    if (ss.running && !ss.current.empty()) {
      ImGui::SameLine(0, theme::kS2);
      ImGui::PushFont(a.fontMono, theme::kFontLabel + 0.5f);
      ui::TextFit(fs::path(ss.current).filename().string().c_str(), std::max(40.0f, ImGui::GetContentRegionAvail().x), P.textFaint, true);
      ImGui::PopFont();
    }
    ImGui::BeginDisabled(ss.running);
    if (ui::TextButton("Scan all", true, 0, "Finds new and changed plugins in every folder that is on (each plugin is loaded in its own process)")) {
      host::startScan(exe, false);
      a.scanByUser = true;
    }
    ImGui::SameLine(0, theme::kS2);
    if (ui::TextButton("Rescan all", false, 0, "Scans every plugin again, including the ones that failed")) {
      host::startScan(exe, true);
      a.scanByUser = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0, theme::kS2);
    if (ui::TextButton("Plugin Manager", false, 0, "Every plugin found: formats, favourites, sandbox, failed scans")) a.showPluginManager = true;
  }
  ui::EndSection();

  std::map<std::string, int> perFormat;
  for (const auto& e : host::cacheEntries())
    if (e.ok) perFormat[e.format] += std::max(0, e.classes);

  // ---- per format ----
  ui::BeginSection(tr("PLUGIN FOLDERS"));
  ImGui::PushTextWrapPos(0);
  ImGui::TextColored(theme::vec(P.textFaint), "%s",
                     tr("Each format is searched in its default folders, its environment variable and the folders you add. Changes are scanned at once."));
  ImGui::PopTextWrapPos();
  const float indent = ImGui::GetFrameHeight() + theme::kS2;
  for (const host::Format& f : host::formats()) {
    const std::string id = f.id;
    ImGui::PushID(f.id);
    ImGui::Dummy(ImVec2(0, theme::kS1));
    // Format header: on / off (the Plugin Manager's format switch) and how many plugins it has.
    bool on = !prefs.disabledFormats.count(id);
    if (ImGui::Checkbox(f.label, &on)) {
      if (on) prefs.disabledFormats.erase(id);
      else prefs.disabledFormats.insert(id);
      changed = true;
    }
    if (ImGui::IsItemHovered()) ui::Tooltip(on ? "Scanned and listed (click to switch the format off)" : "Not scanned, hidden everywhere (click to switch it on)");
    ImGui::SameLine(0, theme::kS2);
    {
      char n[64];
      const auto it = perFormat.find(id);
      if (on) std::snprintf(n, sizeof n, tr("%d plugins"), it == perFormat.end() ? 0 : it->second);
      else std::snprintf(n, sizeof n, "%s", tr("switched off"));
      ImGui::AlignTextToFramePadding();
      ImGui::TextColored(theme::vec(P.textFaint), "%s", n);
    }
    const std::vector<host::FolderRow> rows = host::folderRows(f, prefs);
    const bool applies = host::folderSearchApplies(id);
    // Default: the OS's standard folders, one switch, listed below it.
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
    if (applies) {
      bool defOn = !prefs.noDefaultFolders.count(id);
      if (ImGui::Checkbox(tr("Default folders"), &defOn)) {
        if (defOn) prefs.noDefaultFolders.erase(id);
        else prefs.noDefaultFolders.insert(id);
        changed = true;
      }
      if (ImGui::IsItemHovered()) ui::Tooltip("The standard folders for this format on this computer (switch off to search only your own folders)");
    } else {
      ImGui::AlignTextToFramePadding();
      ImGui::TextColored(theme::vec(P.textDim), "%s", tr("Registered with the system: found wherever they are installed"));
    }
    for (const auto& r : rows) {
      if (r.source != host::FolderSource::Default) continue;
      if (folderRow(a, r, indent * 2, false, false, nullptr, !on, ss.running) == kRescan) host::rescanFolder(exe, id, r.path);
    }
    for (const auto& r : rows) {
      if (r.source != host::FolderSource::Env) continue;
      if (folderRow(a, r, indent, false, false, host::folderEnvVar(id), !on, ss.running) == kRescan) host::rescanFolder(exe, id, r.path);
    }
    int removeAt = -1, idx = 0;
    for (const auto& r : rows) {
      if (r.source != host::FolderSource::User) continue;
      const RowAction act = folderRow(a, r, indent, true, true, nullptr, !on, ss.running);
      const std::string key = host::folderKey(id, r.path);
      if (act == kToggle) {
        if (prefs.disabledFolders.count(key)) prefs.disabledFolders.erase(key);
        else prefs.disabledFolders.insert(key);
        changed = true;
      } else if (act == kRescan) {
        host::rescanFolder(exe, id, r.path);
      } else if (act == kRemove) {
        removeAt = idx;
      }
      ++idx;
    }
    if (removeAt >= 0) {
      auto& list = prefs.formatFolders[id];
      prefs.disabledFolders.erase(host::folderKey(id, list[static_cast<size_t>(removeAt)]));
      list.erase(list.begin() + removeAt);
      if (list.empty()) prefs.formatFolders.erase(id);
      changed = true;
    }
    if (applies) {
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
      const std::string label = std::string(tr("Add folder...")) + "##add";
      if (ui::TextButton(label.c_str(), false, 0, "Search one more folder for plugins of this format")) chooseFolder(a, id);
    }
    ImGui::PopID();
  }
  ui::EndSection();

  // ---- every format ----
  ui::BeginSection(tr("FOLDERS FOR EVERY FORMAT"));
  {
    int removeAt = -1;
    for (size_t i = 0; i < prefs.extraFolders.size(); ++i) {
      host::FolderRow r;
      r.format = "*";
      r.path = prefs.extraFolders[i];
      r.source = host::FolderSource::Every;
      r.enabled = !prefs.disabledFolders.count(host::folderKey("*", r.path));
      std::error_code ec;
      r.exists = fs::is_directory(r.path, ec);
      const RowAction act = folderRow(a, r, 0, true, true, nullptr, false, ss.running);
      if (act == kToggle) {
        const std::string key = host::folderKey("*", r.path);
        if (prefs.disabledFolders.count(key)) prefs.disabledFolders.erase(key);
        else prefs.disabledFolders.insert(key);
        changed = true;
      } else if (act == kRescan) {  // every format's plugins in it, one scan
        std::vector<std::pair<std::string, std::string>> entries;
        for (const host::Format& f : host::formats())
          if (host::folderSearchApplies(f.id) && !prefs.disabledFormats.count(f.id))
            for (auto& e : f.discover({r.path})) entries.push_back({f.id, std::move(e)});
        if (entries.empty()) host::requestScan(exe);
        else host::rescanEntries(exe, entries);
      } else if (act == kRemove) {
        removeAt = static_cast<int>(i);
      }
    }
    if (removeAt >= 0) {
      prefs.disabledFolders.erase(host::folderKey("*", prefs.extraFolders[static_cast<size_t>(removeAt)]));
      prefs.extraFolders.erase(prefs.extraFolders.begin() + removeAt);
      changed = true;
    }
    if (prefs.extraFolders.empty()) {
      ImGui::TextColored(theme::vec(P.textFaint), "%s", tr("None: a folder added here is searched for VST3, VST2, CLAP and LV2 plugins."));
    }
    if (ui::TextButton("Add folder...", false, 0, "Search one more folder for plugins of every format")) chooseFolder(a, "");
  }
  ui::EndSection();

  if (changed) {
    host::setPluginPrefs(prefs);
    host::requestScan(exe);
  }
}

}  // namespace gl
