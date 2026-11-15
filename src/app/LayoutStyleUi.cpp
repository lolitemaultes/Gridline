// Gridline - the layout style in the interface (K-112). See LayoutStyleUi.h.
#include "LayoutStyleUi.h"
#include <algorithm>
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "App.h"
#include "LayoutPresets.h"
#include "LayoutShellUi.h"
#include "Theme.h"

namespace gl::layoutui {

namespace {
struct Geometry {
  ImVec2 framePadding, itemSpacing, titleAlign;
  float frameRounding = 0, grabRounding = 0, tabRounding = 0, windowRounding = 0, childRounding = 0, popupRounding = 0, separator = 0;
  float frameBorder = 0;
};
Geometry gBase;
bool gStyled = false;
// K-112c (round 19): the title colours a styled frame replaced (restored at its end).
ImVec4 gTitle[3];
bool gTitled = false;
const layout::Style* gStyle = nullptr;

Geometry read(const ImGuiStyle& st) {
  Geometry g;
  g.framePadding = st.FramePadding;
  g.itemSpacing = st.ItemSpacing;
  g.titleAlign = st.WindowTitleAlign;
  g.frameRounding = st.FrameRounding;
  g.grabRounding = st.GrabRounding;
  g.tabRounding = st.TabRounding;
  g.windowRounding = st.WindowRounding;
  g.childRounding = st.ChildRounding;
  g.popupRounding = st.PopupRounding;
  g.separator = st.DockingSeparatorSize;
  g.frameBorder = st.FrameBorderSize;
  return g;
}
void write(ImGuiStyle& st, const Geometry& g) {
  st.FramePadding = g.framePadding;
  st.ItemSpacing = g.itemSpacing;
  st.WindowTitleAlign = g.titleAlign;
  st.FrameRounding = g.frameRounding;
  st.GrabRounding = g.grabRounding;
  st.TabRounding = g.tabRounding;
  st.WindowRounding = g.windowRounding;
  st.ChildRounding = g.childRounding;
  st.PopupRounding = g.popupRounding;
  st.DockingSeparatorSize = g.separator;
  st.FrameBorderSize = g.frameBorder;
}
}  // namespace

const layout::Style& style() { return gStyle ? *gStyle : layout::style(layout::Preset::Studio); }

bool styled() { return gStyled; }

void beginStyleFrame(App& a, bool welcomeScreen) {
  gStyle = shellBuilt(a, active()) ? &layout::style(active()) : &layout::style(layout::Preset::Studio);
  gStyled = false;
  const layout::Style& s = *gStyle;
  if (welcomeScreen || layout::isStudio(s)) return;  // Studio and the welcome screen: nothing changes
  ImGuiStyle& st = ImGui::GetStyle();
  gBase = read(st);
  Geometry g = gBase;
  g.framePadding = ImVec2(s.fieldPadX, s.fieldPadY);
  g.itemSpacing = ImVec2(s.itemSpacingX, s.rowSpacing);
  g.frameRounding = s.frameRounding;
  g.grabRounding = std::min(s.frameRounding, gBase.grabRounding * 2.0f);
  g.tabRounding = s.tabRounding;
  g.windowRounding = g.childRounding = g.popupRounding = s.panelRounding;
  if (s.menuRounding >= 0) g.popupRounding = s.menuRounding;
  g.separator = s.panelGap;
  // K-112b: the Tracks layout centres window titles (every reference window does).
  if (s.preset == layout::Preset::Logic) g.titleAlign = ImVec2(0.5f, gBase.titleAlign.y);
  // K-112b (checkpoint 20): flat buttons and fields, no outline, as every reference window draws its
  // controls (a fill on the panel, no stroke). Plugin editors get the outline back (BaseStyleScope).
  if (s.preset == layout::Preset::Logic) g.frameBorder = 0;
  write(st, g);
  gStyled = true;
  // K-112c (round 19): in Live a floating window's caption is the body's own colour, the focused one
  // a step lifted, as the refs' device and plugin windows (192, 194, 196, 424).
  if (s.preset == layout::Preset::Live) {
    const ImGuiCol ids[3] = {ImGuiCol_TitleBg, ImGuiCol_TitleBgActive, ImGuiCol_TitleBgCollapsed};
    for (int i = 0; i < 3; ++i) gTitle[i] = st.Colors[ids[i]];
    const ImVec4 body = st.Colors[ImGuiCol_WindowBg], lift = st.Colors[ImGuiCol_Text];
    st.Colors[ImGuiCol_TitleBg] = st.Colors[ImGuiCol_TitleBgCollapsed] = body;
    st.Colors[ImGuiCol_TitleBgActive] = ImVec4(body.x + (lift.x - body.x) * 0.06f, body.y + (lift.y - body.y) * 0.06f,
                                               body.z + (lift.z - body.z) * 0.06f, body.w);
    gTitled = true;
    GImGuiGridlineMenuLive = true;  // round 25: right-aligned shortcuts, edge-to-edge separators (420, 425, 417)
  }
  // K-112b (checkpoint 21): menus as every reference draws them, shortcuts flush right, check marks in
  // the text colour (216, 021, 177, 168).
  GImGuiGridlineMenuRight = s.preset == layout::Preset::Logic;
}

void endStyleFrame() {
  GImGuiGridlineToggleW = 0.0f;  // never carried into the next frame (a group left open by an early return)
  GImGuiGridlineMenuLive = false;
  if (gStyled) write(ImGui::GetStyle(), gBase);
  if (gTitled) {
    ImGuiStyle& st = ImGui::GetStyle();
    st.Colors[ImGuiCol_TitleBg] = gTitle[0], st.Colors[ImGuiCol_TitleBgActive] = gTitle[1], st.Colors[ImGuiCol_TitleBgCollapsed] = gTitle[2];
  }
  gTitled = false;
  GImGuiGridlineMenuRight = false;
  gStyled = false;
}

namespace {
int gBaseDepth = 0;  // open BaseStyleScopes (a plugin body drawing)
}

bool inBaseStyle() { return gBaseDepth > 0; }

namespace {
float gToggleSaved[16];  // the check box field width at each open scope (a plugin body draws plain boxes)
bool gMenuSaved[16];     // the Live menu look at each open scope
}

BaseStyleScope::BaseStyleScope() {
  if (gBaseDepth < 16) gToggleSaved[gBaseDepth] = GImGuiGridlineToggleW, gMenuSaved[gBaseDepth] = GImGuiGridlineMenuLive;
  GImGuiGridlineToggleW = 0.0f;
  GImGuiGridlineMenuLive = false;  // a plugin body's menus keep ImGui's look
  ++gBaseDepth;
  if (!gStyled) return;
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, gBase.framePadding);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, gBase.itemSpacing);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, gBase.frameRounding);
  ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, gBase.grabRounding);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, gBase.childRounding);
  ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, gBase.popupRounding);
  ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, gBase.tabRounding);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, gBase.frameBorder);  // K-112b: the Tracks layout's flat controls stay out
  pushed = 8;
  menuRight = GImGuiGridlineMenuRight;  // K-112b: the editor's own menus keep their look
  GImGuiGridlineMenuRight = false;
}

BaseStyleScope::~BaseStyleScope() {
  --gBaseDepth;
  if (gBaseDepth >= 0 && gBaseDepth < 16) GImGuiGridlineToggleW = gToggleSaved[gBaseDepth], GImGuiGridlineMenuLive = gMenuSaved[gBaseDepth];
  if (pushed) ImGui::PopStyleVar(pushed);
  if (pushed) GImGuiGridlineMenuRight = menuRight;
}

BaseBorderScope::BaseBorderScope() {
  if (!gStyled) return;
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, gBase.frameBorder);
  pushed = true;
  menuRight = GImGuiGridlineMenuRight;  // K-112b: a plugin window's own menus keep their look
  GImGuiGridlineMenuRight = false;
}

BaseBorderScope::~BaseBorderScope() {
  if (pushed) ImGui::PopStyleVar();
  if (pushed) GImGuiGridlineMenuRight = menuRight;
}

float dockMargin() { return gStyled ? style().outerMargin : 4.0f; }

float statusBarHeight() { return gStyled && style().statusBarH <= 0 ? 0.0f : theme::kStatusBarH; }

bool menuTitles() { return !gStyled || style().menuHeadings; }

void menuHeading(const char* text) {
  if (menuTitles()) ImGui::SeparatorText(text);
  else ImGui::Separator();
}

}  // namespace gl::layoutui
