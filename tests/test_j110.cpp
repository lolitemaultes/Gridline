// Gridline — J-110: plugins' own play / run states follow Stop.
//  * Seltmann: the pattern latched by its key stops on the first Stop, on the
//    panic (Stop twice) and on a focus loss's release, and starts again from
//    step 1 on the next key; the editor's Play button (RunLatch) shows stopped
//    after each;
//  * the engine's stop epoch moves on every silencing Stop (not on Play);
//  * a one-shot that ends by itself while its key is held (Kerf's Play):
//    the channel's `sounding` flag drops, the latch reports Finished;
//  * Shuttle free-running with Hold on: Stop ends the bar repeat;
//  * Loupe in a mixer slot: Stop ends the editor's playback;
//  * the audio thread allocates nothing for any of it.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <vector>
#include "Check.h"
#include "app/RunLatch.h"
#include "core/AudioBuffer.h"
#include "core/Document.h"
#include "core/PluginParams.h"
#include "core/Song.h"
#include "core/WavWriter.h"
#include "engine/AudioEngine.h"
#include "engine/Effects.h"
#include "engine/Program.h"
#include "engine/Sample.h"
#include "packs/Packs.h"
#include "packs/edison/edison.h"
#include "packs/seltmann/seltmann.h"

// ---- allocation counter (realtime-safety checks) ----
namespace {
thread_local bool gCount = false;
std::atomic<long> gAllocs{0};
}  // namespace
void* operator new(std::size_t n) {
  if (gCount) gAllocs.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
  if (gCount) gAllocs.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"  // the replaced operator new is malloc
#endif
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace gl;
namespace sm = gl::packs::seltmann;

namespace {

constexpr double kSr = 48000.0;
constexpr int kBlock = 256;

struct Rig {
  ProgramBuilder builder;
  AudioEngine eng{kSr};
  std::vector<float> L, R;
  std::vector<float> bl = std::vector<float>(kBlock), br = std::vector<float>(kBlock);
  long allocs = 0;
  explicit Rig(const Song& s) {
    builder.setSampleRate(kSr);
    eng.setProgram(builder.build(s));
    L.reserve(1 << 23);
    R.reserve(1 << 23);
  }
  void run(int frames) {
    for (int d = 0; d < frames; d += kBlock) {
      gAllocs = 0;
      gCount = true;
      eng.process(bl.data(), br.data(), kBlock);
      gCount = false;
      allocs += gAllocs.load();
      L.insert(L.end(), bl.begin(), bl.end());
      R.insert(R.end(), br.begin(), br.end());
    }
  }
  size_t mark() const { return L.size(); }
  float peakFrom(size_t from) const {
    float p = 0;
    for (size_t i = from; i < L.size(); ++i) p = std::max(p, std::max(std::fabs(L[i]), std::fabs(R[i])));
    return p;
  }
};

int secs(double s) { return static_cast<int>(s * kSr); }

// A 2-bar break at 120 BPM: a decaying noise hit on every eighth.
std::string writeBreak() {
  const int sr = 44100;
  const int n = sr * 4;  // 2 bars of 4/4 at 120 BPM
  AudioBuffer b(2, static_cast<size_t>(n));
  uint32_t rng = 12345;
  for (int k = 0; k < 16; ++k) {
    const int at = k * sr / 4;
    const float amp = k % 4 == 0 ? 0.9f : 0.5f;
    for (int i = 0; i < sr / 5 && at + i < n; ++i) {
      rng = rng * 1664525u + 1013904223u;
      const float noise = static_cast<float>((rng >> 9) & 0xFFFF) / 32768.f - 1.f;
      const float v = amp * std::exp(-static_cast<float>(i) / (sr * 0.03f)) * (0.6f * noise + 0.4f * std::sin(static_cast<float>(i) * 0.05f));
      b.data[static_cast<size_t>(at + i)] = v;
      b.data[static_cast<size_t>(n + at + i)] = v;
    }
  }
  const std::string path = "j110_break.wav";
  expect(writeWav(path, b, sr), "test break written");
  return path;
}

sm::LiveView seltLive(Rig& rig, ObjectId ch) {
  sm::LiveView lv;
  if (auto rt = rig.builder.channelRuntime(ch); rt && rt->gen) sm::liveView(rt->gen.get(), lv);
  return lv;
}

// 1. Seltmann: the pattern flag clears on Stop / panic / focus loss and the
// pattern restarts at step 1; the editor's latch follows.
void testSeltmannPattern() {
  const std::string path = writeBreak();
  Document d(makeEmptySong());
  const ObjectId ch = d.addChannel("selt", sm::kType);
  d.editChannel(ch, "", [&](Channel& c) {
    c.plugin.data = path;
    c.plugin.params[sm::SmSync] = sm::SyncOriginal;  // (no warp render to wait for)
    c.plugin.params[sm::SmLock] = 0;                 // free-running from the key
  });
  Rig rig(d.song());
  const int key = 48;  // the octave below the base note (60)
  auto start = [&](RunLatch& latch) {
    rig.eng.noteOn(ch, key, 0.9f);
    latch.start(rig.eng);
  };
  RunLatch latch;
  start(latch);
  rig.run(secs(1.3));  // a few steps
  sm::LiveView lv = seltLive(rig, ch);
  expect(lv.patternRunning, "Seltmann: the pattern key starts the pattern");
  expect(lv.step > 0, "Seltmann: the pattern advances past step 1");
  expect(rig.peakFrom(rig.mark() - secs(0.5)) > 0.05f, "Seltmann: the pattern sounds");
  expect(latch.sync(rig.eng, lv.patternRunning) == RunLatch::End::None && latch.on, "Seltmann: the Play button shows playing");

  // first Stop (transport stopped already: a release)
  expect(!rig.eng.stopPressed(), "Seltmann: first Stop releases");
  expect(latch.sync(rig.eng, lv.patternRunning) == RunLatch::End::Stopped && !latch.on,
         "Seltmann: Stop: the Play button shows stopped at once (before the audio thread ran)");
  rig.run(kBlock);
  lv = seltLive(rig, ch);
  expect(!lv.patternRunning, "Seltmann: Stop clears the pattern's running flag");
  expect(lv.patternPos < 0, "Seltmann: Stop: no pattern position shown");
  rig.run(secs(1.5));
  expect(rig.peakFrom(rig.mark() - secs(0.5)) < 1e-4f, "Seltmann: Stop: the pattern is silent (no step plays any more)");

  // one click: plays again, from step 1
  start(latch);
  rig.run(kBlock * 2);
  lv = seltLive(rig, ch);
  expect(lv.patternRunning && lv.step == 0, "Seltmann: after Stop one start plays, from step 1");
  expect(latch.sync(rig.eng, lv.patternRunning) == RunLatch::End::None && latch.on, "Seltmann: ... the button shows playing");
  rig.run(secs(1.0));

  // Stop twice: the panic
  rig.eng.stopPressed();
  rig.run(kBlock);
  expect(rig.eng.stopPressed(), "Seltmann: Stop again while stopped: the panic");
  rig.run(kBlock * 4);
  lv = seltLive(rig, ch);
  expect(!lv.patternRunning, "Seltmann: the panic leaves the pattern stopped");
  expect(latch.sync(rig.eng, lv.patternRunning) == RunLatch::End::Stopped, "Seltmann: panic: the button shows stopped");
  start(latch);
  rig.run(kBlock * 2);
  lv = seltLive(rig, ch);
  expect(lv.patternRunning && lv.step == 0, "Seltmann: after the panic one start plays, from step 1");
  rig.run(secs(1.0));
  expect(rig.peakFrom(rig.mark() - secs(0.5)) > 0.05f, "Seltmann: after the panic the pattern sounds again");

  // focus loss (the interface's notes released)
  rig.eng.releaseLiveNotes();
  rig.run(kBlock);
  lv = seltLive(rig, ch);
  expect(!lv.patternRunning, "Seltmann: a focus loss stops the pattern (its key is released)");
  expect(latch.sync(rig.eng, lv.patternRunning) == RunLatch::End::Stopped, "Seltmann: focus loss: the button shows stopped");
  start(latch);
  rig.run(kBlock * 2);
  lv = seltLive(rig, ch);
  expect(lv.patternRunning && lv.step == 0, "Seltmann: after a focus loss one start plays, from step 1");

  // the song transport starts while it plays: it keeps playing; the song's Stop stops it
  const uint32_t ep = rig.eng.stopEpoch();
  rig.eng.play();
  rig.run(secs(0.5));
  lv = seltLive(rig, ch);
  expect(lv.patternRunning && rig.eng.stopEpoch() == ep, "Seltmann: the song starting leaves the pattern playing");
  expect(latch.sync(rig.eng, lv.patternRunning) == RunLatch::End::None && latch.on, "Seltmann: ... and its button lit");
  rig.eng.stopPressed();
  rig.run(kBlock);
  lv = seltLive(rig, ch);
  expect(!lv.patternRunning && latch.sync(rig.eng, lv.patternRunning) == RunLatch::End::Stopped, "Seltmann: the song's Stop stops it");

  // the pattern stopped by the instrument itself (killed: a project reload's panic(false))
  start(latch);
  rig.run(kBlock * 2);
  latch.sync(rig.eng, seltLive(rig, ch).patternRunning);
  rig.builder.channelRuntime(ch)->gen->kill();  // (as the audio thread would; nothing else runs here)
  lv = seltLive(rig, ch);
  expect(!lv.patternRunning, "Seltmann: a killed instrument clears the flag");
  expect(latch.sync(rig.eng, lv.patternRunning) == RunLatch::End::Finished && !latch.on,
         "Seltmann: the button follows the instrument's own flag (ended without a Stop)");
  expect(rig.allocs == 0, "Seltmann pattern + Stop / panic / focus: no allocation on the audio thread");
}

// 2. The stop epoch: every silencing Stop moves it, Play does not.
void testStopEpoch() {
  Document d(makeEmptySong());
  Rig rig(d.song());
  uint32_t e = rig.eng.stopEpoch();
  rig.eng.play();
  expect(rig.eng.stopEpoch() == e, "epoch: Play leaves it");
  rig.eng.stopPressed();
  expect(rig.eng.stopEpoch() != e, "epoch: Stop moves it");
  e = rig.eng.stopEpoch();
  rig.run(kBlock);
  rig.eng.stopPressed();  // the panic
  expect(rig.eng.stopEpoch() != e, "epoch: the panic moves it");
  e = rig.eng.stopEpoch();
  rig.eng.releaseLiveNotes();
  expect(rig.eng.stopEpoch() != e, "epoch: a focus loss moves it");
  e = rig.eng.stopEpoch();
  rig.eng.panic();  // Tools > Stop all sound
  expect(rig.eng.stopEpoch() != e, "epoch: Stop all sound moves it");
  // a latch with no DSP state to read follows the epoch only (chord editor preview)
  RunLatch l;
  l.start(rig.eng);
  bool stays = true;
  for (int i = 0; i < 200; ++i) stays = stays && l.sync(rig.eng, true) == RunLatch::End::None;
  expect(stays && l.on, "latch: stays on while nothing stops it");
  rig.eng.stopPressed();
  expect(l.sync(rig.eng, true) == RunLatch::End::Stopped && !l.on, "latch: Stop turns it off");
  // a latch whose sound never starts gives up (no audio device, no instrument)
  l.start(rig.eng);
  bool off = false;
  for (int i = 0; i < 200 && !off; ++i) off = l.sync(rig.eng, false) == RunLatch::End::Finished;
  expect(off, "latch: never running: off after the grace frames");
}

// 3. Kerf's Play: a one-shot that ends while its key is held. The channel's
// sounding flag drops and the latch reports Finished (the editor lets the key go).
void testOneShotEnds() {
  Document d(makeEmptySong());
  const ObjectId ch = d.addChannel("kick", "gl.kick");
  Rig rig(d.song());
  RunLatch l;
  rig.eng.noteOn(ch, 60, 0.9f);  // held: no note-off
  l.start(rig.eng);
  rig.run(kBlock * 2);
  auto rt = rig.builder.channelRuntime(ch);
  expect(rt && rt->sounding.load(), "one-shot: sounding while it plays");
  expect(l.sync(rig.eng, rt->sounding.load()) == RunLatch::End::None, "one-shot: the Play button lit");
  rig.run(secs(3.0));
  expect(!rt->sounding.load(), "one-shot: the instrument reports silence at its end (key still held)");
  expect(l.sync(rig.eng, rt->sounding.load()) == RunLatch::End::Finished, "one-shot: the Play button goes off by itself");
  expect(rig.allocs == 0, "one-shot: no allocation on the audio thread");
}

// 4. Shuttle free-running with Hold: the bar repeat goes on while stopped,
// Stop ends it.
void testShuttleFreeRun() {
  Document d(makeEmptySong());
  const ObjectId ch = d.addChannel("kick", "gl.kick");
  const ObjectId t1 = d.song().mixer[1].id;
  d.routeChannel(ch, t1);
  d.setSlotPlugin(t1, 0, "gl.timegate");
  Song s = d.song();
  PluginState& tg = s.mixer[1].slots[0].plugin;
  tg.params[static_cast<size_t>(pp::TgFreeRun)] = 1;
  Rig rig(s);
  rig.eng.noteOn(ch, 60, 1.0f);
  rig.run(secs(0.6));
  rig.eng.noteOff(ch, 60);
  tg.params[static_cast<size_t>(pp::TgHold)] = 1;  // hold on: the last bar repeats
  rig.eng.setProgram(rig.builder.build(s));
  rig.run(secs(4.0));
  expect(rig.peakFrom(rig.mark() - secs(2.0)) > 0.05f, "Shuttle: free-running hold repeats the kick while stopped");
  rig.allocs = 0;
  rig.eng.stopPressed();
  rig.run(secs(4.0));
  expect(rig.peakFrom(rig.mark() - secs(3.5)) < 1e-4f, "Shuttle: Stop ends the free-running bar repeat");
  expect(rig.allocs == 0, "Shuttle + Stop: no allocation on the audio thread");
}

// 5. Loupe in a mixer slot (the effect, then in the engine's mixer): Stop ends the
// editor's playback; isPlaying() (what the editor's button reads) follows.
void testLoupe() {
  auto smp = std::make_shared<Sample>();
  smp->sampleRate = kSr;
  smp->left.resize(static_cast<size_t>(secs(5.0)));
  for (size_t i = 0; i < smp->left.size(); ++i) smp->left[i] = 0.4f * static_cast<float>(std::sin(static_cast<double>(i) * 0.03));
  smp->setMono();
  ChannelAux aux;
  aux.samples.push_back(smp);
  PluginState st = defaultPluginState(packs::edison::kType);
  ProcessContext ctx;
  ctx.sampleRate = kSr;
  auto fx = packs::createEffect(packs::edison::kType);
  auto* ed = dynamic_cast<packs::edison::EdisonFx*>(fx.get());
  expect(ed != nullptr, "Loupe: the slot effect");
  if (!ed) return;
  std::vector<float> L(kBlock), R(kBlock);
  auto block = [&]() {
    std::fill(L.begin(), L.end(), 0.f);
    std::fill(R.begin(), R.end(), 0.f);
    gAllocs = 0;
    gCount = true;
    ed->processAux(st, aux, ctx, L.data(), R.data(), kBlock);
    gCount = false;
    float p = 0;
    for (int i = 0; i < kBlock; ++i) p = std::max(p, std::fabs(L[static_cast<size_t>(i)]));
    return p;
  };
  ed->play(0, 0, false);
  block();
  expect(ed->isPlaying() && block() > 0.1f, "Loupe: the editor's Play sounds in the slot");
  long allocs = 0;
  gAllocs = 0;
  gCount = true;
  ed->stopSound();  // what the engine's Stop does to every slot
  gCount = false;
  allocs += gAllocs.load();
  expect(!ed->isPlaying(), "Loupe: Stop: the editor's Play button shows stopped");
  expect(block() < 1e-7f, "Loupe: Stop: the slot's playback is silent");
  allocs += gAllocs.load();
  expect(allocs == 0, "Loupe: Stop allocates nothing");

  // the engine calls stopSound on every slot on the first Stop
  Document d(makeEmptySong());
  const ObjectId t1 = d.song().mixer[1].id;
  d.setSlotPlugin(t1, 0, packs::edison::kType);
  Rig rig(d.song());
  rig.run(kBlock);
  const auto& prog = rig.eng.program();
  auto* slot = prog && prog->mixer.size() > 1 && !prog->mixer[1].slots.empty() && prog->mixer[1].slots[0].rt
                   ? dynamic_cast<packs::edison::EdisonFx*>(prog->mixer[1].slots[0].rt->fx.get())
                   : nullptr;
  expect(slot != nullptr, "Loupe in the mixer: the slot runtime");
  if (slot) {
    slot->play(0, 0, false);
    expect(slot->isPlaying(), "Loupe in the mixer: playing");
    rig.eng.stopPressed();
    rig.run(kBlock);
    expect(!slot->isPlaying(), "Loupe in the mixer: the transport's Stop ends the editor's playback");
  }
  expect(rig.allocs == 0, "Loupe in the mixer + Stop: no allocation on the audio thread");
}

}  // namespace

int main() {
  testSeltmannPattern();
  testStopEpoch();
  testOneShotEnds();
  testShuttleFreeRun();
  testLoupe();
  sm::waitAnalysisIdle();
  return finish();
}
