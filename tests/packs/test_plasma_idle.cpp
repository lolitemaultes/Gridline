// Gridline: Plasma instances that sit idle cost next to nothing.
//  * many complex patches (every factory preset with a full effect rack, 4x quality, a mono LFO
//    moving an effect) with no notes and the transport stopped: once their tails are done they
//    sleep, output exact zeros and do a small fraction of the awake work (thread CPU time, with a
//    wide margin), and report it (plasma::asleep);
//  * a note wakes an instance at once and it plays exactly what an instance that never slept
//    plays: the same samples, mono LFO phases included; on a slept tail, within -114 dB;
//  * a complex patch with modulated effects wakes at the same level;
//  * a parameter change wakes it; offline renders never sleep;
//  * a long delay tail is never cut: an echo that comes back after a quiet gap still plays;
//  * in the engine, a sleeping channel's exact silence lets the effects on its mixer track rest.
// "bench [count] [idle seconds] [0 complex | 1 default | 2 factory presets]" prints per-instance
// idle cost (no assertions), for before / after numbers.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <new>
#include <string>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#endif
#include "../../src/core/PluginInfo.h"
#include "../../src/core/Project.h"
#include "../../src/core/Song.h"
#include "../../src/engine/AudioEngine.h"
#include "../../src/engine/Program.h"
#include "../../src/engine/Denormals.h"
#include "../../src/engine/Generators.h"
#include "../../src/packs/Packs.h"
#include "../../src/packs/plasma/plasma.h"
#include "../Check.h"

using namespace gl;
namespace pl = gl::packs::plasma;

namespace {

std::atomic<bool> gWatch{false};
std::atomic<long> gAllocs{0};

constexpr double kSr = 48000;
constexpr int kBlock = 512;

// This thread's CPU time. Windows: its cycle count at a nominal 1 GHz (GetThreadTimes only moves
// in scheduler ticks of about 15.6 ms, too coarse for the idle stretches; the test compares ratios).
double threadSeconds() {
#if defined(_WIN32)
  ULONG64 cycles = 0;
  QueryThreadCycleTime(GetCurrentThread(), &cycles);
  return static_cast<double>(cycles) * 1e-9;
#else
  timespec ts{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
#endif
}

void setP(PluginState& s, int k, float v) {
  if (static_cast<int>(s.params.size()) <= k) s.params.resize(static_cast<size_t>(k + 1), 0.f);
  s.params[static_cast<size_t>(k)] = v;
}
void fx(PluginState& s, int slot, int type, std::initializer_list<std::pair<const char*, double>> vals) {
  setP(s, pl::fxP(slot, pl::X_Type), static_cast<float>(type));
  setP(s, pl::fxP(slot, pl::X_On), 1.f);
  const auto& d = pl::fxParams(type);
  for (size_t i = 0; i < d.size(); ++i) setP(s, pl::fxP(slot, pl::X_P0 + static_cast<int>(i)), pl::fxDefaultNorm(type, static_cast<int>(i)));
  for (const auto& [key, plain] : vals) {
    const int i = pl::fxParamIndex(type, key);
    if (i >= 0) setP(s, pl::fxP(slot, pl::X_P0 + i), static_cast<float>(pl::fxNorm(type, i, plain)));
  }
}

// A factory preset made heavy: 4x quality, a ten-module rack, a mono LFO on the rack's filter.
PluginState complexPatch(size_t preset) {
  const PluginInfo* info = findPlugin(pl::kType);
  PluginState s = presetState(*info, preset % info->presets.size());
  setP(s, pl::G_Quality, 2);
  const int base = 12;  // past the preset's own slots
  fx(s, base + 0, pl::FxDistortion, {{"kParamDrive", 30}, {"kParamWet", 40}});
  fx(s, base + 1, pl::FxChorus, {{"kParamWet", 30}});
  fx(s, base + 2, pl::FxPhaser, {{"kParamWet", 30}});
  fx(s, base + 3, pl::FxFlanger, {{"kParamWet", 25}});
  fx(s, base + 4, pl::FxDelay, {{"kParamWet", 25}, {"kParamFeedback", 50}});
  fx(s, base + 5, pl::FxComp, {{"kParamWet", 100}, {"kParamMultiband", 1}});
  fx(s, base + 6, pl::FxHyper, {{"kParamWet", 40}});
  fx(s, base + 7, pl::FxFilter, {{"kParamWet", 100}});
  fx(s, base + 8, pl::FxEq, {{"kParamGain2", -3}});
  fx(s, base + 9, pl::FxReverb, {{"kParamWet", 35}, {"kParamSize", 85}});
  // mono LFO 1 sweeps the rack filter's cutoff (a free-running global modulation)
  setP(s, pl::lfoP(0, pl::L_Mono), 1.f);
  const int slot = 60;
  setP(s, pl::modP(slot, pl::M_Source), static_cast<float>(pl::SrcLfo1));
  setP(s, pl::modP(slot, pl::M_Dest), static_cast<float>(pl::fxP(base + 7, pl::X_P0) + 1));
  setP(s, pl::modP(slot, pl::M_Amount), 0.3f);
  return s;
}

struct Inst {
  PluginState st;
  ChannelAux aux;
  std::unique_ptr<Generator> g;
};
Inst make(const PluginState& st) {
  Inst i;
  i.st = st;
  i.g = packs::createGenerator(st.type);
  packs::prepareAux(st, i.aux);
  i.g->prepareHost(st, kSr);
  return i;
}
ProcessContext stopped() {
  ProcessContext c;
  c.sampleRate = kSr;
  c.tempo = 120;
  c.ppq = 96;
  c.playing = false;
  return c;
}
// Runs `blocks` blocks of every instance; returns the thread CPU seconds spent; out: last block peak.
double run(std::vector<Inst>& v, int blocks, float* peak = nullptr) {
  static std::vector<float> L(kBlock), R(kBlock);  // (no allocation in the watched stretches)
  const ProcessContext c = stopped();
  float pk = 0;
  const double t0 = threadSeconds();
  for (int b = 0; b < blocks; ++b)
    for (auto& i : v) {
      std::fill(L.begin(), L.end(), 0.f), std::fill(R.begin(), R.end(), 0.f);
      i.g->process(i.st, i.aux, c, L.data(), R.data(), kBlock);
      if (b == blocks - 1)
        for (int k = 0; k < kBlock; ++k) pk = std::max({pk, std::fabs(L[static_cast<size_t>(k)]), std::fabs(R[static_cast<size_t>(k)])});
    }
  if (peak) *peak = pk;
  return threadSeconds() - t0;
}
void chord(Inst& i, bool on) {
  for (int k = 0; k < 4; ++k) {
    if (on) i.g->noteOn(static_cast<uint32_t>(k + 1), 48 + k * 4, 0.8f, 0);
    else i.g->noteOff(static_cast<uint32_t>(k + 1), 0);
  }
}
int blocksFor(double secs) { return static_cast<int>(secs * kSr / kBlock); }

struct Cost {
  double awakeUs = 0, idleUs = 0;  // per instance per block
  int asleep = 0;
  float idlePeak = 0;
};
// N complex instances: play a chord, release, then sit idle with the transport stopped.
// kind: 0 complex patches, 1 the default patch (no effects), 2 the factory presets as they are
Cost measure(int count, double settleSecs, double idleSecs, int kind = 0) {
  std::vector<Inst> v;
  const PluginInfo* info = findPlugin(pl::kType);
  for (int i = 0; i < count; ++i)
    v.push_back(make(kind == 1   ? defaultPluginState(pl::kType)
                     : kind == 2 ? presetState(*info, static_cast<size_t>(1 + i) % info->presets.size())
                                 : complexPatch(static_cast<size_t>(1 + i))));
  for (auto& i : v) chord(i, true);
  run(v, blocksFor(0.5));
  for (auto& i : v) chord(i, false);
  Cost c;
  const int tailBlocks = blocksFor(0.25);
  c.awakeUs = run(v, tailBlocks) / tailBlocks / count * 1e6;  // release tails: everything running
  run(v, blocksFor(settleSecs));
  const int idle = blocksFor(idleSecs);
  c.idleUs = run(v, idle, &c.idlePeak) / idle / count * 1e6;
  for (size_t k = 0; k < v.size(); ++k) {
    const bool a = pl::asleep(v[k].g.get());
    c.asleep += a ? 1 : 0;
    if (!a && std::getenv("GL_IDLE_VERBOSE")) {
      pl::LiveView lv;
      pl::liveView(v[k].g.get(), lv);
      std::printf("# awake: instance %zu (preset %zu) voices %d active %d out %g\n", k, k + 1, lv.voices, v[k].g->active() ? 1 : 0, static_cast<double>(lv.outL));
    }
  }
  return c;
}

// Renders until every instance sleeps (or `capSecs`); returns the blocks it took.
int settle(std::vector<Inst>& v, double capSecs) {
  const int step = blocksFor(1);
  for (int k = step; k <= blocksFor(capSecs); k += step) {
    run(v, step);
    bool all = true;
    for (auto& i : v) all = all && pl::asleep(i.g.get());
    if (all) return k;
  }
  return blocksFor(capSecs);
}

void idleInstancesSleep() {
  const int n = 6;
  std::vector<Inst> v;
  for (int i = 0; i < n; ++i) v.push_back(make(complexPatch(static_cast<size_t>(1 + i))));
  for (auto& i : v) chord(i, true);
  run(v, blocksFor(0.5));
  for (auto& i : v) chord(i, false);
  const int tail = blocksFor(0.5);
  const double awakeUs = run(v, tail) / tail / n * 1e6;
  const double took = settle(v, 90) * kBlock / kSr;
  int asleep = 0;
  for (auto& i : v) asleep += pl::asleep(i.g.get()) ? 1 : 0;
  float pk = 1;
  const int idle = blocksFor(2);
  const double idleUs = run(v, idle, &pk) / idle / n * 1e6;
  std::printf("# %d complex Plasma instances: release tail %.1f us / block each; all asleep after %.0f s; idle %.2f us / block each\n", n,
              awakeUs, took, idleUs);
  expect(asleep == n, "every idle complex instance falls asleep once its tails are done");
  expect(pk == 0.f, "a sleeping instance outputs exact zeros");
  expect(idleUs * 50 < awakeUs, "a sleeping instance costs under 2 % of a running one");
  // the default patch (no effects) sleeps a quarter of a second after its note
  std::vector<Inst> d;
  d.push_back(make(defaultPluginState(pl::kType)));
  chord(d[0], true);
  run(d, blocksFor(0.2));
  chord(d[0], false);
  expect(settle(d, 5) <= blocksFor(3), "the default patch sleeps within seconds of its release");
  // offline renders never sleep (exports stay what they always were)
  std::vector<float> L(kBlock), R(kBlock);
  ProcessContext off = stopped();
  off.offline = true;
  Inst o = make(defaultPluginState(pl::kType));
  for (int b = 0; b < blocksFor(5); ++b) o.g->process(o.st, o.aux, off, L.data(), R.data(), kBlock);
  expect(!pl::asleep(o.g.get()), "an offline render never sleeps");
}

// Effects without free-running modulation of their own (delay, EQ, distortion, filter, compressor),
// a mono LFO on the table position, another moving the filter, an unrouted one.
PluginState plainFxPatch() {
  PluginState s = defaultPluginState(pl::kType);
  setP(s, pl::G_Quality, 1);
  fx(s, 0, pl::FxDistortion, {{"kParamDrive", 30}, {"kParamWet", 50}});
  fx(s, 1, pl::FxFilter, {{"kParamWet", 100}});
  fx(s, 2, pl::FxDelay, {{"kParamWet", 30}, {"kParamFeedback", 55}});
  fx(s, 3, pl::FxEq, {{"kParamGain2", -4}});
  fx(s, 4, pl::FxComp, {{"kParamWet", 100}});
  setP(s, pl::lfoP(0, pl::L_Mono), 1.f);
  setP(s, pl::lfoP(0, pl::L_Rate), 0.37f);
  setP(s, pl::lfoP(1, pl::L_Mono), 1.f);
  setP(s, pl::modP(0, pl::M_Source), static_cast<float>(pl::SrcLfo1));
  setP(s, pl::modP(0, pl::M_Dest), static_cast<float>(pl::oscP(0, pl::O_WtPos) + 1));
  setP(s, pl::modP(0, pl::M_Amount), 0.8f);
  setP(s, pl::modP(1, pl::M_Source), static_cast<float>(pl::SrcLfo1 + 1));
  setP(s, pl::modP(1, pl::M_Dest), static_cast<float>(pl::fxP(1, pl::X_P0) + 1));
  setP(s, pl::modP(1, pl::M_Amount), 0.4f);
  return s;
}

// Runs a against b (b never sleeps) over `secs` with a chord at `onAt` held for 1 s; returns the
// worst sample difference and b's peak.
void compare(Inst& a, Inst& b, double secs, double onAt, double& worst, double& peak) {
  std::vector<float> aL(kBlock), aR(kBlock), bL(kBlock), bR(kBlock);
  const ProcessContext c = stopped();
  worst = 0, peak = 0;
  const int on = blocksFor(onAt), off = blocksFor(onAt + 1);
  for (int k = 0; k < blocksFor(secs); ++k) {
    if (k == on) chord(a, true), chord(b, true);
    if (k == off) chord(a, false), chord(b, false);
    std::fill(aL.begin(), aL.end(), 0.f), std::fill(aR.begin(), aR.end(), 0.f);
    std::fill(bL.begin(), bL.end(), 0.f), std::fill(bR.begin(), bR.end(), 0.f);
    a.g->process(a.st, a.aux, c, aL.data(), aR.data(), kBlock);
    b.g->process(b.st, b.aux, c, bL.data(), bR.data(), kBlock);
    for (int i = 0; i < kBlock; ++i) {
      const size_t j = static_cast<size_t>(i);
      worst = std::max({worst, std::fabs(static_cast<double>(aL[j]) - bL[j]), std::fabs(static_cast<double>(aR[j]) - bR[j])});
      peak = std::max(peak, std::fabs(static_cast<double>(bL[j])));
    }
  }
}

// A note after a long sleep plays exactly what an instance that never slept plays (mono LFO phases,
// the matrix, the effects' silent state); a second note on a sleeping tail differs by less than the
// tail it slept on (-120 dB).
void noteWakesExactly() {
  const PluginState st = plainFxPatch();
  Inst a = make(st), b = make(st);
  pl::setSleepAllowed(b.g.get(), false);
  double worst = 0, peak = 0;
  compare(a, b, 12, 10, worst, peak);  // 10 s of nothing, then the chord
  std::printf("# fresh instance, chord after 10 s idle: peak %.4f, worst difference %.3g\n", peak, worst);
  expect(peak > 0.01, "the chord sounds");
  expect(worst == 0.0, "a woken instance plays exactly what a never-slept one plays");
  // a's tail is now asleep below -120 dB; the next chord against b, which kept running its tail
  std::vector<Inst> va;
  va.push_back(std::move(a));
  const int slept = settle(va, 60);
  expect(pl::asleep(va[0].g.get()), "asleep again after the tail");
  {
    std::vector<Inst> vb;
    vb.push_back(std::move(b));
    run(vb, slept);  // b runs the same stretch, awake
    b = std::move(vb[0]);
  }
  compare(va[0], b, 3, 0.5, worst, peak);
  std::printf("# chord on a slept tail: peak %.4f, worst difference %.3g\n", peak, worst);
  expect(peak > 0.01 && worst < 2e-6, "a chord on a slept tail differs by less than the tail (-114 dB)");
}

// Complex patches woken after 10 s against never-slept ones. Without the modules that run a
// modulation of their own (chorus, phaser, flanger, hyper, the reverb's spin) the woken chord is the
// same to within -120 dB; with them, their sweep resumes where it paused (a free-running phase, as
// arbitrary as it always was in live playback), so only the level is compared.
void complexWakeSameSound() {
  for (int withMod = 0; withMod < 2; ++withMod)
    for (size_t preset : {1, 4, 13}) {
      PluginState st = complexPatch(preset);
      if (!withMod)
        for (int s = 0; s < pl::kFxSlots; ++s) {
          const int t = static_cast<int>(std::lround(st.params[static_cast<size_t>(pl::fxP(s, pl::X_Type))]));
          if (t == pl::FxChorus || t == pl::FxPhaser || t == pl::FxFlanger || t == pl::FxHyper || t == pl::FxReverb)
            setP(st, pl::fxP(s, pl::X_Type), 0.f);
        }
      Inst a = make(st), b = make(st);
      pl::setSleepAllowed(b.g.get(), false);
      std::vector<float> aL(kBlock), aR(kBlock), bL(kBlock), bR(kBlock);
      const ProcessContext c = stopped();
      double ea = 0, eb = 0, worst = 0;
      for (int k = 0; k < blocksFor(12); ++k) {
        if (k == blocksFor(10)) chord(a, true), chord(b, true);
        std::fill(aL.begin(), aL.end(), 0.f), std::fill(aR.begin(), aR.end(), 0.f);
        std::fill(bL.begin(), bL.end(), 0.f), std::fill(bR.begin(), bR.end(), 0.f);
        a.g->process(a.st, a.aux, c, aL.data(), aR.data(), kBlock);
        b.g->process(b.st, b.aux, c, bL.data(), bR.data(), kBlock);
        for (int i = 0; i < kBlock; ++i) {
          const size_t j = static_cast<size_t>(i);
          ea += static_cast<double>(aL[j]) * aL[j] + static_cast<double>(aR[j]) * aR[j];
          eb += static_cast<double>(bL[j]) * bL[j] + static_cast<double>(bR[j]) * bR[j];
          worst = std::max({worst, std::fabs(static_cast<double>(aL[j]) - bL[j]), std::fabs(static_cast<double>(aR[j]) - bR[j])});
        }
      }
      const double db = 10 * std::log10((ea + 1e-30) / (eb + 1e-30));
      std::printf("# complex patch %zu%s woken against never-slept: level %.3f dB, worst difference %.3g\n", preset,
                  withMod ? " (modulated effects)" : "", db, worst);
      expect(eb > 1, "the complex chord sounds");
      if (withMod) expect(std::fabs(db) < 3, "with modulated effects: the same level (their sweep resumes where it paused)");
      else expect(worst < 1e-6, "without modulated effects: the same samples (-120 dB)");
    }
}

// A parameter change wakes a sleeping instance; it sleeps again once nothing sounds.
void parameterWakes() {
  std::vector<Inst> v;
  v.push_back(make(plainFxPatch()));
  run(v, 2);  // first blocks (as the engine's warm-up does)
  gWatch = true;  // falling asleep, sleeping, waking and playing allocate nothing
  settle(v, 10);
  expect(pl::asleep(v[0].g.get()), "asleep before the change");
  setP(v[0].st, pl::fxP(2, pl::X_P0 + pl::fxParamIndex(pl::FxDelay, "kParamWet")), 0.9f);
  run(v, 1);
  expect(!pl::asleep(v[0].g.get()), "a parameter change wakes the instance");
  settle(v, 10);
  expect(pl::asleep(v[0].g.get()), "and it sleeps again");
  // a note wakes it in the block it starts in
  chord(v[0], true);
  float pk = 0;
  run(v, 1, &pk);
  expect(!pl::asleep(v[0].g.get()) && pk > 1e-3f, "a note wakes it and sounds in its first block");
  gWatch = false;
  expect(gAllocs.exchange(0) == 0, "no allocation on the audio thread through sleep and wake");
}

// A 1.6 s free delay at high feedback: the echoes after the quiet gaps all still play.
void longDelayTailKept() {
  PluginState st = defaultPluginState(pl::kType);
  fx(st, 0, pl::FxDelay, {{"kParamWet", 100}, {"kParamFeedback", 60}, {"kParamBeatSync", 0}, {"kParamTimeL", 0.9}, {"kParamTimeR", 0.9}});
  setP(st, pl::envP(0, pl::E_Sustain), 0.f);
  setP(st, pl::envP(0, pl::E_Decay), 0.2f);
  setP(st, pl::envP(0, pl::E_Release), 0.1f);
  Inst a = make(st), b = make(st);
  pl::setSleepAllowed(b.g.get(), false);
  double worst = 0, peak = 0;
  compare(a, b, 12, 0, worst, peak);
  std::printf("# long delay: peak %.4f, worst difference %.3g\n", peak, worst);
  expect(peak > 1e-2, "the delay sounds");
  expect(worst < 2e-6, "every echo comes back with sleep allowed");
}

// In the engine (live, transport stopped): after a chord, the Plasma channel goes to sleep and its
// exact silence lets the reverb on its mixer track rest too (a tail at -150 dB kept it running).
void engineLetsInsertsRest() {
  constexpr int kEsr = 48000;
  Song s = makeEmptySong();
  Channel ch;
  ch.id = s.allocateId();
  ch.name = "plasma";
  ch.plugin = plainFxPatch();
  MixerTrack& m = s.mixer[1];
  ch.mixerTrack = m.id;
  m.slots[0].plugin = defaultPluginState("gl.reverb");
  m.slots[0].enabled = true;
  const ObjectId track = m.id;
  s.channels.push_back(ch);
  ProgramBuilder b;
  AudioEngine eng(kEsr);
  b.setSampleRate(kEsr);
  eng.setProgram(b.build(s));
  auto rt = b.channelRuntime(ch.id);
  expect(rt && rt->gen, "the Plasma channel runs");
  if (!rt || !rt->gen) return;
  std::vector<float> L(512), R(512);
  float peak = 0;
  for (int i = 0; i < blocksFor(30); ++i) {
    if (i == 0)
      for (int k = 0; k < 4; ++k) rt->gen->noteOn(static_cast<uint32_t>(k + 1), 48 + k * 4, 0.8f, 0);
    if (i == blocksFor(0.5))
      for (int k = 0; k < 4; ++k) rt->gen->noteOff(static_cast<uint32_t>(k + 1), 0);
    std::fill(L.begin(), L.end(), 0.f), std::fill(R.begin(), R.end(), 0.f);
    eng.process(L.data(), R.data(), 512);
    if (i < blocksFor(1)) peak = std::max(peak, std::fabs(L[0]));
  }
  auto rv = b.effectRuntime(track, 0);
  std::printf("# engine: Plasma asleep %d, reverb insert quiet for %.1f s\n", pl::asleep(rt->gen.get()) ? 1 : 0,
              rv ? static_cast<double>(rv->quiet) / kEsr : -1.0);
  expect(peak > 1e-3f, "the chord plays through the engine");
  expect(pl::asleep(rt->gen.get()), "the idle Plasma channel sleeps in the engine");
  expect(rv && rv->quiet >= 6 * kEsr - 1024, "the reverb on its track rests behind it (6 s of exact silence in)");
}

}  // namespace

void* operator new(std::size_t n) {
  if (gWatch) ++gAllocs;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
  if (gWatch) ++gAllocs;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 11  // (GCC 11 added this warning)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"  // the replaced operator new is malloc
#endif
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

int main(int argc, char** argv) {
  const ScopedFlushDenormals ftz;  // as the audio thread runs
  if (argc > 3 && std::string(argv[1]) == "project") {  // project <path> <count>: N complex Plasma channels, no notes
    Song s = makeEmptySong();
    for (int i = 0; i < std::atoi(argv[3]); ++i) {
      Channel ch;
      ch.id = s.allocateId();
      ch.name = "Plasma " + std::to_string(i + 1);
      ch.plugin = complexPatch(static_cast<size_t>(1 + i));
      ch.mixerTrack = s.mixer[static_cast<size_t>(1 + i % 8)].id;
      s.channels.push_back(ch);
    }
    std::string err;
    const bool ok = saveProject(argv[2], s, &err);
    std::printf("%s %s\n", ok ? "wrote" : "failed", ok ? argv[2] : err.c_str());
    return ok ? 0 : 1;
  }
  if (argc > 2 && std::string(argv[1]) == "decay") {  // per-second output peak after a chord: decay <preset> [kind]
    const PluginInfo* info = findPlugin(pl::kType);
    const size_t pi = static_cast<size_t>(std::atoi(argv[2]));
    const int kind = argc > 3 ? std::atoi(argv[3]) : 0;
    Inst i = make(kind == 2 ? presetState(*info, pi % info->presets.size()) : complexPatch(pi));
    pl::setSleepAllowed(i.g.get(), false);
    std::printf("preset %s\n", info->presets[pi % info->presets.size()].name.c_str());
    std::vector<Inst> v;
    v.push_back(std::move(i));
    chord(v[0], true);
    run(v, blocksFor(0.5));
    chord(v[0], false);
    for (int sec = 0; sec < 60; ++sec) {
      float worst = 0;
      for (int b = 0; b < blocksFor(1); ++b) {
        float pk = 0;
        run(v, 1, &pk);
        worst = std::max(worst, pk);
      }
      std::printf("%d s\t%.3g\n", sec, static_cast<double>(worst));
    }
    return 0;
  }
  if (argc > 1 && std::string(argv[1]) == "bench") {  // before / after numbers: bench [count] [idle seconds]
    const int n = argc > 2 ? std::atoi(argv[2]) : 32;
    const double secs = argc > 3 ? std::atof(argv[3]) : 5;
    const int kind = argc > 4 ? std::atoi(argv[4]) : 0;
    const Cost c = measure(n, 40, secs, kind);
    const double blockUs = kBlock / kSr * 1e6;
    std::printf("instances %d\tawake %.1f us/block each\tidle %.2f us/block each\tidle total %.1f %% of one core\tasleep %d\tidle peak %g\n", n,
                c.awakeUs, c.idleUs, 100.0 * c.idleUs * n / blockUs, c.asleep, static_cast<double>(c.idlePeak));
    return 0;
  }
  idleInstancesSleep();
  noteWakesExactly();
  complexWakeSameSound();
  parameterWakes();
  longDelayTailKept();
  engineLetsInsertsRest();
  return finish();
}
