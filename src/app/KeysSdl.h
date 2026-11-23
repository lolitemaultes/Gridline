// Gridline - GitHub #79: SDL's key events to the interface, with the keys as core/KeyCaps.h makes
// them (the app and its plugin windows; the plugin builds have no SDL).
#pragma once
#include <SDL.h>
#include "../../third_party/imgui/imgui_impl_sdl2.h"
#include "../core/KeyCaps.h"

namespace gl::keys {

// Hands an SDL event to Dear ImGui (ImGui_ImplSDL2_ProcessEvent). On a layout where the interface's
// own rule leaves a punctuation key without a key (US Dvorak has no / key that way), a key press is
// passed as the punctuation it types (keycaps::remapPosition), so every shortcut keeps a key.
inline bool processEvent(const SDL_Event& ev) {
  if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP)
    if (const int to = keycaps::remapPosition(static_cast<int>(ev.key.keysym.scancode))) {
      SDL_Event copy = ev;  // the interface makes that punctuation key of the US position it names
      copy.key.keysym.sym = SDLK_UNKNOWN;
      copy.key.keysym.scancode = static_cast<SDL_Scancode>(to);
      return ImGui_ImplSDL2_ProcessEvent(&copy);
    }
  return ImGui_ImplSDL2_ProcessEvent(&ev);
}

}  // namespace gl::keys
