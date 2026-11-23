// Gridline: a press anywhere on a window brings it to the front and gives it the keyboard.
//
// Dear ImGui raises a window by itself only on a left press over empty space, or when the item
// pressed runs ImGui's own button logic for that mouse button. Right and middle presses (the piano
// roll's right-click delete, a knob's menu, a middle-drag) and left presses on controls that read
// the mouse by hand left the window where it was, under the others. raiseOnPress() runs once a
// frame, right after ImGui::NewFrame(): any press (left, right or middle) over a window focuses
// that window, which brings it (or its dock) to the front, as in the reference workflow.
//
// It leaves alone: a press while another button is already held (a drag under way), a press while
// something is being dragged and dropped, a press inside a popup or tooltip, a press under a modal
// dialog (the dialog stays on top) and a press on the window that already has the focus.
#pragma once
#include <string>

struct ImGuiWindow;

namespace gl::ui {

void raiseOnPress();

// Dev check ("zorder"): the shown root windows back to front, the focused window and the window
// that was last raised by a press, to stdout.
void devDumpZOrder(int frame);

// K-297 (round 6): a window open as a docked tab behind another one of its region comes to the
// front of it (its tab selected). Returns false when the window is not such a hidden tab (shown,
// floating or closed): a show/hide toggle then toggles. A toggle used to close the hidden tab.
bool bringTabForward(const char* name);
// K-297 (round 7): gives a window the keys by program (a page switch, keys back after a button)
// without closing what is open over it: under a modal dialog or with a menu or pop-up open it does
// nothing and returns false. ImGui's FocusWindow closes every pop-up above the window it focuses, so
// the Tracker's Files > Save as opened the save dialog and the keys going back to the pattern closed
// it the next frame.
bool focusWindowKeepPopups(ImGuiWindow* w);
// K-297 (round 7): the docked area with the keys (a main view, a side panel, an editor band, a list)
// carries a quiet 1 px accent frame, so it is clear where typing and shortcuts go. Only docked
// content areas of some size (not the bars, not dialogs or floating windows, whose captions show
// it) and not while a menu or pop-up is open. Drawn once a frame, after every window. colour 0: the
// palette's accent; otherwise that colour (an ImU32, alpha included).
void drawKeyFocusFrame(unsigned int colour = 0);
// K-376: an area that only borrows the keys for a moment (true for it) keeps the frame on the area
// that had it: the Tracker's header band, which hands the keys back to the pattern once a click on
// it is over. One function, set by the layout that needs it (it checks the layout itself).
void setKeyFocusBorrower(bool (*borrows)(const ImGuiWindow* area));
// K-376 (dev, tests): the frame's record since the last reset: frames drawn, frames without it (no
// area with the keys), frames with a menu or pop-up open, moves to another area, the area now.
struct KeyFocusFrameStats {
  int drawn = 0, missed = 0, popups = 0, moves = 0;
  std::string area;
};
KeyFocusFrameStats& keyFocusFrameStats();

}  // namespace gl::ui
