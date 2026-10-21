// Q-4: piano roll event lane — paint sweep (fast drags hit every note, chords,
// very short notes, selection only) and the right-drag line tool (exact values,
// chords, selection, every note property), one undo step per drag.
#include <algorithm>
#include <cmath>
#include <vector>
#include "Check.h"
#include "core/Document.h"
#include "core/NoteLaneEdit.h"
#include "core/NoteTools.h"
#include "core/Project.h"

using namespace gl;
using tools::NoteProp;

namespace {

std::vector<Note> row(int count, Tick step, Tick len, int chord = 1, int vel = 64) {
  std::vector<Note> v;
  for (int i = 0; i < count; ++i)
    for (int k = 0; k < chord; ++k) v.push_back(Note(static_cast<Tick>(i) * step, len, 60 + 4 * k, vel));
  return v;
}

// Velocity of the normalised value v, as the lane writes it.
int vel(double v) { return static_cast<int>(std::lround(v * 127)); }

void testSegmentValue() {
  expect(std::fabs(notelane::segmentValue(0, 0.0, 100, 1.0, 25) - 0.25) < 1e-12, "segment value: quarter way");
  expect(std::fabs(notelane::segmentValue(100, 1.0, 0, 0.0, 25) - 0.25) < 1e-12, "segment value: either direction");
  expect(notelane::segmentValue(0, 0.2, 100, 0.8, -50) == 0.2 && notelane::segmentValue(0, 0.2, 100, 0.8, 500) == 0.8, "segment value: clamped");
  expect(notelane::segmentValue(40, 0.3, 40, 0.9, 40) == 0.9, "segment value: a vertical segment gives the end value");
}

void testFastPaintHitsEveryNote() {
  // 32 sixteenth notes; a drag crossing the whole row in only 3 frames.
  auto n = row(32, 24, 12);
  const double xs[] = {10, 300, 650, 790};
  const double vs[] = {0.1, 0.4, 0.7, 0.9};
  for (int f = 1; f < 4; ++f) notelane::paintSegment(n, {}, NoteProp::Velocity, xs[f - 1], vs[f - 1], xs[f], vs[f]);
  bool all = true, exact = true;
  for (const auto& x : n) {
    all = all && x.velocity != 64;
    // The last frame whose segment reaches the stem's cell (half way to the
    // neighbouring stems, 12 ticks) decides.
    int seg = 1;
    const double cLo = x.start == 0 ? -1e9 : x.start - 12.0, cHi = x.start == 744 ? 1e9 : x.start + 12.0;
    for (int f = 1; f < 4; ++f)
      if (cHi >= xs[f - 1] && cLo <= xs[f]) seg = f;
    double v = notelane::segmentValue(xs[seg - 1], vs[seg - 1], xs[seg], vs[seg], static_cast<double>(x.start));
    exact = exact && x.velocity == std::clamp(vel(v), 1, 127);
  }
  expect(all, "fast paint: every note crossed in 3 frames changed");
  expect(exact, "fast paint: each note gets the value at its x along the drag path");

  // Backwards (right to left) drag.
  auto b = row(16, 24, 12);
  notelane::paintSegment(b, {}, NoteProp::Velocity, 400, 1.0, 0, 0.0);
  bool allB = true;
  for (const auto& x : b) allB = allB && x.velocity == std::clamp(vel(x.start / 400.0), 1, 127);
  expect(allB, "fast paint: a right-to-left drag hits every note too");
}

void testPaintChordsShortNotesAndZoom() {
  auto n = row(8, 96, 1, 3);  // three-note chords of one-tick notes
  notelane::paintSegment(n, {}, NoteProp::Velocity, 0, 0.8, 800, 0.8);
  bool all = true;
  for (const auto& x : n) all = all && x.velocity == vel(0.8);
  expect(all, "paint: every note of every chord, even one-tick notes");

  // FL picks the nearest stem: a click between two stems edits the closer one,
  // even far from it, and a click exactly half way edits both.
  auto z = row(2, 96, 1);
  notelane::paintSegment(z, {}, NoteProp::Velocity, 40, 1.0, 40, 1.0);
  expect(z[0].velocity == 127 && z[1].velocity == 64, "paint: a click nearer the first stem edits only that note");
  auto z2 = row(2, 96, 1);
  notelane::paintSegment(z2, {}, NoteProp::Velocity, 60, 1.0, 60, 1.0);
  expect(z2[0].velocity == 64 && z2[1].velocity == 127, "paint: a click nearer the second stem edits only that note");
  auto z3 = row(2, 96, 1);
  notelane::paintSegment(z3, {}, NoteProp::Velocity, 48, 1.0, 48, 1.0);
  expect(z3[0].velocity == 127 && z3[1].velocity == 127, "paint: half way between two stems edits both");
  auto z4 = row(2, 96, 1);
  notelane::paintSegment(z4, {}, NoteProp::Velocity, 500, 1.0, 600, 1.0);
  expect(z4[1].velocity == 127 && z4[0].velocity == 64, "paint: right of the last stem edits the last note");
  // Fractional ticks (very high zoom): a sweep between two pointer positions a
  // fraction of a tick apart still finds its stem.
  auto z5 = row(3, 1, 1);
  notelane::paintSegment(z5, {}, NoteProp::Velocity, 0.9, 1.0, 1.2, 1.0);
  expect(z5[0].velocity == 64 && z5[1].velocity == 127 && z5[2].velocity == 64, "paint at high zoom: sub-tick sweep");
}

void testPaintSelectionOnly() {
  auto n = row(6, 24, 12);
  std::vector<bool> sel = {false, true, false, true, false, false};
  notelane::paintSegment(n, sel, NoteProp::Velocity, 0, 1.0, 200, 1.0);
  expect(n[1].velocity == 127 && n[3].velocity == 127, "paint: selected notes change");
  expect(n[0].velocity == 64 && n[2].velocity == 64 && n[4].velocity == 64 && n[5].velocity == 64,
         "paint: unselected notes are left alone when a selection exists");
  auto n2 = row(6, 24, 12);
  notelane::paintSegment(n2, sel, NoteProp::Velocity, 0, 1.0, 0, 1.0);
  expect(n2[0].velocity == 64 && n2[1].velocity == 127, "paint: with a selection, a click edits the nearest selected stem");
}

void testLineTool() {
  // The user's case: low velocity on the first note, right-drag four notes on to
  // a high velocity — the notes in between ramp evenly.
  auto n = row(6, 96, 48);
  const double a = 20.0 / 127, b = 120.0 / 127;
  notelane::applyLine(n, {}, NoteProp::Velocity, 0, a, 4 * 96, b);
  expect(n[0].velocity == 20 && n[1].velocity == 45 && n[2].velocity == 70 && n[3].velocity == 95 && n[4].velocity == 120,
         "line: 20 -> 120 over five notes = 20, 45, 70, 95, 120");
  expect(n[5].velocity == 64, "line: notes after B are untouched");

  // Dragged right to left: same result.
  auto r = row(6, 96, 48);
  notelane::applyLine(r, {}, NoteProp::Velocity, 4 * 96, b, 0, a);
  expect(r[0].velocity == 20 && r[2].velocity == 70 && r[4].velocity == 120, "line: works dragged right to left");

  // Chords: every note of a chord gets the same value.
  auto c = row(3, 96, 48, 3);
  notelane::applyLine(c, {}, NoteProp::Velocity, 0, 0.0, 192, 1.0);
  bool chordOk = true;
  for (const auto& x : c) chordOk = chordOk && x.velocity == std::clamp(vel(x.start / 192.0), 1, 127);
  expect(chordOk && c[3].velocity == 64 && c[5].velocity == 64, "line: chord notes share the value at their tick");

  // Selection: only selected notes between A and B.
  auto s = row(5, 96, 48);
  std::vector<bool> sel = {true, false, true, false, true};
  notelane::applyLine(s, sel, NoteProp::Velocity, 0, 0.0, 384, 1.0);
  expect(s[0].velocity == 1 && s[2].velocity == 64 && s[4].velocity == 127 && s[1].velocity == 64 && s[3].velocity == 64,
         "line: only selected notes when a selection exists (0 -> 1, the minimum velocity)");
  // s[2] sits at the middle: 0.5 -> 64 (rounded) which is its old value; check a non-trivial middle instead.
  auto s2 = row(5, 96, 48);
  notelane::applyLine(s2, sel, NoteProp::Velocity, 0, 0.0, 384, 0.5);
  expect(s2[2].velocity == vel(0.25), "line: selected middle note gets the line value");

  // Pick radius: pressing just right of the first stem still takes that note.
  auto p = row(3, 96, 48);
  notelane::applyLine(p, {}, NoteProp::Velocity, 3, 1.0, 192, 1.0, nullptr, 5.0);
  expect(p[0].velocity == 127, "line: slack takes a stem just beside the press point");
  auto p2 = row(3, 96, 48);
  notelane::applyLine(p2, {}, NoteProp::Velocity, 3, 1.0, 192, 1.0);
  expect(p2[0].velocity == 64 && p2[1].velocity == 127, "line: without slack only notes between A and B");
}

void testEveryProperty() {
  const NoteProp props[] = {NoteProp::Velocity, NoteProp::Pan, NoteProp::Release, NoteProp::Fine, NoteProp::ModX, NoteProp::ModY};
  bool ok = true;
  for (NoteProp p : props) {
    auto n = row(5, 100, 50);
    notelane::applyLine(n, {}, p, 0, 0.0, 400, 1.0);
    for (size_t i = 0; i < n.size(); ++i) {
      Note want = row(5, 100, 50)[i];
      tools::setNotePropNorm(want, p, static_cast<double>(i) / 4.0);
      ok = ok && n[i] == want;
    }
    auto q = row(5, 100, 50);
    notelane::paintSegment(q, {}, p, 0, 1.0, 400, 0.0);
    for (size_t i = 0; i < q.size(); ++i) {
      Note want = row(5, 100, 50)[i];
      tools::setNotePropNorm(want, p, 1.0 - static_cast<double>(i) / 4.0);
      ok = ok && q[i] == want;
    }
  }
  expect(ok, "line and paint: velocity, pan, release, fine pitch, mod X, mod Y");
}

int customCalls = 0;
void customSet(Note& n, NoteProp, double v) {
  ++customCalls;
  n.velocity = v > 0.5 ? 100 : 10;
}

void testCustomSetterAndNoChange() {
  auto n = row(3, 96, 48);
  bool changed = notelane::applyLine(n, {}, NoteProp::Velocity, 0, 1.0, 192, 1.0, customSet);
  expect(changed && customCalls == 3 && n[1].velocity == 100, "line: custom setter (the lane's rounding)");
  auto m = row(3, 96, 48);
  expect(!notelane::paintSegment(m, {}, NoteProp::Velocity, 0, 64.0 / 127, 192, 64.0 / 127), "paint: no change reported when values stay");
  std::vector<Note> empty;
  expect(!notelane::paintSegment(empty, {}, NoteProp::Velocity, 0, 1.0, 600, 1.0), "paint: no notes, no change");
}

void testOneUndoStepPerDrag() {
  Document d(makeEmptySong());
  const ObjectId pat = d.song().patterns[0].id;
  const ObjectId ch = d.addChannel("Synth", "gl.trio");
  d.setNotes(pat, ch, row(16, 24, 12), "notes", 0);
  const size_t before = d.history().position();
  // A drag: many frames, each a segment sweep, all committed with one gesture key.
  const uint64_t key = 0x5134000000000100ull;
  double prevT = 0, prevV = 0.2;
  for (int f = 1; f <= 6; ++f) {
    double t = f * 70.0, v = 0.2 + 0.1 * f;
    std::vector<Note> ns = d.song().pattern(pat)->notes.at(ch);
    if (notelane::paintSegment(ns, {}, NoteProp::Velocity, prevT, prevV, t, v)) d.setNotes(pat, ch, ns, "change velocity", key);
    prevT = t, prevV = v;
  }
  d.endGesture();
  expect(d.history().position() == before + 1, "paint drag: one undo step");
  bool allChanged = true;
  for (const auto& x : d.song().pattern(pat)->notes.at(ch)) allChanged = allChanged && x.velocity != 64;
  expect(allChanged, "paint drag: every note on the path changed");
  d.undo();
  bool restored = true;
  for (const auto& x : d.song().pattern(pat)->notes.at(ch)) restored = restored && x.velocity == 64;
  expect(restored, "paint drag: one undo restores every note");
}

}  // namespace

int main() {
  testSegmentValue();
  testFastPaintHitsEveryNote();
  testPaintChordsShortNotesAndZoom();
  testPaintSelectionOnly();
  testLineTool();
  testEveryProperty();
  testCustomSetterAndNoChange();
  testOneUndoStepPerDrag();
  return finish();
}
