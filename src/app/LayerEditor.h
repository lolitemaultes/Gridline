// Gridline — K-169: the layer editor (the Plugin tab of a layer channel's window). One lane per
// child with its mute, solo, level, pan and transpose and a key or velocity zone bar to drag
// (move, edges, fade handles), the layer's play mode, split helpers, eight macro knobs with their
// mappings, and layer presets (save, load, the built-in ones).
#pragma once
#include <string>
#include <vector>
#include "../core/Types.h"

namespace gl {
class App;
}

namespace gl::layerui {

// Draws the editor for `layer` at `width` px. Every edit is one undo step.
void drawEditor(App& a, ObjectId layer, float width);
// The width the editor wants (the channel window widens to it).
float editorWidth();

// Dev checks: the last drawn frame's text boxes and how many of them overlap, lane and zone rects.
struct DevState {
  int frame = -1;
  int texts = 0, overlaps = 0;
  std::vector<std::string> overlapping;  // "a | b"
  struct Lane {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // the lane's zone area
    float zx0 = 0, zx1 = 0;                 // the zone bar
  };
  std::vector<Lane> lanes;
  float macroX[8] = {}, macroY[8] = {};      // macro knob centres
  float minX = 0, minY = 0, maxX = 0, maxY = 0;  // the editor
};
const DevState& devState();

}  // namespace gl::layerui
