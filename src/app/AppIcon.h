// K-14: Gridline's icon on every window, on every OS.
#pragma once
struct SDL_Window;

namespace gl::appicon {

// Before SDL_Init: on X11 and Wayland (Linux, the BSDs) the window class / app id becomes the
// desktop entry id (AppIconImage.h kDesktopId) unless the user set SDL_VIDEO_X11_WMCLASS or
// SDL_VIDEO_WAYLAND_WMCLASS, so the panel, dock and window switcher match every Gridline window
// to gridline.desktop and its icon.
void prepareIdentity();

// The window's own icon, for every window Gridline opens (main, detached, plugin windows):
// X11: _NET_WM_ICON from the embedded image (shown even without a desktop entry);
// Windows: the program's multi-size icon resource, big and small at the window's DPI (title bar,
// Alt+Tab, taskbar); macOS: the Dock icon when the program runs outside an app bundle (inside
// one, the bundle's Gridline.icns already is). Wayland takes the icon from the desktop entry.
void apply(SDL_Window* w);

}  // namespace gl::appicon
