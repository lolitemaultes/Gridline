// Gridline - K-133: see WorkerStall.h.
#include "WorkerStall.h"
#include <chrono>
#include "../core/Crash.h"
#include "../core/Log.h"
#include "App.h"
#include "DevSignal.h"

namespace gl::workerstall {

int64_t nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void start(StallWatch& w, const WorkerBeat& b) { w.start(nowMs(), b.count()); }

bool watch(App& a, StallWatch& w, const WorkerBeat& b, const std::string& job, const std::string& status) {
  const int64_t now = nowMs();
  switch (w.update(now, b.count())) {
    case StallWatch::Event::Stalled: {
      const std::string details = job + (status.empty() ? std::string() : ", " + status) + ", no progress for " +
                                  std::to_string(w.silentMs(now) / 1000) + " s";
      crash::crumb("worker stalled", job.c_str());
      a.notify("The work in progress has stopped moving on. A plugin or the disk may not be responding: wait, or press Cancel.", true, details);
      devsig::reach("worker-stalled");
      break;
    }
    case StallWatch::Event::Moving:
      crash::crumb("worker moving again", job.c_str());
      a.notify("The work in progress is moving on again.", false, job);
      devsig::reach("worker-moving");
      break;
    case StallWatch::Event::None: break;
  }
  return w.stalled();
}

}  // namespace gl::workerstall
