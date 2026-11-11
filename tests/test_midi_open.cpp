// Gridline: opening the system MIDI service without trusting it (MidiOpener): an answer is
// taken, a refusal or a throw becomes an error with a retry on a growing delay, a probe that
// hangs times out without blocking anyone, is never doubled by a retry, and its late answer
// still counts; Retry starts a new probe at once. Simulated probes only, no MIDI service.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include "../src/core/MidiOpen.h"
#include "Check.h"

using namespace gl;

namespace {

MidiProbeResult okResult() {
  MidiProbeResult r;
  r.ok = true;
  r.inputs = {"Keys In"};
  r.outputs = {"Keys Out", "Synth"};
  return r;
}

// A probe that blocks until the test opens the gate (a hung MIDI service).
struct Gate {
  std::mutex mu;
  std::condition_variable cv;
  bool open = false;
  std::atomic<int> entered{0}, left{0};
  void release() {
    std::lock_guard<std::mutex> lk(mu);
    open = true;
    cv.notify_all();
  }
};

double secondsSince(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

int main() {
  expect(std::string(midiOpenStateName(MidiOpenState::Ready)) == "ready" &&
             std::string(midiOpenStateName(MidiOpenState::NotResponding)) == "not-responding" &&
             std::string(midiOpenStateName(MidiOpenState::Failed)) == "failed",
         "state names");

  {  // a working service: the ports come back
    MidiOpener o([] { return okResult(); });
    expect(o.state() == MidiOpenState::Idle && o.attempts() == 0, "idle before the start");
    o.start(0.0);
    expect(o.state() == MidiOpenState::Opening && o.attempts() == 1, "opening after the start");
    expect(o.waitFor(5.0), "a working probe answers");
    expect(o.update(0.01), "update takes the answer");
    expect(o.state() == MidiOpenState::Ready && o.error().empty(), "ready");
    const MidiProbeResult r = o.takeResult();
    expect(r.ok && r.inputs.size() == 1 && r.outputs.size() == 2 && r.outputs[1] == "Synth", "the port lists arrive");
    expect(o.nextRetryIn(0.01) < 0, "no retry once ready");
    expect(!o.update(100.0) && o.state() == MidiOpenState::Ready && o.attempts() == 1, "ready stays ready, no more probes");
    o.start(200.0);  // a rescan
    expect(o.attempts() == 2 && o.state() == MidiOpenState::Opening, "a rescan starts a new probe");
    o.waitFor(5.0);
    expect(o.update(200.01) && o.state() == MidiOpenState::Ready, "the rescan answers");
  }

  {  // a refusing service: an error, retries on a growing delay
    std::atomic<int> calls{0};
    MidiOpener o(
        [&calls] {
          ++calls;
          MidiProbeResult r;
          r.error = "error creating the MIDI client (268435460)";
          return r;
        },
        3.0, {5.0, 15.0, 30.0});
    o.start(0.0);
    o.waitFor(5.0);
    expect(o.update(0.0) && o.state() == MidiOpenState::Failed, "a refusal fails");
    expect(o.error() == "error creating the MIDI client (268435460)", "the service's error text is kept");
    expect(!o.takeResult().ok, "the result says not ok");
    expect(o.nextRetryIn(0.0) > 4.99 && o.nextRetryIn(0.0) < 5.01, "first retry in 5 s");
    expect(!o.update(4.0) && o.attempts() == 1, "no retry before its time");
    o.update(5.0);
    expect(o.attempts() == 2 && o.state() == MidiOpenState::Opening, "it retries on its own");
    o.waitFor(5.0);
    o.update(5.1);
    expect(o.state() == MidiOpenState::Failed && o.nextRetryIn(5.1) > 14.9 && o.nextRetryIn(5.1) < 15.1, "the delay grows");
    o.update(20.1);
    o.waitFor(5.0);
    o.update(20.2);
    o.update(50.2);
    o.waitFor(5.0);
    o.update(50.3);
    expect(o.nextRetryIn(50.3) > 29.9 && o.nextRetryIn(50.3) < 30.1, "the last delay repeats");
    expect(calls.load() == o.attempts() && o.attempts() == 4, "one probe per attempt");
    o.retryNow(51.0);
    expect(o.attempts() == 5 && o.state() == MidiOpenState::Opening, "Retry starts a probe at once");
    o.waitFor(5.0);
    o.update(51.1);
    expect(o.nextRetryIn(51.1) > 4.99 && o.nextRetryIn(51.1) < 5.01, "Retry resets the delay");
  }

  {  // throws from the driver become errors, never a crash
    MidiOpener o([]() -> MidiProbeResult { throw std::runtime_error("driver exploded"); });
    o.start(0.0);
    o.waitFor(5.0);
    o.update(0.0);
    expect(o.state() == MidiOpenState::Failed && o.error() == "driver exploded", "a thrown exception is an error");
    MidiOpener o2([]() -> MidiProbeResult { throw 42; });
    o2.start(0.0);
    o2.waitFor(5.0);
    o2.update(0.0);
    expect(o2.state() == MidiOpenState::Failed && o2.error() == "unknown error", "a thrown non-exception is an error");
    MidiOpener o3([] { return MidiProbeResult{}; });  // not ok, no text
    o3.start(0.0);
    o3.waitFor(5.0);
    o3.update(0.0);
    expect(o3.state() == MidiOpenState::Failed && !o3.error().empty(), "a failure always carries a reason");
  }

  {  // a hung service: a timeout, no second probe beside it, its late answer still counts
    auto gate = std::make_shared<Gate>();
    std::atomic<int> calls{0};
    auto opener = std::make_unique<MidiOpener>(
        [gate, &calls] {
          ++calls;
          ++gate->entered;
          std::unique_lock<std::mutex> lk(gate->mu);
          gate->cv.wait(lk, [&] { return gate->open; });
          ++gate->left;
          return okResult();
        },
        3.0, std::vector<double>{5.0, 15.0});
    MidiOpener& o = *opener;
    const auto t0 = std::chrono::steady_clock::now();
    o.start(0.0);
    expect(secondsSince(t0) < 0.5, "start never waits on a hung service");
    const auto t1 = std::chrono::steady_clock::now();
    expect(!o.waitFor(0.2), "the startup wait gives up");
    const double waited = secondsSince(t1);
    expect(waited >= 0.15 && waited < 2.0, "... after the time it was given");
    expect(!o.update(1.0) && o.state() == MidiOpenState::Opening, "within the timeout: opening");
    expect(!o.update(3.0) && o.state() == MidiOpenState::NotResponding, "past the timeout: not responding");
    expect(o.probeRunning() && o.nextRetryIn(3.0) > 4.99, "a retry is scheduled, the probe still out");
    o.update(8.5);  // the scheduled retry comes while the probe is still out
    expect(o.attempts() == 1 && o.state() == MidiOpenState::NotResponding, "no second probe beside a hung one");
    expect(o.nextRetryIn(8.5) > 14.9, "it waits on it longer");
    o.retryNow(9.0);
    expect(o.attempts() == 1 && calls.load() == 1, "Retry keeps waiting on the hung probe");
    gate->release();  // the service comes back
    expect(o.waitFor(5.0), "the late answer arrives");
    expect(o.update(9.5) && o.state() == MidiOpenState::Ready, "the late answer is taken: ready");
    expect(o.takeResult().inputs.size() == 1, "with its ports");
    expect(gate->entered.load() == 1 && gate->left.load() == 1, "exactly one probe ran");

    // An opener destroyed while its probe hangs: nothing waits, the probe ends on its own later.
    auto gate2 = std::make_shared<Gate>();
    {
      MidiOpener o2([gate2] {
        ++gate2->entered;
        std::unique_lock<std::mutex> lk(gate2->mu);
        gate2->cv.wait(lk, [&] { return gate2->open; });
        ++gate2->left;
        return okResult();
      });
      o2.start(0.0);
      while (gate2->entered.load() == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    expect(gate2->left.load() == 0, "an opener with a hung probe is destroyed without waiting");
    gate2->release();
    for (int i = 0; i < 500 && gate2->left.load() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    expect(gate2->left.load() == 1, "the orphaned probe finishes on its own");
  }

  {  // a service that refuses, then works: Retry recovers
    std::atomic<int> calls{0};
    MidiOpener o([&calls] {
      if (++calls < 2) {
        MidiProbeResult r;
        r.error = "busy";
        return r;
      }
      return okResult();
    });
    o.start(0.0);
    o.waitFor(5.0);
    o.update(0.0);
    expect(o.state() == MidiOpenState::Failed, "first try refused");
    o.retryNow(1.0);
    o.waitFor(5.0);
    expect(o.update(1.1) && o.state() == MidiOpenState::Ready && o.error().empty(), "Retry recovers");
    expect(o.takeResult().outputs.size() == 2, "the ports after recovery");
  }

  {  // start() while a probe is out is a no-op
    auto gate = std::make_shared<Gate>();
    MidiOpener o([gate] {
      std::unique_lock<std::mutex> lk(gate->mu);
      gate->cv.wait(lk, [&] { return gate->open; });
      return okResult();
    });
    o.start(0.0);
    o.start(0.1);
    o.retryNow(0.2);  // within the timeout: keeps waiting
    expect(o.attempts() == 1 && o.state() == MidiOpenState::Opening, "no second probe while one is out");
    gate->release();
    o.waitFor(5.0);
    expect(o.update(0.3) && o.state() == MidiOpenState::Ready, "the one probe answers");
  }

  return finish();
}
