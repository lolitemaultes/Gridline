// Gridline: a press anywhere on a window brings it to the front (see WindowRaise.h).
#include "WindowRaise.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl::ui {
namespace {
std::string lastRaised;  // dev: the window the last press brought forward
int lastRaisedFrame = -1;
ImGuiID pendingRaise = 0;  // a window pressed while a menu or popup was open: raised the frame after

void focusAndNote(ImGuiWindow* hw) {
  ImGuiContext& g = *GImGui;
  ImGui::FocusWindow(hw, ImGuiFocusRequestFlags_UnlessBelowModal);
  if (g.NavWindow == hw && hw->RootWindowDockTree) {
    lastRaised = stableWindowName(hw->RootWindowDockTree->Name);
    lastRaisedFrame = g.FrameCount;
  }
}
}  // namespace

bool focusWindowKeepPopups(ImGuiWindow* w) {
  ImGuiContext& g = *GImGui;
  if (!w) return false;
  if (g.NavWindow == w || (g.NavWindow && g.NavWindow->RootWindow == w)) return true;
  if (g.OpenPopupStack.Size > 0 || g.BeginPopupStack.Size > 0) return false;
  ImGui::FocusWindow(w, ImGuiFocusRequestFlags_UnlessBelowModal);
  return true;
}

namespace {
bool (*gKeyBorrower)(const ImGuiWindow* area) = nullptr;
ImGuiID gFrameArea = 0;  // K-376: the area the frame was last drawn round
KeyFocusFrameStats gFrameStats;

// The docked area a window with the keys belongs to: a focused child inside a docked part (an
// inspector's body, a list) frames the docked window it sits in (its RootWindow runs on to the dock
// host, which is no area).
const ImGuiWindow* dockedAreaOf(const ImGuiWindow* w) {
  while (w && !w->DockIsActive && (w->Flags & ImGuiWindowFlags_ChildWindow) && w->ParentWindow) w = w->ParentWindow;
  return w;
}

// Whether the area can carry the frame. K-376: a page just brought to the front of its dock node is
// hidden for its first frames (ImGui measures it) while the node it fills is on screen: it counts.
bool frameable(const ImGuiWindow* area) {
  if (!area || !area->DockIsActive || area->Size.x < 160 || area->Size.y < 120 || (area->Flags & ImGuiWindowFlags_NoInputs)) return false;
  if (!area->Hidden) return true;
  const ImGuiDockNode* n = area->DockNode;
  return n && n->IsVisible && n->Size.x >= 160 && n->Size.y >= 120;
}

void noteFrame(const ImGuiWindow* area) {
  if (!area) {
    ++gFrameStats.missed;
    gFrameStats.area.clear();
    return;
  }
  ++gFrameStats.drawn;
  if (gFrameArea != 0 && gFrameArea != area->ID) ++gFrameStats.moves;
  gFrameArea = area->ID;
  gFrameStats.area = stableWindowName(area->Name);
}
}  // namespace

void setKeyFocusBorrower(bool (*borrows)(const ImGuiWindow* area)) { gKeyBorrower = borrows; }
KeyFocusFrameStats& keyFocusFrameStats() { return gFrameStats; }

void drawKeyFocusFrame(unsigned int colour) {
  ImGuiContext& g = *GImGui;
  if (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
    ++gFrameStats.popups;  // (a menu or pop-up open: no frame, by design)
    return;
  }
  const ImGuiWindow* area = dockedAreaOf(g.NavWindow);
  // K-376: an area that only borrows the keys (the Tracker's header band, which hands them back to
  // the pattern once a click on it is over) leaves the frame where it was. Before, every click on a
  // band button moved the frame up to the band for the click's frames and back: it flickered.
  if (area && gKeyBorrower && gKeyBorrower(area)) {
    const ImGuiWindow* before = gFrameArea ? ImGui::FindWindowByID(gFrameArea) : nullptr;
    if (before && before != area && before->WasActive && frameable(before)) area = before;
  }
  if (!frameable(area)) {
    noteFrame(nullptr);
    return;
  }
  noteFrame(area);
  // (A hidden page on its first frames: its dock node's rectangle, the one it fills.)
  const bool byNode = area->Hidden && area->DockNode;
  const ImVec2 pos = byNode ? area->DockNode->Pos : area->Pos, size = byNode ? area->DockNode->Size : area->Size;
  const ImVec2 p0(std::round(pos.x) + 0.5f, std::round(pos.y) + 0.5f);
  const ImVec2 p1(std::round(pos.x + size.x) - 0.5f, std::round(pos.y + size.y) - 0.5f);
  const ImU32 col = colour ? static_cast<ImU32>(colour) : theme::withAlpha(theme::pal().accent, 0.55f);
  // K-112c checkpoint 55 (N54-05): the frame is drawn over the area's own children (foreground), but not
  // through a window in front of the area (a floating instrument window): its edges skip those windows.
  const ImGuiWindow* root = area->RootWindowDockTree ? area->RootWindowDockTree : area->RootWindow;
  ImVector<ImRect> front;
  bool past = false;
  for (ImGuiWindow* w : g.Windows) {
    if (w == root) {
      past = true;
      continue;
    }
    if (!past || !w->WasActive || w->Hidden || (w->Flags & ImGuiWindowFlags_ChildWindow) || w->Viewport != area->Viewport) {
      continue;
    }
    if ((w->RootWindowDockTree ? w->RootWindowDockTree : w->RootWindow) == root) {
      continue;
    }
    if (w->Flags & (ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_NoBackground)) {
      continue;  // (a tooltip or an overlay's input window hides nothing)
    }
    front.push_back(w->Rect());
  }
  ImDrawList* dl = ImGui::GetForegroundDrawList(area->Viewport);
  // One edge from a to b (horizontal when a.y == b.y), less the parts under the front windows.
  auto edge = [&](ImVec2 a, ImVec2 b) {
    const bool hz = a.y == b.y;
    const float lo = hz ? a.x : a.y, hi = hz ? b.x : b.y, at = hz ? a.y : a.x;
    ImVector<ImVec2> cuts;  // covered spans
    for (const ImRect& r : front) {
      const float c0 = hz ? r.Min.y : r.Min.x, c1 = hz ? r.Max.y : r.Max.x;
      if (at < c0 || at > c1) {
        continue;
      }
      const float s0 = std::max(lo, hz ? r.Min.x : r.Min.y), s1 = std::min(hi, hz ? r.Max.x : r.Max.y);
      if (s1 > s0) {
        cuts.push_back(ImVec2(s0, s1));
      }
    }
    float from = lo;
    while (from < hi) {
      float skip = from;
      for (const ImVec2& c : cuts) {
        if (c.x <= from && c.y > from) {
          skip = std::max(skip, c.y);  // under a window: on past it
        }
      }
      if (skip > from) {
        from = skip;
        continue;
      }
      float to = hi;
      for (const ImVec2& c : cuts) {
        if (c.x > from && c.x < to) {
          to = c.x;
        }
      }
      dl->AddLine(hz ? ImVec2(from, at) : ImVec2(at, from), hz ? ImVec2(to, at) : ImVec2(at, to), col, 1.0f);
      from = to;
    }
  };
  edge(p0, ImVec2(p1.x, p0.y));
  edge(ImVec2(p0.x, p1.y), p1);
  edge(p0, ImVec2(p0.x, p1.y));
  edge(ImVec2(p1.x, p0.y), p1);
}

void raiseOnPress() {
  ImGuiContext& g = *GImGui;
  const ImGuiIO& io = g.IO;
  // A press outside an open menu or popup only closes it (ImGui closes it at the end of that frame):
  // focusing the window under it in the same frame would let that window's button take the press
  // too (one click both closing the Tools menu and tapping the tempo). The raise waits a frame,
  // when the press is no longer new.
  if (pendingRaise) {
    ImGuiWindow* w = ImGui::FindWindowByID(pendingRaise);
    pendingRaise = 0;
    if (w && w->WasActive && g.OpenPopupStack.Size == 0 && g.NavWindow != w) focusAndNote(w);
  }
  bool pressed = false, heldBefore = false;
  for (int b = 0; b < 3; ++b) {
    pressed = pressed || io.MouseClicked[b];
    heldBefore = heldBefore || (io.MouseDown[b] && !io.MouseClicked[b]);
  }
  if (!pressed || heldBefore) return;  // no new press, or a drag with another button is under way
  if (g.DragDropActive || g.MovingWindow) return;
  ImGuiWindow* hw = g.HoveredWindow;
  if (!hw || !hw->RootWindow || !hw->RootWindowDockTree) return;
  if (hw->RootWindow->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_Tooltip)) return;  // menus handle their own presses
  ImGuiWindow* front = hw->RootWindowDockTree;
  if (g.OpenPopupStack.Size > 0) {  // the press closes the open menu or popup; raise next frame
    pendingRaise = hw->ID;
    return;
  }
  if (g.NavWindow == hw) {  // already focused: only make sure it is drawn in front
    if (!((hw->Flags | hw->RootWindow->Flags | front->Flags) & ImGuiWindowFlags_NoBringToFrontOnFocus)) ImGui::BringWindowToDisplayFront(front);
    return;
  }
  // A window under a modal dialog is not focused (FocusWindow keeps the dialog above it).
  focusAndNote(hw);
}

void devDumpZOrder(int frame) {
  const ImGuiContext& g = *GImGui;
  std::string order;
  for (const ImGuiWindow* w : g.Windows) {
    if (!w->WasActive || w->Hidden || (w->Flags & (ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_Tooltip))) continue;
    if (w->DockIsActive || (w->Flags & ImGuiWindowFlags_NoBringToFrontOnFocus)) continue;  // docked panels, the dock host, bars
    if (std::string(stableWindowName(w->Name)).rfind("##", 0) == 0) continue;               // overlays (notifications)
    if (!order.empty()) order += " | ";
    order += stableWindowName(w->Name);
  }
  const ImGuiWindow* nav = g.NavWindow;
  std::printf("zorder frame %d: front-last [%s] focused \"%s\" focused-root \"%s\" raised \"%s\" at %d\n", frame, order.c_str(),
              nav ? stableWindowName(nav->Name) : "", nav && nav->RootWindowDockTree ? stableWindowName(nav->RootWindowDockTree->Name) : "",
              lastRaised.c_str(), lastRaisedFrame);
  std::fflush(stdout);
}

bool bringTabForward(const char* name) {
  ImGuiWindow* w = name ? ImGui::FindWindowByName(name) : nullptr;
  ImGuiDockNode* n = w && w->DockIsActive ? w->DockNode : nullptr;
  if (!n || !n->VisibleWindow || n->VisibleWindow == w) return false;
  if (n->TabBar) n->TabBar->NextSelectedTabId = w->TabId;
  ImGui::FocusWindow(w);
  return true;
}

}  // namespace gl::ui
