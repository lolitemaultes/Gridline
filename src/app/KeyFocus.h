// Gridline: keys and text that arrive while SDL knows of no window with the keyboard focus.
//
// GitHub issue #52: on some Linux compositors (seen on Hyprland, through XWayland) the window could
// get the keys without SDL ever being told it has the keyboard focus. SDL then sends every key and
// every typed character with window id 0, and the interface's input layer, which takes keys only
// for its own windows, dropped them all: no typing in text fields, no Space to play, until a later
// focus change (another window, a workspace switch) told SDL. The system only hands keys to a
// window that has the focus (or, on X11 without a focus, the window under the pointer), so a key
// that reaches Gridline with no window named is Gridline's: it goes to the main window.
//
// Header only, SDL only: the program (main.cpp) and the test (tests/test_key_focus.cpp) share it.
#pragma once
#include <SDL.h>

namespace gl::keyfocus {

// Gives a key, text or text-editing event that names no window to `mainWindowId`. True when the
// event was changed. Events that name a window (a plugin window, a detached Gridline window) and
// every other kind of event are left alone.
inline bool adopt(SDL_Event& ev, Uint32 mainWindowId) {
  if (mainWindowId == 0) return false;
  switch (ev.type) {
    case SDL_KEYDOWN:
    case SDL_KEYUP:
      if (ev.key.windowID != 0) return false;
      ev.key.windowID = mainWindowId;
      return true;
    case SDL_TEXTINPUT:
      if (ev.text.windowID != 0) return false;
      ev.text.windowID = mainWindowId;
      return true;
    case SDL_TEXTEDITING:
      if (ev.edit.windowID != 0) return false;
      ev.edit.windowID = mainWindowId;
      return true;
    default: return false;
  }
}

}  // namespace gl::keyfocus
