// Gridline — "seltmann" pack tests (0.7.8, GitHub #80): what a sounding voice reads stays in memory.
//   render    a key held on a stretched region while another file and layout come in: the data
//             before is freed as the engine does, its renders leave use long before the note
//             ends; the note plays on cleanly to its end, and then everything is freed.
//   source    the same with Follow: Resample (the voice reads the file itself) while another file
//             is loaded: the first file stays in memory until the note ends.
//   stress    an audio thread renders without pause while another thread switches through every
//             layout, both follow modes, two grids and two files, freeing each prepared data two
//             blocks after the audio thread moved on (as the engine does). Keys and the pattern
//             play throughout, the tempo moves. Run under AddressSanitizer this is the crash of
//             GitHub #80 when a voice outlives what it reads.
//   drag      30 s of audio (run at twice real time) while the tempo is dragged and jumps at random,
//             keys and the pattern playing: the worker makes renders while the tempo moves, and the
//             renders it holds stay within the memory cap (shortened here, so that the cap is met
//             many times; the long grace period of a render that left use as the app has it), with
//             a render's room over it at most for the renders of a tempo that holds still; once
//             the tempo holds still again the cap is kept; every render is freed at the end. The
//             renders made while the tempo moves are rate limited (the worker stretched over 80%
//             of the time here before, about 50% with the limit; asserted under 70%).
//             DRAG_DEFAULT_CAPS=1 runs it with the app's own caps (to measure).
// The worker's linger and grace times are shortened to milliseconds so the old data really goes.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include "../Check.h"
#include "core/PluginInfo.h"
#include "core/Song.h"
#include "engine/Generators.h"
#include "engine/Sample.h"
#include "packs/Packs.h"
#include "packs/seltmann/seltmann.h"
#include "packs/seltmann/seltmann_editor.h"

using namespace gl;
namespace sm = gl::packs::seltmann;
namespace fs = std::filesystem;

namespace {

constexpr double kRate = 44100, kSrcBpm = 100, kTempo = 130;
constexpr int kBlock = 512;

fs::path tmpDir() {
  const fs::path d = fs::temp_directory_path() / "gl_seltmann_lifetime_test";
  std::error_code ec;
  fs::create_directories(d, ec);
  return d;
}

// A sine of `f0` Hz, 16 bars at 100 bpm, written to a file and decoded through the cache.
SamplePtr sineFile(const std::string& name, double f0, std::string& path) {
  std::vector<float> L(static_cast<size_t>(kRate * 60.0 / kSrcBpm * 4 * 16));
  for (size_t i = 0; i < L.size(); ++i) L[i] = static_cast<float>(0.5 * std::sin(2 * 3.14159265358979 * f0 * static_cast<double>(i) / kRate));
  path = (tmpDir() / name).string();
  if (!sm::writeLoopWav(path, L, L, kRate)) return nullptr;
  return sampleCache().get(path);
}

// Four one-bar regions from bar 1, played in order, on a flat grid at `bpm`.
sm::Model baseModel(const Sample& src, double bpm = kSrcBpm) {
  sm::Model m;
  m.srcFrames = static_cast<int64_t>(src.frames());
  m.srcRate = kRate;
  m.grid = sm::flatGrid(src, bpm);
  m.chop.mode = sm::ChopGrid;
  m.chop.division = sm::Div1Bar;
  m.chop.zeroCross = false;
  sm::setRegionBars(m, 0, 4);
  sm::rechop(m, &src);
  sm::straightPattern(m);
  return m;
}

PluginState stateOf(const std::string& path, const sm::Model& m, int sync) {
  PluginState st = defaultPluginState(sm::kType);
  st.data = sm::formatData(path, m);
  st.params[sm::SmSync] = static_cast<float>(sync);
  st.params[sm::SmVelocity] = 0;
  return st;
}

// Median frequency of a window (zero-crossing periods).
double windowFreq(const std::vector<float>& x, size_t i0, size_t i1) {
  std::vector<double> per;
  double last = -1;
  for (size_t i = std::max<size_t>(1, i0); i < std::min(i1, x.size()); ++i)
    if (x[i - 1] < 0 && x[i] >= 0 && std::fabs(x[i]) + std::fabs(x[i - 1]) > 1e-3f) {
      const double t = static_cast<double>(i - 1) + x[i - 1] / static_cast<double>(x[i - 1] - x[i]);
      if (last >= 0) per.push_back(t - last);
      last = t;
    }
  if (per.size() < 3) return 0;
  std::nth_element(per.begin(), per.begin() + static_cast<long>(per.size() / 2), per.end());
  return kRate / per[per.size() / 2];
}

double rms(const std::vector<float>& x, size_t a, size_t b) {
  double s = 0;
  for (size_t i = a; i < b && i < x.size(); ++i) s += static_cast<double>(x[i]) * x[i];
  return std::sqrt(s / static_cast<double>(std::max<size_t>(1, b - a)));
}

// Waits (running the worker's clock) until the families of renders come down to `n`.
bool familiesDropTo(int n, int ms = 2000) {
  for (int i = 0; i < ms / 10; ++i) {
    if (sm::warpFamiliesForTest() <= n) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return sm::warpFamiliesForTest() <= n;
}

// One instrument run as the audio thread runs it: blocks paced in real time, the prepared data
// replaced on a change and the data before freed two blocks later (as the engine frees a program).
struct Player {
  std::unique_ptr<Generator> gen = createGenerator(sm::kType);
  std::unique_ptr<ChannelAux> cur, old;
  PluginState st;
  int oldBlocks = 0;
  bool trim = false;  // the app trims the sample cache after a program swap
  ProcessContext ctx;
  std::vector<float> out;
  explicit Player(const PluginState& s) : st(s) {
    cur = std::make_unique<ChannelAux>();
    packs::prepareAux(st, *cur);
    ctx.sampleRate = kRate, ctx.tempo = kTempo, ctx.ppq = 96;
  }
  void swap(const PluginState& s) {
    st = s;
    old = std::move(cur);
    cur = std::make_unique<ChannelAux>();
    packs::prepareAux(st, *cur);
    oldBlocks = 2;
  }
  void run(double seconds, bool keep = true) {
    std::vector<float> L(kBlock), R(kBlock);
    const int blocks = static_cast<int>(seconds * kRate / kBlock);
    for (int b = 0; b < blocks; ++b) {
      const auto t0 = std::chrono::steady_clock::now();
      std::fill(L.begin(), L.end(), 0.f);
      std::fill(R.begin(), R.end(), 0.f);
      gen->process(st, *cur, ctx, L.data(), R.data(), kBlock);
      if (keep) out.insert(out.end(), L.begin(), L.end());
      if (old && --oldBlocks <= 0) old.reset();
      if (trim && !old) sampleCache().trim();
      std::this_thread::sleep_until(t0 + std::chrono::microseconds(static_cast<int64_t>(kBlock / kRate * 1e6)));
    }
  }
};

// A held key on region A, a switch mid-note, then the note to its end: it keeps its pitch and level.
void heldNote(const char* label, const PluginState& s0, const PluginState& s1, double want, bool trim) {
  Player p(s0);
  p.trim = trim;
  p.run(2.0, false);  // the warp worker makes the render
  const size_t on = p.out.size();
  p.gen->noteOn(1, 60, 1.0f, 0);  // the base note: region A, held (one source bar: 1.85 s here)
  p.run(0.3);
  const size_t sw = p.out.size();
  p.swap(s1);
  p.run(1.2);  // the render (or the file) the note reads leaves use within ~0.2 s of the switch
  int bad = 0, windows = 0;
  double worstCents = 0, worstDb = 0;
  const double steady = rms(p.out, on + 2000, sw);
  const size_t win = static_cast<size_t>(kRate * 0.05);
  for (size_t i = sw; i + win <= p.out.size(); i += win) {
    const double f = windowFreq(p.out, i, i + win);
    const double c = f > 0 ? std::fabs(1200.0 * std::log2(f / want)) : 1e9;
    const double db = std::fabs(20 * std::log10(std::max(1e-9, rms(p.out, i, i + win)) / std::max(1e-9, steady)));
    ++windows;
    worstCents = std::max(worstCents, std::min(c, 9999.0));
    worstDb = std::max(worstDb, db);
    if (c > 25 || db > 1) ++bad;
  }
  bool finite = true;
  for (float x : p.out) finite = finite && std::isfinite(x) && std::fabs(x) < 4;
  std::printf("# %-32s %2d/%2d windows off, worst %.1f cents, %.2f dB\n", label, bad, windows, worstCents, worstDb);
  expect(steady > 0.05, (std::string(label) + ": the note sounds").c_str());
  expect(finite && bad == 0 && windows > 10, (std::string(label) + ": the held note plays on cleanly after the switch").c_str());
  p.gen->noteOff(1, 0);
  p.run(0.5, false);
  // everything the note held goes once it ended: only the current data's file is left
  expect(familiesDropTo(1), (std::string(label) + ": the renders before are freed once the note ended").c_str());
  p.cur.reset();
  p.gen.reset();
  expect(familiesDropTo(0), (std::string(label) + ": ... and all of them once nothing plays").c_str());
}

void testRender() {
  std::string pa, pb;
  SamplePtr a = sineFile("life_a.wav", 220, pa), b = sineFile("life_b.wav", 330, pb);
  expect(a && b, "render: the sines decode");
  if (!a || !b) return;
  // another file with another layout: the first file's renders all leave use
  const sm::Model m0 = baseModel(*a), m1 = baseModel(*b, 104);
  const PluginState s0 = stateOf(pa, m0, sm::SyncStretch), s1 = stateOf(pb, m1, sm::SyncStretch);
  a.reset();
  b.reset();
  heldNote("render: Stretch, new file", s0, s1, 220, true);
}

void testSource() {
  std::string pa, pb;
  SamplePtr a = sineFile("life_src_a.wav", 220, pa), b = sineFile("life_src_b.wav", 330, pb);
  expect(a && b, "source: the sines decode");
  if (!a || !b) return;
  const sm::Model ma = baseModel(*a), mb = baseModel(*b);
  const PluginState s0 = stateOf(pa, ma, sm::SyncResample), s1 = stateOf(pb, mb, sm::SyncResample);
  a.reset();
  b.reset();  // only the prepared data holds the files
  heldNote("source: Resample, new file", s0, s1, 220 * kTempo / kSrcBpm, true);
}

// ------------------------------------------------------------------ stress
struct Prog {
  PluginState st;
  ChannelAux aux;
};

void testStress() {
  std::string pa, pb;
  SamplePtr a = sineFile("life_st_a.wav", 220, pa), b = sineFile("life_st_b.wav", 330, pb);
  expect(a && b, "stress: the sines decode");
  if (!a || !b) return;
  std::vector<PluginState> states;
  for (int file = 0; file < 2; ++file) {
    const Sample& s = file ? *b : *a;
    const std::string& path = file ? pb : pa;
    for (double bpm : {kSrcBpm, 97.0}) {
      const sm::Model base = baseModel(s, bpm);
      for (int sync : {sm::SyncStretch, sm::SyncResample}) {
        states.push_back(stateOf(path, base, sync));
        for (const auto& pr : sm::factoryPresets()) states.push_back(stateOf(path, sm::previewModel(base, &s, pr, 1), sync));
      }
    }
  }
  a.reset();
  b.reset();
  std::printf("# stress: %zu prepared states\n", states.size());

  auto gen = createGenerator(sm::kType);
  std::atomic<Prog*> pub{nullptr};
  std::atomic<const Prog*> running{nullptr};
  std::atomic<uint64_t> blocks{0};
  std::atomic<bool> stop{false}, bad{false};
  auto first = std::make_unique<Prog>();
  first->st = states[0];
  packs::prepareAux(first->st, first->aux);
  pub.store(first.get());

  std::thread audio([&] {
    std::mt19937 rng(80);
    ProcessContext ctx;
    ctx.sampleRate = kRate, ctx.ppq = 96, ctx.playing = true;
    std::vector<float> L(kBlock), R(kBlock);
    uint32_t nextId = 10;
    std::vector<std::pair<uint32_t, int>> held;  // id, blocks left
    int64_t pos = 0;
    while (!stop.load()) {
      const auto t0 = std::chrono::steady_clock::now();
      const Prog* p = pub.load(std::memory_order_acquire);
      running.store(p);
      ctx.tempo = (pos / (kBlock * 200)) % 2 ? kTempo + 3 : kTempo;  // the tempo moves now and then
      ctx.tick = static_cast<double>(pos) / kRate * ctx.tempo / 60.0 * ctx.ppq;
      ctx.jumped = pos == 0;
      if (pos == 0) gen->noteOn(1, 48, 1.0f, 0);  // the pattern
      if (rng() % 6 == 0) {                        // keys: short and long notes
        const uint32_t id = nextId++;
        gen->noteOn(id, 60 + static_cast<int>(rng() % 6), 1.0f, 0);
        held.push_back({id, static_cast<int>(rng() % 6 == 0 ? 400 : 4 + rng() % 40)});
      }
      for (auto it = held.begin(); it != held.end();)
        if (--it->second <= 0) {
          gen->noteOff(it->first, 0);
          it = held.erase(it);
        } else {
          ++it;
        }
      std::fill(L.begin(), L.end(), 0.f);
      std::fill(R.begin(), R.end(), 0.f);
      gen->process(p->st, p->aux, ctx, L.data(), R.data(), kBlock);
      for (int i = 0; i < kBlock; ++i)
        if (!std::isfinite(L[i]) || std::fabs(L[i]) > 8) bad.store(true);
      pos += kBlock;
      blocks.fetch_add(1);
      // a little faster than real time: more blocks, the same worker timing
      std::this_thread::sleep_until(t0 + std::chrono::microseconds(static_cast<int64_t>(kBlock / kRate * 0.6e6)));
    }
    running.store(nullptr);
  });

  // the UI: a new program every 15 to 60 ms; the one before freed two blocks after the audio
  // thread moved on from it
  std::mt19937 rng(1);
  std::vector<std::pair<std::unique_ptr<Prog>, uint64_t>> retired;
  std::unique_ptr<Prog> live = std::move(first);
  int swaps = 0;
  // Six seconds, longer if needed for 60 switches (a busy shared machine prepares slower), at
  // most 40 seconds.
  const auto begin = std::chrono::steady_clock::now();
  const auto end = begin + std::chrono::seconds(6), cap = begin + std::chrono::seconds(40);
  for (auto now = begin; (now < end || swaps < 60) && now < cap; now = std::chrono::steady_clock::now()) {
    auto nx = std::make_unique<Prog>();
    nx->st = states[rng() % states.size()];
    packs::prepareAux(nx->st, nx->aux);
    pub.store(nx.get(), std::memory_order_release);
    retired.push_back({std::move(live), blocks.load() + 2});
    live = std::move(nx);
    ++swaps;
    for (int k = 0; k < 4; ++k) {
      std::this_thread::sleep_for(std::chrono::milliseconds(4 + rng() % 12));
      const uint64_t bnow = blocks.load();
      const Prog* run = running.load();
      const size_t before = retired.size();
      retired.erase(std::remove_if(retired.begin(), retired.end(), [&](const auto& r) { return bnow >= r.second && r.first.get() != run; }),
                    retired.end());
      if (retired.size() != before) sampleCache().trim();
    }
  }
  stop.store(true);
  audio.join();
  std::printf("# stress: %d switches, %llu blocks\n", swaps, static_cast<unsigned long long>(blocks.load()));
  expect(swaps >= 50, "stress: many switches");  // (fewer under a sanitizer: preparing is slower)
  expect(!bad.load(), "stress: the sound stays finite and in range");
  retired.clear();
  gen.reset();
  live.reset();
  expect(familiesDropTo(0, 4000), "stress: every render is freed at the end");
}

// ------------------------------------------------------------------ drag
void testDrag() {
  std::string pa;
  SamplePtr a = sineFile("life_drag.wav", 220, pa);
  expect(a != nullptr, "drag: the sine decodes");
  if (!a) return;
  const sm::Model m = baseModel(*a);
  // the largest render here: the 8-bar span at the slowest tempo
  constexpr double kSlowest = 70, kFastest = 180;
  const auto spanBytes = static_cast<int64_t>(4 * 8 * 60.0 / kSlowest * kRate * 2 * sizeof(float));
  const bool appCaps = std::getenv("DRAG_DEFAULT_CAPS") != nullptr;
  const int64_t cap = int64_t{48} << 20;
  if (!appCaps) sm::setWarpCapsForTest(cap, 2 * cap);
  sm::setWarpTimesForTest(120000, 60000);
  sm::resetWarpPeakForTest();
  const sm::WarpStats w0 = sm::warpStatsForTest();
  Player p(stateOf(pa, m, sm::SyncStretch));
  a.reset();
  p.run(2.0, false);  // the render at the first tempo
  p.ctx.playing = true;
  p.gen->noteOn(1, 48, 1.0f, 0);  // the pattern
  std::mt19937 rng(78);
  std::uniform_real_distribution<double> uni(0, 1);
  std::vector<float> L(kBlock), R(kBlock);
  bool finite = true;
  int64_t worstHeld = 0;
  uint32_t key = 10;
  std::vector<std::pair<uint32_t, int>> held;
  const int blocks = static_cast<int>(30.0 * kRate / kBlock);
  const auto t0 = std::chrono::steady_clock::now();
  for (int b = 0; b < blocks; ++b) {
    const double x = uni(rng);
    if (x < 0.02) p.ctx.tempo = kSlowest + (kFastest - kSlowest) * uni(rng);                              // a jump
    else if (x < 0.4) p.ctx.tempo = std::clamp(p.ctx.tempo + (uni(rng) - 0.45) * 6, kSlowest, kFastest);  // dragged
    p.ctx.tick += kBlock / kRate * p.ctx.tempo / 60.0 * p.ctx.ppq;
    if (rng() % 8 == 0) {
      held.push_back({key, static_cast<int>(4 + rng() % 60)});
      p.gen->noteOn(key++, 60 + static_cast<int>(rng() % 4), 1.0f, 0);
    }
    for (auto it = held.begin(); it != held.end();)
      if (--it->second <= 0) {
        p.gen->noteOff(it->first, 0);
        it = held.erase(it);
      } else {
        ++it;
      }
    std::fill(L.begin(), L.end(), 0.f);
    std::fill(R.begin(), R.end(), 0.f);
    p.gen->process(p.st, *p.cur, p.ctx, L.data(), R.data(), kBlock);
    for (int i = 0; i < kBlock; ++i) finite = finite && std::isfinite(L[i]) && std::fabs(L[i]) < 4;
    worstHeld = std::max(worstHeld, sm::warpStatsForTest().bytes);
    std::this_thread::sleep_until(t0 + std::chrono::microseconds(static_cast<int64_t>((b + 1) * kBlock / kRate * 0.5e6)));
  }
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  const sm::WarpStats w1 = sm::warpStatsForTest();
  // the tempo holds still: what was evicted goes after its short grace period
  p.run(3.0, false);
  const sm::WarpStats w2 = sm::warpStatsForTest();
  const double mb = 1 << 20;
  std::printf("# drag: %d renders (%d while the tempo moved, %d skipped for the cap, %d wakeups rate limited), the worker stretching %.1f s of %.1f s (%.0f%%)\n",
              w1.renders - w0.renders, w1.moving - w0.moving, w1.skipped - w0.skipped, w1.throttled - w0.throttled, w1.workSeconds - w0.workSeconds, secs,
              100 * (w1.workSeconds - w0.workSeconds) / std::max(1e-3, secs));
  std::printf("# drag: renders held at most %.1f MB (cap %s, a span render %.1f MB), %.1f MB after the tempo held still\n",
              static_cast<double>(std::max(worstHeld, w1.peakBytes)) / mb, appCaps ? "the app's" : "48 MB", static_cast<double>(spanBytes) / mb,
              static_cast<double>(w2.bytes) / mb);
  expect(finite, "drag: the sound stays finite and in range");
  expect(w1.moving - w0.moving >= 10, "drag: the worker makes renders while the tempo moves");
  // 0.7.8: rate limited, the worker never stretches back to back for a dragged tempo (before: over
  // 80% of the time here)
  expect(w1.throttled > w0.throttled, "drag: the renders made while the tempo moves are rate limited");
  expect(w1.workSeconds - w0.workSeconds <= 0.7 * secs, "drag: ... so the worker stretches well under all of the time");
  if (!appCaps) {
    expect(std::max(worstHeld, w1.peakBytes) <= cap + 2 * spanBytes, "drag: the renders held stay within the cap (and a render's room)");
    expect(w2.bytes <= cap + spanBytes, "drag: ... and within it once the tempo holds still");
  }
  for (auto& h : held) p.gen->noteOff(h.first, 0);
  p.gen->noteOff(1, 0);  // (the pattern)
  p.run(0.5, false);
  p.cur.reset();
  p.gen.reset();
  sm::setWarpTimesForTest(30, 30);
  expect(familiesDropTo(0, 4000), "drag: every render is freed at the end");
  sm::setWarpCapsForTest(int64_t{128} << 20, int64_t{512} << 20);
}

}  // namespace

int main(int argc, char** argv) {
  const std::string only = argc > 1 ? argv[1] : "";
  sm::setWarpTimesForTest(30, 30);
  if (only.empty() || only == "render") testRender();
  if (only.empty() || only == "source") testSource();
  if (only.empty() || only == "stress") testStress();
  if (only.empty() || only == "drag") testDrag();
  sm::waitAnalysisIdle();
  return finish();
}
