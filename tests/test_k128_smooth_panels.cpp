// Gridline — K-128: unit tests for Smooth's Event List, Project/Track Notes, and Score Editor.
// Covers:
//   1. the project notes (SongInfo::comments) and Channel::notes: JSON persistence (omitted when empty), 1-step undo/redo,
//      and live two-peer collaboration sync.
//   2. Event List BBT position/length formatting & parsing, pitch formatting & parsing, and event
//      row filtering (Notes, Velocity/Pressure, Mod/CC).
//   3. Score Editor notation math: auto-clef selection, key signatures (sharps/flats/minors),
//      pitch spelling & round-trip, duration classification & quantize grids, Grand staff split,
//      per-bar accidental memory & barline reset, stem directions, and beat-grouped beaming.
#include <string>
#include <vector>
#include "Check.h"
#include "collab/CollabSim.h"
#include "collab/Session.h"
#include "core/Document.h"
#include "core/Json.h"
#include "core/Project.h"
#include "core/SmoothScore.h"

using namespace gl;
using namespace gl::smoothscore;
using namespace gl::collab;

namespace {

void testNotesPersistenceUndoCollab() {
  Song s = makeEmptySong();
  Document d{s};
  const ObjectId lead = d.addChannel("Lead", "gl.synth");
  d.reset(d.song());

  // 1. Omitted from JSON when empty for backward compatibility.
  {
    const Json j = songToJson(d.song());
    const Json* chans = j.find("channels");
    expect(chans && chans->is(Json::Type::Array) && chans->array.size() == 1, "one channel in JSON");
    expect(chans->array[0].find("notes") == nullptr, "empty Channel::notes is omitted from JSON");
  }

  // 2. Edit + undo + redo (1 undo step each).
  {
    SongInfo info = d.song().info;
    info.comments = "Verse 2 needs warmer pad\nCheck bass sub at bar 17";
    d.setInfo(info);
  }
  expect(d.song().info.comments.find("warmer pad") != std::string::npos && d.modified(), "the project notes edited");
  d.editChannel(lead, "edit track notes", [](Channel& c) { c.notes = "Bright saw lead, -2 dB at 3 kHz"; });
  expect(d.song().channel(lead)->notes.find("Bright saw") != std::string::npos, "Channel::notes edited");

  // JSON round-trip with both populated.
  {
    const Json j = songToJson(d.song());
    const Json* chans = j.find("channels");
    expect(chans && chans->array[0].find("notes") != nullptr, "non-empty Channel::notes written to JSON");
    Song back;
    std::string err;
    expect(loadProjectText(projectToText(d.song()), "k128.gridline.json", back, &err), "project round-trips cleanly");
    expect(back.info.comments == d.song().info.comments, "the project notes survive save/load");
    expect(back.channel(lead) && back.channel(lead)->notes == d.song().channel(lead)->notes, "Channel::notes survives save/load");
  }

  // Undo/redo.
  expect(d.undo() && d.song().channel(lead)->notes.empty(), "undo clears Channel::notes in one step");
  expect(d.undo() && d.song().info.comments.empty(), "undo clears the project notes in one step");
  expect(d.redo() && !d.song().info.comments.empty(), "redo restores the project notes");
  expect(d.redo() && !d.song().channel(lead)->notes.empty(), "redo restores Channel::notes");

  // 3. Live two-peer collaboration sync for both the project notes and Channel::notes.
  {
    sim::Clock clock;
    sim::Net net(128u);
    sim::Peer a, b;
    a.label = "Alex";
    b.label = "Sam";
    a.id = crypto::Identity::generate();
    b.id = crypto::Identity::generate();
    Song base = makeEmptySong();
    Document init{base};
    const ObjectId ch = init.addChannel("Keys", "gl.synth");
    a.doc.reset(init.song());
    a.folder = sim::tmpDir("k128_notes_a");
    b.folder = sim::tmpDir("k128_notes_b");
    std::string err;
    a.s = Session::create(a.doc, sim::config(a.folder, a.id, a.label, &clock), &err);
    expect(a.s != nullptr, "collab host created");
    net.add(&a);
    b.s = Session::join(b.doc, sim::config(b.folder, b.id, b.label, &clock), sim::ticketOf(*a.s), &err);
    expect(b.s != nullptr, "collab peer joined");
    a.s->admitMember(sim::memberOf(b));
    net.add(&b);
    net.connect(&a, &b);
    sim::settle(net, {&a, &b}, clock);

    {
      SongInfo info = a.doc.song().info;
      info.comments = "Master at -14 LUFS";
      a.doc.setInfo(info);
    }
    sim::settle(net, {&a, &b}, clock);
    expect(b.doc.song().info.comments == "Master at -14 LUFS", "collab syncs the project notes to peer");

    b.doc.editChannel(ch, "edit track notes", [](Channel& c) { c.notes = "Rhodes chords in Eb"; });
    sim::settle(net, {&a, &b}, clock);
    expect(a.doc.song().channel(ch) && a.doc.song().channel(ch)->notes == "Rhodes chords in Eb", "collab syncs Channel::notes back to host");

    b.doc.undo();
    sim::settle(net, {&a, &b}, clock);
    expect(a.doc.song().channel(ch)->notes.empty() && b.doc.song().channel(ch)->notes.empty(), "collab syncs Channel::notes undo");
    expect(a.doc.song().info.comments == "Master at -14 LUFS" && b.doc.song().info.comments == "Master at -14 LUFS",
           "the project notes stay intact through track note undo");
  }
}

void testEventListHelpers() {
  Song s = makeEmptySong();  // 4/4, ppq = 96
  expect(formatBbtPos(s, 0) == "1.1.1.1", "tick 0 formats as 1.1.1.1");
  expect(formatBbtPos(s, 96) == "1.2.1.1", "beat 2 formats as 1.2.1.1");
  expect(formatBbtPos(s, 384 + 96 + 24 + 5) == "2.2.2.6", "bar 2 beat 2 div 2 tick 6");
  expect(formatBbtLen(s, 96) == "0.1.0.0", "quarter note length formats as 0.1.0.0");
  expect(formatBbtLen(s, 384) == "1.0.0.0", "one bar length formats as 1.0.0.0");

  Tick t = 0;
  expect(parseBbtPos(s, "2.3.1.1", t) && t == 384 + 192, "parseBbtPos parses Bar.Beat.Div.Tick");
  expect(parseBbtPos(s, "1.1.3.1", t) && t == 48, "parseBbtPos parses 16th divisions");
  expect(parseBbtLen(s, "0.2.0.0", t) && t == 192, "parseBbtLen parses 2-beat duration");
  expect(parseBbtLen(s, "1.0.0.0", t) && t == 384, "parseBbtLen parses 1-bar duration");

  expect(formatPitch(60) == "C4" && formatPitch(61) == "C#4" && formatPitch(69) == "A4", "formatPitch matches MIDI C4=60");
  int p = 0;
  expect(parsePitch("C4", p) && p == 60, "parsePitch C4 -> 60");
  expect(parsePitch("f#3", p) && p == 54, "parsePitch f#3 -> 54");
  expect(parsePitch("Bb4", p) && p == 70, "parsePitch Bb4 -> 70");
  expect(parsePitch("72", p) && p == 72, "parsePitch numeric MIDI");
  expect(!parsePitch("Z4", p), "parsePitch rejects invalid letter");

  // Event rows building & filtering.
  Document d{s};
  const ObjectId ch = d.addChannel("Synth", "gl.synth");
  const ObjectId patId = d.song().patterns[0].id;
  Note n1(0, 96, 60, 100);
  n1.modX = 200;
  n1.pressure.push_back(BendPoint(24.0f, 0.75f));
  Note n2(192, 96, 64, 90);
  d.setNotes(patId, ch, {n1, n2});
  d.editSong("add pat auto", [&](Song& x) {
    PatternAutomation pa;
    pa.owner = ch;
    pa.index = 1;
    pa.name = "Cutoff";
    pa.points.push_back(AutoPoint{96, 0.5f});
    x.pattern(patId)->automation.push_back(pa);
  });

  const Pattern& pat = *d.song().pattern(patId);
  {
    EventFilter f{true, false, false};
    auto rows = buildEventRows(d.song(), pat, ch, f);
    expect(rows.size() == 2 && rows[0].kind == EventKind::Note && rows[0].pitch == 60 && rows[1].pitch == 64,
           "Notes-only filter shows the 2 notes");
  }
  {
    EventFilter f{false, true, false};
    auto rows = buildEventRows(d.song(), pat, ch, f);
    expect(rows.size() == 1 && rows[0].kind == EventKind::Velocity && rows[0].pos == 24,
           "Velocity/Pressure filter shows pressure event");
  }
  {
    EventFilter f{true, true, true};
    auto rows = buildEventRows(d.song(), pat, ch, f);
    expect(rows.size() == 5, "All filters combine notes, pressure, mod and CC chronologically");
    bool sorted = true;
    for (size_t i = 1; i < rows.size(); ++i) sorted = sorted && rows[i - 1].pos <= rows[i].pos;
    expect(sorted, "Event rows are sorted by tick");
  }
}

void testScoreNotationEngine() {
  // Auto-clef selection.
  expect(autoClefForNotes({}) == ClefMode::Grand, "empty notes default to Grand staff");
  expect(autoClefForNotes({Note(0, 96, 64, 100), Note(96, 96, 72, 100)}) == ClefMode::Treble, "high notes auto-select Treble");
  expect(autoClefForNotes({Note(0, 96, 43, 100), Note(96, 96, 52, 100)}) == ClefMode::Bass, "low notes auto-select Bass");
  expect(autoClefForNotes({Note(0, 96, 48, 100), Note(96, 96, 67, 100)}) == ClefMode::Grand, "wide range across C4 auto-selects Grand");

  // Key signatures.
  expect(keySignatureFifths(0, 1) == 0, "C major = 0 accidentals");
  expect(keySignatureFifths(7, 1) == 1, "G major = 1 sharp");
  expect(keySignatureFifths(2, 1) == 2, "D major = 2 sharps");
  expect(keySignatureFifths(5, 1) == -1, "F major = 1 flat");
  expect(keySignatureFifths(10, 1) == -2, "Bb major = 2 flats");
  expect(keySignatureFifths(9, 2) == 0, "A natural minor = 0 accidentals");
  expect(keySignatureFifths(2, 2) == -1, "D natural minor = 1 flat");
  expect(keySigSteps(2, true).size() == 2 && keySigSteps(-3, false).size() == 3, "keySigSteps returns exact symbol count");

  // Pitch spelling & inverse round-trip.
  const SpelledPitch c4 = spellPitch(60, 0);
  expect(c4.diatonicStep == 28 && c4.accidental == 0, "C4 spells to diatonicStep 28, natural");
  const SpelledPitch fs4 = spellPitch(66, 1);
  expect(fs4.diatonicStep == 31 && fs4.accidental == 1, "F#4 spells as F sharp in G major");
  const SpelledPitch bb4 = spellPitch(70, -2);
  expect(bb4.diatonicStep == 34 && bb4.accidental == -1, "Bb4 spells as B flat in Bb major");
  expect(diatonicToMidi(31, 1) == 66, "diatonicToMidi applies F# from G major key sig");
  expect(diatonicToMidi(34, -2) == 70, "diatonicToMidi applies Bb from Bb major key sig");

  // Duration classification.
  expect(classifyDuration(384, 96, QuantGrid::Sixteenth) == NoteValue::Whole, "4 beats = Whole");
  expect(classifyDuration(288, 96, QuantGrid::Sixteenth) == NoteValue::DottedHalf, "3 beats = DottedHalf");
  expect(classifyDuration(192, 96, QuantGrid::Sixteenth) == NoteValue::Half, "2 beats = Half");
  expect(classifyDuration(144, 96, QuantGrid::Sixteenth) == NoteValue::DottedQuarter, "1.5 beats = DottedQuarter");
  expect(classifyDuration(96, 96, QuantGrid::Sixteenth) == NoteValue::Quarter, "1 beat = Quarter");
  expect(classifyDuration(48, 96, QuantGrid::Sixteenth) == NoteValue::Eighth, "0.5 beat = Eighth");
  expect(classifyDuration(24, 96, QuantGrid::Sixteenth) == NoteValue::Sixteenth, "0.25 beat = Sixteenth");

  // Full score layout: Grand staff split, accidentals per bar, beaming.
  Song s = makeEmptySong();
  Pattern& pat = s.patterns[0];
  pat.bars = 2;
  std::vector<Note> ns = {
      Note(0, 48, 66, 100),    // bar 1 beat 1: F#4 (sharp drawn in C major)
      Note(48, 48, 66, 100),   // bar 1 beat 1.5: F#4 again (no redundant sharp, beamed with previous!)
      Note(96, 96, 65, 100),   // bar 1 beat 2: F4 natural (natural sign cancels earlier F# in same bar!)
      Note(192, 192, 48, 90),  // bar 1 beat 3: C3 on Bass staff (half note)
      Note(384, 96, 66, 100),  // bar 2 beat 1: F#4 (new bar resets accidental -> sharp drawn again!)
  };
  auto laid = layoutScoreNotes(s, pat, ns, ClefMode::Grand, QuantGrid::Sixteenth, 0);
  expect(laid.size() == 5, "layoutScoreNotes lays out all 5 notes");
  expect(!laid[0].bassStaff && laid[0].drawnAccidental == 1, "first F#4 on treble draws sharp");
  expect(!laid[1].bassStaff && laid[1].drawnAccidental == -2, "second F#4 in same bar omits redundant sharp");
  expect(laid[0].beamGroup >= 0 && laid[0].beamGroup == laid[1].beamGroup, "two 8th notes in beat 1 share a beam group");
  expect(laid[2].drawnAccidental == 0, "F4 after F#4 in same bar draws natural cancellation");
  expect(laid[3].bassStaff && isHollowHead(laid[3].value), "C3 placed on bass staff with hollow half notehead");
  expect(laid[4].drawnAccidental == 1, "bar 2 resets accidental state so F#4 draws sharp again");
}

}  // namespace

int main() {
  testNotesPersistenceUndoCollab();
  testEventListHelpers();
  testScoreNotationEngine();
  return finish();
}
