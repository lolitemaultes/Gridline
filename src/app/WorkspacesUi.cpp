// Gridline - K-237: the workspace strip and menu. See WorkspacesUi.h.
#include "WorkspacesUi.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/I18n.h"
#include "../core/LayoutShell.h"
#include "../core/Settings.h"
#include "../core/Workspaces.h"
#include "../core/DockPlan.h"
#include "../core/Toolbar.h"
#include "Widgets.h"
#include "SettingsPages.h"
#include <functional>
#include "App.h"
#include "DockPlanUi.h"
#include "ToolbarUi.h"
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

// One workspace that keeps nothing of its own: nothing to keep (the layout's arrangement is the workspace).
bool plainSingle(const ws::Set& s) {
  if (s.items.size() > 1 || !s.autoSave) return false;
  if (s.items.empty()) return true;
  const ws::Workspace& w = s.items[0];
  return !w.hasOwnBar() && !w.hasOwnPins() && w.keepWindows && w.keepPanes;
}

void store(App& a, Preset p, const ws::Set& s) {
  const std::string text = plainSingle(s) ? std::string() : ws::toJson(s);
  if (layoutui::hasDevOverride()) gRunText[static_cast<int>(p)] = text;
  else if (a.settings.getString(ws::settingsKey(p)) != text) a.settings.set(ws::settingsKey(p), text);
}

ws::Arrangement capture(App& a, Preset p) {
  ws::Arrangement r;
  for (const layoutui::WindowFlag& f : layoutui::windowFlags(a))
    if (*f.open && !dialogWindow(f.window)) r.open.insert(f.window);
  if (layoutui::shellBuilt(a, p)) r.regionState = layout::toJson(layoutui::regionState(a, p));
  else if (p == Preset::Studio) r.dock = dockplanui::capture(a);  // K-256: Studio's panes and floating windows
  return r;
}

// K-256: what the workspace keeps comes back (all of it unless its card says otherwise).
void apply(App& a, Preset p, const ws::Arrangement& r, bool windows = true, bool panes = true) {
  if (windows) {
    for (const layoutui::WindowFlag& f : layoutui::windowFlags(a))
      if (!dialogWindow(f.window)) *f.open = r.open.count(f.window) > 0;
    if (p == Preset::Studio) a.showPlaylist = true;  // Studio's main view never goes
  }
  if (!panes) return;
  if (layoutui::shellBuilt(a, p)) {
    layoutui::setRegionState(a, p, layout::fromJson(p, r.regionState));
    layoutui::requestShellRebuild();
  } else if (p == Preset::Studio && !r.dock.empty()) {
    dockplanui::request(a, r.dock);
    layoutui::requestShellRebuild();  // (main.cpp: a rebuild that keeps the windows open)
  }
}

void applyWorkspace(App& a, Preset p, const ws::Workspace& w, const ws::Arrangement& r) {
  apply(a, p, r, w.keepWindows, w.keepPanes);
  toolbarui::changed();  // its own bar and pins (or the layout's) from now
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
    applyWorkspace(a, p, s.items[static_cast<size_t>(s.active)], target);
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
  if (wasActive) applyWorkspace(a, p, s.items[static_cast<size_t>(s.active)], s.items[static_cast<size_t>(s.active)].current);
}

void undoClose(App& a) {
  if (!gClosed.have || gClosed.layout != layoutui::active()) return;
  const Preset p = gClosed.layout;
  ws::Set s = current(a, p);
  ws::record(s, capture(a, p));
  ws::restore(s, gClosed.w, gClosed.index);
  store(a, p, s);
  applyWorkspace(a, p, gClosed.w, gClosed.w.current);
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

bool gDrawer = false;

float drawStrip(App& a, ImVec2 pos, float width) {
  const Preset p = layoutui::active();
  // K-256: Settings > Layouts > Switching: the strip always, once there are two workspaces, or never (the
  // keys, the menu and the drawer still switch).
  const int when = static_cast<int>(a.settings.getNumber("ui.workspaceStrip"));
  const bool undoing = gClosed.have && gClosed.layout == p && ImGui::GetTime() < gClosed.until;
  if (when == 2 && !undoing) return 0;
  if (when != 0 && load(a, p).items.size() <= 1 && !undoing && !gDrawer) return 0;
  ws::Set s = current(a, p);
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
  // The layout this set belongs to; K-256: a click opens the drawer of cards.
  {
    const char* name = i18n::tr(layout::displayName(p));
    const float tw = a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, name).x;
    const float pw = tw + 30;
    ImGui::SetCursorScreenPos(ImVec2(x, y0));
    if (ImGui::InvisibleButton("##wslayout", ImVec2(pw, h))) gDrawer = !gDrawer;
    const bool hov = ImGui::IsItemHovered();
    if (hov) ImGui::SetTooltip("%s", i18n::tr(gDrawer ? "Hide the workspace cards" : "Show every workspace as a card"));
    dl->AddRectFilled(ImVec2(x, y0), ImVec2(x + pw, y0 + h), gDrawer ? P.surface600 : hov ? theme::mix(P.surface700, P.surface600, 0.5f) : P.surface700, h * 0.5f);
    dl->AddText(a.fontRegular, fs, ImVec2(x + 8, std::round(y0 + (h - fs) * 0.5f)), gDrawer || hov ? P.text : P.textDim, name);
    ui::drawIcon(dl, gDrawer ? ui::Icon::ChevronUp : ui::Icon::ChevronDown, ImVec2(x + pw - 11, y0 + h * 0.5f), 10, P.textDim);
    x += pw + theme::kS2;
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
        if (const ws::Arrangement* r = ws::revert(s)) applyWorkspace(a, p, s.items[static_cast<size_t>(s.active)], *r), changed = true;
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
    if (p == now) applyWorkspace(a, p, s.items[static_cast<size_t>(s.active)], s.items[static_cast<size_t>(s.active)].current);
  }
}

bool importProjectCopy(App& a) {
  const Preset p = layoutui::active();
  const std::map<std::string, std::string>& copy = a.doc.song().workspaceCopy;
  const auto it = copy.find(layout::id(p));
  if (it == copy.end()) return false;
  ws::Set s = ws::fromJson(it->second);
  if (s.items.empty()) return false;
  store(a, p, s);
  applyWorkspace(a, p, s.items[static_cast<size_t>(s.active)], s.items[static_cast<size_t>(s.active)].current);
  return true;
}

void menu(App& a) {
  const Preset p = layoutui::active();
  ws::Set s = current(a, p);
  if (ImGui::MenuItem(i18n::tr("New workspace"))) addNew(a, "");
  if (ImGui::MenuItem(i18n::tr("Add starter workspaces"))) addStarters(a);
  if (ImGui::MenuItem(i18n::tr("Close workspace"), nullptr, false, s.items.size() > 1)) closeIndex(a, s.active);
  // K-256: every workspace as a card, and the full editor in Settings > Layouts.
  if (bool on = gDrawer; ImGui::MenuItem(i18n::tr("Show all as cards"), nullptr, &on)) setDrawerOpen(on);
  if (ImGui::MenuItem(i18n::tr("Workspace settings..."))) {
    a.showSettings = true;
    a.settingsTab = kLayoutsSettingsTab;
    showLayoutsPageFor(static_cast<int>(p));
    a.focusWindow = "Settings";
  }
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

std::string activeOwnBar(App& a, Preset p) {
  const ws::Set s = load(a, p);
  const ws::Workspace* w = ws::activeWorkspace(s);
  return w ? w->ownBar : std::string();
}

std::string activeOwnPins(App& a, Preset p) {
  const ws::Set s = load(a, p);
  const ws::Workspace* w = ws::activeWorkspace(s);
  return w ? w->ownPins : std::string();
}

void setActiveOwn(App& a, Preset p, const std::string* bar, const std::string* pins) {
  ws::Set s = current(a, p);
  ws::Workspace* w = ws::activeWorkspace(s);
  if (!w) return;
  if (bar) w->ownBar = *bar;
  if (pins) w->ownPins = *pins;
  store(a, p, s);
  toolbarui::changed();
}

std::vector<Info> list(App& a, Preset p) {
  const ws::Set s = layoutui::active() == p ? current(a, p) : load(a, p);
  std::vector<Info> out;
  for (int i = 0; i < static_cast<int>(s.items.size()); ++i) {
    const ws::Workspace& w = s.items[static_cast<size_t>(i)];
    Info in;
    in.name = w.name;
    in.active = i == s.active;
    in.open = std::vector<std::string>(w.current.open.begin(), w.current.open.end());
    in.keepWindows = w.keepWindows;
    in.keepPanes = w.keepPanes;
    in.ownBar = w.hasOwnBar();
    in.ownPins = w.hasOwnPins();
    in.changed = w.changed();
    in.regionState = w.current.regionState;
    in.dock = w.current.dock;
    if (i < ws::kKeyed) {
      char id[32];
      std::snprintf(id, sizeof id, "view.workspace%d", i + 1);
      if (const char* l = keys::label(id)) in.key = l;
    }
    out.push_back(in);
  }
  return out;
}

void select(App& a, int i) { selectIndex(a, i); }
void close(App& a, int i) { closeIndex(a, i); }
void addFromNow(App& a) { addNew(a, ""); }
void starters(App& a) { addStarters(a); }

bool rename(App& a, int i, const std::string& name) {
  const Preset p = layoutui::active();
  ws::Set s = current(a, p);
  if (!ws::rename(s, i, name)) return false;
  store(a, p, s);
  return true;
}

void duplicate(App& a, int i) {
  const Preset p = layoutui::active();
  ws::Set s = current(a, p);
  ws::record(s, capture(a, p));
  if (ws::duplicate(s, i) >= 0) store(a, p, s);
}

void move(App& a, int from, int to) {
  const Preset p = layoutui::active();
  ws::Set s = current(a, p);
  if (ws::move(s, from, to)) store(a, p, s);
}

void saveNow(App& a) {
  const Preset p = layoutui::active();
  ws::Set s = current(a, p);
  ws::record(s, capture(a, p));
  ws::save(s);
  store(a, p, s);
}

void revertNow(App& a) {
  const Preset p = layoutui::active();
  ws::Set s = current(a, p);
  if (const ws::Arrangement* r = ws::revert(s)) {
    const ws::Arrangement target = *r;
    store(a, p, s);
    applyWorkspace(a, p, s.items[static_cast<size_t>(s.active)], target);
  }
}

bool autoSave(App& a) { return current(a, layoutui::active()).autoSave; }

void setAutoSave(App& a, bool on) {
  const Preset p = layoutui::active();
  ws::Set s = current(a, p);
  s.autoSave = on;
  store(a, p, s);
}

void setKeeps(App& a, int i, int what, bool on) {
  const Preset p = layoutui::active();
  ws::Set s = current(a, p);
  if (i < 0 || i >= static_cast<int>(s.items.size())) return;
  ws::Workspace& w = s.items[static_cast<size_t>(i)];
  const bool active = i == s.active;
  switch (what) {
    case kKeepWindows: w.keepWindows = on; break;
    case kKeepPanes: w.keepPanes = on; break;
    case kKeepBar:
      // Its own bar starts as the bar it uses now (the layout's); off: back to the layout's.
      w.ownBar = on ? toolbar::barJson(toolbarui::layoutModel(a, p).bar) : std::string();
      break;
    case kKeepPins: w.ownPins = on ? toolbar::pinsJson(toolbarui::layoutModel(a, p).pins) : std::string(); break;
    default: return;
  }
  store(a, p, s);
  if (active) toolbarui::changed();
}


// ---------------------------------------------------------------- drawer ----

namespace {

int gEdit = -1;           // the card turned over to edit (-1 none)
int gCardRename = -1;     // the card whose name is being typed
char gCardBuf[64] = "";

// A miniature of an arrangement: the Studio dock plan's panes (tinted by what they hold), or the region
// shell's open windows as a row of blocks.
void miniature(App& a, ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Info& w) {
  const auto& P = theme::pal();
  dl->AddRectFilled(p0, p1, P.surface950, 4.0f);
  auto tint = [&](const std::string& win) {
    if (win == "Playlist") return theme::withAlpha(P.accent, 0.55f);
    if (win == "Mixer") return theme::withAlpha(P.hot, 0.45f);
    if (win == "Piano roll") return theme::withAlpha(theme::mix(P.accent, P.hot, 0.5f), 0.5f);
    return P.surface700;
  };
  const dockplan::Plan plan = w.dock.empty() ? dockplan::Plan{} : dockplan::fromJson(w.dock);
  const ImVec2 i0(p0.x + 3, p0.y + 3), i1(p1.x - 3, p1.y - 3);
  if (!plan.empty()) {
    struct Walk {
      ImDrawList* dl;
      const std::function<ImU32(const std::string&)>* tint;
      const theme::Palette* P;
      void node(const dockplan::Node& n, ImVec2 a0, ImVec2 a1, int depth) {
        if (n.split && n.kids.size() == 2 && depth < dockplan::kMaxDepth) {
          ImVec2 m0 = a1, m1 = a0;
          if (n.axis == 0) m0.x = m1.x = a0.x + (a1.x - a0.x) * n.ratio;
          else m0.y = m1.y = a0.y + (a1.y - a0.y) * n.ratio;
          node(n.kids[0], a0, ImVec2(n.axis == 0 ? m0.x - 1 : a1.x, n.axis == 1 ? m0.y - 1 : a1.y), depth + 1);
          node(n.kids[1], ImVec2(n.axis == 0 ? m1.x + 1 : a0.x, n.axis == 1 ? m1.y + 1 : a0.y), a1, depth + 1);
          return;
        }
        const std::string front = !n.front.empty() ? n.front : n.windows.empty() ? std::string() : n.windows[0];
        dl->AddRectFilled(a0, a1, n.windows.empty() ? P->surface800 : (*tint)(front), 2.0f);
      }
    };
    const std::function<ImU32(const std::string&)> t = tint;
    Walk{dl, &t, &P}.node(plan.root, i0, i1, 0);
    return;
  }
  // The region shell's layouts: the open windows as blocks across.
  const int n = std::max(1, std::min(static_cast<int>(w.open.size()), 6));
  const float bw = (i1.x - i0.x - (n - 1) * 2) / n;
  for (int k = 0; k < n; ++k) {
    const std::string win = k < static_cast<int>(w.open.size()) ? w.open[static_cast<size_t>(k)] : std::string();
    const float x0 = i0.x + k * (bw + 2);
    dl->AddRectFilled(ImVec2(x0, i0.y), ImVec2(x0 + bw, i1.y), win.empty() ? P.surface800 : tint(win), 2.0f);
  }
  (void)a;
}

}  // namespace

void setDrawerOpen(bool on) {
  gDrawer = on;
  if (!on) gEdit = -1;
}
bool drawerOpen() { return gDrawer; }

float drawDrawer(App& a, ImVec2 pos, float width) {
  if (!gDrawer) return 0;
  const Preset p = layoutui::active();
  const std::vector<Info> items = list(a, p);
  const auto& P = theme::pal();
  const float fs = ImGui::GetFontSize();
  const float gap = theme::kS3, headH = 30;
  // The cards share the width (the New card is a narrower one), between a least and a most.
  const float slots = static_cast<float>(items.size()) + 0.6f;
  const float cardW = std::clamp((width - 2 * theme::kS3 - slots * gap) / slots, 196.0f, 236.0f);
  const float cardH = gEdit >= 0 ? 232 : 176;
  const float H = headH + cardH + theme::kS3 * 2;
  ImGui::SetNextWindowPos(pos);
  ImGui::SetNextWindowSize(ImVec2(width, H));
  ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::vec(P.surface900));
  ImGui::Begin("##wsdrawer", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus);
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
  // Over the floating windows of the arrangement (a popup open elsewhere stays on top).
  if (GImGui->OpenPopupStack.Size == 0) ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddLine(ImVec2(pos.x, pos.y + H - 0.5f), ImVec2(pos.x + width, pos.y + H - 0.5f), P.line);
  // ---- the head: title, starters, the project copy, close ----
  {
    const float hy = pos.y + theme::kS2;
    std::string title = std::string(i18n::tr("Workspaces")) + "  \xC2\xB7  " + i18n::tr(layout::displayName(p));
    dl->AddText(a.fontBold, fs, ImVec2(pos.x + theme::kS3, std::round(hy + (22 - fs) * 0.5f)), P.text, title.c_str());
    float rx = pos.x + width - theme::kS3;
    const char* closeL = i18n::tr("Close");
    const float cw = ui::TextButtonWidth("Close");
    rx -= cw;
    ImGui::SetCursorScreenPos(ImVec2(rx, hy));
    if (ui::TextButton("Close", false, cw)) setDrawerOpen(false);
    (void)closeL;
    bool copy = projectCopyOn(a);
    const char* copyL = i18n::tr("Keep a copy in the project");
    const float kw = ImGui::GetFrameHeight() + ImGui::CalcTextSize(copyL).x + 8;
    rx -= kw + theme::kS3;
    ImGui::SetCursorScreenPos(ImVec2(rx, hy + 1));
    if (ImGui::Checkbox(copyL, &copy)) setProjectCopy(a, copy);
    const float sw = ui::TextButtonWidth("Starter sets");
    rx -= sw + theme::kS3;
    ImGui::SetCursorScreenPos(ImVec2(rx, hy));
    if (ui::TextButton("Starter sets", false, sw, "Add the starter workspaces for this layout")) starters(a);
  }
  // ---- the cards ----
  float x = pos.x + theme::kS3;
  const float y = pos.y + headH + theme::kS2;
  int selectTo = -1, closeTo = -1;
  for (int i = 0; i < static_cast<int>(items.size()); ++i) {
    const Info& w = items[static_cast<size_t>(i)];
    ImGui::PushID(i);
    const ImVec2 c0(x, y), c1(x + cardW, y + cardH);
    const bool editing = gEdit == i;
    const bool hov = ImGui::IsMouseHoveringRect(c0, c1) && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    dl->AddRectFilled(c0, c1, P.surface800, 6.0f);
    dl->AddRect(c0, c1, w.active ? P.accent : hov ? P.lineStrong : P.line, 6.0f, 0, w.active ? 1.5f : 1.0f);
    const float ix = x + theme::kS2, iw = cardW - 2 * theme::kS2;
    float cy = y + theme::kS2;
    // name and key chip
    if (gCardRename == i) {
      ImGui::SetCursorScreenPos(ImVec2(ix, cy - 2));
      ImGui::SetNextItemWidth(iw - 40);
      if (!ImGui::IsAnyItemActive()) ImGui::SetKeyboardFocusHere();
      if (ImGui::InputText("##cardname", gCardBuf, sizeof gCardBuf, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
        rename(a, i, gCardBuf);
        gCardRename = -1;
      } else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) gCardRename = -1;
    } else {
      theme::textEllipsis(dl, a.fontBold, fs, ImVec2(ix, cy), iw - 44, P.text, w.name.c_str());
      ImGui::SetCursorScreenPos(ImVec2(ix, cy));
      ImGui::InvisibleButton("##name", ImVec2(iw - 44, fs + 2));
      if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) gCardRename = i, std::snprintf(gCardBuf, sizeof gCardBuf, "%s", w.name.c_str());
    }
    if (!w.key.empty()) {
      const float kw = a.fontRegular->CalcTextSizeA(theme::kFontCaption, 1e9f, 0, w.key.c_str()).x + 10;
      const ImVec2 k0(x + cardW - theme::kS2 - kw, cy - 1);
      dl->AddRectFilled(k0, ImVec2(k0.x + kw, k0.y + fs + 2), P.surface700, 3.0f);
      dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(k0.x + 5, k0.y + 2), P.textDim, w.key.c_str());
    }
    cy += fs + theme::kS2;
    if (!editing) {
      // miniature
      const float mh = 86;
      miniature(a, dl, ImVec2(ix, cy), ImVec2(ix + iw, cy + mh), w);
      ImGui::SetCursorScreenPos(ImVec2(ix, cy));
      if (ImGui::InvisibleButton("##mini", ImVec2(iw, mh)) && !w.active) selectTo = i;
      cy += mh + 6;
      // what it holds
      char line[128];
      const char* sizes = w.keepPanes ? i18n::tr("sizes") : i18n::tr("no sizes");
      std::snprintf(line, sizeof line, i18n::tr("%d windows / %s / %s"), static_cast<int>(w.open.size()), sizes,
                    w.ownPins ? i18n::tr("own pins") : w.ownBar ? i18n::tr("own bar") : i18n::tr("layout pins"));
      theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption, ImVec2(ix, cy), iw, P.textFaint, line);
      if (w.changed) dl->AddCircleFilled(ImVec2(ix + iw - 3, cy + theme::kFontCaption * 0.5f), 2.5f, P.accent);
      cy += theme::kFontCaption + 8;
      // Switch and "..."
      ImGui::SetCursorScreenPos(ImVec2(ix, cy));
      if (w.active) {
        dl->AddText(a.fontRegular, fs, ImVec2(ix, cy + 3), P.textDim, i18n::tr("In use"));
      } else if (ui::PrimaryButton("Switch", iw - 32)) {
        selectTo = i;
      }
      ImGui::SetCursorScreenPos(ImVec2(ix + iw - 26, cy));
      if (ui::IconButton("##more", ui::Icon::More, false, 26, "Edit this workspace")) gEdit = i;
    } else {
      // The card turned over: what this workspace keeps.
      const float fy = cy;
      dl->AddText(a.fontBold, theme::kFontLabel, ImVec2(ix, fy), P.textFaint, i18n::tr("THIS WORKSPACE KEEPS"));
      {  // Done, at the head of the turned card
        const float dw = ui::TextButtonWidth("Done");
        ImGui::SetCursorScreenPos(ImVec2(ix + iw - dw, fy - 4));
        if (ui::PrimaryButton("Done", dw)) gEdit = -1;
      }
      cy += theme::kFontLabel + 6;
      struct Row {
        const char* label;
        bool on;
        int what;
      };
      const Row rows[] = {{"Its windows", w.keepWindows, kKeepWindows},
                          {"Its pane sizes", w.keepPanes, kKeepPanes},
                          {"Its own control bar", w.ownBar, kKeepBar},
                          {"Its own pinned commands", w.ownPins, kKeepPins}};
      for (const Row& r : rows) {
        bool on = r.on;
        ImGui::SetCursorScreenPos(ImVec2(ix, cy));
        if (ImGui::Checkbox(i18n::tr(r.label), &on)) setKeeps(a, i, r.what, on);
        cy += ImGui::GetFrameHeight() + 2;
      }
      ImGui::SetCursorScreenPos(ImVec2(ix, cy));
      if (ui::TextButton("Customise its bar...", false, 0, "This workspace keeps a bar of its own, set in the list that opens")) {
        if (!w.active) selectTo = i;
        if (!w.ownBar) setKeeps(a, i, kKeepBar, true);
        toolbarui::openBarList(a);
      }
      cy += ImGui::GetFrameHeight() + 4;
      // Save as now and Revert side by side; Duplicate and Close below.
      const float bw = (iw - 4) / 2;
      ImGui::SetCursorScreenPos(ImVec2(ix, cy));
      if (ui::TextButton("Save as now", false, bw, "Keep the arrangement on screen now as this workspace's") && w.active) saveNow(a);
      ImGui::SetCursorScreenPos(ImVec2(ix + bw + 4, cy));
      if (ui::TextButton("Revert", false, bw, "Back to the arrangement it was saved with") && w.active) revertNow(a);
      cy += ImGui::GetFrameHeight() + 4;
      ImGui::SetCursorScreenPos(ImVec2(ix, cy));
      if (ui::TextButton("Duplicate", false, bw, "A copy of this workspace, after it")) duplicate(a, i);
      if (items.size() > 1) {
        ImGui::SetCursorScreenPos(ImVec2(ix + bw + 4, cy));
        if (ui::TextButton("Close workspace", false, bw, "Remove this workspace (the windows stay as they are)")) closeTo = i, gEdit = -1;
      }
    }
    ImGui::SetCursorScreenPos(c0);
    ImGui::Dummy(ImVec2(cardW, cardH));
    if (ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GL_WORKSPACE")) move(a, *static_cast<const int*>(pl->Data), i);
      ImGui::EndDragDropTarget();
    }
    ImGui::PopID();
    x += cardW + gap;
  }
  // ---- New workspace ----
  if (static_cast<int>(items.size()) < ws::kMaxWorkspaces) {
    const ImVec2 c0(x, y), c1(x + cardW * 0.6f, y + cardH);
    ImGui::SetCursorScreenPos(c0);
    if (ImGui::InvisibleButton("##wsnewcard", ImVec2(c1.x - c0.x, c1.y - c0.y))) addFromNow(a);
    const bool hov = ImGui::IsItemHovered();
    dl->AddRect(c0, c1, hov ? P.lineStrong : P.line, 6.0f, 0, 1.0f);
    const ImVec2 m((c0.x + c1.x) * 0.5f, (c0.y + c1.y) * 0.5f - 10);
    ui::drawIcon(dl, ui::Icon::Plus, m, 16, hov ? P.text : P.textDim);
    theme::centreText(dl, a.fontRegular, theme::kFontCaption, ImVec2(c0.x, m.y + 14), ImVec2(c1.x, m.y + 30), hov ? P.text : P.textDim,
                      i18n::tr("New workspace"));
    if (hov) ImGui::SetTooltip("%s", i18n::tr("New workspace from the current arrangement"));
  }
  ImGui::End();
  if (selectTo >= 0) select(a, selectTo);
  if (closeTo >= 0) close(a, closeTo);
  return H;
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
  else if (c == "keep" && args.size() > 3) {  // K-256: "workspace keep <n> windows|panes|bar|pins on|off"
    const std::string& w = args[2];
    const int what = w == "windows" ? kKeepWindows : w == "panes" ? kKeepPanes : w == "bar" ? kKeepBar : w == "pins" ? kKeepPins : -1;
    if (what >= 0) setKeeps(a, std::atoi(args[1].c_str()), what, args[3] == "on");
  } else if (c == "drawer") {  // K-256: the drawer of cards open or closed ("drawer edit <n>" turns a card over)
    setDrawerOpen(arg != "off");
    if (args.size() > 2 && args[1] == "edit") gEdit = std::atoi(args[2].c_str());
  } else if (c == "dockcheck") {  // K-256: the Studio dock tree against the plan last built
    std::printf("WORKSPACE dockcheck %s\n", dockplanui::devCheck(a).c_str());
    std::fflush(stdout);
  } else if (c == "switchcheck") {  // K-256: every open window drawn, inside the main window (as --switch-check)
    const std::string bad = layoutui::switchCheck(a);
    std::printf("WORKSPACE switchcheck %s\n", bad.empty() ? "ok" : bad.c_str());
    std::fflush(stdout);
  } else if (c == "dock") {  // K-256: the active workspace's dock plan
    const ws::Set s = current(a, p);
    const ws::Workspace* w = ws::activeWorkspace(s);
    std::printf("WORKSPACE dock %s\n", w ? w->current.dock.c_str() : "-");
    std::fflush(stdout);
  }
  else if (c == "rename" && args.size() > 2) {
    ws::Set s = current(a, p);
    std::string name = args[2];
    for (size_t i = 3; i < args.size(); ++i) name += " " + args[i];
    if (ws::rename(s, std::atoi(args[1].c_str()), name)) store(a, p, s);
  } else if (c == "dump") {
    const ws::Set s = current(a, p);
    std::printf("WORKSPACES %s active %d:", layout::id(p), s.active);
    for (const ws::Workspace& w : s.items)
      std::printf(" [%s%s%s%s%s%s]", w.name.c_str(), w.changed() ? " *" : "", w.hasOwnBar() ? " bar" : "", w.hasOwnPins() ? " pins" : "",
                  w.keepWindows ? "" : " -windows", w.keepPanes ? "" : " -panes");
    std::printf(" copy %s", projectCopyOn(a) ? "on" : "off");
    std::printf("\n");
    std::fflush(stdout);
  }
}

}  // namespace gl::workspacesui
