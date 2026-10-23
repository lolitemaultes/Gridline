#include "DialogLayout.h"
#include <algorithm>
#include <cmath>
#include <cfloat>
#include <cstring>
#include "../../third_party/imgui/imgui_internal.h"
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
}  // namespace

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
    ImVec2 a0, a1;
    WorkArea(a0, a1);
    const ImVec2 p = ClampWindowPos(w->Pos, w->Size, a0, a1, false);
    if (p.x != w->Pos.x || p.y != w->Pos.y) ImGui::SetWindowPos(p);
  }
}

void ClampTrackedWindows() {
  ImGuiContext& g = *GImGui;
  if (gTrackedCtx != &g) gTracked.clear(), gTrackedCtx = &g;
  ImVec2 a0, a1;
  WorkArea(a0, a1);
  for (int i = 0; i < gTracked.Size;) {
    ImGuiWindow* w = ImGui::FindWindowByID(gTracked[i].id);
    if (!w || g.FrameCount - gTracked[i].frame > 2) {  // closed
      gTracked.erase(gTracked.Data + i);
      continue;
    }
    ++i;
    if (!w->WasActive || w->Hidden || w->DockIsActive) continue;
    const ImVec2 p = ClampWindowPos(w->Pos, w->Size, a0, a1, beingDragged(w));
    if (p.x != w->Pos.x || p.y != w->Pos.y) ImGui::SetWindowPos(w, p, ImGuiCond_Always);
  }
}

float DialogMaxHeight() {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  return std::max(200.0f, vp->WorkSize.y - theme::kTopBarH - theme::kStatusBarH);
}

ImVec2 DialogArea() {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  return ImVec2(std::max(240.0f, vp->WorkSize.x - 2 * theme::kS2), std::max(200.0f, DialogMaxHeight() - 2 * theme::kS2));
}

ImVec2 FitDialogSize(ImVec2 want) {
  const ImVec2 area = DialogArea();
  return ImVec2(want.x > 0 ? std::min(want.x, area.x) : want.x, want.y > 0 ? std::min(want.y, area.y) : want.y);
}

void PlaceNextDialog(ImVec2 want, ImGuiCond cond, ImVec2 minSize) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const ImVec2 area = DialogArea();
  const float top = vp->WorkPos.y + theme::kTopBarH, bottom = vp->WorkPos.y + vp->WorkSize.y - theme::kStatusBarH;
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, (top + bottom) * 0.5f), cond, ImVec2(0.5f, 0.5f));
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
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const float top = vp->WorkPos.y + theme::kTopBarH, bottom = vp->WorkPos.y + vp->WorkSize.y - theme::kStatusBarH;
  ImGui::SetWindowPos(ImVec2(std::round(vp->WorkPos.x + (vp->WorkSize.x - w->Size.x) * 0.5f), std::round(std::max(top, (top + bottom - w->Size.y) * 0.5f))));
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

void FormLabelFit(const char* label, float x, ImU32 col, float gap) {
  // SameLine(x) counts from the window (or enclosing group) start, not from the indent.
  const ImGuiWindow* w = ImGui::GetCurrentWindow();
  const float colX = w->Pos.x - w->Scroll.x + x + w->DC.GroupOffset.x + w->DC.ColumnsOffset.x;
  const float room = colX - ImGui::GetCursorScreenPos().x - gap;
  TextFit(label, room, col, true);
  ImGui::SameLine(x);
}

}  // namespace gl::ui
