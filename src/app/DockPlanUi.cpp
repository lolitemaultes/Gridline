// Gridline - the Studio layout's dock tree as a workspace keeps it. See DockPlanUi.h.
#include "DockPlanUi.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/DockPlan.h"
#include "App.h"
#include "LayoutPresets.h"
#include "LayoutShellUi.h"
#include "Widgets.h"

namespace gl::dockplanui {

namespace {

std::string gPending;           // a plan the next dock build makes
dockplan::Plan gLast;           // the last plan built (dev check)
bool gHaveLast = false;
std::vector<dockplan::Float> gFloats;  // floating windows still to place (they may not exist yet)
std::vector<std::string> gFronts;      // each pane's front window, to bring forward
int gFrames = 0;                       // frames left to place them in
ImVec2 gOrigin(0, 0), gSize(0, 0);     // the dock space of the last build

bool dialogWindow(const char* w) {
  static const char* const kDialogs[] = {"Settings", "Keyboard shortcuts", "Project info", "Plugin manager", "Collaboration", "Sync review"};
  for (const char* d : kDialogs)
    if (std::strcmp(d, w) == 0) return true;
  return false;
}

bool known(App& a, const std::string& w) { return layoutui::windowOpenFlag(a, w) != nullptr && !dialogWindow(w.c_str()); }

ImGuiDockNode* rootNode() {
  ImGuiWindow* host = ImGui::FindWindowByName("##dockhost");
  return host ? ImGui::DockBuilderGetNode(host->GetID("GridlineDock")) : nullptr;
}

dockplan::Node captureNode(const ImGuiDockNode* n) {
  dockplan::Node out;
  if (!n) return out;
  if (n->IsSplitNode() && n->ChildNodes[0] && n->ChildNodes[1]) {
    out.split = true;
    out.axis = n->SplitAxis == ImGuiAxis_Y ? 1 : 0;
    const int ax = out.axis;
    const float s0 = n->ChildNodes[0]->Size[ax], s1 = n->ChildNodes[1]->Size[ax];
    out.ratio = s0 + s1 > 0 ? s0 / (s0 + s1) : 0.5f;
    out.kids = {captureNode(n->ChildNodes[0]), captureNode(n->ChildNodes[1])};
    return out;
  }
  out.central = n->IsCentralNode();
  if (n->TabBar && n->TabBar->Tabs.Size > 0) {
    for (const ImGuiTabItem& t : n->TabBar->Tabs)
      if (t.Window) out.windows.push_back(ui::stableWindowName(t.Window->Name));
  } else {
    for (const ImGuiWindow* w : n->Windows) out.windows.push_back(ui::stableWindowName(w->Name));
  }
  if (n->VisibleWindow) out.front = ui::stableWindowName(n->VisibleWindow->Name);
  return out;
}

void buildNode(const dockplan::Node& n, ImGuiID id) {
  if (!n.split) {
    int order = 0;
    for (const std::string& w : n.windows) {
      ImGui::DockBuilderDockWindow(w.c_str(), id);
      if (ImGuiWindow* win = ImGui::FindWindowByName(w.c_str())) win->DockOrder = static_cast<short>(order);  // the tabs' order
      ++order;
    }
    if (!n.front.empty()) gFronts.push_back(n.front);
    return;
  }
  ImGuiID first = 0, second = 0;
  // The main area must stay on the side that keeps the dock space's central node: ImGui hands it to the
  // side opposite the split direction, so split from the other side when it is in the first child.
  if (dockplan::hasCentral(n.kids[0])) {
    ImGui::DockBuilderSplitNode(id, n.axis == 1 ? ImGuiDir_Down : ImGuiDir_Right, 1.0f - n.ratio, &second, &first);
  } else {
    ImGui::DockBuilderSplitNode(id, n.axis == 1 ? ImGuiDir_Up : ImGuiDir_Left, n.ratio, &first, &second);
  }
  buildNode(n.kids[0], first);
  buildNode(n.kids[1], second);
}

dockplan::Node* leafWith(dockplan::Node& n, const std::string& w) {
  if (n.split) {
    for (dockplan::Node& k : n.kids)
      if (dockplan::Node* f = leafWith(k, w)) return f;
    return nullptr;
  }
  for (const std::string& x : n.windows)
    if (x == w) return &n;
  return nullptr;
}

dockplan::Node* centralLeaf(dockplan::Node& n) {
  if (!n.split) return n.central ? &n : nullptr;
  for (dockplan::Node& k : n.kids)
    if (dockplan::Node* f = centralLeaf(k)) return f;
  return nullptr;
}

// Studio's docked windows a plan does not name (closed when it was kept) get a pane, so opening one later
// docks it with its usual neighbour, or in the main area, as the default build does; the channel rack floats.
void placeTheRest(dockplan::Plan& p) {
  const std::vector<std::string> named = dockplan::windows(p);
  auto has = [&](const char* w) { return std::find(named.begin(), named.end(), w) != named.end(); };
  struct Home {
    const char* window;
    const char* buddy;
  };
  static const Home kHomes[] = {{"Browser", "Undo history"}, {"Undo history", "Browser"}, {"Playlist", "Piano roll"},
                                {"Piano roll", "Playlist"}, {"Mixer", nullptr}};
  for (const Home& h : kHomes) {
    if (has(h.window)) continue;
    dockplan::Node* leaf = h.buddy ? leafWith(p.root, h.buddy) : nullptr;
    if (!leaf) leaf = centralLeaf(p.root);
    if (leaf) leaf->windows.push_back(h.window);
  }
}

}  // namespace

std::string capture(App& a) {
  if (layoutui::active() != layout::Preset::Studio) return "";
  const ImGuiDockNode* root = rootNode();
  if (!root) return "";
  dockplan::Plan p;
  p.root = captureNode(root);
  for (const layoutui::WindowFlag& f : layoutui::windowFlags(a)) {
    if (!*f.open || dialogWindow(f.window)) continue;
    const ImGuiWindow* w = ImGui::FindWindowByName(f.window);
    if (!w || !w->WasActive) continue;
    const ImGuiDockNode* top = w->DockIsActive ? w->DockNode : nullptr;
    while (top && top->ParentNode) top = top->ParentNode;
    const bool docked = top == root;
    if (docked) continue;
    p.floating.push_back({f.window, w->Pos.x - root->Pos.x, w->Pos.y - root->Pos.y, w->Size.x, w->Size.y});
  }
  dockplan::sanitise(p, [&](const std::string& w) { return known(a, w); });
  return dockplan::toJson(p);
}

void request(App& a, const std::string& planText) {
  if (planText.empty() || layoutui::active() != layout::Preset::Studio) return;
  gPending = planText;
  (void)a;
}

bool buildPending(App& a, ImGuiID dockspace, ImVec2 size) {
  if (gPending.empty()) return false;
  dockplan::Plan p = dockplan::fromJson(gPending);
  gPending.clear();
  dockplan::sanitise(p, [&](const std::string& w) { return known(a, w); });
  if (p.empty()) return false;
  gLast = p;  // (the dev check compares with the plan as kept, before the rest is placed)
  placeTheRest(p);
  ImGui::DockBuilderRemoveNode(dockspace);
  ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
  ImGui::DockBuilderSetNodeSize(dockspace, size);
  gFronts.clear();
  buildNode(p.root, dockspace);
  ImGui::DockBuilderFinish(dockspace);
  gFloats = p.floating;
  for (const dockplan::Float& f : gFloats) ImGui::DockBuilderDockWindow(f.window.c_str(), 0);  // floating, not in a pane
  gFrames = 8;
  gHaveLast = true;
  gSize = size;
  if (const ImGuiDockNode* n = ImGui::DockBuilderGetNode(dockspace)) gOrigin = n->Pos;
  return true;
}

void frame(App& a) {
  (void)a;
  if (gFrames <= 0) return;
  --gFrames;
  const ImGuiDockNode* root = rootNode();
  const ImVec2 origin = root ? root->Pos : gOrigin;
  const ImVec2 size = root ? root->Size : gSize;
  for (size_t i = 0; i < gFloats.size();) {
    ImGuiWindow* w = ImGui::FindWindowByName(gFloats[i].window.c_str());
    if (!w || !w->WasActive) {
      ++i;
      continue;
    }
    if (w->DockIsActive) {  // still docked from before: undock it first (done at the next frame's start)
      ImGui::DockContextQueueUndockWindow(ImGui::GetCurrentContext(), w);
      ++i;
      continue;
    }
    const dockplan::Float f = dockplan::fitInto(gFloats[i], size.x, size.y);
    ImGui::SetWindowPos(w, ImVec2(std::round(origin.x + f.x), std::round(origin.y + f.y)), ImGuiCond_Always);
    ImGui::SetWindowSize(w, ImVec2(f.w, f.h), ImGuiCond_Always);
    gFloats.erase(gFloats.begin() + static_cast<std::ptrdiff_t>(i));
  }
  for (size_t i = 0; i < gFronts.size();) {
    ImGuiWindow* w = ImGui::FindWindowByName(gFronts[i].c_str());
    ImGuiDockNode* n = w && w->DockIsActive ? w->DockNode : nullptr;
    if (n && n->TabBar) {
      n->TabBar->NextSelectedTabId = w->TabId;
      gFronts.erase(gFronts.begin() + static_cast<std::ptrdiff_t>(i));
    } else {
      ++i;
    }
  }
  if (gFrames == 0) gFloats.clear(), gFronts.clear();
}

std::string devCheck(App& a) {
  if (!gHaveLast) return "no plan built";
  dockplan::Plan now = dockplan::fromJson(capture(a));
  dockplan::sanitise(now, [&](const std::string& w) { return known(a, w); });
  if (dockplan::similar(now, gLast)) return "same";
  return "differs now " + dockplan::toJson(now) + " built " + dockplan::toJson(gLast);
}

}  // namespace gl::dockplanui
