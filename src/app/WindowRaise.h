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

struct ImGuiWindow;

namespace gl::ui {

void raiseOnPress();

// Dev check ("zorder"): the shown root windows back to front, the focused window and the window
// that was last raised by a press, to stdout.
void devDumpZOrder(int frame);

}  // namespace gl::ui
