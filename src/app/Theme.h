// Gridline — visual theme from the logo guide v1.0: a monochrome interface
// on the approved surfaces (950/900/800 dark, 100 light), Inter for text and
// JetBrains Mono for numbers. Colour comes from user content (channel,
// pattern and clip colours); the interface itself stays Ink/Paper and greys.
// The accent (#3D63DD) is reserved for focus, selection and value arcs.
//
// ---------------------------------------------------------------------------
// Design system (all sizes are logical pixels; the whole UI is scaled as one
// by the interface-scale setting / display DPI in main.cpp):
//
//   Spacing   4 · 8 · 12 · 16 · 24        theme::kS1 … kS6
//   Radius    4 small (steps, chips) · 6 controls · 8 panels/windows
//   Heights   24 control (buttons, fields, combos, tabs, toolbar icons)
//             20 small control (inline icon buttons, row widgets)
//             36 window toolbar strip      26 status bar      48 top bar
//             (top-bar panels 36, top-bar icon buttons 28)
//   Type      Title   Inter SemiBold 16    (dialog titles, inspector heads)
//             Body    Inter Regular 14     (everything else)
//             Caption Inter Regular 12     (secondary text, knob labels)
//             Label   JetBrains Mono 11, upper case (section labels)
//             Number  JetBrains Mono 12/13 (values), Bold 18 (LCD readouts)
//   Lines     `line` for separators/borders, `lineStrong` for emphasis
//   States    hover = one surface step up, pressed = two steps,
//             active/on = Paper fill + Ink text (or accent where it means
//             "running"/"selected"), focus = 2 px accent ring
//   Brand     Mark 28 px as the main-menu button (hover 0.8), empty states
//             80 px at 0.5, lockup only in splash/about (56 px, Paper on 950)
// ---------------------------------------------------------------------------
#pragma once
#include <cstdint>
#include "../../third_party/imgui/imgui.h"

namespace gl::theme {

// ---- spacing scale ----
constexpr float kS1 = 4.0f, kS2 = 8.0f, kS3 = 12.0f, kS4 = 16.0f, kS6 = 24.0f;
// ---- radii ----
// Tight radii (studio look: square-ish buttons, slightly rounded panels).
constexpr float kRadiusSm = 2.0f, kRadius = 3.0f, kRadiusLg = 4.0f;
// ---- heights ----
constexpr float kControlH = 24.0f;   // buttons, fields, combos, tabs, toolbar icon buttons
constexpr float kControlSmH = 20.0f; // inline icon buttons
constexpr float kToolbarH = 36.0f;   // per-window toolbar strip
constexpr float kTopBarH = 48.0f;
constexpr float kStatusBarH = 26.0f;
constexpr float kIconBtn = 28.0f;    // top-bar icon button edge
// ---- type scale ----
constexpr float kFontTitle = 16.0f, kFontBody = 14.0f, kFontCaption = 12.0f, kFontLabel = 11.0f;
constexpr float kFontNumber = 12.0f, kFontLcd = 18.0f;

struct Palette {
  ImU32 surface950, surface900, surface800, surface700, surface600;
  ImU32 line, lineStrong;
  ImU32 text, textDim, textFaint;
  ImU32 paper, ink;        // logo / strongest contrast
  ImU32 accent;            // selection & focus (#3D63DD)
  ImU32 danger;            // record, clipping, destructive (#E5484D)
  ImU32 stepOff, stepOffAlt, stepOn;
  ImU32 gridBar, gridBeat, gridStep;
  ImU32 playhead;
  ImU32 meterLow, meterMid, meterHigh;
  ImU32 logo;              // Paper on dark surfaces, Ink on light (guide §03)
  ImU32 chrome;            // top/status bar and dock background (approved logo surface)
  // Studio colour roles (J-13): the main windows follow the reference DAW's
  // role scheme — slate panels, a lit-glyph orange, green LEDs, light knob rings.
  ImU32 hot;               // lit glyphs, active toggles, pressed knob rings
  ImU32 led;               // LED "on" (mute lamps, select lamps, effect enable)
  ImU32 well;              // recessed editors: playlist / piano-roll grids, step wells
  ImU32 field;             // input fields, LCDs, combo boxes, number boxes
  ImU32 knobFill;          // knob value ring at rest
  ImU32 keyWhite, keyBlack;  // piano-roll keyboard
  bool dark;
};

// Per-tool glyph colours (the reference editors give each tool its own lit
// colour). Used as IconButton activeCol for the editors' tool strips.
namespace role {
constexpr uint32_t kDraw = 0xFFBC40, kPaint = 0x77C5F2, kDelete = 0xFF5B53, kMute = 0xFF53A9, kSlip = 0xFFA04A,
                   kSlice = 0x80ACFF, kSelect = 0xFFAB60, kZoom = 0x80ACFF, kPlay = 0xB1E758, kSnap = 0x7AE4A3,
                   kStamp = 0xD963F1, kLoop = 0xA8E44A, kGraph = 0x4ECCFF, kMenu = 0xFF9637, kView = 0xFB9244,
                   kGhost = 0x80ACFF;
}  // namespace role

const Palette& pal();
// Menu / combo / tooltip panel colour for a palette (ImGuiCol_PopupBg).
ImU32 popupBg(const Palette& p);
// Temporarily replaces the palette (plugin editor skins); pops restore it. Nestable.
void pushPalette(const Palette& p);
void popPalette();
void apply(bool dark);  // sets ImGui style + palette
bool isDark();

ImU32 withAlpha(ImU32 c, float a);
ImU32 mix(ImU32 a, ImU32 b, float t);
ImU32 rgb(uint32_t rrggbb, float alpha = 1.0f);
ImVec4 vec(ImU32 c);
// A role colour (lit glyph, tool colour, LED) as it should be drawn on the
// current theme's faces: unchanged on dark slate, deepened on the light variant.
ImU32 lit(ImU32 role);
ImU32 litRole(uint32_t rrggbb, float alpha = 1.0f);
// A user content colour (channel / track / insert colour) as drawn on chrome:
// unchanged on dark slate; on the light variant neutral dark greys (the
// default "no colour" tracks and inserts) are lifted so headers don't read heavy.
ImU32 content(uint32_t rrggbb, float alpha = 1.0f);
// Readable text colour (ink/paper) on a given background.
ImU32 contrastText(ImU32 bg);

// Optional provider of rasterised marks (white, true transparent holes) at an
// exact pixel size, installed by main.cpp once the GL context exists. Without
// it the mark is drawn as vectors (holes painted with `bg`).
using MarkTextureFn = ImTextureID (*)(int pixels);
void setMarkTextureProvider(MarkTextureFn fn);
// The Tile mark (guide §05), size = edge length in px. `bg` is the colour
// behind the mark (only used by the vector fallback). opacity per guide §04.
void drawMark(ImDrawList* dl, ImVec2 pos, float size, ImU32 fg, ImU32 bg, float opacity = 1.0f);
// Horizontal lockup (guide §01, master SVG proportions): mark + "gridline" set
// in Inter Bold, height = mark size. Returns width used. Pass the Inter Bold font.
float drawLockup(ImDrawList* dl, ImVec2 pos, float height, ImU32 fg, ImU32 bg, ImFont* font, float opacity = 1.0f);
float lockupWidth(float height, ImFont* font);

// Text that never spills: drawn clipped to maxW with a trailing ellipsis.
void textEllipsis(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, float maxW, ImU32 col, const char* text);
// Ellipsised copy of `text` that fits maxW at the current font.
const char* fitText(const char* text, float maxW);

}  // namespace gl::theme
