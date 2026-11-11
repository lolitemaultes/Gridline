// Gridline: the window system request made to SDL (see VideoDriver.h).
#include "VideoDriver.h"
#include <SDL.h>
#include <cstdio>
#include <cstdlib>
#include "../core/Log.h"
#include "../core/VideoDriverChoice.h"
#include "../host/ui/NativeView.h"

namespace gl::videodriver {

std::string currentName() {
  const char* now = SDL_GetCurrentVideoDriver();
  if (!now) return "none";
  const std::string d = now;
  if (d == "wayland") return "Wayland";
  if (d == "x11") return "X11";
  if (d == "windows") return "Windows";
  if (d == "cocoa") return "macOS";
  return d;
}

#if defined(__APPLE__) || defined(_WIN32)

void prepare(const std::string&) {}
void finish() {}
bool dropRequest() { return false; }
std::string interfaceProblem() { return std::string(); }

#else

namespace {
constexpr const char* kSdl2Name = "SDL_VIDEODRIVER";
constexpr const char* kSdl3Name = "SDL_VIDEO_DRIVER";  // SDL3 and sdl2-compat

Choice gChoice;
std::string gSavedSdl2, gSavedSdl3;
bool gHadSdl2 = false, gHadSdl3 = false;

void remember(const char* name, std::string& saved, bool& had) {
  const char* v = std::getenv(name);
  had = v != nullptr;
  saved = v ? v : "";
}

void restore(const char* name, const std::string& saved, bool had) {
  if (had)
    setenv(name, saved.c_str(), 1);
  else
    unsetenv(name);
}

void request(const char* driver) {
  // A hint for SDL 2.0.22 and later and sdl2-compat (which maps the name; SDL3 takes its environment
  // once, early), the variables for older SDL2. Only OVERRIDE beats a variable the session has set.
  SDL_SetHintWithPriority(kSdl2Name, driver, SDL_HINT_OVERRIDE);
  setenv(kSdl2Name, driver, 1);
  setenv(kSdl3Name, driver, 1);
}

const char* whyText(Why w) {
  switch (w) {
    case Why::SdlDecides: return "SDL's choice";
    case Why::UserAsked: return "GRIDLINE_VIDEO_DRIVER";
    case Why::SettingAsked: return "the Window system setting";
    case Why::UserSdlOther: return "SDL_VIDEODRIVER";
    case Why::NoXServer: return "no X server answers";
    case Why::PluginsNeedX: return "plugin interfaces need X11";
  }
  return "";
}
}  // namespace

void prepare(const std::string& setting) {
  // K-83: SDL asks X11 compositors to stop compositing its windows (meant for full-screen games).
  // A window the size of the monitor is then shown directly, and the compositor switches back and
  // forth, flashing the screen, whenever a menu, hint or plugin window appears over it. Gridline
  // is a desktop application: its windows stay composited. (A variable the session sets still wins.)
  SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
  remember(kSdl2Name, gSavedSdl2, gHadSdl2);
  remember(kSdl3Name, gSavedSdl3, gHadSdl3);
  Inputs in;
  in.gridlineDriver = std::getenv("GRIDLINE_VIDEO_DRIVER");
  in.setting = setting.c_str();
  in.sdl2Driver = gHadSdl2 ? gSavedSdl2.c_str() : nullptr;
  in.sdl3Driver = gHadSdl3 ? gSavedSdl3.c_str() : nullptr;
  in.xServerAnswers = host::native::xServerAnswers();
  gChoice = choose(in);
  if (!gChoice.driver.empty()) request(gChoice.driver.c_str());
}

bool dropRequest() {
  if (gChoice.driver.empty()) return false;
  std::fprintf(stderr, "[video] the %s driver failed (%s); trying SDL's own choice\n", gChoice.driver.c_str(), SDL_GetError());
  gChoice = Choice{std::string(), Why::NoXServer};
  // The value the session had, or SDL's own order (an empty name means no choice to SDL).
  SDL_SetHintWithPriority(kSdl2Name, gHadSdl2 ? gSavedSdl2.c_str() : gHadSdl3 ? gSavedSdl3.c_str() : "", SDL_HINT_OVERRIDE);
  restore(kSdl2Name, gSavedSdl2, gHadSdl2);
  restore(kSdl3Name, gSavedSdl3, gHadSdl3);
  return true;
}

void finish() {
  restore(kSdl2Name, gSavedSdl2, gHadSdl2);
  restore(kSdl3Name, gSavedSdl3, gHadSdl3);
  const char* now = SDL_GetCurrentVideoDriver();
  std::string line = std::string("video driver: ") + (now ? now : "none") + " (" + whyText(gChoice.why) + "; session " +
                     (std::getenv("XDG_SESSION_TYPE") ? std::getenv("XDG_SESSION_TYPE") : "unknown") + ")";
  logging::info("app", line);
  std::fprintf(stderr, "[video] %s\n", line.c_str());
}

std::string interfaceProblem() {
  const char* now = SDL_GetCurrentVideoDriver();
  const bool wayland = now && std::string(now) == "wayland";
  return blockMessage(interfaceBlock(!wayland, gChoice.why));
}

#endif

}  // namespace gl::videodriver
