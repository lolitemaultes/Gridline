// Gridline — 0.7.8: Dear ImGui ID-conflict checks for scripted runs, see IdSweep.h.
#include "IdSweep.h"

#include <algorithm>
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
  ImVec2 min, max;
  ImGuiWindow* window;
  bool allowDup;
};

ImGuiContext* mainCtx = nullptr;   // the sweep's context (plugin windows have their own)
std::vector<Item> items;           // this frame's visible items (all windows)
std::unordered_map<ImGuiID, std::string> labels;

// The counter since the start of the run.
int conflictFrames = 0;
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
  if (r.GetWidth() < 1.0f || r.GetHeight() < 1.0f) return;
  items.push_back({id, r.Min, r.Max, w, (g->LastItemData.ItemFlags & ImGuiItemFlags_AllowDuplicateId) != 0});
}

void onInfo(ImGuiID id, const char* label, int) {
  if (ImGui::GetCurrentContext() != mainCtx || id == 0 || !label) return;
  if (labels.size() < 20000) labels[id] = label;
}

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
  std::printf("idsweep done frame %d frames %d items %d conflicts %zu static %zu\n", frameNo, ImGui::GetFrameCount() - startFrame, visited, sweepConflicts.size(),
              sweepStatic.size());
  for (const auto& [name, n] : sweptWindows) std::printf("idsweep window \"%s\" items %d\n", name.c_str(), n);
  std::fflush(stdout);
  sweeping = false;
  queue.clear();
  seen.clear();
  imhook::setSweepHooks(nullptr, nullptr);
  devsig::reach("idsweep-done");  // ("waitfor idsweep-done <s>" in a script)
}

int demoFrames = 0;  // "idsweep demo": frames left of the check's own conflicting pair

}  // namespace

void drawDemo() {
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
  if (cid) {
    ++conflictFrames;
    lastConflict = cid;
    const bool fresh = conflictIds.insert(cid).second;
    if (sweeping) sweepConflicts.insert(cid);
    if (dev && fresh) {
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
  std::unordered_map<ImGuiID, int> perId;
  for (const Item& it : items) {
    if (!matches(it.window)) continue;
    if (!it.allowDup) ++perId[it.id];
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
  for (const auto& [id, n] : perId) {
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
  if (idle >= 3) stop(frameNo);
}

bool devCommand(const std::string& cmd, const std::vector<std::string>& args, int frameNo) {
  if (cmd == "idconflicts") {
    std::printf("idconflicts frame %d conflicts %d ids %zu last 0x%08X\n", frameNo, conflictFrames, conflictIds.size(), lastConflict);
    std::fflush(stdout);
    return true;
  }
  if (cmd != "idsweep") return false;
  const std::string a0 = args.empty() ? std::string() : args[0];
  if (a0 == "demo") {  // idsweep demo [frames]: a window with two buttons sharing an ID (the check checked)
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
  maxTargets = args.size() > 1 ? std::max(1, std::atoi(args[1].c_str())) : 3000;
  dwell = args.size() > 2 ? std::max(1, std::atoi(args[2].c_str())) : 1;
  dwellLeft = 0;
  mainCtx = ImGui::GetCurrentContext();
  sweeping = true;
  queue.clear();
  seen.clear();
  labels.clear();
  sweepConflicts.clear();
  sweepStatic.clear();
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
