// Gridline - K-328: quitting during a collaboration. Closing the last link releases the router
// port mapper (libplum), whose cleanup runs on a thread nobody joins (it asks the router to remove
// mappings, which can take seconds), and libplum calls back into Gridline (log lines, mappings
// destroyed) from its own threads. Quitting then ran static teardown while such a thread still used
// the mapper's state: a destroyed mutex ("mutex lock failed"), abort, exit code 134 for one of the
// two peers in k141_collab_undo_app now and then. The state now outlives static teardown.
// This test ends its process while threads keep using the mapper: before the fix it aborted (or
// crashed on the destroyed log) on the way out; now it exits 0.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
#include "Check.h"
#include "collab/Link.h"

using namespace gl::collab;

namespace {
// trivially destructible: the detached thread uses them during exit
std::atomic<long> gRounds{0};
std::atomic<long> gSpin{0};
std::atomic<bool> gArmed{false};
// Constructed before the library's statics (this file links first), so destroyed after them: the
// last step of static teardown waits a little, the way a quitting app's long teardown does, while
// the threads still run. (The order across files is up to the linker; where it differs the check
// is weaker, never wrong.)
struct LingerAtExit {
  ~LingerAtExit() {
    if (gArmed.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
  }
} gLinger;
}  // namespace

int main() {
#if defined(GRIDLINE_COLLAB_NET)
  // Acquire / release cycles: each release starts a cleanup thread, the next acquire waits for it.
  for (int i = 0; i < 3; ++i) {
    portMapperAcquire();
    portMapperRelease();
  }
  expect(true, "the port mapper starts and stops again");
  portMapperCapture(true);
  portMapperAcquire();
  const std::vector<std::string> log = portMapperLog();
  portMapperCapture(false);
  expect(true, "the port mapper's log can be read while it runs");
  std::printf("# %zu libplum log lines while starting\n", log.size());
  // A thread that keeps using the mapper's state (as libplum's threads do) while the process ends.
  std::thread([] {
    for (;;) {
      portMapperCapture(true);
      (void)portMapperLog();
      gRounds.fetch_add(1, std::memory_order_relaxed);
      // unlocked most of the time, but never asleep (a mutex destroyed while locked stays usable
      // on macOS, and the process ends a few microseconds after static teardown)
      for (int spin = 0; spin < 200; ++spin) {
        gSpin.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }).detach();
  while (gRounds.load(std::memory_order_relaxed) < 100) {
    std::this_thread::yield();
  }
  gArmed = true;
  portMapperRelease();  // its cleanup thread (logging through the same state) runs into the exit too
  expect(true, "the process ends while threads still use the port mapper");
#else
  expect(true, "networking is not built in (nothing to check)");
#endif
  return finish();
}
