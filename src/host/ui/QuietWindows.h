// Gridline — quiet windows: test and tool runs that never show anything on the user's screen and
// never take the keyboard from the app the user is working in, while every window is still really
// made, sized, parented, shown, hidden, made key (inside the app) and drawn.
//
// On when GRIDLINE_QUIET_WINDOWS is set to anything but "0". A --hidden run of the app turns it on
// for itself and the processes it starts (the plugin sandbox, the plugin scanner, the crash dialog)
// unless GRIDLINE_QUIET_WINDOWS=0 asks to see the windows. ctest sets it for every test. A plain
// start of Gridline never has it, and then nothing here does anything (install() returns at once).
//
// macOS (QuietWindowsMac.mm):
//   - the app never becomes active: activation policy "prohibited" (no Dock icon, no menu bar, no
//     Cmd-Tab entry), SDL's start-up activation is off (SDL_MAC_BACKGROUND_APP), and every request
//     to activate (this app, the Dock, any other app) is dropped and counted;
//   - every window is fully transparent and lets the mouse through from the moment it is ordered
//     in, wherever it is: its place, size, level, parent and visibility stay what the code asked
//     for. alphaValue and ignoresMouseEvents still answer what the code set (the real ones stay
//     0 and YES), so code that reads them behaves as in a normal run;
//   - the key window is kept by Gridline itself: makeKeyWindow / makeKeyAndOrderFront make a window
//     key inside the app (isKeyWindow, NSApp.keyWindow and the did-become / did-resign-key
//     notifications its delegate and SDL see), as if the app were active, so keyboard focus
//     between Gridline's windows works and is tested as before.
// Linux and Windows (QuietWindows.cpp): windows are shown without being activated (SDL's
// SDL_WINDOW_NO_ACTIVATION_WHEN_SHOWN) and the app makes each window it shows fully transparent
// (window opacity 0, which lets the mouse through on Windows; X11 needs a compositing manager, so
// test runs on a desktop should still use a virtual display such as xvfb-run).
#pragma once
#include <string>

namespace gl::quiet {

// GRIDLINE_QUIET_WINDOWS is set (and is not "0").
bool on();
// A --hidden run: quiet for this process and the processes it starts, unless the variable is "0".
// Before the window system starts.
void enableForHiddenRun();
// Installs the guards when on(); does nothing otherwise. Idempotent. Before the window system
// starts (macOS: before SDL_Init or NSApplication).
void install();
// The guards are in place in this process.
bool installed();

// What a check sees (the dev-input command "quietcheck"; the quiet windows test).
struct Check {
  bool quiet = false;      // on()
  bool guards = false;     // installed()
  std::string policy;      // macOS: "regular", "accessory", "prohibited" ("" elsewhere / before the app starts)
  bool activeNow = false;  // this app (or a process it started) is the active app now
  int becameActive = 0;    // times this app became active since the guards went in
  int ownActivations = 0;  // times the system made this app or one it started the active app
  int blockedActivations = 0;  // requests to activate an app, dropped
  int windowsQuieted = 0;  // windows ordered in (made transparent and click-through) in this process
  int onScreen = 0;        // windows of this app and the processes it started that are ordered in now
  int visible = 0;         // of them, not fully transparent on a display: must stay 0
  std::string keyWindow;   // title of the window that has the keyboard inside the app ("" none)
  std::string detail;      // the visible ones (owner pid, size, alpha)
};
Check check();
// One line: "quiet 1 guards 1 policy prohibited active 0 became-active 0 ...".
std::string describe(const Check& c);

// Linux / Windows: a window was shown and made transparent (counted for check()).
void noteQuieted();

}  // namespace gl::quiet
