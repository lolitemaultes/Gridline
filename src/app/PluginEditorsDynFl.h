// Gridline — dynamics editors laid out on the reference forms' geometry with
// per-plugin palettes (κ2): Limiter, Compressor, Soft Clipper, Transient
// Shaper, Multiband Compressor. Shared helpers for the pack editors too.
#pragma once
#include <cstdint>
#include <vector>
#include "PluginSkin.h"

namespace gl {
class App;

// Returns false for types it does not draw.
bool drawDynamicsFlEditor(App& a, const PluginTarget& t, float width);

namespace dynfl {

// Live meters of a mixer-slot effect (zeros for channel targets).
struct LiveMeters {
  float gr = 0, in = -120, b[4] = {0, 0, 0, 0};
};
LiveMeters liveMeters(App& a, const PluginTarget& t);

// Scrolling history display (one column per tick) with up to 6 lanes.
struct ScrollHistory {
  static constexpr int kCols = 640, kLanes = 6;
  float v[kLanes][kCols];
  int head = 0;
  float acc = 0;
  bool frozen = false;
  ScrollHistory();
  // Advances by speed (0..1, 0.5 = default) and records the lanes' values.
  void push(float speed, const float* lanes, int n);
  float at(int lane, int age) const { return v[lane][(head - age + kCols * 2) % kCols]; }
};
ScrollHistory& history(uint64_t key);

// Per-editor UI state (view toggles, speed, spare A/B state).
struct EditorUi {
  bool vis[8] = {true, true, true, true, true, true, true, true};
  float speed = 0.5f;
  int page = 0;
  std::vector<float> spare;
};
EditorUi& editorUi(uint64_t key);

}  // namespace dynfl
}  // namespace gl
