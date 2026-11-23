// Gridline - K-142 (0.7.8): what the CPU report's counters change, which must be nothing.
//  * the demo song rendered with DSP call counting on and off is bit-identical (realtime blocks,
//    resting on, one thread and four), and with counting off no instrument or effect counts a call;
//  * with counting on, the calls counted are the blocks the nodes ran (a resting node counts none);
//  * thread names: a thread that names itself is listed under that name with its processor time;
//    woke() counts only while counting is on, per name;
//  * the file system wrappers count only while counting is on and answer the same either way.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>
#include "../src/core/Demo.h"
#include "../src/core/SafeFs.h"
#include "../src/core/Song.h"
#include "../src/core/ThreadName.h"
#include "../src/engine/AudioEngine.h"
#include "../src/engine/Program.h"
#include "Check.h"

using namespace gl;

#define CHECK(x) expect((x), #x)

namespace {

constexpr int kSr = 44100;
constexpr int kHost = 512;

struct Render {
  std::vector<float> L, R;
  uint64_t calls = 0, blocks = 0, restBlocks = 0;
};

uint64_t sumCalls(const Program& p) {
  uint64_t n = 0;
  for (const auto& c : p.channels)
    if (c.rt) n += c.rt->dspCalls.load();
  for (const auto& m : p.mixer)
    for (const auto& s : m.slots)
      if (s.rt) n += s.rt->dspCalls.load();
  return n;
}

// The demo song: its first 6 seconds playing, then 6 seconds stopped (the nodes come to rest).
Render render(bool count, int threads) {
  const Song song = makeDemoTrack();
  ProgramBuilder b;
  b.setSampleRate(kSr);
  AudioEngine e(kSr);
  e.setThreads(threads);
  e.setProgram(b.build(song));
  e.setCallCounting(count);
  e.setLooping(false);
  e.setMode(PlayMode::Song);
  e.play();
  Render r;
  const int frames = kSr * 12;
  r.L.assign(static_cast<size_t>(frames), 0.f);
  r.R.assign(static_cast<size_t>(frames), 0.f);
  for (int at = 0; at < frames; at += kHost) {
    if (at >= kSr * 6 && e.playing()) e.stop();
    const int n = std::min(kHost, frames - at);
    e.process(r.L.data() + at, r.R.data() + at, n);
  }
  r.calls = sumCalls(*e.program());
  r.blocks = e.countedBlocks();
  for (const auto& c : e.program()->channels)
    if (c.rt) r.restBlocks += c.rt->restBlocks.load();
  return r;
}

void countingChangesNothingHeard() {
  for (int threads : {1, 4}) {
    const Render off = render(false, threads);
    const Render on = render(true, threads);
    CHECK(off.L == on.L && off.R == on.R);  // bit-identical
    CHECK(off.calls == 0 && off.blocks == 0);  // off: nothing counted
    CHECK(on.calls > 0 && on.blocks > 0);
    CHECK(on.restBlocks == off.restBlocks);  // resting just the same
    std::printf("threads %d: identical %d, calls off %llu on %llu, blocks on %llu\n", threads, off.L == on.L && off.R == on.R ? 1 : 0,
                static_cast<unsigned long long>(off.calls), static_cast<unsigned long long>(on.calls), static_cast<unsigned long long>(on.blocks));
  }
}

// A resting instrument counts no call; the calls of a running one are its blocks.
void callsAreBlocksRun() {
  Song s = makeEmptySong();
  Channel c;
  c.id = s.allocateId();
  c.name = "Lead";
  c.plugin = defaultPluginState("gl.synth");
  c.mixerTrack = s.masterId();
  s.channels.push_back(c);
  ProgramBuilder b;
  b.setSampleRate(kSr);
  AudioEngine e(kSr);
  e.setProgram(b.build(s));
  e.setCallCounting(true);
  std::vector<float> L(kHost), R(kHost);
  for (int i = 0; i < kSr * 8 / kHost; ++i) e.process(L.data(), R.data(), kHost);  // stopped, nothing playing
  const auto rt = b.channelRuntime(c.id);
  CHECK(rt != nullptr);
  if (!rt) return;
  const uint64_t run = rt->runBlocks.load(), rest = rt->restBlocks.load(), calls = rt->dspCalls.load();
  std::printf("idle synth: run %llu rest %llu calls %llu\n", static_cast<unsigned long long>(run), static_cast<unsigned long long>(rest),
              static_cast<unsigned long long>(calls));
  CHECK(rest > 0);          // it came to rest
  CHECK(calls == run);      // the calls are exactly the blocks it ran
  const uint64_t before = rt->dspCalls.load();
  for (int i = 0; i < 20; ++i) e.process(L.data(), R.data(), kHost);
  CHECK(rt->dspCalls.load() == before);  // resting: no call
}

void threadNamesAndWakes() {
  std::atomic<bool> stop{false};
  std::atomic<int> ready{0};
  std::thread spin([&] {
    threads::setName("gl-k142-test-thread-long-name");  // cut to 15 characters
    ready = 1;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(80);
    volatile double x = 0;
    while (std::chrono::steady_clock::now() < until) x = x + 1;  // some processor time
    while (!stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  });
  while (!ready.load()) std::this_thread::yield();
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
#if defined(__APPLE__) || defined(__linux__)
  bool found = false;
  for (const auto& t : threads::sampleThreads())
    if (t.name == "gl-k142-test-th") found = found || t.cpu > 0.02;
  CHECK(found);
#endif
  stop = true;
  spin.join();
  // Wakeups: counted only while on, by name.
  auto count = [](const std::string& name) {
    for (const auto& w : threads::wakes())
      if (w.name == name) return w.count;
    return uint64_t{0};
  };
  std::thread a([] {
    threads::setName("gl-k142-wake");
    for (int i = 0; i < 5; ++i) threads::woke();  // off: not counted
  });
  a.join();
  CHECK(count("gl-k142-wake") == 0);
  threads::setCountWakes(true);
  std::thread b([] {
    threads::setName("gl-k142-wake");
    for (int i = 0; i < 7; ++i) threads::woke();
  });
  b.join();
  threads::setCountWakes(false);
  CHECK(count("gl-k142-wake") == 7);
}

void fsCountsOnlyWhenOn() {
  const std::string here = std::filesystem::current_path().string();
  const uint64_t before = safefs::gCalls[safefs::kFsIsDir].load();
  CHECK(safefs::isDir(here));
  CHECK(safefs::gCalls[safefs::kFsIsDir].load() == before);
  safefs::gCountCalls = true;
  CHECK(safefs::isDir(here));
  CHECK(!safefs::exists(here + "/no such file k142"));
  safefs::gCountCalls = false;
  CHECK(safefs::gCalls[safefs::kFsIsDir].load() == before + 1);
  CHECK(safefs::gCalls[safefs::kFsExists].load() >= 1);
}

}  // namespace

int main() {
  countingChangesNothingHeard();
  callsAreBlocksRun();
  threadNamesAndWakes();
  fsCountsOnlyWhenOn();
  return finish();
}
