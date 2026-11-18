// Gridline - K-237: the workspace strip and menu. See WorkspacesUi.h.
#include "WorkspacesUi.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include "../core/I18n.h"
#include "../core/LayoutShell.h"
#include "../core/Settings.h"
#include "../core/Workspaces.h"
#include "App.h"
#include "Keys.h"
#include "LayoutPresets.h"
#include "LayoutShellUi.h"
#include "Theme.h"

namespace gl::workspacesui {

using layout::Preset;
namespace ws = workspaces;

namespace {

constexpr float kStripH = 26.0f;

// Dialog windows a workspace leaves alone (they are not part of an arrangement).
bool dialogWindow(const char* w) {
  static const char* const kDialogs[] = {"Settings", "Keyboard shortcuts", "Project info", "Plugin manager", "Collaboration", "Sync review"};
  for (const char* d : kDialogs)
    if (std::strcmp(d, w) == 0) return true;
  return false;
}

// A --layout run keeps its workspaces in memory only (like its region state).
std::map<int, std::string> gRunText;

ws::Set load(App& a, Preset p) {
  const std::string text = layoutui::hasDevOverride() ? gRunText[static_cast<int>(p)] : a.settings.getString(ws::settingsKey(p));
  return ws::fromJson(text);
}

void store(App& a, Preset p, const ws::Set& s) {
  const std::string text = s.items.size() <= 1 && s.autoSave ? std::string() : ws::toJson(s);  // one workspace: nothing to keep
  if (layoutui::hasDevOverride()) gRunText[static_cast<int>(p)] = text;
  else if (a.settings.getString(ws::settingsKey(p)) != text) a.settings.set(ws::settingsKey(p), text);
}

ws::Arrangement capture(App& a, Preset p) {
  ws::Arrangement r;
  for (const layoutui::WindowFlag& f : layoutui::windowFlags(a))
    if (*f.open && !dialogWindow(f.window)) r.open.insert(f.window);
  if (layoutui::shellBuilt(a, p)) r.regionState = layout::toJson(layoutui::regionState(a, p));
  return r;
}

void apply(App& a, Preset p, const ws::Arrangement& r) {
  for (const layoutui::WindowFlag& f : layoutui::windowFlags(a))
    if (!dialogWindow(f.window)) *f.open = r.open.count(f.window) > 0;
  if (p == Preset::Studio) a.showPlaylist = true;  // Studio's main view never goes
  if (layoutui::shellBuilt(a, p)) {
    layoutui::setRegionState(a, p, layout::fromJson(p, r.regionState));
    layoutui::requestShellRebuild();
  }
}

// The set of the active layout, made with today's arrangement on first use.
ws::Set current(App& a, Preset p) {
  ws::Set s = load(a, p);
  if (s.items.empty()) s = ws::initial(p, capture(a, p));
  return s;
}

void selectIndex(App& a, int i) {
  const Preset p = layoutui::active();
  ws::Set s = current(a, p);
  if (const ws::Arrangement* to = ws::select(s, i, capture(a, p))) {
    const ws::Arrangement target = *to;
    store(a, p, s);
    apply(a, p, target);
  }
}

void addNew(App& a, const std::string& name) {
  const Preset p = layoutui::active();
  ws::Set s = current(a, p);
  ws::record(s, capture(a, p));
  if (ws::add(s, name, capture(a, p)) >= 0) store(a, p, s);
}

// Undo for a closed workspace: kept a few seconds, shown in the strip.
struct Closed {
  bool have = false;
  Preset layout = Preset::Studio;
  ws::Workspace w;
  int index = 0;
  double until = 0;
};
Closed gClosed;

void closeIndex(App& a, int i) {
  const Preset p = layoutui::active();
  ws::Set s = current(a, p);
  const bool wasActive = i == s.active;
  ws::record(s, capture(a, p));
  ws::Workspace gone;
  if (!ws::remove(s, i, &gone)) return;
  gClosed = Closed{true, p, gone, i, ImGui::GetTime() + 6.0};
  store(a, p, s);
  if (wasActive) apply(a, p, s.items[static_cast<size_t>(s.active)].current);
}

void undoClose(App& a) {
  if (!gClosed.have || gClosed.layout != layoutui::active()) return;
  const Preset p = gClosed.layout;
  ws::Set s = current(a, p);
  ws::record(s, capture(a, p));
  ws::restore(s, gClosed.w, gClosed.index);
  store(a, p, s);
  apply(a, p, gClosed.w.current);
  gClosed.have = false;
}

void addStarters(App& a) {
  const Preset p = layoutui::active();
  ws::Set s = current(a, p);
  ws::record(s, capture(a, p));
  const int keep = s.active;
  for (const ws::Workspace& w : ws::starters(p)) {
    bool have = false;
    for (const ws::Workspace& o : s.items) have = have || o.name == w.name;
    if (!have) {
      s.active = static_cast<int>(s.items.size()) - 1;
      ws::add(s, w.name, w.saved);
    }
  }
  s.active = keep;
  store(a, p, s);
}

int gRename = -1;  // the workspace being renamed in the strip
char gRenameBuf[ws::kMaxName * 4 + 1] = {};
int gMenuFor = -1;

}  // namespace

float drawStrip(App& a, ImVec2 pos, float width) {
  const Preset p = layoutui::active();
  ws::Set s = current(a, p);
  if (s.items.size() <= 1 && !(gClosed.have && gClosed.layout == p && ImGui::GetTime() < gClosed.until)) return 0;
  const auto& P = theme::pal();
  ImGui::SetNextWindowPos(pos);
  ImGui::SetNextWindowSize(ImVec2(width, kStripH));
  ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1, 1));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::vec(P.surface900));
  ImGui::Begin("##workspaces", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus);
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(3);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  dl->AddLine(ImVec2(pos.x, pos.y + kStripH - 0.5f), ImVec2(pos.x + width, pos.y + kStripH - 0.5f), P.line);
  float x = pos.x + theme::kS2;
  const float h = kStripH - 6, y0 = pos.y + 3;
  // The layout this set belongs to.
  {
    const char* name = i18n::tr(layout::displayName(p));
    const float tw = a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, name).x;
    dl->AddRectFilled(ImVec2(x, y0), ImVec2(x + tw + 16, y0 + h), P.surface700, h * 0.5f);
    dl->AddText(a.fontRegular, fs, ImVec2(x + 8, std::round(y0 + (h - fs) * 0.5f)), P.textDim, name);
    x += tw + 16 + theme::kS2;
  }
  bool changed = false;
  int selectTo = -1, closeTo = -1;
  for (int i = 0; i < static_cast<int>(s.items.size()); ++i) {
    const ws::Workspace& w = s.items[static_cast<size_t>(i)];
    const bool on = i == s.active;
    char key[24] = "";
    if (i < ws::kKeyed) {
      char id[32];
      std::snprintf(id, sizeof id, "view.workspace%d", i + 1);
      if (const char* l = keys::label(id)) std::snprintf(key, sizeof key, "%s", l);  // no key in this shortcut set: none shown
    }
    const float tw = (on ? a.fontBold : a.fontRegular)->CalcTextSizeA(fs, 1e9f, 0, w.name.c_str()).x;
    const float kw = *key ? a.fontRegular->CalcTextSizeA(fs * 0.85f, 1e9f, 0, key).x + 8 : 0;
    const float tabW = std::ceil(tw + kw + 20 + (w.changed() ? 8 : 0));
    if (x + tabW > pos.x + width - 40) break;
    ImGui::PushID(i);
    ImGui::SetCursorScreenPos(ImVec2(x, y0));
    if (gRename == i) {
      ImGui::SetNextItemWidth(std::max(tabW, 120.0f));
      if (ImGui::IsWindowAppearing() || !ImGui::IsAnyItemActive()) ImGui::SetKeyboardFocusHere();
      const bool done = ImGui::InputText("##rename", gRenameBuf, sizeof gRenameBuf, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
      if (done) changed = ws::rename(s, i, gRenameBuf) || changed;
      if (done || ImGui::IsKeyPressed(ImGuiKey_Escape) || (!ImGui::IsItemActive() && !ImGui::IsItemFocused() && ImGui::IsMouseClicked(0))) gRename = -1;
      x += std::max(tabW, 120.0f) + 4;
      ImGui::PopID();
      continue;
    }
    const bool click = ImGui::InvisibleButton("##ws", ImVec2(tabW, h));
    const bool hov = ImGui::IsItemHovered();
    if (hov && ImGui::IsMouseDoubleClicked(0)) {
      gRename = i;
      std::snprintf(gRenameBuf, sizeof gRenameBuf, "%s", w.name.c_str());
    } else if (click && !on) selectTo = i;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) gMenuFor = i, ImGui::OpenPopup("##wsmenu");
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
      ImGui::SetDragDropPayload("GL_WORKSPACE", &i, sizeof i);
      ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GL_WORKSPACE")) changed = ws::move(s, *static_cast<const int*>(pl->Data), i) || changed;
      ImGui::EndDragDropTarget();
    }
    ImGui::PopID();
    if (on) dl->AddRectFilled(ImVec2(x, y0), ImVec2(x + tabW, y0 + h), P.surface600, 4.0f);
    else if (hov) dl->AddRectFilled(ImVec2(x, y0), ImVec2(x + tabW, y0 + h), theme::withAlpha(P.surface600, 0.5f), 4.0f);
    float tx = x + 10;
    dl->AddText(on ? a.fontBold : a.fontRegular, fs, ImVec2(tx, std::round(y0 + (h - fs) * 0.5f)), on || hov ? P.text : P.textDim, w.name.c_str());
    tx += tw + 6;
    if (w.changed()) {  // changed since it was saved
      dl->AddCircleFilled(ImVec2(tx + 1, y0 + h * 0.5f), 2.5f, P.accent);
      tx += 8;
    }
    if (*key) dl->AddText(a.fontRegular, fs * 0.85f, ImVec2(tx, std::round(y0 + (h - fs * 0.85f) * 0.5f)), P.textFaint, key);
    x += tabW + 2;
  }
  // "+": a new workspace from the current arrangement.
  ImGui::SetCursorScreenPos(ImVec2(x + 4, y0));
  if (ImGui::InvisibleButton("##wsadd", ImVec2(h, h))) addNew(a, "");
  {
    const bool hov = ImGui::IsItemHovered();
    if (hov) ImGui::SetTooltip("%s", i18n::tr("New workspace from the current arrangement"));
    const ImVec2 m(x + 4 + h * 0.5f, y0 + h * 0.5f);
    const ImU32 c = hov ? P.text : P.textDim;
    dl->AddLine(ImVec2(m.x - 5, m.y), ImVec2(m.x + 5, m.y), c, 1.4f);
    dl->AddLine(ImVec2(m.x, m.y - 5), ImVec2(m.x, m.y + 5), c, 1.4f);
  }
  x += h + 12;
  // Undo for a closed workspace.
  if (gClosed.have && gClosed.layout == p && ImGui::GetTime() < gClosed.until) {
    char msg[128];
    std::snprintf(msg, sizeof msg, i18n::tr("Closed %s."), gClosed.w.name.c_str());
    const float mw = a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, msg).x;
    dl->AddText(a.fontRegular, fs, ImVec2(x, std::round(y0 + (h - fs) * 0.5f)), P.textDim, msg);
    const char* undo = i18n::tr("Undo");
    const float uw = a.fontBold->CalcTextSizeA(fs, 1e9f, 0, undo).x;
    ImGui::SetCursorScreenPos(ImVec2(x + mw + 8, y0));
    if (ImGui::InvisibleButton("##wsundo", ImVec2(uw + 8, h))) undoClose(a);
    dl->AddText(a.fontBold, fs, ImVec2(x + mw + 12, std::round(y0 + (h - fs) * 0.5f)), ImGui::IsItemHovered() ? P.text : P.accent, undo);
  }
  if (ImGui::BeginPopup("##wsmenu")) {
    const int i = gMenuFor;
    if (i >= 0 && i < static_cast<int>(s.items.size())) {
      const bool on = i == s.active;
      if (ImGui::MenuItem(i18n::tr("Rename"))) gRename = i, std::snprintf(gRenameBuf, sizeof gRenameBuf, "%s", s.items[static_cast<size_t>(i)].name.c_str());
      if (ImGui::MenuItem(i18n::tr("Duplicate"), nullptr, false, static_cast<int>(s.items.size()) < ws::kMaxWorkspaces)) {
        ws::record(s, capture(a, p));
        changed = ws::duplicate(s, i) >= 0 || changed;
      }
      if (ImGui::MenuItem(i18n::tr("Move left"), nullptr, false, i > 0)) changed = ws::move(s, i, i - 1) || changed;
      if (ImGui::MenuItem(i18n::tr("Move right"), nullptr, false, i + 1 < static_cast<int>(s.items.size()))) changed = ws::move(s, i, i + 1) || changed;
      ImGui::Separator();
      if (ImGui::MenuItem(i18n::tr("Save current arrangement"), nullptr, false, on && !s.autoSave)) {
        ws::record(s, capture(a, p));
        ws::save(s);
        changed = true;
      }
      if (ImGui::MenuItem(i18n::tr("Revert to saved"), nullptr, false, on && !s.autoSave && s.items[static_cast<size_t>(i)].changed()))
        if (const ws::Arrangement* r = ws::revert(s)) apply(a, p, *r), changed = true;
      bool autoSave = s.autoSave;
      if (ImGui::MenuItem(i18n::tr("Save workspace changes automatically"), nullptr, &autoSave)) s.autoSave = autoSave, changed = true;
      ImGui::Separator();
      if (ImGui::MenuItem(i18n::tr("Close workspace"), nullptr, false, s.items.size() > 1)) closeTo = i;
    }
    ImGui::EndPopup();
  }
  ImGui::End();
  if (changed) store(a, p, s);
  if (selectTo >= 0) selectIndex(a, selectTo);
  if (closeTo >= 0) closeIndex(a, closeTo);
  return kStripH;
}

namespace {
// Every layout's set as the project copy keeps it (the active layout's always, with today's arrangement).
std::map<std::string, std::string> copyNow(App& a) {
  std::map<std::string, std::string> m;
  const Preset now = layoutui::active();
  for (int k = 0; k < layout::kLayoutCount; ++k) {
    const Preset p = static_cast<Preset>(k);
    ws::Set s = load(a, p);
    if (p == now) {
      if (s.items.empty()) s = ws::initial(p, capture(a, p));
      ws::record(s, capture(a, p));
    }
    if (!s.items.empty()) m[layout::id(p)] = ws::toJson(s);
  }
  return m;
}
}  // namespace

bool projectCopyOn(const App& a) { return !a.doc.song().workspaceCopy.empty(); }

void setProjectCopy(App& a, bool on) {
  std::map<std::string, std::string> m;
  if (on) m = copyNow(a);
  if (a.doc.song().workspaceCopy != m) a.doc.patchSong([&m](Song& s) { s.workspaceCopy = m; });
}

void refreshProjectCopy(App& a) {
  if (!projectCopyOn(a)) return;
  // K-314 B-01: a layout this person has no workspaces in keeps the copy's set (a collaborator's, or the
  // one the project came with), so one person's save never drops another's.
  std::map<std::string, std::string> m = a.doc.song().workspaceCopy;
  for (const auto& [lay, text] : copyNow(a)) {
    m[lay] = text;
  }
  if (a.doc.song().workspaceCopy != m) a.doc.patchSong([&m](Song& s) { s.workspaceCopy = m; });
}

void projectOpened(App& a) {
  const std::map<std::string, std::string> copy = a.doc.song().workspaceCopy;
  if (copy.empty()) return;  // an older project, or one without the copy: nothing changes
  const Preset now = layoutui::active();
  for (int k = 0; k < layout::kLayoutCount; ++k) {
    const Preset p = static_cast<Preset>(k);
    const auto it = copy.find(layout::id(p));
    if (it == copy.end() || load(a, p).items.size() > 1) continue;  // the person's own workspaces win
    ws::Set s = ws::fromJson(it->second);
    if (s.items.size() <= 1) continue;
    store(a, p, s);
    if (p == now) apply(a, p, s.items[static_cast<size_t>(s.active)].current);
  }
}

void menu(App& a) {
  const Preset p = layoutui::active();
  ws::Set s = current(a, p);
  if (ImGui::MenuItem(i18n::tr("New workspace"))) addNew(a, "");
  if (ImGui::MenuItem(i18n::tr("Add starter workspaces"))) addStarters(a);
  if (ImGui::MenuItem(i18n::tr("Close workspace"), nullptr, false, s.items.size() > 1)) closeIndex(a, s.active);
  ImGui::Separator();
  // K-237: an opt-in copy in the project, for another computer or a person without workspaces of their own.
  if (bool on = projectCopyOn(a); ImGui::MenuItem(i18n::tr("Keep a copy in the project"), nullptr, &on)) setProjectCopy(a, on);
  ImGui::Separator();
  for (int i = 0; i < static_cast<int>(s.items.size()); ++i) {
    char id[32] = "";
    if (i < ws::kKeyed) std::snprintf(id, sizeof id, "view.workspace%d", i + 1);
    ImGui::PushID(i);
    const char* key = *id ? keys::label(id) : nullptr;
    if (ImGui::MenuItem(s.items[static_cast<size_t>(i)].name.c_str(), key && *key ? key : nullptr, i == s.active)) selectIndex(a, i);
    ImGui::PopID();
  }
}

void frameKeys(App& a) {
  for (int i = 0; i < ws::kKeyed; ++i) {
    char id[32];
    std::snprintf(id, sizeof id, "view.workspace%d", i + 1);
    if (keys::pressed(id)) selectIndex(a, i);
  }
  const bool next = keys::pressed("view.workspaceNext"), prev = keys::pressed("view.workspacePrevious");
  if (next || prev) {
    const ws::Set s = current(a, layoutui::active());
    const int n = static_cast<int>(s.items.size());
    if (n > 1) selectIndex(a, ((s.active + (next ? 1 : -1)) % n + n) % n);
  }
}

void devCommand(App& a, const std::vector<std::string>& args) {
  if (args.empty()) return;
  const std::string& c = args[0];
  std::string arg = args.size() > 1 ? args[1] : std::string();
  for (size_t i = 2; i < args.size(); ++i) arg += " " + args[i];
  const Preset p = layoutui::active();
  if (c == "add") addNew(a, arg);
  else if (c == "select") selectIndex(a, std::atoi(arg.c_str()));
  else if (c == "close") closeIndex(a, std::atoi(arg.c_str()));
  else if (c == "undo") undoClose(a);
  else if (c == "starters") addStarters(a);
  else if (c == "copy") setProjectCopy(a, arg == "on");  // K-237: the opt-in copy in the project
  else if (c == "rename" && args.size() > 2) {
    ws::Set s = current(a, p);
    std::string name = args[2];
    for (size_t i = 3; i < args.size(); ++i) name += " " + args[i];
    if (ws::rename(s, std::atoi(args[1].c_str()), name)) store(a, p, s);
  } else if (c == "dump") {
    const ws::Set s = current(a, p);
    std::printf("WORKSPACES %s active %d:", layout::id(p), s.active);
    for (const ws::Workspace& w : s.items) std::printf(" [%s%s]", w.name.c_str(), w.changed() ? " *" : "");
    std::printf(" copy %s", projectCopyOn(a) ? "on" : "off");
    std::printf("\n");
    std::fflush(stdout);
  }
}

}  // namespace gl::workspacesui
