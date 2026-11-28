// Gridline — the Tracks layout's (Smooth's) check boxes, radio buttons and progress bars: a ticked box and a
// selected radio are filled with the layout's blue and marked in white, as its reference windows draw
// them. Kept apart from LayoutStyleUi so a headless test can draw them as the layout does.
#pragma once
#include "../../third_party/imgui/imgui.h"

namespace gl::smoothtoggles {

// The blue a ticked box and a selected radio are filled with.
ImU32 accent();
// The colours apply() replaces, for restore() (and for windows that keep the base look).
struct Saved {
  ImVec4 mark, selected, bar;  // CheckMark, CheckboxSelectedBg, PlotHistogram (progress bars)
};
// The style's colours and the toggle globals as Smooth draws them; a progress bar fills with the
// same blue (it was the theme's text colour, near black on the light theme).
void apply(ImGuiStyle& st, Saved& saved);
// The style's colours put back, the globals off.
void restore(ImGuiStyle& st, const Saved& saved);

}  // namespace gl::smoothtoggles
