// Gridline - the layout style report (K-112, the Studio golden's first level).
//
// A plain text description of what the layout puts on screen, deterministic on every machine:
// every ImGui style size, a hash of the style colours, the top and status bar heights, the dock
// tree (node rectangles, split axes, the windows each node holds) and the open windows. The
// Studio report at a fixed window size is committed (tests/k112/studio_style_report.txt), so a
// change that moves Studio by a pixel fails a test on every machine, before any screenshot.
#pragma once
#include <string>

namespace gl {
class App;
}

namespace gl::layoutui {

// Call during a frame, after the dock space and windows were submitted.
std::string styleReport(App& a);

}  // namespace gl::layoutui
