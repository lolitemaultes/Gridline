// Gridline - J-89: update check in the app. At startup (once a day, on a
// worker thread, after the window is up) and from Help > Check for updates...
// / Settings > General > Check now; a popup offers a newer release (styled like
// the unsaved-changes prompt): open the release page, or download the installer
// for this computer into Downloads (checked against SHA256SUMS) and show it in
// the file manager. Never while the transport runs or over another prompt.
// Logic: core/Update.h; network: host/net/Http.h; behaviour: docs/updates.md.
#pragma once
#include <functional>
#include <string>
#include "../collab/VersionGate.h"

namespace gl {
class App;
}

namespace gl::updui {

struct Options {
  bool disabled = false;     // turned off by the user: --no-update-check, GRIDLINE_NO_UPDATE_CHECK, CI
  bool automated = false;    // screenshots, profiling, scripted input, --hidden: no network at all
  bool hidden = false;       // --hidden: links and folders are printed, not opened
  std::string fakeVersion;   // --fake-update <version>: a made-up release, offered at once (no network)
  std::function<bool(const std::string&)> openUrl;  // the system browser (SDL_OpenURL in main.cpp)
};

// After the main window is up (the app only; plugins never start it).
void start(App& a, const Options& o);
bool started();
// Help > Check for updates..., Settings > Check now: always asks, always reports.
void checkNow(App& a);
// Per frame (drawDialogs): collects results, shows the popup when the time is right.
void draw(App& a);
// Settings > General > Updates.
void settingsSection(App& a);
// At exit: a running download is cancelled.
void shutdown();

// J-111: the collaboration version gate (its own check against the newest release with a
// download for this computer, cached for an hour; never per frame). Starts a check when due.
const collab::GateState& gate(App& a);
bool gateChecking();
// The update dialog in "required" mode: `title` ("This is a collaborative project. Update to
// Gridline 0.8.0 to open it."), the same notes / download / verify, Cancel instead of skip.
void showRequired(App& a, const std::string& title, const std::string& version);

}  // namespace gl::updui
