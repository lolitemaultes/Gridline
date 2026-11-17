// Gridline - the region shell in the interface (K-112). See LayoutShellUi.h.
#include "LayoutShellUi.h"
#include <SDL.h>
#include <SDL_opengl.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string_view>
#include <vector>
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/I18n.h"
#include "../core/LayoutHomes.h"
#include "../core/PaneTabs.h"
#include "../core/PluginInfo.h"
#include "../core/Project.h"
#include "../core/SemanticAnchor.h"
#include "../core/Settings.h"
#include "App.h"
#include "CollabUi.h"
#include "GlEpoch.h"
#include "HostedBodies.h"
#include "TrackReceiptUi.h"
#include "McpUi.h"
#include "LayoutParts.h"
#include "J31Ui.h"
#include "Keys.h"
#include "AutomationEditor.h"
#include "LayoutPresets.h"
#include "LayoutStyleUi.h"
#include "Theme.h"
#include "TrackerView.h"
#include "Widgets.h"
#include "Windows.h"
#include "../packs/PacksUi.h"

namespace gl::layoutui {

using layout::HomeKind;
using layout::Preset;

// ------------------------------------------------------------ registry ----

namespace {
// Open flags of the new parts (K-112 R4), by window name. Closed until the person opens one.
std::map<std::string, bool>& partFlags() {
  static std::map<std::string, bool> m;
  return m;
}
}  // namespace

namespace {
const App* gFlagsFor = nullptr;
std::vector<WindowFlag> gFlags;
std::map<std::string, bool*> gFlagByName;
}  // namespace

const std::vector<WindowFlag>& windowFlags(App& a) {
  if (gFlagsFor == &a) return gFlags;  // the flags are members of the App and the part map's nodes: they never move
  std::vector<WindowFlag> v = {
      {"Browser", &a.showBrowser},       {"Playlist", &a.showPlaylist},
      {"Piano roll", &a.showPiano},      {"Channel rack", &a.showRack},
      {"Mixer", &a.showMixer},           {"Live pads", &a.showLivePads},
      {"Master chain", &a.showMaster},   {"Undo history", &a.showHistory},
      {"Performance", &a.showPerformance}, {"Settings", &a.showSettings},
      {"Keyboard shortcuts", &a.showShortcuts}, {"Project info", &a.showInfo},
      {"Plugin manager", &a.showPluginManager}, {"Collaboration", &a.showCollab},
      {"Sync review", &a.showSyncReview}, {"Groove editor", &a.showGroove},
  };
  for (const layout::Item& it : layout::items())
    if (std::strncmp(it.id, "N", 1) == 0 && *it.window && !*it.later) v.push_back({it.window, &partFlags()[it.window]});
  gFlags = std::move(v);
  gFlagByName.clear();
  for (const WindowFlag& f : gFlags) gFlagByName.emplace(f.window, f.open);
  gFlagsFor = &a;
  return gFlags;
}

bool* windowOpenFlag(App& a, const std::string& stableName) {
  windowFlags(a);
  const auto it = gFlagByName.find(stableName);
  return it == gFlagByName.end() ? nullptr : it->second;
}

// ------------------------------------------------------- dev layouts ----

namespace {
bool gDevLayouts = false;
}
// K-112: development builds only (GRIDLINE_DEV_LAYOUTS); a release build ignores --layout-dev and ui.layoutDev.
#ifdef GRIDLINE_DEV_LAYOUTS
void setDevLayouts(bool on) { gDevLayouts = on, layout::setDevRun(on); }
#else
void setDevLayouts(bool) {}
#endif
bool devLayouts(const App& a) { return gDevLayouts || layout::devSetting(a.settings); }

bool shellBuilt(const App& a, Preset p) {
  if (p == Preset::Tracker) return true;
  return (p == Preset::Live || p == Preset::Logic) && devLayouts(a);
}

std::string iniFile(const App& a, Preset p) {
  if (p == Preset::Tracker || !shellBuilt(a, p)) return layout::iniFileName(p);
  return std::string("layout-") + layout::id(p) + "-shell.ini";
}

std::string versionKey(const App& a, Preset p) {
  if (p == Preset::Tracker || !shellBuilt(a, p)) return layout::versionKey(p);
  return layout::versionKey(p) + ".shell";
}

// --------------------------------------------------------------- state ----

namespace {
layout::State gRunState[layout::kLayoutCount];  // a --layout run keeps its state in memory only
bool gRunStateSet[layout::kLayoutCount] = {};
bool gRebuild = false;
std::map<std::string, ImGuiID> gNodes;  // region -> dock node of the last build (where its windows dock)
std::map<std::string, ImGuiID> gOuter;  // region -> its whole node (with its title strip)
std::map<std::string, ImGuiID> gStrip;  // Segmented region -> its title strip node
std::map<std::string, ImGuiID> gRail;   // K-237: Rail region -> its vertical tab rail node
Preset gBuiltFor = Preset::Count;
int gBuiltFrame = -100;  // K-297: the frame the dock tree was last built
std::set<std::string> gClosedByGroup;  // windows an exclusive region closed (open for the person)
// K-297: each region shell's panes and regions as they were last left in this run (never saved).
struct Kept {
  layout::State state;
  std::map<std::string, bool> open;  // pane window -> open
  std::vector<std::string> fronts;   // K-297 (round 9): the tabs in front where a region holds several
};
std::map<int, Kept> gKept;  // layout -> what it was left with
std::set<int> gRestorePanes;  // layouts entered again: their panes come back in showShellWindows
std::map<std::string, int> gFrontTabs;  // K-297 (round 8): window -> frames waited; the tab a restart brings to the front
// R01-04 (Tracks layout): the windows open there last frame, and the frame until which a return
// to the layout restores them (the dock build after a switch can land a few frames late).
std::set<std::string> gLogicOpen;
bool gLogicHaveOpen = false;
int gLogicRestoreUntil = -1;
void restoreLogicWindows(App& a);
std::vector<std::string> frontTabs(App& a, Preset p);
void bringFronts(Preset p, const std::vector<std::string>& fronts, const std::set<std::string>* open, bool trackerPages);

// K-237: windows whose tab the person dragged to another pane (State::moved) live there: their home
// in this layout is the table's with that pane as its region. Kept in step with the region state.
std::map<std::string, layout::Home> gMovedHome[layout::kLayoutCount];
std::map<std::string, std::string> gMovedFrom[layout::kLayoutCount];
void syncMoved(Preset p, const layout::State& st) {
  const int k = static_cast<int>(p);
  if (k < 0 || k >= layout::kLayoutCount || gMovedFrom[k] == st.moved) return;
  gMovedFrom[k] = st.moved;
  gMovedHome[k].clear();
  for (const auto& [w, region] : st.moved)
    if (const layout::Item* it = layout::findWindow(w); it && layout::findRegion(p, region)) {
      layout::Home h = it->home[k];
      h.region = layout::findRegion(p, region)->id;  // (the shell's own text: it lives as long as the program)
      gMovedHome[k][w] = h;
    }
}
// A window's home in layout p, with a moved tab's pane.
const layout::Home& homeIn(const layout::Item& it, Preset p) {
  const int k = static_cast<int>(p);
  if (k >= 0 && k < layout::kLayoutCount && *it.window && !gMovedHome[k].empty())
    if (const auto f = gMovedHome[k].find(it.window); f != gMovedHome[k].end()) return f->second;
  return it.home[k];
}
layout::Home windowHomeIn(const std::string& window, Preset p) {
  if (const layout::Item* it = layout::findWindow(window)) return homeIn(*it, p);
  return layout::windowHome(window, p);
}
}  // namespace

layout::State regionState(const App& a, Preset p) {
  const int k = static_cast<int>(p);
  if (hasDevOverride() && gRunStateSet[k]) return syncMoved(p, gRunState[k]), gRunState[k];
  // Read every frame: parsed only when the stored text changed.
  static std::string text[layout::kLayoutCount];
  static layout::State parsed[layout::kLayoutCount];
  static bool have[layout::kLayoutCount] = {};
  if (k < 0 || k >= layout::kLayoutCount) return layout::fromJson(p, a.settings.getString(layout::stateKey(p)));
  std::string now = a.settings.getString(layout::stateKey(p));
  if (!have[k] || now != text[k]) {
    parsed[k] = layout::fromJson(p, now);
    text[k] = std::move(now);
    have[k] = true;
  }
  syncMoved(p, parsed[k]);
  return parsed[k];
}

void setRegionState(App& a, Preset p, const layout::State& st) {
  const int k = static_cast<int>(p);
  syncMoved(p, st);
  if (hasDevOverride()) {
    gRunState[k] = st;
    gRunStateSet[k] = true;
    return;
  }
  a.settings.set(layout::stateKey(p), layout::toJson(st));
}

void setRegionVisible(App& a, const std::string& region, bool on, bool alone) {
  const Preset p = active();
  if (!layout::findRegion(p, region)) return;
  layout::State st = regionState(a, p);
  if (layout::visible(layout::shell(p), st, region) == on) return;
  // K-112b: showing a region inside a hidden one (the quick help box in the right pane, now closed
  // by default) shows the one around it too, or nothing would appear.
  if (on)
    if (const layout::Region* r = layout::findRegion(p, region); r && *r->parent && !layout::visible(layout::shell(p), st, r->parent)) {
      setRegionVisible(a, r->parent, true);
      st = regionState(a, p);
    }
  // K-112c: a region of a group (the Clips detail bands) hides the others first, with their windows.
  if (const layout::Region* r = layout::findRegion(p, region); on && alone && r && *r->group) {
    for (const layout::Region& o : layout::shell(p).regions)
      if (region != o.id && std::string(o.group) == r->group) setRegionVisible(a, o.id, false);
    st = regionState(a, p);
  }
  if (layout::visible(layout::shell(p), st, region) != on) {
    layout::setVisible(layout::shell(p), st, region, on, alone);
    setRegionState(a, p, st);
  }
  // A hidden region's windows close with it; a shown one opens its default windows.
  for (const layout::Item& it : layout::items()) {
    const layout::Home& h = homeIn(it, p);
    if (!*it.window || region != h.region) continue;
    if (bool* f = windowOpenFlag(a, it.window)) {
      if (!on) *f = false;
      else if (h.openByDefault) *f = true;
    }
  }
  gRebuild = true;
}

void requestShellRebuild() { gRebuild = true; }

bool takeShellRebuild() {
  const bool r = gRebuild;
  gRebuild = false;
  return r;
}

// ------------------------------------------------------------ building ----

namespace {

ImGuiDir dirOf(layout::Edge e) {
  switch (e) {
    case layout::Edge::Left: return ImGuiDir_Left;
    case layout::Edge::Right: return ImGuiDir_Right;
    case layout::Edge::Top: return ImGuiDir_Up;
    default: return ImGuiDir_Down;
  }
}

}  // namespace

ImGuiID regionNode(const std::string& region) {
  auto it = gNodes.find(region);
  return it == gNodes.end() ? 0 : it->second;
}

namespace {
// The whole node of a region (its title strip included): its size is the region's size.
ImGuiID outerNode(const std::string& region) {
  auto it = gOuter.find(region);
  return it == gOuter.end() ? regionNode(region) : it->second;
}
}  // namespace

std::string stripWindowName(const std::string& region) { return "##pane " + region; }

void buildShell(App& a, Preset p, ImGuiID dockspace, ImVec2 size) {
  const layout::Shell& sh = layout::shell(p);
  const layout::State st = regionState(a, p);
  // K-297 (round 8): the Tracker's page in front before a rebuild of the same layout (a region shown
  // or hidden) stays in front after it; it went back to the pattern page.
  std::string trackerFront = "Pattern editor";
  if (p == Preset::Tracker && gBuiltFor == p)
    if (const ImGuiDockNode* n = ImGui::DockBuilderGetNode(regionNode("tracker.pages")); n && n->VisibleWindow)
      if (const layout::Item* it = layout::findWindow(ui::stableWindowName(n->VisibleWindow->Name)); it && *it->window) trackerFront = it->window;
  ImGui::DockBuilderRemoveNode(dockspace);
  ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
  ImGui::DockBuilderSetNodeSize(dockspace, size);
  gNodes.clear();
  gOuter.clear();
  gStrip.clear();
  gRail.clear();
  // The same walk as layout::solve: each region splits off what is left of its parent.
  std::map<std::string, ImGuiID> rest{{"", dockspace}};
  std::map<std::string, ImVec2> restSize{{"", size}};
  for (const layout::Region& r : sh.regions) {
    if (!layout::visible(sh, st, r.id) || !rest.count(r.parent)) continue;
    const bool hz = r.edge == layout::Edge::Left || r.edge == layout::Edge::Right;
    ImVec2& ps = restSize[r.parent];
    const float parentExt = hz ? ps.x : ps.y;
    const std::string member = r.exclusive && st.member.count(r.id) ? st.member.at(r.id) : std::string();
    const float e = layout::regionExtent(r, parentExt, st, member);
    ImGuiID node = 0, remaining = 0;
    ImGui::DockBuilderSplitNode(rest[r.parent], dirOf(r.edge), parentExt > 0 ? e / parentExt : 0.25f, &node, &remaining);
    rest[r.parent] = remaining;
    (hz ? ps.x : ps.y) = std::max(1.0f, parentExt - e);
    ImVec2 own = hz ? ImVec2(e, ps.y) : ImVec2(ps.x, e);
    if (ImGuiDockNode* n = ImGui::DockBuilderGetNode(node)) n->SizeRef = own;
    gOuter[r.id] = node;
    // A Segmented region: a title strip along its top names its windows (the region's own tabs);
    // its windows and its child regions share what is under it.
    if (r.tabs == layout::TabStyle::Segmented && own.y > layout::kPaneStripH * 2) {
      ImGuiID strip = 0, body = 0;
      ImGui::DockBuilderSplitNode(node, ImGuiDir_Up, layout::kPaneStripH / own.y, &strip, &body);
      if (ImGuiDockNode* n = ImGui::DockBuilderGetNode(strip)) {
        n->SizeRef = ImVec2(own.x, layout::kPaneStripH);
        n->SetLocalFlags(n->LocalFlags | ImGuiDockNodeFlags_NoTabBar | ImGuiDockNodeFlags_NoDockingOverMe | ImGuiDockNodeFlags_NoDockingSplit |
                         ImGuiDockNodeFlags_NoResizeY);
      }
      ImGui::DockBuilderDockWindow(stripWindowName(r.id).c_str(), strip);
      gStrip[r.id] = strip;
      node = body;
      own.y -= layout::kPaneStripH;
      if (ImGuiDockNode* n = ImGui::DockBuilderGetNode(node)) n->SizeRef = own;
    } else if (r.tabs == layout::TabStyle::Rail && own.x > layout::kPaneRailW * 3) {
      // K-237: a Rail region: a narrow vertical tab rail along its outer edge names its windows.
      ImGuiID rail = 0, body = 0;
      const ImGuiDir side = r.edge == layout::Edge::Left ? ImGuiDir_Left : ImGuiDir_Right;
      ImGui::DockBuilderSplitNode(node, side, layout::kPaneRailW / own.x, &rail, &body);
      if (ImGuiDockNode* n = ImGui::DockBuilderGetNode(rail)) {
        n->SizeRef = ImVec2(layout::kPaneRailW, own.y);
        n->SetLocalFlags(n->LocalFlags | ImGuiDockNodeFlags_NoTabBar | ImGuiDockNodeFlags_NoDockingOverMe | ImGuiDockNodeFlags_NoDockingSplit |
                         ImGuiDockNodeFlags_NoResizeX);
      }
      ImGui::DockBuilderDockWindow(stripWindowName(r.id).c_str(), rail);
      gRail[r.id] = rail;
      node = body;
      own.x -= layout::kPaneRailW;
      if (ImGuiDockNode* n = ImGui::DockBuilderGetNode(node)) n->SizeRef = own;
    }
    rest[r.id] = node;
    restSize[r.id] = own;
  }
  for (const layout::Region& r : sh.regions)
    if (rest.count(r.id)) {
      gNodes[r.id] = rest[r.id];
      // A Segmented region's strip names its windows, so it draws no tab bar; in Logic, the single-window
      // panes (TabStyle::None) title themselves too. Elsewhere a None region keeps its tab bar: a window
      // brought forward (a chip opening the master chain) selects its tab there.
      if (layout::hasPaneTabs(r) || (r.tabs == layout::TabStyle::None && p == Preset::Logic))
        if (ImGuiDockNode* n = ImGui::DockBuilderGetNode(rest[r.id])) n->SetLocalFlags(n->LocalFlags | ImGuiDockNodeFlags_NoTabBar);
    }
  gNodes[sh.centre] = rest[""];
  gOuter[sh.centre] = rest[""];
  if (ImGuiDockNode* centre = ImGui::DockBuilderGetNode(rest[""])) {
    centre->SetLocalFlags(centre->LocalFlags | ImGuiDockNodeFlags_CentralNode);
    if (sh.centreTabs != layout::TabStyle::Dock) centre->SetLocalFlags(centre->LocalFlags | ImGuiDockNodeFlags_NoTabBar);
  }
  // Every homed window into its region (closed ones too: they open there later).
  for (const layout::Item& it : layout::items()) {
    if (!*it.window) continue;
    const layout::Home& h = homeIn(it, p);
    if (h.kind != HomeKind::Dock && h.kind != HomeKind::Part) continue;
    const ImGuiID node = regionNode(h.region);
    if (node) ImGui::DockBuilderDockWindow(it.window, node);
  }
  ImGui::DockBuilderFinish(dockspace);
  // K-112c: Live's panels show no tab bar while they hold one part; a second window docked there
  // (the master chain beside the device band) brings the tabs back so both stay reachable. The
  // side panel keeps its tabs (it holds several tools). Round 23: the clip band too shows no title
  // while it holds the piano roll alone, as no ref titles its clip view (038, 041, 112, 198, 276);
  // a second editor docked there (channel rack, Loupe) brings its tabs back.
  if (p == Preset::Live)
    for (const auto& [id, node] : gNodes)
      if (id != "live.side")
        if (ImGuiDockNode* n = ImGui::DockBuilderGetNode(node)) n->SetLocalFlags(n->LocalFlags | ImGuiDockNodeFlags_AutoHideTabBar);
  // K-106: the Tracker's header band and lower frame show no tab bar while they hold one part (the
  // header boxes, the device chain); a second window docked there (the browser as the disk page)
  // brings the tabs back so both stay reachable.
  if (p == Preset::Tracker)
    for (const auto& [id, node] : gNodes)
      if (id == "tracker.header" || id == "tracker.lower")
        if (ImGuiDockNode* n = ImGui::DockBuilderGetNode(node)) n->SetLocalFlags(n->LocalFlags | ImGuiDockNodeFlags_AutoHideTabBar);
  // K-106: the Tracker's centre holds its pages as hidden tabs (the header's page strip picks one);
  // the pattern editor is the page it starts on.
  if (p == Preset::Tracker)
    if (const bool* f = windowOpenFlag(a, trackerFront); f && *f) trackerview::bringPage(trackerFront.c_str(), false);
  gBuiltFor = p;
  gBuiltFrame = ImGui::GetFrameCount();
}

namespace {
void restoreLogicWindows(App& a) {
  if (!gLogicHaveOpen) return;
  for (const WindowFlag& f : windowFlags(a))
    if (std::strcmp(f.window, "Playlist") != 0) *f.open = gLogicOpen.count(f.window) > 0;
}
}  // namespace

void forgetLogicWindows() {
  gLogicOpen.clear();
  gLogicHaveOpen = false;
  gLogicRestoreUntil = -1;
}

void showShellWindows(App& a, Preset p) {
  const layout::Shell& sh = layout::shell(p);
  const layout::State st = regionState(a, p);
  // K-112c (round 45): Clips keeps the main view it has (a new set opens on the clip grid).
  const bool keepView = p == Preset::Live && a.showLivePads != a.showPlaylist;
  const bool pads = a.showLivePads, playlist = a.showPlaylist;
  for (const layout::Item& it : layout::items()) {
    if (!*it.window) continue;
    bool* f = windowOpenFlag(a, it.window);
    if (!f) continue;
    const layout::Home& h = homeIn(it, p);
    const bool homed = (h.kind == HomeKind::Dock || h.kind == HomeKind::Part) && *h.region;
    if (!homed) continue;  // floating windows and dialogs keep their state
    *f = h.openByDefault && layout::visible(sh, st, h.region);
  }
  if (keepView) a.showLivePads = pads, a.showPlaylist = playlist;
  // Exclusive regions open with the member the person last chose.
  for (const layout::Region& r : sh.regions) {
    if (!r.exclusive || !st.member.count(r.id) || !layout::visible(sh, st, r.id)) continue;
    if (bool* f = windowOpenFlag(a, st.member.at(r.id))) *f = true;
  }
  // K-297: back in a layout visited earlier in this run, its own panes as they were left.
  const int k = static_cast<int>(p);
  if (gRestorePanes.count(k) && gKept.count(k)) {
    for (const auto& [w, open] : gKept[k].open)
      if (bool* f = windowOpenFlag(a, w)) *f = open;
    bringFronts(p, gKept[k].fronts, nullptr, false);  // K-297 (round 9): the tabs in front as left
    gRestorePanes.erase(k);
  }
  // R01-04: a build just after a return to the Tracks layout keeps the windows that were open there.
  if (p == Preset::Logic && ImGui::GetFrameCount() <= gLogicRestoreUntil) restoreLogicWindows(a);
}

namespace {
// K-297: the windows that make a layout's pane set: homed in one of its regions, or a new part (an "N"
// row) that floats there. Dialogs and the old floating windows keep their own state across switches.
bool paneOf(const layout::Item& it, Preset p) {
  const layout::Home& h = homeIn(it, p);
  if ((h.kind == HomeKind::Dock || h.kind == HomeKind::Part) && *h.region) return true;
  return it.id[0] == 'N' && (h.kind == HomeKind::Float || h.kind == HomeKind::Part);
}
}  // namespace

void leaveLayout(App& a) {
  // K-297: a region shell's pane set and region state are kept for this run, so coming back to it
  // later opens it as it was left (taken before the windows an exclusive region hid come back).
  const Preset p = active();
  gFrontTabs.clear();  // K-297 (round 9): tabs still waiting to come to the front belong to the layout left
  if (shellBuilt(a, p)) {
    Kept& kp = gKept[static_cast<int>(p)];
    kp.state = regionState(a, p);
    kp.open.clear();
    kp.fronts = frontTabs(a, p);  // K-297 (round 9): a return to the layout shows the same tabs in front
    for (const layout::Item& it : layout::items())
      if (*it.window && paneOf(it, p))
        if (const bool* f = windowOpenFlag(a, it.window)) kp.open[it.window] = *f && !gClosedByGroup.count(it.window);
    // K-297: a region whose windows were all closed (just now, before shellFrame hid it) is kept hidden.
    const layout::Shell& sh = layout::shell(p);
    for (const layout::Region& r : sh.regions) {
      if (!layout::visible(sh, kp.state, r.id)) continue;
      bool parent = false, held = false, homed = false;
      for (const layout::Region& c : sh.regions) parent = parent || std::string(c.parent) == r.id;
      for (const layout::Item& it : layout::items()) {
        const layout::Home& h = homeIn(it, p);
        if (!*it.window || r.id != std::string(h.region) || (h.kind != HomeKind::Dock && h.kind != HomeKind::Part)) continue;
        homed = true;
        const bool* f = windowOpenFlag(a, it.window);
        held = held || !f || *f || gClosedByGroup.count(it.window);
      }
      if (homed && !parent && !held) layout::setVisible(sh, kp.state, r.id, false);
    }
  }
  // The windows an exclusive region only hid come back: they were open when the person left.
  for (const std::string& w : gClosedByGroup)
    if (bool* f = windowOpenFlag(a, w)) *f = true;
  gClosedByGroup.clear();
}

namespace {
// K-297 (round 8): a band sized as a share of the window (Clips' mixer band) keeps the height it
// shows. Another band's divider drag leaves it as it is (the centre takes the change), but the next
// build sized it again from what was left: the mixer band came back 24 px shorter after a restart
// once the device band had been pulled taller. Its height goes in the region state.
bool keepBandSizes(Preset p, layout::State& st) {
  const layout::Shell& sh = layout::shell(p);
  bool changed = false;
  for (const layout::Region& r : sh.regions) {
    if (r.size.px > 0 || r.exclusive || st.regionSize.count(r.id) || !layout::visible(sh, st, r.id)) continue;
    const ImGuiID node = outerNode(r.id);
    const ImGuiDockNode* dn = node ? ImGui::DockBuilderGetNode(node) : nullptr;
    if (!dn || !dn->IsVisible || !dn->ParentNode) continue;
    const bool hz = r.edge == layout::Edge::Left || r.edge == layout::Edge::Right;
    const float now = std::floor(hz ? dn->Size.x : dn->Size.y);
    const float want = layout::regionExtent(r, hz ? dn->ParentNode->Size.x : dn->ParentNode->Size.y, st);
    if (now > 0 && std::fabs(now - want) > 1.5f) st.regionSize[r.id] = now, changed = true;
  }
  return changed;
}

std::string panesKey(Preset p) { return std::string("layout.") + layout::id(p) + ".panes"; }
// The windows kept across a restart: a shell's panes; Studio's docked and floating windows.
bool keptAcrossRuns(const App& a, const layout::Item& it, Preset p) {
  const HomeKind k = homeIn(it, p).kind;
  // K-297 (round 8): a shell's floating windows too (Smooth's groove editor and master chain came back closed).
  // K-297 (round 9): not the dialogs (Settings, About, Project info: modal in Studio): Smooth reopened
  // Settings after a restart, which no other layout does.
  if (shellBuilt(a, p)) return paneOf(it, p) || (k == HomeKind::Float && it.home[0].kind != HomeKind::Modal);
  return k == HomeKind::Dock || k == HomeKind::Float || k == HomeKind::Part;
}
}  // namespace

namespace {
// The tab in front where a region holds several windows (the Tracker's page; K-297 round 8: Clips'
// clip band came back with the piano roll in front of the Loupe left there).
std::vector<std::string> frontTabs(App& a, Preset p) {
  std::vector<std::string> v;
  if (!shellBuilt(a, p) || gBuiltFor != p) return v;
  for (const layout::Item& it : layout::items()) {
    const layout::Home& h = homeIn(it, p);
    if (!*it.window || !*h.region || (h.kind != HomeKind::Dock && h.kind != HomeKind::Part)) continue;
    const ImGuiWindow* w = ImGui::FindWindowByName((std::string("###") + it.window).c_str());
    if (!w) w = ImGui::FindWindowByName(it.window);
    if (w && w->DockIsActive && w->DockNode && w->DockNode->VisibleWindow == w && w->Active && w->DockNode->Windows.Size > 1)
      v.push_back(it.window);
  }
  return v;
}

// Brings those tabs to the front again once their windows dock (the Tracker's pages at once; a
// return to the Tracker from another layout keeps its own rule: the pattern page, with the keys).
void bringFronts(Preset p, const std::vector<std::string>& fronts, const std::set<std::string>* open, bool trackerPages) {
  for (const std::string& front : fronts) {
    const layout::Item* it = layout::findWindow(front);
    if (!it || !*it->window || (open && !open->count(it->window))) continue;
    if (p == Preset::Tracker && std::strcmp(homeIn(*it, p).region, "tracker.pages") == 0) {
      if (!trackerPages) continue;
      trackerview::bringPage(it->window);
      trackerview::setStartPage(it->window);  // K-297 (round 8): the Tracker's first frame on screen shows it, not the pattern page
    } else {
      gFrontTabs[it->window] = 0;  // K-297 (round 8): brought to the front of its region once it docks
    }
  }
}
}  // namespace

void savePanes(App& a) {
  const Preset p = active();
  if (shellBuilt(a, p) && gBuiltFor == p) {  // K-297 (round 8): the bands' heights as left
    layout::State st = regionState(a, p);
    if (keepBandSizes(p, st)) setRegionState(a, p, st);
  }
  std::string s = "1";  // (a version mark: a layout with every window closed still says so)
  for (const layout::Item& it : layout::items())
    if (*it.window && keptAcrossRuns(a, it, p))
      if (const bool* f = windowOpenFlag(a, it.window); f && *f && !gClosedByGroup.count(it.window)) s += std::string("\n") + it.window;
  for (const std::string& w : frontTabs(a, p)) s += std::string("\nfront:") + w;
  a.settings.set(panesKey(p), s);
}

bool restorePanes(App& a, Preset p) {
  const std::string s = a.settings.getString(panesKey(p));
  if (s.empty() || s[0] != '1') return false;
  std::set<std::string> open;
  std::vector<std::string> fronts;
  for (size_t i = 0, j; i < s.size(); i = j + 1) {
    j = s.find('\n', i);
    if (j == std::string::npos) j = s.size();
    const std::string w = s.substr(i, j - i);
    if (!w.compare(0, 6, "front:")) fronts.push_back(w.substr(6));
    else open.insert(w);
  }
  for (const layout::Item& it : layout::items())
    if (*it.window && keptAcrossRuns(a, it, p))
      if (bool* f = windowOpenFlag(a, it.window)) *f = open.count(it.window) > 0;
  bringFronts(p, fronts, &open, true);
  return true;
}

void enterLayout(App& a, Preset to) {
  if (to == Preset::Studio) return;
  const int k = static_cast<int>(to);
  // K-284: a layout's first visit in a run opens its designed regions; K-297: a later visit opens them
  // as they were left (its panes come back in showShellWindows).
  const auto kept = gKept.find(k);
  layout::State designed;
  if (kept == gKept.end()) {  // K-237: the person's tab order, pins and moved tabs are theirs, not the arrangement's
    const layout::State before = regionState(a, to);
    designed.tabs = before.tabs;
    designed.moved = before.moved;
  }
  setRegionState(a, to, kept != gKept.end() ? kept->second.state : designed);
  if (kept != gKept.end()) gRestorePanes.insert(k);
  if (!shellBuilt(a, to)) return;
  // K-297: another layout's new parts do not follow into this one as stray floating windows (the
  // Tracker's order list over the tracks): a part that only floats here closes unless it opens here by default.
  for (const layout::Item& it : layout::items()) {
    if (!*it.window || it.id[0] != 'N') continue;
    const layout::Home& h = it.home[k];
    if ((h.kind == HomeKind::Float || (h.kind == HomeKind::Part && !*h.region)) && !h.openByDefault)
      if (bool* f = windowOpenFlag(a, it.window)) *f = false;
  }
}

namespace {
struct Boost {
  float px = 0;
  int frame = -10;
  bool exact = false;  // setBandSize: the band is px tall (wide), not px more
  float maxShare = 0.6f;  // the most of its parent the band may take
};
std::map<std::string, Boost>& boosts() {
  static std::map<std::string, Boost> m;
  return m;
}
}  // namespace

void setBandBoost(const std::string& region, float px) { boosts()[region] = Boost{std::max(0.0f, px), ImGui::GetFrameCount(), false}; }
void setBandSize(const std::string& region, float px, float maxShare) {
  boosts()[region] = Boost{std::max(1.0f, px), ImGui::GetFrameCount(), true, std::clamp(maxShare, 0.1f, 0.95f)};
}

namespace {

// A window's name in a region's title strip, and whether the strip always lists it (the others
// show while they are open).
struct StripName {
  const char* window;
  const char* label;
  bool always;
};
const StripName kStripNames[] = {
    {"Browser", i18n::N_("Browser"), true},      {"Lists", i18n::N_("Lists"), true},          {"Notes", i18n::N_("Notes"), true},
    {"Files", i18n::N_("Files"), true},          {"###mcpactivity", i18n::N_("Assistant"), false},
    {"Collaboration", i18n::N_("Collaborate"), false}, {"Sync review", i18n::N_("Review"), false},
    {"Piano roll", i18n::N_("Piano roll"), true}, {"Channel rack", i18n::N_("Steps"), true}, {"Loupe", i18n::N_("Loupe"), true},
    {"###autoedit", i18n::N_("Automation"), true}, {"Chord progression", i18n::N_("Chords"), false}, {"Mixer", i18n::N_("Mixer"), true},
    {"Smart controls", i18n::N_("Smart controls"), false}, {"Pattern editor", i18n::N_("Pattern"), false},
};

// K-237: tabs the person closed in a pane (this run; their window can come back from the menu, the
// reopen key or its own key), the closed-tab history, and the tab whose menu is open.
std::set<std::string>& tabClosed() {
  static std::set<std::string> s;
  return s;
}
panetabs::History& tabHistory() {
  static panetabs::History h;
  return h;
}
std::string gTabMenu;

using TabCands = std::map<std::string, std::pair<const StripName*, bool*>>;

// R04-13: in Smooth the audio editor (Loupe pinned into the lower pane) has no window flag: its tab
// stands on this one, lit while it holds the pane, so a tab is always lit for the editor shown.
bool gAudioOn = false;
const StripName kAudioSeg = {"Loupe", i18n::N_("Audio"), true};

// Shows a pane tab's window (the region's one-at-a-time rule closes the one shown).
void showPaneTab(App& a, bool* f) {
  if (f == &gAudioOn) {  // as the editors' key shows the audio editor
    if (bool* sm = windowOpenFlag(a, "Smart controls")) *sm = false;
    a.settings.set("ui.loupePinned", false);
    toggleTracksAudioEditor(a);  // (the selected audio region's sound, else the focused track's)
    return;
  }
  *f = true;
  if (gAudioOn) {  // the audio editor gives the pane back (it has no flag for that rule)
    a.settings.set("ui.loupePinned", false);
    packs::closeAudioEditorWindows(a);
    gAudioOn = false;
  }
}

// Closes a pane tab: refused for a pinned tab or the last one. When it was the one shown, its
// right-hand neighbour shows instead.
void closeTab(App& a, const std::string& region, panetabs::Strip& strip, const std::string& m, TabCands& cand) {
  const bool wasOn = m == strip.active;
  if (!panetabs::close(strip, m, region, tabHistory())) return;
  tabClosed().insert(m);
  if (wasOn) {
    if (cand.count(m) && cand[m].second != &gAudioOn) *cand[m].second = false;
    else if (cand.count(m)) {
      a.settings.set("ui.loupePinned", false);
      packs::closeAudioEditorWindows(a);
      gAudioOn = false;
    }
    if (cand.count(strip.active)) showPaneTab(a, cand[strip.active].second);
  }
}

// A pane's tabs: the windows its strip lists (closed tabs left out), in the person's order, the
// window shown there active.
panetabs::Strip paneStrip(App& a, Preset p, const std::string& region, const layout::State& st, TabCands& cand) {
  std::vector<std::string> members;
  // (the mixer's flag can stay set behind the audio editor: the audio editor is what the pane shows then)
  gAudioOn = p == Preset::Logic && a.settings.getBool("ui.loupePinned") && !a.showPiano && !a.showRack;
  for (const StripName& n : kStripNames) {
    if (region != windowHomeIn(n.window, p).region) continue;
    const bool audioSeg = p == Preset::Logic && std::string_view(n.window) == "Loupe";
    bool* f = audioSeg ? &gAudioOn : windowOpenFlag(a, n.window);
    if (!f || (!n.always && !*f)) continue;
    if (*f) tabClosed().erase(n.window), tabHistory().forget(n.window);  // opened another way: its tab is back
    if (tabClosed().count(n.window)) continue;
    cand[n.window] = {audioSeg ? &kAudioSeg : &n, f};
    members.push_back(n.window);
  }
  panetabs::Strip strip = panetabs::load(st.tabs.count(region) ? st.tabs.at(region) : std::string());
  panetabs::sync(strip, members);
  for (const std::string& m : strip.order)
    if (cand[m].second == &gAudioOn ? gAudioOn : *cand[m].second && !gAudioOn) strip.active = m;
  return strip;
}

bool hasClosedTab(const std::string& region) {
  for (const panetabs::Closed& c : tabHistory().items())
    if (region.empty() || c.region == region) return true;
  return false;
}

const char* reopenKey() {
  const char* l = keys::label("view.reopenTab");
  return l && *l ? l : nullptr;
}

void toggleMaximised(App& a, const std::string& region) {
  const Preset p = active();
  layout::State st = regionState(a, p);
  if (!st.maximised.erase(region)) st.maximised.insert(region);
  setRegionState(a, p, st);
  gRebuild = true;
}

// ---- K-237: tabs between panes, and the vertical rail ----

// A pane's name in menus ("Move to ...").
const char* paneName(const std::string& region) {
  if (region == "logic.right") return i18n::tr("Right pane");
  if (region == "logic.lower") return i18n::tr("Lower pane");
  if (region == "live.side" || region == "tracker.side") return i18n::tr("Side panel");
  return region.c_str();
}

// The panes of the active layout a window's tab may go to (not the one it is in).
std::vector<std::string> moveTargets(App& a, Preset p, const std::string& window) {
  std::vector<std::string> v;
  if (p == Preset::Logic && window == "Loupe") return v;  // the audio editor's tab stands for the pinned Loupe (no window flag)
  const std::string at = layout::tabRegion(p, regionState(a, p), window);
  for (const layout::Region& r : layout::shell(p).regions)
    if (r.id != at && layout::canMoveTab(p, window, r.id)) v.push_back(r.id);
  return v;
}

// Moves a window's tab to pane `to`: the pane shows (with the window in front), the pane it left
// shows its next tab when it keeps one window at a time, and the dock tree is rebuilt next frame
// with the window in its new place. Kept per layout in the region state (State::moved).
// The window a move into a one-at-a-time pane displaced there, which shows again when the moved tab
// leaves (pane -> window).
std::map<std::string, std::string> gDisplaced;

bool moveTabTo(App& a, const std::string& window, const std::string& to) {
  const Preset p = active();
  const std::vector<std::string> targets = moveTargets(a, p, window);
  if (std::find(targets.begin(), targets.end(), to) == targets.end()) return false;
  layout::State st = regionState(a, p);
  const std::string from = layout::tabRegion(p, st, window);
  // The tab goes to the end of the pane it joins (after the tabs it shows now, in their order).
  TabCands before;
  panetabs::Strip into = paneStrip(a, p, to, st, before);
  if (!layout::moveTab(p, st, window, to)) return false;
  into.order.erase(std::remove(into.order.begin(), into.order.end(), window), into.order.end());
  into.order.push_back(window);
  into.active = window;
  st.tabs[to] = panetabs::save(into);
  setRegionState(a, p, st);
  tabClosed().erase(window);
  tabHistory().forget(window);
  bool* f = windowOpenFlag(a, window);
  const bool wasOpen = f && *f;
  // The pane it left keeps showing something: the window the tab displaced when it came, else the
  // strip's own pick.
  if (const layout::Region* r = layout::findRegion(p, from); wasOpen && r && r->exclusive && gStrip.count(from)) {
    TabCands cand;
    panetabs::Strip left = paneStrip(a, p, from, st, cand);
    if (const auto d = gDisplaced.find(from); d != gDisplaced.end()) {
      if (cand.count(d->second)) left.active = d->second;
      gDisplaced.erase(d);
    }
    if (cand.count(left.active) && cand[left.active].second && !*cand[left.active].second) showPaneTab(a, cand[left.active].second);
  }
  // A one-at-a-time pane shows the moved window alone (its flag was already set, so the pane's rule
  // would not see it open); the one it displaces comes back when the tab leaves.
  if (const layout::Region* r = layout::findRegion(p, to); r && r->exclusive) {
    for (const std::string& m : into.order) {
      if (m == window || !before.count(m) || !before[m].second) continue;
      bool* g = before[m].second;
      if (g == &gAudioOn ? gAudioOn : *g) {
        if (!gDisplaced.count(to)) gDisplaced[to] = m;
        if (g != &gAudioOn) *g = false;
      }
    }
  }
  setRegionVisible(a, to, true);
  if (f) showPaneTab(a, f);
  gFrontTabs[window] = 0;
  gRebuild = true;  // (redocked where it lives now)
  return true;
}

// A drop of a pane tab waits for the end of the frame's strips (they read the state as they draw).
struct PendingMove {
  std::string window, to;
};
std::vector<PendingMove> gPendingMoves;
// Accepts a dragged pane tab ("<region>|<window>"): another pane's moves here, this pane's goes to
// position `at` of `strip` (the end when -1). True when the strip's order changed.
bool acceptPaneTab(App& a, const std::string& region, panetabs::Strip& strip, int at) {
  const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GL_PANETAB");
  if (!pl) return false;
  const std::string txt(static_cast<const char*>(pl->Data));
  const size_t bar = txt.find('|');
  if (bar == std::string::npos) return false;
  const std::string from = txt.substr(0, bar), w = txt.substr(bar + 1);
  if (from == region) return panetabs::move(strip, w, at < 0 ? static_cast<int>(strip.order.size()) : at);
  const auto t = moveTargets(a, active(), w);
  if (std::find(t.begin(), t.end(), region) != t.end()) gPendingMoves.push_back({w, region});
  return false;
}
void runPendingMoves(App& a) {
  std::vector<PendingMove> v;
  v.swap(gPendingMoves);
  for (const PendingMove& m : v) moveTabTo(a, m.window, m.to);
}

// Dev (tests): the tab a scripted drag starts from and the pane it ends on ("layout tabtarget").
std::string gDevTabSrc, gDevTabDst;

// A window's label on the rail: its strip name, else its title as it shows (without the "##" id).
std::string railLabel(const std::string& window) {
  for (const StripName& n : kStripNames)
    if (window == n.window) return i18n::tr(n.label);
  std::string t = window;
  if (const ImGuiWindow* w = ImGui::FindWindowByName((std::string("###") + window).c_str())) t = w->Name;
  else if (const ImGuiWindow* w2 = ImGui::FindWindowByName(window.c_str())) t = w2->Name;
  if (const size_t h = t.find("##"); h != std::string::npos) t = t.substr(0, h);
  while (!t.empty() && t.back() == ' ') t.pop_back();
  if (t.empty())
    if (const layout::Item* it = layout::findWindow(window)) t = it->name;
  return t;
}

// A Rail region's tabs: its open windows (closed tabs left out), in the person's order, the window in
// front of its body active. `cand` gets each one's open flag (no strip name).
panetabs::Strip railStrip(App& a, Preset p, const std::string& region, const layout::State& st, TabCands& cand) {
  std::vector<std::string> members;
  for (const layout::Item& it : layout::items()) {
    if (!*it.window || region != homeIn(it, p).region) continue;
    const layout::Home& h = homeIn(it, p);
    if (h.kind != HomeKind::Dock && h.kind != HomeKind::Part) continue;
    bool* f = windowOpenFlag(a, it.window);
    if (!f || !*f) continue;
    tabClosed().erase(it.window), tabHistory().forget(it.window);
    cand[it.window] = {nullptr, f};
    members.push_back(it.window);
  }
  panetabs::Strip strip = panetabs::load(st.tabs.count(region) ? st.tabs.at(region) : std::string());
  panetabs::sync(strip, members);
  if (const ImGuiDockNode* n = ImGui::DockBuilderGetNode(regionNode(region)); n && n->VisibleWindow)
    if (const std::string front = ui::stableWindowName(n->VisibleWindow->Name); cand.count(front)) strip.active = front;
  return strip;
}

// Text drawn turned a quarter clockwise (reading top to bottom), centred on `c`.
void addTextDown(ImDrawList* dl, ImFont* font, float fs, ImVec2 c, ImU32 col, const char* text) {
  const ImVec2 sz = font->CalcTextSizeA(fs, 1e9f, 0, text);
  const int first = dl->VtxBuffer.Size;
  // (drawn level first: the clip rect then covers the level text, else glyphs past the rail's width are
  // dropped before the turn; the turned text stays inside its tab)
  const float r = std::max(sz.x, sz.y) * 0.5f + 2;
  dl->PushClipRect(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), false);
  dl->AddText(font, fs, ImVec2(std::round(c.x - sz.x * 0.5f), std::round(c.y - sz.y * 0.5f)), col, text);
  dl->PopClipRect();
  for (int i = first; i < dl->VtxBuffer.Size; ++i) {
    ImDrawVert& v = dl->VtxBuffer[i];
    const float dx = v.pos.x - c.x, dy = v.pos.y - c.y;
    v.pos = ImVec2(c.x - dy, c.y + dx);
  }
}

// The vertical tab rail of each Rail region (the side panels of Clips and the Tracker): one tab per
// open window, the one in front lit with a bar on its inner edge; click brings a window to the front,
// middle-click closes it, right-click for its menu, drag to reorder or onto another pane's tabs.
void drawPaneRails(App& a) {
  const Preset p = active();
  const auto& P = theme::pal();
  for (const auto& [region, railNode] : gRail) {
    if (!ImGui::DockBuilderGetNode(railNode)) continue;
    ImGui::SetNextWindowDockID(railNode, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1, 1));
    const bool vis = ImGui::Begin(stripWindowName(region).c_str(), nullptr,
                                  ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav |
                                      ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::PopStyleVar(2);
    if (vis) {
      const layout::Region* reg = layout::findRegion(p, region);
      const bool onLeft = reg && reg->edge == layout::Edge::Left;
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
      dl->AddRectFilled(wp, ImVec2(wp.x + ws.x, wp.y + ws.y), P.surface800);
      const float lineX = onLeft ? wp.x + ws.x - 0.5f : wp.x + 0.5f;  // the line toward the pane's body
      dl->AddLine(ImVec2(lineX, wp.y), ImVec2(lineX, wp.y + ws.y), P.line);
      const float fs = ImGui::GetFontSize();
      TabCands cand;
      const layout::State st = regionState(a, p);
      panetabs::Strip strip = railStrip(a, p, region, st, cand);
      const std::string savedText = st.tabs.count(region) ? st.tabs.at(region) : std::string();
      std::vector<std::string> labels;
      std::vector<float> heights;
      for (const std::string& m : strip.order) {
        labels.push_back(railLabel(m));
        heights.push_back(std::ceil(a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, labels.back().c_str()).x) + 18);
      }
      const float overflowH = 26;
      const std::vector<int> shownIdx = panetabs::fit(strip, heights, ws.y - theme::kS2 * 2, overflowH);
      const float x0 = wp.x + 2, x1 = wp.x + ws.x - 2;
      float y = wp.y + theme::kS2;
      bool changed = false;
      std::string menuFor;
      for (int i : shownIdx) {
        const std::string& m = strip.order[static_cast<size_t>(i)];
        const float hgt = heights[static_cast<size_t>(i)];
        ImGui::SetCursorScreenPos(ImVec2(x0, y));
        ImGui::PushID(m.c_str());
        const bool click = ImGui::InvisibleButton("##r", ImVec2(x1 - x0, hgt), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
        const bool hov = ImGui::IsItemHovered();
        const bool middle = click && ImGui::IsMouseReleased(ImGuiMouseButton_Middle);
        if (m == gDevTabSrc) devNoteRect("ptabsrc");
        if ("tab:" + m == gDevTabDst) devNoteRect("ptabdst");  // (a drop on this tab: a reorder)
        if (hov) ImGui::SetTooltip("%s", labels[static_cast<size_t>(i)].c_str());
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) menuFor = m;
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
          const std::string payload = region + "|" + m;
          ImGui::SetDragDropPayload("GL_PANETAB", payload.c_str(), payload.size() + 1);
          ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
          changed = acceptPaneTab(a, region, strip, i) || changed;
          ImGui::EndDragDropTarget();
        }
        ImGui::PopID();
        const bool on = m == strip.active;
        if (on) dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + hgt), P.surface600, 4.0f);
        else if (hov) dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + hgt), theme::withAlpha(P.surface600, 0.5f), 4.0f);
        if (on) {  // the tab in front: a bar on the edge toward the body
          const float bx = onLeft ? x1 - 1.5f : x0 + 1.5f;
          dl->AddLine(ImVec2(bx, y + 3), ImVec2(bx, y + hgt - 3), P.accent, 2.0f);
        }
        if (strip.pinned.count(m)) dl->AddCircle(ImVec2((x0 + x1) * 0.5f, y + 7), 2.5f, P.accent, 8, 1.5f);  // pinned: a small ring
        addTextDown(dl, a.fontRegular, fs, ImVec2((x0 + x1) * 0.5f, y + hgt * 0.5f + (strip.pinned.count(m) ? 3.0f : 0.0f)), on || hov ? P.text : P.textDim,
                    labels[static_cast<size_t>(i)].c_str());
        if (middle) {
          const bool wasOn = on;
          if (panetabs::close(strip, m, region, tabHistory())) {
            tabClosed().insert(m);
            *cand[m].second = false;
            if (wasOn && !strip.active.empty()) gFrontTabs[strip.active] = 0;
            changed = true;
          }
        } else if (click && !on) {
          gFrontTabs[m] = 0;  // brought to the front of the pane
          strip.active = m;
        }
        y += hgt + 2;
      }
      if (shownIdx.size() < strip.order.size()) {  // the tabs that do not fit, in a menu
        ImGui::SetCursorScreenPos(ImVec2(x0, y));
        if (ImGui::InvisibleButton("##more", ImVec2(x1 - x0, overflowH))) ImGui::OpenPopup("##railmore");
        char more[16];
        std::snprintf(more, sizeof more, "+%d", static_cast<int>(strip.order.size() - shownIdx.size()));
        const float tw = a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, more).x;
        dl->AddText(a.fontRegular, fs, ImVec2(std::round((x0 + x1 - tw) * 0.5f), std::round(y + (overflowH - fs) * 0.5f)),
                    ImGui::IsItemHovered() ? P.text : P.textDim, more);
        if (ImGui::BeginPopup("##railmore")) {
          for (size_t i = 0; i < strip.order.size(); ++i) {
            if (std::find(shownIdx.begin(), shownIdx.end(), static_cast<int>(i)) != shownIdx.end()) continue;
            if (ImGui::MenuItem(railLabel(strip.order[i]).c_str())) gFrontTabs[strip.order[i]] = 0;
          }
          ImGui::EndPopup();
        }
      }
      // The whole rail takes a tab dropped below its tabs (another pane's moves here, at the end).
      if (region == gDevTabDst) devNoteRectAt("ptabdst", wp.x, std::min(y + 4, wp.y + ws.y - 8), wp.x + ws.x, wp.y + ws.y);
      if (ImGui::BeginDragDropTargetCustom(ImRect(wp, ImVec2(wp.x + ws.x, wp.y + ws.y)), ImGui::GetID("##raildrop"))) {
        changed = acceptPaneTab(a, region, strip, -1) || changed;
        ImGui::EndDragDropTarget();
      }
      if (!menuFor.empty()) {
        gTabMenu = menuFor;
        ImGui::OpenPopup("##railtab");
      }
      if (ImGui::BeginPopup("##railtab")) {
        const std::string m = gTabMenu;
        if (cand.count(m)) {
          const bool pinned = strip.pinned.count(m) > 0;
          const auto it = std::find(strip.order.begin(), strip.order.end(), m);
          const int at = static_cast<int>(it - strip.order.begin());
          if (ImGui::MenuItem(pinned ? i18n::tr("Unpin tab") : i18n::tr("Pin tab"))) changed = panetabs::setPinned(strip, m, !pinned) || changed;
          if (ImGui::MenuItem(i18n::tr("Move up"), nullptr, false, at > 0)) changed = panetabs::move(strip, m, at - 1) || changed;
          if (ImGui::MenuItem(i18n::tr("Move down"), nullptr, false, at + 1 < static_cast<int>(strip.order.size())))
            changed = panetabs::move(strip, m, at + 2) || changed;
          for (const std::string& to : moveTargets(a, p, m)) {
            char label[96];
            std::snprintf(label, sizeof label, i18n::tr("Move to %s"), paneName(to));
            if (ImGui::MenuItem(label)) gPendingMoves.push_back({m, to});
          }
          if (ImGui::MenuItem(i18n::tr("Close tab"), nullptr, false, !pinned && strip.order.size() > 1)) {
            if (panetabs::close(strip, m, region, tabHistory())) tabClosed().insert(m), *cand[m].second = false, changed = true;
          }
        }
        ImGui::Separator();
        if (ImGui::MenuItem(i18n::tr("Reopen closed tab"), reopenKey(), false, hasClosedTab(region))) reopenClosedTab(a, region);
        if (ImGui::MenuItem(i18n::tr("Close pane"))) setRegionVisible(a, region, false);
        ImGui::EndPopup();
      }
      if (changed) {
        const std::string text = panetabs::save(strip);
        if (text != savedText) {
          layout::State now = regionState(a, p);
          now.tabs[region] = text;
          setRegionState(a, p, now);
        }
      }
    }
    ImGui::End();
  }
}

// The title strip of each Segmented region: its windows by name, the one shown lit; a click shows
// another (the region keeps one at a time).
void drawPaneStrips(App& a) {
  const Preset p = active();
  const auto& P = theme::pal();
  for (const auto& [region, strip] : gStrip) {
    if (!ImGui::DockBuilderGetNode(strip)) continue;
    ImGui::SetNextWindowDockID(strip, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1, 1));  // the strip is shorter than a window's minimum
    const bool vis = ImGui::Begin(stripWindowName(region).c_str(), nullptr,
                                  ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav |
                                      ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::PopStyleVar(2);
    if (vis) {
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
      dl->AddRectFilled(wp, ImVec2(wp.x + ws.x, wp.y + ws.y), P.surface800);
      dl->AddLine(ImVec2(wp.x, wp.y + ws.y - 0.5f), ImVec2(wp.x + ws.x, wp.y + ws.y - 0.5f), P.line);
      float x = wp.x + theme::kS3;
      const float fs = ImGui::GetFontSize();
      if (p == Preset::Logic) {  // K-112b: a centred segmented control, the shown window's segment raised
        // K-237: the segments are the pane's tab strip (core/PaneTabs.h): the person's order and pins,
        // closed tabs (reopened from the menu or with their key), and an overflow menu for tabs that
        // do not fit. With nothing changed it draws exactly as before.
        TabCands cand;
        layout::State st = regionState(a, p);
        panetabs::Strip strip = paneStrip(a, p, region, st, cand);
        const std::string savedText = st.tabs.count(region) ? st.tabs.at(region) : std::string();
        std::vector<float> widths;
        for (const std::string& m : strip.order)
          widths.push_back(std::ceil(a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, i18n::tr(cand[m].first->label)).x) + 22);
        // K-112D (audit F10, I6): the mixer is not an editor: it sits apart from the editors' segments,
        // in a group of its own; the shown segment is filled in the accent with white text (144, 125).
        // The right-hand pane (H5) shows only its window's name: the control bar opens each of them.
        const bool smoothLook = smoothFaces();
        const bool titleOnly = smoothLook && region == "logic.right";
        const float overflowW = 30, mixGap = 14;
        const bool hasMix = strip.order.size() > 1 && std::find(strip.order.begin() + 1, strip.order.end(), "Mixer") != strip.order.end();
        std::vector<int> shownIdx = panetabs::fit(strip, widths, ws.x - theme::kS3 * 2 - (hasMix ? mixGap : 0), overflowW);
        const float h = std::min(22.0f, ws.y - 6), y0 = std::round(wp.y + (ws.y - 1 - h) * 0.5f);
        if (titleOnly) {
          if (cand.count(strip.active)) {
            const char* label = i18n::tr(cand[strip.active].first->label);
            const float tw = a.fontBold->CalcTextSizeA(fs, 1e9f, 0, label).x;
            const ImVec2 t0(std::round(wp.x + (ws.x - tw) * 0.5f), std::round(y0 + (h - fs) * 0.5f));
            dl->AddText(a.fontBold, fs, t0, P.text, label);
            // K-237: the title is the shown window's tab: it drags to another pane.
            ImGui::SetCursorScreenPos(ImVec2(t0.x - 6, y0));
            ImGui::InvisibleButton("##title", ImVec2(tw + 12, h));
            if (strip.active == gDevTabSrc) devNoteRect("ptabsrc");
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
              const std::string payload = region + "|" + strip.active;
              ImGui::SetDragDropPayload("GL_PANETAB", payload.c_str(), payload.size() + 1);
              ImGui::EndDragDropSource();
            }
          }
          shownIdx.clear();
        }
        float total = 0;
        int mixAt = -1;  // position in shownIdx of the mixer's segment, when it is not the first
        for (size_t k = 0; k < shownIdx.size(); ++k) {
          total += widths[static_cast<size_t>(shownIdx[k])];
          if (k > 0 && strip.order[static_cast<size_t>(shownIdx[k])] == "Mixer") mixAt = static_cast<int>(k);
        }
        const float gap = mixAt > 0 ? mixGap : 0.0f;
        const bool overflow = !titleOnly && shownIdx.size() < strip.order.size();
        float sx = std::round(wp.x + (ws.x - total - gap - (overflow ? overflowW : 0)) * 0.5f);
        if (!shownIdx.empty()) {
          float split = sx + total;
          if (mixAt > 0) {
            split = sx;
            for (int k = 0; k < mixAt; ++k) split += widths[static_cast<size_t>(shownIdx[static_cast<size_t>(k)])];
          }
          dl->AddRectFilled(ImVec2(sx - 2, y0 - 2), ImVec2(split + 2, y0 + h + 2), P.surface900, 7.0f);
          if (mixAt > 0) dl->AddRectFilled(ImVec2(split + gap - 2, y0 - 2), ImVec2(sx + total + gap + 2, y0 + h + 2), P.surface900, 7.0f);
        }
        bool changed = false;
        std::string menuFor;
        for (size_t k = 0; k < shownIdx.size(); ++k) {
          const int i = shownIdx[k];
          if (static_cast<int>(k) == mixAt) sx += gap;
          const std::string& m = strip.order[static_cast<size_t>(i)];
          const StripName* n = cand[m].first;
          bool* f = cand[m].second;
          const float w = widths[static_cast<size_t>(i)];
          ImGui::SetCursorScreenPos(ImVec2(sx, y0));
          ImGui::PushID(n->window);
          const bool click = ImGui::InvisibleButton("##t", ImVec2(w, h), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
          const bool hov = ImGui::IsItemHovered();
          const bool middle = click && ImGui::IsMouseReleased(ImGuiMouseButton_Middle);
          if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) menuFor = m;
          if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {  // reorder along the strip
            const std::string payload = region + "|" + m;
            ImGui::SetDragDropPayload("GL_PANETAB", payload.c_str(), payload.size() + 1);
            ImGui::EndDragDropSource();
          }
          // Reordered along the strip, or a tab from another pane moves here (the main area never takes one).
          if (ImGui::BeginDragDropTarget()) {
            changed = acceptPaneTab(a, region, strip, i) || changed;
            ImGui::EndDragDropTarget();
          }
          if (m == gDevTabSrc) devNoteRect("ptabsrc");
          if ("tab:" + m == gDevTabDst) devNoteRect("ptabdst");
          ImGui::PopID();
          const bool on = m == strip.active;
          if (on) dl->AddRectFilled(ImVec2(sx, y0), ImVec2(sx + w, y0 + h), smoothLook ? P.accent : P.surface600, 5.0f);
          else if (hov) dl->AddRectFilled(ImVec2(sx, y0), ImVec2(sx + w, y0 + h), theme::withAlpha(P.surface600, 0.5f), 5.0f);
          if (strip.pinned.count(m)) dl->AddCircle(ImVec2(sx + 7, y0 + h * 0.5f), 2.5f, on && smoothLook ? theme::contrastText(P.accent) : P.accent, 8, 1.5f);  // pinned: a small ring
          const char* label = i18n::tr(n->label);
          const float tw = a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, label).x;
          const ImU32 tc = on && smoothLook ? theme::contrastText(P.accent) : on || hov ? P.text : P.textDim;
          dl->AddText(a.fontRegular, fs, ImVec2(std::round(sx + (w - tw) * 0.5f), std::round(y0 + (h - fs) * 0.5f)), tc, label);
          if (middle) closeTab(a, region, strip, m, cand), changed = true;
          else if (click && !on) showPaneTab(a, f);
          sx += w;
        }
        if (overflow) {  // the tabs that do not fit, in a menu
          ImGui::SetCursorScreenPos(ImVec2(sx + 4, y0));
          if (ImGui::InvisibleButton("##more", ImVec2(overflowW - 4, h))) ImGui::OpenPopup("##panemore");
          const bool hov = ImGui::IsItemHovered();
          char more[16];
          std::snprintf(more, sizeof more, "+%d", static_cast<int>(strip.order.size() - shownIdx.size()));
          const float tw = a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, more).x;
          dl->AddText(a.fontRegular, fs, ImVec2(std::round(sx + 4 + (overflowW - 4 - tw) * 0.5f), std::round(y0 + (h - fs) * 0.5f)), hov ? P.text : P.textDim, more);
          if (ImGui::BeginPopup("##panemore")) {
            for (size_t i = 0; i < strip.order.size(); ++i) {
              if (std::find(shownIdx.begin(), shownIdx.end(), static_cast<int>(i)) != shownIdx.end()) continue;
              const std::string& m = strip.order[i];
              if (ImGui::MenuItem(i18n::tr(cand[m].first->label), nullptr, *cand[m].second)) showPaneTab(a, cand[m].second);
            }
            ImGui::EndPopup();
          }
          sx += overflowW;
        }
        // Collapse and maximise, at the strip's right end while the pointer is over the strip.
        const bool stripHov = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        if (stripHov || st.maximised.count(region)) {
          const float bw = 18, bx0 = wp.x + ws.x - theme::kS1 - bw * 2, by = std::round(wp.y + (ws.y - 1 - bw) * 0.5f);
          const bool maxOn = st.maximised.count(region) > 0;
          for (int b = 0; b < 2; ++b) {
            const ImVec2 b0(bx0 + b * bw, by), b1(b0.x + bw, by + bw);
            ImGui::SetCursorScreenPos(b0);
            ImGui::PushID(b);
            const bool click = ImGui::InvisibleButton("##pb", ImVec2(bw, bw));
            const bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            if (hov) ImGui::SetTooltip("%s", b == 0 ? (maxOn ? i18n::tr("Restore pane size") : i18n::tr("Maximise pane")) : i18n::tr("Close pane"));
            const ImU32 c = hov ? P.text : P.textDim;
            const ImVec2 m((b0.x + b1.x) * 0.5f, (b0.y + b1.y) * 0.5f);
            if (b == 0) {
              if (maxOn) dl->AddRect(ImVec2(m.x - 3, m.y - 3), ImVec2(m.x + 3, m.y + 3), c, 0, 0, 1.2f);
              else dl->AddRect(ImVec2(m.x - 5, m.y - 4), ImVec2(m.x + 5, m.y + 4), c, 0, 0, 1.2f);
            } else {
              dl->AddLine(ImVec2(m.x - 4, m.y - 4), ImVec2(m.x + 4, m.y + 4), c, 1.2f);
              dl->AddLine(ImVec2(m.x - 4, m.y + 4), ImVec2(m.x + 4, m.y - 4), c, 1.2f);
            }
            if (click) (b == 0 ? toggleMaximised(a, region) : setRegionVisible(a, region, false));
          }
        }
        if (stripHov && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered()) toggleMaximised(a, region);
        // K-237: the whole strip takes a tab dragged from another pane (it joins at the end).
        if (region == gDevTabDst) devNoteRectAt("ptabdst", wp.x + 4, wp.y + 2, wp.x + 20, wp.y + ws.y - 2);
        if (ImGui::BeginDragDropTargetCustom(ImRect(wp, ImVec2(wp.x + ws.x, wp.y + ws.y)), ImGui::GetID("##stripdrop"))) {
          changed = acceptPaneTab(a, region, strip, -1) || changed;
          ImGui::EndDragDropTarget();
        }
        if (!menuFor.empty()) {
          gTabMenu = menuFor;
          ImGui::OpenPopup("##panetab");
        }
        if (ImGui::BeginPopup("##panetab")) {
          const std::string& m = gTabMenu;
          if (cand.count(m)) {
            const bool pinned = strip.pinned.count(m) > 0;
            const auto it = std::find(strip.order.begin(), strip.order.end(), m);
            const int at = static_cast<int>(it - strip.order.begin());
            if (ImGui::MenuItem(pinned ? i18n::tr("Unpin tab") : i18n::tr("Pin tab"))) changed = panetabs::setPinned(strip, m, !pinned) || changed;
            if (ImGui::MenuItem(i18n::tr("Move left"), nullptr, false, at > 0)) changed = panetabs::move(strip, m, at - 1) || changed;
            if (ImGui::MenuItem(i18n::tr("Move right"), nullptr, false, at + 1 < static_cast<int>(strip.order.size())))
              changed = panetabs::move(strip, m, at + 2) || changed;
            for (const std::string& to : moveTargets(a, p, m)) {  // K-237: to another pane
              char label[96];
              std::snprintf(label, sizeof label, i18n::tr("Move to %s"), paneName(to));
              if (ImGui::MenuItem(label)) gPendingMoves.push_back({m, to});
            }
            if (ImGui::MenuItem(i18n::tr("Close tab"), nullptr, false, !pinned && strip.order.size() > 1)) closeTab(a, region, strip, m, cand), changed = true;
          }
          ImGui::Separator();
          if (ImGui::MenuItem(i18n::tr("Reopen closed tab"), reopenKey(), false, hasClosedTab(region)))
            reopenClosedTab(a, region);
          if (ImGui::MenuItem(st.maximised.count(region) ? i18n::tr("Restore pane size") : i18n::tr("Maximise pane"))) toggleMaximised(a, region);
          if (ImGui::MenuItem(i18n::tr("Close pane"))) setRegionVisible(a, region, false);
          ImGui::EndPopup();
        }
        if (changed) {
          const std::string text = panetabs::save(strip);
          if (text != savedText) {
            layout::State now = regionState(a, p);
            now.tabs[region] = text;
            setRegionState(a, p, now);
          }
        }
        x = sx;
      }
      for (const StripName& n : kStripNames) {
        if (p == Preset::Logic) break;
        const layout::Home h = windowHomeIn(n.window, p);
        if (region != h.region) continue;
        bool* f = windowOpenFlag(a, n.window);
        if (!f || (!n.always && !*f)) continue;
        const char* label = i18n::tr(n.label);
        ImFont* font = *f ? a.fontBold : a.fontRegular;
        const float tw = font->CalcTextSizeA(fs, 1e9f, 0, label).x;
        if (x + tw > wp.x + ws.x - theme::kS2) break;
        ImGui::SetCursorScreenPos(ImVec2(x - 4, wp.y));
        ImGui::PushID(n.window);
        const bool click = ImGui::InvisibleButton("##t", ImVec2(tw + 8, ws.y - 1));
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        dl->AddText(font, fs, ImVec2(x, std::round(wp.y + (ws.y - fs) * 0.5f)), *f ? P.text : hov ? P.text : P.textDim, label);
        if (click && !*f) *f = true;  // the region's one-at-a-time rule closes the one shown
        x += tw + theme::kS3 + 2;
      }
    }
    ImGui::End();
  }
}

}  // namespace

bool reopenClosedTab(App& a, const std::string& region) {
  const Preset p = active();
  if (!shellBuilt(a, p)) return false;  // (the Smooth strips; K-237: the Clips and Tracker rails too)
  // The last tab closed (in `region` when one is named) leaves the history; the others stay in order.
  std::vector<panetabs::Closed> items = tabHistory().items();
  int pick = -1;
  for (int i = static_cast<int>(items.size()) - 1; i >= 0 && pick < 0; --i)
    if ((region.empty() || items[static_cast<size_t>(i)].region == region) && layout::hasRegion(p, items[static_cast<size_t>(i)].region)) pick = i;
  if (pick < 0) return false;
  const panetabs::Closed c = items[static_cast<size_t>(pick)];
  items.erase(items.begin() + pick);
  tabHistory() = panetabs::History{};
  for (const panetabs::Closed& it : items) tabHistory().push(it);
  tabClosed().erase(c.window);
  layout::State st = regionState(a, p);
  TabCands cand;
  panetabs::Strip strip = gRail.count(c.region) ? railStrip(a, p, c.region, st, cand) : paneStrip(a, p, c.region, st, cand);
  panetabs::reopen(strip, c);
  const std::string text = panetabs::save(strip);
  if (!st.tabs.count(c.region) || st.tabs[c.region] != text) {
    st.tabs[c.region] = text;
    setRegionState(a, p, st);
  }
  if (bool* f = windowOpenFlag(a, c.window)) *f = true;  // its pane shows it (and comes back if it was closed)
  gFrontTabs[c.window] = 0;  // (in front, where the pane holds several)
  return true;
}

bool cyclePaneTab(App& a, int dir) {
  const Preset p = active();
  if (!shellBuilt(a, p) || (gStrip.empty() && gRail.empty())) return false;
  // K-237: the Smooth strips and the Clips and Tracker rails.
  std::map<std::string, ImGuiID> panes = gStrip;
  panes.insert(gRail.begin(), gRail.end());
  // The pane that holds the focused window, else the lower pane, else the first one shown.
  std::string region;
  if (const ImGuiWindow* nav = ImGui::GetCurrentContext()->NavWindow)
    for (const ImGuiDockNode* n = nav->RootWindowDockTree ? nav->RootWindowDockTree->DockNode : nullptr; n && region.empty(); n = n->ParentNode)
      for (const auto& [r, strip] : panes)
        if (regionNode(r) == n->ID || outerNode(r) == n->ID) region = r;
  if (const ImGuiWindow* nav = ImGui::GetCurrentContext()->NavWindow; region.empty() && nav && nav->DockNode)
    for (const ImGuiDockNode* n = nav->DockNode; n && region.empty(); n = n->ParentNode)
      for (const auto& [r, strip] : panes)
        if (regionNode(r) == n->ID || outerNode(r) == n->ID) region = r;
  const layout::State st = regionState(a, p);
  if (region.empty()) region = panes.count("logic.lower") && layout::visible(layout::shell(p), st, "logic.lower") ? "logic.lower" : panes.begin()->first;
  TabCands cand;
  const bool rail = gRail.count(region) > 0;
  const panetabs::Strip strip = rail ? railStrip(a, p, region, st, cand) : paneStrip(a, p, region, st, cand);
  const std::string next = panetabs::cycle(strip, dir);
  if (next.empty() || next == strip.active || !cand.count(next)) return false;
  if (rail) gFrontTabs[next] = 0;  // the rail's windows stay open: the next comes to the front
  else showPaneTab(a, cand[next].second);  // the region's one-at-a-time rule closes the one shown
  return true;
}

namespace {
bool gLogicRestore = false;  // R01-04: returned to the Tracks layout: its windows come back once it is built
const ImGuiWindow* findTop(const std::string& name);
}

void shellFrame(App& a) {
  const Preset p = active();
  {
    static int logicSeen = -10;
    if (p == Preset::Logic) {
      if (logicSeen != ImGui::GetFrameCount() - 1) gLogicRestore = true, gLogicRestoreUntil = ImGui::GetFrameCount() + 60;
      logicSeen = ImGui::GetFrameCount();
    }
  }
  if (!shellBuilt(a, p) || gBuiltFor != p) return;
  // K-297 (round 8): the tabs in front at quit come to the front once their windows dock (no key
  // focus moves: the keys stay in the main view). Given up after two seconds.
  for (auto it = gFrontTabs.begin(); it != gFrontTabs.end();) {
    ImGuiWindow* w = ImGui::FindWindowByName((std::string("###") + it->first).c_str());
    if (!w) w = ImGui::FindWindowByName(it->first.c_str());
    ImGuiDockNode* n = w && w->DockIsActive ? w->DockNode : nullptr;
    bool done = ++it->second > 120 || (n && n->VisibleWindow == w);
    if (!done && n && n->TabBar && w->TabId) n->TabBar->NextSelectedTabId = w->TabId;
    else if (!done && n && (n->IsNoTabBar() || n->IsHiddenTabBar()))
      if (ImGuiWindow** f = n->Windows.find(w); f != n->Windows.end() && f != n->Windows.begin()) n->Windows.erase(f), n->Windows.push_front(w);
    it = done ? gFrontTabs.erase(it) : std::next(it);
  }
  const layout::Shell& sh = layout::shell(p);
  layout::State st = regionState(a, p);
  // A window opened while its region is hidden (View > Panels, a command) shows the region.
  for (const layout::Item& it : layout::items()) {
    const layout::Home& h = homeIn(it, p);
    if (!*it.window || (h.kind != HomeKind::Dock && h.kind != HomeKind::Part) || !*h.region) continue;
    bool* f = windowOpenFlag(a, it.window);
    // Round 45: a window opened by its own command (no flag: Loupe pinned, the automation editor, ...)
    // counts as open while it is drawn; with the Clips detail bands taking turns its band may be hidden.
    // Only the frame it appears (closing its band while it stays drawn must not bring the band back).
    static std::set<std::string> drawnBefore;
    const ImGuiWindow* drawn = f ? nullptr : ImGui::FindWindowByName(it.window);
    const bool drawnNow = drawn && drawn->WasActive;
    const bool appeared = drawnNow && !drawnBefore.count(it.window);
    if (!f) drawnNow ? (void)drawnBefore.insert(it.window) : (void)drawnBefore.erase(it.window);
    const bool open = f ? *f : appeared;
    if (open && layout::hasRegion(p, h.region) && !layout::visible(sh, st, h.region)) {
      setRegionVisible(a, h.region, true);
      // Tracks layout: an exclusive pane shows the window asked for, not its default member beside
      // it (the default would win the pane and close this one: R01-04's Mixer and Lists).
      if (const layout::Region* r = layout::findRegion(p, h.region); p == Preset::Logic && r && r->exclusive)
        for (const layout::Item& o : layout::items())
          if (*o.window && std::strcmp(o.window, it.window) != 0 && std::strcmp(homeIn(o, p).region, h.region) == 0)
            if (bool* g = windowOpenFlag(a, o.window)) *g = false;
      if (f) *f = true;
      return;  // rebuilt next frame
    }
  }
  // Tracks layout: a view with no open flag (Loupe, the automation editor, Chord progression,
  // the activity log) that has just started drawing while its pane is closed (the pane closed
  // with its last view) shows the pane too, once, as it starts.
  if (p == Preset::Logic) {
    static std::set<std::string> drawnBefore;
    std::set<std::string> drawnNow;
    for (const layout::Item& it : layout::items()) {
      const layout::Home& h = homeIn(it, p);
      if (!*it.window || (h.kind != HomeKind::Dock && h.kind != HomeKind::Part) || !*h.region) continue;
      if (windowOpenFlag(a, it.window)) continue;
      const ImGuiWindow* w = findTop(it.window);
      if (!w || !w->WasActive) continue;
      drawnNow.insert(it.window);
      if (!drawnBefore.count(it.window) && layout::hasRegion(p, h.region) && !layout::visible(sh, st, h.region)) {
        drawnBefore = drawnNow;
        drawnBefore.insert(it.window);
        setRegionVisible(a, h.region, true);
        return;  // rebuilt next frame
      }
    }
    drawnBefore = std::move(drawnNow);
  }
  // R01-05 / R01-06 (Tracks layout): a pane whose last view closed (E, X, B or D pressed again, a
  // window's own close) closes too, so the tracks area takes the room back, as the reference's
  // panes do. Only on that change: a pane shown for a window that opens a frame later (the audio
  // editor, an automation clip) stays, and so does a pane its own command just showed.
  // R01-04: the windows open in the layout come back as they were when the person returns to it
  // (another layout's build opened and closed others).
  if (p == Preset::Logic) {
    static std::map<std::string, bool> hadOpen;
    if (gLogicRestore) {
      gLogicRestore = false;
      hadOpen.clear();
      restoreLogicWindows(a);
    }
    for (const layout::Region& r : sh.regions) {
      if (!r.exclusive || !layout::visible(sh, st, r.id)) {
        hadOpen.erase(r.id);
        continue;
      }
      bool any = false;
      for (const layout::Item& it : layout::items()) {
        const layout::Home& h = homeIn(it, p);
        if (!*it.window || r.id != std::string(h.region)) continue;
        if (const bool* f = windowOpenFlag(a, it.window)) any = any || *f;
        else if (const ImGuiWindow* w = findTop(it.window)) any = any || w->WasActive;  // no flag: drawn last frame
      }
      const bool was = hadOpen.count(r.id) && hadOpen[r.id];
      hadOpen[r.id] = any;
      if (was && !any && !(r.id == std::string("logic.lower") && a.settings.getBool("ui.loupePinned"))) {
        hadOpen.erase(r.id);
        setRegionVisible(a, r.id, false);
        return;  // rebuilt next frame
      }
    }
    gLogicOpen.clear();
    for (const WindowFlag& f : windowFlags(a))
      if (*f.open) gLogicOpen.insert(f.window);
    gLogicHaveOpen = true;
  }
  // A window opened into a region that is already shown goes to its home there (the dock
  // builder places only the windows open when the region was built). One the person dragged out
  // stays where they put it until it is closed.
  static std::map<std::string, bool> wasOpen;  // window -> open last frame (this layout)
  static Preset wasOpenFor = Preset::Studio;
  static int wasOpenFrame = -10;
  // Just switched here (or back): the windows are where this layout keeps them.
  const bool fresh = wasOpenFor != p || wasOpenFrame != ImGui::GetFrameCount() - 1;
  if (fresh) wasOpen.clear(), wasOpenFor = p;
  wasOpenFrame = ImGui::GetFrameCount();
  std::vector<std::string> closedIn;  // shown regions a window closed in this frame
  for (const layout::Item& it : layout::items()) {
    const layout::Home& h = homeIn(it, p);
    if (!*it.window || (h.kind != HomeKind::Dock && h.kind != HomeKind::Part) || !*h.region) continue;
    bool* f = windowOpenFlag(a, it.window);
    if (!f) continue;
    bool& before = wasOpen[it.window];
    if (*f && !before && !fresh)
      if (const ImGuiID node = regionNode(h.region))
        if (ImGui::DockBuilderGetNode(node)) ImGui::DockBuilderDockWindow(it.window, node);
    if (!*f && before && !fresh && layout::hasRegion(p, h.region) && layout::visible(sh, st, h.region)) closedIn.push_back(h.region);
    before = *f;
  }
  // K-297: the last window of a region closed (its close button, View > Panels, a shortcut): the region
  // is hidden in the region state once it has stayed empty for a moment. It stayed "shown", an empty
  // band (ImGui already folds its node away) that the status bar showed as off while the state, the
  // layout switch and the saved settings kept it. Only the state changes (no rebuild), and a window
  // opened there soon after (a part swapped for another) finds the region still shown.
  static std::map<std::string, int> emptySince;  // region -> the frame its last window closed
  if (fresh) emptySince.clear();
  for (const std::string& region : closedIn) emptySince.emplace(region, ImGui::GetFrameCount());
  for (auto e = emptySince.begin(); e != emptySince.end();) {
    bool any = false;
    for (const layout::Item& it : layout::items()) {
      const layout::Home& h = homeIn(it, p);
      if (!*it.window || e->first != h.region) continue;
      // A window homed here without an open flag (a plugin's window, the agent activity) opens without
      // showing its region first: such a region stays shown.
      const bool* f = windowOpenFlag(a, it.window);
      if (!f || *f) any = true;
    }
    if (any || !layout::visible(sh, st, e->first)) {
      e = emptySince.erase(e);
    } else if (ImGui::GetFrameCount() - e->second >= 30) {
      // Another window drawn there (one not homed in the region, docked there by the person) keeps it.
      // (A closed window stays in its dock node's list, inactive.)
      bool held = false;
      if (const ImGuiDockNode* dn = ImGui::DockBuilderGetNode(regionNode(e->first)))
        for (const ImGuiWindow* w : dn->Windows) held = held || w->WasActive;
      if (!held) layout::setVisible(sh, st, e->first, false), setRegionState(a, p, st);
      e = emptySince.erase(e);
    } else {
      ++e;
    }
  }
  bool changed = false;
  static std::map<std::string, bool> prevOpen;  // "<region>|<window>" -> open last frame
  // K-297: a divider the person drags. ImGui moves a dock split while its splitter is the active item
  // of the dock host. Meanwhile fixed bands are not pinned back (they snapped back every frame, so the
  // device band's divider shrank the clip band instead), and when it lets go the regions it moved keep
  // their new size in the region state: a rebuild (a region shown or hidden) or a window resize no
  // longer undoes the drag.
  const ImGuiContext& g = *ImGui::GetCurrentContext();
  const ImGuiWindow* host = ImGui::FindWindowByName("##dockhost");
  // (The splitters belong to the dock space's own child window, "##dockhost/DockSpace_...".)
  const bool splitting = host && g.ActiveId != 0 && g.ActiveIdWindow && g.ActiveIdWindow->RootWindow == host && ImGui::IsMouseDown(ImGuiMouseButton_Left);
  static bool wasSplitting = false;
  static std::map<std::string, float> atGrab;  // region -> its extent when the divider was grabbed
  const bool grabbed = splitting && !wasSplitting, released = wasSplitting && !splitting;
  wasSplitting = splitting;
  if (grabbed) atGrab.clear();
  // K-297: ImGui moves the band that touches a divider and keeps the far one, so dragging the device
  // band's divider shrank the clip band above it while the main view kept its height. A band the
  // divider only touched (deeper in the dock tree than the one the divider belongs to) keeps its
  // size; the centre beside it takes the change, as a stacked detail view grows over the main view.
  auto depth = [](const ImGuiDockNode* n) {
    int d = 0;
    for (; n && n->ParentNode; n = n->ParentNode) ++d;
    return d;
  };
  auto holdsCentre = [](auto&& self, const ImGuiDockNode* n) -> bool {
    return n && (n->IsCentralNode() || self(self, n->ChildNodes[0]) || self(self, n->ChildNodes[1]));
  };
  std::set<std::string> touchedOnly;
  if ((splitting || released) && !atGrab.empty()) {
    int top = 1 << 20;
    std::vector<std::pair<const layout::Region*, ImGuiDockNode*>> moved;
    for (const layout::Region& r : sh.regions) {
      ImGuiDockNode* dn = outerNode(r.id) ? ImGui::DockBuilderGetNode(outerNode(r.id)) : nullptr;
      const auto at = atGrab.find(r.id);
      if (!dn || at == atGrab.end()) continue;
      const bool hz = r.edge == layout::Edge::Left || r.edge == layout::Edge::Right;
      if (std::fabs(std::floor(hz ? dn->Size.x : dn->Size.y) - at->second) > 1.5f) moved.push_back({&r, dn}), top = std::min(top, depth(dn));
    }
    for (const auto& [r, dn] : moved) {
      ImGuiDockNode* par = dn->ParentNode;
      ImGuiDockNode* sib = par ? (par->ChildNodes[0] == dn ? par->ChildNodes[1] : par->ChildNodes[0]) : nullptr;
      if (depth(dn) <= top || !sib || !holdsCentre(holdsCentre, sib)) continue;
      touchedOnly.insert(r->id);
      const bool hz = r->edge == layout::Edge::Left || r->edge == layout::Edge::Right;
      const float now = hz ? dn->Size.x : dn->Size.y, centre = hz ? sib->Size.x : sib->Size.y;
      // The centre keeps a usable size; past it the band gives way as before.
      const float back = std::min(atGrab[r->id] - now, std::max(0.0f, centre - 120.0f));
      (hz ? dn->SizeRef.x : dn->SizeRef.y) = std::floor(now + back);
    }
  }
  for (const layout::Region& r : sh.regions) {
    const ImGuiID node = outerNode(r.id);
    ImGuiDockNode* dn = node ? ImGui::DockBuilderGetNode(node) : nullptr;
    const bool hz = r.edge == layout::Edge::Left || r.edge == layout::Edge::Right;
    if (dn && (grabbed || released)) {
      const float now = std::floor(hz ? dn->Size.x : dn->Size.y);
      if (grabbed) {
        atGrab[r.id] = now;
      } else if (const auto at = atGrab.find(r.id); at != atGrab.end() && now > 0 && std::fabs(now - at->second) > 1.5f && !touchedOnly.count(r.id)) {
        const std::string member = r.exclusive && st.member.count(r.id) ? st.member.at(r.id) : std::string();
        if (!member.empty()) st.memberSize[std::string(r.id) + "|" + member] = now;
        else st.regionSize[r.id] = now;
        changed = true;
      }
    }
    if (r.exclusive && dn) {
      // The members: windows homed here. One opened this frame closes the others.
      std::vector<std::pair<const char*, bool*>> members;
      for (const layout::Item& it : layout::items()) {
        const layout::Home& h = homeIn(it, p);
        if (*it.window && r.id == std::string(h.region))
          if (bool* f = windowOpenFlag(a, it.window)) members.push_back({it.window, f});
      }
      const char* opened = nullptr;
      for (auto& [w, f] : members) {
        const std::string key = std::string(r.id) + "|" + w;
        if (*f && !prevOpen[key]) opened = w;
      }
      if (!opened) {  // several open at once (a saved layout): the first stays
        for (auto& [w, f] : members)
          if (*f) {
            if (!opened) opened = w;
            else *f = false, gClosedByGroup.insert(w);
          }
      } else {
        for (auto& [w, f] : members)
          if (std::strcmp(w, opened) != 0 && *f) *f = false, gClosedByGroup.insert(w);
      }
      if (opened) {
        const std::string was = st.member.count(r.id) ? st.member.at(r.id) : std::string();
        if (was != opened) {
          // The member leaving keeps its size; the one coming takes its own.
          if (!was.empty()) st.memberSize[std::string(r.id) + "|" + was] = std::floor(hz ? dn->SizeRef.x : dn->SizeRef.y);
          const auto sz = st.memberSize.find(std::string(r.id) + "|" + opened);
          if (sz != st.memberSize.end()) (hz ? dn->SizeRef.x : dn->SizeRef.y) = sz->second;
          st.member[r.id] = opened;
          changed = true;
        }
      }
      for (auto& [w, f] : members) {
        const std::string key = std::string(r.id) + "|" + w;
        if (*f) gClosedByGroup.erase(w);
        prevOpen[key] = *f;
      }
    }
    // Fixed bands keep their pixel size (the refit after a window resize scales side panels): the
    // layout's, or the one the person dragged the band to (State::regionSize). Not while a divider moves.
    // K-297: the other bands sized in pixels take their size too in the first frames after a build. The
    // tree is built at the size of the dock space as the frame began, which on a layout switch is the
    // old layout's (its margins, its bars): ImGui then scaled them, and Clips' browser came back 2 to 4 px
    // narrower after every visit to another layout.
    const bool settling = ImGui::GetFrameCount() - gBuiltFrame <= 2;
    if ((r.fixed || settling) && dn && r.size.px > 0 && dn->ParentNode && !splitting) {
      const float parent = hz ? dn->ParentNode->Size.x : dn->ParentNode->Size.y;
      float want = layout::regionExtent(r, parent, st);
      if (const auto b = boosts().find(r.id); b != boosts().end() && b->second.px > 0 && ImGui::GetFrameCount() - b->second.frame <= 1)
        want = b->second.exact ? std::min(b->second.px, std::floor(parent * b->second.maxShare))
                               : std::max(want, std::min(want + b->second.px, std::floor(parent * b->second.maxShare)));
      float& v = hz ? dn->SizeRef.x : dn->SizeRef.y;
      if (want > 0 && std::fabs(v - want) > 0.5f) v = want;
      // K-112c (round 22): a sibling without the central node shares the parent by the ratio of the
      // two sizes, which rounded the band a few pixels off its size (the info corner stood 4 to 6 px
      // taller than the device band beside it). The sibling takes exactly what is left, so the band
      // is its pixel size and bands of the same size line up across the window.
      ImGuiDockNode* par = dn->ParentNode;
      ImGuiDockNode* sib = par->ChildNodes[0] == dn ? par->ChildNodes[1] : par->ChildNodes[0];
      if (want > 0 && sib && !holdsCentre(holdsCentre, sib)) {
        const float rest = std::max(1.0f, parent - ImGui::GetStyle().DockingSeparatorSize - want);
        float& sv = hz ? sib->SizeRef.x : sib->SizeRef.y;
        if (std::fabs(sv - rest) > 0.5f) sv = rest;
      }
    }
  }
  if (released && keepBandSizes(p, st)) changed = true;  // K-297 (round 8)
  // Title strips keep their height.
  for (const auto& [region, strip] : gStrip)
    if (ImGuiDockNode* n = ImGui::DockBuilderGetNode(strip)) {
      // A locked size: the split would otherwise give each side at least the minimum window height.
      // (Every frame: the lock lasts one layout pass.)
      n->Size.y = n->SizeRef.y = layout::kPaneStripH;
      n->WantLockSizeOnce = true;
    }
  for (const auto& [region, rail] : gRail)  // K-237: and tab rails their width
    if (ImGuiDockNode* n = ImGui::DockBuilderGetNode(rail)) {
      n->Size.x = n->SizeRef.x = layout::kPaneRailW;
      n->WantLockSizeOnce = true;
    }
  if (changed) setRegionState(a, p, st);
  drawPaneStrips(a);
  drawPaneRails(a);
  runPendingMoves(a);  // K-237: tabs dropped on another pane
}

int dockFlags(const char* windowName, int base) {
  const Preset p = active();
  if (p == Preset::Studio) return base;
  const layout::Home h = windowHomeIn(ui::stableWindowName(windowName), p);
  if ((h.kind == HomeKind::Dock || h.kind == HomeKind::Part) && *h.region) {
    // A dialog placement (SetNextWindowPos) must not take the window out of its region.
    ImGui::GetCurrentContext()->NextWindowData.PosUndock = false;
    // Opening (appearing): into its region, when the region is built. One the person dragged out
    // stays where they put it until it is closed.
    const ImGuiWindow* w = ImGui::FindWindowByName(windowName);
    if (!w || !w->WasActive)
      if (const ImGuiID node = regionNode(h.region))
        if (ImGui::DockBuilderGetNode(node)) ImGui::SetNextWindowDockID(node, ImGuiCond_Always);
    return base & ~ImGuiWindowFlags_NoDocking & ~ImGuiWindowFlags_AlwaysAutoResize;
  }
  return base;
}

namespace {
std::map<std::string, ImVec2> gFloatWant;      // each floating part's wanted size (as it asked)
std::map<std::string, std::string> gFloatKey;  // the tiling each one was last placed for
}  // namespace

bool floatHome(App& a, const char* stableName, ImVec2 want, ImVec2* pos, ImVec2* size, bool* retile) {
  const Preset p = active();
  const std::string name = ui::stableWindowName(stableName);
  if (!shellBuilt(a, p) || windowHomeIn(name, p).kind != HomeKind::Float) return false;
  const ImGuiDockNode* n = ImGui::DockBuilderGetNode(regionNode(layout::shell(p).centre));
  if (!n || n->Size.x <= 0 || n->Size.y <= 0) return false;
  const float in = theme::kS3, top = theme::kToolbarH + ImGui::GetFrameHeight() + in;  // the centre's tab row and toolbar stay clear
  const ImVec2 room(n->Size.x - 2 * in, n->Size.y - top - in);
  if (room.x < 160 || room.y < 80) return false;
  gFloatWant[name] = want;
  // The open floating parts of this layout, in table order: right to left along the lower edge.
  std::vector<std::string> open;
  for (const layout::Item& it : layout::items())
    if (std::strncmp(it.id, "N", 1) == 0 && *it.window && !*it.later && homeIn(it, p).kind == HomeKind::Float)
      if (const bool* f = windowOpenFlag(a, it.window); (f && *f) || name == it.window) open.emplace_back(it.window);
  const float gap = theme::kS2;
  const float share = std::floor((room.x - gap * static_cast<float>(open.size() - 1)) / static_cast<float>(std::max<size_t>(1, open.size())));
  float right = n->Pos.x + n->Size.x - in;
  std::string key = std::to_string(static_cast<int>(n->Pos.x)) + "," + std::to_string(static_cast<int>(n->Pos.y)) + "," +
                    std::to_string(static_cast<int>(n->Size.x)) + "," + std::to_string(static_cast<int>(n->Size.y));
  for (const std::string& w : open) {
    const auto wi = gFloatWant.find(w);
    const ImVec2 ww = wi != gFloatWant.end() ? wi->second : want;
    const ImVec2 sz(std::floor(std::min(ww.x, share)), std::floor(std::min(ww.y, room.y)));
    key += "|" + w;
    if (w == name) {
      *size = sz;
      *pos = ImVec2(std::floor(right - sz.x), std::floor(n->Pos.y + n->Size.y - in - sz.y));
    }
    right -= sz.x + gap;
  }
  if (retile) {
    std::string& last = gFloatKey[name];
    *retile = !last.empty() && last != key;
    last = key;
  }
  return true;
}

// ------------------------------------------------------------ snapshot ----

namespace {
GLuint gSnapTex = 0;
int gSnapW = 0, gSnapH = 0;
uint64_t gSnapEpoch = 0;
int gSnapFrames = -1;      // frames drawn since the switch (-1 = no snapshot)
double gSnapFadeAt = -1;   // when the fade began
double gSnapTakenAt = -1;  // when it was first drawn
int gSnapFrameNo = -1;     // ImGui's frame count when it was taken
float gSnapAlpha = -1;
constexpr int kHoldFrames = 2;
constexpr double kFade = 0.12;    // seconds
constexpr double kMaxLife = 0.6;  // seconds: never shown longer than this, whatever the frame rate

// The snapshot is gone and its texture freed: a stale frame can never come back.
void dropSnapshot() {
  gSnapFrames = -1;
  gSnapFadeAt = gSnapTakenAt = -1;
  if (gSnapTex && gSnapEpoch == glepoch::current()) glDeleteTextures(1, &gSnapTex);
  gSnapTex = 0;
}
}  // namespace

void captureSnapshot(int fbW, int fbH) {
  if (fbW <= 0 || fbH <= 0) return;
  if (gSnapEpoch != glepoch::current()) {  // K-47: a driver reset took the texture
    gSnapTex = 0;
    gSnapEpoch = glepoch::current();
  }
  GLint last = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &last);
  if (!gSnapTex) glGenTextures(1, &gSnapTex);
  glBindTexture(GL_TEXTURE_2D, gSnapTex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 0, 0, fbW, fbH, 0);
  glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(last));
  gSnapW = fbW;
  gSnapH = fbH;
  gSnapFrames = 0;
  gSnapFadeAt = gSnapTakenAt = -1;
  gSnapFrameNo = ImGui::GetFrameCount();
}

bool drawSnapshot(double now, int fbW, int fbH, bool homeShown) {
  gSnapAlpha = -1;
  if (gSnapFrames < 0) return false;
  // Only ever over the very next frames of the same main window: a frame it was not drawn on
  // (the home screen, a skipped frame), a resize, a driver reset or a long stall drops it.
  const bool next = gSnapFrames > 0 || ImGui::GetFrameCount() == gSnapFrameNo + 1;
  if (homeShown || !next || !gSnapTex || gSnapEpoch != glepoch::current() || fbW != gSnapW || fbH != gSnapH ||
      (gSnapTakenAt >= 0 && now - gSnapTakenAt > kMaxLife)) {
    dropSnapshot();
    return false;
  }
  if (gSnapTakenAt < 0) gSnapTakenAt = now;
  float k = 1.0f;
  if (gSnapFrames >= kHoldFrames) {
    if (gSnapFadeAt < 0) gSnapFadeAt = now;
    const double t = (now - gSnapFadeAt) / kFade;
    if (t >= 1.0) {
      dropSnapshot();
      return false;
    }
    k = static_cast<float>(1.0 - t);
  }
  ++gSnapFrames;
  ImGuiViewport* vp = ImGui::GetMainViewport();
  // The frame was read bottom-up: flip it.
  ImGui::GetForegroundDrawList(vp)->AddImage(static_cast<ImTextureID>(gSnapTex), vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
                                             ImVec2(0, 1), ImVec2(1, 0), IM_COL32(255, 255, 255, static_cast<int>(std::lround(255.0f * k))));
  gSnapAlpha = k;
  return true;
}

float snapshotAlpha() { return gSnapAlpha; }
bool snapshotHeld() { return gSnapTex != 0; }
void releaseSnapshot() { dropSnapshot(); }

// -------------------------------------------------------------- report ----

std::string shellReport(App& a) {
  const Preset p = active();
  std::string out = std::string("shell ") + layout::id(p) + (shellBuilt(a, p) ? " built" : " k79") + "\n";
  if (!shellBuilt(a, p)) return out;
  const layout::Shell& sh = layout::shell(p);
  const layout::State st = regionState(a, p);
  auto one = [&](const std::string& id) {
    const ImGuiID n = regionNode(id);
    const ImGuiDockNode* dn = n ? ImGui::DockBuilderGetNode(n) : nullptr;
    std::string ws;
    if (dn)
      for (const ImGuiWindow* w : dn->Windows) ws += std::string(ws.empty() ? "" : ",") + ui::stableWindowName(w->Name);
    char b[512];  // K-297 (round 9): and the tab in front
    std::snprintf(b, sizeof b, "region %s visible %d node %d rect %.0f %.0f %.0f %.0f windows [%s] front %s\n", id.c_str(),
                  layout::visible(sh, st, id) ? 1 : 0, dn && dn->IsVisible ? 1 : 0, dn ? static_cast<double>(dn->Pos.x) : 0.0,
                  dn ? static_cast<double>(dn->Pos.y) : 0.0, dn ? static_cast<double>(dn->Size.x) : 0.0, dn ? static_cast<double>(dn->Size.y) : 0.0,
                  ws.c_str(), dn && dn->VisibleWindow ? ui::stableWindowName(dn->VisibleWindow->Name) : "-");
    out += b;
  };
  for (const layout::Region& r : sh.regions) one(r.id);
  one(sh.centre);
  out += "state " + layout::toJson(st) + "\n";
  return out;
}

std::string stateLine(App& a) {
  uint32_t h = 2166136261u;
  for (unsigned char c : projectToText(a.doc.song())) h = (h ^ c) * 16777619u;
  std::string clips;
  for (ObjectId id : a.playlist.selected) clips += (clips.empty() ? "" : ",") + std::to_string(id);
  std::string notes;
  for (size_t i : a.piano.selected) notes += (notes.empty() ? "" : ",") + std::to_string(i);
  std::string open;
  for (const WindowFlag& f : windowFlags(a))
    if (*f.open) open += (open.empty() ? "" : ",") + std::string(f.window);
  char b[1600];
  std::snprintf(b, sizeof b,
                "layoutstate layout %s channel %llu pattern %llu mixer %llu clips [%s] notes [%s] playing %d tick %.0f "
                "plzoom %.4f plscroll %.0f prscroll %.0f project %08x snapshot %.2f open [%s]",
                layout::id(active()), static_cast<unsigned long long>(a.selChannel), static_cast<unsigned long long>(a.curPattern),
                static_cast<unsigned long long>(a.selMixer), clips.c_str(), notes.c_str(), a.engine.playing() ? 1 : 0,
                static_cast<double>(a.engine.position()), static_cast<double>(a.playlist.pxPerTick), static_cast<double>(a.playlist.scrollX),
                static_cast<double>(a.piano.scrollX), static_cast<unsigned>(h), static_cast<double>(snapshotAlpha()), open.c_str());
  return b;
}

// ------------------------------------------------------------- reveal ----

namespace {
// The windows that show a kind of object, best first; the first one the active layout docks
// (its home is a region) is used, else the first.
std::vector<const char*> showersOf(anchor::Kind k) {
  switch (k) {
    case anchor::Kind::Channel: return {"Channel rack", "Device chain", "Inspector"};
    case anchor::Kind::MixerTrack: return {"Mixer", "Device chain", "Inspector"};
    case anchor::Kind::Pattern: return {"Piano roll", "Channel rack"};
    case anchor::Kind::Clip: return {"Playlist", "Clip panel"};
    case anchor::Kind::PlaylistTrack:
    case anchor::Kind::Marker: return {"Playlist"};
    default: return {};
  }
}
}  // namespace

const char* revealWindow(const App& a, anchor::Kind k) {
  const Preset p = active();
  const std::vector<const char*> v = showersOf(k);
  if (v.empty()) return nullptr;
  if (shellBuilt(a, p))
    for (const char* w : v) {
      const layout::Home h = windowHomeIn(w, p);
      if ((h.kind == HomeKind::Dock || h.kind == HomeKind::Part) && *h.region) return w;
    }
  return v[0];
}

bool reveal(App& a, const anchor::Target& t) {
  const Song& s = a.doc.song();
  if (!anchor::exists(s, t)) return false;
  // Only the selection and the view change: nothing in the project does.
  switch (t.kind) {
    case anchor::Kind::Channel: a.selChannel = t.id; break;
    case anchor::Kind::MixerTrack:
      a.selMixer = t.id;
      if (t.sub > 0 && t.sub <= static_cast<uint32_t>(kEffectSlots)) a.fxEditor = {t.id, static_cast<int>(t.sub) - 1};
      break;
    case anchor::Kind::Pattern:
      a.curPattern = t.id;
      if (t.sub && s.channel(t.sub)) a.selChannel = t.sub;
      break;
    case anchor::Kind::Clip:
      a.playlist.selected = {t.id};
      for (const Clip& c : s.playlist)  // the playhead goes there only when stopped (playback is never cut)
        if (c.id == t.id && !a.engine.playing()) a.engine.setPosition(c.start);
      break;
    case anchor::Kind::Marker:
      if (!a.engine.playing()) a.engine.setPosition(static_cast<Tick>(t.id));
      break;
    default: break;
  }
  if (const char* w = revealWindow(a, t.kind)) {
    if (bool* f = windowOpenFlag(a, w)) *f = true;
    a.focusWindow = w;
  }
  return true;
}

// ------------------------------------------------------------ inventory ----
// "layout inventory": every item of the homes table that is a window is opened in turn in the
// active layout, and where it lands is checked against its home (one line per item).

namespace {
struct Inventory {
  bool on = false;
  size_t item = 0;
  int step = 0;
  int ok = 0, failed = 0, skipped = 0, planned = 0;
  bool wasOpen = false;  // the item's window was open before: it stays open after its check
  // A plugin row: the channel or slot made for it (removed after its check).
  ObjectId channel = kNoId, track = kNoId;
  int slot = -1;
  bool chainWasOpen = false;
};
Inventory gInv;

// A top level window by its stable name ("Mixer", "###channelsettings"). Docked windows count:
// ImGui marks a docked window as a child of its dock host.
const ImGuiWindow* findTop(const std::string& name) {
  const std::string stable = ui::stableWindowName(name.c_str());
  for (const ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
    if ((!(w->Flags & ImGuiWindowFlags_ChildWindow) || w->DockNode) && stable == ui::stableWindowName(w->Name)) return w;
  return nullptr;
}

std::string regionOf(const ImGuiWindow* w) {
  if (!w || !w->DockIsActive || !w->DockNode) return "-";
  for (const ImGuiDockNode* n = w->DockNode; n; n = n->ParentNode)
    for (const auto& [rid, nid] : gNodes)
      if (nid == n->ID) return rid;
  return "?";
}

// A window the person opens with a selection or its own command rather than a flag (the instrument
// and effect windows, the automation and chord editors, Loupe pinned into the layout, ...).
bool openSpecial(App& a, const std::string& w, bool on) {
  const Song& s = a.doc.song();
  if (w == "Loupe") {  // the Loupe panel: Loupe pinned into the layout
    a.settings.set("ui.loupePinned", on);
    return true;
  }
  if (w == "###mcpactivity") {
    if (on) mcpui::showActivity();
    else mcpui::hideActivity();
    return true;
  }
  if (w == "###mixrouting") {
    a.mixerView.showRouting = on;
    return true;
  }
  if (w == "###autoedit") {
    if (!on) {
      closeAutomationEditor();
      return true;
    }
    if (s.automations.empty()) {
      if (s.channels.empty()) return false;
      const ObjectId owner = s.channels[0].id;
      a.doc.editSong("add automation clip", [owner](Song& x) {
        AutomationClip ac;
        ac.id = x.allocateId();
        ac.owner = owner;
        AutoPoint p0, p1;
        p0.value = 0.2f;
        p1.pos = static_cast<Tick>(x.ppq) * 4;
        p1.value = 0.8f;
        ac.points = {p0, p1};
        x.automations.push_back(ac);
      });
    }
    openAutomationEditor(a, a.doc.song().automations.front().id);
    return true;
  }
  if (w == "Chord progression") {
    if (on) j31::openChordEditor(a);
    else j31::closeChordEditor(a);
    return true;
  }
  if (w == "About Gridline") {
    a.showAbout = on;
    return true;
  }
  if (w == "###trackreceipt") {
    if (on) receiptui::open(a);
    else receiptui::close();
    return true;
  }
  if (w == "Remote control settings") {
    a.showRemoteDialog = on;
    return true;
  }
  if (w == "Section probe") {
    collabui::devCommand(a, on ? std::vector<std::string>{"section-probe"} : std::vector<std::string>{"section-probe", "off"}, 0);
    return true;
  }
  if (w == "###channelsettings") {
    a.settingsChannel = on && !s.channels.empty() ? s.channels[0].id : kNoId;
    return true;
  }
  if (w == "###fxeditor") {
    if (!on) {
      a.fxEditor = {kNoId, -1};
      return true;
    }
    if (s.mixer.size() < 2) return false;
    const ObjectId t = s.mixer[1].id;
    if (s.mixer[1].slots[0].plugin.type.empty()) a.doc.setSlotPlugin(t, 0, "gl.eq");
    a.fxEditor = {t, 0};
    return true;
  }
  return false;
}

// The host that draws a hosted window's body in this layout (a part open in that region).
const char* hostOf(const std::string& w) {
  if (w == "###channelsettings") return hostbody::claimed(hostbody::Body::ChannelSettings) ? hostbody::claimedBy(hostbody::Body::ChannelSettings) : nullptr;
  if (w == "###fxeditor") return hostbody::claimed(hostbody::Body::EffectEditor) ? hostbody::claimedBy(hostbody::Body::EffectEditor) : nullptr;
  return nullptr;
}

// A host region with no part built yet (its host is a later row): the window floats until then.
bool hostIsLater(Preset p, const char* region) {
  for (const layout::Item& it : layout::items()) {
    const layout::Home& h = homeIn(it, p);
    if (h.kind == HomeKind::Part && std::strcmp(h.region, region) == 0 && !*it.later) return false;
  }
  return true;
}

// Rows that are not windows of their own, and plugin editors, checked for what their home means.
// A layout home a later phase row builds (a band, an inspector field, a tracker page) is listed as
// planned with that row; everything else must hold now.
const char* plannedIn(const char* id, Preset p) {
  struct Plan {
    const char* id;
    const char* row[4];  // studio, live, logic, tracker ("" = holds now)
  };
  static const Plan kPlans[] = {
      {"C2", {"", "", "", "K-112d5"}},           {"C3", {"", "", "", "K-112d5"}},
      {"C8", {"", "K-112c7", "K-112b6", "K-112d5"}}, {"C10", {"", "K-112c5", "K-112b3", "K-112d1"}},
      {"C11", {"", "K-112c5", "", ""}},          {"C12", {"", "K-112c1", "K-112b7", "K-112d1"}},
      {"C13", {"", "K-112c1", "K-112b7", "K-112d1"}}, {"C14", {"", "K-112c5", "K-112b7", "K-112d1"}},
      {"C18", {"", "K-112c5", "", ""}},          {"C19", {"", "K-112c5", "", ""}},
      {"C20", {"", "K-112c5", "K-112b3", "K-112d1"}}, {"C21", {"", "K-112c6", "K-112b3", ""}},
      {"C25", {"", "", "K-112b7", ""}},          {"C26", {"", "", "", "K-112d5"}},
      {"D6", {"", "", "", "K-112d1"}},           {"D10", {"", "K-112c1", "K-112b7", "K-112d1"}},
      {"D15", {"", "K-112c7", "K-112b7", "K-112d1"}}, {"D18", {"", "K-112c1", "K-112b7", "K-112d1"}},
      {"D21", {"", "K-112c1", "K-112b7", "K-112d1"}}, {"D45", {"", "", "K-112b2", "K-112d1"}},
      {"G3", {"", "", "", "K-112d1"}},           {"G5", {"", "K-112c6", "K-112b5", "K-112d1"}},
      {"G7", {"", "K-112c5", "K-112b2", "K-112d1"}}, {"G8", {"", "", "", "K-112d5"}},
  };
  for (const Plan& x : kPlans)
    if (std::strcmp(x.id, id) == 0) return x.row[static_cast<int>(p)];
  return "";
}

// A row with no window of its own: what its home means, checked where it can be.
void staticRow(App& a, const layout::Item& it, Preset p, int frame) {
  const layout::Home& h = homeIn(it, p);
  const char* later = plannedIn(it.id, p);
  std::string want;
  bool pass = true;
  if (*later) {
    std::printf("frame %d inventory %s home %s %s planned %s\n", frame, it.id, layout::kindName(h.kind), *h.region ? h.region : "-", later);
    ++gInv.planned;
    return;
  }
  auto shown = [](const char* name) {
    const ImGuiWindow* w = ImGui::FindWindowByName(name);
    return w && w->Active;
  };
  switch (h.kind) {
    case HomeKind::Bar:  // in the top bar or the status line: both are drawn
      want = "in the bars";
      pass = shown("##topbar") && (shown("##status") || layoutui::statusBarHeight() <= 0);  // a layout with no status line
      break;
    case HomeKind::Dock:  // the dock host itself (A5): the layout's centre region
      want = *h.region ? h.region : "dock host";
      pass = p == Preset::Studio || !shellBuilt(a, p) || regionNode(*h.region ? h.region : layout::shell(p).centre) != 0;
      break;
    case HomeKind::Overlay:
      want = "over the windows";
      break;
    case HomeKind::Menu:
      want = "a menu";
      break;
    case HomeKind::Modal:
      want = "a dialog over the window";
      break;
    case HomeKind::Inside:
      want = "inside its window";
      break;
    case HomeKind::Float:
      want = "its own window, as in Studio";
      pass = it.home[0].kind == HomeKind::Float || it.home[0].kind == HomeKind::Modal;
      break;
    case HomeKind::Unchanged:
      want = "unchanged";
      break;
    default:
      want = layout::kindName(h.kind);
      pass = false;
      break;
  }
  std::printf("frame %d inventory %s home %s %s want %s %s\n", frame, it.id, layout::kindName(h.kind), *h.region ? h.region : "-", want.c_str(),
              pass ? "ok" : "FAIL");
  (pass ? gInv.ok : gInv.failed)++;
}

// A plugin row: the plugin is added (an instrument channel, or the last slot of the first mixer
// track), its editor opened as the person would, and the body's one host checked: its own window
// where the home is Float, the Device chain where it is Host. The channel or slot goes again.
bool pluginRow(App& a, const layout::Item& it, Preset p, int frame) {
  const layout::Home& h = homeIn(it, p);
  const PluginInfo* info = findPlugin(it.plugin);
  if (gInv.step == 0) {
    if (!info) {
      std::printf("frame %d inventory %s %s home %s %s FAIL not a plugin\n", frame, it.id, it.plugin, layout::kindName(h.kind), *h.region ? h.region : "-");
      ++gInv.failed;
      return true;
    }
    bool* chain = windowOpenFlag(a, "Device chain");
    gInv.chainWasOpen = chain && *chain;
    gInv.channel = gInv.track = kNoId;
    gInv.slot = -1;
    if (info->kind == PluginKind::Generator) {
      gInv.channel = a.addInstrument(it.plugin);
      a.settingsChannel = gInv.channel;
    } else if (info->kind == PluginKind::NoteFx) {
      gInv.channel = a.addInstrument("gl.hat");
      a.doc.addNoteFx(gInv.channel, it.plugin);
      a.settingsChannel = gInv.channel;
    } else if (a.doc.song().mixer.size() > 1) {
      gInv.track = a.doc.song().mixer[1].id;
      gInv.slot = kEffectSlots - 1;
      a.doc.setSlotPlugin(gInv.track, gInv.slot, it.plugin);
      a.fxEditor = {gInv.track, gInv.slot};
    }
    a.syncEngine();
    gInv.step = 1;
    return false;
  }
  if (gInv.step < 6) {
    ++gInv.step;
    return false;
  }
  std::string host;
  const int n = gInv.channel != kNoId ? hostbody::drawnNow(hostbody::Body::ChannelSettings, gInv.channel, -1, &host)
                                      : hostbody::drawnNow(hostbody::Body::EffectEditor, gInv.track, gInv.slot, &host);
  const bool hosted = h.kind == HomeKind::Host;
  const bool pass = n == 1 && (hosted ? host == "Device chain" : host != "Device chain");
  std::printf("frame %d inventory %s %s home %s %s got drawn %d by %s want %s %s\n", frame, it.id, it.plugin, layout::kindName(h.kind),
              *h.region ? h.region : "-", n, host.empty() ? "-" : host.c_str(), hosted ? "the Device chain" : "its own window", pass ? "ok" : "FAIL");
  (pass ? gInv.ok : gInv.failed)++;
  if (gInv.channel != kNoId) {
    a.settingsChannel = kNoId;
    a.doc.removeChannel(gInv.channel);
  } else if (gInv.track != kNoId) {
    a.fxEditor = {kNoId, -1};
    a.doc.setSlotPlugin(gInv.track, gInv.slot, "");
  }
  if (bool* chain = windowOpenFlag(a, "Device chain")) *chain = gInv.chainWasOpen;
  a.syncEngine();
  return true;
}

void inventoryFrame(App& a, int frame) {
  if (!gInv.on) return;
  const auto& items = layout::items();
  const Preset p = active();
  // Rows that are not windows: checked at once; plugin rows over a few frames.
  while (gInv.item < items.size() && !*items[gInv.item].window) {
    const layout::Item& row = items[gInv.item];
    if (*row.plugin) {
      if (!pluginRow(a, row, p, frame)) return;
      gInv.step = 0;
      ++gInv.item;
      std::fflush(stdout);
      return;
    }
    staticRow(a, row, p, frame);
    ++gInv.item;
  }
  if (gInv.item >= items.size()) {
    std::printf("frame %d inventory done layout %s rows %zu ok %d failed %d skipped %d planned %d\n", frame, layout::id(p), items.size(), gInv.ok,
                gInv.failed, gInv.skipped, gInv.planned);
    std::fflush(stdout);
    gInv = {};
    return;
  }
  const layout::Item& it = items[gInv.item];
  const std::string w = it.window;
  const layout::Home& h = homeIn(it, p);
  bool* f = windowOpenFlag(a, w);
  if (gInv.step == 0) {
    const bool can = f || openSpecial(a, w, true);
    if (w == "Sync review" && !collabui::session(a)) {  // shows only in a shared session (the collab layout test covers it)
      std::printf("frame %d inventory %s %s home %s %s skip needs a shared session\n", frame, it.id, w.c_str(), layout::kindName(h.kind),
                  *h.region ? h.region : "-");
      ++gInv.skipped, ++gInv.item;
      return;
    }
    if (!can || *it.later) {
      std::printf("frame %d inventory %s %s home %s %s skip %s\n", frame, it.id, w.c_str(), layout::kindName(h.kind), *h.region ? h.region : "-",
                  *it.later ? it.later : "opened by its own command");
      ++gInv.skipped, ++gInv.item;
      return;
    }
    gInv.wasOpen = f && *f;
    if (f) *f = true;  // a window homed in a hidden region shows the region (shellFrame)
    gInv.step = 1;
    return;
  }
  if (gInv.step < 8) {  // the dock builder and the window settle over a few frames
    ++gInv.step;
    return;
  }
  const ImGuiWindow* win = findTop(w);
  const bool drawn = win && win->Active;  // checked at the end of the frame: drawn this frame
  const std::string got = drawn ? regionOf(win) : "-";
  const char* host = hostOf(w);
  bool pass = false;
  std::string want;
  switch (h.kind) {
    case HomeKind::Dock:
    case HomeKind::Part:
      if (p == Preset::Studio || !shellBuilt(a, p) || !*h.region) {
        want = *h.region ? "docked" : "floating";
        pass = drawn && (*h.region ? win->DockIsActive : true);
      } else {
        want = h.region;
        pass = drawn && got == h.region;
      }
      break;
    case HomeKind::Host:
      if (host) want = "hosted", pass = !drawn;
      else if (hostIsLater(p, h.region)) want = "floating (host later)", pass = drawn;
      else want = "hosted", pass = false;
      break;
    case HomeKind::Bar:  // the Info box: in the status line, no window
      want = "in the status line";
      pass = !drawn && w == "Info box" && layoutparts::infoInBar(a);
      break;
    case HomeKind::Float:
    case HomeKind::Modal:
      want = h.kind == HomeKind::Float ? "floating" : "a dialog (floating)";
      pass = drawn && !win->DockIsActive;
      break;
    default:
      want = "drawn";
      pass = drawn;
      break;
  }
  std::printf("frame %d inventory %s %s home %s %s got drawn %d region %s host %s want %s %s\n", frame, it.id, w.c_str(), layout::kindName(h.kind),
              *h.region ? h.region : "-", drawn ? 1 : 0, got.c_str(), host ? host : "-", want.c_str(), pass ? "ok" : "FAIL");
  std::fflush(stdout);
  (pass ? gInv.ok : gInv.failed)++;
  if (f) *f = gInv.wasOpen;
  else openSpecial(a, w, false);
  gInv.step = 0;
  ++gInv.item;
}
}  // namespace

void devFrame(App& a, int frame) { inventoryFrame(a, frame); }

void devCommand(App& a, const std::vector<std::string>& args, int frame) {
  if (args.empty()) return;
  const std::string& c = args[0];
  std::string arg = args.size() > 1 ? args[1] : std::string();
  for (size_t i = 2; i < args.size(); ++i) arg += " " + args[i];  // window names have spaces ("Piano roll")
  if (c == "dump") {
    std::printf("LAYOUT BEGIN frame %d\n%sLAYOUT END\n", frame, shellReport(a).c_str());
  } else if (c == "show" || c == "hide") {
    setRegionVisible(a, arg, c == "show");
  } else if (c == "open" || c == "close") {
    if (bool* f = windowOpenFlag(a, arg)) *f = c == "open";
    else openSpecial(a, arg, c == "open");  // the instrument and effect windows (a selection, not a flag)
  } else if (c == "select") {  // a selection to keep across switches: the second channel, the first two clips, the first note
    const Song& song = a.doc.song();
    if (song.channels.size() > 1) a.selChannel = song.channels[1].id;
    a.playlist.selected.clear();
    for (size_t i = 0; i < song.playlist.size() && i < 2; ++i) a.playlist.selected.insert(song.playlist[i].id);
    a.piano.selected = {0};
    a.piano.selPattern = a.curPattern;  // as a selection made in the piano roll (it keeps it for this pattern and channel)
    a.piano.selChannel = a.selChannel;
    if (song.mixer.size() > 1) a.selMixer = song.mixer[1].id;
  } else if (c == "big") {  // a big project for the switch speed check: 400 channels, 100 mixer tracks, 2000 clips
    const int channels = args.size() > 1 ? std::max(1, std::atoi(args[1].c_str())) : 400;
    for (int i = 0; i < channels; ++i) a.doc.addChannel("Big " + std::to_string(i + 1), "gl.synth");
    for (int i = 0; i < 100; ++i) a.doc.addMixerTrack();
    std::vector<ObjectId> pats;
    for (int i = 0; i < 20; ++i) pats.push_back(a.doc.addPattern("Big pattern " + std::to_string(i + 1)));
    const Tick bar = static_cast<Tick>(a.doc.song().ppq) * 4;
    for (int i = 0; i < 2000; ++i) a.doc.addClip(pats[static_cast<size_t>(i) % pats.size()], i % 100, bar * (i / 100), bar);
    std::printf("frame %d big project: %zu channels %zu mixer %zu clips\n", frame, a.doc.song().channels.size(), a.doc.song().mixer.size(),
                a.doc.song().playlist.size());
  } else if (c == "docks") {  // K-284: the main dock tree, every layout (each node's split, rectangle and windows)
    std::string out;
    auto walk = [&](auto&& self, const ImGuiDockNode* n, const std::string& path) -> void {
      if (!n) return;
      std::string ws;
      for (const ImGuiWindow* w : n->Windows) ws += std::string(ws.empty() ? "" : ",") + ui::stableWindowName(w->Name);
      char b[512];
      std::snprintf(b, sizeof b, "dock %s split %s central %d rect %.0f %.0f %.0f %.0f windows [%s]\n", path.c_str(),
                    n->IsSplitNode() ? (n->SplitAxis == ImGuiAxis_X ? "x" : "y") : "-", n->IsCentralNode() ? 1 : 0, static_cast<double>(n->Pos.x),
                    static_cast<double>(n->Pos.y), static_cast<double>(n->Size.x), static_cast<double>(n->Size.y), ws.c_str());
      out += b;
      self(self, n->ChildNodes[0], path + ".0");
      self(self, n->ChildNodes[1], path + ".1");
    };
    ImGuiWindow* host = ImGui::FindWindowByName("##dockhost");
    walk(walk, host ? ImGui::DockBuilderGetNode(host->GetID("GridlineDock")) : nullptr, "r");
    std::printf("DOCKS BEGIN frame %d layout %s\n%sDOCKS END\n", frame, layout::id(active()), out.c_str());
  } else if (c == "tabtarget" || c == "tabmove") {
    // K-237: "layout tabtarget <window>|<pane>": the "ptabsrc" and "ptabdst" sdlat targets of a scripted
    // drag (the window's tab, the pane's strip or rail); "layout tabmove <window>|<pane>": the menu's Move to.
    const size_t bar = arg.find('|');
    const std::string w = arg.substr(0, bar), to = bar == std::string::npos ? std::string() : arg.substr(bar + 1);
    if (c == "tabtarget") gDevTabSrc = w, gDevTabDst = to;
    else std::printf("frame %d tabmove %s to %s %s\n", frame, w.c_str(), to.c_str(), moveTabTo(a, w, to) ? "ok" : "refused");
  } else if (c == "tabs") {  // K-237: each pane's tabs (order, pins, active) and the moved tabs
    const Preset p = active();
    const layout::State st = regionState(a, p);
    std::map<std::string, ImGuiID> panes = gStrip;
    panes.insert(gRail.begin(), gRail.end());
    for (const auto& [region, node] : panes) {
      TabCands cand;
      const panetabs::Strip s = gRail.count(region) ? railStrip(a, p, region, st, cand) : paneStrip(a, p, region, st, cand);
      std::printf("frame %d tabs %s %s [%s]\n", frame, region.c_str(), gRail.count(region) ? "rail" : "strip", panetabs::save(s).c_str());
    }
    std::string moved;
    for (const auto& [w, r] : st.moved) moved += (moved.empty() ? "" : ",") + w + ">" + r;
    std::printf("frame %d tabs moved [%s]\n", frame, moved.c_str());
  } else if (c == "state") {
    std::printf("frame %d %s\n", frame, stateLine(a).c_str());
  } else if (c == "bodies") {  // layout bodies: last frame's hosted body draws (one host per body)
    std::printf("frame %d bodies [%s]\n", frame, hostbody::drawnReport().c_str());
  } else if (c == "snapshot") {  // layout snapshot: is a switch snapshot texture held, and its last alpha
    std::printf("frame %d snapshot held %d alpha %.2f\n", frame, snapshotHeld() ? 1 : 0, static_cast<double>(snapshotAlpha()));
  } else if (c == "reveal") {  // layout reveal <kind:id[/sub]>: shows that object in this layout
    anchor::Target t;
    const bool ok = anchor::fromText(arg, t) && reveal(a, t);
    std::printf("frame %d reveal %s %s window %s\n", frame, arg.c_str(), ok ? "ok" : "missing", ok && revealWindow(a, t.kind) ? revealWindow(a, t.kind) : "-");
  } else if (c == "inventory") {
    gInv = {};
    gInv.on = true;
  } else if (c == "where") {  // where a window is: its region (docked), floating or not drawn
    const ImGuiWindow* found = findTop(arg);
    const bool drawn = found && found->WasActive;
    const std::string region = drawn ? regionOf(found) : "-";
    std::printf("frame %d where %s layout %s drawn %d docked %d region %s rect %.0f %.0f %.0f %.0f\n", frame, arg.c_str(), layout::id(active()),
                drawn ? 1 : 0, drawn && found->DockIsActive ? 1 : 0, region.c_str(), found ? static_cast<double>(found->Pos.x) : 0.0,
                found ? static_cast<double>(found->Pos.y) : 0.0, found ? static_cast<double>(found->Size.x) : 0.0,
                found ? static_cast<double>(found->Size.y) : 0.0);
    // The centre region (Float homes open over it) and whether the window shows in the status bar.
    const ImGuiDockNode* cn = shellBuilt(a, active()) ? ImGui::DockBuilderGetNode(regionNode(layout::shell(active()).centre)) : nullptr;
    std::printf("frame %d whereextra %s centre %.0f %.0f %.0f %.0f inbar %d\n", frame, arg.c_str(), cn ? static_cast<double>(cn->Pos.x) : 0.0,
                cn ? static_cast<double>(cn->Pos.y) : 0.0, cn ? static_cast<double>(cn->Size.x) : 0.0, cn ? static_cast<double>(cn->Size.y) : 0.0,
                arg == "Info box" && layoutparts::infoInBar(a) ? 1 : 0);
  }
  std::fflush(stdout);
}

}  // namespace gl::layoutui
