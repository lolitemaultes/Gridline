// Gridline - K-199: game controllers through SDL (the program only; see GamepadUi.h). Plugin
// builds have no SDL, so this file stays out of them and the rules in GamepadUi.cpp never call SDL.
#include <SDL.h>
#include <cstdio>
#include <map>
#include "GamepadUi.h"

namespace gl::gamepadui {

namespace {
std::map<SDL_JoystickID, SDL_GameController*> gPads;
bool openUrl(const char* url) { return SDL_OpenURL(url) == 0; }
}  // namespace

void init(App& a) {
  initRules(a);
  setOpenUrl(&openUrl);
  if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) std::fprintf(stderr, "[gamepad] controllers unavailable: %s\n", SDL_GetError());
}

bool handleEvent(App& a, const SDL_Event& e) {
  switch (e.type) {
    case SDL_CONTROLLERDEVICEADDED:
      if (SDL_GameController* c = SDL_GameControllerOpen(e.cdevice.which)) gPads[SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c))] = c;
      setControllers(gPads.size());
      return true;
    case SDL_CONTROLLERDEVICEREMOVED: {
      auto it = gPads.find(e.cdevice.which);
      if (it != gPads.end()) {
        SDL_GameControllerClose(it->second);
        gPads.erase(it);
      }
      setControllers(gPads.size());
      return true;
    }
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP: {
      const char* n = SDL_GameControllerGetStringForButton(static_cast<SDL_GameControllerButton>(e.cbutton.button));
      const gamepad::Button b = gamepad::buttonFromName(n ? n : "");
      if (b != gamepad::Button::Count) pressButton(a, b, e.type == SDL_CONTROLLERBUTTONDOWN);
      return true;
    }
    case SDL_CONTROLLERAXISMOTION:
      if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) stickAxis(a, 0, e.caxis.value);
      if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) stickAxis(a, 1, e.caxis.value);
      return true;
    default: return false;
  }
}

}  // namespace gl::gamepadui
