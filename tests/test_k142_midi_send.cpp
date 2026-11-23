// Gridline - K-142 (0.7.8): the MIDI sync sender sleeps until a message is due.
// Before, the "gl-midi-send" thread woke every 500 microseconds whatever happened (about 1275
// times a second, 2.5 % of a core, even in an empty project). Now it sleeps until the next
// message is due, and while there is none until the engine pushes one. Checked here:
//  * WakeSignal: a post wakes a waiter at once, a timed wait ends on time;
//  * the ring's sleepUntilPushed: no wakeup while nothing is pushed, a push wakes it at once, and
//    no push is ever missed (a producer racing the sleep, many thousands of times);
//  * timing: MIDI clock (120 BPM) and MTC (25 fps) generated in simulated audio blocks are sent
//    at least as close to when they are due as the old polling sender did (both measured here, on
//    this computer, side by side), in order, none lost, with far fewer wakeups;
//  * quitting ends the sender at once, sleeping or waiting for a message.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>
#include "../src/core/MidiSync.h"

#include "Check.h"

using namespace gl;

#define CHECK(x) expect((x), #x)

namespace {

double nowSec() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

void sleepUntil(double t) {
  const double d = t - nowSec();
  if (d > 0) std::this_thread::sleep_for(std::chrono::duration<double>(d));
}

// A simulated audio engine: blocks of 256 samples at 44.1 kHz in real time, generating sync
// output as the engine does (MidiClockOut into the ring, the block's time and first sample
// published for the sender).
struct SimEngine {
  static constexpr double kSr = 44100.0;
  static constexpr int kBlock = 256;
  static constexpr double kLatency = 0.010;
  MidiEventRing ring;
  MidiClockOut gen;
  std::atomic<double> blockTime{0};
  std::atomic<int64_t> blockSample{0};
  std::atomic<bool> stop{false};
  std::thread t;
  void start(SyncOut mode, double seconds) {
    gen.out = &ring;
    gen.setMode(mode);
    gen.setPpq(96);
    gen.setSampleRate(kSr);
    t = std::thread([this, seconds] {
      // the engine's blocks come from the audio device's real-time thread, on time: so here too
      // (else the simulated block times themselves jitter by milliseconds)
      makeSyncSenderRealtime();
      const double bpm = 120.0, samplesPerTick = kSr * 60.0 / bpm / 96.0;
      const double t0 = nowSec() + 0.02;
      int64_t at = 0;
      gen.start(0, 0, 0);
      for (int b = 0; !stop.load() && b * kBlock < seconds * kSr; ++b) {
        const double start = t0 + static_cast<double>(at) / kSr;
        sleepUntil(start);
        blockTime.store(nowSec());  // as the engine stamps a block: when its callback ran
        blockSample.store(at);
        const double tk0 = static_cast<double>(at) / samplesPerTick, tk1 = static_cast<double>(at + kBlock) / samplesPerTick;
        gen.segment(tk0, tk1, static_cast<double>(at) / kSr, static_cast<double>(at + kBlock) / kSr, at, kBlock,
                    [&](double tick) { return (tick - tk0) * samplesPerTick; });
        at += kBlock;
      }
    });
  }
  double due(const MidiOutEvent& e) const {
    return blockTime.load() + static_cast<double>(e.sample - blockSample.load()) / kSr + kLatency;
  }
};

struct Sent {
  double err;  // sent minus due, seconds
  int64_t sample;
};

struct Result {
  std::vector<Sent> sent;
  uint64_t wakeups = 0;
  double seconds = 0;
  double p50 = 0, p99 = 0, worst = 0, early = 0;  // |error| (ms), and the earliest (ms, negative = early)
  bool ordered = true;
};

void summarise(Result& r) {
  std::vector<double> a;
  for (const auto& s : r.sent) a.push_back(std::fabs(s.err) * 1000.0);
  std::sort(a.begin(), a.end());
  if (a.empty()) return;
  r.p50 = a[a.size() / 2];
  r.p99 = a[std::min(a.size() - 1, static_cast<size_t>(static_cast<double>(a.size()) * 0.99))];
  r.worst = a.back();
  r.early = 0;
  for (const auto& s : r.sent) r.early = std::min(r.early, s.err * 1000.0);
  for (size_t i = 1; i < r.sent.size(); ++i) r.ordered = r.ordered && r.sent[i].sample >= r.sent[i - 1].sample;
}

// The sender as it was before K-142 (MidiManager::senderLoop): every 500 microseconds.
void legacySender(SimEngine& e, std::atomic<bool>& quit, Result& r) {
  while (!quit.load()) {
    MidiOutEvent ev;
    while (e.ring.peek(ev)) {
      const double due = e.due(ev);
      const double now = nowSec();
      if (due > now + 0.0005 && due < now + 1.0) break;  // not yet
      e.ring.pop(ev);
      r.sent.push_back({nowSec() - due, ev.sample});
    }
    std::this_thread::sleep_for(std::chrono::microseconds(500));
    ++r.wakeups;
  }
}

Result measure(SyncOut mode, bool legacy, double seconds, bool coarse = false) {
  SimEngine e;
  Result r;
  std::atomic<bool> quit{false};
  WakeSignal wake;
  std::thread s;
  if (legacy) {
    s = std::thread([&] { legacySender(e, quit, r); });
  } else {
    s = std::thread([&] {
      SyncSenderHooks h;
      if (coarse) h.coarseTimers = true;  // (no real-time policy asked for)
      else prepareSyncSender(h);          // as MidiManager::senderLoop
      // (measured against the due time the sender went by, as for the polling sender: the next
      // block's time can come in between when a busy machine holds the sender up, or when timed
      // waits end on clock ticks and a block that was held back is stamped in the same tick)
      double decided = 0;
      h.due = [&](const MidiOutEvent& ev) { return decided = e.due(ev); };
      h.now = nowSec;
      h.send = [&](const MidiOutEvent& ev) { r.sent.push_back({nowSec() - decided, ev.sample}); };
      h.woke = [&] { ++r.wakeups; };
      runSyncSender(e.ring, wake, quit, h);
    });
  }
  const double t0 = nowSec();
  e.start(mode, seconds);
  e.t.join();
  sleepUntil(nowSec() + 0.1);  // the last block's messages fall due
  quit = true;
  wake.post();
  s.join();
  r.seconds = nowSec() - t0;
  summarise(r);
  return r;
}

void print(const char* what, const Result& r) {
  std::printf("# %-28s %5zu sent, %7.1f wakeups/s, |error| p50 %.3f ms, p99 %.3f ms, worst %.3f ms, earliest %.3f ms\n", what, r.sent.size(),
              static_cast<double>(r.wakeups) / r.seconds, r.p50, r.p99, r.worst, r.early);
}

void timing(SyncOut mode, const char* name, size_t expectAtLeast) {
  const double secs = 4.0;
  const Result before = measure(mode, true, secs);
  const Result after = measure(mode, false, secs);
  char label[96];
  std::snprintf(label, sizeof label, "%s, before (polling)", name);
  print(label, before);
  std::snprintf(label, sizeof label, "%s, after (sleeping)", name);
  print(label, after);
  expect(after.sent.size() == before.sent.size() && after.sent.size() >= expectAtLeast, "every message sent (as many as before)");
  expect(after.ordered, "in order");
  // The polling sender asks for 2000 wakeups a second. A system whose timed waits end only on its
  // clock ticks (OpenBSD and NetBSD: 10 ms, so about 50 a second) gives it far fewer: there both
  // senders can only send on a tick, both are late by up to a tick or two, and the sleeping one
  // looks on every tick while a message waits (SyncSenderHooks::tickTimers), as the poller did.
  const double pollRate = static_cast<double>(before.wakeups) / before.seconds;
  const bool tickBound = pollRate < 1000.0;
  const double tickMs = tickBound ? 1000.0 / pollRate : 0.0;
  if (tickBound) std::printf("# timed waits end on clock ticks of about %.1f ms\n", tickMs);
  // at least as close to due as before (a small allowance for a busy test machine; on clock ticks,
  // a fifth of a tick for the two runs falling differently on them)
  expect(after.p99 <= std::max(before.p99, 0.5) + 0.25 + 0.2 * tickMs, "99 % of messages at least as close to due as before");
  expect(after.p50 <= std::max(before.p50, 0.3) + 0.1 + 0.2 * tickMs, "the typical message at least as close to due as before");
  expect(after.early >= -0.5, "never sent more than half a millisecond early");
#if defined(__FreeBSD__)
  // FreeBSD waits in shorter steps (timed waits may end up to 5 % late), as macOS does without the
  // real-time policy: the same bar as that case below
  const double fewer = 2.0;
#else
  const double fewer = 4.0;
#endif
  if (!tickBound)
    expect(static_cast<double>(after.wakeups) < static_cast<double>(before.wakeups) / fewer, "far fewer wakeups than before");
  else  // (the poller already woke only once a tick): a tick while a message waits, one per push
    expect(after.wakeups <= before.wakeups + 2 * after.sent.size(), "no more wakeups than before besides those for pushed messages");
#if defined(__APPLE__)
  // without the real-time policy (refused, or GRIDLINE_NO_REALTIME): shorter steps keep it on time
  const Result coarse = measure(mode, false, secs, true);
  std::snprintf(label, sizeof label, "%s, after, no real-time", name);
  print(label, coarse);
  expect(coarse.sent.size() == before.sent.size() && coarse.ordered, "no real-time policy: every message, in order");
  expect(coarse.p99 <= std::max(before.p99, 0.5) + 0.5 && coarse.early >= -0.5, "no real-time policy: about as close to due as before");
  expect(static_cast<double>(coarse.wakeups) < static_cast<double>(before.wakeups) / 2.0, "no real-time policy: still far fewer wakeups");
#endif
}

void wakeSignal() {
  WakeSignal w;
  double t = nowSec();
  CHECK(!w.waitFor(0.05));
  const double took = nowSec() - t;
  expect(took >= 0.045 && took < 0.5, "a timed wait ends on time");
  w.post();
  t = nowSec();
  CHECK(w.waitFor(1.0));
  expect(nowSec() - t < 0.05, "a post before the wait: it returns at once");
  std::thread p([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    w.post();
  });
  t = nowSec();
  CHECK(w.waitFor(2.0));
  const double woke = nowSec() - t;
  p.join();
  expect(woke < 0.5, "a post wakes a waiter");
}

void ringSleep() {
  MidiEventRing ring;
  WakeSignal w;
  std::atomic<bool> quit{false};
  std::atomic<uint64_t> wakes{0}, got{0};
  std::atomic<double> lastGot{0};
  std::thread c([&] {
    SyncSenderHooks h;
    h.due = [](const MidiOutEvent&) { return 0.0; };  // due at once
    h.now = nowSec;
    h.send = [&](const MidiOutEvent&) {
      ++got;
      lastGot.store(nowSec());
    };
    h.woke = [&] { ++wakes; };
    runSyncSender(ring, w, quit, h);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const uint64_t w0 = wakes.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  expect(wakes.load() - w0 == 0, "an empty ring: no wakeup in a second");
  // one push wakes it at once
  std::vector<double> lat;
  for (int i = 0; i < 50; ++i) {
    const uint64_t g = got.load();
    MidiOutEvent e;
    e.size = 1;
    e.data[0] = 0xF8;
    const double t = nowSec();
    ring.push(e);
    while (got.load() == g && nowSec() - t < 1.0) std::this_thread::yield();
    lat.push_back((lastGot.load() - t) * 1000.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
  }
  std::sort(lat.begin(), lat.end());
  std::printf("# push to send: median %.3f ms, worst %.3f ms\n", lat[lat.size() / 2], lat.back());
  expect(got.load() == 50, "every pushed message sent");
  expect(lat[lat.size() / 2] < 2.0 && lat.back() < 50.0, "a push wakes the sleeping sender at once");
  // a producer racing the sleep: never a message left behind
  const uint64_t g0 = got.load();
  for (int i = 0; i < 20000; ++i) {
    MidiOutEvent e;
    e.size = 1;
    while (!ring.push(e)) std::this_thread::yield();
    if (i % 7 == 0) std::this_thread::yield();
  }
  const double t = nowSec();
  while (got.load() - g0 < 20000 && nowSec() - t < 5.0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  expect(got.load() - g0 == 20000, "20000 racing pushes: none missed");
  // quitting while asleep
  quit = true;
  w.post();
  const double q = nowSec();
  c.join();
  expect(nowSec() - q < 0.2, "quitting wakes the sleeping sender");
}

void quitWhileWaiting() {
  MidiEventRing ring;
  WakeSignal w;
  std::atomic<bool> quit{false};
  std::atomic<int> sent{0};
  const double far = nowSec() + 0.9;  // due in 0.9 s
  std::thread c([&] {
    SyncSenderHooks h;
    h.due = [&](const MidiOutEvent&) { return far; };
    h.now = nowSec;
    h.send = [&](const MidiOutEvent&) { ++sent; };
    runSyncSender(ring, w, quit, h);
  });
  MidiOutEvent e;
  e.size = 1;
  ring.push(e);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  quit = true;
  w.post();
  const double q = nowSec();
  c.join();
  expect(nowSec() - q < 0.2 && sent.load() == 0, "quitting while waiting for a message due later ends at once");
  // an event 'due' more than a second away (a clock jump) goes at once, as before
  MidiEventRing r2;
  WakeSignal w2;
  std::atomic<bool> q2{false};
  std::atomic<int> s2{0};
  std::thread c2([&] {
    SyncSenderHooks h;
    h.due = [](const MidiOutEvent&) { return nowSec() + 5.0; };
    h.now = nowSec;
    h.send = [&](const MidiOutEvent&) { ++s2; };
    runSyncSender(r2, w2, q2, h);
  });
  r2.push(e);
  const double t = nowSec();
  while (s2.load() == 0 && nowSec() - t < 1.0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  expect(s2.load() == 1, "a message due more than a second away is sent at once (as before)");
  q2 = true;
  w2.post();
  c2.join();
}

}  // namespace

int main() {
  wakeSignal();
  ringSleep();
  quitWhileWaiting();
  timing(SyncOut::Clock, "MIDI clock 120 BPM", 180);
  timing(SyncOut::Mtc25, "MTC 25 fps", 380);
  return finish();
}
