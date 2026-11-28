// Gridline - the splash screen (Splash.cpp): the loading bar, and the automatic update's states
// (checking, downloading, the system's password prompt, restarting), drawn in the splash's own colours
// (logo guide: Paper on surface-950, the same in the Dark and Light themes).
#pragma once
#include <cstdint>
#include <string>

struct ImFont;

namespace gl {

void drawSplash(ImFont* bold, ImFont* mono, float progress, const char* status, double time);

struct SplashUpdateView {
  enum State { Checking, Downloading, Verifying, Installing, Permission, Restarting, Failed, Background } state = Checking;
  std::string version;  // the version being installed
  float progress = 0;   // the bar
  int64_t got = 0, total = 0;
  bool windows = false;  // Permission: Windows asks (UAC), not a password
};
// Status text and the bar only: no buttons, no keys (the two Settings switches decide everything).
void drawSplashUpdate(ImFont* bold, ImFont* mono, const SplashUpdateView& v, double time);

}  // namespace gl
