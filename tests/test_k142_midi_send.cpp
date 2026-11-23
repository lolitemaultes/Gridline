// Gridline - K-142 (0.7.8): the MIDI sync sender sleeps until a message is due.
// Before, the "gl-midi-send" thread woke every 500 microseconds whatever happened (about 1275
// times a second, 2.5 % of a core, even in an empty project). Now it sleeps until the next
// message is due, and while there is none until the engine pushes one. Checked here:
//  * WakeSignal: a post wakes a waiter at once, a timed wait ends on time;
//  * the ring's sleepUntilPushed: no wakeup while nothing is pushed, a push wakes it at once, and
//    no push is ever missed (a producer racing the sleep, many thousands of times);
//  * timing: MIDI clock (120 BPM) and MTC (25 fps) generated in simulated audio blocks are sent
//    at least as close to when they are due as the old polling sender did (both run here, on this
//    computer, side by side on the same messages, over several trials), in order, none lost, with
//    far fewer wakeups;
//  * quitting ends the sender at once, sleeping or waiting for a message.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
// output as the engine does (MidiClockOut into a ring). It feeds the same messages to several
// rings at once, one per sender, so the senders compared run side by side on the same messages at
// the same moments: whatever else the machine does then (a shared machine's other guests, other
// tests) falls on all of them alike, not on one run and not the next.
// Each block is stamped with the time it is due to start, as an audio device's callback is (on
// its own clock). Stamping it with when the simulation got to run it instead would move every
// due time with the simulation's own lateness: where timed waits end only on clock ticks, blocks
// come in bursts, and the due time a sender read would depend on which block of a burst it saw.
struct SimEngine {
  static constexpr double kSr = 44100.0;
  static constexpr int kBlock = 256;
  static constexpr double kLatency = 0.010;
  static constexpr int kMaxOut = 3;
  MidiEventRing ring[kMaxOut];
  MidiClockOut gen[kMaxOut];
  int outs = 1;
  double t0 = 0;  // when sample 0 starts
  std::thread t;
  // (before the senders start: they read t0)
  void prepare(SyncOut mode, int n) {
    outs = n;
    for (int i = 0; i < outs; ++i) {
      gen[i].out = &ring[i];
      gen[i].setMode(mode);
      gen[i].setPpq(96);
      gen[i].setSampleRate(kSr);
    }
    t0 = nowSec() + 0.05;
  }
  void run(double seconds) {
    t = std::thread([this, seconds] {
      // the engine's blocks come from the audio device's real-time thread, on time: so here too
      makeSyncSenderRealtime();
      const double bpm = 120.0, samplesPerTick = kSr * 60.0 / bpm / 96.0;
      int64_t at = 0;
      for (int i = 0; i < outs; ++i) gen[i].start(0, 0, 0);
      for (int b = 0; b * kBlock < seconds * kSr; ++b) {
        sleepUntil(t0 + static_cast<double>(at) / kSr);
        const double tk0 = static_cast<double>(at) / samplesPerTick, tk1 = static_cast<double>(at + kBlock) / samplesPerTick;
        for (int i = 0; i < outs; ++i)
          gen[i].segment(tk0, tk1, static_cast<double>(at) / kSr, static_cast<double>(at + kBlock) / kSr, at, kBlock,
                         [&](double tick) { return (tick - tk0) * samplesPerTick; });
        at += kBlock;
      }
    });
  }
  double due(const MidiOutEvent& e) const { return t0 + static_cast<double>(e.sample) / kSr + kLatency; }
};

struct Sent {
  double err;  // sent minus due, seconds
  int64_t sample;
};

// One sender over all trials: every message (pooled), and each trial's 99th percentile.
struct Result {
  std::vector<Sent> sent;
  std::vector<double> trialP99;  // |error| (ms)
  uint64_t wakeups = 0;
  double seconds = 0;
  // |error| (ms): p50 and worst of every message, p99 the trials' median; the earliest (ms, negative = early)
  double p50 = 0, p99 = 0, worst = 0, early = 0;
  bool ordered = true;
};

double percentile(std::vector<double> a, double q) {
  if (a.empty()) return 0;
  std::sort(a.begin(), a.end());
  return a[std::min(a.size() - 1, static_cast<size_t>(static_cast<double>(a.size()) * q))];
}
double median(std::vector<double> a) {
  if (a.empty()) return 0;
  std::sort(a.begin(), a.end());
  const size_t n = a.size();
  return n % 2 ? a[n / 2] : 0.5 * (a[n / 2 - 1] + a[n / 2]);
}
std::vector<double> absMs(const std::vector<Sent>& s) {
  std::vector<double> a;
  for (const auto& x : s) a.push_back(std::fabs(x.err) * 1000.0);
  return a;
}

void summarise(Result& r) {
  const std::vector<double> a = absMs(r.sent);
  if (a.empty()) return;
  r.p50 = percentile(a, 0.5);
  r.p99 = median(r.trialP99);
  r.worst = *std::max_element(a.begin(), a.end());
  r.early = 0;
  for (const auto& s : r.sent) r.early = std::min(r.early, s.err * 1000.0);
}

// The sender as it was before K-142 (MidiManager::senderLoop): every 500 microseconds. It also
// measures the machine: how late each of its plain 500 microsecond sleeps ends (`over`, ms).
void legacySender(MidiEventRing& ring, const SimEngine& e, std::atomic<bool>& quit, Result& r, std::vector<double>& over) {
  while (!quit.load()) {
    MidiOutEvent ev;
    while (ring.peek(ev)) {
      const double due = e.due(ev);
      const double now = nowSec();
      if (due > now + 0.0005 && due < now + 1.0) break;  // not yet
      ring.pop(ev);
      r.sent.push_back({nowSec() - due, ev.sample});
    }
    const double t = nowSec();
    std::this_thread::sleep_for(std::chrono::microseconds(500));
    over.push_back((nowSec() - t - 0.0005) * 1000.0);
    ++r.wakeups;
  }
}

enum Kind { kPolling = 0, kSleeping, kNoRealtime };

// One trial: the senders side by side on one simulated engine for `seconds`; their results are
// added to `res` (by Kind), the polling sender's sleep lateness to `over`.
void trial(SyncOut mode, double seconds, int kinds, Result* res, std::vector<double>& over) {
  SimEngine e;
  e.prepare(mode, kinds);
  std::atomic<bool> quit{false};
  WakeSignal wake[SimEngine::kMaxOut];
  Result r[SimEngine::kMaxOut];
  std::thread s[SimEngine::kMaxOut];
  for (int k = 0; k < kinds; ++k) {
    if (k == kPolling) {
      s[k] = std::thread([&] { legacySender(e.ring[kPolling], e, quit, r[kPolling], over); });
      continue;
    }
    s[k] = std::thread([&, k] {
      SyncSenderHooks h;
      // (no real-time policy asked for: the hooks as prepareSyncSender leaves them without it)
      if (k != kNoRealtime) prepareSyncSender(h);  // as MidiManager::senderLoop
      h.due = [&](const MidiOutEvent& ev) { return e.due(ev); };
      h.now = nowSec;
      h.send = [&, k](const MidiOutEvent& ev) { r[k].sent.push_back({nowSec() - e.due(ev), ev.sample}); };
      h.woke = [&, k] { ++r[k].wakeups; };
      runSyncSender(e.ring[k], wake[k], quit, h);
    });
  }
  const double t0 = nowSec();
  e.run(seconds);
  e.t.join();
  sleepUntil(nowSec() + 0.1);  // the last block's messages fall due
  quit = true;
  for (int k = 0; k < kinds; ++k) wake[k].post();
  for (int k = 0; k < kinds; ++k) s[k].join();
  const double took = nowSec() - t0;
  for (int k = 0; k < kinds; ++k) {
    Result& to = res[k];
    for (size_t i = 1; i < r[k].sent.size(); ++i) to.ordered = to.ordered && r[k].sent[i].sample >= r[k].sent[i - 1].sample;
    to.trialP99.push_back(percentile(absMs(r[k].sent), 0.99));
    to.sent.insert(to.sent.end(), r[k].sent.begin(), r[k].sent.end());
    to.wakeups += r[k].wakeups;
    to.seconds += took;
  }
}

void print(const char* what, const Result& r) {
  std::printf("# %-28s %5zu sent, %7.1f wakeups/s, |error| p50 %.3f ms, p99 %.3f ms (trials:", what, r.sent.size(),
              static_cast<double>(r.wakeups) / r.seconds, r.p50, r.p99);
  for (double p : r.trialP99) std::printf(" %.2f", p);
  std::printf("), worst %.3f ms, earliest %.3f ms\n", r.worst, r.early);
}

void timing(SyncOut mode, const char* name, size_t expectAtLeast) {
  // Several short trials, each running the polling sender (as before) and the sleeping one (and on
  // macOS the sleeping one without the real-time policy) side by side on the same messages. Each
  // trial's 99th percentile is compared by the trials' median: a moment of a busy machine falls in
  // one trial and moves only that trial's figure, while a sender that is really later is later in
  // every trial.
  constexpr int kTrials = 7;
  constexpr double kTrialSecs = 1.3;
#if defined(__APPLE__)
  const int kinds = 3;
#else
  const int kinds = 2;
#endif
  Result res[SimEngine::kMaxOut];
  std::vector<double> jitter;  // per trial: how much later than the typical one the slowest 1 % of plain sleeps end (ms)
  for (int t = 0; t < kTrials; ++t) {
    std::vector<double> over;
    over.reserve(4096);
    trial(mode, kTrialSecs, kinds, res, over);
    jitter.push_back(percentile(over, 0.99) - percentile(over, 0.5));
  }
  for (int k = 0; k < kinds; ++k) summarise(res[k]);
  const Result& before = res[kPolling];
  const Result& after = res[kSleeping];
  char label[96];
  std::snprintf(label, sizeof label, "%s, before (polling)", name);
  print(label, before);
  std::snprintf(label, sizeof label, "%s, after (sleeping)", name);
  print(label, after);
  expect(after.sent.size() == before.sent.size() && after.sent.size() >= expectAtLeast, "every message sent (as many as before)");
  expect(after.ordered, "in order");
  // The polling sender asks for 2000 wakeups a second. A system whose timed waits end only on its
  // clock ticks (OpenBSD and NetBSD: 10 ms, so about 50 a second) gives it far fewer: there it can
  // only send on a tick and is late by up to a tick or two.
  const double pollRate = static_cast<double>(before.wakeups) / before.seconds;
  const bool tickBound = pollRate < 1000.0;
  const double tickMs = tickBound ? 1000.0 / pollRate : 0.0;
  if (tickBound) std::printf("# timed waits end on clock ticks of about %.1f ms\n", tickMs);
  // The machine's own noise: how much later than the typical one its slowest plain sleeps end. On
  // a shared test machine (CI set) the 99th percentile may be that much worse, for the senders meet
  // such moments independently; on any other machine the bars are as they were.
  const double noise = median(jitter);
  const double allowance = std::getenv("CI") ? noise : 0.0;
  std::printf("# the slowest 1 %% of plain 0.5 ms sleeps end %.3f ms later than the typical one (allowed on a shared test machine: %.3f ms)\n",
              noise, allowance);
  // at least as close to due as before (a small allowance for a busy test machine; on clock ticks,
  // a fifth of a tick for the senders falling differently on them)
  expect(after.p99 <= std::max(before.p99, 0.5) + 0.25 + 0.2 * tickMs + allowance, "99 % of messages at least as close to due as before");
  expect(after.p50 <= std::max(before.p50, 0.3) + 0.1 + 0.2 * tickMs, "the typical message at least as close to due as before");
  expect(after.early >= -0.5, "never sent more than half a millisecond early");
#if defined(__APPLE__)
  const double fewer = 4.0;
#else
  // Away from macOS's real-time policy the sender may wait in shorter steps near each message
  // (FreeBSD's timed waits may end up to 5 % late; Linux has no real-time policy for it), as macOS
  // does without that policy: the same bar as that case below
  const double fewer = 2.0;
#endif
  if (!tickBound)
    expect(static_cast<double>(after.wakeups) < static_cast<double>(before.wakeups) / fewer, "far fewer wakeups than before");
  else  // (the poller already woke only once a tick): a tick while a message waits, one per push
    expect(after.wakeups <= before.wakeups + 2 * after.sent.size(), "no more wakeups than before besides those for pushed messages");
#if defined(__APPLE__)
  // without the real-time policy (refused, or GRIDLINE_NO_REALTIME): its timed waits still end on
  // time (WakeSignal's critical timer: before it, a process the system ran at a lower latency tier,
  // as shared test machines do, had them end several times the wait late now and then)
  const Result& coarse = res[kNoRealtime];
  std::snprintf(label, sizeof label, "%s, after, no real-time", name);
  print(label, coarse);
  expect(coarse.sent.size() == before.sent.size() && coarse.ordered, "no real-time policy: every message, in order");
  expect(coarse.p99 <= std::max(before.p99, 0.5) + 0.5 + 0.2 * tickMs + allowance && coarse.early >= -0.5,
         "no real-time policy: about as close to due as before");
  if (!tickBound)
    expect(static_cast<double>(coarse.wakeups) < static_cast<double>(before.wakeups) / 2.0, "no real-time policy: still far fewer wakeups");
  else  // (its waits end late: it looks on every tick while a message waits, as the poller did)
    expect(coarse.wakeups <= before.wakeups + 2 * coarse.sent.size(), "no real-time policy: no more wakeups than before besides those for pushed messages");
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
  // posts racing short timed waits that end: each post taken exactly once, none lost, none extra
  constexpr int kPosts = 5000;
  std::thread q([&] {
    for (int i = 0; i < kPosts; ++i) {
      w.post();
      if (i % 3 == 0) std::this_thread::yield();
    }
  });
  int taken = 0;
  t = nowSec();
  int waits = 0;
  while (taken < kPosts && nowSec() - t < 30.0) taken += w.waitFor(0.00002 * (++waits % 5)) ? 1 : 0;
  const double tookRace = nowSec() - t;
  q.join();
  const bool extra = w.waitFor(0.02);
  std::printf("# posts racing timed waits: %d of %d taken in %.3f s (%d waits)%s\n", taken, kPosts, tookRace, waits, extra ? ", then one more" : "");
  expect(taken == kPosts && !extra, "posts racing timed waits: each taken once");
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
  timing(SyncOut::Clock, "MIDI clock 120 BPM", 400);
  timing(SyncOut::Mtc25, "MTC 25 fps", 850);
  return finish();
}
