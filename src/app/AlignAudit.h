// Gridline — J-128 panel alignment audit (--align-audit <file>): on the last
// frame of an off-screen run every ImGui item (through the item hook) and every
// text run (read back from the draw lists) of each visible window is checked by
// AlignCheck.h; the findings go to <file> (TSV: window, rule, px, rect, detail).
#pragma once
#include <cstdint>
#include <string>

namespace gl::alignaudit {

// Starts collecting items for this frame (call after ImGui::NewFrame()).
void begin();
// The kind (align::Kind) of the next item (default: a button); a no-op unless collecting.
void nextKind(uint8_t kind);
// Checks every window drawn this frame (call before ImGui::Render()) and
// writes the findings; returns their number.
int finish(const std::string& path);

}  // namespace gl::alignaudit
