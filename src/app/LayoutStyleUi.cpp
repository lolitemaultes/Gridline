// Gridline - the layout style in the interface (K-112). See LayoutStyleUi.h.
#include "LayoutStyleUi.h"
#include <algorithm>
#include <cmath>
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "App.h"
#include "LayoutPresets.h"
#include "LayoutShellUi.h"
#include "Look.h"
#include "Theme.h"
#include "TrackerView.h"

namespace gl::layoutui {

namespace {
struct Geometry {
  ImVec2 framePadding, itemSpacing, titleAlign;
  float frameRounding = 0, grabRounding = 0, tabRounding = 0, windowRounding = 0, childRounding = 0, popupRounding = 0, separator = 0;
  float frameBorder = 0;
  float scrollbarRounding = 0, popupBorder = 0;  // K-106 cp21
  float menuItemRounding = 0;  // K-112D
  float tabCloseSelected = 0;  // K-297
};
Geometry gBase;
bool gStyled = false;
// K-112c (round 19): the title colours a styled frame replaced (restored at its end).
ImVec4 gTitle[3];
bool gTitled = false;
const layout::Style* gStyle = nullptr;
ImVec4 gSep[2];       // checkpoint 30: the base splitter hover and held colours, put back at frame end
bool gSepSet = false;
ImVec4 gChk[2];       // checkpoint 38: the base check mark and checked-box colours, put back at frame end
bool gChkSet = false;
// K-112D: Smooth's neutral faces. The reference's panels are neutral greys, darker than Gridline's slate:
// while Smooth draws (dark theme, panels' tint and brightness untouched in Settings > Theme), every
// face colour (the palette's panels, lines, fields and text, and the ImGui colours made from them) is
// taken to grey and a step darker; role colours (accent, lit glyphs, LEDs, track colours) keep theirs.
// Plugin bodies get the base colours back (BaseStyleScope), so no editor changes.
constexpr uint32_t kSmoothBlue = 0x2F7FE0;  // audit X2: the accent, a muted system blue
ImVec4 gFaceSaved[ImGuiCol_COUNT];
bool gFaces = false;
theme::Palette gFacePal, gFaceBase;

// A face colour toward Smooth's grey: saturation out, darker in the dark range (text barely moves).
ImVec4 smoothFace(ImVec4 c) {
  float h = 0, sat = 0, v = 0;
  ImGui::ColorConvertRGBtoHSV(c.x, c.y, c.z, h, sat, v);
  if (sat > 0.32f) return c;  // a role colour
  const float t = std::clamp((v - 0.45f) / 0.40f, 0.0f, 1.0f), e = 1.36f - 0.34f * t * t * (3.0f - 2.0f * t);
  const float v2 = std::pow(std::max(v, 0.0f), e);
  float r = 0, g = 0, b = 0;
  ImGui::ColorConvertHSVtoRGB(h, sat * 0.06f, v2, r, g, b);
  return ImVec4(r, g, b, c.w);
}
ImU32 smoothFaceU(ImU32 c) { return ImGui::ColorConvertFloat4ToU32(smoothFace(ImGui::ColorConvertU32ToFloat4(c))); }

bool smoothFacesWanted(const layout::Style& s) {
  if (s.preset != layout::Preset::Logic || !theme::pal().dark) return false;
  const theme::Tweaks& t = theme::tweaks();
  return t.hue == 0.0f && t.saturation == 0.0f && t.brightness == 0.0f;
}

void beginSmoothFaces(ImGuiStyle& st) {
  // K-112D (audit X2): the reference's "on" colour is a muted system blue, not orange: the accent and
  // the lit-glyph role turn blue (ImGui colours made from them too); solo, the cycle strip and input
  // monitoring keep their yellow and orange, as the reference keeps them.
  const theme::Palette base = theme::pal();
  const ImU32 blue = theme::rgb(kSmoothBlue);
  auto same = [](ImVec4 a, ImU32 b) {
    const ImVec4 v = ImGui::ColorConvertU32ToFloat4(b);
    return std::fabs(a.x - v.x) < 0.004f && std::fabs(a.y - v.y) < 0.004f && std::fabs(a.z - v.z) < 0.004f;
  };
  for (int i = 0; i < ImGuiCol_COUNT; ++i) {
    gFaceSaved[i] = st.Colors[i];
    ImVec4 c = st.Colors[i];
    if (same(c, base.accent) || same(c, base.hot)) {
      const ImVec4 b = ImGui::ColorConvertU32ToFloat4(blue);
      c = ImVec4(b.x, b.y, b.z, c.w);
    }
    st.Colors[i] = smoothFace(c);
  }
  theme::Palette p = base;
  gFaceBase = p;
  p.accent = blue;
  p.hot = blue;
  p.selection = theme::withAlpha(blue, 0.35f);
  p.playhead = theme::rgb(0xE6E6E6);  // audit C6: a light grey playhead, not lime
  for (ImU32* c : {&p.surface950, &p.surface900, &p.surface800, &p.surface700, &p.surface600, &p.line, &p.lineStrong, &p.text, &p.textDim,
                   &p.textFaint, &p.stepOff, &p.stepOffAlt, &p.gridBar, &p.gridBeat, &p.gridStep, &p.chrome, &p.well, &p.field, &p.keyBlack, &p.keyWhite, &p.knobFill})
    *c = smoothFaceU(*c);
  gFacePal = p;
  theme::pushPalette(gFacePal);
  gFaces = true;
}

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
  g.scrollbarRounding = st.ScrollbarRounding;
  g.popupBorder = st.PopupBorderSize;
  g.tabCloseSelected = st.TabCloseButtonMinWidthSelected;
  g.menuItemRounding = st.MenuItemRounding;
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
  st.ScrollbarRounding = g.scrollbarRounding;
  st.PopupBorderSize = g.popupBorder;
  st.TabCloseButtonMinWidthSelected = g.tabCloseSelected;
  st.MenuItemRounding = g.menuItemRounding;
}
}  // namespace

const layout::Style& style() { return gStyle ? *gStyle : layout::style(layout::Preset::Studio); }

bool styled() { return gStyled; }

bool smoothFaces() { return gFaces; }

void beginStyleFrame(App& a, bool welcomeScreen) {
  gStyle = shellBuilt(a, active()) ? &layout::style(active()) : &layout::style(layout::Preset::Studio);
  gStyled = false;
  const layout::Style& s = *gStyle;
  // K-106: the Tracker's menus use its own words (Song, Roll, Steps) for the shared items.
  GImGuiGridlineMenuLabel = !welcomeScreen && s.preset == layout::Preset::Tracker ? &trackerview::menuWords : nullptr;
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
  // K-297: a docked panel's tab shows its close button all the time, not only under the pointer (a
  // panel docked in a side region was otherwise closed from the View menu only).
  g.tabCloseSelected = -1.0f;
  // K-112b: the Tracks layout centres window titles (every reference window does).
  if (s.preset == layout::Preset::Logic) g.titleAlign = ImVec2(0.5f, gBase.titleAlign.y);
  // K-112b (checkpoint 20): flat buttons and fields, no outline, as every reference window draws its
  // controls (a fill on the panel, no stroke). Plugin editors get the outline back (BaseStyleScope).
  if (s.preset == layout::Preset::Logic) g.frameBorder = 0;
  // K-106 cp21 (look audit): the Tracker's scroll bars square (no rounded thumb) and its menus framed by
  // a line, as the classic screens; plugin editors keep theirs (BaseStyleScope).
  if (s.preset == layout::Preset::Tracker) g.scrollbarRounding = 0, g.popupBorder = 1;
  // K-112D (audit I3): the open menu's title in the menu bar is a rounded pill, not a square block.
  if (s.preset == layout::Preset::Logic) g.menuItemRounding = 5.0f;
  write(st, g);
  gStyled = true;
  if (smoothFacesWanted(s)) beginSmoothFaces(st);  // K-112D
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
  // Checkpoint 28: window titles in the bold face, as every reference window sets its title (023, 031, 015, 199).
  GImGuiGridlineTitleFont = s.preset == layout::Preset::Logic ? a.fontBold : nullptr;
  // Checkpoint 29: flat faces for the layout's own buttons (no lip or shadow that makes one control a
  // pixel or two taller than its neighbours).
  look::setFlatFaces(s.preset == layout::Preset::Logic);
  // Checkpoint 30: a pane splitter under the pointer lifts a step toward the text (held: two steps), no
  // accent flash; the references show only the resize pointer there.
  if (s.preset == layout::Preset::Logic) {
    gSep[0] = st.Colors[ImGuiCol_SeparatorHovered], gSep[1] = st.Colors[ImGuiCol_SeparatorActive];
    const ImVec4 line = st.Colors[ImGuiCol_Separator], text = st.Colors[ImGuiCol_Text];
    auto toward = [&](float t) { return ImVec4(line.x + (text.x - line.x) * t, line.y + (text.y - line.y) * t, line.z + (text.z - line.z) * t, 1.0f); };
    st.Colors[ImGuiCol_SeparatorHovered] = toward(0.25f);
    st.Colors[ImGuiCol_SeparatorActive] = toward(0.45f);
    gSepSet = true;
    // Checkpoint 38: a ticked box is filled with the layout's blue and ticked in white, as every reference
    // window draws its check boxes (023, 100, 132); it was an orange tick on a faintly tinted field.
    gChk[0] = st.Colors[ImGuiCol_CheckMark], gChk[1] = st.Colors[ImGuiCol_CheckboxSelectedBg];
    st.Colors[ImGuiCol_CheckMark] = theme::vec(theme::contrastText(theme::rgb(0x2F7EDB)));
    st.Colors[ImGuiCol_CheckboxSelectedBg] = theme::vec(theme::rgb(0x2F7EDB));
    gChkSet = true;
    GImGuiGridlineCheckRounding = 4.0f;  // rounded squares (023, 100, 132), not the pill-round frames
    // Checkpoint 39: a menu row under the pointer fills with the same blue, rounded (177, 021, 216).
    GImGuiGridlineMenuHover = theme::rgb(0x2F7EDB);
  }
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
  if (gSepSet) {
    ImGuiStyle& st = ImGui::GetStyle();
    st.Colors[ImGuiCol_SeparatorHovered] = gSep[0], st.Colors[ImGuiCol_SeparatorActive] = gSep[1];
  }
  gSepSet = false;
  if (gChkSet) {
    ImGuiStyle& st = ImGui::GetStyle();
    st.Colors[ImGuiCol_CheckMark] = gChk[0], st.Colors[ImGuiCol_CheckboxSelectedBg] = gChk[1];
  }
  gChkSet = false;
  GImGuiGridlineCheckRounding = -1.0f;
  GImGuiGridlineMenuHover = 0;
  GImGuiGridlineMenuLabel = nullptr;
  GImGuiGridlineMenuRight = false;
  GImGuiGridlineTitleFont = nullptr;
  look::setFlatFaces(false);
  if (gFaces) {  // K-112D: the base colours back, after the overrides above
    ImGuiStyle& st = ImGui::GetStyle();
    // the theme applied during the frame (Settings > Theme): its new palette and colours stand
    const bool applied = !(theme::pal().surface900 == gFacePal.surface900 && theme::pal().text == gFacePal.text);
    for (int i = 0; i < ImGuiCol_COUNT; ++i) st.Colors[i] = gFaceSaved[i];
    theme::popPalette();
    if (applied) theme::apply(theme::isDark());
    gFaces = false;
  }
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
  ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, gBase.scrollbarRounding);  // K-106 cp21
  ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, gBase.popupBorder);
  pushed = 10;
  menuRight = GImGuiGridlineMenuRight;  // K-112b: the editor's own menus keep their look
  GImGuiGridlineMenuRight = false;
  flat = look::flatFaces();  // checkpoint 29: an editor's buttons keep their own faces
  look::setFlatFaces(false);
  if (gFaces) {  // K-112D: an editor keeps the base colours and palette (Smooth's neutral faces stay out)
    for (int i = 0; i < ImGuiCol_COUNT; ++i) ImGui::PushStyleColor(i, gFaceSaved[i]);
    colours = ImGuiCol_COUNT;
    // the base palette over Smooth's faces (a palette an editor's skin pushed before stays as it is)
    faces = theme::pal().surface900 == gFacePal.surface900 && theme::pal().text == gFacePal.text;
    if (faces) theme::pushPalette(gFaceBase);
  }
  if (gChkSet) {  // checkpoint 38: an editor's check boxes keep their own colours
    ImGui::PushStyleColor(ImGuiCol_CheckMark, gChk[0]);
    ImGui::PushStyleColor(ImGuiCol_CheckboxSelectedBg, gChk[1]);
    colours += 2;
    checkRound = GImGuiGridlineCheckRounding, GImGuiGridlineCheckRounding = -1.0f;
  }
}

BaseStyleScope::~BaseStyleScope() {
  --gBaseDepth;
  if (faces) theme::popPalette();  // K-112D
  if (gBaseDepth >= 0 && gBaseDepth < 16) GImGuiGridlineToggleW = gToggleSaved[gBaseDepth], GImGuiGridlineMenuLive = gMenuSaved[gBaseDepth];
  if (colours) ImGui::PopStyleColor(colours), GImGuiGridlineCheckRounding = checkRound;
  if (pushed) ImGui::PopStyleVar(pushed);
  if (pushed) GImGuiGridlineMenuRight = menuRight;
  if (pushed) look::setFlatFaces(flat);
}

BaseBorderScope::BaseBorderScope() {
  if (!gStyled) return;
  ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, gBase.frameBorder);
  pushed = true;
  menuRight = GImGuiGridlineMenuRight;  // K-112b: a plugin window's own menus keep their look
  GImGuiGridlineMenuRight = false;
  flat = look::flatFaces();  // checkpoint 29: a plugin window's buttons keep their own faces
  look::setFlatFaces(false);
  if (gChkSet) {  // checkpoint 38: a plugin window's check boxes keep their own colours
    ImGui::PushStyleColor(ImGuiCol_CheckMark, gChk[0]);
    ImGui::PushStyleColor(ImGuiCol_CheckboxSelectedBg, gChk[1]);
    colours = 2;
    checkRound = GImGuiGridlineCheckRounding, GImGuiGridlineCheckRounding = -1.0f;
  }
}

BaseBorderScope::~BaseBorderScope() {
  if (colours) ImGui::PopStyleColor(colours), GImGuiGridlineCheckRounding = checkRound;
  if (pushed) ImGui::PopStyleVar();
  if (pushed) GImGuiGridlineMenuRight = menuRight;
  if (pushed) look::setFlatFaces(flat);
}

float dockMargin() { return gStyled ? style().outerMargin : 4.0f; }

float statusBarHeight() { return gStyled && style().statusBarH <= 0 ? 0.0f : theme::kStatusBarH; }

bool menuTitles() { return !gStyled || style().menuHeadings; }

void menuHeading(const char* text) {
  if (menuTitles()) ImGui::SeparatorText(text);
  else ImGui::Separator();
}

}  // namespace gl::layoutui
