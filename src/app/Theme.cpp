#include "Theme.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace gl::theme {

namespace {
Palette gPal;
bool gDark = true;
MarkTextureFn gMarkTex = nullptr;

constexpr ImU32 hex(uint32_t rrggbb, uint8_t a = 255) {
  return IM_COL32((rrggbb >> 16) & 0xFF, (rrggbb >> 8) & 0xFF, rrggbb & 0xFF, a);
}
}  // namespace

const Palette& pal() { return gPal; }

ImU32 popupBg(const Palette& p) { return p.dark ? mix(p.surface900, p.surface800, 0.35f) : mix(p.surface800, p.paper, 0.6f); }

namespace {
Palette gPalStack[8];
int gPalDepth = 0;
}  // namespace

void pushPalette(const Palette& p) {
  if (gPalDepth < 8) gPalStack[gPalDepth] = gPal;
  ++gPalDepth;
  gPal = p;
}

void popPalette() {
  if (gPalDepth <= 0) return;
  --gPalDepth;
  if (gPalDepth < 8) gPal = gPalStack[gPalDepth];
}
bool isDark() { return gDark; }

ImU32 rgb(uint32_t rrggbb, float alpha) {
  return hex(rrggbb, static_cast<uint8_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f));
}

ImU32 withAlpha(ImU32 c, float a) {
  uint32_t al = static_cast<uint32_t>(((c >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(a, 0.0f, 1.0f) + 0.5f);
  return (c & ~IM_COL32_A_MASK) | (al << IM_COL32_A_SHIFT);
}

ImU32 mix(ImU32 a, ImU32 b, float t) {
  auto ch = [&](int s) {
    float x = static_cast<float>((a >> s) & 0xFF), y = static_cast<float>((b >> s) & 0xFF);
    return static_cast<ImU32>(x + (y - x) * t + 0.5f) << s;
  };
  return ch(IM_COL32_R_SHIFT) | ch(IM_COL32_G_SHIFT) | ch(IM_COL32_B_SHIFT) | ch(IM_COL32_A_SHIFT);
}

ImVec4 vec(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

ImU32 lit(ImU32 role) { return gPal.dark ? role : mix(role, (role & IM_COL32_A_MASK) | hex(0x101418, 0), 0.34f); }
ImU32 litRole(uint32_t rrggbb, float alpha) { return lit(rgb(rrggbb, alpha)); }

ImU32 content(uint32_t rrggbb, float alpha) {
  ImU32 c = rgb(rrggbb, alpha);
  if (gPal.dark) return c;
  const int r = (rrggbb >> 16) & 0xFF, g = (rrggbb >> 8) & 0xFF, b = rrggbb & 0xFF;
  const int hi = std::max({r, g, b}), lo = std::min({r, g, b});
  if (hi - lo < 32 && hi < 150) return mix(c, withAlpha(gPal.lineStrong, alpha), 0.55f);  // neutral dark grey
  return c;
}

ImU32 contrastText(ImU32 bg) {
  ImVec4 v = vec(bg);
  float lum = 0.2126f * v.x + 0.7152f * v.y + 0.0722f * v.z;
  return lum > 0.55f ? hex(0x000000) : hex(0xFFFFFF);
}

void apply(bool dark) {
  gDark = dark;
  Palette& p = gPal;
  p.dark = dark;
  p.paper = hex(0xFFFFFF);
  p.ink = hex(0x000000);
  if (dark) {
    // Studio slate (J-13): the reference DAW's form colour roles — slate
    // panels (#3F484D family), dark fields (#262B2F), light-grey captions
    // (#C9CFD3), orange lit glyphs, green LEDs — drawn by Gridline.
    p.accent = hex(0xFF9E3D);
    p.danger = hex(0xFF5B53);
    p.surface950 = hex(0x262C30);
    p.surface900 = hex(0x353D42);
    p.surface800 = hex(0x434C51);
    p.surface700 = hex(0x4F595E);
    p.surface600 = hex(0x5D676C);
    p.line = hex(0x293034);
    p.lineStrong = hex(0x5A6368);
    p.text = hex(0xD5DBDF);
    p.textDim = hex(0xA2ABB0);
    p.textFaint = hex(0x778186);
    p.stepOff = hex(0x4A535A);
    p.stepOffAlt = hex(0x5E4A52);
    p.stepOn = hex(0xE3E8EB);
    p.gridBar = hex(0x1D2225);
    p.gridBeat = hex(0x283034);
    p.gridStep = hex(0x2E363A);
    p.playhead = hex(0xC6F07A);
    p.logo = p.paper;
    p.chrome = p.surface950;
    p.hot = hex(0xFFB547);
    p.led = hex(0xA8E44A);
    p.well = hex(0x30373B);
    p.field = hex(0x23282B);
    p.knobFill = hex(0xCFD8DD);
    p.keyWhite = hex(0xDCE1E4);
    p.keyBlack = hex(0x1E2225);
  } else {
    // Light studio variant (J-14): the same colour roles on light slate —
    // pale panels (#E3E8EB family) on a darker chrome, white fields, an
    // orange lit glyph deep enough to read on light faces, green LEDs.
    p.accent = hex(0xE07A12);
    p.danger = hex(0xE0443C);
    p.surface950 = hex(0xC9D1D6);
    p.surface900 = hex(0xE0E5E8);
    p.surface800 = hex(0xEEF1F3);
    p.surface700 = hex(0xD5DCE0);
    p.surface600 = hex(0xC4CDD2);
    p.line = hex(0xB7C0C6);
    p.lineStrong = hex(0x929DA4);
    p.text = hex(0x1B2226);
    p.textDim = hex(0x4C575D);
    p.textFaint = hex(0x7B868C);
    p.stepOff = hex(0xC8D0D5);
    p.stepOffAlt = hex(0xDCC6CD);
    p.stepOn = hex(0x5A666D);
    p.gridBar = hex(0x97A2A9);
    p.gridBeat = hex(0xB6BFC5);
    p.gridStep = hex(0xC9D0D5);
    p.playhead = hex(0x3E8E14);
    p.logo = p.ink;
    p.chrome = p.surface950;
    p.hot = hex(0xD96A00);
    p.led = hex(0x4CA81C);
    p.well = hex(0xD8DEE2);
    p.field = hex(0xFAFBFC);
    p.knobFill = hex(0x4A555B);
    p.keyWhite = hex(0xFFFFFF);
    p.keyBlack = hex(0x2A3135);
  }
  p.meterLow = dark ? hex(0x86D64A) : hex(0x4CA81C);
  p.meterMid = dark ? hex(0xF2D04A) : hex(0xD9A800);
  p.meterHigh = p.danger;

  ImGuiStyle& s = ImGui::GetStyle();
  s.WindowPadding = ImVec2(kS2, kS2);
  s.FramePadding = ImVec2(kS2, (kControlH - kFontBody) * 0.5f);  // 24 px controls
  s.ItemSpacing = ImVec2(kS2, 6);  // 6 px rows: the one off-scale value, keeps lists and editors dense
  s.ItemInnerSpacing = ImVec2(kS2, kS1);
  s.CellPadding = ImVec2(kS2, kS1);
  s.IndentSpacing = kS4;
  s.ScrollbarSize = 10;
  s.ScrollbarRounding = 10;
  s.ScrollbarPadding = 2;
  s.GrabMinSize = 12;
  s.GrabRounding = kRadiusSm;
  s.WindowBorderSize = 1;
  s.ChildBorderSize = 1;
  s.PopupBorderSize = 1;
  s.FrameBorderSize = 1;  // fields, checkboxes and sliders read as controls on every surface
  s.TabBorderSize = 0;
  s.TabBarBorderSize = 1;
  s.TabBarOverlineSize = 2;
  s.WindowRounding = kRadiusLg;
  s.ChildRounding = kRadiusLg;
  s.FrameRounding = kRadius;
  s.PopupRounding = kRadiusLg;
  s.TabRounding = kRadius;
  s.WindowMinSize = ImVec2(160, 96);
  s.WindowTitleAlign = ImVec2(0.0f, 0.5f);
  s.WindowMenuButtonPosition = ImGuiDir_None;  // no docking triangle in tab bars
  s.DockingNodeHasCloseButton = false;         // one close button per tab, not per node
  s.TabCloseButtonMinWidthSelected = 0.0f;     // close buttons appear on hover
  s.TabCloseButtonMinWidthUnselected = 0.0f;
  s.DockingSeparatorSize = kS1;                // panels read as cards on the chrome
  s.SeparatorTextBorderSize = 1;
  s.SeparatorTextPadding = ImVec2(0, kS1);
  s.SelectableTextAlign = ImVec2(0.0f, 0.5f);
  s.HoverDelayShort = 0.25f;
  s.HoverDelayNormal = 0.6f;
  s.HoverStationaryDelay = 0.2f;

  ImVec4* c = s.Colors;
  auto V = [](ImU32 x) { return vec(x); };
  ImVec4 acc = V(p.accent);
  c[ImGuiCol_Text] = V(p.text);
  c[ImGuiCol_TextDisabled] = V(p.textFaint);
  c[ImGuiCol_WindowBg] = V(p.surface900);
  c[ImGuiCol_ChildBg] = V(withAlpha(p.surface900, 0));
  // Menus, combos and tooltips: a panel one step off the window slate with a
  // dark (light theme: strong) outline, so popups read above any surface.
  c[ImGuiCol_PopupBg] = V(popupBg(p));
  c[ImGuiCol_Border] = V(dark ? p.line : p.lineStrong);  // dark hairlines on slate, visible on light fields
  c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_FrameBg] = V(p.field);
  c[ImGuiCol_FrameBgHovered] = V(dark ? mix(p.field, p.surface700, 0.35f) : p.surface700);
  c[ImGuiCol_FrameBgActive] = V(dark ? mix(p.field, p.surface700, 0.55f) : p.surface600);
  c[ImGuiCol_TitleBg] = V(p.chrome);
  c[ImGuiCol_TitleBgActive] = V(mix(p.chrome, p.surface900, 0.45f));  // focused floating window: lifted caption
  c[ImGuiCol_TitleBgCollapsed] = V(p.chrome);
  c[ImGuiCol_MenuBarBg] = V(p.chrome);
  c[ImGuiCol_ScrollbarBg] = V(withAlpha(p.ink, dark ? 0.14f : 0.05f));  // slim groove, like the studio scroll bars
  c[ImGuiCol_ScrollbarGrab] = V(p.lineStrong);
  c[ImGuiCol_ScrollbarGrabHovered] = V(mix(p.lineStrong, p.text, 0.25f));
  c[ImGuiCol_ScrollbarGrabActive] = V(mix(p.lineStrong, p.text, 0.45f));
  c[ImGuiCol_CheckMark] = V(p.hot);
  c[ImGuiCol_CheckboxSelectedBg] = V(mix(p.field, p.hot, dark ? 0.10f : 0.08f));
  c[ImGuiCol_TreeLines] = V(p.lineStrong);
  c[ImGuiCol_UnsavedMarker] = V(p.hot);
  c[ImGuiCol_SliderGrab] = V(p.knobFill);
  c[ImGuiCol_SliderGrabActive] = V(p.hot);
  c[ImGuiCol_Button] = V(p.surface800);
  c[ImGuiCol_ButtonHovered] = V(p.surface700);
  c[ImGuiCol_ButtonActive] = V(p.surface600);
  c[ImGuiCol_Header] = V(p.surface700);
  c[ImGuiCol_HeaderHovered] = V(dark ? mix(p.surface700, p.surface600, 0.5f) : p.surface700);  // menu / list hover bar
  c[ImGuiCol_HeaderActive] = V(p.surface600);
  c[ImGuiCol_Separator] = V(p.line);
  c[ImGuiCol_SeparatorHovered] = acc;
  c[ImGuiCol_SeparatorActive] = acc;
  c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_ResizeGripHovered] = V(withAlpha(p.accent, 0.6f));
  c[ImGuiCol_ResizeGripActive] = acc;
  c[ImGuiCol_InputTextCursor] = acc;
  c[ImGuiCol_Tab] = V(p.chrome);
  c[ImGuiCol_TabHovered] = V(p.surface800);
  c[ImGuiCol_TabSelected] = V(p.surface900);
  c[ImGuiCol_TabSelectedOverline] = acc;
  c[ImGuiCol_TabDimmed] = V(p.chrome);
  c[ImGuiCol_TabDimmedSelected] = V(p.surface900);
  c[ImGuiCol_TabDimmedSelectedOverline] = V(withAlpha(p.text, 0));
  c[ImGuiCol_DockingPreview] = V(withAlpha(p.accent, 0.45f));
  c[ImGuiCol_DockingEmptyBg] = V(p.chrome);
  c[ImGuiCol_PlotLines] = V(p.text);
  c[ImGuiCol_PlotHistogram] = V(p.text);
  c[ImGuiCol_TableHeaderBg] = V(p.surface800);
  c[ImGuiCol_TableBorderStrong] = V(p.lineStrong);
  c[ImGuiCol_TableBorderLight] = V(p.line);
  c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_TableRowBgAlt] = V(withAlpha(p.text, 0.03f));
  c[ImGuiCol_TextSelectedBg] = V(withAlpha(p.accent, 0.45f));
  c[ImGuiCol_DragDropTarget] = acc;
  c[ImGuiCol_NavCursor] = acc;
  c[ImGuiCol_NavWindowingHighlight] = acc;
  c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, dark ? 0.6f : 0.3f);
}

// ------------------------------------------------------------------ logo ----

void setMarkTextureProvider(MarkTextureFn fn) { gMarkTex = fn; }

void drawMark(ImDrawList* dl, ImVec2 pos, float size, ImU32 fg, ImU32 bg, float opacity) {
  ImU32 f = withAlpha(fg, opacity);
  if (gMarkTex) {  // true holes: whatever is behind shows through; pixel-exact raster
    float sc = std::max(0.5f, ImGui::GetIO().DisplayFramebufferScale.x);
    int px = static_cast<int>(std::lround(size * sc));
    ImTextureID tex = px >= 8 ? gMarkTex(px) : ImTextureID_Invalid;
    if (tex != ImTextureID_Invalid) {
      pos = ImVec2(std::round(pos.x * sc) / sc, std::round(pos.y * sc) / sc);
      float s2 = static_cast<float>(px) / sc;
      dl->AddImage(ImTextureRef(tex), pos, ImVec2(pos.x + s2, pos.y + s2), ImVec2(0, 0), ImVec2(1, 1), f);
      return;
    }
  }
  const float u = size / 120.0f;
  dl->AddRectFilled(pos, ImVec2(pos.x + size, pos.y + size), f, 28.0f * u);
  // Cut-outs: four 10u bars centred at 40u and 80u, inset 24u (guide §05).
  ImU32 b = withAlpha(bg, 1.0f);
  auto bar = [&](float x0, float y0, float x1, float y1) {
    dl->AddRectFilled(ImVec2(pos.x + x0 * u, pos.y + y0 * u), ImVec2(pos.x + x1 * u, pos.y + y1 * u), b);
  };
  for (float c : {40.0f, 80.0f}) {
    bar(c - 5, 24, c + 5, 96);
    bar(24, c - 5, 96, c + 5);
  }
}

// Master lockup SVG: viewBox 520 x 120, mark 120u, wordmark at x = 148u,
// baseline 92u, Inter Bold 104u, letter-spacing -3u.
namespace {
constexpr float kWordX = 148.0f / 120.0f, kWordBase = 92.0f / 120.0f, kWordSize = 104.0f / 120.0f,
                kWordTrack = -3.0f / 120.0f;
const char* kWord = "gridline";

float wordWidth(ImFont* font, float fs, float track) {
  if (!font) return fs * 3.6f;
  ImFontBaked* fb = font->GetFontBaked(fs);
  float w = 0;
  for (const char* c = kWord; *c; ++c) w += fb->GetCharAdvance(static_cast<ImWchar>(*c)) + (c[1] ? track : 0.0f);
  return w;
}
}  // namespace

float lockupWidth(float height, ImFont* font) {
  return height * kWordX + wordWidth(font, height * kWordSize, height * kWordTrack);
}

float drawLockup(ImDrawList* dl, ImVec2 pos, float height, ImU32 fg, ImU32 bg, ImFont* font, float opacity) {
  drawMark(dl, pos, height, fg, bg, opacity);
  const float fs = height * kWordSize, track = height * kWordTrack;
  float x = pos.x + height * kWordX;
  if (!font) {
    dl->AddText(ImVec2(x, pos.y), withAlpha(fg, opacity), kWord);
    return lockupWidth(height, font);
  }
  ImFontBaked* fb = font->GetFontBaked(fs);
  float top = pos.y + height * kWordBase - fb->Ascent;
  for (const char* c = kWord; *c; ++c) {
    char one[2] = {*c, 0};
    dl->AddText(font, fs, ImVec2(x, top), withAlpha(fg, opacity), one);
    x += fb->GetCharAdvance(static_cast<ImWchar>(*c)) + track;
  }
  return lockupWidth(height, font);
}

// ------------------------------------------------------------------ text ----

namespace {
std::string fitImpl(ImFont* font, float size, const char* text, float maxW) {
  auto width = [&](const char* b, const char* e) { return font->CalcTextSizeA(size, 1e9f, 0, b, e).x; };
  const char* end = text + std::strlen(text);
  if (width(text, end) <= maxW) return text;
  const char* ell = "\xE2\x80\xA6";  // …
  float ew = width(ell, ell + 3);
  if (maxW <= ew) return "";
  // Binary search the longest prefix (on UTF-8 boundaries) that fits with the ellipsis.
  size_t lo = 0, hi = static_cast<size_t>(end - text);
  while (lo < hi) {
    size_t mid = (lo + hi + 1) / 2;
    while (mid > 0 && (static_cast<unsigned char>(text[mid]) & 0xC0) == 0x80) --mid;
    if (mid <= lo) {
      hi = lo;
      break;
    }
    if (width(text, text + mid) + ew <= maxW)
      lo = mid;
    else
      hi = mid - 1;
  }
  while (lo > 0 && (static_cast<unsigned char>(text[lo]) & 0xC0) == 0x80) --lo;
  std::string out(text, lo);
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out + ell;
}
}  // namespace

void textEllipsis(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, float maxW, ImU32 col, const char* text) {
  if (!font) font = ImGui::GetFont();
  if (size <= 0) size = ImGui::GetFontSize();
  std::string s = fitImpl(font, size, text, maxW);
  if (!s.empty()) dl->AddText(font, size, pos, col, s.c_str());
}

const char* fitText(const char* text, float maxW) {
  static std::string buf[4];
  static int slot = 0;
  slot = (slot + 1) & 3;
  buf[slot] = fitImpl(ImGui::GetFont(), ImGui::GetFontSize(), text, maxW);
  return buf[slot].c_str();
}

}  // namespace gl::theme
