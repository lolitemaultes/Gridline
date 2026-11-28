// Gridline — K-430 (GitHub #84): Linux SDL2 audio driver preference and capture spec helper.
//
// On Linux, modern desktops run PipeWire with a PulseAudio compatibility layer (pipewire-pulse).
// Older SDL2 builds (e.g. Debian 11's 2.0.14) lacked native PipeWire support and older 2.0.16..20
// builds ordered PulseAudio ahead of PipeWire. When the user or test environment did not set
// SDL_AUDIODRIVER / SDL_AUDIO_DRIVER explicitly, prefer native `pipewire` when compiled in and a
// PipeWire socket is live, falling back cleanly to SDL's default order (`pulseaudio` -> `alsa`)
// if SDL_Init fails. Restores the environment after SDL_Init so child processes are unaffected.
#pragma once

#include <SDL.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include "../core/Log.h"
#if !defined(_WIN32)
#include <dlfcn.h>
#endif

namespace gl::audiodriver {

namespace detail {
inline std::string gRequested;
inline std::string gSavedSdl2, gSavedSdl3;
inline bool gHadSdl2 = false, gHadSdl3 = false;

inline bool hasCompiledDriver(const char* name) {
  const int n = SDL_GetNumAudioDrivers();
  for (int i = 0; i < n; ++i) {
    const char* d = SDL_GetAudioDriver(i);
    if (d && std::strcmp(d, name) == 0) return true;
  }
  return false;
}

inline bool pipewireSocketPresent() {
#if defined(__linux__)
  std::error_code ec;
  if (const char* pw = std::getenv("PIPEWIRE_RUNTIME_DIR"); pw && *pw) {
    if (std::filesystem::exists(std::filesystem::path(pw) / "pipewire-0", ec)) return true;
  }
  if (const char* xdg = std::getenv("XDG_RUNTIME_DIR"); xdg && *xdg) {
    if (std::filesystem::exists(std::filesystem::path(xdg) / "pipewire-0", ec)) return true;
  }
#endif
  return false;
}

inline void restoreEnv(const char* name, const std::string& saved, bool had) {
#if !defined(_WIN32)
  if (had)
    setenv(name, saved.c_str(), 1);
  else
    unsetenv(name);
#else
  (void)name;
  (void)saved;
  (void)had;
#endif
}
}  // namespace detail

// Before SDL_Init. Never overrides an explicit SDL_AUDIODRIVER / SDL_AUDIO_DRIVER (such as dummy in tests).
inline void prepare() {
#if defined(__linux__)
  const char* s2 = std::getenv("SDL_AUDIODRIVER");
  const char* s3 = std::getenv("SDL_AUDIO_DRIVER");
  detail::gHadSdl2 = s2 != nullptr;
  detail::gSavedSdl2 = s2 ? s2 : "";
  detail::gHadSdl3 = s3 != nullptr;
  detail::gSavedSdl3 = s3 ? s3 : "";
  detail::gRequested.clear();
  if ((s2 && *s2) || (s3 && *s3)) return;
  if (detail::hasCompiledDriver("pipewire") && detail::pipewireSocketPresent()) {
    detail::gRequested = "pipewire";
    SDL_SetHintWithPriority("SDL_AUDIODRIVER", "pipewire", SDL_HINT_OVERRIDE);
    setenv("SDL_AUDIODRIVER", "pipewire", 1);
    setenv("SDL_AUDIO_DRIVER", "pipewire", 1);
  }
#endif
}

// Called if SDL_Init failed with our requested audio driver; drops the preference so SDL falls back
// through pulseaudio -> alsa -> default.
inline bool dropRequest() {
#if defined(__linux__)
  if (detail::gRequested.empty()) return false;
  std::fprintf(stderr, "[audio] driver '%s' unavailable (%s); falling back to SDL default order\n", detail::gRequested.c_str(), SDL_GetError());
  detail::gRequested.clear();
  SDL_SetHintWithPriority("SDL_AUDIODRIVER", "", SDL_HINT_OVERRIDE);
  detail::restoreEnv("SDL_AUDIODRIVER", detail::gSavedSdl2, detail::gHadSdl2);
  detail::restoreEnv("SDL_AUDIO_DRIVER", detail::gSavedSdl3, detail::gHadSdl3);
  return true;
#else
  return false;
#endif
}

// After SDL_Init: restores environment and logs the active audio driver and SDL runtime version.
inline void finish() {
#if defined(__linux__)
  if (!detail::gRequested.empty()) {
    detail::restoreEnv("SDL_AUDIODRIVER", detail::gSavedSdl2, detail::gHadSdl2);
    detail::restoreEnv("SDL_AUDIO_DRIVER", detail::gSavedSdl3, detail::gHadSdl3);
    detail::gRequested.clear();
  }
#endif
  SDL_version sv{};
  SDL_GetVersion(&sv);
  const char* drv = SDL_GetCurrentAudioDriver();
  char buf[192];
  std::snprintf(buf, sizeof buf, "audio driver: %s (SDL %u.%u.%u)", drv ? drv : "none", static_cast<unsigned>(sv.major),
                static_cast<unsigned>(sv.minor), static_cast<unsigned>(sv.patch));
  logging::info("audio", buf);
}

// Queries the hardware preferred spec of capture device `devIdx` via SDL_GetAudioDeviceSpec (SDL 2.0.16+),
// using a runtime dlsym lookup on UNIX when compiled against older SDL2 headers (e.g. Debian 11 2.0.14).
inline bool queryCaptureDeviceSpec(int devIdx, SDL_AudioSpec* outSpec) {
  if (!outSpec || devIdx < 0) return false;
  SDL_zero(*outSpec);
#if SDL_VERSION_ATLEAST(2, 0, 16)
  return SDL_GetAudioDeviceSpec(devIdx, 1, outSpec) == 0;
#elif !defined(_WIN32)
  using Fn = int (*)(int, int, SDL_AudioSpec*);
  static Fn fn = [] {
    Fn f = nullptr;
    void* sym = dlsym(RTLD_DEFAULT, "SDL_GetAudioDeviceSpec");
    if (sym) std::memcpy(&f, &sym, sizeof f);
    return f;
  }();
  return fn && fn(devIdx, 1, outSpec) == 0;
#else
  (void)devIdx;
  return false;
#endif
}

// Queries the default capture device's preferred spec (SDL 2.24.0+), when "*" / System default is selected.
inline bool queryDefaultCaptureSpec(SDL_AudioSpec* outSpec) {
  if (!outSpec) return false;
  SDL_zero(*outSpec);
#if SDL_VERSION_ATLEAST(2, 24, 0)
  char* defName = nullptr;
  const int r = SDL_GetDefaultAudioInfo(&defName, outSpec, 1);
  if (defName) SDL_free(defName);
  return r == 0;
#elif !defined(_WIN32)
  using Fn = int (*)(char**, SDL_AudioSpec*, int);
  static Fn fn = [] {
    Fn f = nullptr;
    void* sym = dlsym(RTLD_DEFAULT, "SDL_GetDefaultAudioInfo");
    if (sym) std::memcpy(&f, &sym, sizeof f);
    return f;
  }();
  if (!fn) return false;
  char* defName = nullptr;
  const int r = fn(&defName, outSpec, 1);
  if (defName) SDL_free(defName);
  return r == 0;
#else
  return false;
#endif
}

}  // namespace gl::audiodriver
