// Gridline — J-112: Play while the transport is stopped and a plugin plays on
// its own stops that plugin's playback instead of starting the pattern / song;
// the next Play plays.
//  * the engine's report (AudioEngine::pluginPlayback) for Seltmann's pattern,
//    Kerf's region play (an editor's own key), the chord editor's preview,
//    Loupe in a mixer slot and standalone (its own preview), Shuttle's
//    free-running held bar;
//  * stopPluginPlayback(): that playback ends with a normal release (no hard
//    cut), the stop epoch moves (the editors' RunLatch shows stopped), the
//    transport stays stopped, and the next Play starts the transport;
//  * not plugin playback: a key held live, the browser's preview;
//  * the song started some other way leaves the pattern playing (J-110);
//  * the audio thread allocates nothing for any of it.
// play() below is the rule of menuparity::playPressed (app/MenuParityUi.cpp)
// on the engine alone.
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
using OP = AudioEngine::OwnPlay;

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
  float peakFrom(size_t from, size_t to = SIZE_MAX) const {
    float p = 0;
    for (size_t i = from; i < std::min(to, L.size()); ++i) p = std::max(p, std::max(std::fabs(L[i]), std::fabs(R[i])));
    return p;
  }
};

int secs(double s) { return static_cast<int>(s * kSr); }

// The rule (menuparity::playPressed): stopped + plugin playback -> stop it; else play.
// Returns true when the transport was started.
bool play(Rig& rig) {
  if (rig.eng.playing()) return false;
  if (rig.eng.pluginPlaybackActive()) {
    rig.eng.stopPluginPlayback();
    return false;
  }
  rig.eng.play();
  return true;
}

// A 2-bar break at 120 BPM: a decaying noise hit on every eighth.
std::string writeBreak() {
  const int sr = 44100;
  const int n = sr * 4;
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
  const std::string path = "j112_break.wav";
  expect(writeWav(path, b, sr), "test break written");
  return path;
}

// A held tone: a sustained sine for a sampler-like one-shot (Kerf's region).
std::string writeTone() {
  const int sr = 44100;
  const int n = sr * 6;
  AudioBuffer b(2, static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) {
    const float v = 0.5f * std::sin(static_cast<float>(i) * 0.06f);
    b.data[static_cast<size_t>(i)] = v;
    b.data[static_cast<size_t>(n + i)] = v;
  }
  const std::string path = "j112_tone.wav";
  expect(writeWav(path, b, sr), "test tone written");
  return path;
}

sm::LiveView seltLive(Rig& rig, ObjectId ch) {
  sm::LiveView lv;
  if (auto rt = rig.builder.channelRuntime(ch); rt && rt->gen) sm::liveView(rt->gen.get(), lv);
  return lv;
}

// 1. Seltmann's pattern (the editor's Play: its key held as the editor's own).
void testSeltmann() {
  const std::string path = writeBreak();
  Document d(makeEmptySong());
  const ObjectId ch = d.addChannel("selt", sm::kType);
  d.editChannel(ch, "", [&](Channel& c) {
    c.plugin.data = path;
    c.plugin.params[sm::SmSync] = sm::SyncOriginal;
    c.plugin.params[sm::SmLock] = 0;
  });
  Rig rig(d.song());
  const int key = 48;
  RunLatch latch;
  rig.eng.ownNoteOn(ch, key, 0.9f);
  latch.start(rig.eng);
  rig.run(secs(1.3));
  expect(seltLive(rig, ch).patternRunning, "Seltmann: the pattern runs");
  AudioEngine::OwnPlayback own = rig.eng.pluginPlayback();
  expect(own.kind == OP::Instrument && own.id == ch, "Seltmann: the engine reports plugin playback (the instrument, its channel)");
  expect(!rig.eng.playing(), "Seltmann: the transport is stopped");

  const uint32_t ep = rig.eng.stopEpoch();
  expect(!play(rig), "Seltmann: Play while the pattern runs does not start the transport");
  expect(!rig.eng.playing(), "Seltmann: ... the transport stays stopped");
  expect(rig.eng.stopEpoch() != ep, "Seltmann: ... the stop epoch moved");
  expect(latch.sync(rig.eng, true) == RunLatch::End::Stopped && !latch.on, "Seltmann: ... the editor's Play button shows stopped at once");
  expect(!rig.eng.pluginPlaybackActive(), "Seltmann: ... no plugin playback reported while the stop is on its way");
  rig.run(kBlock);
  sm::LiveView lv = seltLive(rig, ch);
  expect(!lv.patternRunning, "Seltmann: ... the pattern's running flag cleared");
  expect(rig.eng.liveHeld() == 0, "Seltmann: ... its key released");
  expect(!rig.eng.pluginPlaybackActive(), "Seltmann: ... no plugin playback after the audio thread ran the stop");
  rig.run(secs(1.5));
  expect(rig.peakFrom(rig.mark() - secs(0.5)) < 1e-4f, "Seltmann: ... silent (no step plays any more)");

  expect(play(rig) && rig.eng.playing(), "Seltmann: the next Play starts the transport");
  rig.eng.stop();
  rig.eng.releaseAllNotes();
  rig.run(kBlock);

  // The song started some other way (a playlist click): the pattern keeps running (J-110).
  rig.eng.ownNoteOn(ch, key, 0.9f);
  rig.run(secs(0.5));
  expect(seltLive(rig, ch).patternRunning, "Seltmann: running again");
  rig.eng.play();  // not the Play command
  rig.run(secs(0.5));
  expect(seltLive(rig, ch).patternRunning && rig.eng.playing(), "Seltmann: a song start other than Play leaves the pattern playing");
  expect(!play(rig), "Seltmann: Play while the song plays is not this rule (Pause / Stop as before)");
  rig.eng.stopPressed();
  rig.run(kBlock);

  // A MIDI keyboard's key in the pattern octave latched it (no editor key): still the instrument's playback.
  rig.eng.setMidiTarget(ch);
  rig.eng.midiEvent(0x90, key, 100);
  rig.run(secs(0.4));
  if (seltLive(rig, ch).patternRunning) {
    expect(rig.eng.pluginPlaybackActive(), "Seltmann (MIDI key): plugin playback");
    expect(!play(rig), "Seltmann (MIDI key): Play stops it");
    rig.run(kBlock);
    expect(!seltLive(rig, ch).patternRunning, "Seltmann (MIDI key): the pattern stopped");
    rig.eng.midiEvent(0x80, key, 0);
    rig.run(kBlock);
  }
  expect(rig.allocs == 0, "Seltmann + Play stops it: no allocation on the audio thread");
}

// 2. Release, not a cut: the pattern's sound decays after the stop instead of
// ending in the next sample (a voice holding when Play is pressed).
void testNoHardCut() {
  Document d(makeEmptySong());
  const ObjectId ch = d.addChannel("pad", "gl.synth");
  Rig rig(d.song());
  rig.eng.ownNoteOn(ch, 60, 0.9f, OP::ChordPreview);  // a chord preview's note: held, sounding
  rig.run(secs(0.5));
  expect(rig.eng.pluginPlayback().kind == OP::ChordPreview, "chord preview: plugin playback (named the chord preview)");
  const float held = rig.peakFrom(rig.mark() - secs(0.1));
  expect(held > 0.01f, "chord preview: sounds");
  expect(!play(rig), "chord preview: Play stops it");
  const size_t at = rig.mark();
  rig.run(kBlock);
  const float first = rig.peakFrom(at, at + 64);
  expect(first > held * 0.2f, "chord preview: the first samples after the stop still sound (a release, not a cut)");
  rig.run(secs(3.0));
  expect(rig.peakFrom(rig.mark() - secs(0.5)) < 1e-4f, "chord preview: released to silence");
  expect(rig.eng.liveHeld() == 0, "chord preview: its keys released");
  expect(play(rig) && rig.eng.playing(), "chord preview: the next Play starts the transport");
  expect(rig.allocs == 0, "chord preview + Play: no allocation on the audio thread");
}

// 3. Kerf's region play (the editor's own key on a sampler channel), and a
// live key on another channel that must survive the stop.
void testKerf() {
  const std::string tone = writeTone();
  Document d(makeEmptySong());
  const ObjectId kerf = d.addChannel("kerf", "gl.slicer");
  d.editChannel(kerf, "", [&](Channel& c) { c.plugin.data = tone; });
  const ObjectId syn = d.addChannel("lead", "gl.synth");
  Rig rig(d.song());
  rig.eng.noteOn(syn, 64, 0.8f);  // held live by the user
  rig.run(secs(0.2));
  expect(!rig.eng.pluginPlaybackActive(), "live key only: no plugin playback");
  RunLatch latch;
  rig.eng.ownNoteOn(kerf, 60, 0.9f);
  latch.start(rig.eng);
  rig.run(secs(0.3));
  auto rt = rig.builder.channelRuntime(kerf);
  const bool sounding = rt && rt->sounding.load();
  expect(sounding, "Kerf: the region sounds");
  if (sounding) {
    const AudioEngine::OwnPlayback own = rig.eng.pluginPlayback();
    expect(own.kind == OP::EditorKey && own.id == kerf, "Kerf: plugin playback (the editor's key, its channel)");
    expect(!play(rig) && !rig.eng.playing(), "Kerf: Play stops it, the transport stays stopped");
    expect(latch.sync(rig.eng, true) == RunLatch::End::Stopped, "Kerf: the deck's Play button shows stopped");
    rig.run(kBlock);
    expect(rig.eng.liveHeld() == 1, "Kerf: its key released, the live key on the other channel still held");
    expect(!rig.eng.pluginPlaybackActive(), "Kerf: no plugin playback any more");
  }
  // An editor key on a channel that fell silent (the region ended) is not playback.
  rig.eng.noteOff(syn, 64);
  rig.run(secs(3.0));
  const ObjectId kick = d.addChannel("kick", "gl.kick");
  rig.eng.setProgram(rig.builder.build(d.song()));
  rig.eng.ownNoteOn(kick, 60, 0.9f);
  rig.run(secs(3.0));
  expect(!rig.eng.pluginPlaybackActive(), "a one-shot that ended under its held editor key: not playback (Play plays)");
  rig.eng.noteOff(kick, 60);
  expect(play(rig) && rig.eng.playing(), "live key only / ended one-shot: Play starts the transport");
  expect(rig.allocs == 0, "Kerf + Play: no allocation on the audio thread");
}

// 4. Previews: the browser's is not plugin playback; Loupe's own is.
void testPreviews() {
  Document d(makeEmptySong());
  Rig rig(d.song());
  auto smp = std::make_shared<Sample>();
  smp->sampleRate = kSr;
  smp->left.resize(static_cast<size_t>(secs(4.0)));
  for (size_t i = 0; i < smp->left.size(); ++i) smp->left[i] = 0.3f * static_cast<float>(std::sin(static_cast<double>(i) * 0.02));
  smp->setMono();
  rig.eng.preview(smp);  // the browser
  rig.run(kBlock * 2);
  expect(rig.eng.previewing() && !rig.eng.pluginPlaybackActive(), "browser preview: not plugin playback");
  expect(play(rig) && rig.eng.playing(), "browser preview: Play starts the transport");
  rig.run(kBlock);
  expect(rig.eng.previewing(), "browser preview: plays on over the song (as before)");
  rig.eng.stopPressed();
  rig.run(kBlock);
  expect(!rig.eng.previewing(), "browser preview: Stop ends it (J-109)");

  rig.eng.preview(smp, true);  // Loupe standalone
  rig.run(kBlock * 2);
  expect(rig.eng.pluginPlayback().kind == OP::EditorPreview, "Loupe standalone: plugin playback");
  const uint32_t ep = rig.eng.stopEpoch();
  expect(!play(rig) && !rig.eng.playing(), "Loupe standalone: Play stops it");
  expect(!rig.eng.previewing() && rig.eng.stopEpoch() != ep, "Loupe standalone: the preview stopped, the epoch moved");
  rig.run(kBlock);
  expect(play(rig) && rig.eng.playing(), "Loupe standalone: the next Play starts the transport");
  rig.eng.stopPressed();
  rig.run(kBlock);
  // A browser preview started after Loupe's: the browser's (not playback).
  rig.eng.preview(smp, true);
  rig.eng.preview(smp);
  rig.run(kBlock);
  expect(!rig.eng.pluginPlaybackActive(), "a browser preview replacing Loupe's: not plugin playback");
  rig.eng.preview(nullptr);
  expect(rig.allocs == 0, "previews + Play: no allocation on the audio thread");
}

// 5. Loupe's Play in a mixer slot.
void testLoupeSlot() {
  const std::string tone = writeTone();
  Document d(makeEmptySong());
  const ObjectId t1 = d.song().mixer[1].id;
  d.setSlotPlugin(t1, 0, packs::edison::kType);
  Song s = d.song();
  s.mixer[1].slots[0].plugin.data = tone;  // (a bare path: the audio the editor holds)
  Rig rig(s);
  rig.run(kBlock);
  const auto& prog = rig.eng.program();
  auto* slot = prog && prog->mixer.size() > 1 && !prog->mixer[1].slots.empty() && prog->mixer[1].slots[0].rt
                   ? dynamic_cast<packs::edison::EdisonFx*>(prog->mixer[1].slots[0].rt->fx.get())
                   : nullptr;
  expect(slot != nullptr, "Loupe slot: the slot runtime");
  if (!slot) return;
  slot->play(0, 0, false);
  rig.run(kBlock);
  const AudioEngine::OwnPlayback own = rig.eng.pluginPlayback();
  expect(own.kind == OP::Effect && own.id == t1 && own.slot == 0, "Loupe slot: plugin playback (the effect, its track and slot)");
  expect(!play(rig) && !rig.eng.playing(), "Loupe slot: Play stops it, the transport stays stopped");
  rig.run(kBlock);
  expect(!slot->isPlaying(), "Loupe slot: the editor's Play button shows stopped");
  expect(play(rig) && rig.eng.playing(), "Loupe slot: the next Play starts the transport");
  expect(rig.allocs == 0, "Loupe slot + Play: no allocation on the audio thread");
}

// 6. Shuttle free-running with Hold on: the held bar is plugin playback; Play
// ends it; with Hold still on, the (silent) hold is not playback: Play plays.
void testShuttle() {
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
  expect(!rig.eng.pluginPlaybackActive(), "Shuttle: free-running without Hold: not playback");
  tg.params[static_cast<size_t>(pp::TgHold)] = 1;
  rig.eng.setProgram(rig.builder.build(s));
  rig.run(secs(3.0));
  expect(rig.peakFrom(rig.mark() - secs(2.0)) > 0.05f, "Shuttle: the held bar repeats while stopped");
  rig.allocs = 0;  // (the kick's first blocks into Shuttle allocate: Q-18; counted from here, as test_j110)
  const AudioEngine::OwnPlayback own = rig.eng.pluginPlayback();
  expect(own.kind == OP::Effect && own.id == t1, "Shuttle: plugin playback (the effect)");
  expect(!play(rig) && !rig.eng.playing(), "Shuttle: Play stops it, the transport stays stopped");
  rig.run(secs(3.0));
  expect(rig.peakFrom(rig.mark() - secs(2.5)) < 1e-4f, "Shuttle: the bar repeat ended");
  expect(!rig.eng.pluginPlaybackActive(), "Shuttle: Hold still on but silent: not playback");
  expect(play(rig) && rig.eng.playing(), "Shuttle: the next Play starts the transport");
  expect(rig.allocs == 0, "Shuttle + Play: no allocation on the audio thread");
}

// 7. Nothing at all: Play plays (and the two-stage Stop is untouched by a plugin stop).
void testPlain() {
  Document d(makeEmptySong());
  Rig rig(d.song());
  rig.run(kBlock);
  expect(!rig.eng.pluginPlaybackActive(), "nothing sounding: no plugin playback");
  const uint32_t ep = rig.eng.stopEpoch();
  expect(play(rig) && rig.eng.playing() && rig.eng.stopEpoch() == ep, "nothing sounding: Play starts the transport, epoch unchanged");
  // stopPluginPlayback is not a Stop: it does not arm the panic.
  rig.eng.stopPressed();  // first Stop
  rig.run(kBlock);
  const uint32_t panics = rig.eng.panicsDone();
  rig.eng.stopPluginPlayback();
  rig.run(kBlock);
  expect(rig.eng.panicsDone() == panics, "a plugin stop is no panic");
}

}  // namespace

int main() {
  testSeltmann();
  testNoHardCut();
  testKerf();
  testPreviews();
  testLoupeSlot();
  testShuttle();
  testPlain();
  return finish();
}
