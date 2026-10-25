#include "DialogLayout.h"
#include <algorithm>
#include <cmath>
#include <cfloat>
#include <cstring>
#include <iterator>
#include <unordered_map>
#include <vector>
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/WindowPlacement.h"
#include "../core/WindowRubber.h"
#include "Theme.h"

namespace gl::ui {

float DialogFooterHeight(int rows) {
  rows = std::max(1, rows);
  const float sp = ImGui::GetStyle().ItemSpacing.y;
  return static_cast<float>(rows) * ImGui::GetFrameHeight() + 2 * theme::kS1 + static_cast<float>(rows + 2) * sp;
}

void DialogFooter(float buttonsWidth) {
  const auto& P = theme::pal();
  ImGuiWindow* w = ImGui::GetCurrentWindow();
  ImGui::Dummy(ImVec2(0, theme::kS1));
  const ImVec2 p = ImGui::GetCursorScreenPos();
  // An auto-sizing dialog aligns to its content's right edge: aligning to the
  // window edge would keep the window as wide as it ever was (it never shrinks).
  const float right = (w->Flags & ImGuiWindowFlags_AlwaysAutoResize) ? std::max(w->DC.CursorMaxPos.x, p.x + buttonsWidth)
                                                                    : p.x + ImGui::GetContentRegionAvail().x;
  w->DrawList->AddLine(ImVec2(p.x, p.y), ImVec2(right, p.y), P.line);
  ImGui::Dummy(ImVec2(0, theme::kS1));
  if (buttonsWidth > 0) ImGui::SetCursorScreenPos(ImVec2(std::max(p.x, right - buttonsWidth), ImGui::GetCursorScreenPos().y));
}

float ButtonRowWidth(int count, float width) {
  return count <= 0 ? 0.0f : static_cast<float>(count) * width + static_cast<float>(count - 1) * theme::kS2;
}

ImVec2 ClampWindowPos(ImVec2 pos, ImVec2 size, ImVec2 a0, ImVec2 a1, bool dragging) {
  ImVec2 p = pos;
  if (dragging) {  // while dragged: the title bar stays reachable
    p.x = std::clamp(p.x, a0.x - size.x + 80.0f, std::max(a0.x - size.x + 80.0f, a1.x - 80.0f));
    p.y = std::clamp(p.y, a0.y, std::max(a0.y, a1.y - 32.0f));
  } else {  // at rest: inside (a window larger than the area keeps its top-left corner in)
    p.x = std::max(a0.x, std::min(p.x, a1.x - size.x));
    p.y = std::max(a0.y, std::min(p.y, a1.y - size.y));
  }
  return p;
}

void WorkArea(ImVec2& a0, ImVec2& a1) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  a0 = ImVec2(vp->WorkPos.x, vp->WorkPos.y + theme::kTopBarH);
  a1 = ImVec2(vp->WorkPos.x + vp->WorkSize.x, vp->WorkPos.y + vp->WorkSize.y - theme::kStatusBarH);
}

namespace {
struct Tracked {
  ImGuiID id;
  int frame;  // last frame the window called TrackWindowInViewport
};
ImVector<Tracked> gTracked;  // per process; windows of the current ImGui context
ImGuiContext* gTrackedCtx = nullptr;

// Moved by its title bar, or resized / dragged inside with the mouse (as 0.6.0: a resize past the edge is not pushed back mid-drag).
bool beingDragged(const ImGuiWindow* w) {
  const ImGuiContext& g = *GImGui;
  if (g.MovingWindow && (g.MovingWindow == w || g.MovingWindow->RootWindowDockTree == w)) return true;
  return g.ActiveIdWindow && g.ActiveIdWindow->RootWindow == w && ImGui::IsMouseDragging(ImGuiMouseButton_Left);
}

// J-113: windows may be outside the main window (their own OS windows) this frame.
bool viewportsOn() { return (GImGui->ConfigFlagsCurrFrame & ImGuiConfigFlags_ViewportsEnable) != 0; }
bool outsideMain(const ImGuiWindow* w) {
  // J-114: also a window marked detached that gets its own OS window when it is next begun.
  return (w->Viewport && w->Viewport != ImGui::GetMainViewport()) || (w->WindowClass.ViewportFlagsOverrideSet & ImGuiViewportFlags_NoAutoMerge) != 0;
}

// Dragged out, or resting outside the main window: its title bar kept on a monitor.
void keepOnMonitors(ImGuiWindow* w, bool current) {
  std::vector<place::Rect> mons;
  for (const ImGuiPlatformMonitor& m : GImGui->PlatformIO.Monitors) mons.push_back({m.WorkPos.x, m.WorkPos.y, m.WorkSize.x, m.WorkSize.y});
  const place::Rect r = place::keepTitleOnMonitors({w->Pos.x, w->Pos.y, w->Size.x, w->Size.y}, mons);
  if (r.x == w->Pos.x && r.y == w->Pos.y) return;
  if (current) ImGui::SetWindowPos(ImVec2(r.x, r.y));
  else ImGui::SetWindowPos(w, ImVec2(r.x, r.y), ImGuiCond_Always);
}

// Where a dialog opens: over the main window's work area (between the top bar and the
// status bar) or, J-113, over the detached window it was opened from, within its monitor.
struct Anchor {
  ImVec2 centre, a0, a1;
};
Anchor dialogAnchor() {
  const ImGuiViewport* m = ImGui::GetMainViewport();
  const ImGuiViewport* vp = m;
  if (viewportsOn()) {
    ImGuiWindow* w = GImGui->NavWindow;
    while (w && (w->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_ChildMenu | ImGuiWindowFlags_Tooltip)) && w->ParentWindow) w = w->ParentWindow;
    if (w && w->RootWindowDockTree->Viewport) vp = w->RootWindowDockTree->Viewport;
  }
  const ImVector<ImGuiPlatformMonitor>& mons = GImGui->PlatformIO.Monitors;
  const int mi = static_cast<const ImGuiViewportP*>(vp)->PlatformMonitor;
  Anchor a;
  if (vp == m || mi < 0 || mi >= mons.Size) {
    a.a0 = ImVec2(m->WorkPos.x, m->WorkPos.y + theme::kTopBarH);
    a.a1 = ImVec2(m->WorkPos.x + m->WorkSize.x, m->WorkPos.y + m->WorkSize.y - theme::kStatusBarH);
    a.centre = ImVec2(m->WorkPos.x + m->WorkSize.x * 0.5f, (a.a0.y + a.a1.y) * 0.5f);
    return a;
  }
  a.a0 = mons[mi].WorkPos;
  a.a1 = ImVec2(mons[mi].WorkPos.x + mons[mi].WorkSize.x, mons[mi].WorkPos.y + mons[mi].WorkSize.y);
  a.centre = ImVec2(std::clamp(vp->Pos.x + vp->Size.x * 0.5f, a.a0.x, std::max(a.a0.x, a.a1.x)),
                    std::clamp(vp->Pos.y + vp->Size.y * 0.5f, a.a0.y, std::max(a.a0.y, a.a1.y)));
  return a;
}
}  // namespace

namespace {
ImGuiStorage gDetachable;  // J-114: windows that pop out although they never dock (editors)
}
void MarkDetachable() {
  if (ImGuiWindow* w = GImGui->CurrentWindow) gDetachable.SetBool(w->ID, true);
}
bool IsMarkedDetachable(const ImGuiWindow* w) { return w && gDetachable.GetBool(w->ID, false); }

void TrackWindowInViewport() {
  ImGuiContext& g = *GImGui;
  if (gTrackedCtx != &g) gTracked.clear(), gTrackedCtx = &g;
  ImGuiWindow* w = g.CurrentWindow;
  if (!w) return;
  bool found = false;
  for (Tracked& t : gTracked)
    if (t.id == w->ID) t.frame = g.FrameCount, found = true;
  if (!found) gTracked.push_back({w->ID, g.FrameCount});
  // The frame a window appears its size is new: placed at once (one frame
  // of content offset at most, never while the user drags it).
  if (ImGui::IsWindowAppearing() && !beingDragged(w)) {
    if (viewportsOn() && outsideMain(w)) {
      keepOnMonitors(w, true);
      return;
    }
    ImVec2 a0, a1;
    WorkArea(a0, a1);
    const ImVec2 p = ClampWindowPos(w->Pos, w->Size, a0, a1, false);
    if (p.x != w->Pos.x || p.y != w->Pos.y) ImGui::SetWindowPos(p);
  }
}

namespace {
// J-117: the elastic boundary. Per window: where it was last put, unrounded (ImGui truncates
// window positions: a spring stepping from the truncated one could stall a pixel short), its size
// then, and the last frame it was looked at (not seen the frame before: it just opened and is
// placed at once).
struct Elastic {
  float x = 0, y = 0, w = 0, h = 0;
  int seen = -100;
};
std::unordered_map<ImGuiID, Elastic> gElastic;
ImGuiContext* gElasticCtx = nullptr;
rubber::Params gRubber;

bool isTracked(ImGuiID id) {
  for (const Tracked& t : gTracked)
    if (t.id == id) return true;
  return false;
}

// A window the boundary holds: a tracked one (dialogs, channel settings, ...), a floating panel
// or editor (anything that docks or pops out: the channel rack, the mixer, the piano roll, plugin
// editors) or a floating dock node holding several. Never a popup, menu, tooltip or combo list
// (unless tracked: modal dialogs), a docked window (its node moves it), a fixed (NoMove) window
// or Gridline's own chrome (##topbar, ##status, the dock space host).
bool elasticWindow(const ImGuiWindow* w, bool tracked) {
  if (!w->WasActive || w->Hidden || w->IsFallbackWindow || w != w->RootWindowDockTree || w->DockIsActive) return false;
  if (tracked) return true;
  if (w->Flags & (ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_Popup | ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_ChildMenu |
                  ImGuiWindowFlags_Modal | ImGuiWindowFlags_NoMove))
    return false;
  if (w->Flags & ImGuiWindowFlags_DockNodeHost) return true;
  if (w->Name[0] == '#' && w->Name[1] == '#') return false;
  return !(w->Flags & ImGuiWindowFlags_NoDocking) || IsMarkedDetachable(w);
}

// Once a frame, inside NewFrame() right after ImGui moved the window dragged with the mouse
// (GRIDLINE PATCH (J-117) WindowsMovedHook): before anything is laid out, floating dock node
// hosts included (NewFrame lays those out itself; moved after that, their tab bar was drawn
// where the window no longer was). The first frame of a context, before the hook is installed,
// it runs from ClampTrackedWindows() after NewFrame().
int gElasticFrame = -1;
void keepInWorkArea(ImGuiContext* ctx) {
  ImGuiContext& g = *ctx;
  if (gTrackedCtx != &g) gTracked.clear(), gTrackedCtx = &g;
  if (gElasticCtx != &g) gElastic.clear(), gElasticCtx = &g, gElasticFrame = -1;
  if (gElasticFrame == g.FrameCount) return;
  gElasticFrame = g.FrameCount;
  for (auto it = gElastic.begin(); it != gElastic.end();) it = g.FrameCount - it->second.seen > 120 ? gElastic.erase(it) : std::next(it);
  ImVec2 a0, a1;
  WorkArea(a0, a1);
  const double dt = static_cast<double>(g.IO.DeltaTime);
  const ImGuiWindow* moving = g.MovingWindow ? g.MovingWindow->RootWindowDockTree : nullptr;
  for (ImGuiWindow* w : g.Windows) {
    if (!elasticWindow(w, isTracked(w->ID))) continue;
    if (viewportsOn() && outsideMain(w)) continue;  // J-114: detached, moved and sized by the OS
    Elastic& e = gElastic[w->ID];
    const bool fresh = e.seen < g.FrameCount - 1;  // just opened (an editor, a layout loaded): placed at once
    e.seen = g.FrameCount;
    const bool resized = std::fabs(e.w - w->Size.x) >= 1.0f || std::fabs(e.h - w->Size.y) >= 1.0f;  // by code (the user's resize is held below)
    e.w = w->Size.x, e.h = w->Size.y;
    const rubber::Range rx = rubber::range(a0.x, a1.x, w->Size.x), ry = rubber::range(a0.y, a1.y, w->Size.y);
    ImVec2 p;
    if (w == moving) {
      // Dragged by its title bar or tab: ImGui put it under the pointer (NewFrame); the part past
      // an edge is damped. Recomputed from the pointer every frame, the stretch never accumulates.
      p = ImVec2(rubber::drag(w->Pos.x, rx, gRubber), rubber::drag(w->Pos.y, ry, gRubber));
    } else {
      if (g.ActiveIdWindow && g.ActiveIdWindow->RootWindowDockTree == w && g.IO.MouseDown[0]) {  // resized (or a control held): after
        e.x = w->Pos.x, e.y = w->Pos.y;
        continue;
      }
      const ImVec2 target(rubber::rest(w->Pos.x, rx), rubber::rest(w->Pos.y, ry));
      if (target.x == w->Pos.x && target.y == w->Pos.y) {
        e.x = w->Pos.x, e.y = w->Pos.y;
        continue;
      }
      // Put outside (moved or grown) by someone else since: code, a layout applied, a dialog
      // sizing itself; rather than let go there or left there by a shrinking work area: placed
      // at once, like a window that just opened.
      const bool placed = resized || std::fabs(e.x - w->Pos.x) >= 1.0f || std::fabs(e.y - w->Pos.y) >= 1.0f;
      if (fresh || placed || !gRubber.elastic) p = target;
      else p = ImVec2(rubber::spring(e.x, target.x, dt, gRubber), rubber::spring(e.y, target.y, dt, gRubber));
    }
    e.x = p.x, e.y = p.y;
    if (p.x != w->Pos.x || p.y != w->Pos.y) {
      ImGui::SetWindowPos(w, p, ImGuiCond_Always);
      if (ImGuiDockNode* n = w->DockNodeAsHost) n->Pos = w->Pos;
    }
  }
}
}  // namespace

void SetWindowRubber(bool elastic) { gRubber.elastic = elastic; }

void ClampTrackedWindows() {
  ImGuiContext& g = *GImGui;
  if (gTrackedCtx != &g) gTracked.clear(), gTrackedCtx = &g;
  for (int i = 0; i < gTracked.Size;) {
    ImGuiWindow* w = ImGui::FindWindowByID(gTracked[i].id);
    if (!w || g.FrameCount - gTracked[i].frame > 2) {  // closed
      gTracked.erase(gTracked.Data + i);
      continue;
    }
    ++i;
  }
  keepInWorkArea(&g);  // (done already this frame once the hook is in)
  g.WindowsMovedHook = keepInWorkArea;
}

float DialogMaxHeight() {
  const Anchor a = dialogAnchor();
  return std::max(200.0f, a.a1.y - a.a0.y);
}

ImVec2 DialogArea() {
  const Anchor a = dialogAnchor();
  return ImVec2(std::max(240.0f, a.a1.x - a.a0.x - 2 * theme::kS2), std::max(200.0f, std::max(200.0f, a.a1.y - a.a0.y) - 2 * theme::kS2));
}

ImVec2 FitDialogSize(ImVec2 want) {
  const ImVec2 area = DialogArea();
  return ImVec2(want.x > 0 ? std::min(want.x, area.x) : want.x, want.y > 0 ? std::min(want.y, area.y) : want.y);
}

void PlaceNextDialog(ImVec2 want, ImGuiCond cond, ImVec2 minSize) {
  const ImVec2 area = DialogArea();
  ImGui::SetNextWindowPos(dialogAnchor().centre, cond, ImVec2(0.5f, 0.5f));
  const ImVec2 size = FitDialogSize(want);
  if (size.x > 0 || size.y > 0) ImGui::SetNextWindowSize(size, cond);
  ImGui::SetNextWindowSizeConstraints(ImVec2(std::min(minSize.x, area.x), std::min(minSize.y, area.y)), area);
}

namespace {
// An auto-sizing dialog only knows its final size a few frames after it
// appears (its body measures itself first): keep it centred in the work area
// until then, as PlaceNextDialog placed it.
void settleCentred() {
  ImGuiWindow* w = ImGui::GetCurrentWindow();
  const ImGuiID key = ImGui::GetID("##j54settle");
  if (w->Appearing) w->StateStorage.SetInt(key, 4);
  const int n = w->StateStorage.GetInt(key, 0);
  if (n <= 0) return;
  w->StateStorage.SetInt(key, n - 1);
  const Anchor a = dialogAnchor();
  ImGui::SetWindowPos(ImVec2(std::round(a.centre.x - w->Size.x * 0.5f), std::round(std::max(a.a0.y, a.centre.y - w->Size.y * 0.5f))));
}
}  // namespace

void BeginDialogBody(const char* id, int footerRows) {
  settleCentred();
  const float padY = ImGui::GetCurrentWindow()->WindowPadding.y;  // the dialog's own padding (often pushed only around Begin)
  const float maxH = std::max(120.0f, DialogArea().y - ImGui::GetCursorPosY() - padY - DialogFooterHeight(footerRows));
  ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, maxH));
  ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysAutoResize,
                    ImGuiWindowFlags_NoBackground);
}

void EndDialogBody() { ImGui::EndChild(); }  // the auto-fit width already makes room for the scrollbar

float FormColumn(const char* const* labels, int count, float minX, float maxX, float gap) {
  float w = 0;
  for (int i = 0; i < count; ++i)
    if (labels[i]) w = std::max(w, ImGui::CalcTextSize(labels[i]).x);
  return std::max(minX, std::min(maxX, w + gap));
}

namespace {
void fullTextTooltip(const char* text) {
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) && ImGui::BeginTooltip()) {
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
}
}  // namespace

bool TextFit(const char* text, float maxW, ImU32 col, bool framed) {
  if (framed) ImGui::AlignTextToFramePadding();
  const char* shown = theme::fitText(text, std::max(0.0f, maxW));
  const bool cut = std::strcmp(shown, text) != 0;
  ImGui::PushStyleColor(ImGuiCol_Text, col);
  ImGui::TextUnformatted(shown);
  ImGui::PopStyleColor();
  if (cut) fullTextTooltip(text);
  return cut;
}

bool PathFit(const char* text, float maxW, ImU32 col, bool framed) {
  if (framed) ImGui::AlignTextToFramePadding();
  const char* shown = theme::fitTextMiddle(text, std::max(0.0f, maxW));
  const bool cut = std::strcmp(shown, text) != 0;
  ImGui::PushStyleColor(ImGuiCol_Text, col);
  ImGui::TextUnformatted(shown);
  ImGui::PopStyleColor();
  if (cut) fullTextTooltip(text);
  return cut;
}

void FormLabelFit(const char* label, float x, ImU32 col, float gap) {
  // SameLine(x) counts from the window (or enclosing group) start, not from the indent.
  const ImGuiWindow* w = ImGui::GetCurrentWindow();
  const float colX = w->Pos.x - w->Scroll.x + x + w->DC.GroupOffset.x + w->DC.ColumnsOffset.x;
  const float room = colX - ImGui::GetCursorScreenPos().x - gap;
  TextFit(label, room, col, true);
  ImGui::SameLine(x);
}

}  // namespace gl::ui
