// Gridline - automatic updates at start-up (the splash screen's part; logic in core/UpdateFlow.h).
//
// recoverAtStart() runs before the settings are read: it finishes or undoes an update that was cut
// short, counts the new version's first starts and puts the old version back when the new one
// keeps failing to start. run() runs once the settings are read and before the audio starts: with
// "Check for updates" and "Install updates automatically" on, and an install that can update
// itself, it checks, downloads, installs and asks main() to quit so the watcher starts the new
// version. Anything that goes wrong ends in a normal start of this version. docs/updates.md.
#pragma once
#include <functional>
#include <string>
#include <vector>
#include "../core/UpdateApply.h"
#include "Splash.h"

namespace gl {
class App;
}

namespace gl::splashupd {

// True: this process put the old version back and started it; end now.
bool recoverAtStart(const std::string& configDir, const std::vector<std::string>& args);
// What recoverAtStart found (Updated to ..., ... was put back), for the notices after start-up.
const update::apply::Recovery& startRecovery();

// One splash frame showing `view`; `quit` set when the window was closed. The splash has no
// buttons: the two Settings switches decide what happens.
using FrameFn = std::function<void(const SplashUpdateView& view, bool* quit)>;
enum class Result { Continue, Restart, Quit };
struct Options {
  bool automated = false;  // --hidden, --screenshot, scripted runs: only with the tests' local server
  bool disabled = false;   // --no-update-check
};
Result run(App& a, const std::vector<std::string>& args, const Options& o, const FrameFn& frame);
// A download moved to the background: still running / finished. At exit it is stopped (kept).
bool backgroundRunning();
void shutdown();

// --dev-update-splash <state>: a made-up view of each state for screenshots
// (checking, downloading, verifying, installing, permission, permission-windows, restarting, failed, background).
bool devView(const std::string& name, SplashUpdateView& out);

// Settings: the install kind Gridline detected, once per run (cached).
const update::Install& install();

}  // namespace gl::splashupd
