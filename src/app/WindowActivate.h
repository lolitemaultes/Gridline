// Gridline: bringing the main window forward for a later launch (K-121 single instance, K-153).
//
// Desktops guard against windows taking the focus by themselves. A launch the user just made gets a
// token from the desktop (Wayland: XDG_ACTIVATION_TOKEN; X11: DESKTOP_STARTUP_ID) that allows its
// window to come forward; a later launch hands it to the running Gridline with its files
// (core/SingleInstance.h), and the running Gridline uses it here.
//
//   SDL's Wayland driver  SDL 2 uses a token only when a window is first shown, so Gridline passes
//                         the token to the compositor itself (xdg-activation, through the
//                         libwayland-client SDL has already loaded; nothing else is linked).
//   X11, XWayland, other  SDL's own raise.
#pragma once
#include <string>

struct SDL_Window;

namespace gl::activate {

// Restores `w` when it is minimised and brings it forward, with `token` when there is one. True
// when the token was handed to the compositor (Wayland), false when SDL's own raise was used.
bool bringForward(SDL_Window* w, const std::string& token);

}  // namespace gl::activate
