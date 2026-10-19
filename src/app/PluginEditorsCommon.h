// Gridline — helpers shared by the built-in plugin editors (PluginEditors*.cpp).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "../core/PluginParams.h"
#include "App.h"
#include "PluginUI.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl::ed {

inline float val(const PluginState& s, int k, float d = 0) {
  return k >= 0 && static_cast<size_t>(k) < s.params.size() ? s.params[static_cast<size_t>(k)] : d;
}
inline int ival(const PluginState& s, int k, int d = 0) { return static_cast<int>(std::lround(val(s, k, static_cast<float>(d)))); }
inline bool bval(const PluginState& s, int k, bool d = false) { return val(s, k, d ? 1.f : 0.f) >= 0.5f; }

inline std::string noteName(int pitch) {
  static const char* nm[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  return std::string(nm[((pitch % 12) + 12) % 12]) + std::to_string(pitch / 12 - 1);
}

// Draws text with the mono font at a size.
inline void monoText(ImDrawList* dl, ImVec2 p, ImU32 col, const char* text, float size = 10.0f) {
  ImGui::PushFont(app().fontMono, size);
  dl->AddText(p, col, text);
  ImGui::PopFont();
}

// Small oscillator / LFO shape icons (0 sine, 1 triangle, 2 saw, 3 square, 4 noise,
// 5 saw down, 6 pulse 25%, 7 random steps).
void waveIcon(ImDrawList* dl, int shape, ImVec2 c, float w, float h, ImU32 col, float thick = 1.5f);
// Response curve of an SVF-style filter for drawing (mode 0 LP, 1 BP, 2 HP, 3 notch), in dB.
double svfResponseDb(int mode, double cutoff, double reso01, double hz, double sr);

}  // namespace gl::ed
