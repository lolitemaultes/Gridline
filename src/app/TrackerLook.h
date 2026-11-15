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

}  // namespace gl::trackerview
