// Gridline: K-36: the Audio Unit listener and the backend's lifetime, on Apple's own AUDelay.
//   Edits made in a unit (what its view does) still reach the host as events with the right
//   normalised value. A stress of create, listen, edit and destroy, with edits landing right up to
//   the destruction and the parameter list being read again while blocks run. Run it under
//   ThreadSanitizer too: nothing the listener block reads may belong to the backend.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "host/PluginScan.h"

using namespace gl;
using Clock = std::chrono::steady_clock;

namespace {

const char* const kUnit = "aufx/dely/appl";

std::unique_ptr<host::Backend> make() {
  std::string err;
  auto b = host::findFormat("au")->create(kUnit, "", &err);
  if (!b) std::printf("# create: %s\n", err.c_str());
  return b;
}

// The first writable parameter; the expected normalised value is read back from the unit.
void testEventsArrive() {
  auto b = make();
  expect(b != nullptr, "AUDelay opens");
  if (!b) return;
  uint32_t id = 0;
  bool found = false;
  for (const auto& p : b->params())
    if (!(p.flags & host::kParamReadOnly) && !found) {
      id = p.id;
      found = true;
    }
  expect(found, "it has a writable parameter");
  const float plain = 0.5f;
  expect(host::auPostParameterForTests(*b, id, plain), "an edit can be posted");
  const double norm = b->paramNormalized(id);
  std::vector<host::HostEvent> got;
  const auto until = Clock::now() + std::chrono::seconds(5);
  bool seen = false;
  double value = -1;
  while (!seen && Clock::now() < until) {
    got.clear();
    b->drainHostEvents(got);
    for (const auto& e : got)
      if (e.kind == host::HostEvent::PerformEdit && e.id == id) {
        seen = true;
        value = e.value;
      }
    if (!seen) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  expect(seen, "an edit in the unit reaches the host as an event");
  expect(std::fabs(value - norm) < 1e-5, "... with the normalised value of the parameter");
}

// The parameter list is read again (a unit announcing a new list) while edits arrive.
void testParamListChange() {
  auto b = make();
  if (!b) return;
  const uint32_t id = b->params().empty() ? 0 : b->params().front().id;
  std::atomic<bool> stop{false};
  std::thread edits([&] {
    float v = 0;
    while (!stop.load()) {
      host::auPostParameterForTests(*b, id, v);
      v = v > 0.9f ? 0 : v + 0.05f;
      std::this_thread::yield();
    }
  });
  std::vector<host::HostEvent> sink;
  int kept = 0;
  for (int k = 0; k < 400; ++k) {
    b->restart(host::kRestartParamInfo);
    kept += b->params().empty() ? 0 : 1;
    sink.clear();
    b->drainHostEvents(sink);
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  stop = true;
  edits.join();
  expect(kept == 400, "the parameter list stays readable while edits arrive");
}

void testStress() {
  const int rounds = 300;
  long events = 0;
  int made = 0;
  for (int r = 0; r < rounds; ++r) {
    auto b = make();
    if (!b) continue;
    ++made;
    const uint32_t id = b->params().empty() ? 0 : b->params().front().id;
    // Some rounds give the listener time to run; others destroy with edits still on their way.
    const int edits = 20 + r % 30;
    std::atomic<bool> stop{false};
    std::thread other([&] {
      float v = 0;
      while (!stop.load()) {
        host::auPostParameterForTests(*b, id, v);
        v = v > 0.9f ? 0 : v + 0.1f;
      }
    });
    for (int k = 0; k < edits; ++k) host::auPostParameterForTests(*b, id, 0.01f * static_cast<float>(k));
    if (r % 3 == 1) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (r % 3 == 2) {
      std::vector<host::HostEvent> sink;
      std::this_thread::sleep_for(std::chrono::milliseconds(12));
      b->drainHostEvents(sink);
      events += static_cast<long>(sink.size());
    }
    stop = true;
    other.join();
    b.reset();
  }
  std::printf("# stress: %d instances, %ld events drained\n", made, events);
  expect(made == rounds, "stress: every instance opened");
  expect(events > 0, "stress: the listener was really running");
  // Anything still pending runs here, with every backend gone (a sanitizer or a crash tells).
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

}  // namespace

int main() {
  test::hermeticTestEnv("k36");
  testEventsArrive();
  testParamListChange();
  testStress();
  return finish();
}
