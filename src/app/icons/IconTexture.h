// Gridline — Q-13 interface icons on screen: the atlas pages as ImGui textures
// and tinted, pixel-snapped quads drawn from them.
//
// Each ImGui context keeps its own atlas (plugin windows may sit on screens of
// different density). The density is read from io.DisplayFramebufferScale
// (UI scale x display density): when it changes the atlas is cleared and the
// icons are rasterised again at their new device size, once. Drawing never
// rasterises an icon that is already in the atlas.
#pragma once
#include "../../../third_party/imgui/imgui.h"
#include "IconAtlas.h"

namespace gl::icons {

// Standard logical icon sizes (px at UI scale 1): nearest of 10, 12, 14, 16,
// 20, 24, 28, 32, 40, 48, 64 (anything larger is rounded to whole pixels).
float standardSize(float logicalPx);
// J-77: the SVG box of a top-bar toggle glyph (metronome, typing keyboard, ...)
// in a `button` px square: a glyph of 0.52 x the button, SVG box 1.2 x that.
// 28 px button -> 16 px box.
float toggleIconBox(float button);
// J-77: the SVG box of an editing tool (playlist / piano roll / automation
// strips) in a `button` px square: sized like the top-bar toggles (16 px in
// the 24 px strip buttons, 4 px clear on every side; 20 at 32, 24 at 36).
float toolIconBox(float button);
// Draws the embedded SVG `svgIndex` centred at `c` (logical px) in a box of
// `logicalPx` (the 24-unit grid), tinted `col`. The quad lands on whole
// device pixels and maps texels 1:1, so the glyph stays crisp.
void draw(ImDrawList* dl, int svgIndex, ImVec2 c, float logicalPx, ImU32 col);
void draw(ImDrawList* dl, const char* svgName, ImVec2 c, float logicalPx, ImU32 col);
// The current ImGui context's atlas (created on first use).
Atlas& atlas();
// Number of atlas page textures registered with the current context.
int textureCount();

}  // namespace gl::icons
