// Gridline — J-130 plugin wordmarks: the "Tile" text logo of every built-in
// plugin and of the two suites, drawn sharp at any UI scale and density.
//
// Sources: assets/wordmarks/*.svg (tools/wordmarks/make_wordmarks.py), 24 units
// tall, embedded at build time (cmake/EmbedWordmarks.cmake). Key "gl.<id>" is
// the wordmark of a plugin type, "suite" / "suite-mini" the suite logos.
//
// Each shape's id names its colour role, so the caller tints the logo from the
// current theme instead of baking colours in:
//   mark*  the family-coloured tile (and the suite's "mini")
//   cut*   cut out of the mark: drawn as true holes, whatever the surface
//   word*  the name (theme text)       dim*  secondary text ("suite")
// Every (wordmark, device height) is rasterised once with nanosvg, 3x3
// supersampled, into one texture (one band per role) and cached per ImGui
// context; a density change (UI scale, display) rasterises again at the new
// device height, so nothing is ever drawn from a scaled bitmap.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "../../../third_party/imgui/imgui.h"

namespace gl::icons::wordmark {

struct Source {
  const char* key;  // "gl.<id>", "suite", "suite-mini"
  const char* svg;
};
// Every embedded wordmark (generated from assets/wordmarks/*.svg).
const Source* embedded(int* count);
// SVG text of `key`, or nullptr.
const char* find(const std::string& key);
// The <title> of an SVG (the display name it was made for), "" when none.
std::string title(const char* svg);
// The mark layer's authored colour (#RRGGBB, opaque ImU32), 0 when it has none.
ImU32 markColour(const std::string& key);

// Coverage planes of one wordmark rasterised `deviceHeight` pixels tall.
struct Raster {
  int w = 0, h = 0;
  std::vector<uint8_t> mark, word, dim;  // w * h each; mark already has the cut-outs removed
};
// False when the SVG does not parse.
bool rasterize(const char* svg, int deviceHeight, Raster& out);

// Colours of the three roles (the caller takes them from the theme).
struct Colours {
  ImU32 mark = 0, word = 0, dim = 0;
};
// Logical size of `key` drawn `height` logical px tall at the current
// context's density; false when there is no such wordmark.
bool size(const std::string& key, float height, ImVec2* out);
// Draws `key` with its top-left at `pos` (snapped to device pixels), `height`
// logical px tall. False when there is no such wordmark.
bool draw(ImDrawList* dl, const std::string& key, ImVec2 pos, float height, const Colours& c);
// Wordmarks rasterised in the current context so far (tests / diagnostics).
int cachedCount();

}  // namespace gl::icons::wordmark
