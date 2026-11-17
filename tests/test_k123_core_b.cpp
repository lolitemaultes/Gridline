// Gridline - K-123 core B (0.7.7 recording rows): take ratings, solo and the takes list; the MIDI input
// filter; Recover what I just played; channels armed for notes. Everything runs on simulated input.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include "Check.h"
#include "collab/Crdt.h"
#include "collab/LocalState.h"
#include "core/Document.h"
#include "core/MidiRecord.h"
#include "core/NoteExprEdit.h"
#include "core/Project.h"
#include "core/Settings.h"
#include "engine/TakeLanes.h"

using namespace gl;

namespace {

constexpr Tick kBar = 384;  // makeEmptySong: 4/4, 96 ticks a beat

std::vector<Note> notesOf(const Song& s, ObjectId pat, ObjectId ch) {
  const Pattern* p = s.pattern(pat);
  if (!p) return {};
  auto it = p->notes.find(ch);
  if (it == p->notes.end()) return {};
  std::vector<Note> v = it->second;
  std::sort(v.begin(), v.end(), [](const Note& a, const Note& b) { return a.start < b.start || (a.start == b.start && a.pitch < b.pitch); });
  return v;
}

bool has(const std::vector<Note>& v, Tick start, int pitch, Tick length = -1) {
  for (const Note& n : v) {
    if (n.start == start && n.pitch == pitch && (length < 0 || n.length == length)) return true;
  }
  return false;
}

RecordOptions raw() {  // no quantize: positions as played
  RecordOptions o;
  o.quantize = false;
  o.grid = 1;
  return o;
}

// Three takes of one group on track 2 (pattern clips stand in for audio: the lanes only look at the
// take group and number), the newest playing, as loop recording leaves them.
Song takesSong() {
  Song s = makeEmptySong();
  const ObjectId pat = s.patterns[0].id;
  for (int k = 0; k < 3; ++k) {
    Clip c;
    c.id = s.nextId++;
    c.kind = ClipKind::Pattern;
    c.pattern = pat;
    c.track = 2;
    c.start = kBar;
    c.length = kBar;
    c.takeGroup = 9000;
    c.take = k;
    c.muted = k != 2;
    s.playlist.push_back(c);
  }
  s.tracks[2].takeLanes = true;
  return s;
}

std::string takesOn(const Song& s, int track) {
  std::string out;
  for (const Clip& c : s.playlist) {
    if (c.track == track && c.takeGroup != kNoId && !c.muted) out += std::to_string(c.take);
  }
  return out;
}

// ---- 1: ratings, solo and the takes list ----
void testRatings() {
  Document doc;
  doc.reset(takesSong());
  const Song before = doc.song();
  const std::string plain = jsonCompact(songToJson(before));
  expect(plain.find("takeRating") == std::string::npos, "a song without ratings saves no rating field");
  doc.editSong("rate take", [](Song& x) { takes::setRating(x, 2, 1, 2); });
  expect(takes::rating(doc.song(), 2, 1) == 2 && takes::rating(doc.song(), 2, 0) == 0, "take 2 rated two dots, take 1 none");
  Song other = before;
  expect(!takes::setRating(other, 5, 0, 1), "rating a track without takes changes nothing");
  Song tmp = doc.song();
  expect(!takes::setRating(tmp, 2, 1, 2), "rating it the same again changes nothing");
  expect(takes::setRating(tmp, 2, 1, 7) && takes::rating(tmp, 2, 1) == 3, "a rating is at most three dots");
  // The takes list.
  const std::vector<takes::TakeInfo> list = takes::takeList(doc.song(), 2);
  expect(list.size() == 3, "the takes list: three takes");
  if (list.size() == 3) {
    expect(list[1].rating == 2 && !list[1].inComp && list[2].inComp && list[0].start == kBar && list[0].end == 2 * kBar,
           "the takes list: ratings, which take is in the comp, the range");
  }
  expect(takes::tracksWithTakes(doc.song()) == std::vector<int>{2}, "the tracks with takes");
  // Undo, save and load.
  doc.undo();
  expect(takes::rating(doc.song(), 2, 1) == 0, "one undo takes the rating back");
  doc.redo();
  Song back;
  std::string err;
  expect(songFromJson(songToJson(doc.song()), back, &err) && takes::rating(back, 2, 1) == 2, "the rating is saved and loaded");
  // A comp edit splits a take: every piece keeps the rating.
  doc.editSong("comp", [](Song& x) { takes::compRange(x, 2, 1, kBar, kBar + kBar / 2); });
  int pieces = 0, rated = 0;
  for (const Clip& c : doc.song().playlist) {
    if (c.takeGroup == 9000 && c.take == 1) ++pieces, rated += c.takeRating == 2 ? 1 : 0;
  }
  expect(pieces >= 1 && rated == pieces, "comping keeps the rating on every piece of the take");
}

void testSoloKeepsComp() {
  Song s = takesSong();
  const Song song = s;
  Song played = s;
  expect(takes::listenTo(played, 2, 0), "solo take 1");
  expect(takesOn(played, 2) == "0", "the engine's copy plays only take 1");
  expect(takesOn(song, 2) == "2" && song.playlist == s.playlist, "the song and its comp are untouched");
  Song none = s;
  expect(!takes::listenTo(none, 2, 7) && none.playlist == s.playlist, "a take that is not there: nothing changes");
}

// Ratings reach a collaborator: the change replicates as ops (masked for the session as Gridline sends it).
void testRatingsShared() {
  using namespace collab;
  Song base = takesSong();
  Json j0 = songToJson(base, false);
  maskSessionJson(j0);
  Replica a, b;
  uint64_t t = 1;
  auto next = [&t] { return ++t; };
  std::vector<Op> ops;
  a.diff({"doc"}, j0, 1, next, ops);
  for (const Op& op : ops) {
    a.apply(op, 1);
    b.apply(op, 1);
  }
  Song rated = base;
  takes::setRating(rated, 2, 0, 3);
  Json j1 = songToJson(rated, false);
  maskSessionJson(j1);
  ops.clear();
  a.diff({"doc"}, j1, 1, next, ops);
  expect(!ops.empty(), "collab: a rating is a song edit (ops to send)");
  for (const Op& op : ops) b.apply(op, 1);
  Song peer;
  std::string err;
  expect(songFromJson(b.materialize({"doc"}), peer, &err) && takes::rating(peer, 2, 0) == 3, "collab: the peer has the rating");
}

// ---- 2: the MIDI input filter ----
void testFilter() {
  RecordOptions o;
  expect(o.notes && o.automation && o.bend && o.pressure && o.program, "the filter: everything records by default");
  expect(midiRecordKind(0x90, 60) == MidiRecordKind::Note && midiRecordKind(0x81, 60) == MidiRecordKind::Note &&
             midiRecordKind(0xB0, 64) == MidiRecordKind::Note && midiRecordKind(0xB3, 1) == MidiRecordKind::Controller &&
             midiRecordKind(0xE0, 0) == MidiRecordKind::PitchBend && midiRecordKind(0xD0, 0) == MidiRecordKind::Pressure &&
             midiRecordKind(0xA0, 60) == MidiRecordKind::Pressure && midiRecordKind(0xC5, 3) == MidiRecordKind::Program &&
             midiRecordKind(0xF8, 0) == MidiRecordKind::Other,
         "message kinds: notes (and the pedal), controllers, bend, pressure, program");
  o.bend = false;
  o.program = false;
  expect(!recordFilterTakes(o, 0xE0, 0) && !recordFilterTakes(o, 0xC0, 1) && recordFilterTakes(o, 0xB0, 1) && recordFilterTakes(o, 0xD0, 0) &&
             recordFilterTakes(o, 0x90, 60),
         "bend and program off: they do not record, the rest does");
  expect(recordFilterText(o) == "notes 1 controllers 1 bend 0 pressure 1 program 0", "the filter as text");
  // An MPE note's bend and pressure follow the filter.
  for (int pass = 0; pass < 2; ++pass) {
    Document doc(makeEmptySong());
    const ObjectId ch = doc.addChannel("Keys", "gl.synth");
    const ObjectId pat = doc.song().patterns[0].id;
    RecordOptions r = raw();
    r.bend = pass == 1;
    r.pressure = pass == 0;
    NoteRecorder rec;
    rec.beginPattern(doc, pat, r, kBar);
    rec.noteOn(doc, ch, 60, 100, 0);
    rec.expression(ch, 60, ExprDim::Pitch, 1.0f, 10);
    rec.expression(ch, 60, ExprDim::Pitch, 2.0f, 40);
    rec.expression(ch, 60, ExprDim::Pressure, 0.5f, 20);
    rec.expression(ch, 60, ExprDim::Pressure, 0.9f, 50);
    rec.noteOff(doc, ch, 60, 96);
    rec.end(doc, 100);
    const std::vector<Note> v = notesOf(doc.song(), pat, ch);
    const bool bendKept = !v.empty() && !exprlane::laneOf(v[0], ExprDim::Pitch).empty();
    const bool pressKept = !v.empty() && !exprlane::laneOf(v[0], ExprDim::Pressure).empty();
    expect(v.size() == 1 && bendKept == (pass == 1) && pressKept == (pass == 0),
           pass == 0 ? "bend off, pressure on: the note keeps its pressure only" : "bend on, pressure off: the note keeps its bend only");
  }
  // Each person's own: kept in this computer's settings.
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "gridline_k123b_settings";
  fs::create_directories(dir);
  const std::string file = (dir / "settings.json").string();
  {
    Settings s;
    RecordOptions d;
    int bars = 0;
    bool punch = false;
    loadRecordOptions(s, d, bars, punch);
    expect(d.bend && d.pressure && d.program, "settings: the filter's defaults (everything on)");
    d.bend = false;
    d.pressure = false;
    saveRecordOptions(s, d, bars, punch);
    s.save(file);
  }
  Settings back;
  back.load(file);
  RecordOptions d;
  int bars = 0;
  bool punch = false;
  loadRecordOptions(back, d, bars, punch);
  expect(!d.bend && !d.pressure && d.program && d.notes && d.automation, "settings: the filter survives a save and load");
  std::error_code ec;
  fs::remove_all(dir, ec);
}

// ---- 3: Recover what I just played ----
void testRecoverPattern() {
  Document doc(makeEmptySong());
  const ObjectId ch = doc.addChannel("Keys", "gl.synth");
  const ObjectId pat = doc.song().patterns[0].id;
  NoteCapture cap;
  CaptureTime at{CaptureTime::Pattern, 96, pat};
  cap.noteOn(ch, 60, 100, 1.0, at);
  cap.noteOff(ch, 60, 1.5, 192);
  at.tick = 300;
  cap.noteOn(ch, 64, 90, 2.0, at);
  cap.noteOff(ch, 64, 2.2, 340);
  at.tick = 20;  // the pattern looped
  cap.noteOn(ch, 67, 80, 2.5, at);
  cap.noteOff(ch, 67, 2.7, 60);
  const uint64_t undoBefore = doc.history().stateSerial();
  const NoteCapture::Recovered r = cap.recover(doc, pat, 120, raw(), 3.0);
  const std::vector<Note> v = notesOf(doc.song(), pat, ch);
  expect(r.timed && r.notes == 3 && r.pattern == pat, "pattern mode: three notes put back in time");
  expect(has(v, 96, 60, 96) && has(v, 300, 64, 40) && has(v, 20, 67, 40), "each where it was heard (also after the loop wrapped)");
  expect(doc.history().stateSerial() != undoBefore && doc.history().undoLabel() == "recover played notes", "one undo step");
  expect(cap.size() == 0, "what was kept is used up");
  doc.undo();
  expect(notesOf(doc.song(), pat, ch).empty(), "one undo removes them");
}

void testRecoverSong() {
  Document doc(makeEmptySong());
  const ObjectId ch = doc.addChannel("Keys", "gl.synth");
  const ObjectId pat = doc.song().patterns[0].id;
  NoteCapture cap;
  CaptureTime at{CaptureTime::Song, 2 * kBar + 48, kNoId};
  cap.noteOn(ch, 60, 100, 10.0, at);
  cap.noteOff(ch, 60, 10.2, 2 * kBar + 120);
  at.tick = 3 * kBar + 10;
  cap.noteOn(ch, 62, 100, 11.0, at);
  cap.noteOff(ch, 62, 11.1, 3 * kBar + 50);
  const NoteCapture::Recovered r = cap.recover(doc, pat, 120, raw(), 12.0);
  const Clip* c = doc.song().clip(r.clip);
  expect(r.timed && r.notes == 2 && c && c->start == 2 * kBar, "song mode: a take clip at the bar the phrase was played in");
  const std::vector<Note> v = notesOf(doc.song(), r.pattern, ch);
  expect(has(v, 48, 60, 72) && has(v, kBar + 10, 62, 40), "the notes at their places in the take");
}

void testRecoverStopped() {
  Document doc(makeEmptySong());
  const ObjectId ch = doc.addChannel("Keys", "gl.synth");
  const ObjectId pat = doc.song().patterns[0].id;
  NoteCapture cap;
  cap.noteOn(ch, 60, 100, 0.0);  // played while the transport stood still
  cap.noteOff(ch, 60, 100.0);
  cap.noteOn(ch, 61, 100, 300.0);  // two minutes later: the first is forgotten
  expect(cap.size() == 1, "notes are kept for two minutes");
  cap.noteOff(ch, 61, 300.5);
  cap.noteOn(ch, 62, 100, 301.0);
  cap.noteOff(ch, 62, 301.25);
  const NoteCapture::Recovered r = cap.recover(doc, pat, 120, raw(), 302.0);
  const std::vector<Note> v = notesOf(doc.song(), pat, ch);
  // 120 BPM, 96 ticks a beat: 192 ticks a second.
  expect(!r.timed && r.notes == 2 && has(v, 0, 61, 96) && has(v, 192, 62, 48), "stopped: the phrase from the pattern's start, timed as played");
  // A phrase partly played while playing and partly while stopped goes in the same way.
  NoteCapture mix;
  mix.noteOn(ch, 70, 100, 1.0, CaptureTime{CaptureTime::Pattern, 50, pat});
  mix.noteOff(ch, 70, 1.1, 70);
  mix.noteOn(ch, 71, 100, 1.5);
  mix.noteOff(ch, 71, 1.6);
  expect(!mix.recover(doc, pat, 120, raw(), 2.0).timed, "a phrase partly played while stopped: placed from the start");
}

// ---- 4: channels armed for notes ----
void testArming() {
  const ObjectId a = 11, b = 12, c = 13;
  expect(armedLiveChannel({}, a) == a && armedRecords({}, b), "nothing armed: the selected channel plays, every channel records");
  expect(armedLiveChannel({b, c}, a) == b && armedLiveChannel({b, c}, c) == c, "armed: the selected one when armed, else the first armed");
  expect(armedRecords({b, c}, c) && !armedRecords({b, c}, a), "armed: only the armed channels record");
  // Two armed channels record at once into one take (pattern mode): each note on its own channel.
  Document doc(makeEmptySong());
  const ObjectId k1 = doc.addChannel("Keys", "gl.synth");
  const ObjectId k2 = doc.addChannel("Pad", "gl.synth");
  const ObjectId pat = doc.song().patterns[0].id;
  NoteRecorder rec;
  rec.beginPattern(doc, pat, raw(), kBar);
  for (ObjectId ch : {k1, k2}) rec.noteOn(doc, ch, 60, 100, 24);
  for (ObjectId ch : {k1, k2}) rec.noteOff(doc, ch, 60, 120);
  rec.end(doc, 130);
  expect(has(notesOf(doc.song(), pat, k1), 24, 60, 96) && has(notesOf(doc.song(), pat, k2), 24, 60, 96), "both armed channels have the note");
  doc.undo();
  expect(notesOf(doc.song(), pat, k1).empty() && notesOf(doc.song(), pat, k2).empty(), "one undo removes the take on both");
}

}  // namespace

int main() {
  testRatings();
  testSoloKeepsComp();
  testRatingsShared();
  testFilter();
  testRecoverPattern();
  testRecoverSong();
  testRecoverStopped();
  testArming();
  return finish();
}
