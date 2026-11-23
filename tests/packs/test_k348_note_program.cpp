// Gridline: a note's program change (the Tracker's I effect) on Patch Player channels.
//  * the effect letter: set, shown, clamped to 7F, reset, carried by a copy of the effect part,
//    its line in the help;
//  * saved and loaded with the project (old projects: none);
//  * played in a render: the note and the notes after it play the new program; a drum kit channel
//    picks another kit (the program stays in the chosen bank until a bank select says otherwise);
//  * written to a MIDI file as a program change at the note.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "../Check.h"
#include "../Hermetic.h"
#include "core/MidiFile.h"
#include "core/Project.h"
#include "core/TrackerGrid.h"
#include "engine/Render.h"
#include "patchplayer_rig.h"

using namespace gl;
using namespace gl::packs::samplers;
using namespace pptest;
using namespace sf2t;
namespace trk = gl::trk;

namespace {

// Programs 0, 1, 40 in bank 0 and a kit (128:0), each a DC level of its own.
std::string makeFont() {
  Bank b;
  b.name = "Levels";
  for (int i = 0; i < 4; ++i)
    b.samples.push_back(sf2t::Sample{"L" + std::to_string(i), dc(4410, static_cast<int16_t>(4096 * (i + 1))), {}, {}, 44100, 60, 0, 0, 4410, 1, 0});
  for (int i = 0; i < 4; ++i)
    b.insts.push_back(Inst{"I" + std::to_string(i), {sf2t::Zone{}.g(AttackVolEnv, -12000).g(ReleaseVolEnv, -12000).g(SampleModes, 1).g(SampleId, i)}});
  b.presets = {Preset{"One", 0, 0, {sf2t::Zone{}.g(Instrument, 0)}}, Preset{"Two", 1, 0, {sf2t::Zone{}.g(Instrument, 1)}},
               Preset{"Strings", 40, 0, {sf2t::Zone{}.g(Instrument, 2)}}, Preset{"Kit", 0, 128, {sf2t::Zone{}.g(Instrument, 3)}}};
  return writeBank(b, "k348 levels.sf2");
}

void testLetter() {
  Note n(0, 96, 60, 100);
  expect(n.program == -1 && !n.hasProps() && trk::effectsOf(n).empty(), "a note has no program change by default");
  expect(std::string(trk::kEffectLetters).find('I') != std::string::npos, "I is an effect letter");
  expect(trk::setEffect(n, 'I', 0x28) && n.program == 40 && n.hasProps(), "I28 sets program 40");
  const auto fx = trk::effectsOf(n);
  expect(fx.size() == 1 && fx[0].letter == 'I' && fx[0].value == 0x28 && fx[0].exact, "the effect column shows I28");
  expect(trk::setEffect(n, 'I', 0x9A) && n.program == 127, "a program past 7F stops at 7F");
  expect(trk::effectHelp('I', 0x28).find("program change to 40") != std::string::npos, "the status line says what I does");
  expect(trk::effectHelp(0, 0).find("I program change") != std::string::npos, "the letters' summary names I");
  Note to(0, 96, 62, 100);
  trk::copyPart(n, to, trk::Part::Effect);
  expect(to.program == 127 && !trk::partEmpty(n, trk::Part::Effect), "the effect part carries the program");
  trk::resetEffect(n, 'I');
  expect(n.program == -1 && trk::effectsOf(n).empty(), "Delete puts it back to none");
  Note both(0, 96, 60, 100);
  both.fine = 10;
  both.program = 3;
  const auto two = trk::effectsOf(both);
  expect(two.size() == 2 && two[0].letter == 'I' && two[1].letter == 'T', "letters keep their column order (I first)");
  expect(trk::kEffectLetters[0] == 'I', "I is the first letter of the effect column order");
  Note many(0, 96, 60, 100);
  many.slide = true;
  many.release = 20;
  many.program = 7;
  const auto three = trk::effectsOf(many);
  expect(three.size() == 3 && three[0].letter == 'I' && three[0].value == 7, "a note with a slide, a release and a program shows I in its effect field");
}

Song songWith(const std::string& font, ObjectId& pat, ObjectId& melodic, ObjectId& kit) {
  Song s = makeEmptySong();
  s.tempo = 120;
  s.ppq = 96;
  pat = s.patterns[0].id;
  auto add = [&](const char* name, int bank, int program) {
    Channel c;
    c.id = s.allocateId();
    c.name = name;
    c.plugin = defaultPluginState(kPatchPlayerType);
    setPatch(c.plugin, font, bank, program, name);
    c.plugin.params[PpVolume] = 0;
    c.mixerTrack = s.masterId();
    s.channels.push_back(c);
    return c.id;
  };
  melodic = add("Keys", 0, 0);
  kit = add("Kit", 128, 0);
  // Three notes a beat apart on each: the second changes the program, the third has none.
  for (ObjectId ch : {melodic, kit}) {
    std::vector<Note>& v = s.pattern(pat)->notes[ch];
    for (int k = 0; k < 3; ++k) v.push_back(Note(k * 96, 90, 60, 100));
    v[1].program = ch == melodic ? 40 : 5;
  }
  return s;
}

void testSaveLoad(const Song& s, ObjectId pat, ObjectId melodic) {
  const Json j = songToJson(s);
  Song back;
  std::string err;
  expect(songFromJson(j, back, &err), "the project loads");
  const auto& v = back.pattern(pat)->notes[melodic];
  expect(v.size() == 3 && v[0].program == -1 && v[1].program == 40 && v[2].program == -1, "the program change saves and loads");
  expect(jsonStringify(j, 0).find("\"program\"") != std::string::npos, "saved as the note's program");
  // A project saved before has no program on its notes.
  Song old = s;
  for (auto& [ch, notes] : old.pattern(pat)->notes)
    for (auto& n : notes) n.program = -1;
  expect(jsonStringify(songToJson(old), 0).find("\"program\"") == std::string::npos, "notes without one save as before");
}

// Level of channel `ch` alone in a pattern render, at the middle of each beat's note.
std::vector<float> levels(const Song& s0, ObjectId pat, ObjectId ch) {
  Song s = s0;
  for (auto& c : s.channels) c.muted = c.id != ch;
  RenderJob job;
  job.scope = RenderScope::Pattern;
  job.pattern = pat;
  job.tail = TailMode::Cut;
  const RenderResult r = renderAudio(s, job);
  std::vector<float> out;
  for (int k = 0; k < 3; ++k) {
    const size_t at = static_cast<size_t>(k) * 22050 + 11000;
    out.push_back(at < r.audio.frames ? std::fabs(r.audio.plane(0)[at]) : -1.f);
  }
  return out;
}

void testRender(const Song& s, ObjectId pat, ObjectId melodic, ObjectId kit) {
  const std::vector<float> m = levels(s, pat, melodic);
  std::printf("# melodic levels %.5f %.5f %.5f\n", m[0], m[1], m[2]);
  expect(m[0] > 1e-4f && std::fabs(m[1] / m[0] - 3.0f) < 1e-3f, "the note with I28 plays program 40 (three times program 0's level)");
  expect(std::fabs(m[2] / m[0] - 3.0f) < 1e-3f, "the note after it keeps program 40");
  const std::vector<float> k = levels(s, pat, kit);
  std::printf("# kit levels %.5f %.5f %.5f\n", k[0], k[1], k[2]);
  expect(k[0] > 1e-4f && std::fabs(k[1] / k[0] - 1.0f) < 1e-3f && std::fabs(k[2] / k[0] - 1.0f) < 1e-3f,
         "on a drum kit channel the program stays in the kit bank (the nearest kit), never a melodic patch");
  // Without the program change the render plays program 0 throughout.
  Song plain = s;
  plain.pattern(pat)->notes[melodic][1].program = -1;
  const std::vector<float> p = levels(plain, pat, melodic);
  expect(std::fabs(p[1] / p[0] - 1.0f) < 1e-3f && std::fabs(p[1] - m[0]) < 1e-5f, "no program change: program 0 throughout");
}

void testBankStays(const std::string& font) {
  Rig kit(font, 128, 0);
  kit.gen->midi(0xC0, 5, 0, 0);
  kit.gen->noteOn(1, 60, 1.f, 0);
  std::vector<float> L, R;
  kit.run(L, R, 2000);
  expect(std::fabs(L[1500] - 0.5f) < 1e-3f, "a program change without a bank select stays in the chosen bank");
  kit.gen->noteOff(1, 0);
  kit.run(L, R, 500);
  kit.gen->midi(0xB0, 0, 0, 0);  // bank select 0: the melodic bank
  kit.gen->midi(0xC0, 40, 0, 0);
  kit.gen->noteOn(2, 60, 1.f, 0);
  std::vector<float> L2, R2;
  kit.run(L2, R2, 2000);
  expect(std::fabs(L2[1500] - 0.375f) < 1e-3f, "a bank select still chooses the bank for the next program change");
}

void testMidiExport(const Song& s, ObjectId pat) {
  const MidiFileData d = patternToMidi(s, pat);
  bool found = false;
  for (const auto& tr : d.tracks)
    for (const auto& c : tr.programs) found = found || (c.tick == 96 && c.value == 40 && c.controller == kMidiProgram && c.channel == tr.channel);
  expect(found, "MIDI export: a program change at the note");
  const std::string path = tmpPath("k348 programs.mid");
  std::string err;
  MidiFileData back;
  expect(writeMidiFile(path, d, &err) && readMidiFile(path, back, &err), "MIDI file written and read");
  bool again = false;
  for (const auto& tr : back.tracks)
    for (const auto& c : tr.programs) again = again || (c.tick == 96 && c.value == 40);
  expect(again, "MIDI export: the program change is in the file");
}

}  // namespace

int main() {
  gl::test::hermeticTestEnv("k348_note_program");
  const std::string font = makeFont();
  testLetter();
  ObjectId pat = kNoId, melodic = kNoId, kit = kNoId;
  const Song s = songWith(font, pat, melodic, kit);
  testSaveLoad(s, pat, melodic);
  testRender(s, pat, melodic, kit);
  testBankStays(font);
  testMidiExport(s, pat);
  return finish();
}
