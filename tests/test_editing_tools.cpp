// J-11: channel rack / piano roll / playlist editing tools on synthetic notes,
// steps and clips, plus undo of the song-level operations.
#include <algorithm>
#include <cmath>
#include <string>
#include "Check.h"
#include "core/Document.h"
#include "core/NoteTools.h"
#include "core/Project.h"
#include "engine/Program.h"

using namespace gl;
using namespace gl::tools;

namespace {

Note N(Tick s, Tick l, int p, int v = 100) { return Note(s, l, p, v); }
NoteSel none(const std::vector<Note>& n) { return NoteSel(n.size(), false); }

Document makeDoc(ObjectId& pat, ObjectId& ch) {
  Document d(makeEmptySong());
  pat = d.song().patterns[0].id;
  ch = d.addChannel("Synth", "gl.trio");
  return d;
}

void testQuantize() {
  // Grid 24: 10 → 0 (strength 1), 30 → 24.
  std::vector<Note> n = {N(10, 30, 60), N(30, 20, 62)};
  auto s = none(n);
  QuantizeOpts o;
  o.grid = 24;
  quantizeEx(n, s, o);
  expect(n[0].start == 0 && n[1].start == 24, "quantize: starts to the nearest line");
  expect(n[0].length == 30, "quantize: lengths kept by default");
  // Half strength.
  n = {N(10, 30, 60)};
  s = none(n);
  o.startStrength = 0.5;
  quantizeEx(n, s, o);
  expect(n[0].start == 5, "quantize: strength 50% moves halfway");
  // Keep end times.
  n = {N(10, 30, 60)};
  s = none(n);
  o.startStrength = 1;
  o.duration = DurationMode::LeaveEnd;
  quantizeEx(n, s, o);
  expect(n[0].start == 0 && n[0].length == 40, "quantize: keep end times");
  // Quantize end times.
  n = {N(2, 45, 60)};
  s = none(n);
  o.duration = DurationMode::QuantizeEnd;
  quantizeEx(n, s, o);
  expect(n[0].start == 0 && n[0].length == 48, "quantize: end times to the grid");
  // Quantize lengths.
  n = {N(0, 30, 60)};
  s = none(n);
  o.duration = DurationMode::QuantizeLength;
  quantizeEx(n, s, o);
  expect(n[0].length == 24, "quantize: lengths to the grid");
  // Swing delays every second grid line.
  n = {N(25, 10, 60)};
  s = none(n);
  o = QuantizeOpts{};
  o.grid = 24;
  o.swing = 0.5;
  quantizeEx(n, s, o);
  expect(n[0].start == 30, "quantize: swing moves odd lines by swing * half a cell");
  // Window: far notes stay.
  n = {N(10, 10, 60), N(49, 10, 60)};
  s = none(n);
  o = QuantizeOpts{};
  o.grid = 24;
  o.window = 0.25;  // ±3 ticks
  quantizeEx(n, s, o);
  expect(n[0].start == 10 && n[1].start == 48, "quantize: window sensitivity leaves far notes");
  // Groove template shifts lines.
  o = QuantizeOpts{};
  o.grid = 24;
  o.groove = 2;  // heavy swing
  expect(quantizeTarget(o, 1) > 24 && quantizeTarget(o, 2) == 48, "quantize: groove template shifts odd lines");
  // Groove taken from notes.
  std::vector<Note> ref = {N(0, 5, 60), N(30, 5, 60), N(48, 5, 60), N(78, 5, 60)};
  auto shifts = extractGroove(ref, 24, 2);
  expect(shifts.size() == 2 && std::fabs(shifts[0]) < 1e-9 && std::fabs(shifts[1] - 0.25) < 1e-9, "quantize: groove extracted from notes");
  n = {N(23, 5, 60)};
  s = none(n);
  quantizeWithShifts(n, s, QuantizeOpts{}, shifts);
  expect(n[0].start == 30, "quantize: notes follow an extracted groove");
  // Selection only.
  n = {N(10, 10, 60), N(34, 10, 62)};
  s = {false, true};
  o = QuantizeOpts{};
  o.grid = 24;
  quantizeEx(n, s, o);
  expect(n[0].start == 10 && n[1].start == 24 && s[1] && !s[0], "quantize: only the selection, selection kept");
}

void testChopGlueStrum() {
  std::vector<Note> n = {N(0, 96, 60)};
  n[0].pan = 20;
  auto s = none(n);
  chopEvery(n, s, 24);
  expect(n.size() == 4 && n[3].start == 72 && n[3].length == 24, "chop: by grid");
  expect(std::all_of(n.begin(), n.end(), [](const Note& x) { return x.pan == 20; }), "chop: keeps note properties");
  // Pattern with pitch offsets (arpeggio-like chop).
  n = {N(0, 96, 60)};
  s = none(n);
  chopByPattern(n, s, chopPatterns()[10], 24);  // fifth climb 0,7,12,7
  expect(n.size() == 4 && n[0].pitch == 60 && n[1].pitch == 67 && n[2].pitch == 72 && n[3].pitch == 67, "chop: pattern pitch offsets");
  n = {N(0, 96, 60)};
  s = none(n);
  chopByPattern(n, s, chopPatterns()[5], 24);  // gallop 2,1,1
  expect(n.size() == 3 && n[0].length == 48 && n[1].start == 48 && n[2].start == 72, "chop: gallop lengths");
  // Glue undoes the chop.
  n = {N(0, 24, 60), N(24, 24, 60), N(48, 24, 60)};
  s = none(n);
  ops::glue(n, s);
  expect(n.size() == 1 && n[0].length == 72, "glue: joins touching notes");
  // Strum: chord of three, last note 24 ticks later, velocity reduced.
  n = {N(0, 96, 60), N(0, 96, 64), N(0, 96, 67)};
  s = none(n);
  StrumOpts so;
  so.time = 24;
  so.velocity = 0.5;
  strumEx(n, s, so);
  expect(n[0].start == 0 && n[1].start == 12 && n[2].start == 24, "strum: even offsets lowest first");
  expect(n[2].velocity == 50 && n[0].velocity == 100, "strum: velocity reduced towards the last note");
  expect(n[2].start + n[2].length == 96, "strum: ends stay by default");
  n = {N(0, 96, 60), N(0, 96, 64), N(0, 96, 67)};
  s = none(n);
  so = StrumOpts{};
  so.time = 24;
  so.direction = 1;
  so.end = 1;
  strumEx(n, s, so);
  const Note* top = nullptr;
  for (auto& x : n)
    if (x.pitch == 67) top = &x;
  expect(top && top->start == 0, "strum: downwards starts with the top note");
  bool endsMoved = true;
  for (auto& x : n) endsMoved = endsMoved && x.length == 96;
  expect(endsMoved, "strum: end strength 100% keeps lengths");
  so = StrumOpts{};
  so.time = 30;
  so.timeTension = 0.8;
  n = {N(0, 96, 60), N(0, 96, 64), N(0, 96, 67)};
  s = none(n);
  strumEx(n, s, so);
  expect(n[1].start < 15 && n[2].start == 30, "strum: tension bends the offsets");
}

void testArpFlamArticulate() {
  std::vector<Note> n = {N(0, 96, 60), N(0, 96, 64), N(0, 96, 67)};
  auto s = none(n);
  ArpOpts a;
  a.step = 24;
  a.gate = 0.5;
  arpeggiateEx(n, s, a);
  expect(n.size() == 4 && n[0].pitch == 60 && n[1].pitch == 64 && n[2].pitch == 67 && n[3].pitch == 60, "arp: up over the chord length");
  expect(n[0].length == 12, "arp: gate shortens notes");
  n = {N(0, 96, 60), N(0, 96, 64)};
  s = none(n);
  a = ArpOpts{};
  a.step = 24;
  a.octaves = 2;
  a.pattern = ArpPattern::Down;
  arpeggiateEx(n, s, a);
  expect(n.size() == 4 && n[0].pitch == 76 && n[1].pitch == 72 && n[2].pitch == 64, "arp: two octaves downwards");
  n = {N(0, 48, 60), N(0, 48, 64)};
  s = none(n);
  a = ArpOpts{};
  a.step = 24;
  a.pattern = ArpPattern::Chord;
  arpeggiateEx(n, s, a);
  expect(n.size() == 4, "arp: chord pattern repeats the whole chord");
  n = {N(0, 96, 60), N(0, 96, 64), N(0, 96, 67)};
  s = none(n);
  a = ArpOpts{};
  a.step = 24;
  a.pattern = ArpPattern::UpDown;
  arpeggiateEx(n, s, a);
  expect(n.size() == 4 && n[3].pitch == 64, "arp: up and down turns at the top");
  // Flam.
  n = {N(48, 24, 60)};
  s = none(n);
  FlamOpts f;
  f.time = 6;
  f.velocity = 0.5;
  flam(n, s, f);
  expect(n.size() == 2 && n[0].start == 42 && n[0].velocity == 50 && n[1].start == 48, "flam: grace note before the note");
  n = {N(48, 24, 60)};
  s = none(n);
  f.early = false;
  f.count = 2;
  flam(n, s, f);
  expect(n.size() == 3 && n[0].start == 48 && n[1].start == 54 && n[2].start == 60 && n[2].length == 12, "flam: double grace on the beat");
  expect(flamPresets().size() >= 4 && flamPresetOpts(0, 96).time == 8, "flam: presets scale with the PPQ");
  // Articulate.
  n = {N(0, 10, 60), N(48, 10, 62), N(96, 10, 64)};
  s = none(n);
  ArticulateOpts ar;
  ar.length = 1.0;
  articulate(n, s, ar);
  expect(n[0].length == 48 && n[1].length == 48 && n[2].length == 10, "articulate: connected fills to the next note");
  ar.length = 0.5;
  n = {N(0, 10, 60), N(48, 10, 62)};
  s = none(n);
  articulate(n, s, ar);
  expect(n[0].length == 24, "articulate: detached is half the distance");
  n = {N(0, 60, 60), N(48, 10, 62)};
  s = none(n);
  articulate(n, s, articulatePresetOpts(5, 96));
  expect(n[0].length == 48, "articulate: trim overlaps only");
}

void testRandomLfoLevelsLimit() {
  RandomOpts r;
  r.root = 0;
  r.scale = 1;  // major
  r.span = 384;
  r.step = 24;
  r.probability = 1.0;
  r.seed = 42;
  auto n = randomNotes(r);
  bool inKey = !n.empty();
  for (auto& x : n) inKey = inKey && inScale(x.pitch, 0, 1);
  expect(inKey && n.size() == 16, "randomize: one note per step, all in the scale");
  expect(randomNotes(r) == n, "randomize: same seed, same notes");
  r.seed = 43;
  expect(randomNotes(r) != n, "randomize: other seed, other notes");
  r.probability = 0;
  expect(randomNotes(r).empty(), "randomize: probability 0 gives nothing");
  // Level randomizer.
  std::vector<Note> m = {N(0, 24, 60), N(24, 24, 60), N(48, 24, 60)};
  auto s = none(m);
  LevelRandom lr;
  lr.strength[static_cast<int>(NoteProp::Pan)] = 1.0;
  lr.seed = 7;
  randomizeLevels(m, s, lr);
  bool panChanged = false, velSame = true;
  for (auto& x : m) panChanged = panChanged || x.pan != 0, velSame = velSame && x.velocity == 100;
  expect(panChanged && velSame, "randomize levels: only the chosen property");
  // LFO over velocity: square wave alternates.
  m.clear();
  for (int k = 0; k < 8; ++k) m.push_back(N(k * 24, 24, 60));
  s = none(m);
  NoteLfoOpts lo;
  lo.wave = LfoWave::Square;
  lo.startPeriod = lo.endPeriod = 48;
  lo.startRange = lo.endRange = 1.0;
  noteLfo(m, s, lo);
  expect(m[0].velocity == 127 && m[1].velocity == 1 && m[2].velocity == 127, "note LFO: square wave on velocity");
  // Scale levels: halve around 0.
  m = {N(0, 24, 60, 100), N(24, 24, 60, 50)};
  s = none(m);
  ScaleLevelOpts sl;
  sl.centre = 0;
  sl.scale = 0.5;
  scaleLevels(m, s, sl);
  expect(m[0].velocity == 50 && m[1].velocity == 25, "scale levels: multiply around the pivot");
  m = {N(0, 24, 60, 100), N(96, 24, 60, 100)};
  s = none(m);
  sl = ScaleLevelOpts{};
  sl.centre = 0;
  sl.rampFrom = 0;
  sl.rampTo = 1;
  scaleLevels(m, s, sl);
  expect(m[0].velocity == 1 && m[1].velocity == 100, "scale levels: ramp (fade in)");
  // Limit.
  m = {N(0, 24, 30), N(0, 24, 90), N(0, 24, 60)};
  s = none(m);
  limitKeys(m, s, 48, 72, LimitMode::Fold);
  bool inRange = true;
  for (auto& x : m) inRange = inRange && x.pitch >= 48 && x.pitch <= 72;
  expect(inRange && (m[0].pitch % 12) == (30 % 12), "limit: fold by octaves keeps pitch classes");
  m = {N(0, 24, 30), N(0, 24, 90), N(0, 24, 60)};
  s = none(m);
  limitKeys(m, s, 48, 72, LimitMode::Remove);
  expect(m.size() == 1 && m[0].pitch == 60, "limit: remove outside");
}

void testScalesChordsRiff() {
  expect(inScale(64, 0, 1) && !inScale(63, 0, 1), "scale: E in C major, D# not");
  expect(snapPitchToScale(61, 0, 1) == 60, "scale: C# snaps down to C");
  expect(snapPitchToScale(66, 0, 1) == 65, "scale: F# snaps to F");
  std::vector<Note> n = {N(0, 24, 61), N(0, 24, 63)};
  auto s = none(n);
  snapToScale(n, s, 0, 2);  // C minor: C# → C, D# stays
  expect(n[0].pitch == 60 && n[1].pitch == 63, "scale: snap notes to the key");
  selectInScale(n, s, 0, 1, false);
  expect(!s[0] && s[1], "scale: select notes out of the key");
  expect(chordPitches(60, 1) == std::vector<int>({60, 63, 67}), "chord: minor triad");
  expect(chordPitches(60, 0, 1) == std::vector<int>({64, 67, 72}), "chord: first inversion");
  Note b = N(96, 48, 62, 90);
  b.pan = 10;
  auto st = stampChord(b, 10);  // major 7th
  expect(st.size() == 4 && st[3].pitch == 73 && st[0].start == 96 && st[2].pan == 10, "chord stamp: notes with the base timing");
  RiffOpts r;
  r.bars = 4;
  r.barTicks = 384;
  r.step = 24;
  r.seed = 5;
  auto riff = generateRiff(r);
  bool ok = !riff.empty();
  Tick maxEnd = 0;
  for (auto& x : riff) ok = ok && inScale(x.pitch, 0, 1), maxEnd = std::max(maxEnd, x.start + x.length);
  expect(ok && maxEnd <= 4 * 384, "riff: in key, inside the bars");
  expect(generateRiff(r) == riff, "riff: repeatable per seed");
  r.chords = true;
  expect(generateRiff(r).size() > riff.size(), "riff: chords add notes");
  expect(pitchName(60) == "C4" && pitchName(61) == "C#4", "pitch names");
}

void testMiscNoteTools() {
  std::vector<Note> n = {N(0, 24, 60), N(24, 24, 62)};
  NoteSel s = {true, false};
  setNotesMuted(n, s, 1);
  expect(n[0].muted && !n[1].muted, "mute: selected notes");
  selectMuted(n, s);
  expect(s[0] && !s[1], "select muted notes");
  setNotesColor(n, s, 3);
  selectColor(n, s, 3);
  expect(s[0] && !s[1], "colour groups: set and select");
  s = none(n);
  setNotesSlide(n, s, 2);
  expect(n[0].slide && n[1].slide, "slide: toggles all when nothing is selected");
  setNotesPorta(n, s, 1);
  expect(n[0].porta, "porta: set");
  detune(n, s, 50);
  expect(n[0].fine == 50, "detune: fine pitch in cents");
  setNotesProp(n, s, NoteProp::Pan, 0.0);
  expect(n[0].pan == -kNotePanMax, "set property: pan hard left");
  expect(notesAt(n, 30) == std::vector<size_t>({1}), "notes at a position");
  // Muted notes do not play.
  Song song = makeEmptySong();
  Channel c;
  c.id = song.allocateId();
  song.channels.push_back(c);
  Pattern& p = song.patterns[0];
  p.notes[c.id] = {N(0, 24, 60), N(24, 24, 62)};
  p.notes[c.id][1].muted = true;
  expect(compileChannelNotes(song, p, song.channels[0]).size() == 1, "mute: muted notes are skipped by the engine");
}

void testSteps() {
  StepRow r(16, 0);
  StepPropRow pr;
  fillEvery(r, 4, 0);
  expect(r[0] && r[4] && r[8] && r[12] && !r[1], "steps: fill every 4");
  alignStepProps(r, pr);
  pr[0].pitch = 72;
  rotateSteps(r, pr, 1);
  expect(r[1] && !r[0] && pr[1].pitch == 72, "steps: rotate right moves parameters too");
  shiftSteps(r, pr, -2);
  expect(r[15] == 0 && r[3] && !r[1], "steps: shift left without wrapping");
  r.assign(16, 0);
  r[0] = 100;
  r[1] = 50;
  pr.assign(16, StepProps{});
  pr[1].pan = 10;
  reverseSteps(r, pr);
  expect(r[15] == 100 && r[14] == 50 && pr[14].pan == 10, "steps: reverse");
  invertSteps(r);
  expect(r[15] == 0 && r[0] == 100, "steps: invert");
  r.assign(16, 0);
  r[2] = 100;
  pr.assign(16, StepProps{});
  insertStep(r, pr, 1);
  expect(r[3] == 100 && r.size() == 16, "steps: insert step pushes later steps");
  deleteStep(r, pr, 0);
  expect(r[2] == 100 && r.size() == 16, "steps: delete step pulls later steps");
  euclidSteps(r, 3, 8, 0);
  int hits = 0;
  for (int k = 0; k < 8; ++k) hits += r[static_cast<size_t>(k)] > 0;
  expect(hits == 3 && r[0] && r[8], "steps: euclidean 3 of 8, repeated");
  randomizeSteps(r, 0.5, 60, 90, 9);
  bool velOk = true;
  for (auto v : r) velOk = velOk && (v == 0 || (v >= 60 && v <= 90));
  expect(velOk, "steps: random velocities in range");
  // Graph lanes.
  uint8_t vel = 100;
  StepProps sp;
  setStepLaneNorm(vel, sp, StepLane::Shift, 0.5);
  setStepLaneNorm(vel, sp, StepLane::Pitch, 72.0 / 127.0);
  expect(sp.shift == 50 && sp.pitch == 72 && vel == 100, "steps: graph lanes set shift and pitch");
  expect(std::fabs(stepLaneNorm(vel, sp, StepLane::Shift) - 0.5) < 1e-9, "steps: lane read back");
  // Steps ↔ notes round trip.
  r.assign(16, 0);
  pr.assign(16, StepProps{});
  r[0] = 100;
  r[4] = 80;
  pr[4].pitch = 67;
  pr[4].shift = 50;
  auto notes = stepsToNotes(r, pr, 24);
  expect(notes.size() == 2 && notes[1].pitch == 67 && notes[1].start == 96 + 12, "steps to notes: pitch and shift");
  StepRow r2;
  StepPropRow p2;
  std::vector<Note> rest;
  notes.push_back(N(5, 10, 60));  // off the step grid (5 ticks is not a whole percent of a step)
  notesToSteps(notes, 24, 16, r2, p2, &rest);
  expect(r2 == r && p2[4] == pr[4] && rest.size() == 1, "notes to steps: round trip, odd notes kept apart");
}

void testEngineStepProps() {
  Song song = makeEmptySong();
  Channel c;
  c.id = song.allocateId();
  song.channels.push_back(c);
  Pattern& p = song.patterns[0];
  p.steps[c.id] = std::vector<uint8_t>(16, 0);
  p.steps[c.id][0] = 100;
  p.steps[c.id][1] = 100;
  p.stepProps[c.id] = std::vector<StepProps>(16);
  p.stepProps[c.id][1].pitch = 72;
  p.stepProps[c.id][1].shift = 50;
  p.stepProps[c.id][1].pan = 32;
  auto ev = compileChannelNotes(song, p, song.channels[0]);
  bool ok = ev.size() == 2;
  if (ok) {
    const auto& e = ev[1].pitch == 72.0f ? ev[1] : ev[0];
    const Tick tps = song.ticksPerStep(p);
    ok = e.pitch == 72.0f && e.on == tps + tps / 2 && std::fabs(e.props.pan - 0.5f) < 1e-6f;
  }
  expect(ok, "engine: steps play their pitch, shift and pan");
}

void testDocOps() {
  ObjectId pat, ch;
  Document d = makeDoc(pat, ch);
  // Step row edits with props, undo.
  ops::editStepRow(d, pat, ch, "fill", [](StepRow& r, StepPropRow& p) {
    fillEvery(r, 4, 0);
    p[4].pitch = 70;
  });
  expect(d.song().pattern(pat)->steps.at(ch)[4] == 100 && d.song().pattern(pat)->stepProps.at(ch)[4].pitch == 70, "doc: step row edit");
  // Save / load keeps step parameters, note mute, track lock, marker actions.
  {
    Song s2 = d.song();
    s2.patterns[0].notes[ch] = {N(0, 24, 60)};
    s2.patterns[0].notes[ch][0].muted = true;
    s2.tracks[2].locked = true;
    Marker m;
    m.pos = 384;
    m.name = "Hook";
    m.action = MarkerAction::Loop;
    s2.markers.push_back(m);
    Song back;
    std::string err;
    bool ok = songFromJson(songToJson(s2), back, &err);
    expect(ok && back.patterns[0].stepProps == s2.patterns[0].stepProps, "project: step parameters saved");
    expect(ok && back.patterns[0].notes[ch][0].muted && back.tracks[2].locked, "project: muted notes and locked tracks saved");
    expect(ok && back.markers.size() == 1 && back.markers[0].action == MarkerAction::Loop, "project: marker actions saved");
  }
  expect(ops::convertStepsToNotes(d, pat, ch), "doc: steps to notes");
  {
    const Pattern* p = d.song().pattern(pat);
    expect(!p->steps.count(ch) && !p->stepProps.count(ch) && p->notes.at(ch).size() == 4 && p->notes.at(ch)[1].pitch == 70,
           "doc: steps became notes with their pitch");
  }
  expect(ops::convertNotesToSteps(d, pat, ch), "doc: notes to steps");
  expect(d.song().pattern(pat)->steps.at(ch)[4] == 100 && !d.song().pattern(pat)->notes.count(ch), "doc: notes became steps");
  d.undo();
  d.undo();
  expect(d.song().pattern(pat)->stepProps.at(ch)[4].pitch == 70, "doc: conversions undo");
  // Channels to a new pattern (move).
  size_t before = d.song().patterns.size();
  ObjectId np = ops::channelsToNewPattern(d, pat, {ch}, true);
  expect(np && d.song().patterns.size() == before + 1 && d.song().pattern(np)->steps.count(ch) && !d.song().pattern(pat)->steps.count(ch),
         "doc: selected channels moved to a new pattern");
  d.undo();
  expect(d.song().patterns.size() == before && d.song().pattern(pat)->steps.count(ch), "doc: new pattern undoes in one step");
  // Layer.
  ObjectId ch2 = d.addChannel("Bass", "gl.trio");
  ObjectId layer = ops::createLayer(d, {ch, ch2});
  expect(layer && d.song().channel(layer)->layer.size() == 2, "doc: layer from selected channels");
  ops::setSwingMix(d, {ch, ch2}, 0.25f);
  expect(d.song().channel(ch2)->swingMix == 0.25f, "doc: swing mix for selected");
  // Send notes to another channel.
  d.setNotes(pat, ch, {N(0, 24, 60), N(24, 24, 62)});
  ops::sendNotesToChannel(d, pat, ch, ch2, {N(24, 24, 62)}, true, {N(0, 24, 60)});
  expect(d.song().pattern(pat)->notes.at(ch).size() == 1 && d.song().pattern(pat)->notes.at(ch2).size() == 1, "doc: move notes to channel");
}

void testPlaylistOps() {
  ObjectId pat, ch;
  Document d = makeDoc(pat, ch);
  d.setNotes(pat, ch, {N(0, 96, 60), N(96, 96, 62), N(192, 96, 64), N(288, 96, 65)});
  const Tick bar = d.song().barTicks();
  ObjectId c = d.addClip(pat, 0, 0, bar * 2);
  // Slice.
  ObjectId r = ops::sliceClip(d, c, bar / 2);
  expect(r && d.song().clip(c)->length == bar / 2 && d.song().clip(r)->start == bar / 2 && d.song().clip(r)->offset == bar / 2,
         "playlist: slice splits with a content offset");
  expect(d.song().clip(r)->length == bar * 2 - bar / 2, "playlist: slice keeps the total length");
  d.undo();
  expect(d.song().playlist.size() == 1 && d.song().clip(c)->length == bar * 2, "playlist: slice undoes");
  // Chop into bars.
  auto parts = ops::chopClips(d, {c}, bar / 2);
  expect(parts.size() == 4 && d.song().playlist.size() == 4, "playlist: chop clip into pieces");
  d.undo();
  // Slip.
  ops::slipClip(d, c, 24);
  expect(d.song().clip(c)->offset == 24, "playlist: slip edit moves the content");
  ops::slipClip(d, c, -100);
  expect(d.song().clip(c)->offset == 0, "playlist: slip clamps at the content start");
  // Mute clips and tracks.
  ObjectId c2 = d.addClip(pat, 0, bar * 2);
  ops::muteClips(d, {c}, 1);
  expect(d.song().clip(c)->muted && !d.song().clip(c2)->muted, "playlist: mute tool");
  ops::muteTrackClips(d, 0, true);
  expect(d.song().clip(c2)->muted, "playlist: mute all clips on a track");
  // Same source.
  expect(ops::clipsOfSameSource(d.song(), c).size() == 2, "playlist: select clips of the same source");
  // Merge pattern clips on a track.
  ObjectId merged = ops::mergeTrackPatternClips(d, 0);
  expect(merged && d.song().playlist.size() == 1, "playlist: merge pattern clips of a track");
  d.undo();
  // Track lock / size.
  ops::setTrackLocked(d, 1, true);
  ops::setTrackHeights(d, {1, 2}, 0.5f);
  expect(d.song().tracks[1].locked && d.song().tracks[2].height == 0.5f, "playlist: lock and size tracks");
  // Markers.
  Marker m;
  m.pos = bar * 4;
  m.name = "Verse";
  int i0 = ops::addMarker(d, m);
  m.pos = bar;
  m.name = "Intro";
  int i1 = ops::addMarker(d, m);
  expect(i0 == 0 && i1 == 0 && d.song().markers[1].name == "Verse", "markers: kept sorted");
  ops::editMarker(d, 0, "marker action", [](Marker& k) { k.action = MarkerAction::Loop; });
  auto range = ops::markerLoopRange(d.song(), 0);
  expect(d.song().markers[0].action == MarkerAction::Loop && range.first == bar && range.second == bar * 4, "markers: loop range to the next");
  expect(ops::addMarkersEvery(d, 0, bar * 4, bar) == 3, "markers: every bar (skips existing)");
  ops::removeMarker(d, 0);
  expect(d.song().markers.size() == 4, "markers: delete");
  d.undo();
  expect(d.song().markers.size() == 5, "markers: delete undoes");
  // Split one clip by channel.
  ObjectId ch2 = d.addChannel("Hat", "gl.trio");
  d.setStep(pat, ch2, 0, 100);
  ObjectId other = d.addClip(pat, 3, 0);
  auto split = ops::splitClipByChannel(d, other);
  expect(split.size() == 2 && d.song().clip(c) && d.song().clip(c)->pattern == pat, "playlist: split one clip by channel");
}

}  // namespace

int main() {
  testQuantize();
  testChopGlueStrum();
  testArpFlamArticulate();
  testRandomLfoLevelsLimit();
  testScalesChordsRiff();
  testMiscNoteTools();
  testSteps();
  testEngineStepProps();
  testDocOps();
  testPlaylistOps();
  return finish();
}
