// Gridline: K-249, the audio clip editor's settings (engine/ClipFields.h) and the pointer gestures it
// shares with the playlist (engine/ClipGestures.h). Checks:
//  * every field reads back what it set, within its range;
//  * several selected clips: a level or a fade moves each by the same amount, a switch sets them all,
//    the speed scales each by the same ratio; one undo step brings every clip back;
//  * a held drag (one merge key) is one undo step;
//  * speed: the clip's length and its pitch follow (a record played faster);
//  * the shared gestures give the clip the playlist's tools give (trim, resize, slip; fade lengths
//    never cross the other fade; the curve's detent at the plain curve).
#include <cmath>
#include <string>
#include "Check.h"
#include "Hermetic.h"
#include "core/ClipGain.h"
#include "core/Document.h"
#include "core/Fades.h"
#include "core/Song.h"
#include "engine/ClipFields.h"
#include "engine/ClipGestures.h"
#include "engine/PlaylistTools.h"

using namespace gl;
namespace cf = gl::clipfields;

namespace {

bool near(double a, double b, double tol = 1e-4) { return std::fabs(a - b) <= tol; }

// A song with one sample channel and three audio clips of it.
Document makeDoc() {
  Document d;
  d.editSong("setup", [](Song& s) {
    Channel ch;
    ch.id = 10;
    ch.name = "Loop";
    ch.plugin.type = "gl.sampler";
    s.channels.push_back(ch);
    for (int i = 0; i < 3; ++i) {
      Clip c;
      c.id = static_cast<ObjectId>(20 + i);
      c.kind = ClipKind::Audio;
      c.source = 10;
      c.track = i;
      c.start = 0;
      c.length = 384;
      c.gain = 1.0f;
      s.playlist.push_back(c);
    }
    s.nextId = 100;
  });
  return d;
}

const Clip& clip(const Document& d, ObjectId id) { return *d.song().clip(id); }

void testSingle() {
  Document d = makeDoc();
  expect(cf::set(d, {20}, 20, cf::Field::GainDb, -6.0, 0), "gain set");
  expect(near(cf::get(d.song(), clip(d, 20), cf::Field::GainDb), -6.0, 1e-3), "gain reads back -6 dB");
  expect(near(clip(d, 21).gain, 1.0), "the other clips keep their gain");
  cf::set(d, {20}, 20, cf::Field::GainDb, 99.0, 0);
  expect(near(cf::get(d.song(), clip(d, 20), cf::Field::GainDb), clipgain::kMaxDb, 1e-3), "gain clamps at the top of the range");
  cf::set(d, {20}, 20, cf::Field::FadeIn, 0.25, 0);
  expect(near(clip(d, 20).fadeIn, 0.25) && clip(d, 20).fadeInManual, "fade in set by hand");
  cf::set(d, {20}, 20, cf::Field::FadeOutShape, 1, 0);
  expect(clip(d, 20).fadeOutShape == 1, "fade out shape straight");
  cf::set(d, {20}, 20, cf::Field::FadeInCurve, 0.5, 0);
  expect(near(fadeTensionToUi(clip(d, 20).fadeInTension), 0.5, 1e-3), "fade curve reads back");
  cf::set(d, {20}, 20, cf::Field::Semitones, 7, 0);
  cf::set(d, {20}, 20, cf::Field::Cents, -20, 0);
  expect(near(clip(d, 20).pitch, 680.0f) && near(cf::get(d.song(), clip(d, 20), cf::Field::Semitones), 7) &&
             near(cf::get(d.song(), clip(d, 20), cf::Field::Cents), -20),
         "semitones and cents are kept apart");
  cf::set(d, {20}, 20, cf::Field::Mute, 1, 0);
  expect(clip(d, 20).muted, "mute");
  cf::set(d, {20}, 20, cf::Field::Reverse, 1, 0);
  expect(clip(d, 20).reverse, "reverse");
}

void testMulti() {
  Document d = makeDoc();
  cf::set(d, {21}, 21, cf::Field::GainDb, -3.0, 0);  // the second clip starts 3 dB lower
  const size_t before = d.history().position();
  expect(cf::set(d, {20, 21, 22}, 20, cf::Field::GainDb, -6.0, 0), "gain on three clips");
  expect(d.history().position() == before + 1, "one undo step for three clips");
  expect(near(cf::get(d.song(), clip(d, 20), cf::Field::GainDb), -6.0, 1e-3) && near(cf::get(d.song(), clip(d, 21), cf::Field::GainDb), -9.0, 1e-3) &&
             near(cf::get(d.song(), clip(d, 22), cf::Field::GainDb), -6.0, 1e-3),
         "each clip moves by the same amount (the anchor to the value)");
  cf::set(d, {20, 21, 22}, 21, cf::Field::Mute, 1, 0);
  expect(clip(d, 20).muted && clip(d, 21).muted && clip(d, 22).muted, "a switch sets every selected clip");
  d.undo();
  d.undo();
  expect(near(cf::get(d.song(), clip(d, 21), cf::Field::GainDb), -3.0, 1e-3) && near(clip(d, 20).gain, 1.0) && !clip(d, 20).muted,
         "undo brings every clip back");
  // A held drag: the same merge key is one step.
  const size_t b2 = d.history().position();
  for (int i = 1; i <= 10; ++i) cf::set(d, {20, 22}, 20, cf::Field::FadeOut, 0.01 * i, 777);
  d.endGesture();
  expect(d.history().position() == b2 + 1, "a drag of ten moves is one undo step");
  expect(near(clip(d, 22).fadeOut, 0.10, 1e-9), "the other clip followed the drag");
}

void testSpeed() {
  Document d = makeDoc();
  const Tick len0 = d.song().clipLength(clip(d, 20));
  expect(cf::set(d, {20}, 20, cf::Field::Speed, 200.0, 0), "speed 200 %");
  const Clip& c = clip(d, 20);
  expect(near(cf::get(d.song(), c, cf::Field::Speed), 200.0, 0.01), "speed reads back");
  expect(near(c.pitch, 1200.0f, 0.01), "an octave up (the pitch follows)");
  const Tick len1 = d.song().clipLength(c);
  std::printf("# length %lld -> %lld\n", static_cast<long long>(len0), static_cast<long long>(len1));
  expect(std::llabs(static_cast<long long>(len1) - static_cast<long long>(len0) / 2) <= 1, "half as long");
}

void testGestures() {
  Document d = makeDoc();
  const Song& s = d.song();
  TempoMap tm;
  tm.build(s);
  const Clip c = clip(d, 20);
  const Clip t = clipgest::trimStart(s, tm, c, 96, false, false);
  const std::vector<Clip> ref = pltools::trimStarts(s, tm, {c}, {c.id}, 96, false, false);
  expect(!ref.empty() && t == ref[0], "trim start: the playlist's own result");
  const Clip e = clipgest::trimEnd(s, tm, c, -96, false, false);
  const std::vector<Clip> ref2 = pltools::resizeEnds(s, {c}, {c.id}, -96, false, &tm, false);
  expect(!ref2.empty() && e == ref2[0], "trim end: the playlist's own result");
  expect(clipgest::slip(s, tm, c, 48, false) == pltools::slipClip(s, tm, c, 48, false), "slip: the playlist's own result");
  expect(near(clipgest::fadeLength(1, 0.3, 1.0, 0.0), 0.3), "fade in: as long as the drag");
  expect(near(clipgest::fadeLength(1, 0.9, 1.0, 0.4), 0.6), "a fade in never runs into the fade out");
  expect(near(clipgest::fadeLength(2, 0.75, 1.0, 0.0), 0.25), "fade out: from the drag to the end");
  expect(near(clipgest::fadeLength(2, -1.0, 1.0, 0.2), 0.8), "a fade out never runs into the fade in");
  expect(clipgest::fadeCurveUi(0.0f, 1.0f) == 0.0f, "the curve's detent at the plain curve");
  expect(clipgest::fadeCurveUi(0.0f, -35.0f) > 0.4f, "dragging up bends the curve louder");
  expect(clipgest::fadeCurveUi(0.9f, -700.0f) == 1.0f, "the curve stops at the end of its range");
  expect(near(clipgest::fadeCurve(0.5f, 0.0f), fadeTensionFromUi(0.5f)), "the curve's tension in the stored unit");
}

}  // namespace

int main() {
  gl::test::hermeticTestEnv("k249f");
  testSingle();
  testMulti();
  testSpeed();
  testGestures();
  return finish();
}
