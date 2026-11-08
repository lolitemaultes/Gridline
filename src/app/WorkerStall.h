// Gridline - K-133: render workers that hang (core/WorkerWatch.h) are told to the person. The
// interface thread calls watch() once a frame for each running job; the worker beats as it moves on.
#pragma once
#include <cstdint>
#include <string>
#include "../core/WorkerWatch.h"

namespace gl {
class App;
}

namespace gl::workerstall {

// Milliseconds on a steady clock (the watch's time).
int64_t nowMs();
// A job starts: the watch counts from now.
void start(StallWatch& w, const WorkerBeat& b);
// Once a frame while the job runs: a notification (an error, with the job and its last step) when
// the worker has not moved on for the watch's limit, once, and another when it moves on again. The
// log and the crash breadcrumbs keep both. True while the worker counts as stalled.
bool watch(App& a, StallWatch& w, const WorkerBeat& b, const std::string& job, const std::string& status);

}  // namespace gl::workerstall
