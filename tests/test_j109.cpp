// Gridline — J-109: Stop silences everything that sounds outside the song.
//  * first Stop: the transport stops and every sounding note is released with
//    its normal tail — preview / typed keys, pads, MIDI input (sustain pedal
//    down too), note processor output (arpeggiator) — and the sample preview
//    stops;
//  * Stop again while stopped (panic): a short declick fade, then silence at
//    once, delay / reverb tails included, without a click;
//  * a focus loss releases the notes held from the interface (not the MIDI
//    keyboard's);
//  * the audio thread allocates nothing for any of it.
// (Hosted plugins: tests/test_hosting.cpp, testStopReleasesHosted.)
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <vector>
#include "Check.h"
#include "core/Document.h"
#include "core/PluginParams.h"
#include "core/Song.h"
#include "engine/AudioEngine.h"
#include "engine/Program.h"
#include "engine/Sample.h"

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

namespace {

constexpr double kSr = 48000.0;
constexpr int kBlock = 256;

// An engine playing a document, recording everything it renders.
struct Rig {
  ProgramBuilder builder;
  AudioEngine eng{kSr};
  std::vector<float> L, R;
  std::vector<float> bl = std::vector<float>(kBlock), br = std::vector<float>(kBlock);
  long allocs = 0;
  explicit Rig(const Song& s) {
    builder.setSampleRate(kSr);
    eng.setProgram(builder.build(s));
    L.reserve(1 << 22);
    R.reserve(1 << 22);
  }
  // Renders `frames`, counting allocations on this (the audio) thread.
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
  float peak(size_t from, size_t to) const {
    float p = 0;
    for (size_t i = from; i < std::min(to, L.size()); ++i) p = std::max(p, std::max(std::fabs(L[i]), std::fabs(R[i])));
    return p;
  }
  float peakFrom(size_t from) const { return peak(from, L.size()); }
  // Largest sample-to-sample step in [from, to) (clicks).
  float maxStep(size_t from, size_t to) const {
    float m = 0;
    for (size_t i = std::max<size_t>(1, from); i < std::min(to, L.size()); ++i)
      m = std::max(m, std::max(std::fabs(L[i] - L[i - 1]), std::fabs(R[i] - R[i - 1])));
    return m;
  }
};

int secs(double s) { return static_cast<int>(s * kSr); }

// 1. A preview key held (the piano roll's keyboard), then Stop.
void testPreviewNote() {
  Document d(makeEmptySong());
  const ObjectId ch = d.addChannel("synth", "gl.synth");
  Rig rig(d.song());
  rig.eng.noteOn(ch, 60, 0.9f);  // held: no note-off ever comes
  rig.run(secs(0.5));
  expect(rig.peakFrom(rig.mark() - secs(0.1)) > 0.01f, "preview key: the held note sounds while stopped");
  expect(rig.eng.liveHeld() == 1, "preview key: the engine knows it is held");
  // Before J-109 Stop did nothing to it: the note sounded forever.
  expect(!rig.eng.stopPressed(), "first Stop: a release, not a panic");
  const size_t at = rig.mark();
  rig.run(secs(0.004));
  expect(rig.peak(at, rig.mark()) > 0.001f, "first Stop: the note is released, not cut (its release tail plays)");
  rig.run(secs(2.0));
  expect(rig.peakFrom(rig.mark() - secs(0.2)) < 1e-4f, "first Stop: silence after the release tail");
  expect(rig.eng.liveHeld() == 0, "first Stop: no live note held any more");
  expect(rig.allocs == 0, "preview note + Stop: no allocation on the audio thread");
}

// 2. A note into a reverb + delay: first Stop releases (the tails ring on),
// the second Stop silences at once, tails included, without a click.
void testPanicClearsTails() {
  Document d(makeEmptySong());
  const ObjectId ch = d.addChannel("kick", "gl.kick");
  const ObjectId t1 = d.song().mixer[1].id;
  d.routeChannel(ch, t1);
  d.setSlotPlugin(t1, 0, "gl.delay");
  d.setSlotPlugin(t1, 1, "gl.reverb");
  Rig rig(d.song());
  rig.eng.play();
  rig.eng.noteOn(ch, 60, 1.0f);
  rig.run(secs(0.3));
  rig.allocs = 0;  // (only Stop and the panic are measured here)
  rig.eng.noteOff(ch, 60);
  rig.run(secs(0.3));
  expect(!rig.eng.stopPressed(), "tails: first Stop releases");
  const size_t s1 = rig.mark();
  rig.run(secs(0.3));
  const float tail = rig.peak(s1 + secs(0.2), rig.mark());
  expect(tail > 1e-3f, "tails: after the first Stop the delay / reverb tails still ring");
  const float before = rig.maxStep(s1 + secs(0.1), rig.mark());
  expect(rig.eng.stopPressed(), "tails: Stop again while stopped is the panic");
  expect(rig.eng.panicking(), "tails: the panic is on its way");
  const size_t s2 = rig.mark();
  rig.run(secs(0.02));
  const float fadeStep = rig.maxStep(s2, rig.mark());
  expect(fadeStep <= before * 1.5f + 1e-4f, "panic: the fade adds no click (no step larger than the signal's own)");
  expect(rig.peak(s2 + secs(0.006), rig.mark()) == 0.0f, "panic: silent within 6 ms");
  const size_t s3 = rig.mark();
  rig.run(secs(1.0));
  expect(rig.peakFrom(s3) < 1e-7f, "panic: the delay / reverb tails are gone (they do not come back)");
  expect(!rig.eng.panicking() && rig.eng.panicsDone() == 1, "panic: completed once");
  rig.eng.noteOn(ch, 60, 1.0f);
  const size_t s4 = rig.mark();
  rig.run(secs(0.1));
  rig.eng.noteOff(ch, 60);
  expect(rig.peakFrom(s4) > 0.01f, "after a panic the channel plays again");
  expect(rig.allocs == 0, "tails + panic: no allocation on the audio thread");
}

// 3. A pad (drum kit pad: one-shot) and an arpeggiated held key.
void testPadAndArp() {
  Document d(makeEmptySong());
  const ObjectId kit = d.addChannel("kit", "gl.drumkit");
  const ObjectId arp = d.addChannel("arp", "gl.synth");
  d.addNoteFx(arp, "gl.arp");
  Rig rig(d.song());
  rig.eng.noteOn(kit, 60, 0.9f);  // a pad held down
  rig.eng.noteOn(arp, 64, 0.9f);  // a key into the arpeggiator
  rig.run(secs(0.5));
  expect(rig.peakFrom(rig.mark() - secs(0.25)) > 0.01f, "pad + arpeggiator: sounding");
  rig.eng.stopPressed();
  rig.run(secs(3.0));
  expect(rig.peakFrom(rig.mark() - secs(0.25)) < 1e-4f, "pad + arpeggiator: silent after Stop (the arpeggiator stops making notes)");
  expect(rig.allocs == 0, "pad + arpeggiator + Stop: no allocation on the audio thread");
}

// 4. The browser's sample preview.
void testBrowserPreview() {
  Document d(makeEmptySong());
  Rig rig(d.song());
  auto s = std::make_shared<Sample>();
  s->sampleRate = kSr;
  s->left.resize(static_cast<size_t>(secs(10.0)));
  for (size_t i = 0; i < s->left.size(); ++i) s->left[i] = 0.3f * static_cast<float>(std::sin(static_cast<double>(i) * 0.05));
  s->setMono();
  rig.eng.preview(s);
  rig.run(secs(0.2));
  expect(rig.eng.previewing() && rig.peakFrom(rig.mark() - secs(0.1)) > 0.1f, "browser preview: playing");
  rig.eng.stopPressed();
  expect(!rig.eng.previewing(), "browser preview: Stop stops it");
  const size_t at = rig.mark();
  rig.run(secs(0.1));
  expect(rig.peakFrom(at) < 1e-7f, "browser preview: silent after Stop");
  expect(rig.allocs == 0, "browser preview + Stop: no allocation on the audio thread");
}

// 5. MIDI input: a held key with the sustain pedal down; a focus loss does not
// touch it, Stop releases it.
void testMidiAndFocus() {
  Document d(makeEmptySong());
  const ObjectId ch = d.addChannel("synth", "gl.synth");
  Rig rig(d.song());
  rig.eng.setMidiTarget(ch);
  rig.eng.midiEvent(0xB0, 64, 127);  // sustain down
  rig.eng.midiEvent(0x90, 67, 100);
  rig.eng.midiEvent(0x80, 67, 0);    // released, held by the pedal
  rig.eng.noteOn(ch, 60, 0.9f);      // and a typed key
  rig.run(secs(0.3));
  expect(rig.eng.liveHeld() == 1, "focus: one typed key held");
  rig.eng.releaseLiveNotes();  // the window lost focus
  rig.run(secs(0.1));
  expect(rig.eng.liveHeld() == 0, "focus loss: the typed key is released");
  rig.run(secs(2.0));
  expect(rig.peakFrom(rig.mark() - secs(0.2)) > 0.01f, "focus loss: the MIDI keyboard's note (pedal down) keeps sounding");
  rig.eng.stopPressed();
  rig.run(secs(2.0));
  expect(rig.peakFrom(rig.mark() - secs(0.2)) < 1e-4f, "MIDI: Stop releases a note held by the sustain pedal");
  rig.eng.midiEvent(0x90, 69, 100);
  rig.run(secs(0.2));
  rig.eng.midiEvent(0x80, 69, 0);  // the pedal state was reset by Stop: the release ends it
  rig.run(secs(2.0));
  expect(rig.peakFrom(rig.mark() - secs(0.2)) < 1e-4f, "MIDI: after Stop the sustain pedal no longer holds new notes");
  expect(rig.allocs == 0, "MIDI + focus + Stop: no allocation on the audio thread");
}

// 6. The two-stage rule: anything new between two Stops makes the next one a
// release again; playing then stopping twice panics.
void testTwoStages() {
  Document d(makeEmptySong());
  const ObjectId ch = d.addChannel("synth", "gl.synth");
  Rig rig(d.song());
  rig.eng.play();
  rig.run(kBlock);
  expect(!rig.eng.stopPressed(), "two stages: Stop while playing releases");
  rig.run(kBlock);
  rig.eng.noteOn(ch, 60, 0.8f);  // a key pressed after the first Stop
  rig.run(kBlock);
  expect(!rig.eng.stopPressed(), "two stages: a new note since: the next Stop is a release again");
  rig.run(kBlock);
  expect(rig.eng.stopPressed(), "two stages: nothing new since: the panic");
  rig.run(kBlock * 4);
  expect(rig.eng.stopPressed(), "two stages: further Stops keep silencing");
  rig.run(kBlock * 4);
  expect(rig.eng.panicsDone() == 2, "two stages: each panic completes");
  expect(rig.allocs == 0, "two stages: no allocation on the audio thread");
}

}  // namespace

int main() {
  testPreviewNote();
  testPanicClearsTails();
  testPadAndArp();
  testBrowserPreview();
  testMidiAndFocus();
  testTwoStages();
  return finish();
}
