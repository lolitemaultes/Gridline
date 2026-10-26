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
// Draws the embedded SVG `svgIndex` as large as fits its ink (the texels it
// actually covers, stroke included, as rasterised at this density) inside the
// logical-px box `inkMin`..`inkMax`, the ink centred on the box. Used where an
// SVG replaces an older glyph: pass the old glyph's ink bounds and the new one
// never draws bigger. Tinted `col`, pixel-snapped like draw(). When no size
// fits (a glyph a few pixels across) the smallest is drawn, or with
// `onlyIfFits` nothing, and false is returned so the caller draws the old glyph.
bool drawFit(ImDrawList* dl, int svgIndex, ImVec2 inkMin, ImVec2 inkMax, ImU32 col, bool onlyIfFits = false);
bool drawFit(ImDrawList* dl, const char* svgName, ImVec2 inkMin, ImVec2 inkMax, ImU32 col, bool onlyIfFits = false);
// The ink of font glyph `ch` drawn at `pos` (as AddText places it), logical px:
// the texels of the font atlas at least half covered. False when not visible.
bool glyphInk(ImFont* font, float size, ImWchar ch, ImVec2 pos, ImVec2* inkMin, ImVec2* inkMax);
// Draws an SVG authored on an older glyph's unit grid: one old unit is 10 SVG
// units around the viewBox centre (12, 12), so SVG x = 12 + 10 * u. `unitPx` is
// the old unit in logical px and `oldStroke` the old line width (0 for filled
// glyphs): the icon is fitted (drawFit) to the old glyph's ink, its geometry
// bounds at that scale plus 0.35 of the old stroke, so it lands where the old
// glyph was and never draws bigger, whatever the SVG stroke rule makes of it.
void drawGlyph(ImDrawList* dl, const char* svgName, ImVec2 c, float unitPx, float oldStroke, ImU32 col);
// The current ImGui context's atlas (created on first use). Creating it also
// installs the context's ImGui glyph hook (GRIDLINE PATCH in imgui_internal.h):
// ImGui's own arrows, check marks, close crosses and step "-" / "+" are then
// drawn as SVG icons (caret-*, check, x, minus, plus) in their old ink bounds.
// Call it once when a context is created so the first frame has the hook.
Atlas& atlas();
// Number of atlas page textures registered with the current context.
int textureCount();

}  // namespace gl::icons
