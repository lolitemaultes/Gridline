// Gridline — Kerf's own drawing kit (J-181): the pieces of the region-slicer
// editor that no other editor uses: the layered waveform (peak + RMS from a
// cached peak pyramid), tab strips, the deck bar's tool and menu buttons, the
// display-mode glyphs, the empty drop zone and marker snapping.
// Everything is drawn here as vectors, on the editor's form canvas (iskin::Skin).
#pragma once
#include <cstdint>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../engine/Sample.h"
#include "InstrumentSkin.h"
#include "Widgets.h"

namespace gl {
class App;
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
             const bool* enabled, const char* const* hints, ImU32 accent, float size = 7.5f);

// Deck bar button with an icon (and a small caret under it for menus); `lit`
// 1 fills it (an active tool), 2 lights only its glyph (a view shown). Returns true when clicked.
bool deckButton(App& a, const iskin::Skin& k, const char* id, float x, float y, float w, float h, ui::Icon icon, const char* hint,
                int lit = 0, bool menu = false, const char* caption = nullptr);

// Display mode glyph: the editor's bands as stacked bars, the hidden ones hollow.
// mode 0 full, 1 no wave editor, 2 no region panel, 3 compact.
void bandGlyph(ImDrawList* dl, ImVec2 p0, ImVec2 p1, int mode, ImU32 on, ImU32 off);

// Empty wave view: a dashed drop zone with a prompt. Returns true when clicked.
bool dropZone(App& a, const iskin::Skin& k, const char* id, float x, float y, float w, float h, ImU32 accent);

// Marker snapping: 0 off, 1 the nearest zero crossing (within 5 ms), 2 the
// sixteenth grid of the loop (`beats` long). Frames in, frames out.
int64_t snapFrame(const Sample* smp, int64_t f, int mode, double beats);

// Dashed line (screen px).
void dashedLine(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float dash, float gap, float th = 1.0f);

}  // namespace gl::kerf
