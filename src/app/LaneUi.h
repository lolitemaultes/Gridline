// Gridline - K-255: the lane controls of a pattern row (core/Lanes.h) in every view that shows a
// pattern's steps: the channel rack (Studio, and the Smooth lower pane), the Clips clip band and
// the Tracker's Steps page. Each row gets a grid chip (its steps per bar, orange when it has a grid
// of its own) that opens the grid popover, and a length handle at the end of its lane (drag it;
// a click opens the length popover with the restart / run on choice). Every edit is one undo step
// and goes through the Document, so it syncs in a collaboration.
#pragma once
#include <functional>
#include <string>
#include "../../third_party/imgui/imgui.h"
#include "../core/Song.h"

namespace gl {
class App;
}

namespace gl::laneui {

// The lane column shows: the setting is on, or the pattern has a lane of its own.
bool columnShown(App& a, const Pattern* p);
// "5 per beat" (where the pattern's beats divide the grid), else "7 per bar".
std::string gridText(const Song& s, const Pattern& p, int grid);
// "Every lane lines up again after 7 bars" ("" when nothing runs on).
std::string lineUpText(const Song& s, const Pattern& p);

// Call once per frame inside the window the rows are drawn in, before the rows (the popovers'
// ids belong to that window).
void beginRows();
// The grid chip of a row: [p0, p0 + size). A click opens the grid popover, a right-click the
// length popover.
void chip(App& a, const Pattern& p, const Channel& c, ImVec2 p0, ImVec2 size);
// The length popover for a channel (the handle's click).
void openLength(ObjectId channel);
void openGrid(ObjectId channel);
// The popovers (call once after the rows, in the same window).
void popovers(App& a);

// The length handle of a row whose lane spans [x0 .. x(step)] on screen: `posOf(k)` is the screen
// x of lane step k's left edge (k = 0 .. steps). Call `handleInput` before the row's steps (it
// takes the pointer first) and `handleDraw` after them (it draws on top).
struct Handle {
  float x = 0;        // screen x of the handle (the lane length's end)
  float y0 = 0, y1 = 0;
  int at = 0;         // the lane step it stands at (the length, or the row's end)
  bool looping = false;
  bool hot = false, held = false;
};
// Interaction of the handle (an 8 px wide grab area); returns the lane step it was dragged to
// (-1: not dragged this frame). `clicked`: a press and release without a drag.
int handleInput(App& a, const Pattern& p, const Channel& c, Handle& h, int steps, const std::function<float(int)>& posOf, bool* clicked);
void handleDraw(ImDrawList* dl, const Handle& h, LaneMode mode);

// The rack menu's lane items (show the column).
void menuItems(App& a);

}  // namespace gl::laneui
