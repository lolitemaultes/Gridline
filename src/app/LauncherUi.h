// Gridline - J-141: the welcome launcher, Gridline's home screen. It shows before a project
// (plain launches) and again from the logo button in the project view: a large grid of
// recent projects (sketches of their arrangements, name, when, tempo, length), the
// new-project tile with the Gridline mark first, templates, the demo song, Open, and
// recent-list management. The logic (start-up plan, state machine, layout) is in
// core/Launcher.h; this file draws it and keeps the sketches (core/ProjectPreview.h).
#pragma once
#include <string>
#include "../core/ProjectPreview.h"

namespace gl {
class App;
}

namespace gl::launcherui {

// Per frame while a.home.visible(): the whole window (main.cpp skips the project view).
void draw(App& a);
// Per frame while hidden: the project view fading back in for a moment after the launcher left.
void drawReturnFade(App& a);
// The sketch of a project just opened or saved (no disk read): kept for the launcher and
// written to the cache in the background.
void rememberSketch(App& a, const std::string& path, preview::Sketch k);
// 0 = Recent, 1 = Templates (dev: --show launcher-templates).
void setView(int view);
// Dev check (--dev-input "launcher"): the launcher's state and tiles to stdout.
void devReport(App& a);
// Stops the sketch worker (App's destructor).
void shutdown();

}  // namespace gl::launcherui
