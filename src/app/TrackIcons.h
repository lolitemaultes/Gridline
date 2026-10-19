// Gridline — in-house mixer track icons (MixerTrack::icon), drawn as vectors.
#pragma once
#include "../../third_party/imgui/imgui.h"

namespace gl::trackicons {

int count();                  // icons 1..count() (0 = none)
const char* name(int icon);   // "Kick", "Vocal", …
// Draws icon `icon` centred at `c`, fitting a `size` px square.
void draw(ImDrawList* dl, int icon, ImVec2 c, float size, ImU32 col);
// Icon picker grid inside a menu; returns true and sets *out when one is picked.
bool picker(int current, int* out);

}  // namespace gl::trackicons
