// Gridline: a press anywhere on a window brings it to the front (see WindowRaise.h).
#include "WindowRaise.h"
#include <cstdio>
#include <string>
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
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

}  // namespace gl::ui
