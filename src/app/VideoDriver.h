// Gridline: asks SDL for the window system plugin interfaces can open on (see
// core/VideoDriverChoice.h). Linux and the BSDs; on the other systems these do nothing.
#pragma once
#include <string>

namespace gl::videodriver {

// Before SDL_Init. `setting`: ui.windowSystem ("auto" or "wayland", K-153).
void prepare(const std::string& setting = std::string());
// After SDL_Init, whether it worked or not: puts the environment back, so that programs Gridline
// starts (plugin hosts, Wine) see what the user's session set, and logs the driver in use.
void finish();
// SDL_Init or the first window failed on the driver Gridline asked for: forget the request so that
// SDL's own order gets a try. False when nothing had been asked for (nothing more to try).
bool dropRequest();

// The window system in use, for people: "Wayland", "X11", "Windows", "macOS" (K-153).
std::string currentName();

// Empty when a plugin's interface can open; otherwise the reason, in a sentence for the user.
std::string interfaceProblem();

}  // namespace gl::videodriver
