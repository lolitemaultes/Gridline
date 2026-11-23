// Gridline — quiet windows (QuietWindows.h): the switch, and Linux / Windows. macOS: QuietWindowsMac.mm.
#include "QuietWindows.h"
#include <atomic>
#include <cstdlib>
#include <cstring>

namespace gl::quiet {

namespace {
void setVar(const char* name, const char* value, bool overwrite) {
  if (!overwrite && std::getenv(name)) return;
#if defined(_WIN32)
  _putenv_s(name, value);
#else
  setenv(name, value, 1);
#endif
}
}  // namespace

bool on() {
  const char* v = std::getenv("GRIDLINE_QUIET_WINDOWS");
  return v && *v && std::strcmp(v, "0") != 0;
}

void enableForHiddenRun() {
  const char* v = std::getenv("GRIDLINE_QUIET_WINDOWS");
  if (v && std::strcmp(v, "0") == 0) return;  // the developer asked to see the windows
  setVar("GRIDLINE_QUIET_WINDOWS", "1", true);  // the processes this one starts are quiet too
}

#if !defined(__APPLE__)
namespace {
std::atomic<bool> gInstalled{false};
std::atomic<int> gQuieted{0};
}  // namespace

void install() {
  if (!on() || gInstalled.exchange(true)) return;
  // SDL shows windows without activating them (X11: no focus request, _NET_WM_USER_TIME 0;
  // Windows: SW_SHOWNA). The variable outranks the hint Gridline sets around some shows.
  setVar("SDL_WINDOW_NO_ACTIVATION_WHEN_SHOWN", "1", true);
}

bool installed() { return gInstalled.load(); }

void noteQuieted() { gQuieted.fetch_add(1); }

Check check() {
  Check c;
  c.quiet = on();
  c.guards = installed();
  c.windowsQuieted = gQuieted.load();
  return c;
}
#endif

std::string describe(const Check& c) {
  std::string s = "quiet " + std::to_string(c.quiet ? 1 : 0) + " guards " + std::to_string(c.guards ? 1 : 0);
  s += " policy " + (c.policy.empty() ? std::string("-") : c.policy);
  s += " active " + std::to_string(c.activeNow ? 1 : 0);
  s += " became-active " + std::to_string(c.becameActive);
  s += " own-activations " + std::to_string(c.ownActivations);
  s += " blocked-activations " + std::to_string(c.blockedActivations);
  s += " quieted " + std::to_string(c.windowsQuieted);
  s += " on-screen " + std::to_string(c.onScreen);
  s += " visible " + std::to_string(c.visible);
  s += " key \"" + c.keyWindow + "\"";
  if (!c.detail.empty()) s += " | " + c.detail;
  return s;
}

}  // namespace gl::quiet
