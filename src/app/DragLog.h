// Gridline - 0.7.8: a drag log for finding why a drag stops following the pointer on someone's own
// computer. Off unless GRIDLINE_DRAGLOG names a file; then every frame while a mouse button is down
// (and a few after) writes the pointer, the buttons, the active and hovered items, the focused window,
// the mouse and focus events of the frame, every change of the active item with the code that made it,
// and the editors' own notes (a drag started, moved by so much, ended and why).
#pragma once
#include <string>

struct ImGuiContext;

namespace gl::draglog {

bool on();
// After ImGui::CreateContext(): opens the file and hooks the active item's changes.
void install();
// One line of the current frame (the editors' notes, the program's events).
void note(const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
// Once a frame, after it was drawn: `platform` is the program's own part (the window system's
// mouse and focus state).
void frameEnd(const std::string& platform);

}  // namespace gl::draglog
