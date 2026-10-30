// Gridline — Kerf's own drawing kit (J-181): the pieces of the region-slicer
// editor that no other editor uses: the layered waveform (peak + RMS from a
// cached peak pyramid), tab strips, the deck bar's tool and menu buttons, the
// display-mode glyphs and the empty drop zone (J-182: marker snapping is engine/KerfMarks.h).
// Everything is drawn here as vectors, on the editor's form canvas (iskin::Skin).
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../engine/Sample.h"
#include "InstrumentSkin.h"
#include "Widgets.h"

namespace gl {
class App;
struct PluginTarget;
}

namespace gl::kerf {

// Peak pyramid of a sample's mono mix: per block of kBlock frames the minimum,
// the maximum and the sum of squares (RMS). Built once per sample (UI thread)
// and kept for the few samples on screen.
struct Peaks {
  static constexpr int kBlock = 64;
  std::vector<float> lo, hi, sq;
  size_t frames = 0;
};
const Peaks* peaksOf(const Sample* smp);

// Colours of a waveform layer.
struct WaveStyle {
  ImU32 peak = 0;     // min / max body
  ImU32 rms = 0;      // RMS core (drawn over the body)
  ImU32 edge = 0;     // crisp outline along the peaks (0 = none)
  ImU32 centre = 0;   // zero line (0 = none)
  float gain = 0.46f;  // half height used by full scale (fraction of the rect)
};
// The frames f0..f1 of `smp` across the screen rect, one column per pixel.
void drawWave(ImDrawList* dl, const Sample* smp, ImVec2 p0, ImVec2 p1, double f0, double f1, const WaveStyle& ws);

// Tab strip across a form rect: `n` equal tabs, the current one lit. Disabled
// tabs (enabled[i] false) are dimmed and do not react. Returns the clicked tab or -1.
int tabStrip(App& a, const iskin::Skin& k, const char* id, float x, float y, float w, float h, const char* const* labels, int n, int cur,
             const bool* enabled, const char* const* hints, ImU32 accent);

// Deck bar button with an icon (and a small caret under it for menus); `lit`
// 1 fills it (an active tool), 2 lights only its glyph (a view shown). Returns true when clicked.
bool deckButton(App& a, const iskin::Skin& k, const char* id, float x, float y, float w, float h, ui::Icon icon, const char* hint,
                int lit = 0, bool menu = false, const char* caption = nullptr);

// Display mode glyph: the editor's bands as stacked bars, the hidden ones hollow.
// mode 0 full, 1 no wave editor, 2 no region panel, 3 compact.
void bandGlyph(ImDrawList* dl, ImVec2 p0, ImVec2 p1, int mode, ImU32 on, ImU32 off);

// Empty wave view: a dashed drop zone with a prompt. Returns true when clicked.
bool dropZone(App& a, const iskin::Skin& k, const char* id, float x, float y, float w, float h, ImU32 accent);

// Dashed line (screen px).
void dashedLine(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float dash, float gap, float th = 1.0f);

// ---- J-183: text roles, the layout audit and the reference-style controls ----------------
// Text roles: one size per role (form units), never below kMinTextPx on screen.
enum class Role { Title, Label, Tab, Value, Eyebrow, Body };
inline constexpr float kMinTextPx = 8.5f;
float roleSize(Role r);
// Text in a form box (align 0 left, 1 centre, 2 right), vertically centred, clipped to the box.
// Registered with the audit (its ink against its box). Returns the ink width (screen px).
float text(const iskin::Skin& k, float x, float y, float w, float h, const char* s, Role r, ImU32 col, int align = 0);
// `s` shortened with "..." to fit `w` form units in role `r`.
std::string fit(const iskin::Skin& k, const std::string& s, Role r, float w);

// Layout audit of one frame (filled only while it is on, GRIDLINE_KERF_TRACE): every
// control box with its row, every text run with its box. report() lists overlapping
// controls, controls of one row that do not share top and height, clipped text and text
// smaller than kMinTextPx.
void auditBegin(bool on);
bool auditOn();
void auditBox(const char* id, const iskin::Skin& k, float x, float y, float w, float h, int row = -1);
std::string auditReport();

// Group box (a card) with an optional title at (tx, ty); `light` for the region panel's cards.
void card(const iskin::Skin& k, float x, float y, float w, float h, bool light);
// Flat transport glyph (no tile): a ring on hover, orange when lit.
bool flatButton(App& a, const iskin::Skin& k, const char* id, float x, float y, float w, float h, ui::Icon icon, const char* hint, bool lit);
// LED toggle with a caption (a free bool; true when clicked).
bool ledToggle(App& a, const iskin::Skin& k, const char* id, float x, float y, float w, float h, const char* caption, bool on, const char* hint,
               bool dark = false);
// Small square switch with an icon (tile), lit when on. True when clicked.
bool iconSwitch(App& a, const iskin::Skin& k, const char* id, float x, float y, float w, float h, ui::Icon icon, const char* hint, bool on);
// Segmented selector: n equal cells (text or, when icons[i] != Count, an icon); returns the clicked cell or -1.
int segmented(App& a, const iskin::Skin& k, const char* id, float x, float y, float w, float h, const char* const* labels, const ui::Icon* icons,
              int n, int cur, const char* const* hints, bool dark);

// Pad keyboard (two rows: black-key pads over white-key pads). `lit` (128) tints region keys,
// `mark` is the selected region's key, `glow` (128, optional; J-186) lights the pads of the
// regions sounding now (0..1). Plays the clicked key on the channel; returns the clicked
// note or -1, and the hovered note in *hover.
int padKeyboard(App& a, const PluginTarget& t, const iskin::Skin& k, float x, float y, float w, float h, int low, int whites, const bool* lit,
                int mark, int* hover, const float* glow = nullptr);

}  // namespace gl::kerf
