// Gridline - J-131: the Gridline Suite in the app (core/SuiteInstall.h).
// Every download carries the suite. Where a package or the installer already
// put it into the folders hosts scan, nothing is asked. Otherwise (the macOS
// app, the AppImage, the .tar.gz builds) Gridline asks once, after the window is
// up, whether to copy it into this user's plug-in folders; setting
// "suite.install" holds the answer ("ask", "yes", "no"). With "yes" an updated
// Gridline updates the copies at startup. Settings > Files > Gridline Suite
// shows the state and installs, updates or removes the copies.
#pragma once

namespace gl {
class App;
}

namespace gl::suiteui {

// After the main window is up (the app only). `automated`: screenshots,
// profiling, scripted input, --hidden: never asks, never copies.
void start(App& a, bool automated);
// Per frame (drawDialogs): the first-launch question, finished copies.
void draw(App& a);
// Settings > Files.
void settingsSection(App& a);

}  // namespace gl::suiteui
