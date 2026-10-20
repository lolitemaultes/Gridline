// Gridline — J-24 settings pages (see SettingsPages.h).
#include "SettingsPages.h"
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../core/Backup.h"
#include "../core/I18n.h"
#include "../core/Log.h"
#include "../core/Project.h"
#include "App.h"
#include "ScriptUi.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl {

using theme::pal;
using i18n::tr;
namespace fs = std::filesystem;

namespace {

void label(const char* text, float x = 168.0f) { ui::FormLabelFit(tr(text), x, pal().textDim); }  // J-54: fitted, never under the control

std::string langDir(const App& a) { return a.languageDir(); }

}  // namespace

// ------------------------------------------------------------------ General ----
void generalSettingsPage(App& a, float fieldW) {
  Settings& st = a.settings;
  ui::BeginSection(tr("ON STARTUP"));
  {
    static const char* modes[] = {"template", "empty", "last"};
    static const char* names[] = {"Open the default template", "Start with an empty project", "Reopen the last project"};
    std::string cur = st.getString("general.startup");
    for (int i = 0; i < 3; ++i)
      if (ImGui::RadioButton(tr(names[i]), cur == modes[i])) st.set("general.startup", std::string(modes[i]));
  }
  label("Default template");
  {
    std::string key = st.getString("general.defaultTemplate");
    const TemplateInfo* t = key.empty() ? nullptr : a.findTemplate(key);
    std::string preview = key.empty() ? std::string(tr("Starter kit (built in)")) : t ? t->category + " \xE2\x96\xB8 " + t->name : key + " (missing)";
    if (ui::BeginCombo("##deftpl", preview.c_str(), fieldW)) {
      if (ImGui::Selectable(i18n::tr("Starter kit (built in)"), key.empty())) st.set("general.defaultTemplate", std::string());
      std::string cat;
      for (const auto& x : a.templates()) {
        if (x.category != cat) {
          cat = x.category;
          ImGui::SeparatorText(cat.c_str());
        }
        std::string k = x.category + "/" + x.name;
        if (ImGui::Selectable((x.name + "##" + k).c_str(), k == key)) st.set("general.defaultTemplate", k);
      }
      ImGui::EndCombo();
    }
    ui::Hint("Used by File > New (Ctrl+N) and at startup");
  }
  label("");
  if (ui::TextButton(tr("Browse templates..."), false, 0, "File > New from template")) a.showNewProject = true;
  ui::EndSection();

  ui::BeginSection(tr("LANGUAGE"));
  label("Display language");
  {
    auto langs = i18n::available(langDir(a));
    std::string cur = st.getString("general.language");
    std::string name = cur;
    for (const auto& l : langs)
      if (l.code == cur) name = l.name;
    if (ui::BeginCombo("##lang", name.c_str(), fieldW)) {
      for (const auto& l : langs)
        if (ImGui::Selectable((l.name + "##" + l.code).c_str(), l.code == cur)) {
          st.set("general.language", l.code);
          a.applySettingsHooks();
        }
      ImGui::EndCombo();
    }
    ui::Hint("Translations are catalogues in assets/i18n (gettext .po or JSON); Pseudo shows which texts are translatable and where they clip");
  }
  ImGui::PushTextWrapPos(0);
  ImGui::TextColored(theme::vec(pal().textFaint), "%s",
                     tr("Menus, dialogs, settings, hints and editor labels follow the language. To add one, copy assets/i18n/gridline.pot to <code>.po and translate it."));
  ImGui::PopTextWrapPos();
  ui::EndSection();
}

// -------------------------------------------------------------------- Files ----
void fileSettingsPage(App& a, float fieldW) {
  Settings& st = a.settings;
  ui::BeginSection(tr("FOLDERS"));
  auto folder = [&](const char* what, const std::string& path) {
    label(what);
    ImGui::PushID(what);
    // J-54: paths share one fitted column, so the Show buttons line up (full path on hover).
    const float x0 = ImGui::GetCursorPosX(), pathW = std::max(80.0f, ImGui::GetContentRegionAvail().x - 60 - theme::kS2);
    ui::TextFit(path.c_str(), pathW, pal().text, true);
    ImGui::SameLine(0, 0);
    ImGui::SetCursorPosX(x0 + pathW + theme::kS2);
    if (ui::TextButton(tr("Show"), false, 60, path.c_str())) {
      std::error_code ec;
      fs::create_directories(path, ec);
      a.notify(std::string(what) + ": " + path);
    }
    ImGui::PopID();
  };
  folder("User data", a.configDir());
  folder("Templates", a.templatesDir());
  folder("Piano roll scripts", userScriptsDir(a, "pianoroll"));
  folder("Controller scripts", userScriptsDir(a, "controllers"));
  folder("Backups", a.backups->policy().globalFolder);
  ui::EndSection();

  ui::BeginSection(tr("SCRIPTS"));
  label("Scripting");
  bool on = st.getBool("scripts.enabled");
  if (ImGui::Checkbox(tr("Run piano roll and controller scripts"), &on)) st.set("scripts.enabled", on);
  ui::Hint("Scripts run in a sandbox: no system or network access, files only inside their own folder");
  label("Transport");
  bool tr2 = st.getBool("scripts.allowTransport");
  if (ImGui::Checkbox(tr("Project scripts may start, stop and move playback"), &tr2)) st.set("scripts.allowTransport", tr2);
  ui::Hint("Off: project scripts and the console can only read the transport");
  label("Time limit");
  float secs = static_cast<float>(st.getNumber("scripts.maxSeconds"));
  ImGui::SetNextItemWidth(fieldW);
  if (ImGui::SliderFloat("##secs", &secs, 0.1f, 10.0f, "%.1f s per run")) st.set("scripts.maxSeconds", static_cast<double>(secs));
  ui::Hint("A script that runs longer is stopped (controller script hooks: at most 0.25 s)");
  label("Instruction limit");
  int mi = static_cast<int>(st.getNumber("scripts.maxMillionInstructions"));
  ImGui::SetNextItemWidth(fieldW);
  if (ImGui::SliderInt("##mi", &mi, 1, 500, "%d million")) st.set("scripts.maxMillionInstructions", mi);
  label("Script editor");
  {
    static char ed[512];
    static bool init = false;
    if (!init) std::snprintf(ed, sizeof ed, "%s", st.getString("file.scriptEditor").c_str()), init = true;
    ImGui::SetNextItemWidth(fieldW);
    if (ImGui::InputTextWithHint("##ed", i18n::tr("System default text editor"), ed, sizeof ed)) st.set("file.scriptEditor", std::string(ed));
    ui::FocusRing();
    ui::Hint("Command that opens a script for editing (the file path is added at the end)");
  }
  label("");
  if (ui::TextButton(tr("Rescan scripts"))) {
    pianoRollScripts(a, true);
    controllerScripts(a, true);
    shutdownControllerScripts();
    a.notify(std::to_string(pianoRollScripts(a).size()) + " piano roll and " + std::to_string(controllerScripts(a).size()) +
             " controller scripts");
  }
  ui::EndSection();
}

// -------------------------------------------------------------------- Debug ----
void debugSettingsPage(App& a, float fieldW) {
  Settings& st = a.settings;
  const auto& P = pal();
  ui::BeginSection(tr("LOG"));
  label("Detail");
  {
    static const char* lv[] = {"Errors", "Warnings", "Info", "Verbose"};
    int cur = std::clamp(static_cast<int>(st.getNumber("debug.logLevel")), 0, 3);
    if (ui::Segmented("##lvl", lv, 4, &cur, 80)) {
      st.set("debug.logLevel", cur);
      a.applySettingsHooks();
    }
  }
  label("Log file");
  bool toFile = st.getBool("debug.logToFile");
  if (ImGui::Checkbox("##tofile", &toFile)) {
    st.set("debug.logToFile", toFile);
    a.applySettingsHooks();
  }
  ImGui::SameLine();
  ui::TextFit((fs::path(a.configDir()) / "gridline.log").string().c_str(), ImGui::GetContentRegionAvail().x, P.textFaint);
  auto entries = logging::entries();
  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(P.surface950));
  ImGui::BeginChild("##log", ImVec2(0, 150), ImGuiChildFlags_Borders);
  ImGui::PushFont(a.fontMono, theme::kFontNumber);
  for (const auto& e : entries) {
    ImU32 col = e.level == logging::Level::Error ? P.danger : e.level == logging::Level::Warning ? P.accent : e.level == logging::Level::Verbose ? P.textFaint : P.textDim;
    ImGui::TextColored(theme::vec(col), "%s", logging::format(e).c_str());
  }
  if (entries.empty()) ImGui::TextColored(theme::vec(P.textFaint), "%s", tr("Nothing logged yet."));
  static uint64_t seen = 0;
  if (logging::serial() != seen) {
    seen = logging::serial();
    ImGui::SetScrollHereY(1.0f);
  }
  ImGui::PopFont();
  ImGui::EndChild();
  ImGui::PopStyleColor();
  if (ui::TextButton(tr("Copy all"))) {
    std::string all;
    for (const auto& e : entries) all += logging::format(e) + "\n";
    ImGui::SetClipboardText(all.c_str());
  }
  ImGui::SameLine();
  if (ui::TextButton(tr("Clear"))) logging::clear();
  ui::EndSection();
  (void)fieldW;
  ui::BeginSection(tr("SCRIPT CONSOLE"));
  ImGui::TextColored(theme::vec(P.textDim), "%s", tr("Runs Lua against the open project with the controller-script API."));
  drawScriptConsole(a, 120);
  ui::EndSection();
}

// --------------------------------------------------------- panels on pages ----
void projectDefaultsPanel(App& a) {
  Settings& st = a.settings;
  label("Time signature");
  int num = static_cast<int>(st.getNumber("project.defaultNum")), den = static_cast<int>(st.getNumber("project.defaultDen"));
  if (ui::BeginCombo("##dnum", std::to_string(num).c_str(), 64)) {
    for (int n = 1; n <= 16; ++n)
      if (ImGui::Selectable(std::to_string(n).c_str(), n == num)) st.set("project.defaultNum", n);
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  ImGui::TextColored(theme::vec(pal().textDim), "/");
  ImGui::SameLine();
  if (ui::BeginCombo("##dden", std::to_string(den).c_str(), 64)) {
    for (int d : {2, 4, 8, 16})
      if (ImGui::Selectable(std::to_string(d).c_str(), d == den)) st.set("project.defaultDen", d);
    ImGui::EndCombo();
  }
  label("Pattern length");
  int bars = static_cast<int>(st.getNumber("project.defaultPatternBars"));
  ImGui::SetNextItemWidth(160);
  if (ImGui::SliderInt("##dbars", &bars, 1, 16, bars == 1 ? "1 bar" : "%d bars")) st.set("project.defaultPatternBars", bars);
  label("Author");
  static char author[256];
  static bool init = false;
  if (!init) std::snprintf(author, sizeof author, "%s", st.getString("project.defaultAuthor").c_str()), init = true;
  ImGui::SetNextItemWidth(260);
  if (ImGui::InputTextWithHint("##dauthor", i18n::tr("Your name"), author, sizeof author)) st.set("project.defaultAuthor", std::string(author));
  ui::FocusRing();
  ui::Hint("Filled into Project info of new projects");
}

void backupRestorePanel(App& a, float fieldW) {
  Settings& st = a.settings;
  const auto& P = pal();
  ui::BeginSection(tr("AUTOSAVE"));
  bool as = st.getBool("backup.autosave");
  if (ImGui::Checkbox(tr("Save the project file itself every"), &as)) st.set("backup.autosave", as);
  ImGui::SameLine();
  int mins = static_cast<int>(st.getNumber("backup.autosaveMinutes"));
  ImGui::SetNextItemWidth(140);
  if (ImGui::SliderInt("##asmin", &mins, 1, 60, mins == 1 ? "minute" : "%d minutes")) st.set("backup.autosaveMinutes", mins);
  ui::Hint("Only projects that have been saved once; backups keep the older states");
  ui::EndSection();

  ui::BeginSection(tr("RESTORE"));
  auto list = a.backups->list(a.projectPath);
  std::reverse(list.begin(), list.end());  // newest first
  static int pick = 0;
  pick = std::clamp(pick, 0, std::max(0, static_cast<int>(list.size()) - 1));
  ImGui::TextColored(theme::vec(P.textDim), "%s",
                     (std::to_string(list.size()) + (list.size() == 1 ? " backup of " : " backups of ") +
                      (a.projectPath.empty() ? std::string("this unsaved project") : fs::path(a.projectPath).filename().string()))
                         .c_str());
  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(P.surface950));
  ImGui::BeginChild("##backups", ImVec2(std::max(fieldW, ImGui::GetContentRegionAvail().x), 110), ImGuiChildFlags_Borders);
  for (size_t i = 0; i < list.size(); ++i) {
    std::error_code ec;
    auto sz = fs::file_size(list[i], ec);
    char row[512];
    std::snprintf(row, sizeof row, "%s   %.0f KB##b%zu", fs::path(list[i]).filename().string().c_str(), ec ? 0.0 : static_cast<double>(sz) / 1024.0, i);
    if (ImGui::Selectable(row, pick == static_cast<int>(i))) pick = static_cast<int>(i);
  }
  if (list.empty()) ImGui::TextColored(theme::vec(P.textFaint), "%s", tr("No backups yet: they are written while you work."));
  ImGui::EndChild();
  ImGui::PopStyleColor();
  const bool any = !list.empty();
  if (ui::TextButton(tr("Restore"), false, 0, "Replace the project with the selected backup (undoable)") && any) {
    Song s;
    std::string err;
    if (loadProject(list[static_cast<size_t>(pick)], s, &err)) {
      a.engine.stopToStart();
      a.doc.editSong("restore backup", [&s](Song& x) { x = s; });
      a.notify("Restored " + fs::path(list[static_cast<size_t>(pick)]).filename().string());
    } else {
      a.notify("Could not read the backup: " + err, true);
    }
  }
  ImGui::SameLine();
  if (ui::TextButton(tr("Open as new project"), false, 0, "Opens the backup as a separate, unsaved project") && any) {
    std::string path = list[static_cast<size_t>(pick)];
    a.guardUnsaved([&a, path]() {
      Song s;
      std::string err;
      if (loadProject(path, s, &err))
        a.startProject(std::move(s));
      else
        a.notify("Could not read the backup: " + err, true);
    });
  }
  ImGui::SameLine();
  if (ui::TextButton(tr("Show folder"))) a.notify("Backups: " + a.backups->folderFor(a.projectPath));
  ui::EndSection();
}

void hintSettingsPanel(App& a, float fieldW) {
  Settings& st = a.settings;
  label("Tooltip delay");
  float d = static_cast<float>(st.getNumber("ui.hintDelay"));
  ImGui::SetNextItemWidth(fieldW);
  if (ImGui::SliderFloat("##hintdelay", &d, 0.0f, 2.0f, "%.2f s")) st.set("ui.hintDelay", static_cast<double>(d));
  ui::Hint("How long the mouse rests on a control before its tooltip appears (the hint bar updates at once)");
}

}  // namespace gl
