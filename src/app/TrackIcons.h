// Gridline — track icons on screen (mixer tracks, playlist tracks, channels):
// the set itself (ids, names, categories, FL numbers) is core/TrackIconSet.h.
#pragma once
#include "../../third_party/imgui/imgui.h"

namespace gl::trackicons {

// Translated name of icon `icon` ("None" for 0 / unknown).
const char* name(int icon);
// Draws icon `icon` centred at `c` in a `box` px square (the SVG's 24-unit
// grid, snapped to a standard size: 12, 14, 16, 20, ...). The ink stays inside
// the box, so a caller that keeps the box clear never clips the icon.
void draw(ImDrawList* dl, int icon, ImVec2 c, float box, ImU32 col);
// The box to use for an icon in a row `rowH` px tall (never larger than `maxBox`).
float boxFor(float rowH, float maxBox = 16.0f);
// Categorised icon picker (search field, category list, icon grid) inside a
// menu or popup; returns true and sets *out (0 = none) when one is picked.
bool picker(int current, int* out);
// Dev check (--menu icons...): the next picker to open starts with this search
// text and category (-1 every icon, -2 its usual one).
void devPreset(const char* search, int category);

}  // namespace gl::trackicons
