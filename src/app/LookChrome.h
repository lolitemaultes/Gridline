// Gridline - the layouts' chrome control kinds (K-112, core/LayoutStyle.h), drawn in the
// person's control style. A layout picks the kind (a stepper, a line-capped fader, a bevel
// button, a segmented meter, a fill-as-slider field); these drawers build each kind from the
// control style's own elements and tokens (look::knob, look::slider, look::button, ...), so
// every kind comes in every control style's finish and no colour is chosen here.
//
// Studio's kinds draw exactly what the app has always drawn. Plugin editors never use these.
#pragma once
#include "../core/LayoutStyle.h"
#include "Look.h"

namespace gl::look::chrome {

void knob(ImDrawList* dl, ImVec2 c, float r, const Knob& k, layout::KnobKind kind, const char* valueText = nullptr);
void fader(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Fader& f, layout::FaderKind kind);
void button(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* label, int state, layout::ButtonKind kind, ImU32 role = 0);
void meter(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const Meter& m, layout::MeterKind kind);
// A value field: Studio = look::field; FillSlider = the field with its value as a fill; Stepper = "< v >".
void field(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* text, float norm, int state, layout::FieldKind kind);

// Dev gallery page 3 (`--gallery <look>:3`): every layout's chrome kinds in the given control style.
void drawGalleryPage(Dir d, double t);

}  // namespace gl::look::chrome
