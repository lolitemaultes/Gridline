// Gridline - the region shell in the interface (K-112). See LayoutShellUi.h.
#include "LayoutShellUi.h"
#include <SDL_opengl.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/I18n.h"
#include "../core/LayoutHomes.h"
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
#include "AutomationEditor.h"
#include "LayoutPresets.h"
#include "LayoutStyleUi.h"
#include "Theme.h"
#include "Widgets.h"

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
void setDevLayouts(bool on) { gDevLayouts = on; }
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
Preset gBuiltFor = Preset::Count;
std::set<std::string> gClosedByGroup;  // windows an exclusive region closed (open for the person)
}  // namespace

layout::State regionState(const App& a, Preset p) {
  const int k = static_cast<int>(p);
  if (hasDevOverride() && gRunStateSet[k]) return gRunState[k];
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
  return parsed[k];
}

void setRegionState(App& a, Preset p, const layout::State& st) {
  const int k = static_cast<int>(p);
  if (hasDevOverride()) {
    gRunState[k] = st;
    gRunStateSet[k] = true;
    return;
  }
  a.settings.set(layout::stateKey(p), layout::toJson(st));
}

void setRegionVisible(App& a, const std::string& region, bool on) {
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
  if (layout::visible(layout::shell(p), st, region) != on) {
    layout::setVisible(layout::shell(p), st, region, on);
    setRegionState(a, p, st);
  }
  // A hidden region's windows close with it; a shown one opens its default windows.
  for (const layout::Item& it : layout::items()) {
    const layout::Home& h = it.home[static_cast<int>(p)];
    if (!*it.window || region != h.region) continue;
    if (bool* f = windowOpenFlag(a, it.window)) {
      if (!on) *f = false;
      else if (h.openByDefault) *f = true;
    }
  }
  gRebuild = true;
}

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
  ImGui::DockBuilderRemoveNode(dockspace);
  ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
  ImGui::DockBuilderSetNodeSize(dockspace, size);
  gNodes.clear();
  gOuter.clear();
  gStrip.clear();
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
      if (r.tabs == layout::TabStyle::Segmented || (r.tabs == layout::TabStyle::None && p == Preset::Logic))
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
    const layout::Home& h = it.home[static_cast<int>(p)];
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
  gBuiltFor = p;
}

void showShellWindows(App& a, Preset p) {
  const layout::Shell& sh = layout::shell(p);
  const layout::State st = regionState(a, p);
  for (const layout::Item& it : layout::items()) {
    if (!*it.window) continue;
    bool* f = windowOpenFlag(a, it.window);
    if (!f) continue;
    const layout::Home& h = it.home[static_cast<int>(p)];
    const bool homed = (h.kind == HomeKind::Dock || h.kind == HomeKind::Part) && *h.region;
    if (!homed) continue;  // floating windows and dialogs keep their state
    *f = h.openByDefault && layout::visible(sh, st, h.region);
  }
  // Exclusive regions open with the member the person last chose.
  for (const layout::Region& r : sh.regions) {
    if (!r.exclusive || !st.member.count(r.id) || !layout::visible(sh, st, r.id)) continue;
    if (bool* f = windowOpenFlag(a, st.member.at(r.id))) *f = true;
  }
}

void leaveLayout(App& a) {
  // The windows an exclusive region only hid come back: they were open when the person left.
  for (const std::string& w : gClosedByGroup)
    if (bool* f = windowOpenFlag(a, w)) *f = true;
  gClosedByGroup.clear();
}

namespace {
struct Boost {
  float px = 0;
  int frame = -10;
};
std::map<std::string, Boost>& boosts() {
  static std::map<std::string, Boost> m;
  return m;
}
}  // namespace

void setBandBoost(const std::string& region, float px) { boosts()[region] = Boost{std::max(0.0f, px), ImGui::GetFrameCount()}; }

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
    {"Smart controls", i18n::N_("Smart controls"), false},
};

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
        struct Seg {
          const StripName* n;
          bool* f;
          float w;
        };
        std::vector<Seg> segs;
        float total = 0;
        for (const StripName& n : kStripNames) {
          if (region != layout::windowHome(n.window, p).region) continue;
          bool* f = windowOpenFlag(a, n.window);
          if (!f || (!n.always && !*f)) continue;
          const float w = std::ceil(a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, i18n::tr(n.label)).x) + 22;
          if (total + w > ws.x - theme::kS3 * 2) break;
          segs.push_back({&n, f, w});
          total += w;
        }
        const float h = std::min(22.0f, ws.y - 6), y0 = std::round(wp.y + (ws.y - 1 - h) * 0.5f);
        float sx = std::round(wp.x + (ws.x - total) * 0.5f);
        if (!segs.empty()) dl->AddRectFilled(ImVec2(sx - 2, y0 - 2), ImVec2(sx + total + 2, y0 + h + 2), P.surface900, 7.0f);
        for (const Seg& sg : segs) {
          ImGui::SetCursorScreenPos(ImVec2(sx, y0));
          ImGui::PushID(sg.n->window);
          const bool click = ImGui::InvisibleButton("##t", ImVec2(sg.w, h));
          const bool hov = ImGui::IsItemHovered();
          ImGui::PopID();
          const bool on = *sg.f;
          if (on) dl->AddRectFilled(ImVec2(sx, y0), ImVec2(sx + sg.w, y0 + h), P.surface600, 5.0f);
          else if (hov) dl->AddRectFilled(ImVec2(sx, y0), ImVec2(sx + sg.w, y0 + h), theme::withAlpha(P.surface600, 0.5f), 5.0f);
          const char* label = i18n::tr(sg.n->label);
          const float tw = a.fontRegular->CalcTextSizeA(fs, 1e9f, 0, label).x;
          dl->AddText(a.fontRegular, fs, ImVec2(std::round(sx + (sg.w - tw) * 0.5f), std::round(y0 + (h - fs) * 0.5f)), on || hov ? P.text : P.textDim, label);
          if (click && !on) *sg.f = true;  // the region's one-at-a-time rule closes the one shown
          sx += sg.w;
        }
        x = sx;
      }
      for (const StripName& n : kStripNames) {
        if (p == Preset::Logic) break;
        const layout::Home h = layout::windowHome(n.window, p);
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

void shellFrame(App& a) {
  const Preset p = active();
  if (!shellBuilt(a, p) || gBuiltFor != p) return;
  const layout::Shell& sh = layout::shell(p);
  layout::State st = regionState(a, p);
  // A window opened while its region is hidden (View > Panels, a command) shows the region.
  for (const layout::Item& it : layout::items()) {
    const layout::Home& h = it.home[static_cast<int>(p)];
    if (!*it.window || (h.kind != HomeKind::Dock && h.kind != HomeKind::Part) || !*h.region) continue;
    bool* f = windowOpenFlag(a, it.window);
    if (f && *f && layout::hasRegion(p, h.region) && !layout::visible(sh, st, h.region)) {
      setRegionVisible(a, h.region, true);
      *f = true;
      return;  // rebuilt next frame
    }
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
  for (const layout::Item& it : layout::items()) {
    const layout::Home& h = it.home[static_cast<int>(p)];
    if (!*it.window || (h.kind != HomeKind::Dock && h.kind != HomeKind::Part) || !*h.region) continue;
    bool* f = windowOpenFlag(a, it.window);
    if (!f) continue;
    bool& before = wasOpen[it.window];
    if (*f && !before && !fresh)
      if (const ImGuiID node = regionNode(h.region))
        if (ImGui::DockBuilderGetNode(node)) ImGui::DockBuilderDockWindow(it.window, node);
    before = *f;
  }
  bool changed = false;
  static std::map<std::string, bool> prevOpen;  // "<region>|<window>" -> open last frame
  for (const layout::Region& r : sh.regions) {
    const ImGuiID node = outerNode(r.id);
    ImGuiDockNode* dn = node ? ImGui::DockBuilderGetNode(node) : nullptr;
    const bool hz = r.edge == layout::Edge::Left || r.edge == layout::Edge::Right;
    if (r.exclusive && dn) {
      // The members: windows homed here. One opened this frame closes the others.
      std::vector<std::pair<const char*, bool*>> members;
      for (const layout::Item& it : layout::items()) {
        const layout::Home& h = it.home[static_cast<int>(p)];
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
    // Fixed bands keep their pixel size (the refit after a window resize scales side panels).
    if (r.fixed && dn && r.size.px > 0 && dn->ParentNode) {
      const float parent = hz ? dn->ParentNode->Size.x : dn->ParentNode->Size.y;
      float want = layout::regionExtent(r, parent, st);
      if (const auto b = boosts().find(r.id); b != boosts().end() && b->second.px > 0 && ImGui::GetFrameCount() - b->second.frame <= 1)
        want = std::max(want, std::min(want + b->second.px, std::floor(parent * 0.6f)));
      float& v = hz ? dn->SizeRef.x : dn->SizeRef.y;
      if (want > 0 && std::fabs(v - want) > 0.5f) v = want;
      // K-112c (round 22): a sibling without the central node shares the parent by the ratio of the
      // two sizes, which rounded the band a few pixels off its size (the info corner stood 4 to 6 px
      // taller than the device band beside it). The sibling takes exactly what is left, so the band
      // is its pixel size and bands of the same size line up across the window.
      ImGuiDockNode* par = dn->ParentNode;
      ImGuiDockNode* sib = par->ChildNodes[0] == dn ? par->ChildNodes[1] : par->ChildNodes[0];
      auto holdsCentre = [](auto&& self, const ImGuiDockNode* n) -> bool {
        return n && (n->IsCentralNode() || self(self, n->ChildNodes[0]) || self(self, n->ChildNodes[1]));
      };
      if (want > 0 && sib && !holdsCentre(holdsCentre, sib)) {
        const float rest = std::max(1.0f, parent - ImGui::GetStyle().DockingSeparatorSize - want);
        float& sv = hz ? sib->SizeRef.x : sib->SizeRef.y;
        if (std::fabs(sv - rest) > 0.5f) sv = rest;
      }
    }
  }
  // Title strips keep their height.
  for (const auto& [region, strip] : gStrip)
    if (ImGuiDockNode* n = ImGui::DockBuilderGetNode(strip)) {
      // A locked size: the split would otherwise give each side at least the minimum window height.
      // (Every frame: the lock lasts one layout pass.)
      n->Size.y = n->SizeRef.y = layout::kPaneStripH;
      n->WantLockSizeOnce = true;
    }
  if (changed) setRegionState(a, p, st);
  drawPaneStrips(a);
}

int dockFlags(const char* windowName, int base) {
  const Preset p = active();
  if (p == Preset::Studio) return base;
  const layout::Home h = layout::windowHome(ui::stableWindowName(windowName), p);
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
  if (!shellBuilt(a, p) || layout::windowHome(name, p).kind != HomeKind::Float) return false;
  const ImGuiDockNode* n = ImGui::DockBuilderGetNode(regionNode(layout::shell(p).centre));
  if (!n || n->Size.x <= 0 || n->Size.y <= 0) return false;
  const float in = theme::kS3, top = theme::kToolbarH + ImGui::GetFrameHeight() + in;  // the centre's tab row and toolbar stay clear
  const ImVec2 room(n->Size.x - 2 * in, n->Size.y - top - in);
  if (room.x < 160 || room.y < 80) return false;
  gFloatWant[name] = want;
  // The open floating parts of this layout, in table order: right to left along the lower edge.
  std::vector<std::string> open;
  for (const layout::Item& it : layout::items())
    if (std::strncmp(it.id, "N", 1) == 0 && *it.window && !*it.later && it.home[static_cast<int>(p)].kind == HomeKind::Float)
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
    char b[256];
    std::snprintf(b, sizeof b, "region %s visible %d node %d rect %.0f %.0f %.0f %.0f windows [%s]\n", id.c_str(),
                  layout::visible(sh, st, id) ? 1 : 0, dn && dn->IsVisible ? 1 : 0, dn ? static_cast<double>(dn->Pos.x) : 0.0,
                  dn ? static_cast<double>(dn->Pos.y) : 0.0, dn ? static_cast<double>(dn->Size.x) : 0.0, dn ? static_cast<double>(dn->Size.y) : 0.0,
                  ws.c_str());
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
      const layout::Home h = layout::windowHome(w, p);
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
    const layout::Home& h = it.home[static_cast<int>(p)];
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
  const layout::Home& h = it.home[static_cast<int>(p)];
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
  const layout::Home& h = it.home[static_cast<int>(p)];
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
  const layout::Home& h = it.home[static_cast<int>(p)];
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
