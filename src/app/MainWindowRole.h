// Gridline: the one window the program starts in. It opens as a small borderless loading window
// (the splash) and becomes the main window once everything is loaded.
//
// GitHub issue #33: tiling window managers (i3, sway, Hyprland, bspwm, niri and the tiling modes of
// the big desktops) decide whether a window floats once, when it is mapped. A window whose minimum
// and maximum sizes are equal is a fixed-size window and floats; so does a dialog, a utility window
// or a window transient for another. The splash is fixed-size and floats, which is right for it.
// Changing the size hints of a window that is already mapped does not make it tile, so on X11 and
// Wayland the window is unmapped and mapped again as an ordinary resizable top-level window: the
// window manager then manages it afresh and places and sizes it as it does every other window.
//
// Header only, SDL only: the program (main.cpp) and the window hint test (tests/test_k157.cpp)
// share these exact calls.
#pragma once
#include <SDL.h>

namespace gl::winrole {

constexpr int kSplashW = 640, kSplashH = 380;
constexpr int kMainMinW = 960, kMainMinH = 600;

// The loading window: borderless, fixed size, centred. `hidden`: automated runs (--hidden) never
// show a window.
inline SDL_Window* createSplash(bool hidden) {
  return SDL_CreateWindow("Gridline", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, kSplashW, kSplashH,
                          SDL_WINDOW_OPENGL | SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALLOW_HIGHDPI | (hidden ? SDL_WINDOW_HIDDEN : 0));
}

// Window systems whose window managers choose a window's role when it is mapped (X11, Wayland).
inline bool remapsOnRoleChange() {
#if defined(__APPLE__) || defined(_WIN32)
  return false;
#else
  return true;
#endif
}

struct MainPlacement {
  int w = 1600, h = 960;  // asked for; a tiling window manager decides
  int x = 0, y = 0;       // asked for; a tiling window manager decides
};

// The splash becomes the main window: decorated, resizable, with a minimum size and no maximum.
// A window that was on screen is mapped again so that the window manager sees a normal window.
inline void becomeMain(SDL_Window* window, const MainPlacement& p) {
  const bool remap = remapsOnRoleChange() && (SDL_GetWindowFlags(window) & SDL_WINDOW_SHOWN) != 0;
  if (remap) SDL_HideWindow(window);
  SDL_SetWindowBordered(window, SDL_TRUE);
  SDL_SetWindowResizable(window, SDL_TRUE);
  SDL_SetWindowSize(window, p.w, p.h);
  SDL_SetWindowPosition(window, p.x, p.y);
  SDL_SetWindowMinimumSize(window, kMainMinW, kMainMinH);
  if (remap) SDL_ShowWindow(window);
}

}  // namespace gl::winrole
