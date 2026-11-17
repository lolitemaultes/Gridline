// Gridline - K-106: the tracker view of a pattern (core/TrackerGrid.h).
//   - rows, beats and bars come from the pattern; notes get note columns by overlap; OFF is a note's end;
//   - typing a note, an OFF, values and effect letters changes only what was typed (the piano roll's notes);
//   - Delete, Insert and Backspace rows, transpose, block copy / cut / paste;
//   - rack steps show and edit as steps; notes off the row grid and past the end are kept, never moved;
//   - the edits go through the Document (undo) and the project file round-trips them;
//   - the order list is derived from the playlist; the play row follows a clip in song mode.
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>
#include "Check.h"
#include "core/Document.h"
#include "core/Json.h"
#include "core/Project.h"
#include "core/ProjectInterop.h"
#include "core/Song.h"
#include "core/Demo.h"
#include "core/TrackerDemo.h"
#include "core/TrackerGrid.h"
#include "core/TrackerStream.h"

using namespace gl;
using namespace gl::trk;

namespace {

constexpr ObjectId kLead = 1001, kDrum = 1002, kPat = 1010, kPat2 = 1011;

Song makeSong() {
  Song s;
  s.ppq = 96;
  Channel lead;
  lead.id = kLead, lead.name = "Lead";
  Channel drum;
  drum.id = kDrum, drum.name = "Drum";
  s.channels = {lead, drum};
  Pattern p;
  p.id = kPat, p.name = "A", p.stepsPerBar = 16, p.bars = 1;
  Pattern q;
  q.id = kPat2, q.name = "B", q.stepsPerBar = 16, q.bars = 2;
  s.patterns = {p, q};
  s.nextId = 1100;
  return s;
}

const std::vector<Note>& notesOf(const Document& d, ObjectId pat, ObjectId ch) {
  static const std::vector<Note> none;
  const Pattern* p = d.song().pattern(pat);
  return p && p->notes.count(ch) ? p->notes.at(ch) : none;
}

struct Rows {
  std::vector<Note> notes;
  std::vector<uint8_t> steps;
  std::vector<StepProps> props;
};

// One tracker edit as the editor makes it: the track from the song, the edit on copies, then
// setNotes / setSteps in one undo step.
template <class F>
Result edit(Document& d, ObjectId pat, ObjectId ch, F&& fn) {
  const Song& s = d.song();
  const Pattern& p = *s.pattern(pat);
  const Grid g = gridOf(s, p);
  const Track t = buildTrack(s, p, ch, g);
  Rows r;
  r.notes = notesOf(d, pat, ch);
  if (p.steps.count(ch)) r.steps = p.steps.at(ch);
  if (p.stepProps.count(ch)) r.props = p.stepProps.at(ch);
  const Rows before = r;
  Result res = fn(r, t, g);
  if (res.changed) {
    d.beginGroup("tracker");
    if (r.notes != before.notes) d.setNotes(pat, ch, r.notes, "tracker");
    if (r.steps != before.steps || r.props != before.props)
      d.editPattern(pat, "tracker", [&](Pattern& q) {
        q.steps[ch] = r.steps;
        q.stepProps[ch] = r.props;
      });
    d.endGroup();
  }
  return res;
}

Track trackOf(const Document& d, ObjectId pat, ObjectId ch) {
  const Song& s = d.song();
  return buildTrack(s, *s.pattern(pat), ch, gridOf(s, *s.pattern(pat)));
}

}  // namespace

int main() {
  // ---- cp15: record quantize ----
  expect(snapRecordRow(5, 0.2f, 1, 16) == 5 && snapRecordRow(5, 0.7f, 1, 16) == 6, "record snap 1: the nearest row");
  expect(snapRecordRow(15, 0.8f, 1, 16) == 0, "record snap 1: past the last row wraps to the first");
  expect(snapRecordRow(5, 0.0f, 4, 16) == 4 && snapRecordRow(6, 0.1f, 4, 16) == 8 && snapRecordRow(14, 0.5f, 4, 16) == 0, "record snap 4: the nearest multiple, wrapping");
  expect(snapRecordRow(7, 0.9f, 0, 16) == 7, "record snap off: the row playing");
  // ---- cp15: the demo module ----
  {
    const Song d = makeTrackerDemo();
    expect(d.patterns.size() == 4 && d.playlist.size() == 7, "demo module: four patterns, seven order entries");
    bool rows64 = true, inside = true, ids = true;
    size_t most = 0;
    for (const Pattern& p : d.patterns) {
      const Grid g = gridOf(d, p);
      rows64 = rows64 && g.rows == 64;
      most = std::max(most, p.notes.size());
      for (const auto& [ch, v] : p.notes) {
        ids = ids && d.channel(ch) != nullptr;
        for (const Note& n : v) inside = inside && n.start >= 0 && n.start + n.length <= g.length() && n.length > 0;
      }
    }
    expect(rows64, "demo module: 64-row patterns");
    expect(most >= 13, "demo module: the busiest pattern plays on at least 13 tracks");
    expect(inside && ids, "demo module: every note inside its pattern, on an instrument of the song");
    int samplers = 0;
    for (const Channel& c : d.channels) samplers += c.plugin.type == "gl.tracker" ? 1 : 0;
    expect(samplers == 3 && d.channels.size() == makeDemoTrack().channels.size() + 3, "demo module: the demo's instruments and three Tracker Samplers");
    expect(makeDemoTrack().info.title == "Gridline" && makeDemoTrack().patterns.size() == 65, "demo module: the demo song is unchanged");
    expect(d.channels.size() > 9 && d.channels[0].name == "Kick" && d.channels[10].name == "Saw Lead", "demo module: drums first, the samplers in view");
  }
  // ---- grid ----
  {
    Song s = makeSong();
    const Grid g = gridOf(s, *s.pattern(kPat));
    expect(g.rowTicks == 24 && g.rows == 16 && g.rowsPerBeat == 4 && g.rowsPerBar == 16, "one row is one step: 16 rows, 4 per beat");
    const Grid g2 = gridOf(s, *s.pattern(kPat2));
    expect(g2.rows == 32, "two bars give 32 rows");
    Pattern trip = *s.pattern(kPat);
    trip.stepsPerBar = 12;
    const Grid g3 = gridOf(s, trip);
    expect(g3.rowsPerBeat == 3 && g3.rowTicks == 32, "a 12-step pattern shows 3 rows per beat (R19)");
  }
  // ---- text ----
  expect(noteText(60) == "C-4" && noteText(61) == "C#4" && noteText(69) == "A-4" && noteText(0) == "C--", "note names: 60 is C-4");
  expect(noteText(61, true) == "Db4" && noteText(70, true) == "Bb4" && noteText(60, true) == "C-4", "note names with flats: 61 is Db4");
  expect(rowText(0x1F, true) == "1F" && rowText(31, false) == "31" && rowText(0x100, true) == "100", "row numbers in hex and decimal");
  expect(hex2(127) == "7F", "two hex digits");
  expect(hexNum(7) == "07" && hexNum(255) == "FF" && hexNum(440) == "1B8", "order numbers: three digits past FF (cp25: a 440-entry song showed Len FF)");

  // ---- columns ----
  {
    std::vector<Note> ns = {Note(0, 48, 60, 100), Note(0, 48, 64, 100), Note(24, 24, 67, 100), Note(48, 24, 72, 100)};
    int used = 0;
    const std::vector<int> col = assignColumns(ns, &used);
    expect(used == 3 && col[0] == 0 && col[1] == 1 && col[2] == 2 && col[3] == 0, "overlapping notes take the next free column");
    std::vector<Note> cluster;
    for (int i = 0; i < 24; ++i) cluster.push_back(Note(0, 96, 40 + i, 100));
    used = 0;
    const std::vector<int> c2 = assignColumns(cluster, &used);
    int hidden = 0;
    for (int c : c2) hidden += c < 0;
    expect(used == kMaxColumns && hidden == 8, "a 24-note cluster: 16 columns shown, 8 hidden, none lost (R3)");
  }

  Document doc(makeSong());
  // ---- typing notes and OFF ----
  {
    const Result r = edit(doc, kPat, kLead, [](Rows& x, const Track& t, const Grid& g) { return putNote(x.notes, t, g, 0, 0, 60, 90); });
    const auto& ns = notesOf(doc, kPat, kLead);
    expect(r.changed && ns.size() == 1 && ns[0].start == 0 && ns[0].length == 16 * 24 && ns[0].pitch == 60 && ns[0].velocity == 90,
           "a typed note starts on its row and lasts to the pattern end");
  }
  {
    edit(doc, kPat, kLead, [](Rows& x, const Track& t, const Grid& g) { return putNote(x.notes, t, g, 4, 0, 62, 100); });
    const auto& ns = notesOf(doc, kPat, kLead);
    expect(ns.size() == 2 && ns[0].length == 4 * 24 && ns[1].start == 96 && ns[1].length == 12 * 24,
           "a note typed later in the column cuts the one sounding there");
  }
  {
    edit(doc, kPat, kLead, [](Rows& x, const Track& t, const Grid& g) { return putNote(x.notes, t, g, 8, 0, 64, 100); });
    edit(doc, kPat, kLead, [](Rows& x, const Track& t, const Grid& g) { return putOff(x.notes, t, g, 10, 0); });
    const auto& ns = notesOf(doc, kPat, kLead);
    expect(ns.size() == 3 && ns[2].start == 8 * 24 && ns[2].length == 2 * 24, "OFF ends the note sounding in the column");
    const Track t = trackOf(doc, kPat, kLead);
    expect(t.at(0, 0).kind == Cell::On && t.at(4, 0).kind == Cell::On && t.at(8, 0).kind == Cell::On && t.at(10, 0).kind == Cell::Off &&
               t.at(3, 0).kind == Cell::Empty && t.columns == 1,
           "the cells show the notes and the OFF; a note cut by the next one shows no OFF");
  }
  {
    // A note typed above an existing one lasts up to it.
    edit(doc, kPat, kLead, [](Rows& x, const Track& t, const Grid& g) { return putNote(x.notes, t, g, 2, 0, 59, 100); });
    const auto& ns = notesOf(doc, kPat, kLead);
    expect(ns.size() == 4 && ns[1].start == 48 && ns[1].length == 48 && ns[0].length == 48, "a note typed between two ends at the next");
    doc.undo();
    expect(notesOf(doc, kPat, kLead).size() == 3 && notesOf(doc, kPat, kLead)[0].length == 96, "undo: one step per typed note, the cut undone too");
  }
  {
    // Typing over a note changes its pitch only (R12: its other fields stay).
    edit(doc, kPat, kLead, [](Rows& x, const Track&, const Grid&) {
      x.notes[1].pan = 20, x.notes[1].modX = 30, x.notes[1].color = 3;
      Result r;
      r.changed = true;
      return r;
    });
    const Note before = notesOf(doc, kPat, kLead)[1];
    edit(doc, kPat, kLead, [](Rows& x, const Track& t, const Grid& g) { return putNote(x.notes, t, g, 4, 0, 65, 1); });
    Note after = notesOf(doc, kPat, kLead)[1];
    expect(after.pitch == 65, "typing over a note sets its pitch");
    after.pitch = before.pitch;
    expect(after == before, "and nothing else (velocity, pan, mod, colour kept)");
  }
  // ---- values ----
  {
    edit(doc, kPat, kLead, [](Rows& x, const Track& t, const Grid&) { return setVolume(x.notes, x.steps, t, 0, 0, 0x5C); });
    edit(doc, kPat, kLead, [](Rows& x, const Track& t, const Grid&) { return setPan(x.notes, x.props, t, 0, 0, 0x30); });
    const Note& n = notesOf(doc, kPat, kLead)[0];
    expect(n.velocity == 92 && n.pan == -16, "volume 5C is velocity 92; pan 30 is 16 left of centre");
    edit(doc, kPat, kLead, [](Rows& x, const Track& t, const Grid&) { return setVolume(x.notes, x.steps, t, 0, 0, 0); });
    expect(notesOf(doc, kPat, kLead)[0].velocity == 1, "volume 00 stores the lowest velocity (1)");
    bool all = true;
    for (int v = 0; v <= 128; ++v) {
      std::vector<Note> ns = notesOf(doc, kPat, kLead);
      std::vector<StepProps> pr;
      const Track t = trackOf(doc, kPat, kLead);
      setPan(ns, pr, t, 0, 0, v);
      all = all && ns[0].pan == v - 64;
    }
    expect(all, "every pan value 00..80 maps exactly (R6)");
    const Result bad = edit(doc, kPat, kLead, [](Rows& x, const Track& t, const Grid&) { return setVolume(x.notes, x.steps, t, 1, 0, 50); });
    expect(!bad.changed && !bad.refused.empty(), "a value on an empty cell is refused");
  }
  // ---- effect letters on native properties ----
  {
    edit(doc, kPat, kLead, [](Rows& x, const Track& t, const Grid&) { return setEffectAt(x.notes, t, 0, 0, 0, 'T', 0x10); });
    expect(notesOf(doc, kPat, kLead)[0].fine == 16, "T10 is fine tune +16 cents");
    edit(doc, kPat, kLead, [](Rows& x, const Track& t, const Grid&) { return setEffectAt(x.notes, t, 0, 0, 0, 'T', 0xF0); });
    expect(notesOf(doc, kPat, kLead)[0].fine == -16, "TF0 is -16 cents");
    edit(doc, kPat, kLead, [](Rows& x, const Track& t, const Grid&) { return setEffectAt(x.notes, t, 0, 0, 'T', 'G', 0); });
    const Note& n = notesOf(doc, kPat, kLead)[0];
    expect(n.slide && n.fine == 0, "a letter typed over another sets its property and resets the one shown");
    Note big;
    big.fine = 900;
    const std::vector<Effect> fx = effectsOf(big);
    expect(fx.size() == 1 && fx[0].letter == 'T' && !fx[0].exact, "fine tune beyond 127 cents is marked, not cut (R7)");
    edit(doc, kPat, kLead, [](Rows& x, const Track& t, const Grid& g) { return clearField(x.notes, x.steps, x.props, t, g, 0, 0, Field::Effect); });
    expect(!notesOf(doc, kPat, kLead)[0].slide, "Delete on the effect resets the property");
    edit(doc, kPat, kLead, [](Rows& x, const Track& t, const Grid& g) { return clearField(x.notes, x.steps, x.props, t, g, 0, 0, Field::Volume); });
    expect(notesOf(doc, kPat, kLead)[0].velocity == 100 && notesOf(doc, kPat, kLead).size() == 3, "Delete on the volume resets it, the note stays (R14)");
  }
  // ---- Delete on the note, on an OFF ----
  {
    edit(doc, kPat, kLead, [](Rows& x, const Track& t, const Grid& g) { return clearField(x.notes, x.steps, x.props, t, g, 10, 0, Field::Note); });
    const auto& ns = notesOf(doc, kPat, kLead);
    expect(ns.size() == 3 && ns[2].length == 8 * 24, "Delete on an OFF lets the note sound to the end");
    edit(doc, kPat, kLead, [](Rows& x, const Track& t, const Grid& g) { return clearField(x.notes, x.steps, x.props, t, g, 8, 0, Field::Note); });
    expect(notesOf(doc, kPat, kLead).size() == 2, "Delete on the note column deletes the note");
  }
  // ---- insert and delete rows ----
  {
    const std::vector<Note> before = notesOf(doc, kPat, kLead);
    edit(doc, kPat, kLead, [](Rows& x, const Track&, const Grid& g) { return insertRow(x.notes, x.steps, x.props, g, 2); });
    const auto& ns = notesOf(doc, kPat, kLead);
    expect(ns[0].start == 0 && ns[0].length == before[0].length + 24 && ns[1].start == before[1].start + 24,
           "Insert: later notes move one row, a note across the row grows");
    edit(doc, kPat, kLead, [](Rows& x, const Track&, const Grid& g) { return deleteRow(x.notes, x.steps, x.props, g, 2); });
    expect(notesOf(doc, kPat, kLead) == before, "Backspace on the same row brings everything back exactly");
    // R10-02 (R9-06): an insert that pushes a note off the last row drops it (as the classic insert),
    // says how many, and never keeps it out of sight past the end.
    int moved = 0;
    Result dropRes;
    std::vector<Note> ns2;
    for (int i = 0; i < 16; ++i) {
      ns2 = notesOf(doc, kPat, kLead);
      std::vector<uint8_t> st2;
      std::vector<StepProps> pr2;
      dropRes = insertRow(ns2, st2, pr2, gridOf(doc.song(), *doc.song().pattern(kPat)), 1);
      if (!dropRes.refused.empty() || dropRes.dropped > 0) break;
      edit(doc, kPat, kLead, [](Rows& x, const Track&, const Grid& g) { return insertRow(x.notes, x.steps, x.props, g, 1); });
      ++moved;
    }
    const Grid gd = gridOf(doc.song(), *doc.song().pattern(kPat));
    expect(moved > 0 && dropRes.refused.empty() && dropRes.changed && dropRes.dropped == 1 && ns2.size() == 1 &&
               std::all_of(ns2.begin(), ns2.end(), [&](const Note& n) { return n.start < gd.length(); }),
           "the insert that pushes a note off the last row drops it (one note gone, none past the end)");
    expect(trackOf(doc, kPat, kLead).pastEnd == 0 && notesOf(doc, kPat, kLead).size() == 2, "the inserts before it moved every note, nothing hidden past the end");
    for (int i = 0; i < moved; ++i) {
      edit(doc, kPat, kLead, [](Rows& x, const Track&, const Grid& g) { return deleteRow(x.notes, x.steps, x.props, g, 1); });
    }
    {
      // R12-02: the inserts pushed the second note's OFF off the pattern (gone, as in the classic
      // editor), so after the deletes the note is back in place and rings on to the end.
      const auto& back = notesOf(doc, kPat, kLead);
      expect(back.size() == 2 && back[0] == before[0] && back[1].start == before[1].start && back[1].pitch == before[1].pitch &&
                 back[1].start + back[1].length == gd.length(),
             "and the deletes bring the notes back (the OFF pushed off the end stays gone)");
    }
  }
  // ---- off-grid notes stay exactly (R1, R2) ----
  {
    std::vector<Note> ns = notesOf(doc, kPat, kLead);
    Note odd(13 * 24 + 7, 31, 70, 77);
    odd.frac = 0.25f;
    ns.push_back(odd);
    doc.setNotes(kPat, kLead, ns, "add");
    const Track t = trackOf(doc, kPat, kLead);
    int on = -1;
    for (int c = 0; c < t.columns; ++c)
      if (t.at(13, c).kind == Cell::On && t.at(13, c).offGrid) on = c;
    expect(on >= 0, "a note off the row grid shows on the row that holds it, marked");
    edit(doc, kPat, kLead, [](Rows& x, const Track& tr, const Grid&) { return setVolume(x.notes, x.steps, tr, 0, 0, 0x40); });
    bool kept = false;
    for (const Note& n : notesOf(doc, kPat, kLead)) kept = kept || (n.start == odd.start && n.length == 31 && n.frac == 0.25f && n.velocity == 77);
    expect(kept, "an edit to another cell leaves it byte-equal");
  }
  // ---- transpose ----
  {
    std::vector<Note> ns = {Note(0, 24, 125, 100), Note(24, 24, 60, 100)};
    const Grid g = gridOf(doc.song(), *doc.song().pattern(kPat));
    const Result r = transpose(ns, g, 0, 1, 5);
    expect(r.changed && r.refusedCount == 1 && ns[0].pitch == 125 && ns[1].pitch == 65, "transpose moves the notes it can and keeps the others (R15)");
  }
  // ---- blocks ----
  {
    Song s = makeSong();
    Document d(s);
    edit(d, kPat, kLead, [](Rows& x, const Track& t, const Grid& g) { return putNote(x.notes, t, g, 0, 0, 60, 100); });
    edit(d, kPat, kLead, [](Rows& x, const Track& t, const Grid& g) { return putNote(x.notes, t, g, 2, 0, 62, 100); });
    edit(d, kPat, kLead, [](Rows& x, const Track& t, const Grid& g) { return putOff(x.notes, t, g, 3, 0); });
    const Grid g = gridOf(d.song(), *d.song().pattern(kPat));
    const std::vector<Note> block = copyRows(notesOf(d, kPat, kLead), {}, {}, g, 0, 3);
    expect(block.size() == 2 && block[1].start == 48, "copy takes the notes starting in the block, relative to it");
    edit(d, kPat, kLead, [&](Rows& x, const Track&, const Grid& gg) { return pasteRows(x.notes, x.steps, gg, 8, 4, block); });
    const auto& ns = notesOf(d, kPat, kLead);
    expect(ns.size() == 4 && ns[2].start == 8 * 24 && ns[2].pitch == 60 && ns[3].start == 10 * 24 && ns[3].length == 24, "paste puts them at the cursor");
    edit(d, kPat, kLead, [&](Rows& x, const Track&, const Grid& gg) { return pasteRows(x.notes, x.steps, gg, 8, 4, block); });
    expect(notesOf(d, kPat, kLead).size() == 4, "pasting again over the same rows replaces, never doubles");
    edit(d, kPat, kLead, [&](Rows& x, const Track&, const Grid& gg) { return cutRows(x.notes, x.steps, gg, 8, 11); });
    expect(notesOf(d, kPat, kLead).size() == 2, "cut removes the block's notes");
    // R7-03: a block pasted near the end keeps nothing past the last row.
    const int rowsN = g.rows;
    edit(d, kPat, kLead, [&](Rows& x, const Track&, const Grid& gg) { return pasteRows(x.notes, x.steps, gg, rowsN - 1, 4, block); });
    bool inside = true;
    for (const Note& n : notesOf(d, kPat, kLead)) inside = inside && n.start < g.tickOf(rowsN) && n.start + n.length <= g.tickOf(rowsN);
    expect(inside && notesOf(d, kPat, kLead).size() == 3, "R7-03: a block pasted at the last row is clipped to the pattern");
  }
  // R7-06: a pasted note cuts the note that still sounds before it (no second column).
  {
    Song s = makeSong();
    Document d(s);
    edit(d, kPat, kLead, [](Rows& x, const Track& t, const Grid& g) { return putNote(x.notes, t, g, 0, 0, 60, 100); });
    const Grid g = gridOf(d.song(), *d.song().pattern(kPat));
    const std::vector<Note> block = copyRows(notesOf(d, kPat, kLead), {}, {}, g, 0, 3);
    edit(d, kPat, kLead, [&](Rows& x, const Track&, const Grid& gg) { return pasteRows(x.notes, x.steps, gg, 8, 4, block); });
    const auto& ns = notesOf(d, kPat, kLead);
    int cols = 0;
    assignColumns(ns, &cols);
    expect(ns.size() == 2 && ns[0].start + ns[0].length == 8 * 24 && cols == 1, "R7-06: the pasted note cuts the one sounding before it, one column");
  }
  // R11-05: one voice per track, as the classic channel: after a paste, an Insert and a Backspace
  // every note still rings on to the next one (no OFF appears that was not typed, no second column).
  {
    Song s = makeSong();
    Document d(s);
    const int rowsAt[] = {0, 3, 6, 9, 12};
    const int pitches[] = {60, 64, 67, 60, 64};
    for (int i = 0; i < 5; ++i) {
      const int r = rowsAt[i], p = pitches[i];
      edit(d, kPat, kLead, [r, p](Rows& x, const Track& t, const Grid& g) { return putNote(x.notes, t, g, r, 0, p, 100); });
    }
    const Grid g = gridOf(d.song(), *d.song().pattern(kPat));
    auto tied = [&](const char* what) {
      const auto& ns = notesOf(d, kPat, kLead);
      bool ok = !ns.empty();
      for (size_t i = 0; i < ns.size(); ++i) {
        // Each note ends at the next; the last one at the end or past it (Insert moves it along).
        const Tick e = ns[i].start + ns[i].length;
        ok = ok && (i + 1 < ns.size() ? e == ns[i + 1].start : e >= g.length());
      }
      int cols = 0;
      assignColumns(ns, &cols);
      expect(ok && cols == 1, what);
    };
    tied("R11-05: typed notes ring on to the next one");
    const std::vector<Note> block = copyRows(notesOf(d, kPat, kLead), {}, {}, g, 0, 3);
    edit(d, kPat, kLead, [&](Rows& x, const Track&, const Grid& gg) { return pasteRows(x.notes, x.steps, gg, 8, 4, block); });
    tied("R11-05: a pasted block's last note is cut by the next note (no overlap, one column)");
    edit(d, kPat, kLead, [](Rows& x, const Track&, const Grid& gg) { return insertRow(x.notes, x.steps, x.props, gg, 2); });
    tied("R11-05: after Insert every note rings on to the next");
    edit(d, kPat, kLead, [](Rows& x, const Track&, const Grid& gg) { return deleteRow(x.notes, x.steps, x.props, gg, 4); });
    tied("R11-05: Backspace over a note lets the one before ring on (no phantom OFF)");
    // A typed OFF stays where it was typed.
    edit(d, kPat, kLead, [](Rows& x, const Track& t, const Grid& gg) { return putOff(x.notes, t, gg, 1, 0); });
    edit(d, kPat, kLead, [](Rows& x, const Track&, const Grid& gg) { return insertRow(x.notes, x.steps, x.props, gg, 3); });
    const auto& ns = notesOf(d, kPat, kLead);
    expect(!ns.empty() && ns[0].start == 0 && ns[0].length == 24, "R11-05: an OFF typed before the moved rows stays at its row");
  }
  // R12-02: Backspace on the OFF's row takes the OFF with it (the note rings on to the next), and the
  // last note, which rang to the pattern's end, still does (no OFF on the last row).
  {
    Document d(makeSong());
    for (auto [r, p] : {std::pair{0, 60}, std::pair{6, 64}, std::pair{9, 67}})
      edit(d, kPat, kLead, [r = r, p = p](Rows& x, const Track& t, const Grid& gg) { return putNote(x.notes, t, gg, r, 0, p, 100); });
    edit(d, kPat, kLead, [](Rows& x, const Track& t, const Grid& gg) { return putOff(x.notes, t, gg, 3, 0); });
    edit(d, kPat, kLead, [](Rows& x, const Track&, const Grid& gg) { return deleteRow(x.notes, x.steps, x.props, gg, 3); });
    const auto& ns = notesOf(d, kPat, kLead);
    const Grid g = gridOf(d.song(), *d.song().pattern(kPat));
    const Tick r = g.rowTicks;
    expect(ns.size() == 3 && ns[0].start == 0 && ns[0].length == 5 * r && ns[1].start == 5 * r && ns[1].length == 3 * r && ns[2].start == 8 * r &&
               ns[2].start + ns[2].length == g.length(),
           "R12-02: the OFF's row deleted: C rings to E, G still reaches the end");
  }
  // R11-02: the folders Gridline makes beside a project travel with it on Save as: the files are
  // copied and named relative to the new project; a save in place copies nothing.
  {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path root = fs::temp_directory_path() / "gl_k106_r1102";
    fs::remove_all(root, ec);
    const fs::path oldDir = root / "old", newDir = root / "new";
    fs::create_directories(oldDir / "Edited audio", ec);
    fs::create_directories(oldDir / "song Samples", ec);
    fs::create_directories(newDir, ec);
    for (const fs::path& f : {oldDir / "Edited audio" / "a1-0ca4.wav", oldDir / "song Samples" / "s1.wav"}) std::ofstream(f, std::ios::binary) << "RIFF data " << f.filename().string();
    Song s = makeSong();
    s.channels[0].plugin.type = "gl.sampler";
    s.channels[0].plugin.data = (oldDir / "Edited audio" / "a1-0ca4.wav").string();
    s.channels[1].plugin.type = "gl.sampler";
    s.channels[1].plugin.data = (oldDir / "song Samples" / "s1.wav").string();
    const std::vector<std::pair<std::string, std::string>> folders = {{(oldDir / "Edited audio").string(), "Edited audio"},
                                                                      {(oldDir / "song Samples").string(), "copy Samples"}};
    std::string err;
    const int n = adoptProjectFolders(s, folders, newDir.string(), &err);
    expect(n == 2 && err.empty() && s.channels[0].plugin.data == "Edited audio/a1-0ca4.wav" && s.channels[1].plugin.data == "copy Samples/s1.wav",
           "R11-02: Save as names the edited audio and the samples relative to the new project");
    expect(fs::is_regular_file(newDir / "Edited audio" / "a1-0ca4.wav") && fs::is_regular_file(newDir / "copy Samples" / "s1.wav"),
           "R11-02: and copies them beside it");
    // In place: the same folder, nothing copied, the name made relative.
    Song t = makeSong();
    t.channels[0].plugin.type = "gl.sampler";
    t.channels[0].plugin.data = (oldDir / "Edited audio" / "a1-0ca4.wav").string();
    const int m = adoptProjectFolders(t, {{(oldDir / "Edited audio").string(), "Edited audio"}}, oldDir.string(), &err);
    expect(m == 1 && t.channels[0].plugin.data == "Edited audio/a1-0ca4.wav" && !fs::exists(oldDir / "Edited audio" / "a1-0ca4 (2).wav"),
           "R11-02: a save in place names it relative and copies nothing");
    // Loaded again from the new place: found (resolveRelativePaths).
    resolveRelativePaths(s, newDir.string());
    expect(s.channels[0].plugin.data == (newDir / "Edited audio" / "a1-0ca4.wav").lexically_normal().string(), "R11-02: the copy is found beside the moved project");
    fs::remove_all(root, ec);
  }
  // R12-01: a chain of Save as: B (with its edited audio and samples), then v2, then v3. The open
  // song keeps B's absolute names all along; each save finds B's folders wherever they are and v3
  // gets its own copies, named relative. A user's own "Drum Samples" folder is never adopted.
  {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path root = fs::temp_directory_path() / "gl_k106_r1201";
    fs::remove_all(root, ec);
    const fs::path b = root / "B", v2 = root / "v2", v3 = root / "v3", lib = root / "lib" / "Drum Samples";
    for (const fs::path& d : {b / "Edited audio", b / "song Samples", v2, v3, lib}) fs::create_directories(d, ec);
    std::ofstream(b / "song.gridline.json") << "{}";
    for (const fs::path& f : {b / "Edited audio" / "e1.wav", b / "song Samples" / "s1.wav", lib / "kick.wav"}) std::ofstream(f, std::ios::binary) << "RIFF " << f.filename().string();
    Song open = makeSong();
    Channel third;
    third.id = 1200, third.name = "Kick";
    open.channels.push_back(third);
    open.channels[0].plugin.type = "gl.sampler";
    open.channels[0].plugin.data = (b / "Edited audio" / "e1.wav").string();
    open.channels[1].plugin.type = "gl.sampler";
    open.channels[1].plugin.data = (b / "song Samples" / "s1.wav").string();
    open.channels[2].plugin.type = "gl.sampler";
    open.channels[2].plugin.data = (lib / "kick.wav").string();
    struct Step {
      fs::path dir;
      const char* stem;
      fs::path prev;
    };
    bool ok = true;
    for (const Step& st : {Step{v2, "song v2", b}, Step{v3, "song v3", v2}}) {
      Song out = open;  // the open song is not changed by a save
      // As App::saveProject builds the list: the previous project's folders, the new one's, then
      // the Gridline-made folders found from the song's own files.
      std::vector<std::pair<std::string, std::string>> folders = {{(st.prev / "Edited audio").string(), "Edited audio"},
                                                                  {(st.dir / "Edited audio").string(), "Edited audio"}};
      for (auto& f : gridlineMadeFolders(out, st.stem)) folders.push_back(f);
      std::string err;
      adoptProjectFolders(out, folders, st.dir.string(), &err);
      std::ofstream(st.dir / (std::string(st.stem) + ".gridline.json")) << "{}";
      ok = ok && err.empty() && out.channels[0].plugin.data == "Edited audio/e1.wav" && out.channels[1].plugin.data == std::string(st.stem) + " Samples/s1.wav" &&
           out.channels[2].plugin.data == (lib / "kick.wav").string() && fs::is_regular_file(st.dir / "Edited audio" / "e1.wav") &&
           fs::is_regular_file(st.dir / (std::string(st.stem) + " Samples") / "s1.wav");
    }
    expect(ok, "R12-01: Save as twice from the same open song: v3 has its own copies, named relative");
    expect(!fs::exists(v3 / "song v3 Samples" / "kick.wav") && !fs::exists(v3 / "Edited audio" / "kick.wav"), "R12-01: a user's sample folder stays where it is");
    fs::remove_all(root, ec);
  }
  // ---- rack steps (R17) ----
  {
    Song s = makeSong();
    std::vector<uint8_t> row(16, 0);
    row[0] = 100, row[4] = 80, row[15] = 0;
    s.patterns[0].steps[kDrum] = row;
    s.patterns[0].stepProps[kDrum] = std::vector<StepProps>(16);
    s.patterns[0].stepProps[kDrum][4].pitch = 62;
    Document d(s);
    Track t = trackOf(d, kPat, kDrum);
    expect(t.hasSteps && t.at(0, 0).kind == Cell::Step && t.at(4, 0).kind == Cell::Step && t.at(1, 0).kind == Cell::Empty, "rack steps show as cells");
    const Result typed = edit(d, kPat, kDrum, [](Rows& x, const Track& tr, const Grid& g) { return putNote(x.notes, tr, g, 4, 0, 65, 100); });
    expect(!typed.changed && !typed.refused.empty(), "typing a key on a step is refused (edit it in Steps)");
    edit(d, kPat, kDrum, [](Rows& x, const Track& tr, const Grid&) { return setVolume(x.notes, x.steps, tr, 4, 0, 0x30); });
    expect(d.song().pattern(kPat)->steps.at(kDrum)[4] == 0x30 && notesOf(d, kPat, kDrum).empty(), "volume on a step sets its velocity, it stays a step");
    edit(d, kPat, kDrum, [](Rows& x, const Track& tr, const Grid& g) { return clearField(x.notes, x.steps, x.props, tr, g, 0, 0, Field::Note); });
    expect(d.song().pattern(kPat)->steps.at(kDrum)[0] == 0, "Delete clears the step");
    edit(d, kPat, kDrum, [](Rows& x, const Track&, const Grid& g) { return insertRow(x.notes, x.steps, x.props, g, 2); });
    expect(d.song().pattern(kPat)->steps.at(kDrum)[5] == 0x30 && d.song().pattern(kPat)->stepProps.at(kDrum)[5].pitch == 62,
           "Insert moves the steps and their keys down a row");
    d.undo();
    expect(d.song().pattern(kPat)->steps.at(kDrum)[4] == 0x30, "undo brings the steps back");
  }
  // ---- save and load keep every tracker edit (no new field written) ----
  {
    const std::string text = projectToText(doc.song());
    Song back;
    std::string err;
    expect(loadProjectText(text, "", back, &err), "the project loads back");
    if (!err.empty()) std::printf("# load: %s\n", err.c_str());
    expect(back.pattern(kPat) && back.pattern(kPat)->notes == doc.song().pattern(kPat)->notes, "the notes round-trip through the file");
    const std::string again = projectToText(back);
    Song back2;
    expect(loadProjectText(again, "", back2, &err) && projectToText(back2) == again, "and save again byte-equal");
    expect(text.find("tracker") == std::string::npos, "nothing of the tracker's own is written into the project");
  }
  // ---- order list and the play row ----
  {
    Song s = makeSong();
    auto clip = [&](ObjectId id, ObjectId pat, int track, Tick start) {
      Clip c;
      c.id = id, c.pattern = pat, c.track = track, c.start = start;
      s.playlist.push_back(c);
    };
    const Tick bar = s.barTicks();
    clip(50, kPat, 0, 0);
    clip(51, kPat, 0, bar);
    clip(52, kPat2, 0, 2 * bar);
    clip(53, kPat, 1, 2 * bar);  // under the first bar of B: a combined entry
    const std::vector<OrderEntry> ol = orderList(s);
    expect(ol.size() == 4 && ol[0].pattern == kPat && ol[1].pattern == kPat && ol[2].pattern == kPat2 && ol[2].patterns == 2 && ol[3].patterns == 1,
           "the order list is cut wherever a clip starts or ends; an entry with two patterns is combined");
    expect(patternTickAt(s, kPat, bar + 30) == 30 && patternTickAt(s, kPat2, 3 * bar + 5) == bar + 5 && patternTickAt(s, kPat2, 10) == -1,
           "the play row: the pattern's own tick under the song position");
    expect(patternAt(s, 2 * bar + 1) == kPat2 && patternAt(s, 9 * bar) == kNoId, "the pattern under the playhead is the top clip's");
    expect(patternNumber(s, kPat2) == 2, "pattern numbers are Gridline's own (1-based)");
    // GitHub #70: the piano roll's play line in song mode (the pattern's tick under the song position).
    expect(patternPlayPos(s, kPat, true, kNoId, bar + 30.5) == 30.5 && patternPlayPos(s, kPat2, true, kPat, 3.0 * bar + 5) == bar + 5.0,
           "song mode: the play line sits at the pattern's own position, whichever pattern pattern mode last played");
    expect(patternPlayPos(s, kPat2, true, kPat2, 10) == -1 && patternPlayPos(s, kPat, true, kPat, 9.0 * bar) == -1,
           "song mode: no play line where no clip of the pattern plays");
    expect(patternPlayPos(s, kPat, false, kPat, 77.25) == 77.25 && patternPlayPos(s, kPat2, false, kPat, 77) == -1,
           "pattern mode: the engine's position while that pattern plays, none for another pattern");
    {  // GitHub #70 (0.7.7): every place the pattern plays: a looping clip's later passes, a clip cut at its start
      Song l = s;
      Clip c;
      c.id = 60, c.pattern = kPat, c.track = 3, c.start = 20 * bar, c.length = 3 * bar, c.loop = true;
      l.playlist.push_back(c);
      Clip o;
      o.id = 61, o.pattern = kPat2, o.track = 3, o.start = 30 * bar, o.length = bar, o.offset = 100;
      l.playlist.push_back(o);
      expect(patternPlayPos(l, kPat, true, kNoId, 20.0 * bar + 50.25) == 50.25 && patternPlayPos(l, kPat, true, kNoId, 22.0 * bar + 50.25) == 50.25,
             "song mode: a looping clip shows the play line on every pass, at the pattern's own position");
      expect(patternPlayPos(l, kPat2, true, kNoId, 30.0 * bar + 20) == 120 && patternPlayPos(l, kPat2, true, kNoId, 31.0 * bar + 1) == -1,
             "song mode: a clip cut at its start shows the line where its content plays, none past its end");
    }
    // T-13: audio clips. One under the first pattern entry (counted on it, no cut); two after the
    // song's patterns overlapping (audio parts cut at their starts and ends).
    auto audio = [&](ObjectId id, int track, Tick start, Tick len) {
      Clip c;
      c.id = id, c.kind = ClipKind::Audio, c.track = track, c.start = start, c.length = len;
      s.playlist.push_back(c);
    };
    audio(80, 2, bar / 2, bar / 4);
    audio(81, 2, 4 * bar, 2 * bar);
    audio(82, 3, 5 * bar, 2 * bar);
    const std::vector<OrderEntry> la = orderList(s);
    expect(la.size() == 7 && la[0].pattern == kPat && la[0].audio.size() == 1 && la[0].end == bar,
           "an audio clip under a pattern entry is counted on it and does not cut it");
    expect(la[4].pattern == kNoId && la[4].patterns == 0 && la[4].start == 4 * bar && la[4].end == 5 * bar && la[5].audio.size() == 2 &&
               la[6].start == 6 * bar && la[6].end == 7 * bar && la[6].audio.size() == 1,
           "after the patterns: audio parts cut where the audio clips start and end");
    expect(hasPatternEntries(s), "the song has pattern entries");
    Song onlyAudio = makeSong();
    onlyAudio.playlist.clear();
    {
      Clip c;
      c.id = 90, c.kind = ClipKind::Audio, c.track = 0, c.start = 0, c.length = bar;
      onlyAudio.playlist.push_back(c);
    }
    expect(orderList(onlyAudio).size() == 1 && !hasPatternEntries(onlyAudio), "an audio-only song: one audio part, no pattern entry");
    Document ad(onlyAudio);
    expect(orderRemove(ad, 0) == OrderEdit::OtherClips && orderSetPattern(ad, 0, kPat) == OrderEdit::OtherClips && ad.song().playlist.size() == 1,
           "an audio part is not removed or changed from the order list");
    expect(orderInsert(ad, 0, kPat) == OrderEdit::Done && orderList(ad.song()).front().pattern == kPat, "a pattern inserted before an audio part");
  }
  // ---- order list edits (the playlist, one undo step each) ----
  {
    Song s = makeSong();
    s.tracks.resize(4);
    const Tick bar = s.barTicks();
    auto clip = [&](ObjectId id, ObjectId pat, int track, Tick start) {
      Clip c;
      c.id = id, c.pattern = pat, c.track = track, c.start = start;
      s.playlist.push_back(c);
    };
    clip(60, kPat, 0, 0);         // A
    clip(61, kPat2, 1, bar);      // B (two bars) on track 1
    clip(62, kPat, 0, 3 * bar);   // A
    Document doc(s);
    auto starts = [&]() {
      std::vector<std::pair<ObjectId, Tick>> v;
      for (const OrderEntry& e : orderList(doc.song())) v.push_back({e.pattern, e.start});
      return v;
    };
    expect(orderInsert(doc, 1, kPat) == OrderEdit::Done, "insert at entry 1");
    const auto a1 = starts();
    expect(a1.size() == 4 && a1[1] == std::make_pair(kPat, bar) && a1[2] == std::make_pair(kPat2, 2 * bar) && a1[3].second == 4 * bar,
           "the inserted pattern sits at the entry, the song after it moved right by its length");
    const Clip* ins = nullptr;
    for (const Clip& c : doc.song().playlist)
      if (c.id != 60 && c.id != 61 && c.id != 62) ins = &c;
    expect(ins && ins->track == 1, "on the entry's top track");
    expect(doc.undo() && starts().size() == 3 && starts()[1] == std::make_pair(kPat2, bar), "one undo takes the insert and the move back");
    expect(orderRemove(doc, 1) == OrderEdit::Done, "remove entry 1");
    const auto r1 = starts();
    expect(r1.size() == 2 && r1[1] == std::make_pair(kPat, bar) && !doc.song().clip(61), "its clip went, the song after it moved left");
    expect(doc.undo() && starts().size() == 3, "undo brings it back");
    expect(orderInsert(doc, 99, kPat2) == OrderEdit::Done && starts().back() == std::make_pair(kPat2, 4 * bar), "insert past the end appends");
    doc.undo();
    expect(orderSetPattern(doc, 0, kPat2) == OrderEdit::Done, "entry 0 plays B instead");
    const auto p1 = starts();
    expect(p1.size() == 3 && p1[0] == std::make_pair(kPat2, Tick(0)) && p1[1] == std::make_pair(kPat2, 2 * bar) && p1[2].second == 4 * bar,
           "B is a bar longer than A: the song after it moved a bar right");
    expect(doc.undo() && starts()[1] == std::make_pair(kPat2, bar), "one undo");
    // R6-02: A twice as long: the song keeps its order (B and the second A move a bar right).
    {
      const int spb = doc.song().pattern(kPat)->stepsPerBar, was = doc.song().pattern(kPat)->bars;
      setPatternLengthInSong(doc, kPat, was * 2, spb);
      const auto l1 = starts();
      const Tick grow = static_cast<Tick>(was) * bar;
      expect(l1.size() == 3 && l1[0] == std::make_pair(kPat, Tick(0)) && l1[1] == std::make_pair(kPat2, bar + grow) && l1[2] == std::make_pair(kPat, 3 * bar + grow),
             "a longer pattern moves the entries after each of its clips, none overlap");
      expect(doc.undo() && doc.song().pattern(kPat)->bars == was && starts()[1] == std::make_pair(kPat2, bar), "one undo takes the length and the moves back");
      // R6-02 follow-up: "Loop to" (the song loop marker) on entry 2 stays on entry 2, in the same undo step.
      doc.editSong("loop", [&](Song& s) {
        Marker m;
        m.pos = 3 * bar, m.name = "Loop", m.action = MarkerAction::SongLoop;
        s.markers.push_back(m);
      });
      const size_t undoAt = doc.history().position();
      setPatternLengthInSong(doc, kPat, was * 2, spb);
      const auto l2 = starts();
      expect(doc.song().markers.size() == 1 && l2.size() == 3 && doc.song().markers[0].pos == l2[2].second, "the song loop marker follows its entry");
      expect(doc.history().position() == undoAt + 1, "the length, the moves and the marker are one undo step");
      expect(doc.undo() && doc.song().markers[0].pos == 3 * bar, "undo puts the marker back");
      doc.undo();  // the marker itself
    }
    // A clip under two entries is never cut.
    Song t = makeSong();
    t.tracks.resize(4);
    Clip longA;
    longA.id = 70, longA.pattern = kPat2, longA.track = 0, longA.start = 0;
    Clip shortB;
    shortB.id = 71, shortB.pattern = kPat, shortB.track = 1, shortB.start = bar;
    t.playlist = {longA, shortB};
    Document d2(t);
    expect(orderList(d2.song()).size() == 2 && orderRemove(d2, 1) == OrderEdit::Spans && d2.song().playlist.size() == 2,
           "an entry whose clip runs into another entry is refused, nothing removed");
    // R9-01: A also on a parallel track beside B: the song after them moves once, by what that
    // block's end moves (no silent bar).
    {
      Song u = makeSong();
      u.tracks.resize(4);
      const Tick lenA = u.patternLength(*u.pattern(kPat)), lenB = u.patternLength(*u.pattern(kPat2));
      Clip a0, b0, a1, b1;
      a0.id = 80, a0.pattern = kPat, a0.track = 0, a0.start = 0;
      b0.id = 81, b0.pattern = kPat2, b0.track = 0, b0.start = lenA;
      a1.id = 82, a1.pattern = kPat, a1.track = 1, a1.start = lenA;
      b1.id = 83, b1.pattern = kPat2, b1.track = 0, b1.start = lenA + std::max(lenA, lenB);
      u.playlist = {a0, b0, a1, b1};
      Document d3(u);
      const int spb = d3.song().pattern(kPat)->stepsPerBar, was = d3.song().pattern(kPat)->bars;
      setPatternLengthInSong(d3, kPat, was * 2, spb);
      const Song& v = d3.song();
      const Tick blk = std::max(lenB, 2 * lenA);  // the second block's new length
      expect(v.clip(81)->start == 2 * lenA && v.clip(82)->start == 2 * lenA && v.clip(83)->start == 2 * lenA + blk,
             "the parallel A moves with its block; the last entry follows the block's end, once");
      setPatternLengthInSong(d3, kPat, was, spb);
      expect(v.clip(81)->start == lenA && v.clip(83)->start == lenA + std::max(lenA, lenB), "shorter again: back where they were");
    }
  }
  // ---- interpolate (Edit > Interpolate volume / pan / effect) ----
  {
    Song s = makeSong();
    const Grid g = gridOf(s, *s.pattern(kPat));
    std::vector<Note> ns = {Note(0, 24, 60, 20), Note(24, 24, 62, 90), Note(72, 24, 64, 90), Note(96, 24, 65, 120), Note(240, 24, 67, 5)};
    ns[0].pan = -40, ns[3].pan = 40;
    const Result rv = interpolate(ns, g, 0, 4, Field::Volume);
    expect(rv.changed && ns[0].velocity == 20 && ns[1].velocity == 45 && ns[2].velocity == 95 && ns[3].velocity == 120 && ns[4].velocity == 5,
           "volume on a line from the block's first note to its last, by start; the ends and notes outside stay");
    const Result rp = interpolate(ns, g, 0, 4, Field::Pan);
    expect(rp.changed && ns[1].pan == -20 && ns[2].pan == 20, "pan on a line too");
    std::vector<Note> two = {Note(0, 24, 60, 20), Note(24, 24, 62, 90)};
    expect(!interpolate(two, g, 0, 4, Field::Volume).changed && !interpolate(two, g, 0, 4, Field::Volume).refused.empty(),
           "two notes: nothing between to interpolate (refused)");
    std::vector<Note> fx = {Note(0, 24, 60, 100), Note(24, 24, 62, 100), Note(48, 24, 64, 100)};
    fx[0].modX = 0, fx[2].modX = 200;
    expect(interpolate(fx, g, 0, 2, Field::Effect).changed && fx[1].modX == 100, "an effect letter both ends show (X) on a line");
    std::vector<Note> mixed = fx;
    mixed[2].modX = kNoteModCentre, mixed[2].modY = 10;
    expect(!interpolate(mixed, g, 0, 2, Field::Effect).changed, "different letters at the ends: refused");
  }
  // ---- clipboard masks (Advanced edit) ----
  {
    Song s = makeSong();
    const Grid g = gridOf(s, *s.pattern(kPat));  // 24 ticks a row
    const bool all[kParts] = {true, true, true, true};
    // The block: row 0 a C-5 at volume 40 with pan -30, row 2 empty, row 3 an E-5 with fine tune.
    std::vector<Note> block = {Note(0, 24, 60, 40), Note(72, 24, 64, 100)};
    block[0].pan = -30, block[1].fine = 12;
    auto dest = [] {
      std::vector<Note> d = {Note(96, 24, 50, 70), Note(144, 24, 52, 90), Note(168, 24, 53, 110)};
      d[0].pan = 20, d[1].modX = 7;
      return d;
    };
    std::vector<uint8_t> steps;
    // Everything on: an ordinary paste (row 4 on: rows 4..7 replaced).
    {
      std::vector<Note> d = dest();
      Masks m;
      expect(pasteRowsMasked(d, steps, g, 4, 4, block, all, m).changed && d.size() == 2 && d[0].pitch == 60 && d[0].start == 96 && d[0].velocity == 40 &&
                 d[1].pitch == 64 && d[1].start == 168 && d[1].fine == 12,
             "masks all on: the block replaces the rows, as a plain paste");
    }
    // The note part transparent: row 6 (empty in the block) keeps its note.
    {
      std::vector<Note> d = dest();
      Masks m;
      m.transparent[static_cast<int>(Part::Note)] = true;
      pasteRowsMasked(d, steps, g, 4, 4, block, all, m);
      expect(d.size() == 3 && d[0].pitch == 60 && d[1].pitch == 52 && d[1].start == 144 && d[2].pitch == 64,
             "transparent notes: a row the block leaves empty keeps its note");
    }
    // Volume only (no note part): the notes stay, row 4's takes volume 40, pan stays.
    {
      std::vector<Note> d = dest();
      Masks m;
      for (bool& p : m.paste) p = false;
      m.paste[static_cast<int>(Part::Volume)] = true;
      pasteRowsMasked(d, steps, g, 4, 4, block, all, m);
      expect(d.size() == 3 && d[0].pitch == 50 && d[0].velocity == 40 && d[0].pan == 20 && d[2].pitch == 53 && d[2].velocity == 100,
             "volume only: the notes keep pitch and place and take the block's volumes, row by row");
    }
    // Notes pasted, pan not: the new note keeps the pan of the note it replaces.
    {
      std::vector<Note> d = dest();
      Masks m;
      m.paste[static_cast<int>(Part::Pan)] = false;
      pasteRowsMasked(d, steps, g, 4, 4, block, all, m);
      expect(d[0].pitch == 60 && d[0].pan == 20 && d[0].velocity == 40, "pan not pasted: the replaced note's pan stays with the new note");
    }
    // Transparent effect: an empty effect does not clear one (row 7 E-5 has fine tune; row 6 ...).
    {
      std::vector<Note> d = dest();
      Masks m;
      for (bool& p : m.paste) p = false;
      m.paste[static_cast<int>(Part::Effect)] = true;
      m.transparent[static_cast<int>(Part::Effect)] = true;
      std::vector<Note> b2 = {Note(48, 24, 61, 100)};  // row 2 (dest row 6): no effect
      pasteRowsMasked(d, steps, g, 4, 4, b2, all, m);
      expect(d[1].modX == 7, "a transparent effect part does not clear the row's effect");
      m.transparent[static_cast<int>(Part::Effect)] = false;
      pasteRowsMasked(d, steps, g, 4, 4, b2, all, m);
      expect(d[1].modX == kNoteModCentre, "not transparent: the empty effect clears it");
    }
    // The copy mask: parts left out go back to defaults, and are not carried.
    {
      std::vector<Note> b = block;
      Masks m;
      m.copy[static_cast<int>(Part::Volume)] = false;
      maskCopied(b, m);
      expect(b[0].velocity == 100 && b[0].pan == -30, "copy mask: the volume left out is the default, the pan kept");
      std::vector<Note> d = dest();
      const bool carried[kParts] = {true, false, true, true};
      Masks pm;
      for (bool& p : pm.paste) p = false;
      pm.paste[static_cast<int>(Part::Volume)] = true;
      expect(!pasteRowsMasked(d, steps, g, 4, 4, b, carried, pm).changed, "a part the copy left out is never pasted");
    }
  }
  // R8-01: a paste through the masks cuts the note still sounding, as a plain paste does (R7-06).
  {
    Song s = makeSong();
    const Grid g = gridOf(s, *s.pattern(kPat));
    std::vector<Note> d = {Note(0, 3 * 24, 48, 100)};  // rows 0 to 2
    std::vector<uint8_t> steps;
    const std::vector<Note> b = {Note(0, 24, 50, 100)};
    const bool all[kParts] = {true, true, true, true};
    Masks m;
    expect(pasteRowsMasked(d, steps, g, 1, 1, b, all, m).changed, "masked paste on row 1");
    expect(d.size() == 2 && d[0].length == 24 && d[1].start == 24, "the note sounding from row 0 ends where the pasted one starts");
    const Track t = buildTrack([&] { Song x = s; x.patterns[0].notes[kLead] = d; return x; }(), [&] { Pattern p = s.patterns[0]; p.notes[kLead] = d; return p; }(), kLead, g);
    expect(t.voices == 1, "one note column, not two");
  }
  // ---- an imported module's cells (Note::tracker) ----
  {
    using tracker::Cell;
    // A voice: row 0 C-4 instrument 1 volume 30 effect A0F, rows 1-2 empty, row 3 volume slide -2 and C20, then 4 empty rows.
    tracker::StreamHeader h;
    tracker::StreamWriter w(h);
    auto row = [&](Cell c) {
      tracker::StreamRow r;
      r.cell = c;
      w.row(r);
    };
    row(Cell{49, 1, 0x40, 0x0A, 0x0F});
    row(Cell{});
    row(Cell{});
    row(Cell{0, 0, 0x62, 0x0C, 0x20});
    for (int i = 0; i < 4; ++i) row(Cell{});
    const std::vector<uint8_t> bytes = w.finish();
    Song s = makeSong();
    const Grid g = gridOf(s, *s.pattern(kPat));
    std::vector<Note> notes = {Note(0, 8 * 24, 60, 100)};
    notes[0].tracker = bytes;
    const Track t = buildTrack(s, *s.pattern(kPat), kLead, g);
    // (the track is built from the song: give it the note)
    Song s2 = s;
    s2.patterns[0].notes[kLead] = notes;
    const Track t2 = buildTrack(s2, s2.patterns[0], kLead, g);
    (void)t;
    const std::vector<Cell> rows = moduleRows(notes[0], 64);
    expect(rows.size() == 8 && rows[0] == (Cell{49, 1, 0x40, 0x0A, 0x0F}) && rows[3].vol == 0x62, "module rows read back as written");
    expect(moduleVolText(0x40) == "30" && moduleVolText(0x50) == "40" && moduleVolText(0x62) == "-2" && moduleVolText(0xF3) == "M3" && moduleVolText(0).empty(),
           "volume column in the classic notation");
    expect(moduleEffectText(0x0A, 0x0F) == "A0F" && moduleEffectText(0, 0x37) == "037" && moduleEffectText(0, 0).empty() && moduleEffectText(0x21, 1) == "X01",
           "effects in the classic notation");
    int k = -1;
    expect(moduleNoteAt(notes, t2, g, 3, 0, &k) == 0 && k == 3, "row 3 is the voice's fourth row");
    expect(moduleNoteAt(notes, t2, g, 8, 0, &k) == -1, "past the note: no module cell");
    // Editing: set volume on an empty row splits the empty run; the rest reads the same.
    {
      std::vector<Note> d = notes;
      expect(setModuleVolume(d, t2, g, 1, 0, 0x20).changed, "a set volume typed on an empty module row");
      const std::vector<Cell> r2 = moduleRows(d[0], 64);
      expect(r2.size() == 8 && r2[1].vol == 0x30 && r2[0] == rows[0] && r2[3] == rows[3] && r2[2].empty(), "only that row changed");
      expect(setModuleVolume(d, t2, g, 1, 0, -1).changed && moduleRows(d[0], 64) == rows, "cleared again, the cells are as before");
      expect(!setModuleVolume(d, t2, g, 3, 0, 0x10).refused.empty(), "a volume column effect is not overwritten");
      expect(!setModuleEffect(d, t2, g, 0, 0, 0x0C, 0x10).refused.empty(), "an arpeggio / slide effect stays as imported");
      expect(setModuleEffect(d, t2, g, 3, 0, 0x0C, 0x30).changed && moduleRows(d[0], 64)[3].param == 0x30, "C on a C row: its value changes");
      expect(setModuleEffect(d, t2, g, 5, 0, 0x08, 0x80).changed && moduleRows(d[0], 64)[5].efx == 0x08, "8 on an empty row");
      expect(!setModuleEffect(d, t2, g, 6, 0, 0x0A, 0x01).refused.empty(), "other effects are not typed into module cells");
      Note probe;
      expect(!setModuleVolume(d, t2, g, 9, 0, 0x10).changed, "no module cell: nothing changes");
      (void)probe;
    }
    // In place: a set volume changed on a written row keeps the stream's length (the bytes move nowhere).
    {
      std::vector<uint8_t> b = bytes;
      expect(setStreamCell(b, 0, Cell{49, 1, 0x30, 0x0A, 0x0F}) && b.size() == bytes.size(), "a written row changes in place");
      expect(!setStreamCell(b, 40, Cell{0, 0, 0x20, 0, 0}), "no row 40: refused");
    }
    // R9-02, R9-04: edits that would cut, move or drop a voice's later cells are refused; nothing changes.
    {
      std::vector<Note> d = notes;
      std::vector<uint8_t> st;
      std::vector<StepProps> pr;
      expect(!putNote(d, t2, g, 5, 0, 72, 100).refused.empty() && d == notes, "a note typed inside the voice: refused");
      expect(!putOff(d, t2, g, 5, 0).refused.empty() && d == notes, "a key off inside the voice: refused");
      expect(!clearField(d, st, pr, t2, g, 0, 0, Field::Note).refused.empty() && d == notes, "the imported note deleted: refused");
      expect(!insertRow(d, st, pr, g, 3).refused.empty() && d == notes, "a row inserted through the voice: refused");
      expect(!deleteRow(d, st, pr, g, 0).refused.empty() && !deleteRow(d, st, pr, g, 3).refused.empty() && d == notes,
             "a row deleted on or through the voice: refused");
      expect(insertRow(d, st, pr, g, 0).changed && d[0].start == 24 && d[0].tracker == bytes, "a row inserted above it moves the whole voice");    }
  }
  // ---- R10-01: the "<name> Samples" copies are named relative to the project, so they move with it ----
  {
    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() / ("k106_r1001_" + std::to_string(static_cast<long long>(std::hash<std::string>{}(__FILE__) % 100000)));
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp / "cache" / "mod", ec);
    fs::create_directories(tmp / "proj", ec);
    { std::ofstream(tmp / "cache" / "mod" / "s.wav", std::ios::binary) << "RIFF0000WAVE"; }
    Song s;
    Channel c;
    c.id = 7, c.name = "S", c.plugin.type = "gl.sampler", c.plugin.data = (tmp / "cache" / "mod" / "s.wav").u8string();
    s.channels = {c};
    const std::string projDir = (tmp / "proj").u8string();
    const int n = adoptCacheFiles(s, (tmp / "cache").u8string(), (tmp / "proj" / "x Samples").u8string(), nullptr, projDir);
    expect(n > 0 && s.channels[0].plugin.data == "x Samples/s.wav", "the copy is named relative to the project");
    // The project's folder moved: the relative name resolves beside it.
    fs::rename(tmp / "proj", tmp / "moved", ec);
    Song m = s;
    resolveRelativePaths(m, (tmp / "moved").u8string());
    expect(m.channels[0].plugin.data == (tmp / "moved" / "x Samples" / "s.wav").lexically_normal().u8string(), "moved with its project, the copy is found");
    // A project saved with the old absolute name finds the copy beside its new place too.
    Song old = s;
    old.channels[0].plugin.data = (tmp / "proj" / "x Samples" / "s.wav").u8string();
    resolveRelativePaths(old, (tmp / "moved").u8string());
    expect(old.channels[0].plugin.data == (tmp / "moved" / "x Samples" / "s.wav").lexically_normal().u8string(), "an old absolute name is found beside the moved project");
    fs::remove_all(tmp, ec);
  }
  return finish();
}
