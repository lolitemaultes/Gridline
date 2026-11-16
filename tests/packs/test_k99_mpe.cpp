// Gridline — K-99 (with K-144 stage 2): full MPE. An MPE controller's per-note pitch, pressure
// and timbre recorded into the notes' lanes; the lanes edited (the piano roll's painting); MPE out
// of the MIDI Out instrument; MIDI files written and read as MPE.
#include <cmath>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "../Check.h"
#include "../Hermetic.h"
#include "core/Document.h"
#include "core/MidiFile.h"
#include "core/MidiImport.h"
#include "core/MidiRecord.h"
#include "core/MpeInput.h"
#include "core/NoteBend.h"
#include "core/NoteExprEdit.h"
#include "core/PluginInfo.h"
#include "core/PluginParams.h"
#include "engine/Generators.h"

using namespace gl;

namespace {

bool near(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

Song songWith(ObjectId& ch) {
  Song s = makeEmptySong();
  Channel c;
  c.id = ch = s.allocateId();
  c.name = "lead";
  c.mixerTrack = s.masterId();
  c.plugin = defaultPluginState("gl.synth");
  s.channels.push_back(c);
  return s;
}

void testMpeInput() {
  MpeInput in;
  in.bendRange = 48;
  MpeInput::Change ch[4];
  expect(in.feed(0xE0, 0, 0x40, ch, 4) == 0, "mpe input: a bend on the master channel is not a note's");
  expect(in.feed(0xE1, 0, 0x50, ch, 4) == 0, "mpe input: a member bend before its note: kept, nothing held");
  const int k = in.feed(0x91, 60, 100, ch, 4);
  expect(k == 1 && ch[0].dim == ExprDim::Pitch && ch[0].key == 60 && near(ch[0].value, 12.0, 0.01),
         "mpe input: a note starts with its channel's bend (a quarter of 48 semitones)");
  expect(in.held(1) == 60, "mpe input: the member channel holds the key");
  expect(in.feed(0xD1, 64, 0, ch, 4) == 1 && ch[0].dim == ExprDim::Pressure && near(ch[0].value, 64 / 127.0, 1e-6),
         "mpe input: channel pressure is the note's pressure");
  expect(in.feed(0xB1, 74, 127, ch, 4) == 1 && ch[0].dim == ExprDim::Timbre && near(ch[0].value, 1.0, 1e-6), "mpe input: CC 74 is its timbre");
  expect(in.feed(0xB1, 7, 127, ch, 4) == 0, "mpe input: other controllers are not");
  expect(in.feed(0xD2, 20, 0, ch, 4) == 0, "mpe input: another member channel without a note: nothing");
  in.feed(0x81, 60, 0, ch, 4);
  expect(in.held(1) == -1 && in.feed(0xD1, 10, 0, ch, 4) == 0, "mpe input: after its note off the channel moves nothing");
}

void testRecording() {
  ObjectId chId = kNoId;
  Document d(songWith(chId));
  const ObjectId pat = d.song().patterns[0].id;
  RecordOptions o;
  o.quantize = false;
  NoteRecorder r;
  r.beginPattern(d, pat, o, 96 * 16);
  r.noteOn(d, chId, 62, 100, 10);
  r.expression(chId, 62, ExprDim::Pressure, 0.2f, 10);
  r.expression(chId, 62, ExprDim::Pressure, 0.9f, 58);  // a press, 48 ticks in
  r.expression(chId, 62, ExprDim::Timbre, 0.75f, 34);
  for (int k = 0; k <= 24; ++k) r.expression(chId, 62, ExprDim::Pitch, static_cast<float>(k) / 12.0f, 10.0 + k);  // a 2 semitone slide
  r.expression(chId, 64, ExprDim::Pressure, 1.0f, 20);  // no note held there: ignored
  r.noteOff(d, chId, 62, 106);
  r.end(d, 120);
  const auto& ns = d.song().patterns[0].notes.at(chId);
  expect(ns.size() == 1 && ns[0].start == 10 && ns[0].length == 96, "recorded MPE: the note");
  const Note& n = ns[0];
  expect(!n.pressure.empty() && near(bend::value(n.pressure, 0), 0.2, 0.01) && near(bend::value(n.pressure, 60), 0.9, 0.01),
         "recorded MPE: pressure lane as played");
  expect(!n.timbre.empty() && near(bend::value(n.timbre, 30), 0.25, 0.01), "recorded MPE: timbre lane (stored from its centre)");
  expect(!n.bend.empty() && near(bend::value(n.bend, 12), 1.0, 0.05) && near(bend::value(n.bend, 40), 2.0, 0.05),
         "recorded MPE: the note's pitch as its bend line");
  expect(n.bend.size() <= static_cast<size_t>(bend::kMaxPoints) && n.bend.size() < 8, "recorded MPE: a ramp thinned to a few points");

  // Quantized: the lanes keep their timing on the performance.
  Document d2(songWith(chId));
  o.quantize = true;
  o.grid = 24;
  r.beginPattern(d2, d2.song().patterns[0].id, o, 96 * 16);
  r.noteOn(d2, chId, 60, 100, 30);
  r.expression(chId, 60, ExprDim::Pressure, 0.5f, 30);
  r.expression(chId, 60, ExprDim::Pressure, 1.0f, 54);
  r.noteOff(d2, chId, 60, 90);
  r.end(d2, 100);
  const Note& q = d2.song().patterns[0].notes.at(chId)[0];
  expect(q.start == 24 && near(bend::value(q.pressure, 6), 0.5, 0.02) && near(bend::value(q.pressure, 30), 1.0, 0.02),
         "recorded MPE: a quantized note's lanes move with it");
}

void testLaneEditing() {
  std::vector<Note> ns = {Note(0, 96, 60, 100), Note(192, 96, 64, 100)};
  expect(exprlane::valueAt(ns[0], ExprDim::Pressure, 10) == 0.0f && exprlane::valueAt(ns[0], ExprDim::Timbre, 10) == 0.5f,
         "expression lane: neutral without points");
  expect(exprlane::paint(ns, {}, ExprDim::Pressure, 20, 0.5f, 60, 0.5f, 2), "expression lane: a paint changes the note");
  expect(near(exprlane::valueAt(ns[0], ExprDim::Pressure, 40), 0.5, 1e-4) && near(exprlane::valueAt(ns[0], ExprDim::Pressure, 10), 0.0, 1e-4),
         "expression lane: the painted span takes the value, the start keeps its own");
  expect(ns[1].pressure.empty(), "expression lane: a note the sweep did not cross is untouched");
  exprlane::paint(ns, {}, ExprDim::Pressure, 0, 0.0f, 288, 1.0f, 2);  // a line over both
  expect(near(exprlane::valueAt(ns[0], ExprDim::Pressure, 48), 48.0 / 288.0, 0.01) && near(exprlane::valueAt(ns[1], ExprDim::Pressure, 48), 240.0 / 288.0, 0.01),
         "expression lane: a line across notes, each following it");
  expect(ns[0].pressure.size() <= static_cast<size_t>(bend::kMaxPoints) && ns[0].pressure.size() <= 4, "expression lane: a line is a few points");
  std::vector<bool> only = {false, true};
  exprlane::paint(ns, only, ExprDim::Timbre, 0, 1.0f, 288, 1.0f, 2);
  expect(ns[0].timbre.empty() && near(exprlane::valueAt(ns[1], ExprDim::Timbre, 30), 1.0, 1e-4), "expression lane: only the selected notes");
  expect(exprlane::clear(ns, {}, ExprDim::Pressure, -10, 400) && ns[0].pressure.empty() && ns[1].pressure.empty(),
         "expression lane: Alt over the notes takes them back to neutral");
  std::vector<BendPoint> lane;
  for (int k = 0; k < 200; ++k) lane.emplace_back(static_cast<float>(k), static_cast<float>(std::sin(k * 0.1) * 0.4));
  exprlane::simplify(lane, 0.002f, bend::kMaxPoints);
  expect(lane.size() <= static_cast<size_t>(bend::kMaxPoints) && lane.size() > 8, "expression lane: a dense lane fits the engine's slots");
}

void testMidiOutMpe() {
  auto g = createGenerator("gl.midiout");
  PluginState st = defaultPluginState("gl.midiout");
  expect(static_cast<int>(st.params.size()) == pp::MoCount && st.params[pp::MoMpe] == 0.0f, "MIDI Out: MPE is off by default");
  st.params[pp::MoMpe] = 1.0f;
  ChannelAux aux;
  ProcessContext ctx;
  ctx.sampleRate = 48000;
  std::vector<float> L(256), R(256);
  Generator::MidiOutEvent ev[1024];
  g->process(st, aux, ctx, L.data(), R.data(), 256);
  int k = g->takeMidiOut(ev, 1024);
  bool zone = false;
  for (int i = 0; i + 2 < k; ++i)
    if (ev[i].s == 0xB0 && ev[i].a == 101 && ev[i].b == 0 && ev[i + 1].a == 100 && ev[i + 1].b == 6 && ev[i + 2].a == 6 && ev[i + 2].b == 15) zone = true;
  expect(zone, "MIDI Out MPE: the zone is announced (RPN 6 = 15 member channels)");
  std::set<int> onChans;
  int c2 = -1, c1 = -1;
  bool press = false, cc74 = false, bend = false;
  // A fresh one, every message of every block collected (takeMidiOut keeps one block's).
  auto g2 = createGenerator("gl.midiout");
  g2->process(st, aux, ctx, L.data(), R.data(), 256);  // MPE on before the notes
  g2->noteOn(1, 60.0, 0.8f, 0);
  g2->noteOn(2, 64.0, 0.8f, 0);
  g2->process(st, aux, ctx, L.data(), R.data(), 256);
  g2->expr(2, ExprDim::Pressure, 0.5f, 0);
  g2->expr(2, ExprDim::Timbre, 1.0f, 0);
  g2->glide(1, 62.0, 0.001, 0);
  std::vector<Generator::MidiOutEvent> all;
  for (int b = 0; b < 3; ++b) {
    const int n = g2->takeMidiOut(ev, 1024);
    all.insert(all.end(), ev, ev + n);
    g2->process(st, aux, ctx, L.data(), R.data(), 256);
  }
  const int n = g2->takeMidiOut(ev, 1024);
  all.insert(all.end(), ev, ev + n);
  for (const auto& e : all) {
    if ((e.s & 0xF0) == 0x90 && e.b > 0) {
      onChans.insert(e.s & 15);
      (e.a == 60 ? c1 : c2) = e.s & 15;
    }
  }
  for (const auto& e : all) {
    if (e.s == (0xD0 | c2) && e.a == 64) press = true;
    if (e.s == (0xB0 | c2) && e.a == 74 && e.b == 127) cc74 = true;
    if (e.s == (0xE0 | c1) && ((e.a | (e.b << 7)) == 8192 + 2 * 8192 / 48)) bend = true;
  }
  std::printf("# MIDI Out MPE: notes on channels %d and %d\n", c1, c2);
  expect(onChans.size() == 2 && !onChans.count(0), "MIDI Out MPE: each note on a member channel of its own");
  expect(press && cc74, "MIDI Out MPE: pressure and timbre on the note's own channel");
  expect(bend, "MIDI Out MPE: a glide is the note's own channel bend (2 of 48 semitones)");
}

void testMidiFiles() {
  ObjectId chId = kNoId;
  Song s = songWith(chId);
  Note a(0, 96, 60, 100), b(48, 96, 67, 90);
  a.pressure = {BendPoint(0, 0.25f), BendPoint(48, 0.75f)};
  a.timbre = {BendPoint(0, 0.5f)};
  b.bend = {BendPoint(48, 2.0f)};
  b.pressure = {BendPoint(0, 0.5f)};
  s.patterns[0].notes[chId] = {a, b};
  const MidiFileData m = patternToMidi(s, s.patterns[0].id);
  expect(m.tracks.size() == 1, "MPE export: one track");
  const MidiTrack& t = m.tracks[0];
  std::set<int> chans;
  for (const auto& n : t.notes) chans.insert(n.channel);
  expect(chans.size() == 2 && !chans.count(0), "MPE export: overlapping notes on member channels of their own");
  expect(isMpeTrack(t), "MPE export: the track announces the zone");
  bool chanPress = false;
  for (const auto& c : t.ctls) chanPress = chanPress || c.controller == kMidiChannelPressure;
  expect(chanPress, "MPE export: channel pressure written");
  // Through bytes and back in.
  MidiFileData back;
  std::string err;
  expect(parseMidi(encodeMidi(m), back, &err) && back.tracks.size() == 1, "MPE export: the file reads back");
  splitTracksByChannel(back);
  expect(back.tracks.size() == 1, "MPE import: an MPE track is not split by channel");
  Song s2 = makeEmptySong();
  const MidiImportResult res = importMidiIntoSong(s2, back, "mpe");
  const Pattern* p = s2.pattern(res.pattern);
  expect(p && res.channels.size() == 1, "MPE import: one channel");
  if (!p || res.channels.empty()) return;
  const auto& ns = p->notes.at(res.channels[0]);
  expect(ns.size() == 2, "MPE import: both notes");
  if (ns.size() != 2) return;
  const double k = static_cast<double>(s2.ppq) / s.ppq;
  expect(near(bend::value(ns[0].pressure, 0), 0.25, 0.02) && near(bend::value(ns[0].pressure, 48 * k), 0.75, 0.02),
         "MPE import: the first note's pressure lane round trips");
  expect(near(bend::value(ns[0].timbre, 10 * k), 0.5, 0.02), "MPE import: its timbre lane");
  expect(near(bend::value(ns[1].bend, 60 * k), 2.0, 0.1) && near(bend::value(ns[1].pressure, 10 * k), 0.5, 0.02),
         "MPE import: the second note's bend (48 semitone range) and pressure, its own");
  expect(res.skippedControllers == 0, "MPE import: no controller left over");
  // Off: an ordinary track.
  MidiExportOptions off;
  off.mpe = 1;
  const MidiFileData plain = patternToMidi(s, s.patterns[0].id, off);
  expect(!isMpeTrack(plain.tracks[0]), "MPE export off: one channel as before");
  // Notes without expression stay ordinary.
  Song s3 = songWith(chId);
  s3.patterns[0].notes[chId] = {Note(0, 96, 60, 100)};
  expect(!isMpeTrack(patternToMidi(s3, s3.patterns[0].id).tracks[0]), "MPE export: plain notes stay a plain track");
}

}  // namespace

int main() {
  gl::test::hermeticTestEnv("k99_mpe");
  testMpeInput();
  testRecording();
  testLaneEditing();
  testMidiOutMpe();
  testMidiFiles();
  return finish();
}
