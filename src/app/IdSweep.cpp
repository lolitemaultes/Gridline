// Gridline — 0.7.8: Dear ImGui ID-conflict checks for scripted runs, see IdSweep.h.
#include "IdSweep.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "DevSignal.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_hooks/ImGuiItemHook.h"

namespace gl::idsweep {
namespace {

struct Item {
  ImGuiID id;
  ImVec2 min, max;  // (clipped to the window when visible; else its whole rectangle)
  ImGuiWindow* window;
  bool allowDup;
  bool visible;  // (inside the window's clip rectangle: the pointer can reach it)
};

ImGuiContext* mainCtx = nullptr;   // the sweep's context (plugin windows have their own)
std::vector<Item> items;           // this frame's items (all windows; clipped ones too, for the doubled-ID check)
std::unordered_set<ImGuiID> openable;  // this frame's items that open something when hovered (submenus, tree nodes)
std::unordered_map<ImGuiID, std::string> labels;

// The counter since the start of the run.
int conflictFrames = 0;
int popupFrames = 0;  // frames with Dear ImGui's own popup on screen (io.ConfigDebugHighlightIdConflicts)
std::unordered_set<ImGuiID> conflictIds;
ImGuiID lastConflict = 0;

// The sweep.
bool sweeping = false;
std::vector<std::string> filters;
int maxTargets = 3000;
int dwell = 1, dwellLeft = 0;  // frames on each item
std::deque<ImVec2> queue;
std::set<std::pair<ImGuiID, long long>> seen;  // (id, rounded centre): each item once
int visited = 0, idle = 0, startFrame = 0;
bool scanOnly = false;  // K-459: "scan": no pointer over every item, only over what opens (submenus); the doubled-ID check does the rest
std::unordered_set<ImGuiID> sweepImplicit;  // items drawn into the implicit window during the sweep
std::unordered_set<ImGuiID> checkedIds;     // the items of the swept windows the doubled-ID check saw
std::unordered_set<ImGuiID> sweepConflicts, sweepStatic;
std::map<std::string, int> sweptWindows;  // the items queued, by window
std::unordered_map<ImGuiID, int> scrolled;  // the times a swept window was scrolled on

void onItem(ImGuiID id, ImVec2 mn, ImVec2 mx) {
  ImGuiContext* g = ImGui::GetCurrentContext();
  if (g != mainCtx || id == 0) return;
  ImGuiWindow* w = g->CurrentWindow;
  if (!w || id == w->ID) return;
  ImRect r(mn, mx);
  r.ClipWith(w->ClipRect);
  const bool visible = r.GetWidth() >= 1.0f && r.GetHeight() >= 1.0f;
  if (!visible) r = ImRect(mn, mx);
  items.push_back({id, r.Min, r.Max, w, (g->LastItemData.ItemFlags & ImGuiItemFlags_AllowDuplicateId) != 0, visible});
}

void onInfo(ImGuiID id, const char* label, int flags) {
  if (ImGui::GetCurrentContext() != mainCtx || id == 0) return;
  if (flags & ImGuiItemStatusFlags_Openable) openable.insert(id);
  if (label && labels.size() < 20000) labels[id] = label;
}

// K-459: Dear ImGui's implicit window ("Debug##Default"): Dear ImGui begins it every frame (its title
// bar adds two items) and hides it at the end of the frame unless something was drawn into it, outside
// any Begin / End; then it shows as a "Debug" window.
bool implicitWindow(const ImGuiWindow* w) { return w && w->IsFallbackWindow; }

const char* windowName(const ImGuiWindow* w) { return w && w->Name ? w->Name : "?"; }

bool matches(const ImGuiWindow* w) {
  if (!w || (w->Flags & (ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_NoMouseInputs))) return false;
  for (const std::string& f : filters)
    if (f == "*") return true;
  for (const ImGuiWindow* p = w; p; p = p->ParentWindow ? p->ParentWindow : p->ParentWindowForFocusRoute)
    for (const std::string& f : filters)
      if (p->Name && std::strstr(p->Name, f.c_str())) return true;
  return false;
}

std::string labelOf(ImGuiID id) {
  auto it = labels.find(id);
  return it == labels.end() ? std::string() : it->second;
}

// The items of this frame with `id`: where they are and in which window.
void printItems(ImGuiID id) {
  int n = 0;
  for (const Item& it : items) {
    if (it.id != id) continue;
    if (++n > 6) break;
    std::printf("idsweep   item %.0f,%.0f %.0fx%.0f window \"%s\"\n", static_cast<double>(it.min.x), static_cast<double>(it.min.y),
                static_cast<double>(it.max.x - it.min.x), static_cast<double>(it.max.y - it.min.y), windowName(it.window));
  }
}

void stop(int frameNo) {
  std::printf("idsweep done frame %d frames %d items %d conflicts %zu static %zu implicit %zu checked %zu\n", frameNo, ImGui::GetFrameCount() - startFrame,
              visited, sweepConflicts.size(), sweepStatic.size(), sweepImplicit.size(), checkedIds.size());
  for (const auto& [name, n] : sweptWindows) std::printf("idsweep window \"%s\" items %d\n", name.c_str(), n);
  std::fflush(stdout);
  sweeping = false;
  queue.clear();
  seen.clear();
  imhook::setSweepHooks(nullptr, nullptr);
  devsig::reach("idsweep-done");  // ("waitfor idsweep-done <s>" in a script)
}

int demoFrames = 0;  // "idsweep demo": frames left of the check's own conflicting pair
int strayFrames = 0;  // "idsweep demo stray": frames left of a button drawn outside any window

bool highlightOn = false;  // K-459: io.ConfigDebugHighlightIdConflicts of every context (off for users)

}  // namespace

void setHighlight(bool on) { highlightOn = on; }
bool highlight() { return highlightOn; }
bool highlightFor(bool automated) {
  // GRIDLINE_DEV_IDS=0: off (a scripted run that checks what users get), any other value: on.
  const char* env = std::getenv("GRIDLINE_DEV_IDS");
  if (env && *env) return std::strcmp(env, "0") != 0;
#ifndef NDEBUG
  (void)automated;
  return true;  // development builds
#else
  return automated;
#endif
}

void drawDemo() {
  if (strayFrames > 0) {  // K-459: the implicit-window check's own check (a button outside any Begin / End)
    --strayFrames;
    static const char* const kStray = "stray##k459";
    ImGui::Button(kStray);
  }
  if (demoFrames <= 0) return;
  --demoFrames;
  ImGui::SetNextWindowPos(ImVec2(40, 40), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(220, 90), ImGuiCond_Always);
  // A dev-only check window: an ID-only name and no title bar (like the dev menu tree), and its
  // label held in a constant, so it never reaches the translation catalogue.
  static const char* const kTwin = "twin##same";
  if (ImGui::Begin("##idsweep demo", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar)) {
    ImGui::Button(kTwin, ImVec2(80, 24));  // two items, one ID: what Dear ImGui reports
    ImGui::SameLine();
    ImGui::Button(kTwin, ImVec2(80, 24));
  }
  ImGui::End();
}

void frame(ImGuiIO& io, int) {
  items.clear();
  openable.clear();
  if (!sweeping || queue.empty() || --dwellLeft > 0) return;
  dwellLeft = dwell;
  const ImVec2 p = queue.front();
  queue.pop_front();
  ++visited;
  io.AddMousePosEvent(p.x, p.y);
}

void afterRender(bool dev, int frameNo) {
  ImGuiContext* gp = ImGui::GetCurrentContext();
  if (!gp) return;
  ImGuiContext& g = *gp;
  // Dear ImGui's own finding (the tooltip's condition); without its highlight option, the count
  // it bases it on (items with last frame's hovered ID this frame).
  ImGuiID cid = g.DebugDrawIdConflictsId;
  int count = g.DebugDrawIdConflictsCount;
  if (!cid && g.HoveredIdPreviousFrame && g.HoveredIdPreviousFrameItemCount > 1) cid = g.HoveredIdPreviousFrame, count = g.HoveredIdPreviousFrameItemCount;
  if (g.DebugDrawIdConflictsId != 0) ++popupFrames;  // K-459: Dear ImGui's popup and outlines on screen this frame
  if (cid) {
    ++conflictFrames;
    lastConflict = cid;
    const bool fresh = conflictIds.insert(cid).second;
    const bool freshInSweep = sweeping && sweepConflicts.insert(cid).second;  // K-459: named again in each sweep that finds it
    if (dev && (fresh || freshInSweep)) {
      int drawn = 0;  // (items are collected while sweeping)
      for (const Item& it : items) drawn += it.id == cid ? 1 : 0;
      std::printf("idconflict frame %d id 0x%08X tooltip-count %d drawn %d hovered-window \"%s\" mouse %.0f,%.0f label \"%s\"\n", frameNo, cid, count, drawn,
                  windowName(g.HoveredWindow), static_cast<double>(g.IO.MousePos.x), static_cast<double>(g.IO.MousePos.y), labelOf(cid).c_str());
      printItems(cid);
      std::fflush(stdout);
    }
  }
  if (!sweeping) return;
  // New items of the swept windows join the queue (menus and popups the sweep opens too).
  // (found after the first frame, a menu or popup the pointer opened: visited next, while it is open)
  int added = 0;
  std::vector<ImVec2> found;
  // (still active after the frame ended: something wrote into it, and users see it)
  const ImGuiWindow* fallback = ImGui::FindWindowByName("Debug##Default");
  const bool fallbackShown = fallback && fallback->IsFallbackWindow && fallback->Active;
  // (one ID twice at the same place, within 2 px, is one widget: a multi-line text field submits its
  // ID for the field and again for its scrolling child window; Dear ImGui does not count that as a conflict)
  std::unordered_map<ImGuiID, std::vector<ImRect>> perId;
  for (const Item& it : items) {
    if (fallbackShown && implicitWindow(it.window) && sweepImplicit.insert(it.id).second) {  // K-459: drawn outside any window
      std::printf("idsweep implicit frame %d id 0x%08X label \"%s\" at %.0f,%.0f %.0fx%.0f\n", frameNo, it.id, labelOf(it.id).c_str(), static_cast<double>(it.min.x),
                  static_cast<double>(it.min.y), static_cast<double>(it.max.x - it.min.x), static_cast<double>(it.max.y - it.min.y));
    }
    if (!matches(it.window)) continue;
    checkedIds.insert(it.id);
    if (!it.allowDup) {
      std::vector<ImRect>& places = perId[it.id];
      const auto same = [&](const ImRect& r) {
        return std::fabs(r.Min.x - it.min.x) <= 2.0f && std::fabs(r.Min.y - it.min.y) <= 2.0f && std::fabs(r.Max.x - it.max.x) <= 2.0f &&
               std::fabs(r.Max.y - it.max.y) <= 2.0f;
      };
      if (std::none_of(places.begin(), places.end(), same)) places.push_back(ImRect(it.min, it.max));
    }
    if (!it.visible || (scanOnly && !openable.count(it.id))) continue;  // (the pointer: visible items; scanning, only those that open something)
    const ImVec2 c((it.min.x + it.max.x) * 0.5f, (it.min.y + it.max.y) * 0.5f);
    const long long key = (static_cast<long long>(c.x / 3.0f) << 20) ^ static_cast<long long>(c.y / 3.0f);
    if (static_cast<int>(seen.size()) >= maxTargets || !seen.insert({it.id, key}).second) continue;
    found.push_back(c);
    ++sweptWindows[windowName(it.window)];
    ++added;
  }
  if (visited == 0) queue.insert(queue.end(), found.begin(), found.end());
  else queue.insert(queue.begin(), found.begin(), found.end());
  // Without hovering: one ID drawn twice in the swept windows this frame (for the report).
  for (const auto& [id, places] : perId) {
    const int n = static_cast<int>(places.size());
    if (n < 2 || !sweepStatic.insert(id).second) continue;
    std::printf("idsweep static frame %d id 0x%08X items %d label \"%s\"\n", frameNo, id, n, labelOf(id).c_str());
    printItems(id);
  }
  std::fflush(stdout);
  // Swept, the windows that scroll show their next part (a window at most 40 times), then that is swept.
  if (queue.empty() && added == 0 && idle == 2) {
    std::set<ImGuiWindow*> wins;
    for (const Item& it : items)
      if (matches(it.window)) wins.insert(it.window);
    for (ImGuiWindow* w : wins) {
      if (w->ScrollMax.y <= 0.0f || w->Scroll.y >= w->ScrollMax.y - 0.5f || ++scrolled[w->ID] > 40) continue;
      ImGui::SetScrollY(w, std::min(w->ScrollMax.y, w->Scroll.y + std::max(20.0f, w->InnerRect.GetHeight() * 0.75f)));
      idle = -2;  // (the scroll applies next frame)
    }
  }
  idle = queue.empty() && added == 0 ? idle + 1 : 0;
  // (K-459: a window that has no items yet, as one opening or re-laid out, gets 30 frames to show them)
  if (idle >= 3 && (visited > 0 || !checkedIds.empty() || ImGui::GetFrameCount() - startFrame >= 30)) stop(frameNo);
}

bool devCommand(const std::string& cmd, const std::vector<std::string>& args, int frameNo) {
  if (cmd == "idconflicts") {
    std::printf("idconflicts frame %d conflicts %d ids %zu last 0x%08X popup-frames %d\n", frameNo, conflictFrames, conflictIds.size(), lastConflict,
                popupFrames);
    std::fflush(stdout);
    return true;
  }
  if (cmd != "idsweep") return false;
  const std::string a0 = args.empty() ? std::string() : args[0];
  if (a0 == "demo") {  // idsweep demo [frames]: a window with two buttons sharing an ID (the check checked)
    if (args.size() > 1 && args[1] == "stray") {  // idsweep demo stray [frames]: a button drawn outside any window
      strayFrames = args.size() > 2 ? std::atoi(args[2].c_str()) : 600;
      return true;
    }
    demoFrames = args.size() > 1 ? std::atoi(args[1].c_str()) : 600;
    return true;
  }
  if (a0 == "stop") {
    if (sweeping) stop(frameNo);
    return true;
  }
  if (a0.empty()) return true;
  filters.clear();
  for (size_t at = 0;;) {
    const size_t bar = a0.find('|', at);
    filters.push_back(a0.substr(at, bar == std::string::npos ? std::string::npos : bar - at));
    if (bar == std::string::npos) break;
    at = bar + 1;
  }
  // idsweep <filter> scan [dwell]: the doubled-ID check of every item, the pointer only over what opens
  scanOnly = args.size() > 1 && args[1] == "scan";
  maxTargets = args.size() > 1 && !scanOnly ? std::max(1, std::atoi(args[1].c_str())) : 3000;
  dwell = scanOnly ? (args.size() > 2 ? std::max(1, std::atoi(args[2].c_str())) : 4) : args.size() > 2 ? std::max(1, std::atoi(args[2].c_str())) : 1;
  dwellLeft = 0;
  mainCtx = ImGui::GetCurrentContext();
  sweeping = true;
  queue.clear();
  seen.clear();
  labels.clear();
  sweepConflicts.clear();
  sweepStatic.clear();
  sweepImplicit.clear();
  checkedIds.clear();
  sweptWindows.clear();
  scrolled.clear();
  visited = 0, idle = 0, startFrame = ImGui::GetFrameCount();
  imhook::setSweepHooks(&onItem, &onInfo);
  devsig::forget("idsweep-done");
  std::printf("idsweep start frame %d filter \"%s\"\n", frameNo, a0.c_str());
  std::fflush(stdout);
  return true;
}

}  // namespace gl::idsweep
