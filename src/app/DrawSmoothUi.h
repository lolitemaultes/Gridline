// Gridline - J-99: smooth freehand drawing in editors (ImGui glue over
// core/DrawSmooth.h).
//
// Every surface where levels, a curve, a wave or a table is painted with the
// mouse calls drawui::stroke() each frame. It snapshots the surface's values
// when the left button goes down, fills every cell between pointer samples,
// filters the pointer and runs the edge-preserving pass over the painted
// cells only, and returns the values to show and commit. Holding Alt draws raw
// (gaps are still filled, nothing is smoothed). Settings: one amount for all
// surfaces (Settings > Interface > Drawing, "draw.smoothAmount") and an on/off
// per surface ("draw.smooth.<surface>", the small wave chip in the surface's
// top-right corner, on by default).
//
// Undo: the stroke closes the undo merge when it starts and when it ends, so
// commit every frame of the stroke with one merge key (setPluginState(...,
// key) / editChannel(..., key)) and the whole stroke is one undo step.
#pragma once
#include <cstdint>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../core/DrawSmooth.h"

namespace gl {
class App;
}

namespace gl::drawui {

struct Spec {
  const char* surface = "";  // preference id (core/DrawSmooth.h kSurfaces)
  bool cyclic = false;       // single-cycle wave (the pass wraps around)
  float lo = 0.0f, hi = 1.0f;
  drawsmooth::Axis axis;     // cell centres along the axis (empty = uniform)
  ImVec2 p0{0, 0}, p1{0, 0}; // surface rect: a press on its smooth chip draws nothing
  bool raw = false;          // draw raw this frame too (e.g. a quantizing modifier is held)
  int chipCorner = 0;        // where the chip sits: 0 top-right, 1 bottom-right, 2 top-left
  // Surfaces that start drawing only past a drag threshold: the press point
  // (x, v), painted first so the stroke starts where the button went down.
  bool seedPress = false;
  double pressX = 0, pressV = 0;
};

// Per-surface switch and the shared amount.
bool enabled(const App& a, const char* surface);
void setEnabled(App& a, const char* surface, bool on);
double amount(const App& a);
// Alt held: draw raw.
bool rawModifier();

// Drives one freehand stroke over n cells of the item `id`. `drawing`: the
// left button is held on the surface this frame; x: pointer along the axis
// (0..1); v: value under the pointer. `current` is read when the stroke
// starts. Returns the values to show and commit while a stroke runs (they
// change every frame the pointer moves), else nullptr.
const std::vector<float>* stroke(App& a, ImGuiID id, bool drawing, double x, double v, const float* current, int n, const Spec& spec);
// Which cells the running stroke has painted (empty when none runs).
const std::vector<uint8_t>& strokeTouched();
// True while `id` has a stroke running.
bool strokeActive(ImGuiID id);

// The smooth chip in the top-right corner of the surface rect (p0..p1):
// a click toggles the surface's smoothing. Call after the surface is drawn.
void chip(App& a, const char* surface, ImVec2 p0, ImVec2 p1, int corner = 0);
// The chip's rect for a surface rect.
void chipRect(ImVec2 p0, ImVec2 p1, ImVec2& c0, ImVec2& c1, int corner = 0);

}  // namespace gl::drawui
