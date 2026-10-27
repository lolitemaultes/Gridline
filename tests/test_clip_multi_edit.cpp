// Gridline: J-142, playlist clip edits across a selection, and the clip header's hit regions.
//  * relative multi-edits (core/ClipMultiEdit.h): dB adds, fade lengths add in time
//    (both ends), curves add on the handle's scale; per-clip clamping that does not
//    stop the others and that a reversing drag undoes; resets and pasted values set
//    every clip; a typed value is an offset from the clicked clip; a drag of many
//    frames is one undo step; a single clip (or a clip outside the selection) is
//    edited alone, as before; pattern clips are not touched;
//  * the volume line's unbounded drag (ClipGain.h Drag::unbounded);
//  * header hit-testing (app/ClipHeaderHit.h): name box, menu arrow, rest of the
//    header, body, outside; the name box follows the drawn text.
#include <cmath>
#include <set>
#include <vector>
#include "../src/app/ClipHeaderHit.h"
#include "../src/core/Automation.h"
#include "../src/core/ClipGain.h"
#include "../src/core/ClipMultiEdit.h"
#include "../src/core/Document.h"
#include "../src/core/Fades.h"
#include "../src/core/PluginInfo.h"
#include "../src/core/Song.h"
#include "Check.h"

using namespace gl;
using clipmulti::Param;

namespace {

bool near(double a, double b, double eps = 1e-3) { return std::fabs(a - b) < eps; }

struct Fixture {
  Song song;
  ObjectId chan = kNoId, pat = kNoId;
  ObjectId a = kNoId, b = kNoId, c = kNoId, p = kNoId;  // three audio clips and a pattern clip
};

ObjectId addAudio(Song& s, ObjectId chan, int track, Tick start, Tick length, double db) {
  Clip k;
  k.id = s.allocateId(), k.kind = ClipKind::Audio, k.source = chan, k.track = track, k.start = start, k.length = length;
  k.gain = clipgain::fromDb(db);
  s.playlist.push_back(k);
  return k.id;
}

Fixture make() {
  Fixture f;
  f.song = makeEmptySong();
  Song& s = f.song;
  Channel ch;
  ch.id = s.allocateId();
  ch.name = "audio";
  ch.plugin = defaultPluginState("gl.sampler");
  ch.mixerTrack = s.masterId();
  ch.audioClip = true;
  s.channels.push_back(ch);
  f.chan = ch.id;
  const Tick bar = s.barTicks();
  f.a = addAudio(s, f.chan, 0, 0, bar * 2, 0.0);
  f.b = addAudio(s, f.chan, 1, 0, bar * 4, -6.0);
  f.c = addAudio(s, f.chan, 2, 0, bar, 30.0);
  if (s.patterns.empty()) {
    Pattern pt;
    pt.id = s.allocateId();
    s.patterns.push_back(pt);
  }
  f.pat = s.patterns[0].id;
  Clip pc;
  pc.id = s.allocateId(), pc.kind = ClipKind::Pattern, pc.pattern = f.pat, pc.track = 3, pc.start = 0, pc.length = bar;
  s.playlist.push_back(pc);
  f.p = pc.id;
  return f;
}

double db(const Song& s, ObjectId id) { return clipmulti::valueOf(s, *s.clip(id), Param::GainDb); }
double dur(const Song& s, ObjectId id) {
  TempoMap tm;
  tm.build(s);
  const Clip& c = *s.clip(id);
  return tm.secondsAt(static_cast<double>(c.start + s.clipLength(c))) - tm.secondsAt(static_cast<double>(c.start));
}

void testTargets() {
  Fixture f = make();
  const Song& s = f.song;
  const std::set<ObjectId> all{f.a, f.b, f.c, f.p};
  const auto t = clipmulti::targets(s, all, f.b, Param::GainDb);
  expect(t.size() == 3 && t[0] == f.a && t[1] == f.b && t[2] == f.c, "targets: every selected audio clip, in playlist order (not the pattern clip)");
  expect(clipmulti::targets(s, {f.a, f.b}, f.c, Param::GainDb) == std::vector<ObjectId>{f.c}, "targets: a clip outside the selection is edited alone");
  expect(clipmulti::targets(s, {f.a}, f.a, Param::GainDb) == std::vector<ObjectId>{f.a}, "targets: a selection of one is a single edit");
  expect(clipmulti::targets(s, {f.a, f.p}, f.a, Param::GainDb) == std::vector<ObjectId>{f.a}, "targets: a pattern clip has no level: one target");
  expect(!clipmulti::isMulti(s, {f.a, f.p}, f.a, Param::GainDb) && clipmulti::isMulti(s, all, f.a, Param::FadeIn), "isMulti");
  expect(clipmulti::targets(s, all, f.p, Param::GainDb).empty(), "targets: an edit on a pattern clip reaches no audio clip");
}

void testGainRelative() {
  Fixture f = make();
  Document d(f.song);
  const std::set<ObjectId> sel{f.a, f.b, f.c};
  const auto snap = clipmulti::capture(d.song(), clipmulti::targets(d.song(), sel, f.a, Param::GainDb), Param::GainDb, f.a);
  expect(snap.clips.size() == 3 && near(snap.find(f.b)->base, -6.0), "gain: captured bases in dB");
  const size_t h0 = d.history().position();
  // A drag over several frames: +1, +2 dB (each frame from the captured values, one merge key).
  clipmulti::offset(d, "clip volume", snap, 1.0, 77);
  clipmulti::offset(d, "clip volume", snap, 2.0, 77);
  const Song& s = d.song();
  expect(near(db(s, f.a), 2.0) && near(db(s, f.b), -4.0) && near(db(s, f.c), 32.0), "gain: +2 dB adds 2 dB to each clip's own value");
  expect(d.history().position() == h0 + 1, "gain: one drag (many frames) is one undo step");
  // Past the top: the loud clip stops at +36 dB, the others go on.
  clipmulti::offset(d, "clip volume", snap, 8.0, 77);
  expect(near(db(d.song(), f.c), clipgain::kMaxDb) && near(db(d.song(), f.a), 8.0) && near(db(d.song(), f.b), 2.0),
         "gain: clamped per clip at +36 dB without stopping the others");
  clipmulti::offset(d, "clip volume", snap, 2.0, 77);  // the drag comes back
  expect(near(db(d.song(), f.c), 32.0), "gain: a clamped clip follows a reversing drag back (offset from the captured value)");
  expect(d.history().position() == h0 + 1, "gain: still one undo step");
  d.undo();
  expect(near(db(d.song(), f.a), 0.0) && near(db(d.song(), f.b), -6.0) && near(db(d.song(), f.c), 30.0), "gain: one undo restores every clip");
  // Down to silence and back.
  clipmulti::offset(d, "clip volume", snap, -200.0, 78);
  expect(clipgain::silent(d.song().clip(f.a)->gain) && clipgain::silent(d.song().clip(f.b)->gain) && clipgain::silent(d.song().clip(f.c)->gain),
         "gain: far down every clip is silent");
  clipmulti::offset(d, "clip volume", snap, -1.0, 78);
  expect(near(db(d.song(), f.a), -1.0) && near(db(d.song(), f.c), 29.0), "gain: and comes back from silence");
  d.endGesture();
  // Reset: every clip to 0 dB; a pasted value: every clip to it.
  clipmulti::setAll(d, "reset clip volume", snap, clipmulti::defaultOf(Param::GainDb));
  expect(d.song().clip(f.a)->gain == 1.0f && d.song().clip(f.b)->gain == 1.0f && d.song().clip(f.c)->gain == 1.0f, "gain: reset sets every clip to 0 dB");
  clipmulti::setAll(d, "paste clip volume", clipmulti::capture(d.song(), {f.a, f.b}, Param::GainDb, f.a), -3.0);
  expect(near(db(d.song(), f.a), -3.0) && near(db(d.song(), f.b), -3.0) && near(db(d.song(), f.c), 0.0), "gain: a pasted value sets the targets only");
  // A typed value is an offset from the clicked clip: B typed to -9 dB (from -3): A moves -6 too.
  d.editClip(f.a, "x", [](Clip& x) { x.gain = clipgain::fromDb(1.0); });
  const auto s2 = clipmulti::capture(d.song(), {f.a, f.b}, Param::GainDb, f.b);
  clipmulti::offset(d, "clip volume", s2, -9.0 - s2.find(f.b)->base);
  expect(near(db(d.song(), f.b), -9.0) && near(db(d.song(), f.a), -5.0), "gain: a typed value applies as an offset from the clicked clip's value");
}

void testSingleUnchanged() {
  Fixture f = make();
  Document d(f.song);
  const auto snap = clipmulti::capture(d.song(), clipmulti::targets(d.song(), {f.b}, f.b, Param::GainDb), Param::GainDb, f.b);
  clipmulti::offset(d, "clip volume", snap, 3.0);
  expect(near(db(d.song(), f.b), -3.0) && near(db(d.song(), f.a), 0.0) && near(db(d.song(), f.c), 30.0), "single: only the clicked clip moves");
  // The pattern clip is never edited.
  const Clip before = *d.song().clip(f.p);
  const auto sa = clipmulti::capture(d.song(), {f.a, f.b, f.c, f.p}, Param::GainDb, f.a);
  clipmulti::offset(d, "clip volume", sa, 1.0);
  expect(*d.song().clip(f.p) == before && sa.clips.size() == 3, "single: a selected pattern clip is left alone");
}

void testPitch() {
  Fixture f = make();
  Document d(f.song);
  d.editClip(f.b, "x", [](Clip& x) { x.pitch = 4700.0f; });
  const auto snap = clipmulti::capture(d.song(), {f.a, f.b}, Param::Pitch, f.a);
  clipmulti::offset(d, "clip pitch", snap, 200.0);
  expect(near(d.song().clip(f.a)->pitch, 200.0) && near(d.song().clip(f.b)->pitch, 4800.0), "pitch: cents add, clamped per clip at +4800");
}

void testFades() {
  Fixture f = make();
  Document d(f.song);
  d.editClip(f.a, "x", [](Clip& x) { x.fadeIn = 0.1, x.fadeInManual = true, x.fadeOut = 0.2, x.fadeOutManual = true; });
  d.editClip(f.b, "x", [](Clip& x) { x.fadeIn = 0.5, x.fadeInManual = true; });
  const double durA = dur(d.song(), f.a), durB = dur(d.song(), f.b);
  expect(durA > 1.0 && durB > durA, "fades: fixture lengths");
  const std::set<ObjectId> sel{f.a, f.b};
  // Fade-in.
  const auto in = clipmulti::capture(d.song(), clipmulti::targets(d.song(), sel, f.a, Param::FadeIn), Param::FadeIn, f.a);
  expect(near(in.find(f.a)->hi, durA - 0.2) && near(in.find(f.b)->hi, durB), "fades: each clip's limit is its own length minus its other fade");
  const size_t h0 = d.history().position();
  clipmulti::offset(d, "clip fade in", in, 0.1, 91);
  clipmulti::offset(d, "clip fade in", in, 0.2, 91);
  expect(near(d.song().clip(f.a)->fadeIn, 0.3) && near(d.song().clip(f.b)->fadeIn, 0.7), "fade in: +0.2 s adds to each clip's own length");
  expect(d.history().position() == h0 + 1, "fade in: one drag is one undo step");
  clipmulti::offset(d, "clip fade in", in, 100.0, 91);
  expect(near(d.song().clip(f.a)->fadeIn, durA - 0.2) && near(d.song().clip(f.b)->fadeIn, durB), "fade in: clamped per clip to its own length");
  clipmulti::offset(d, "clip fade in", in, -0.3, 91);
  expect(d.song().clip(f.a)->fadeIn == 0.0 && near(d.song().clip(f.b)->fadeIn, 0.2), "fade in: clamped at zero per clip, the others go on");
  d.endGesture();
  d.undo();
  expect(near(d.song().clip(f.a)->fadeIn, 0.1) && near(d.song().clip(f.b)->fadeIn, 0.5), "fade in: undo restores both clips");
  // Fade-out (B had none: its automatic length 0 is the base, then stored by hand).
  const auto out = clipmulti::capture(d.song(), clipmulti::targets(d.song(), sel, f.b, Param::FadeOut), Param::FadeOut, f.b);
  clipmulti::offset(d, "clip fade out", out, 0.25, 92);
  expect(near(d.song().clip(f.a)->fadeOut, 0.45) && near(d.song().clip(f.b)->fadeOut, 0.25) && d.song().clip(f.b)->fadeOutManual,
         "fade out: +0.25 s adds to each (a clip with no fade gets one, set by hand)");
  clipmulti::offset(d, "clip fade out", out, 1e6, 92);
  expect(near(d.song().clip(f.a)->fadeOut, durA - 0.1) && near(d.song().clip(f.b)->fadeOut, durB - 0.5), "fade out: clamped to the length minus the fade-in");
  d.endGesture();
  // Reset of a fade length (the menu's "No fade"): every clip.
  clipmulti::setAll(d, "remove fade", out, 0.0);
  expect(d.song().clip(f.a)->fadeOut == 0.0 && d.song().clip(f.b)->fadeOut == 0.0, "fade out: a reset reaches every clip");
}

void testCurves() {
  Fixture f = make();
  Document d(f.song);
  d.editClip(f.a, "x", [](Clip& x) { x.fadeIn = 0.3, x.fadeInManual = true, x.fadeInTension = fadeTensionFromUi(0.5f); });
  d.editClip(f.b, "x", [](Clip& x) { x.fadeIn = 0.3, x.fadeInManual = true, x.fadeInTension = fadeTensionFromUi(-0.2f); });
  const auto snap = clipmulti::capture(d.song(), {f.a, f.b}, Param::FadeInCurve, f.b);
  clipmulti::offset(d, "fade curve", snap, 0.25, 0, 0.03);
  expect(near(fadeTensionToUi(d.song().clip(f.a)->fadeInTension), 0.75) && near(fadeTensionToUi(d.song().clip(f.b)->fadeInTension), 0.05),
         "curve: the bend adds on the handle's scale");
  clipmulti::offset(d, "fade curve", snap, 0.21, 0, 0.03);
  expect(d.song().clip(f.b)->fadeInTension == 0.0f && near(fadeTensionToUi(d.song().clip(f.a)->fadeInTension), 0.71),
         "curve: the clicked clip keeps the detent at the plain curve, the others do not snap");
  clipmulti::offset(d, "fade curve", snap, 0.9, 0, 0.03);
  expect(near(fadeTensionToUi(d.song().clip(f.a)->fadeInTension), 1.0) && near(fadeTensionToUi(d.song().clip(f.b)->fadeInTension), 0.7),
         "curve: clamped per clip at the end of the scale");
  clipmulti::setAll(d, "plain fade curve", snap, 0.0);
  expect(d.song().clip(f.a)->fadeInTension == 0.0f && d.song().clip(f.b)->fadeInTension == 0.0f, "curve: reset reaches every clip");
}

void testCrossfadeMirror() {
  // Two overlapping clips on one track (automatic crossfade, tied): bending the
  // later clip's fade-in mirrors onto the earlier clip's fade-out.
  Fixture f = make();
  Song& s = f.song;
  const Tick bar = s.barTicks();
  const ObjectId x = addAudio(s, f.chan, 5, 0, bar * 2, 0.0);
  const ObjectId y = addAudio(s, f.chan, 5, bar, bar * 2, 0.0);
  Document d(s);
  const auto snap = clipmulti::capture(d.song(), {y, f.a}, Param::FadeInCurve, y);
  expect(snap.find(y) && snap.find(y)->mirror == x, "curve: the tied crossfade partner is found");
  clipmulti::offset(d, "fade curve", snap, 0.4);
  expect(near(d.song().clip(x)->fadeOutTension, -d.song().clip(y)->fadeInTension, 1e-4) && d.song().clip(y)->fadeInTension > 0,
         "curve: the partner's opposite edge mirrors its clip");
}

void testUnboundedDrag() {
  clipgain::Drag g;
  g.unbounded = true;
  g.begin(30.0, false, true);  // free: no detent
  g.move(100.0);               // 100 px up = +20 dB
  expect(near(g.db(), clipgain::kMaxDb) && near(g.dbUnbounded(), 50.0), "drag: unbounded keeps following the pointer past +36 dB (db() stays in range)");
  g.move(0.0);
  expect(near(g.db(), 30.0), "drag: and comes back at once");
  clipgain::Drag h;  // the usual drag still stops at the top
  h.begin(30.0, false, true);
  h.move(100.0);
  expect(near(h.dbUnbounded(), clipgain::kMaxDb), "drag: a single clip's drag still stops at +36 dB");
}

void testHeaderHit() {
  using namespace clipheader;
  Geom g;
  g.x0 = 100, g.x1 = 300, g.y0 = 50, g.y1 = 110, g.headH = 15;
  g.setMenu(107);
  g.setName(115, 40, 160);
  expect(g.hasMenu && g.hasName && g.nameX0 >= g.menuX1 && near(g.nameX1, 158), "geom: the name box spans the drawn text plus padding");
  expect(hit(g, 107, 57) == Zone::Menu, "hit: the arrow");
  expect(hit(g, 130, 57) == Zone::Name, "hit: the name box");
  expect(hit(g, 157, 51) == Zone::Name, "hit: the name box's padding");
  expect(hit(g, 160, 57) == Zone::Header, "hit: just past the name: header");
  expect(hit(g, 299, 57) == Zone::Header, "hit: the far end of the header");
  expect(hit(g, 130, 65) == Zone::Body && hit(g, 200, 100) == Zone::Body, "hit: the body (the strip ends at headH)");
  expect(hit(g, 99, 57) == Zone::None && hit(g, 300, 57) == Zone::None && hit(g, 150, 110) == Zone::None, "hit: outside");
  expect(opensMenu(Zone::Name) && opensMenu(Zone::Menu) && !opensMenu(Zone::Header) && !opensMenu(Zone::Body) && !opensMenu(Zone::None),
         "right-click: name and arrow open the menu, the rest of the header and the body delete");
  Geom longName = g;
  longName.setName(115, 500, 160);  // cut with an ellipsis: the box ends where the text is cut
  expect(near(longName.nameX1, 278) && hit(longName, 270, 57) == Zone::Name && hit(longName, 285, 57) == Zone::Header,
         "geom: a long name's box ends at the room it is drawn in");
  Geom narrow;
  narrow.x0 = 0, narrow.x1 = 20, narrow.y0 = 0, narrow.y1 = 40, narrow.headH = 12;  // no arrow, no name drawn
  expect(hit(narrow, 10, 5) == Zone::Header && !opensMenu(hit(narrow, 10, 5)), "hit: a clip too narrow for its title: the whole strip deletes");
  Geom noArrow;
  noArrow.x0 = 0, noArrow.x1 = 200, noArrow.y0 = 0, noArrow.y1 = 40, noArrow.headH = 12;
  noArrow.setName(6, 30, 180);
  expect(hit(noArrow, 4, 5) == Zone::Name && hit(noArrow, 2, 5) == Zone::Header && hit(noArrow, 38, 5) == Zone::Name && hit(noArrow, 40, 5) == Zone::Header,
         "hit: without the arrow the name box starts at the text");
  Geom empty;
  empty.x0 = 0, empty.x1 = 200, empty.y0 = 0, empty.y1 = 40, empty.headH = 12;
  empty.setName(6, 0, 180);
  expect(!empty.hasName, "geom: an empty name has no box");
}

}  // namespace

int main() {
  testTargets();
  testGainRelative();
  testSingleUnchanged();
  testPitch();
  testFades();
  testCurves();
  testCrossfadeMirror();
  testUnboundedDrag();
  testHeaderHit();
  return finish();
}
