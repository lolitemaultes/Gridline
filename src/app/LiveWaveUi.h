// Gridline — drawing a recording in progress (K-145): one lane of a LiveWave as a filled
// outline, in the colours of the view that calls it (Loupe's lanes, Seltmann's recorder).
// See engine/LiveWave.h for the model, the fit and the trace.
#pragma once
#include "../../third_party/imgui/imgui.h"
#include "../engine/LiveWave.h"

namespace gl::livewaveui {

struct Style {
  ImU32 wave = 0;    // the outline's fill
  ImU32 centre = 0;  // the zero line (0: none)
  ImU32 head = 0;    // the head line at the drawn end (0: none)
  float headWidth = 2;
  float gain = 1;    // vertical zoom (1: full scale fills half the lane)
  float fill = 0.46f;  // half height of full scale, as a fraction of the lane
  int channel = 0;   // 0 / 1, -1 the mix of both, -2 mirrored absolute peaks of both
};

// Draws lane [p0, p1] of `w` for the view `v` (v.width is set from the lane). Returns the
// head's x (p0.x when nothing is drawn).
float drawLane(ImDrawList* dl, const LiveWave& w, ImVec2 p0, ImVec2 p1, LiveWaveView v, const Style& st);

}  // namespace gl::livewaveui
