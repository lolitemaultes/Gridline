// Gridline - the Tracker layout's drawing pieces (K-106), shared by its header band
// (TrackerFrame.cpp), the pattern editor and its pages (TrackerPages.cpp): bevelled boxes and
// buttons in the theme's roles, numbers in the monospaced face, two-digit hex numbering.
#pragma once
#include "../../third_party/imgui/imgui.h"

namespace gl {
class App;
}

namespace gl::trackerview {

ImFont* monoFont(App& a);
// A bevelled box: a light edge top and left, a dark one bottom and right (sunken: the other way).
void bevelBox(ImDrawList* dl, ImVec2 p0, ImVec2 p1, bool sunken = false);
// A bevel button: raised; pressed (inset) while held; lit when `on`; a lighter face on hover.
bool bevelButton(App& a, const char* id, const char* label, ImVec2 p0, ImVec2 p1, bool on = false, const char* hint = nullptr);
// A box title in the bold face.
void boxTitle(App& a, ImVec2 p, const char* text);
// A small arrow button (a filled triangle on a bevel face); repeats while held.
bool arrowButton(App& a, const char* id, ImVec2 p0, ImVec2 p1, bool up, const char* hint);
// One stepper row of width `w`: the label, the value in a sunken field after it (at `labelW` from
// the left; < 0: 42 percent), the up and down arrows at the right. Returns the step asked for: +1 or
// -1 from an arrow or the wheel, more while the value is dragged (right or up: more). `fill`: the
// value's field runs from the label column to the arrows.
int stepper(App& a, const char* id, const char* label, const std::string& value, ImVec2 p0, float w, float h, const char* hint, float labelW = -1,
            bool fill = false);

}  // namespace gl::trackerview
