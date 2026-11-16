// Gridline - K-315: the Tracker About screen's banner: "gridline" in big chrome 3D letters, spinning
// and morphing (into sine worms, the Gridline mark, particles, a twist, a wireframe) over copper bars
// and scanlines, in the manner of an old demo or tracker intro. Geometry and timing in
// ChromeBannerGeom.h.
//
// Cost: the shapes are built once; the animation is worked out at most 30 times a second into a
// fixed vertex cache, and the frames between only copy that cache into the draw list. Nothing runs
// unless the About screen draws the banner. No allocation per frame, nothing saved anywhere.
// Under reduced motion (Interface animations off, or the system preference) it holds still.
#pragma once
#include <string>
#include <vector>

#include "../../third_party/imgui/imgui.h"

namespace gl::chromebanner {

// The banner's height for an About band of height `bandH` (what the lines under it leave).
float heightFor(float bandH, float fontSize);
// Draws the banner in [p0, p1]; a click on it skips to the next morph.
void draw(ImDrawList* dl, ImVec2 p0, ImVec2 p1);
// True while the banner is animating on screen (the idle frame pacing keeps the full rate).
bool wantsFrames();
// Dev-input "banner": time <seconds> (hold there) | run | next | stats (the measured cost to stdout).
bool devCommand(const std::string& cmd, const std::vector<std::string>& args, int frame);

}  // namespace gl::chromebanner
