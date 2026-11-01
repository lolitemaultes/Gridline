// Gridline — "edison" pack: Loupe's controls and metrics (J-207). One set of
// sizes for the whole editor, taken from Gridline's design system (Theme.h:
// 4 / 8 px spacing, 24 px controls, 3 px radii, mono 11 px labels) and drawn
// with the shared skin faces (raisedFace / insetField), so every button of a
// group has one size, every row one centre line and every gap one width.
//
// Sizes are logical px at the design width (kDesignW); a narrower editor (the
// window capped to a small screen) scales them all by one factor, rounded to
// whole pixels so equal controls stay equal.
#pragma once
#include <cstdint>
#include <string>
#include "../../app/Widgets.h"
#include "../../../third_party/imgui/imgui.h"

namespace gl::packs::edison::sty {

constexpr float kDesignW = 1100;  // the editor width the metrics are given at
// ---- metrics (px at the design width)
constexpr float kPad = 8;         // outer margin, and the gap between rows
constexpr float kCellGap = 2;     // between buttons of one group
constexpr float kFieldGap = 4;    // between fields of the info line
constexpr float kGroupGap = 8;    // between groups of a row
constexpr float kSectionGap = 16; // between sections of a row (transport | record options)
constexpr float kBtn = 28;        // icon buttons of the head (transport, tools, mute)
constexpr float kIcon = 16;       // their glyph box
constexpr float kField = 24;      // combos, info fields, menu cells
constexpr float kBarH = 28;       // the editor's bottom bar
constexpr float kBarCtl = 22;     // its controls
constexpr float kBarIcon = 14;    // their glyph box
constexpr float kRulerH = 20, kScrollH = 12, kSideW = 20;
constexpr float kMeterW = 6;
constexpr float kRadius = 3;
// type (px at the design width)
constexpr float kFontMenu = 13;   // menu words (Inter)
constexpr float kFontValue = 12;  // field values, ruler labels (JetBrains Mono)
constexpr float kFontCap = 11.5f; // captions in caps (JetBrains Mono, dim)

// The editor's scale: metric `v` in screen px (whole pixels), font sizes.
struct Metrics {
  float s = 1;
  float px(float v) const;
  float font(float v) const;  // half-pixel steps, never under 8 px
};
Metrics metricsFor(float width);

// Button look options.
struct Btn {
  bool lit = false;       // latched / active
  bool enabled = true;
  uint32_t litRole = 0;   // lit glyph colour (0xRRGGBB, 0 = the lit-glyph orange)
  const char* hint = nullptr;
  bool* right = nullptr;  // set on a right click
  bool flat = false;      // no face at rest (menu words)
  bool mark = true;       // lit: a short bar under the glyph
};

// Icon button over the screen rect [a, b] (whole pixels). True on a left click.
bool iconButton(const char* id, ImVec2 a, ImVec2 b, ui::Icon icon, float iconPx, const Btn& o);
// Text button (label centred, caps font when `caps`).
bool textButton(const char* id, ImVec2 a, ImVec2 b, const char* label, ImFont* font, float fs, const Btn& o);
// Two-way (or n-way) segmented switch: equal cells, the selected one lit. Returns the clicked cell or -1.
int segmented(const char* id, ImVec2 a, ImVec2 b, const char* const* labels, int n, int sel, ImFont* font, float fs,
              const char* const* hints = nullptr);
// Recessed field with a dim caption on the left and the value on the right (or centred when no caption).
void field(ImDrawList* dl, ImVec2 a, ImVec2 b, const char* caption, const char* value, ImU32 valueCol, ImFont* font, float fs,
           float capFs, int align = 2);
// Drop-down field: the value and a chevron; true on click (the caller opens its popup).
bool dropField(const char* id, ImVec2 a, ImVec2 b, const char* value, ImFont* font, float fs, bool enabled, const char* hint,
               ui::Icon lead = ui::Icon::Count);
// LED check box with a caption to its right, vertically centred on cy.
bool check(const char* id, ImVec2 tl, float box, float w, const char* caption, bool on, bool enabled, ImFont* font, float fs,
           const char* hint);
// Caption text (caps, dim) with its left edge at x, centred on cy.
void caption(ImDrawList* dl, float x, float cy, const char* text, ImU32 col, ImFont* font, float fs);
// Text centred vertically on cy at x (align 0 left, 1 centre on x, 2 right edge at x).
void textAt(ImDrawList* dl, ImFont* font, float fs, float x, float cy, ImU32 col, const char* text, int align = 0);
float textW(ImFont* font, float fs, const char* text);

// Dev check (J-207): GRIDLINE_EDISON_RECTS=<file> writes every widget rect of a
// frame (group, row, name, x0, y0, x1, y1) after frame 25, for the alignment checks.
void note(const char* group, const char* name, ImVec2 a, ImVec2 b);
void frameEnd();

}  // namespace gl::packs::edison::sty
