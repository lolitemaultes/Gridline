// Gridline: the crash pop-up (shown by the running app for caught failures and for reports left
// by an earlier run, and by the small dialog process right after a crash or during a freeze), and
// the project's recovery copies.
#pragma once
#include <string>
#include "../core/Crash.h"

struct ImFont;

namespace gl {
class App;
}

namespace gl::crashui {

// ---- in the app ----
void startup(App& a);         // the copy path and the rescue; reports from earlier runs
void frame(App& a);           // the periodic recovery copy
void draw(App& a);            // at the root level, after every other dialog
void shutdown(App& a);        // a clean end: this run's copy goes
// Opens the recovery copy a report points to, as an unsaved project.
void recoverFrom(App& a, const std::string& reportPath);
// ProjectLoadUi: the project that just loaded was a recovery copy.
bool isRecovering();
void recoveredLoaded(App& a);
// A frame failed with an exception, three in a row: the pop-up alone, until the user decides.
// Returns true to try the normal frame again, false to end (a restart was started).
struct KeepsFailingResult {
  bool done = false;
  bool retry = false;
};
KeepsFailingResult drawKeepsFailing(App& a);

// ---- the dialog process ----
struct DialogFonts {
  ImFont* bold = nullptr;
  ImFont* mono = nullptr;
};
struct DialogState;
DialogState* dialogOpen(const std::string& configDir, const std::string& reportPath, const std::string& sessionFile);
// Draws one frame in a window filling the display. False when the dialog is finished.
bool dialogFrame(DialogState* s, const DialogFonts& fonts);
void dialogClose(DialogState* s);
// How much room the dialog wants, in interface pixels.
void dialogSize(int& w, int& h);

}  // namespace gl::crashui
