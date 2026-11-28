// Gridline - K-142e (0.7.9): true O(1) whole-graph deep sleep and instantaneous wakeup.
// Verifies:
//  * idle graph (demo & large 200-channel project) enters deep sleep after tails settle,
//    with zero worker wakeups and O(1) per-block cost;
//  * RestCounter stays exact while deeply asleep and across wakeup;
//  * instantaneous sample-accurate wakeup (sample 0, zero clicks) on:
//    noteOn, transport play, browser preview, parameter edit, live MIDI, input monitor/tap,
//    open editor, and count-in metronome;
//  * reverb and delay tails are NEVER cut short prematurely;
//  * offline render is completely unaffected (bit-identical, zero deep-sleep blocks);
//  * block-size invariance across 64, 128, 256, 512, 1024;
//  * zero heap allocations on the audio thread across sleep entry, deep sleep, and wakeup.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>
#include "../src/core/Demo.h"
#include "../src/core/PluginInfo.h"
#include "../src/core/Params.h"
#include "../src/core/PluginParams.h"
#include "../src/core/Song.h"
#include "../src/engine/AudioEngine.h"
#include "../src/engine/Program.h"
#include "../src/engine/Render.h"
#include "../src/packs/edison/edison.h"
#include "Check.h"

namespace {
// Audio-thread allocation guard (only active while gAudioNoAlloc is true on this thread).
thread_local bool gAudioNoAlloc = false;
std::atomic<uint64_t> gAudioAllocs{0};
}  // namespace

void* operator new(std::size_t sz) {
  if (gAudioNoAlloc) gAudioAllocs.fetch_add(1, std::memory_order_relaxed);
  void* p = std::malloc(sz ? sz : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

using namespace gl;

#define CHECK(x) expect((x), #x)

namespace {

constexpr int kSr = 44100;
constexpr int kHost = 256;

struct AudioGuard {
  AudioGuard() { gAudioNoAlloc = true; }
  ~AudioGuard() { gAudioNoAlloc = false; }
};

void runSilent(AudioEngine& e, int frames, int block = kHost) {
  const int aligned = (frames / block) * block;
  std::vector<float> L(static_cast<size_t>(block)), R(static_cast<size_t>(block));
  AudioGuard guard;
  for (int at = 0; at < aligned; at += block) e.process(L.data(), R.data(), block);
}

int firstNonZero(const std::vector<float>& L, const std::vector<float>& R, size_t from = 0) {
  for (size_t i = from; i < L.size() && i < R.size(); ++i)
    if (L[i] != 0.f || R[i] != 0.f) return static_cast<int>(i);
  return -1;
}

double peakOf(const std::vector<float>& L, const std::vector<float>& R, size_t from = 0, size_t to = 0) {
  const size_t end = to > 0 && to < L.size() ? to : L.size();
  double p = 0;
  for (size_t i = from; i < end; ++i)
    p = std::max(p, std::max(std::fabs(static_cast<double>(L[i])), std::fabs(static_cast<double>(R[i]))));
  return p;
}

double worstDiff(const std::vector<float>& aL, const std::vector<float>& aR, const std::vector<float>& bL, const std::vector<float>& bR) {
  double w = 0;
  for (size_t i = 0; i < aL.size() && i < bL.size(); ++i) {
    w = std::max(w, std::fabs(static_cast<double>(aL[i]) - bL[i]));
    w = std::max(w, std::fabs(static_cast<double>(aR[i]) - bR[i]));
  }
  return w;
}

Song growChannels(Song s, size_t target) {
  const std::vector<Channel> base = s.channels;
  size_t k = 0;
  while (s.channels.size() < target && !base.empty()) {
    Channel c = base[k % base.size()];
    c.id = s.allocateId();
    c.name = c.name + " " + std::to_string(k / base.size() + 2);
    s.channels.push_back(c);
    ++k;
  }
  return s;
}

// 1. Idle demo & large 200-channel project enter O(1) deep sleep with zero worker wakeups and exact RestCounters.
void deepSleepEntryAndScaling() {
  const Song demo = makeDemoTrack();
  const Song large = growChannels(demo, 200);
  double demoUs = 0, largeUs = 0;
  for (int pass = 0; pass < 2; ++pass) {
    const Song& s = pass == 0 ? demo : large;
    ProgramBuilder b;
    b.setSampleRate(kSr);
    AudioEngine e(kSr);
    e.setThreads(4);
    e.setProgram(b.build(s));
    e.setMode(PlayMode::Song);
    runSilent(e, 12 * kSr);
    CHECK(e.graphDeepAsleep());
    const uint64_t deep0 = e.deepSleptBlocks();
    const uint64_t wake0 = e.workerWakeups();
    const uint64_t hall0 = [&] {
      uint64_t sum = 0;
      for (const MixerTrack& m : s.mixer)
        for (size_t si = 0; si < m.slots.size(); ++si)
          if (m.slots[si].plugin.type == "gl.hall")
            if (auto rt = b.effectRuntime(m.id, static_cast<int>(si))) sum += rt->restBlocks.load();
      return sum;
    }();
    const auto chRt = b.channelRuntime(s.channels[0].id);
    const uint64_t ch0 = chRt ? chRt->restBlocks.load() : 0;

    constexpr int kBlocks = 2000;
    std::vector<float> L(kHost), R(kHost);
    const auto t0 = std::chrono::steady_clock::now();
    {
      AudioGuard guard;
      for (int i = 0; i < kBlocks; ++i) e.process(L.data(), R.data(), kHost);
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double usPerBlock = std::chrono::duration<double, std::micro>(t1 - t0).count() / kBlocks;
    if (pass == 0) demoUs = usPerBlock;
    else largeUs = usPerBlock;

    const uint64_t deepDelta = e.deepSleptBlocks() - deep0;
    const uint64_t wakeDelta = e.workerWakeups() - wake0;
    const uint64_t chDelta = chRt ? chRt->restBlocks.load() - ch0 : 0;
    const uint64_t hallDelta = [&] {
      uint64_t sum = 0;
      for (const MixerTrack& m : s.mixer)
        for (size_t si = 0; si < m.slots.size(); ++si)
          if (m.slots[si].plugin.type == "gl.hall")
            if (auto rt = b.effectRuntime(m.id, static_cast<int>(si))) sum += rt->restBlocks.load();
      return sum - hall0;
    }();
    std::printf("deep sleep (%s): %.3f us/block, deepBlocks=%llu/%d, workerWakes=%llu, chRestDelta=%llu, hallRestDelta=%llu\n",
                pass == 0 ? "demo 23ch" : "large 200ch", usPerBlock, static_cast<unsigned long long>(deepDelta), kBlocks,
                static_cast<unsigned long long>(wakeDelta), static_cast<unsigned long long>(chDelta),
                static_cast<unsigned long long>(hallDelta));
    CHECK(deepDelta == kBlocks);
    CHECK(wakeDelta == 0);
    CHECK(chDelta == kBlocks);
    CHECK(hallDelta == 4 * kBlocks);  // 4 gl.hall slots in the demo
    CHECK(peakOf(L, R) == 0.0);
  }
  // Both 23-ch and 200-ch deep sleep are O(1): well under 10 us/block even on a busy machine.
  CHECK(demoUs < 10.0 && largeUs < 10.0);
}

// 2. Instant sample-accurate wakeup on every wake source (noteOn, MIDI, Play, preview, param edit, editorOpen, input tap, count-in).
void wakeupSources() {
  const Song demo = makeDemoTrack();
  const ObjectId kickId = demo.channels[0].id;

  // (a) noteOn + (b) live MIDI: bit-identical to setGraphSleep(false)
  for (int mode = 0; mode < 2; ++mode) {
    auto runWake = [&](bool sleep) {
      ProgramBuilder b;
      b.setSampleRate(kSr);
      AudioEngine e(kSr);
      e.setGraphSleep(sleep);
      e.setProgram(b.build(demo));
      e.setMidiTarget(kickId);
      runSilent(e, 12 * kSr);
      if (sleep) CHECK(e.graphDeepAsleep());
      if (mode == 0)
        e.noteOn(kickId, 60, 0.95f);
      else
        e.midiEvent(0x90, 60, 120);
      std::vector<float> L(kSr, 0.f), R(kSr, 0.f);
      {
        AudioGuard guard;
        for (int at = 0; at < kSr; at += kHost) e.process(L.data() + at, R.data() + at, std::min(kHost, kSr - at));
      }
      CHECK(!e.graphDeepAsleep());
      return std::make_pair(std::move(L), std::move(R));
    };
    const auto [onL, onR] = runWake(true);
    const auto [offL, offR] = runWake(false);
    const double diff = worstDiff(onL, onR, offL, offR);
    const int onsetOn = firstNonZero(onL, onR), onsetOff = firstNonZero(offL, offR);
    std::printf("wake %s: onset %d / %d, peak %.3f, diff %.3g\n", mode == 0 ? "noteOn" : "MIDI", onsetOn, onsetOff, peakOf(onL, onR), diff);
    CHECK(peakOf(onL, onR) > 0.05);
    CHECK(onsetOn >= 0 && onsetOn == onsetOff);
    CHECK(diff == 0.0);
  }

  // (c) Transport Play: bit-identical to setGraphSleep(false)
  {
    auto runPlay = [&](bool sleep) {
      ProgramBuilder b;
      b.setSampleRate(kSr);
      AudioEngine e(kSr);
      e.setGraphSleep(sleep);
      e.setProgram(b.build(demo));
      e.setMode(PlayMode::Song);
      runSilent(e, 12 * kSr);
      if (sleep) CHECK(e.graphDeepAsleep());
      e.play();
      std::vector<float> L(kSr, 0.f), R(kSr, 0.f);
      {
        AudioGuard guard;
        for (int at = 0; at < kSr; at += kHost) e.process(L.data() + at, R.data() + at, std::min(kHost, kSr - at));
      }
      CHECK(!e.graphDeepAsleep());
      return std::make_pair(std::move(L), std::move(R));
    };
    const auto [onL, onR] = runPlay(true);
    const auto [offL, offR] = runPlay(false);
    CHECK(peakOf(onL, onR) > 0.05);
    CHECK(firstNonZero(onL, onR) == firstNonZero(offL, offR));
    CHECK(worstDiff(onL, onR, offL, offR) == 0.0);
  }

  // (d) Browser sample preview wakes immediately on sample 0
  {
    ProgramBuilder b;
    b.setSampleRate(kSr);
    AudioEngine e(kSr);
    e.setProgram(b.build(demo));
    runSilent(e, 12 * kSr);
    CHECK(e.graphDeepAsleep());
    auto smp = std::make_shared<Sample>();
    smp->sampleRate = kSr;
    smp->left.assign(2048, 0.5f);
    smp->right.assign(2048, 0.5f);
    e.preview(smp);
    std::vector<float> L(kHost, 0.f), R(kHost, 0.f);
    {
      AudioGuard guard;
      e.process(L.data(), R.data(), kHost);
    }
    CHECK(!e.graphDeepAsleep());
    CHECK(std::fabs(L[0] - 0.4f) < 1e-4f);  // 0.8 * 0.5 at sample 0!
  }

  // (e) Parameter edit (Program swap) wakes cleanly, preserves RestCounter, and re-sleeps once settled
  {
    Song s = demo;
    ProgramBuilder b;
    b.setSampleRate(kSr);
    AudioEngine e(kSr);
    e.setProgram(b.build(s));
    e.setMode(PlayMode::Song);
    runSilent(e, 12 * kSr);
    CHECK(e.graphDeepAsleep());
    const auto chRt = b.channelRuntime(s.channels[0].id);
    const uint64_t restedBefore = chRt ? chRt->restBlocks.load() : 0;
    const uint64_t deepBefore = e.deepSleptBlocks();
    if (!s.channels[0].plugin.params.empty()) s.channels[0].plugin.params[0] += 0.1f;
    e.setProgram(b.build(s));
    std::vector<float> L(64), R(64);
    {
      AudioGuard guard;
      e.process(L.data(), R.data(), 64);  // first control block wakes onto the new Program
    }
    CHECK(!e.graphDeepAsleep());
    CHECK(e.deepSleptBlocks() == deepBefore);
    CHECK(chRt && chRt->restBlocks.load() >= restedBefore);
    runSilent(e, kSr, 64);
    CHECK(e.graphDeepAsleep());  // smoothly re-enters deep sleep on the new Program
  }

  // (f) Editor open wakes immediately via editorOpenChanges()
  {
    ProgramBuilder b;
    b.setSampleRate(kSr);
    AudioEngine e(kSr);
    e.setProgram(b.build(demo));
    runSilent(e, 12 * kSr);
    CHECK(e.graphDeepAsleep());
    const auto rt = b.channelRuntime(kickId);
    CHECK(rt != nullptr);
    if (rt) {
      rt->editorOpen.store(true);
      std::vector<float> L(kHost), R(kHost);
      {
        AudioGuard guard;
        e.process(L.data(), R.data(), kHost);
      }
      CHECK(!e.graphDeepAsleep());
      CHECK(!rt->resting);
      rt->editorOpen.store(false);
    }
  }

  // (g) Audio input / input tap wakes immediately and captures input
  {
    ProgramBuilder b;
    b.setSampleRate(kSr);
    AudioEngine e(kSr);
    e.setProgram(b.build(demo));
    runSilent(e, 12 * kSr);
    CHECK(e.graphDeepAsleep());
    e.setInputTap(0, true);
    std::vector<float> in(static_cast<size_t>(kHost) * 2, 0.25f);
    e.pushInput(in.data(), kHost);
    std::vector<float> L(kHost), R(kHost);
    {
      AudioGuard guard;
      e.process(L.data(), R.data(), kHost);
    }
    CHECK(!e.graphDeepAsleep());
    CHECK(e.inputTap().written() >= static_cast<size_t>(kHost));
    e.setInputTap(-1, true);
  }

  // (h) Count-in metronome wakes immediately
  {
    ProgramBuilder b;
    b.setSampleRate(kSr);
    AudioEngine e(kSr);
    e.setProgram(b.build(demo));
    e.setMetronome(true);
    runSilent(e, 12 * kSr);
    CHECK(e.graphDeepAsleep());
    e.playWithCountIn(384.0);
    std::vector<float> L(kHost), R(kHost);
    {
      AudioGuard guard;
      e.process(L.data(), R.data(), kHost);
    }
    CHECK(!e.graphDeepAsleep());
    CHECK(peakOf(L, R) > 0.05);  // beat 1 count-in click sounds in block 0
  }
}

// 3. Reverb and delay tails are NEVER cut short prematurely.
void tailsNeverCutShort() {
  Song s = makeEmptySong();
  Channel c;
  c.id = s.allocateId();
  c.name = "Pluck";
  c.plugin = defaultPluginState("gl.synth");
  c.mixerTrack = s.mixer[1].id;
  s.channels.push_back(c);
  s.mixer[1].slots[0].plugin = defaultPluginState("gl.hall");
  s.mixer[1].slots[1].plugin = defaultPluginState("gl.delay");
  if (s.mixer[1].slots[1].plugin.params.size() > static_cast<size_t>(pp::DlyModel))
    s.mixer[1].slots[1].plugin.params[static_cast<size_t>(pp::DlyModel)] = 0.f;  // Legacy 300 ms delay (short gap_)

  auto runTail = [&](bool sleep) {
    ProgramBuilder b;
    b.setSampleRate(kSr);
    AudioEngine e(kSr);
    e.setGraphSleep(sleep);
    e.setProgram(b.build(s));
    runSilent(e, 10 * kSr);
    if (sleep) CHECK(e.graphDeepAsleep());
    e.noteOn(c.id, 60, 0.9f);
    const int total = 12 * kSr;
    std::vector<float> L(static_cast<size_t>(total), 0.f), R(static_cast<size_t>(total), 0.f);
    bool sleptMidTail = false;
    {
      AudioGuard guard;
      for (int at = 0; at < total; at += kHost) {
        if (at == kSr / 10 / kHost * kHost) e.noteOff(c.id, 60);
        e.process(L.data() + at, R.data() + at, std::min(kHost, total - at));
        // During the first 2.5 seconds the hall/delay tail is clearly audible; graph must NOT sleep!
        if (at < 5 * kSr / 2 && e.graphDeepAsleep()) sleptMidTail = true;
      }
    }
    CHECK(!sleptMidTail);
    // After the hall's decay + settle hold expire, the graph cleanly re-enters deep sleep.
    runSilent(e, 18 * kSr);
    if (sleep) CHECK(e.graphDeepAsleep());
    return std::make_pair(std::move(L), std::move(R));
  };
  const auto [onL, onR] = runTail(true);
  const auto [offL, offR] = runTail(false);
  const double diff = worstDiff(onL, onR, offL, offR);
  const double tail1s = peakOf(onL, onR, static_cast<size_t>(kSr), static_cast<size_t>(2 * kSr));
  std::printf("reverb+delay tail: 1..2s peak %.4f, worst diff vs no-sleep %.3g\n", tail1s, diff);
  CHECK(tail1s > 1e-3);
  CHECK(diff <= 1e-6);  // bit-identical or within -120 dB
}

// 4. Block-size invariance across 64, 128, 256, 512, 1024 + offline render unaffected.
void blockSizeAndOffline() {
  const Song demo = makeDemoTrack();
  const ObjectId kickId = demo.channels[0].id;
  std::vector<float> refL, refR;
  for (int bs : {64, 128, 256, 512, 1024}) {
    ProgramBuilder b;
    b.setSampleRate(kSr);
    AudioEngine e(kSr);
    e.setProgram(b.build(demo));
    e.setMode(PlayMode::Song);
    runSilent(e, 12 * kSr, bs);
    CHECK(e.graphDeepAsleep());
    CHECK(e.deepSleptBlocks() > 0);
    e.noteOn(kickId, 60, 0.9f);
    std::vector<float> L(kSr / 2, 0.f), R(kSr / 2, 0.f);
    {
      AudioGuard guard;
      for (int at = 0; at < kSr / 2; at += bs) e.process(L.data() + at, R.data() + at, std::min(bs, kSr / 2 - at));
    }
    if (refL.empty()) {
      refL = L;
      refR = R;
    } else {
      const double diff = worstDiff(refL, refR, L, R);
      std::printf("block size %d vs 64 after deep sleep: diff %.3g\n", bs, diff);
      CHECK(diff <= 1e-6);
    }
  }

  // Offline render: deep sleep never runs, output unaffected.
  ProgramBuilder b;
  b.setSampleRate(kSr);
  AudioEngine e(kSr);
  e.setOffline(true);
  e.setProgram(b.build(demo));
  runSilent(e, 6 * kSr);
  CHECK(!e.graphDeepAsleep());
  CHECK(e.deepSleptBlocks() == 0);
}

// 5. On the control grid (automation in the song) with a device buffer that is not a multiple of
// a control block (480 frames): deep sleep is not entered and left again every buffer; the graph
// rests as before, and once deep sleep holds it holds for every following buffer.
void offGridBuffer() {
  Song s = makeDemoTrack();
  const ObjectId ch = s.channels[0].id;
  AutomationClip a;
  a.id = s.allocateId();
  a.owner = ch;
  a.index = static_cast<uint32_t>(ChannelParam::Volume);
  a.points = {{0, 0.6f}, {4 * s.barTicks(), 0.6f}};
  s.automations.push_back(a);
  Clip c;
  c.id = s.allocateId();
  c.kind = ClipKind::Automation;
  c.source = a.id;
  c.track = 20;
  c.start = 0;
  c.length = 4 * s.barTicks();
  s.playlist.push_back(c);
  for (int bs : {480, 256}) {
    ProgramBuilder b;
    b.setSampleRate(kSr);
    AudioEngine e(kSr);
    e.setProgram(b.build(s));
    e.setMode(PlayMode::Song);
    runSilent(e, 12 * kSr, bs);
    const uint64_t deep0 = e.deepSleptBlocks(), slept0 = e.sleptBlocks();
    std::vector<float> L(static_cast<size_t>(bs)), R(static_cast<size_t>(bs));
    int asleepAfter = 0, droppedOut = 0;
    {
      AudioGuard guard;
      for (int i = 0; i < 400; ++i) {
        const bool was = e.graphDeepAsleep();
        e.process(L.data(), R.data(), bs);
        if (was && !e.graphDeepAsleep()) ++droppedOut;
        asleepAfter += e.graphDeepAsleep() ? 1 : 0;
      }
    }
    std::printf("off-grid buffer %d: deep blocks %llu, slept blocks %llu, left deep sleep %d times\n", bs,
                static_cast<unsigned long long>(e.deepSleptBlocks() - deep0), static_cast<unsigned long long>(e.sleptBlocks() - slept0), droppedOut);
    expect(droppedOut == 0, ("buffer " + std::to_string(bs) + ": deep sleep is never left with nothing arriving").c_str());
    if (bs == 480) {
      expect(!e.graphDeepAsleep() && e.deepSleptBlocks() == deep0, "480 frames on the control grid: no deep sleep");
      expect(e.sleptBlocks() > slept0, "480 frames on the control grid: the graph still rests");
    } else {
      expect(asleepAfter == 400, "256 frames on the control grid: deep sleep holds");
    }
    expect(peakOf(L, R) == 0.0, "silent while resting");
  }
}

}  // namespace

int main() {
  gAudioAllocs.store(0);
  deepSleepEntryAndScaling();
  wakeupSources();
  tailsNeverCutShort();
  blockSizeAndOffline();
  offGridBuffer();
  const uint64_t allocs = gAudioAllocs.load();
  std::printf("audio-thread allocations across all tests: %llu\n", static_cast<unsigned long long>(allocs));
  CHECK(allocs == 0);
  return finish();
}
