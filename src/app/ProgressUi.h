#pragma once
// Gridline — steady progress windows (0.7.8). A window that shows a job's progress (saving,
// opening, rendering, exporting, downloading) keeps one size and one place for its whole life:
// every line is fitted to a fixed width (long names cut with an ellipsis), the numbers stand in
// fixed fields sized for their widest value ("100%"), and a line that may show up later (a
// stalled job's warning) has its row from the start. These helpers draw those pieces.
//
// For tests (dev-input "progsize"): each progress window notes its rect after Begin (note), and a
// stand-in state (fake) drives it through any progress and any name without a real job.
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"

namespace gl {
class App;
}

namespace gl::progui {

// Width of the widest percent the windows show ("100%") in the current font.
float PercentWidth();
// `text` right-aligned in a field `w` wide that starts at the cursor (cut with an ellipsis when
// wider): what follows never moves while the value changes. One item of exactly `w` x a row.
void RightField(const char* text, float w, ImU32 col);
// The whole percent of `frac` (0..1, floored: 100% only when done) right-aligned in PercentWidth().
void Percent(double frac, ImU32 col);
std::string PercentText(double frac);
// One row, always: `text` fitted to `w` (ellipsis at the end, or in the middle for a path; the
// whole text on hover when cut), or an empty row when there is no text. Exactly `w` wide.
void Line(const char* text, float w, ImU32 col, bool path = false);
// `rows` rows of `w` reserved at the cursor, `text` wrapped inside them (cut past the last row);
// empty text leaves the rows blank. The window never grows or shrinks for it.
void WrappedRows(const char* text, float w, int rows, ImU32 col);
// The height of `rows` text rows as WrappedRows reserves them.
float RowsHeight(int rows);

// ---- tests and screenshots

// Right after Begin of a progress window: notes its rect under `name` (printed by "progsize").
void note(const char* name);
// A stand-in state for the progress window `name` (dev-input "progsize show"), or null.
struct Fake {
  std::string popup;
  double frac = 0;
  bool stalled = false;
  std::string text;  // the name or status line the window shows (any length)
};
const Fake* fake(const char* name);
// dev-input: progsize show <popup> <frac> <stalled 0|1> "<text>" · off · trace on|off · rects
// With trace on, every frame each noted window prints "progsize frame <n> <popup> x y w h".
void devCommand(App& a, const std::vector<std::string>& args, int frame);
// The popups that take a stand-in state (the test drives each one).
const std::vector<std::string>& popups();

}  // namespace gl::progui
