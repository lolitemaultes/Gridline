// Gridline: the one window the program starts in.
//
// macOS and Windows: it opens as a small borderless loading window (the splash) and becomes the
// main window once everything is loaded.
//
// X11 and Wayland (Linux, the BSDs): the window is the main window from its very first map, an
// ordinary resizable, decorated top-level at its planned size, and the loading screen is drawn
// inside it. GitHub issue #33: tiling window managers (i3, sway, Hyprland, bspwm, niri and the
// tiling modes of the big desktops) decide whether a window floats when it is first mapped. A
// fixed-size window (minimum == maximum), a dialog, a utility window or a window transient for
// another floats. Some of them (Hyprland's XWayland windows) keep that decision for the life of the
// X window, so a fixed-size splash that is unmapped and mapped again as a normal window (0.7.5)
// still floated there. GitHub issue #52: the same unmap and map again could leave the window
// without the keyboard focus on such a compositor. So nothing is ever mapped twice.
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

// Window systems whose window managers choose a window's role when it is first mapped (X11,
// Wayland): there the program starts in the main window itself (createMain), never in a splash.
inline bool loadsInMainWindow() {
#if defined(__APPLE__) || defined(_WIN32)
  return false;
#else
  return true;
#endif
}

struct MainPlacement {
  int w = 1600, h = 960;  // asked for; a tiling window manager decides
  int x = 0, y = 0;       // asked for; a tiling window manager decides
  bool maximize = false;  // GitHub issue #50: it was left maximised (never for a hidden window)
};

// X11 and Wayland: the main window from the start. Created hidden so that its minimum size is set
// before the window manager first sees it, then shown (never for `hidden`, automated runs). The
// position is a request a tiling window manager ignores.
inline SDL_Window* createMain(bool hidden, const MainPlacement& p) {
  const Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_HIDDEN;
  SDL_Window* w = SDL_CreateWindow("Gridline", p.x, p.y, p.w, p.h, flags);
  if (!w) return nullptr;
  SDL_SetWindowMinimumSize(w, kMainMinW, kMainMinH);
  if (!hidden) {
    SDL_ShowWindow(w);
    if (p.maximize) SDL_MaximizeWindow(w);
  }
  return w;
}

// The window the program starts in becomes the main window once everything is loaded.
// macOS and Windows: the splash turns into a decorated, resizable window with a minimum size and
// no maximum, in place. X11 and Wayland: it is the main window already (createMain); only a size
// that changed since (a dev run's --size) is applied, and the window is never unmapped.
inline void becomeMain(SDL_Window* window, const MainPlacement& p) {
  if (loadsInMainWindow()) {
    int w = 0, h = 0;
    SDL_GetWindowSize(window, &w, &h);
    if ((SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) == 0 && (w != p.w || h != p.h)) SDL_SetWindowSize(window, p.w, p.h);
    return;
  }
  SDL_SetWindowBordered(window, SDL_TRUE);
  SDL_SetWindowResizable(window, SDL_TRUE);
  SDL_SetWindowSize(window, p.w, p.h);
  SDL_SetWindowPosition(window, p.x, p.y);
  SDL_SetWindowMinimumSize(window, kMainMinW, kMainMinH);
  // After the size: the window keeps that size to go back to when it is restored.
  if (p.maximize) SDL_MaximizeWindow(window);
}

}  // namespace gl::winrole
