// Gridline — template windows (see TemplateUi.h).
#include "../core/I18n.h"
#include "TemplateUi.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include "../../third_party/imgui/imgui.h"
#include "../core/Demo.h"
#include "../core/Templates.h"
#include "App.h"
#include "DialogLayout.h"
#include "J59Ui.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl {

using theme::pal;
namespace fs = std::filesystem;

namespace {

// Built-in entries shown before the template files.
const char* kBuiltinCat = "Gridline";
struct Entry {
  std::string key;  // "Category/Name"
  std::string name, category, description, path;  // path "" = built in (name says which)
  bool user = false;
};

std::vector<Entry> entries(App& a, bool rescan) {
  std::vector<Entry> out;
  out.push_back({std::string(kBuiltinCat) + "/Starter kit", "Starter kit", kBuiltinCat,
                 "Four drum voices and a synth, each on its own mixer track, with an empty pattern. Uses your project defaults.", "",
                 false});
  out.push_back({std::string(kBuiltinCat) + "/Demo song", "Demo song", kBuiltinCat,
                 "\"Gridline\": a full liquid drum & bass song, made only with the built-in instruments, effects and automation.", "",
                 false});
  for (const auto& t : a.templates(rescan)) out.push_back({t.category + "/" + t.name, t.name, t.category, t.description, t.path, t.user});
  return out;
}

Song entrySong(App& a, const Entry& e, std::string* err) {
  if (e.path.empty()) return e.name == "Demo song" ? makeDemoTrack() : a.starterSong();
  Song s;
  if (!loadTemplate(e.path, s, err)) return Song{};
  return s;
}

struct NewState {
  bool init = false;
  std::string cat, key;
  std::map<std::string, std::string> summary;  // path / key -> summary line
  // Project info and time settings applied on Create.
  char title[256] = "", author[256] = "", genre[128] = "";
  double tempo = 140;
  int ppq = 96, num = 4, den = 4;
  bool loaded = false;  // fields filled from the selected template
  std::string error;
} N;

void selectEntry(App& a, const Entry& e) {
  N.key = e.key;
  std::string err;
  Song s = entrySong(a, e, &err);
  N.error = err;
  if (!err.empty()) return;
  N.summary[e.key] = templateSummary(s);
  N.tempo = s.tempo;
  N.ppq = s.ppq;
  N.num = s.timeSigNum;
  N.den = s.timeSigDen;
  std::snprintf(N.author, sizeof N.author, "%s", (s.info.author.empty() ? a.settings.getString("project.defaultAuthor") : s.info.author).c_str());
  std::snprintf(N.genre, sizeof N.genre, "%s", s.info.genre.c_str());
  N.loaded = true;
}

void create(App& a, const Entry& e) {
  std::string err;
  Song s = entrySong(a, e, &err);
  if (!err.empty()) {
    a.notify("Could not open template: " + err, true);
    return;
  }
  if (N.ppq != s.ppq && isValidPpq(N.ppq)) {
    Document tmp(s);
    tmp.setPpq(N.ppq);
    s = tmp.song();
  }
  s.tempo = std::clamp(N.tempo, 10.0, 522.0);
  s.timeSigNum = std::clamp(N.num, 1, 16);
  s.timeSigDen = N.den;
  s.info.title = N.title[0] ? N.title : "Untitled";
  s.info.author = N.author;
  s.info.genre = N.genre;
  a.startProject(std::move(s));
  a.notify("New project from " + e.name);
}

void newProjectWindow(App& a) {
  if (!a.showNewProject) {
    N.init = false;
    return;
  }
  const auto& P = pal();
  ui::PlaceNextDialog(ImVec2(820, 500), ImGuiCond_Appearing, ImVec2(680, 420));  // J-54: clamped to the screen
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS4, theme::kS3));
  bool vis = ImGui::Begin(i18n::tr("New project###j24newproj"), &a.showNewProject, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse);
  ImGui::PopStyleVar();
  if (!vis) {
    ImGui::End();
    return;
  }
  ui::KeepWindowInViewport();
  auto list = entries(a, !N.init);
  if (!N.init) {
    N.init = true;
    N.title[0] = 0;
    std::string def = a.settings.getString("general.defaultTemplate");
    if (def.empty()) def = std::string(kBuiltinCat) + "/Starter kit";
    N.key.clear();
    for (const auto& e : list)
      if (e.key == def) N.cat = e.category, selectEntry(a, e);
    if (N.key.empty() && !list.empty()) N.cat = list[0].category, selectEntry(a, list[0]);
  }
  std::vector<std::string> cats;
  for (const auto& e : list)
    if (std::find(cats.begin(), cats.end(), e.category) == cats.end()) cats.push_back(e.category);
  const Entry* sel = nullptr;
  for (const auto& e : list)
    if (e.key == N.key) sel = &e;

  const float footer = ui::DialogFooterHeight();
  const float h = ImGui::GetContentRegionAvail().y - footer;
  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(P.surface950));
  // Categories.
  ImGui::BeginChild("##cats", ImVec2(170, h), ImGuiChildFlags_Borders);
  ui::Label("CATEGORIES");
  for (const auto& c : cats) {
    bool on = c == N.cat;
    if (ImGui::Selectable((c + "##cat").c_str(), on)) N.cat = c;
  }
  ImGui::EndChild();
  ImGui::SameLine();
  // Templates of the category.
  ImGui::BeginChild("##tpls", ImVec2(220, h), ImGuiChildFlags_Borders);
  ui::Label("TEMPLATES");
  for (const auto& e : list) {
    if (e.category != N.cat) continue;
    std::string l = e.name + (e.user ? "  (yours)" : "") + "##" + e.key;
    if (ImGui::Selectable(l.c_str(), e.key == N.key, ImGuiSelectableFlags_AllowDoubleClick)) {
      selectEntry(a, e);
      if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        create(a, e);
        a.showNewProject = false;
      }
    }
    if (!e.description.empty()) ui::Hint(e.description);
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();
  ImGui::SameLine();
  // Preview and settings.
  ImGui::BeginChild("##preview", ImVec2(0, h), ImGuiChildFlags_None);
  if (sel) {
    ImGui::PushFont(a.fontBold, theme::kFontTitle);
    ImGui::TextUnformatted(sel->name.c_str());
    ImGui::PopFont();
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(theme::vec(P.textDim), "%s", sel->description.empty() ? "No description." : sel->description.c_str());
    if (auto it = N.summary.find(sel->key); it != N.summary.end()) ImGui::TextColored(theme::vec(P.textFaint), "%s", it->second.c_str());
    if (!N.error.empty()) ImGui::TextColored(theme::vec(P.danger), "%s", N.error.c_str());
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, theme::kS1));
    const float lw = 110;
    auto row = [&](const char* label) {
      ImGui::AlignTextToFramePadding();
      ImGui::TextColored(theme::vec(P.textDim), "%s", label);
      ImGui::SameLine(lw);
    };
    ui::BeginSection("PROJECT");
    row("Title");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##title", i18n::tr("Untitled"), N.title, sizeof N.title);
    ui::FocusRing();
    row("Author");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##author", N.author, sizeof N.author);
    ui::FocusRing();
    row("Genre");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##genre", N.genre, sizeof N.genre);
    ui::FocusRing();
    ui::EndSection();
    ui::BeginSection("TIME");
    row("Tempo (BPM)");
    ImGui::SetNextItemWidth(120);
    ImGui::InputDouble("##tempo", &N.tempo, 1, 10, "%.1f");
    row("Time signature");
    if (ui::BeginCombo("##num", std::to_string(N.num).c_str(), 60)) {
      for (int n = 1; n <= 16; ++n)
        if (ImGui::Selectable(std::to_string(n).c_str(), n == N.num)) N.num = n;
      ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextColored(theme::vec(P.textDim), "/");
    ImGui::SameLine();
    if (ui::BeginCombo("##den", std::to_string(N.den).c_str(), 60)) {
      for (int d : {2, 4, 8, 16})
        if (ImGui::Selectable(std::to_string(d).c_str(), d == N.den)) N.den = d;
      ImGui::EndCombo();
    }
    row("Timebase (PPQ)");
    if (ui::BeginCombo("##ppq", std::to_string(N.ppq).c_str(), 120)) {
      for (int p : kPpqChoices)
        if (ImGui::Selectable(std::to_string(p).c_str(), p == N.ppq)) N.ppq = p;
      ImGui::EndCombo();
    }
    ui::Hint("Ticks per quarter note of the new project");
    ui::EndSection();
    const bool isDefault = a.settings.getString("general.defaultTemplate") == sel->key ||
                           (a.settings.getString("general.defaultTemplate").empty() && sel->key == std::string(kBuiltinCat) + "/Starter kit");
    bool def = isDefault;
    if (ImGui::Checkbox(i18n::tr("Use for File > New and at startup"), &def) && def)
      a.settings.set("general.defaultTemplate", sel->key == std::string(kBuiltinCat) + "/Starter kit" ? std::string() : sel->key);
    ui::Hint("The default template (Settings > General)");
    if (sel->user) {
      ImGui::SameLine();
      if (ui::TextButton("Delete template", false, 0, "Removes your template file")) {
        std::error_code ec;
        fs::remove(sel->path, ec);
        fs::path txt = fs::path(sel->path).parent_path() / (sel->name + ".txt");
        fs::remove(txt, ec);
        if (a.settings.getString("general.defaultTemplate") == sel->key) a.settings.set("general.defaultTemplate", std::string());
        a.templates(true);
        N.init = false;
      }
    }
  }
  ImGui::EndChild();
  ui::DialogFooter(ImGui::GetContentRegionAvail().x);
  ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x);
  if (ui::TextButton("Loop starter...", false, 0, "Build a ready-to-play loop from a style instead")) {
    j59::openLoopStarter(a);
    a.showNewProject = false;
  }
  ImGui::SameLine();
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ui::ButtonRowWidth(3, 96));
  bool cancel = ui::TextButton("Cancel", false, 96) || ImGui::IsKeyPressed(ImGuiKey_Escape);
  ImGui::SameLine(0, theme::kS2);
  if (ui::TextButton("Empty", false, 96, "Skip the template: an empty project with your defaults")) {
    a.guardUnsaved([&a]() { a.newFromTemplate(""); });
    a.showNewProject = false;
  }
  ImGui::SameLine(0, theme::kS2);
  bool ok = ui::TextButton("Create", true, 96, "Create the new project (Enter)") ||
            (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Enter));
  ImGui::End();
  if (ok && sel) {
    Entry e = *sel;
    a.guardUnsaved([&a, e]() { create(a, e); });
    a.showNewProject = false;
  } else if (cancel) {
    a.showNewProject = false;
  }
}

struct SaveState {
  bool init = false;
  char name[256] = "", category[128] = "", desc[2048] = "";
} T;

void saveTemplateWindow(App& a) {
  if (!a.showSaveTemplate) {
    T.init = false;
    return;
  }
  const auto& P = pal();
  if (!T.init) {
    T.init = true;
    std::snprintf(T.name, sizeof T.name, "%s", a.doc.song().info.title.c_str());
    std::snprintf(T.category, sizeof T.category, "%s", "My templates");
    std::snprintf(T.desc, sizeof T.desc, "%s", a.doc.song().info.comments.c_str());
  }
  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_Appearing);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(theme::kS4, theme::kS3));
  bool vis = ImGui::Begin(i18n::tr("Save as template###j24savetpl"), &a.showSaveTemplate,
                          ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize);
  ImGui::PopStyleVar();
  if (!vis) {
    ImGui::End();
    return;
  }
  ui::KeepWindowInViewport();
  ImGui::TextColored(theme::vec(P.textDim), "%s", i18n::tr("The whole project (channels, patterns, mixer, arrangement) becomes a template."));
  ui::BeginSection("TEMPLATE", 488);
  auto row = [&](const char* label) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::vec(P.textDim), "%s", label);
    ImGui::SameLine(110);
  };
  row("Name");
  ImGui::SetNextItemWidth(340);
  ImGui::InputText("##tname", T.name, sizeof T.name);
  ui::FocusRing();
  row("Category");
  ImGui::SetNextItemWidth(300);
  ImGui::InputText("##tcat", T.category, sizeof T.category);
  ui::FocusRing();
  ImGui::SameLine(0, 4);
  if (ui::BeginCombo("##tcats", "", 36)) {
    std::set<std::string> cats;
    for (const auto& t : a.templates()) cats.insert(t.category);
    for (const auto& c : cats)
      if (ImGui::Selectable(c.c_str())) std::snprintf(T.category, sizeof T.category, "%s", c.c_str());
    ImGui::EndCombo();
  }
  ui::Hint("Existing categories");
  row("Description");
  ImGui::InputTextMultiline("##tdesc", T.desc, sizeof T.desc, ImVec2(340, 90));
  ui::FocusRing();
  ui::EndSection();
  {  // J-158: a long folder is shortened in the middle (it widened the dialog past the main window)
    char where[1024];
    std::snprintf(where, sizeof where, i18n::tr("Saved in %s"), a.templatesDir().c_str());
    ui::PathFit(where, 488, P.textFaint);
  }
  ui::DialogFooter(96 * 2 + theme::kS2);
  bool cancel = ui::TextButton("Cancel", false, 96) || ImGui::IsKeyPressed(ImGuiKey_Escape);
  ImGui::SameLine(0, theme::kS2);
  bool ok = ui::TextButton("Save", true, 96);
  ImGui::End();
  if (ok && T.name[0]) {
    if (a.saveAsTemplate(T.name, T.category, T.desc)) a.showSaveTemplate = false;
  } else if (cancel) {
    a.showSaveTemplate = false;
  }
}

}  // namespace

void drawTemplateDialogs(App& a) {
  newProjectWindow(a);
  saveTemplateWindow(a);
}

}  // namespace gl
