// Gridline: GitHub #66 (second part), an idle project keeps the worker threads asleep and the
// browser's index stays gentle:
//  * the parallel gate (ParallelGate): one long block (a preempted thread) never shares the graph
//    out, two blocks in a row over the line do, so does one block close to the deadline with several
//    heavy tasks; it closes only after a run of calm blocks, and a load near the line never flaps;
//  * the demo song stopped on four threads: after its effects' tails, built-in effects rest (the
//    reverbs too), the graph runs on the audio thread alone and no worker thread wakes; Play wakes
//    the effects in its first block; a busy graph (offline) still shares its blocks out;
//  * the index: a hold keeps it from starting and from analysing; its pace spaces the files out;
//    stop() ends a resting pass at once; a pass cut short keeps what it analysed and the next one
//    goes on from there; the worker runs as a background job.
// The gate is fed numbers; the engine counts blocks and wakeups; only the index's pace measures time
// (lower bounds, which a busy machine cannot break).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "core/BrowserIndex.h"
#include "core/Demo.h"
#include "core/Song.h"
#include "core/WavWriter.h"
#include "engine/AudioEngine.h"
#include "engine/ParallelGate.h"
#include "engine/Program.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

constexpr int kSr = 44100;
constexpr int kHost = 256;

void gateLogic() {
  const double block = 1e9 * 64 / kSr;  // a 64-frame block
  const auto ns = [&](double share) { return static_cast<uint64_t>(share * block); };
  ParallelGate g;
  bool opened = false;
  for (int i = 0; i < 2000; ++i) opened = g.update(ns(i % 7 == 0 ? 3.0 : 0.15), i % 7 == 0 ? 1 : 0, block) || opened;  // a preempted block now and then (one stretched task)
  expect(!opened, "gate: single long blocks among light ones never share the graph out");
  expect(g.update(ns(0.35), 3, block) == false && g.update(ns(0.35), 3, block), "gate: two blocks in a row over the line open it");
  int open = 0;
  for (int i = 0; i < 63; ++i) open += g.update(ns(0.1), 0, block);
  expect(open == 63, "gate: it stays open through 63 calm blocks");
  expect(!g.update(ns(0.1), 0, block), "gate: ... and closes on the 64th");
  ParallelGate u;
  expect(u.update(ns(0.9), 2, block), "gate: one block near the deadline with two heavy tasks opens it at once");
  ParallelGate v;
  expect(!v.update(ns(0.9), 1, block), "gate: ... but not with one heavy task (one stretched task)");
  ParallelGate f;  // a load near the line: open once, then steady
  int flips = 0;
  bool was = false;
  for (int i = 0; i < 4000; ++i) {
    const bool now = f.update(ns(i < 2 ? 0.35 : i % 2 ? 0.33 : 0.18), 3, block);
    flips += now != was;
    was = now;
  }
  expect(flips == 1, std::string("gate: a load around the line opens once and never flaps (" + std::to_string(flips) + " changes)").c_str());
}

struct Idle {
  uint64_t parallel = 0, serial = 0, wakeups = 0, hallRest = 0, hallRestAfterPlay = 0;
  int halls = 0;
};

// The demo stopped on four threads: 6 s for its tails and rests, then 3 s counted; then Play.
Idle demoStopped(bool rest) {
  Song s = makeDemoTrack();
  ProgramBuilder b;
  b.setSampleRate(kSr);
  AudioEngine e(kSr);
  e.setThreads(4);
  e.setRestInstruments(rest);
  e.setProgram(b.build(s));
  e.setMode(PlayMode::Song);
  e.setLooping(false);
  std::vector<float> L(kHost), R(kHost);
  for (int at = 0; at < 6 * kSr; at += kHost) e.process(L.data(), R.data(), kHost);
  Idle r;
  std::vector<std::shared_ptr<EffectRuntime>> halls;
  for (const MixerTrack& m : s.mixer)
    for (size_t k = 0; k < m.slots.size(); ++k)
      if (m.slots[k].plugin.type == "gl.hall" || m.slots[k].plugin.type == "gl.reverb")
        if (auto rt = b.effectRuntime(m.id, static_cast<int>(k))) halls.push_back(rt);
  r.halls = static_cast<int>(halls.size());
  const uint64_t p0 = e.parallelBlocks(), s0 = e.serialBlocks(), w0 = e.workerWakeups();
  uint64_t h0 = 0;
  for (auto& h : halls) h0 += h->restBlocks.load();
  for (int at = 0; at < 3 * kSr; at += kHost) e.process(L.data(), R.data(), kHost);
  r.parallel = e.parallelBlocks() - p0;
  r.serial = e.serialBlocks() - s0;
  r.wakeups = e.workerWakeups() - w0;
  uint64_t h1 = 0;
  for (auto& h : halls) h1 += h->restBlocks.load();
  r.hallRest = h1 - h0;
  e.play();
  for (int at = 0; at < kSr / 2; at += kHost) e.process(L.data(), R.data(), kHost);
  uint64_t h2 = 0;
  for (auto& h : halls) h2 += h->restBlocks.load();
  r.hallRestAfterPlay = h2 - h1;
  return r;
}

void idleGraph() {
  const Idle on = demoStopped(true);
  std::printf("demo stopped, 4 threads, 3 s: %llu blocks on one thread, %llu shared out, %llu worker wakeups, %d reverbs resting %llu blocks\n",
              static_cast<unsigned long long>(on.serial), static_cast<unsigned long long>(on.parallel),
              static_cast<unsigned long long>(on.wakeups), on.halls, static_cast<unsigned long long>(on.hallRest));
  expect(on.halls > 0, "the demo has reverbs on its mixer tracks");
  expect(on.hallRest > 0, "stopped and fed silence, the demo's reverbs rest");
  expect(on.hallRestAfterPlay == 0, "Play wakes them in its first block");
  // (A busy machine can stretch a block; the gate needs two in a row, so a stray one is rare: 2%.
  // A shared CI machine, a virtual one above all, can stall the audio thread over two blocks: the
  // measured work then takes in the time the machine gave to others, and each such opening holds
  // the gate open for its calm blocks. There a few stray openings are allowed, never more.)
  const uint64_t stray = std::getenv("CI") ? 3 * (ParallelGate::kCalmBlocks + 2) : 0;
  expect(on.serial > 0 && (on.parallel - std::min(on.parallel, stray)) * 50 <= on.serial, "an idle graph runs on the audio thread alone");
  expect(on.wakeups <= on.parallel * 3, std::string("... and wakes no worker thread while it does (" + std::to_string(on.wakeups) + ")").c_str());
  const Idle off = demoStopped(false);
  expect(off.hallRest == 0, "with Rest silent instruments off the reverbs keep running");
  // Offline renders share every block out as before.
  Song s = makeDemoTrack();
  ProgramBuilder b;
  b.setSampleRate(kSr);
  AudioEngine e(kSr);
  e.setOffline(true);
  e.setThreads(4);
  e.setProgram(b.build(s));
  e.setMode(PlayMode::Song);
  e.play();
  std::vector<float> L(kHost), R(kHost);
  for (int at = 0; at < kSr; at += kHost) e.process(L.data(), R.data(), kHost);
  expect(e.parallelBlocks() > 0 && e.serialBlocks() == 0 && e.workerWakeups() > 0, "an offline render still uses every thread");
}

void writeTone(const std::string& path, double seconds) {
  AudioBuffer buf(1, static_cast<size_t>(seconds * kSr));
  for (size_t i = 0; i < buf.frames; ++i) buf.plane(0)[i] = 0.3f * static_cast<float>(std::sin(0.05 * static_cast<double>(i)));
  writeWav(path, buf, kSr);
}

double since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

void indexPace() {
  const std::string base = gl::test::hermeticTestEnv("k078_idlecpu2");
  const std::string root = base + "/lib";
  fs::create_directories(root);
  constexpr int kFiles = 24;
  for (int i = 0; i < kFiles; ++i) writeTone(root + "/tone " + std::to_string(i) + ".wav", 0.05);
  using browse::Index;
  {  // a hold: nothing is walked or analysed until it ends
    Index ix;
    ix.setCacheFile(base + "/held.json");
    ix.setRoots({{root, true}});
    ix.holdFor(0.6);
    const auto t0 = std::chrono::steady_clock::now();
    ix.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    expect(ix.size() == 0 && ix.done() == 0, "index: held, it does not start");
    ix.waitIdle();
    expect(since(t0) >= 0.55 && ix.done() == static_cast<size_t>(kFiles), "index: it starts when the hold ends and finishes");
    expect(ix.rests() > 0, "index: the hold was waited out");
#if defined(__APPLE__) || defined(__linux__) || defined(_WIN32)
    expect(ix.lowPriority(), "index: the worker runs as a background job");
#endif
  }
  {  // no pace: flat out, never resting
    Index ix;
    ix.setCacheFile(base + "/fast.json");
    ix.setRoots({{root, true}});
    ix.start();
    ix.waitIdle();
    expect(ix.done() == static_cast<size_t>(kFiles) && ix.rests() == 0, "index: without a pace it never rests");
  }
  {  // a pace of 20 files a second: 24 files take more than a second
    Index ix;
    ix.setCacheFile(base + "/paced.json");
    ix.setRoots({{root, true}});
    Index::Pace p;
    p.filesPerSec = 20;
    p.share = 0.5;
    ix.setPace(p);
    const auto t0 = std::chrono::steady_clock::now();
    ix.start();
    ix.waitIdle();
    const double took = since(t0);
    std::printf("index: %d files at 20 a second took %.2f s (%llu rests)\n", kFiles, took, static_cast<unsigned long long>(ix.rests()));
    expect(ix.done() == static_cast<size_t>(kFiles) && took >= (kFiles - 1) / 20.0 - 0.05, "index: the pace spaces the files out");
  }
  {  // cut short while resting: stop() is quick; the next pass goes on from what was analysed
    const std::string cache = base + "/resume.json";
    size_t first = 0;
    {
      Index ix;
      ix.setCacheFile(cache);
      ix.setRoots({{root, true}});
      Index::Pace p;
      p.filesPerSec = 10;
      ix.setPace(p);
      ix.start();
      std::this_thread::sleep_for(std::chrono::milliseconds(700));
      const auto t0 = std::chrono::steady_clock::now();
      ix.stop();
      const double took = since(t0);
      first = ix.done();
      std::printf("index: stopped after %zu files in %.3f s\n", first, took);
      expect(took < 1.0, "index: stop() ends a resting pass at once");
      expect(first > 0 && first < static_cast<size_t>(kFiles), "index: the pass was cut short part way");
    }
    Index again;
    again.setCacheFile(cache);
    again.load();
    again.setRoots({{root, true}});
    again.start();
    again.waitIdle();
    expect(again.done() == static_cast<size_t>(kFiles) - first, std::string("index: the next pass analyses only what was left (" + std::to_string(again.done()) + ")").c_str());
    browse::FileMeta m;
    expect(again.lookup(root + "/tone 0.wav", m) && m.analysed, "index: and has every file analysed");
  }
}

}  // namespace

int main() {
  gl::test::hermeticTestEnv("k078_idlecpu2");
  gateLogic();
  idleGraph();
  indexPace();
  return finish();
}
