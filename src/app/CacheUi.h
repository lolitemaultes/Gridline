// Gridline - K-45: the unpacked project cache in the app (core/ProjectArchive.h). One worker does
// every slow step off the interface thread: the sweep at startup and after a project closes, the size
// shown in Settings and Clear cache. The app says which copies its project uses; the worker keeps
// them claimed. Nothing here waits on the disk on the interface thread.
#pragma once
#include <string>
#include <vector>
#include "../core/ProjectArchive.h"

namespace gl {
class App;
}

namespace gl::cacheui {

// Per frame: the sweep for what a crash left, once, a moment after the start (not in an embedded
// project, which owns no project files).
void tick(App& a);
// Scripted runs on the default cache: no startup sweep there.
void skipStartupSweep();
// A project replaced the open one. `use`: the copy it was opened from (may be empty); the copies the
// new song refers to are claimed too. What the old project used is released and swept a moment later.
void projectChanged(App& a, archive::CopyHold use);
// The app is quitting: the same, without the wait, for at most a moment (the next start finishes it).
void shutdown(App& a);
// Settings > Files.
void settingsSection(App& a);
// dev-input: "cache state": the size, sweeps, the button's place and the last report to stdout;
// "cache list": what is in the cache; "cache measure" asks for the size; "cache memory": what the
// samples and clip renders hold.
void devCommand(const std::vector<std::string>& args);

}  // namespace gl::cacheui
