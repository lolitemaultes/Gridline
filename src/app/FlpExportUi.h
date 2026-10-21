// Gridline — J-80: FL Studio export as a background job. Gridline-only
// instruments are rendered to samples first (core/flp/FlpStandIn.h), which can
// take a while: a progress window shows it and can cancel it. When it is done
// a short summary is notified; the full per-plugin report stays in the FL
// Studio export report window (File > Export, or the window's Details).
#pragma once
#include <functional>
#include <string>

namespace gl {

class App;

namespace flpexport {

// Starts writing `path` (.flp, or the zipped loop package when `zip`).
// `done(ok)` runs on the UI thread when it has finished (ok = written).
// False when an export is already running.
bool start(App& a, const std::string& path, bool zip, std::function<void(bool ok)> done = {});
bool running();
// Per frame: the progress window while running, the report window when open.
void draw(App& a);
// Opens the report of the last export.
void openReport();
bool hasReport();
// Waits for a running export to finish (dev checks; its card and done()
// follow on the next draw).
void wait();
// Cancels and waits for a running export (the app is closing).
void shutdown();

}  // namespace flpexport
}  // namespace gl
