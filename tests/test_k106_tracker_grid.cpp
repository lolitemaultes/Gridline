// Gridline - K-106: the tracker view of a pattern (core/TrackerGrid.h).
//   - rows, beats and bars come from the pattern; notes get note columns by overlap; OFF is a note's end;
//   - typing a note, an OFF, values and effect letters changes only what was typed (the piano roll's notes);
//   - Delete, Insert and Backspace rows, transpose, block copy / cut / paste;
//   - rack steps show and edit as steps; notes off the row grid and past the end are kept, never moved;
//   - the edits go through the Document (undo) and the project file round-trips them;
//   - the order list is derived from the playlist; the play row follows a clip in song mode.
#include <string>
#include <vector>
#include "Check.h"
#include "core/Document.h"
#include "core/Json.h"
#include "core/Project.h"
#include "core/Song.h"
#include "core/TrackerGrid.h"

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
  expect(rowText(0x1F, true) == "1F" && rowText(31, false) == "31" && rowText(0x100, true) == "100", "row numbers in hex and decimal");
  expect(hex2(127) == "7F", "two hex digits");

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
    // A note pushed past the end is kept and counted (R16).
    for (int i = 0; i < 13; ++i) edit(doc, kPat, kLead, [](Rows& x, const Track&, const Grid& g) { return insertRow(x.notes, x.steps, x.props, g, 1); });
    const Track t = trackOf(doc, kPat, kLead);
    expect(notesOf(doc, kPat, kLead).size() == 2 && t.pastEnd == 1, "a note pushed past the pattern end is kept and marked");
    for (int i = 0; i < 13; ++i) edit(doc, kPat, kLead, [](Rows& x, const Track&, const Grid& g) { return deleteRow(x.notes, x.steps, x.props, g, 1); });
    expect(notesOf(doc, kPat, kLead) == before, "and comes back when the rows are deleted");
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
  return finish();
}
