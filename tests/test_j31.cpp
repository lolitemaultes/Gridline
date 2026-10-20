// Gridline — J-31: the largest FL feature gaps from the menu audit.
// Chord progression editor core (ChordTools), groups and loops, piano roll
// extras, recording extras (output taps, audio logger, silent takes,
// naming), key markers, quantized jumps, truncated swing, alt solo and
// export by playlist track group. Synthetic songs only.
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "core/ChordTools.h"
#include "core/Document.h"
#include "core/Grouping.h"
#include "core/MenuParity.h"
#include "core/NoteTools.h"
#include "core/Project.h"
#include "core/SongTools.h"
#include "engine/AudioEngine.h"
#include "engine/AudioLogger.h"
#include "engine/AudioRecord.h"
#include "engine/Program.h"
#include "engine/Render.h"

using namespace gl;
namespace ch = gl::chord;

namespace {

Document makeDoc() { return Document(makeEmptySong()); }

ch::Chord parsed(const std::string& t, int key = 0, int scale = 1) {
  ch::Chord c;
  c.quality = -1;
  if (!ch::parseChord(t, key, scale, c)) c.quality = -1;
  return c;
}

std::vector<int> pitchesAt(const std::vector<Note>& v, Tick t) {
  std::vector<int> out;
  for (const auto& n : v)
    if (n.start == t) out.push_back(n.pitch);
  std::sort(out.begin(), out.end());
  return out;
}

// ------------------------------------------------------------------ chord vocabulary
void testChordVocabulary() {
  const auto& q = ch::qualities();
  const auto& stamps = tools::chords();
  bool same = stamps.size() == q.size();
  for (size_t i = 0; same && i < q.size(); ++i) same = std::string(stamps[i].name) == q[i].name && stamps[i].steps == q[i].steps;
  expect(same, "chord stamps come from the shared chord vocabulary");
  expect(std::string(stamps[0].name) == "Major" && std::string(stamps[20].name) == "Octaves", "stamp order kept (J-11 indices)");

  ch::Chord c = parsed("Am7");
  expect(c.root == 9 && c.quality == ch::findQuality("m7"), "parse Am7");
  expect(parsed("F#dim").root == 6 && parsed("F#dim").quality == ch::findQuality("dim"), "parse F#dim");
  expect(parsed("Bb").root == 10 && parsed("Bb").quality == 0, "parse Bb");
  c = parsed("C/G");
  expect(c.root == 0 && c.fixedBass == 7, "parse slash chord");
  expect(parsed("Ebmaj7").root == 3 && parsed("Ebmaj7").quality == ch::findQuality("maj7"), "parse Ebmaj7");
  expect(parsed("Dmin7").quality == ch::findQuality("m7") && parsed("Gsus").quality == ch::findQuality("sus4"), "chord name aliases");
  expect(parsed("IV").root == 5 && parsed("IV").quality == 0, "numeral IV in C");
  expect(parsed("vi7").root == 9 && parsed("vi7").quality == ch::findQuality("m7"), "numeral vi7");
  expect(parsed("bVII").root == 10 && parsed("bVII").quality == 0, "numeral bVII");
  expect(parsed("ii°").root == 2 && parsed("ii°").quality == ch::findQuality("dim"), "numeral ii dim");
  expect(parsed("V7").root == 7 && parsed("V7").quality == ch::findQuality("7"), "numeral V7");
  expect(parsed("iv", 9, 2).root == 2 && parsed("iv", 9, 2).quality == 1, "numeral iv in A minor");
  expect(parsed("H7").quality < 0 && parsed("").quality < 0 && parsed("Cxyz").quality < 0, "unknown chords rejected");
  for (const char* t : {"C", "Am7", "F#dim", "Ebmaj7", "C/G", "G7sus4", "Dm9"}) {
    std::string name = ch::chordName(parsed(t), std::string(t).find('b') != std::string::npos);
    expect(name == t, (std::string("name round trip ") + t).c_str());
  }
  expect(ch::romanName(parsed("Am"), 0, 1) == "vi" && ch::romanName(parsed("G7"), 0, 1) == "V7", "numerals: vi, V7");
  expect(ch::romanName(parsed("Bb"), 0, 1) == "bVII" && ch::romanName(parsed("Bdim"), 0, 1) == "vii°", "numerals: bVII, vii°");

  ch::Chord d;
  expect(ch::detect({64, 67, 72}, d) && d.root == 0 && d.quality == 0 && d.inversion == 1, "detect C first inversion");
  expect(ch::detect({62, 65, 69, 72}, d) && d.root == 2 && d.quality == ch::findQuality("m7"), "detect Dm7");
  expect(!ch::detect({60}, d) && !ch::detect({60, 61, 62}, d), "detect: nothing");
  // J-27's chord name now comes from the same detection.
  std::vector<Note> v = {Note(0, 96, 64, 100), Note(0, 96, 67, 100), Note(0, 96, 72, 100)};
  expect(ops::chordName(v, ops::NoteSel(3, false)) == "C/E", "chord name of the selection uses ChordTools");

  expect(ch::diatonicChord(0, 1, 2).root == 2 && ch::diatonicChord(0, 1, 2).quality == 1, "ii of C is D minor");
  expect(ch::diatonicChord(0, 1, 5, true).quality == ch::findQuality("7"), "V7 of C is a dominant seventh");
  expect(ch::diatonicChord(9, 2, 5).root == 4 && ch::diatonicChord(9, 2, 5).quality == 1, "v of A minor is E minor");
  expect(ch::diatonicChord(0, 1, 7).quality == ch::findQuality("dim"), "vii of C is diminished");
}

// J-61: every degree of every scale the engine offers, triad and seventh,
// with every extension setting: the chord is the scale's own stacked thirds
// (never a fallback with notes outside the key).
void testDiatonicProperty() {
  const int nScales = static_cast<int>(tools::scales().size());
  int bad = 0, badQuality = 0;
  for (int scale = 0; scale < nScales; ++scale) {
    std::set<int> key7;
    for (int k = 0; k < 7; ++k) key7.insert(((ch::degreeOffset(scale, k) % 12) + 12) % 12);
    const auto& steps = tools::scales()[static_cast<size_t>(scale)].steps;
    if (scale > 0 && steps.size() == 7)
      expect(key7 == std::set<int>(steps.begin(), steps.end()), "seven-note scales are used as they are");
    for (int key = 0; key < 12; key += 5)
      for (int deg = 1; deg <= 7; ++deg)
        for (bool sev : {false, true}) {
          const ch::Chord c = ch::diatonicChord(key, scale, deg, sev);
          // The quality is exactly the stacked thirds.
          std::set<int> want, have;
          const int base = ch::degreeOffset(scale, deg - 1);
          for (int k = 0; k < (sev ? 4 : 3); ++k) want.insert((ch::degreeOffset(scale, deg - 1 + 2 * k) - base) % 12);
          for (int st : ch::qualities()[static_cast<size_t>(c.quality)].steps) have.insert(st % 12);
          if (want != have) ++badQuality;
          for (int extra = -1; extra <= 3; ++extra) {
            ch::ChordProgression p;
            p.key = key, p.scale = scale;
            ch::Chord x = c;
            x.extra = extra;
            p.chords.push_back(x);
            for (int pc : ch::chordClasses(p, 0))
              if (!key7.count(((pc - key) % 12 + 12) % 12)) ++bad;
          }
        }
  }
  expect(badQuality == 0, "diatonic chords are the scale's stacked thirds (all scales, degrees, sevenths)");
  expect(bad == 0, "diatonic chords and extensions stay inside the scale");
  const ch::Chord iii = ch::diatonicChord(9, 3, 3, true);  // A harmonic minor
  expect(iii.root == 0 && iii.quality == ch::findQuality("maj7#5"), "III7 of harmonic minor is an augmented major seventh");
  expect(ch::chordName(iii) == "Cmaj7#5" && parsed("C+M7").quality == iii.quality, "augmented major seventh names");
}

ch::ChordProgression cProg(const std::string& text = "C Am F G") {
  ch::ChordProgression p;
  ch::parseProgression(text, 0, 1, 384, p);
  return p;
}

// ------------------------------------------------------------------ voicing / render
void testVoicing() {
  ch::ChordProgression p = cProg("C");
  auto v = ch::voice(p, 0);
  expect(v.main == std::vector<int>({60, 64, 67}) && v.bass == 48, "C close voicing + bass");
  p.chords[0].inversion = 1;
  expect(ch::voice(p, 0).main == std::vector<int>({64, 67, 72}), "first inversion");
  p.chords[0].inversion = -1;
  expect(ch::voice(p, 0).main == std::vector<int>({55, 60, 64}), "inversion down");
  p.chords[0].inversion = 0;
  p.chords[0].voicing = ch::Voicing::Open;
  expect(ch::voice(p, 0).main == std::vector<int>({60, 67, 76}), "open voicing");
  p.chords[0].voicing = ch::Voicing::Drop2;
  expect(ch::voice(p, 0).main == std::vector<int>({52, 60, 67}), "drop 2 voicing");
  p.chords[0].voicing = ch::Voicing::Close;
  p.chords[0].extra = 1;
  expect(ch::voice(p, 0).main == std::vector<int>({60, 64, 67, 74}), "one extension: the 9th");
  p.chords[0].extra = -1;
  expect(ch::voice(p, 0).main == std::vector<int>({60, 64}), "one note left out: the fifth");
  p.chords[0].extra = 0;
  p.chords[0].fixedBass = 7;
  expect(ch::voice(p, 0).bass == 55, "fixed bass G below the chord");
  p.chords[0].fixedBass = -1;
  p.chords[0].bassOctave = 1;
  expect(ch::voice(p, 0).bass == 60, "bass octave up");

  // Smooth voicing: F after C takes the inversion nearest to C.
  ch::ChordProgression s = cProg("C F");
  s.chords[1].voicing = ch::Voicing::Smooth;
  auto f = ch::voice(s, 1).main;
  expect(f.size() == 3 && f.front() == 60 && f.back() <= 69, "smooth voicing: F as C-F-A");
  // Key A: chords sit above the key root.
  ch::ChordProgression a;
  ch::parseProgression("i iv", 9, 2, 384, a);
  a.octave = 4;
  expect(ch::voice(a, 0).main.front() == 57 && ch::voice(a, 1).main.front() == 62, "roots above the key root");
  ch::Chord b = parsed("B");
  ch::transpose(b, 1);
  expect(b.root == 0 && b.octave == 1, "B up a semitone: C an octave higher");
  ch::transpose(b, -13);
  expect(b.root == 11 && b.octave == -1, "transpose down past the octave");
}

void testRender() {
  ch::ChordProgression p = cProg();
  auto n = ch::render(p);
  expect(n.size() == 16, "four held chords of three notes + bass");
  expect(pitchesAt(n, 0) == std::vector<int>({48, 60, 64, 67}), "first chord C");
  expect(pitchesAt(n, 384 * 2) == std::vector<int>({53, 65, 69, 72}), "third chord F");
  expect(ch::totalLength(p) == 4 * 384 && ch::chordAt(p, 400) == 1 && ch::chordAt(p, -1) < 0 && ch::chordAt(p, 2000) < 0, "chord positions");
  p.chords[0].length = 768;
  expect(ch::chordStart(p, 1) == 768 && ch::totalLength(p) == 5 * 384, "per-chord length");
  p.chords[0].length = 0;

  p.perf.strum = 30;
  n = ch::render(p);
  Tick last = 0;
  for (const auto& x : n)
    if (x.start < 384) last = std::max(last, x.start);
  expect(last == 30, "strum spreads the chord over the strum time");
  p.perf.strum = 0;
  p.perf.rhythm = 2;  // quarter pulse
  n = ch::render(p);
  expect(n.size() == 64, "quarter pulse: four hits per chord");
  p.perf.rhythm = 6;  // broken chord
  n = ch::render(p);
  bool arp = true;
  for (int k = 0; k < 8; ++k) arp = arp && !pitchesAt(n, 48 * k).empty();
  expect(arp && pitchesAt(n, 48) == std::vector<int>({64}), "broken chord walks up the notes");
  p.perf.rhythm = 0;
  p.chords[1].main = false;
  n = ch::render(p);
  expect(pitchesAt(n, 384).size() == 1, "main notes off: bass only");
  p.chords[1].main = true;
  p.chords[1].bass = false;
  expect(pitchesAt(ch::render(p), 384).size() == 3, "bass off");
  expect(ch::render(p, 1000, true, false).front().start == 1000 && pitchesAt(ch::render(p, 0, false, true), 0).size() == 1,
         "render offset and main / bass switches");
  p.chords[2].color = 5;
  bool col = false;
  for (const auto& x : ch::render(p)) col = col || (x.start == 768 && x.color == 5);
  expect(col, "chord colour goes to its notes");
}

void testWrite() {
  ch::ChordProgression p = cProg();
  std::vector<Note> existing = {Note(10, 50, 61, 90), Note(10, 50, 64, 90), Note(5000, 50, 70, 90)};
  ch::WriteOpts o;
  std::vector<Note> written;
  auto all = ch::write(existing, p, o, &written);
  expect(all.size() == 17 && written.size() == 16, "replace all: the progression plus the note after it");
  o.mode = ch::NonChordMode::ReplaceClashing;
  all = ch::write(existing, p, o);
  bool has61 = false, has64 = false;
  for (const auto& n : all) has61 = has61 || (n.start == 10 && n.pitch == 61), has64 = has64 || (n.start == 10 && n.pitch == 64);
  expect(!has61 && has64 && all.size() == 18, "replace clashing keeps chord tones");
  o.mode = ch::NonChordMode::Keep;
  expect(ch::write(existing, p, o).size() == 19, "keep");
  o.mode = ch::NonChordMode::Snap;
  all = ch::write(existing, p, o);
  bool snapped = false;
  for (const auto& n : all) snapped = snapped || (n.start == 10 && n.pitch == 60);
  expect(snapped, "snap moves C# to C");
  std::vector<Note> e2 = existing;
  expect(ch::snapNotes(e2, p, 0) == 1, "snap existing notes");

  // Linking: writing again replaces the notes of the previous write.
  o.mode = ch::NonChordMode::Keep;
  o.previous.clear();
  std::vector<Note> first;
  auto pat = ch::write({}, p, o, &first);
  p.chords[0] = parsed("Am");
  o.previous = first;
  std::vector<Note> second;
  pat = ch::write(pat, p, o, &second);
  expect(pat.size() == 16 && pitchesAt(pat, 0) == std::vector<int>({57, 69, 72, 76}), "linked chord replaces its old notes");
  // Unlinked chord: its notes stay as they are.
  p.chords[1].linked = false;
  p.chords[1] = parsed("Dm");
  p.chords[1].linked = false;
  o.previous = second;
  pat = ch::write(pat, p, o);
  expect(pitchesAt(pat, 384) == std::vector<int>({57, 69, 72, 76}), "unlinked chord keeps the old notes");
}

void testEditing() {
  ch::ChordProgression p = cProg();
  p.chords[0].length = 768;
  expect(ch::swapChords(p, 0, 1) && p.chords[0].root == 9 && p.chords[0].length == 768 && p.chords[1].root == 0,
         "swap keeps the slot lengths");
  expect(!ch::swapChords(p, 0, 9), "swap out of range");
  p = cProg();
  const int k = ch::addPassingChord(p, 0, 96);
  expect(k == 1 && p.chords.size() == 5 && ch::chordLength(p, 0) == 192 && ch::chordLength(p, 1) == 192, "passing chord halves the chord");
  expect(p.chords[1].root == 4 && p.chords[1].quality == ch::findQuality("7"), "passing chord: dominant of the next chord");
  expect(ch::totalLength(p) == 4 * 384, "passing chord keeps the length");
  ch::ChordProgression shortP = cProg("C");
  shortP.chords[0].length = 96;
  expect(ch::addPassingChord(shortP, 0, 96) < 0, "no passing chord in a one-beat chord");

  p = cProg();
  auto alts = ch::alternatives(p, 0);
  bool hasAm = false, hasC = false;
  for (const auto& c : alts) hasAm = hasAm || (c.root == 9 && c.quality == 1), hasC = hasC || (c.root == 0 && c.quality == 0);
  expect(!alts.empty() && alts.size() <= 12 && hasAm && !hasC, "alternatives share notes and leave the chord out");

  ch::ChordProgression r1 = cProg(), r2 = cProg();
  r1.chords[2].locked = r2.chords[2].locked = true;
  ch::regenerate(r1, {}, 0.5, 77);
  ch::regenerate(r2, {}, 0.5, 77);
  expect(r1 == r2, "regenerate is deterministic per seed");
  expect(r1.chords[2].root == 5 && r1.chords[2].quality == 0, "regenerate leaves locked chords");
  ch::ChordProgression r3 = cProg();
  ch::regenerate(r3, {1}, 1.0, 5);
  expect(r3.chords[0] == cProg().chords[0] && r3.chords[3] == cProg().chords[3], "regenerate only the chosen chords");
  auto g = ch::generate(0, 1, 6, 384, 0.2, 9);
  bool diatonic = true;
  for (const auto& c : g.chords) diatonic = diatonic && c.quality >= 0;
  expect(g.chords.size() == 6 && diatonic, "generate a progression");

  ch::ChordProgression t;
  int bad = 0;
  expect(ch::parseProgression("C - Am, F | G", 0, 1, 384, t, &bad) && t.chords.size() == 4 && bad == 0, "parse a progression");
  expect(ch::parseProgression("Dm7 X G7 Cmaj7", 0, 1, 384, t, &bad) && t.chords.size() == 3 && bad == 1, "unknown words skipped");
  expect(!ch::parseProgression("nothing here", 0, 1, 384, t), "no chords: false");
  expect(ch::progressionText(cProg("I V vi IV"), true) == "I - V - vi - IV", "progression as numerals");
  expect(ch::progressionText(cProg("I V vi IV"), false) == "C - G - Am - F", "progression as names");
  bool allParse = true;
  for (const auto& pr : ch::presets()) {
    ch::ChordProgression x;
    allParse = allParse && ch::parseProgression(pr.text, 0, pr.minor ? 2 : 1, 384, x);
  }
  expect(ch::presets().size() >= 10 && allParse, "presets parse");
  p = cProg();
  ch::rescale(p, 8 * 384, 96);
  expect(ch::totalLength(p) == 8 * 384 && ch::chordLength(p, 0) == 768, "rescale");
  ch::Chord c = parsed("C");
  for (int i = 0; i < 10; ++i) ch::invert(c, 1);
  expect(c.inversion == 6, "inversion limited");
  expect(ch::addChord(p) == 4 && p.chords.back().root == 0 && ch::removeChord(p, 4) && !ch::removeChord(p, 9), "add / remove chords");
  // Analyze: the chords the notes form, window by window.
  auto an = ch::analyze(ch::render(cProg("Am F C G")), 0, 1, 0, 384, 5);
  expect(an.chords.size() == 5 && ch::progressionText(an, false) == "Am - F - C - G - G", "analyze the notes into chords");
}

// ------------------------------------------------------------------ groups
void testNoteGroups() {
  std::vector<Note> n = {Note(0, 96, 60, 100), Note(96, 96, 62, 100), Note(192, 96, 64, 100), Note(288, 96, 65, 100)};
  ops::NoteSel sel = {true, false, true, false};
  const int g = tools::groupNotes(n, sel);
  expect(g == 1 && n[0].group == 1 && n[2].group == 1 && n[1].group == 0, "group notes");
  expect(tools::groupNotes(n, ops::NoteSel{false, true, false, false}) == 0, "one note: no group");
  ops::NoteSel pick = {true, false, false, false};
  expect(tools::selectGroups(n, pick) == 1 && pick[2], "clicking a grouped note selects its group");
  expect(tools::groupNotes(n, ops::NoteSel{false, true, false, true}) == 2, "second group number");
  expect(tools::ungroupNotes(n, ops::NoteSel{true, false, false, false}) == 1 && n[0].group == 0 && n[2].group == 1, "ungroup the selection");
  expect(tools::ungroupNotes(n, ops::NoteSel{}) == 3, "ungroup all");
  Note a(0, 1, 60, 100);
  a.group = 3;
  expect(a.hasProps() && !(a == Note(0, 1, 60, 100)), "group is a note property");
}

void testPatternGroups() {
  Document d = makeDoc();
  const ObjectId p0 = d.song().patterns[0].id;
  const ObjectId p1 = d.addPattern("B"), p2 = d.addPattern("C");
  expect(ops::groupPatterns(d, {p0, p1}, "Drums") == 0 && ops::patternsInGroup(d.song(), 0) == std::vector<ObjectId>({p0, p1}),
         "group patterns");
  expect(ops::patternGroupOf(d.song(), p1) == 0 && ops::patternGroupOf(d.song(), p2) == -1, "group of a pattern");
  expect(ops::groupPatterns(d, {p1, p2}, "") == 1 && ops::patternsInGroup(d.song(), 0) == std::vector<ObjectId>({p0}),
         "regrouping moves the pattern");
  expect(d.song().patternGroups[1].name == "Group 2", "default group name");
  expect(ops::renamePatternGroup(d, 1, "Keys") && d.song().patternGroups[1].name == "Keys", "rename group");
  expect(ops::ungroupPatterns(d, {p0}) == 1 && d.song().patternGroups.size() == 1, "empty group goes");
  d.undo();
  expect(d.song().patternGroups.size() == 2, "undo ungroup");
  d.removePattern(p2);
  expect(ops::patternsInGroup(d.song(), 1) == std::vector<ObjectId>({p1}), "deleted patterns drop out of the group list");
  expect(ops::deletePatternGroup(d, 0) && d.song().patternGroups.size() == 1, "delete group");
}

void testClipGroups() {
  Document d = makeDoc();
  const ObjectId pat = d.song().patterns[0].id;
  const ObjectId a = d.addClip(pat, 0, 0), b = d.addClip(pat, 1, 384), c = d.addClip(pat, 2, 768);
  expect(ops::groupClips(d, {a}) == kNoId, "one clip: no group");
  const ObjectId g = ops::groupClips(d, {a, b});
  expect(g != kNoId && d.song().clip(a)->group == g && d.song().clip(c)->group == kNoId, "group clips");
  expect(ops::expandClipGroups(d.song(), {a}) == std::set<ObjectId>({a, b}), "selecting a grouped clip selects the group");
  expect(ops::ungroupClips(d, {b}) == 2 && d.song().clip(a)->group == kNoId, "ungroup the whole group");
  d.undo();
  expect(d.song().clip(a)->group == g, "undo ungroup clips");
}

// ------------------------------------------------------------------ step loops / swing / key ranges
Song rackSong(ObjectId& chId) {
  Song s = makeEmptySong();
  Channel c;
  c.id = chId = s.allocateId();
  c.plugin = defaultPluginState("gl.synth");
  c.mixerTrack = s.masterId();
  s.channels.push_back(c);
  return s;
}

void testStepLoops() {
  ObjectId ch = kNoId;
  Song s = rackSong(ch);
  Pattern& p = s.patterns[0];
  p.steps[ch] = std::vector<uint8_t>(16, 0);
  p.steps[ch][0] = 100;
  p.steps[ch][2] = 90;
  p.steps[ch][9] = 80;  // beyond the loop: never plays
  p.stepLoop[ch] = 4;
  const Tick st = s.ticksPerStep(p);
  auto ev = compilePatternChannel(s, p, ch);
  expect(ev.size() == 8 && ev[0].on == 0 && ev[2].on == 4 * st && ev[7].on == 14 * st, "step loop repeats the first steps");
  p.notes[ch] = {Note(st, st, 64, 100), Note(5 * st, st, 67, 100)};
  p.steps[ch] = std::vector<uint8_t>(16, 0);
  ev = compilePatternChannel(s, p, ch);
  expect(ev.size() == 4 && ev[1].on == 5 * st && ev[1].pitch == 64.0f, "notes in the loop repeat, later ones drop");

  Document d(s);
  const ObjectId pid = d.song().patterns[0].id;
  const auto before = compilePatternChannel(d.song(), d.song().patterns[0], ch);
  expect(ops::burnStepLoops(d, pid, {ch}) == 1 && ops::stepLoop(d.song().patterns[0], ch) == 0, "burn removes the loop");
  const auto after = compilePatternChannel(d.song(), d.song().patterns[0], ch);
  bool same = before.size() == after.size();
  for (size_t i = 0; same && i < before.size(); ++i) same = before[i].on == after[i].on && before[i].pitch == after[i].pitch;
  expect(same, "burned pattern plays the same");
  d.undo();
  expect(ops::stepLoop(d.song().patterns[0], ch) == 4, "undo burn");
  expect(ops::setStepLoop(d, pid, {ch}, 0) && ops::stepLoop(d.song().patterns[0], ch) == 0, "don't loop");
  expect(!ops::setStepLoop(d, pid, {ch}, 0), "no change: nothing recorded");
  expect(ops::setStepLoop(d, pid, {ch}, 99) == false, "a loop as long as the pattern is no loop");
  expect(ops::contentSteps(d.song(), d.song().patterns[0], ch) == 6, "content reaches step 6 (note ends)");
  expect(ops::loopToContent(d, pid, {ch}, 4) == 1 && ops::stepLoop(d.song().patterns[0], ch) == 8, "loop to the next beat");
  expect(ops::loopedRow({1, 2, 0}, 2, 6) == std::vector<uint8_t>({1, 2, 1, 2, 1, 2}), "looped row");

  // Round trip.
  Song back;
  std::string err;
  const bool ok = songFromJson(songToJson(d.song()), back, &err);
  expect(ok && back.patterns[0].stepLoop == d.song().patterns[0].stepLoop, "step loops saved");
}

void testTruncatedSwing() {
  ObjectId ch = kNoId;
  Song s = rackSong(ch);
  s.swing = 1.0f;
  Pattern& p = s.patterns[0];
  p.steps[ch] = std::vector<uint8_t>(16, 0);
  p.steps[ch][1] = 100;
  const Tick st = s.ticksPerStep(p);
  auto ev = compilePatternChannel(s, p, ch);
  expect(ev.size() == 1 && ev[0].on == st + st / 2 && ev[0].off == ev[0].on + st, "swing: the step keeps its length");
  s.channels.back().swingTruncate = true;
  ev = compilePatternChannel(s, s.patterns[0], ch);
  expect(ev[0].off == 2 * st, "truncated swing ends on the grid");
  Document d(s);
  expect(ops::setSwingTruncate(d, {ch}, 0) == 1 && !d.song().channel(ch)->swingTruncate, "turn off");
  expect(ops::setSwingTruncate(d, {ch}, 2) == 1 && d.song().channel(ch)->swingTruncate, "toggle");
  expect(ops::setSwingTruncate(d, {ch}, 1) == 0, "already on");
}

void testLayerSplit() {
  Song s = makeEmptySong();
  auto add = [&](const std::string& n) {
    Channel c;
    c.id = s.allocateId();
    c.name = n;
    c.plugin = defaultPluginState("gl.synth");
    c.mixerTrack = s.masterId();
    s.channels.push_back(c);
    return c.id;
  };
  const ObjectId k1 = add("low"), k2 = add("high"), L = add("layer");
  s.channel(L)->plugin = PluginState{};
  s.channel(L)->layer = {k1, k2};
  s.patterns[0].notes[L] = {Note(0, 96, 48, 100), Note(96, 96, 71, 100)};
  Document d(s);
  expect(ops::splitLayerChildren(d, L), "split children");
  const Channel* a = d.song().channel(k1);
  const Channel* b = d.song().channel(k2);
  expect(a->keyLo == 0 && b->keyHi == 127 && a->keyHi + 1 == b->keyLo && a->keyHi >= 48 && b->keyLo <= 71, "ranges split the notes used");
  auto ea = compilePatternChannel(d.song(), d.song().patterns[0], k1);
  auto eb = compilePatternChannel(d.song(), d.song().patterns[0], k2);
  expect(ea.size() == 1 && ea[0].pitch == 48.0f && eb.size() == 1 && eb[0].pitch == 71.0f, "each child plays its part");
  expect(ops::resetLayerChildren(d, L) && d.song().channel(k1)->fullKeyRange(), "reset children");
  expect(!ops::resetLayerChildren(d, L), "reset: nothing to do");
  expect(ops::setKeyRange(d, k2, 80, 60) && d.song().channel(k2)->keyLo == 60 && d.song().channel(k2)->keyHi == 80, "key range sorted");
  Song back;
  expect(songFromJson(songToJson(d.song()), back) && back.channel(k2)->keyLo == 60 && back.channel(k2)->keyHi == 80, "key range saved");
}

void testAltSolo() {
  Song s = makeEmptySong();
  // Insert 1 feeds insert 2, which feeds the master.
  s.mixer[1].sends = {Send{s.mixer[2].id, 1.0f}};
  Document d(s);
  const ObjectId i1 = d.song().mixer[1].id, i2 = d.song().mixer[2].id, i3 = d.song().mixer[3].id;
  ProgramBuilder b;
  d.editMixerTrack(i2, "solo", [](MixerTrack& m) { m.solo = true; });
  auto prog = b.build(d.song());
  auto silent = [&](ObjectId id) { return prog->mixer[static_cast<size_t>(d.song().mixerIndex(id))].soloSilent; };
  expect(!silent(i1) && !silent(i2) && silent(i3), "plain solo keeps the tracks feeding it");
  expect(ops::altSoloMixerTracks(d, {i2}), "alt solo");
  prog = b.build(d.song());
  expect(silent(i1) && !silent(i2) && silent(i3), "alt solo: the feeding tracks are silent");
  expect(ops::altSoloMixerTracks(d, {i2}) && !d.song().mixerTrack(i2)->solo, "alt solo again: unsolo");
  expect(!ops::altSoloMixerTracks(d, {d.song().masterId()}), "the master is never alt-soloed");
}

// ------------------------------------------------------------------ song tools
void testKeyMarkers() {
  Document d = makeDoc();
  const ObjectId pat = d.song().patterns[0].id;
  expect(ops::keyAt(d.song(), 0).marker < 0, "no key markers");
  ops::addKeyMarker(d, 384 * 2, 9, 2);
  ops::addKeyMarker(d, 384 * 8, 2, 4);
  Marker plain;
  plain.pos = 384 * 4;
  plain.name = "Verse";
  ops::addMarker(d, plain);
  auto k = ops::keyAt(d.song(), 384 * 5);
  expect(k.root == 9 && k.scale == 2 && k.marker >= 0, "key in effect after a key marker (plain markers ignored)");
  expect(ops::keyAt(d.song(), 384 * 9).root == 2, "later key marker");
  expect(ops::keyAt(d.song(), 0).marker < 0, "before the first key marker: none");
  expect(d.song().markers[0].name == "A Natural minor", "key marker named after the key");
  d.addClip(pat, 0, 384 * 3);
  d.addClip(pat, 1, 384 * 9);
  expect(ops::patternSongPosition(d.song(), pat) == 384 * 3 && ops::patternKey(d.song(), pat).root == 9, "piano roll key follows the markers");
  Song back;
  expect(songFromJson(songToJson(d.song()), back) && back.markers == d.song().markers, "key markers saved");
}

void testSmallHelpers() {
  Song s = makeEmptySong();
  expect(ops::jumpGrid(s, 0) == 0 && ops::jumpGrid(s, 1) == s.beatTicks() && ops::jumpGrid(s, 5) == 8 * s.barTicks(), "jump grids");
  expect(std::string(ops::jumpGridName(2)) == "Next bar", "jump grid names");
  expect(ops::snapIncoming(61, 0, 1, true) != 61 && ops::snapIncoming(61, 0, 1, false) == 61 && ops::snapIncoming(61, 0, 0, true) == 61,
         "incoming MIDI snaps to the scale");
  Pattern p;
  p.notes[5] = {Note(0, 96, 60, 100), Note(0, 48, 60, 100)};
  p.notes[6] = {Note(0, 384, 62, 100)};
  auto h = ops::ghostNoteAt(p, 6, {}, 10, 60);
  expect(h.channel == 5 && h.index == 1, "ghost note under the pointer (shortest)");
  expect(ops::ghostNoteAt(p, 5, {}, 10, 60).channel == kNoId && ops::ghostNoteAt(p, 6, {7}, 10, 60).channel == kNoId,
         "ghost filters");
  std::vector<float> sine(48000);
  for (size_t i = 0; i < sine.size(); ++i) sine[i] = static_cast<float>(std::sin(i * 0.05));
  auto cols = ops::waveformColumns(sine.data(), nullptr, sine.size(), 48000, 0.001, 0, 0, 100, 20);
  expect(cols.size() == 20 && cols[0].second > 0.9f && cols[0].first < -0.9f && cols[19].second == 0.0f, "background waveform columns");
}

// ------------------------------------------------------------------ engine: jumps, recording, logger
struct Rig {
  double sr = 48000;
  Song song = makeEmptySong();
  ProgramBuilder builder;
  AudioEngine eng{48000};
  AudioRecorder rec;
  std::shared_ptr<const Program> prog;
  float inL = 0.5f, inR = 0.5f;
  Rig() {
    builder.setSampleRate(sr);
    eng.setInputChannels(2);
    eng.setMode(PlayMode::Song);
    eng.setLooping(false);
  }
  void build() {
    prog = builder.build(song);
    eng.setProgram(prog);
  }
  void run(int64_t frames, int block = 256) {
    std::vector<float> L(static_cast<size_t>(block)), R(static_cast<size_t>(block)), in(static_cast<size_t>(block) * 2);
    for (int64_t done = 0; done < frames;) {
      const int n = static_cast<int>(std::min<int64_t>(block, frames - done));
      for (int i = 0; i < n; ++i) in[static_cast<size_t>(2 * i)] = inL, in[static_cast<size_t>(2 * i + 1)] = inR;
      eng.pushInput(in.data(), n);
      eng.process(L.data(), R.data(), n);
      done += n;
      rec.pump(eng, ringsOf(*prog));
    }
  }
  double spt() const { return sr * 60.0 / (song.tempo * song.ppq); }
};

void testQuantizedJump() {
  Rig r;
  r.build();
  r.eng.play();
  r.run(static_cast<int64_t>(r.spt() * 40));
  const double p0 = r.eng.position();
  expect(p0 > 30 && p0 < 50, "playing near tick 40");
  r.eng.jumpTo(960, 96);
  r.run(256);
  expect(r.eng.jumpPending() && r.eng.position() < 96, "jump waits for the next beat");
  r.run(static_cast<int64_t>(r.spt() * 60));
  expect(!r.eng.jumpPending() && r.eng.position() > 960 && r.eng.position() < 1000, "jump lands on the beat");
  r.eng.jumpTo(100, 0);
  r.run(256);
  expect(r.eng.position() < 110 && r.eng.position() >= 100, "unquantized jump at once");
  r.eng.stop();
  r.run(256);
  r.eng.jumpTo(500, 96);
  expect(r.eng.position() == 500 && !r.eng.jumpPending(), "stopped: jump at once");
}

float recordPeak(RecordPoint point, float volume, bool withInput = true) {
  Rig r;
  r.song.mixer[1].input = withInput ? 0 : -1;
  r.song.mixer[1].armed = true;
  r.song.mixer[1].monitor = static_cast<uint8_t>(InputMonitor::On);
  r.song.mixer[1].recPoint = static_cast<uint8_t>(point);
  r.song.mixer[1].volume = volume;
  r.build();
  r.rec.begin();
  r.eng.setRecording(true);
  r.eng.play();
  r.run(9600);
  r.eng.setRecording(false);
  r.eng.stop();
  r.run(4096);
  auto takes = r.rec.release();
  if (takes.empty()) return -1;
  return takePeak(takes[0]);
}

void testRecordPoints() {
  const float in = recordPeak(RecordPoint::Input, 0.3f);
  const float postFx = recordPeak(RecordPoint::PostEffects, 0.3f);
  const float post = recordPeak(RecordPoint::PostFader, 0.3f);
  expect(std::fabs(in - 0.5f) < 1e-3f, "input tap records the input");
  expect(std::fabs(postFx - 0.5f) < 0.02f, "post effects (pre fader) tap: the track before its fader");
  expect(post > 0 && post < in * 0.8f, "post fader tap: the fader applies");
  expect(recordPeak(RecordPoint::PostFader, 0.8f, false) >= 0, "output taps record without a device input");
  MixerTrack m;
  m.armed = true;
  expect(!m.records(), "input tap without an input does not record");
  m.recPoint = static_cast<uint8_t>(RecordPoint::PostFader);
  expect(m.records(), "output tap records without an input");
  Song back;
  Song s = makeEmptySong();
  s.mixer[2].recPoint = 2;
  s.mixer[2].soloAlone = true;
  expect(songFromJson(songToJson(s), back) && back.mixer[2].recPoint == 2 && back.mixer[2].soloAlone, "record point saved");
}

void testRecordingExtras() {
  std::vector<RecordedTake> takes(3);
  takes[0].L = takes[0].R = std::vector<float>(100, 0.5f);
  takes[1].L = takes[1].R = std::vector<float>(100, 0.0001f);
  takes[2].L = takes[2].R = std::vector<float>(100, 0.0f);
  expect(std::fabs(takePeak(takes[0]) - 0.5f) < 1e-6f, "take peak");
  expect(dropSilentTakes(takes, -60.0f) == 2 && takes.size() == 1, "silent takes are dropped");
  Song s = makeEmptySong();
  s.mixer[1].name = "Vocal";
  s.mixer[1].input = 2;
  expect(takeBaseName(s, s.mixer[1].id, TakeCommitOptions::Naming::Insert) == "Vocal", "take named after the insert");
  expect(takeBaseName(s, s.mixer[1].id, TakeCommitOptions::Naming::Input) == "In 3 - In 4", "take named after the input");
  s.mixer[1].inputStereo = false;
  expect(takeBaseName(s, s.mixer[1].id, TakeCommitOptions::Naming::Input) == "In 3", "mono input name");
}

void testAudioLogger() {
  AudioLogger lg(1000, 2.0, AudioLogger::Source::Master);
  std::vector<float> a(700), b(700);
  for (int k = 0; k < 4; ++k) {
    for (size_t i = 0; i < a.size(); ++i) a[i] = static_cast<float>(k * 700 + i), b[i] = -a[i];
    lg.write(a.data(), b.data(), 700);
  }
  expect(lg.written() == 2800 && lg.available() == 2000, "logger keeps the last two seconds");
  std::vector<float> L, R;
  expect(lg.copyLast(500, L, R) == 500 && L.front() == 2300.0f && L.back() == 2799.0f && R.back() == -2799.0f, "copy the last frames");
  expect(lg.copyLast(99999, L, R) == 2000 && L.front() == 800.0f, "copy at most what is kept");
  expect(lg.peakLast(10) == 2799.0f, "logger peak");

  Rig r;
  r.song.mixer[1].input = 0;
  r.song.mixer[1].armed = true;
  r.build();
  auto logger = std::make_shared<AudioLogger>(r.sr, 1.0, AudioLogger::Source::ArmedInputs);
  r.eng.setLogger(logger);
  r.run(4800);
  expect(logger->written() == 4800 && std::fabs(logger->peakLast(4800) - 0.5f) < 1e-4f, "logger hears the armed input");
  auto master = std::make_shared<AudioLogger>(r.sr, 1.0, AudioLogger::Source::Master);
  r.eng.setLogger(master);
  r.run(2400);
  expect(master->written() == 2400 && master->peakLast(2400) > 0.1f && logger->written() == 4800, "switching the logger source");
  r.eng.setLogger(nullptr);
  r.run(256);
  expect(master->written() == 2400 && !r.eng.logger(), "logger off");
}

void testMidiSnapAndKeyRange() {
  const uint16_t cmaj = 0xAB5;  // C D E F G A B
  expect(AudioEngine::snapToScaleMask(61, 0, cmaj) == 60 && AudioEngine::snapToScaleMask(66, 0, cmaj) == 65 &&
             AudioEngine::snapToScaleMask(64, 0, cmaj) == 64 && AudioEngine::snapToScaleMask(61, 0, 0) == 61,
         "engine scale snap (ties down)");
  expect(AudioEngine::snapToScaleMask(62, 2, 0x1) == 62 && AudioEngine::snapToScaleMask(63, 2, 0x1) == 62, "snap around another root");
  // A channel that only plays C5: a played C# sounds only when snapped to C.
  auto peakAfterNote = [](bool snap) {
    Rig r;
    Channel c;
    c.id = r.song.allocateId();
    c.plugin = defaultPluginState("gl.synth");
    c.mixerTrack = r.song.masterId();
    c.keyLo = c.keyHi = 60;
    r.song.channels.push_back(c);
    r.build();
    r.eng.setMidiTarget(c.id);
    r.eng.setMidiScaleSnap(0, snap ? 0xAB5 : 0);
    r.eng.midiEvent(0x90, 61, 100);
    std::vector<float> L(256), R(256);
    float pk = 0;
    for (int b = 0; b < 40; ++b) {
      r.eng.process(L.data(), R.data(), 256);
      for (float v : L) pk = std::max(pk, std::fabs(v));
    }
    return pk;
  };
  expect(peakAfterNote(false) == 0.0f, "key range: a note outside it is silent");
  expect(peakAfterNote(true) > 0.001f, "snapped MIDI note lands in the range and plays");
}

void testExportGroups() {
  Song s = makeEmptySong();
  s.tracks[2].groupSize = 2;  // tracks 2..4
  s.tracks[2].name = "Drums";
  s.tracks[8].groupSize = 1;  // tracks 8..9: empty
  Clip c;
  c.id = s.allocateId();
  c.pattern = s.patterns[0].id;
  c.track = 3;
  c.length = 384;
  s.playlist.push_back(c);
  Clip other = c;
  other.id = s.allocateId();
  other.track = 6;
  s.playlist.push_back(other);
  auto groups = playlistTrackGroups(s);
  expect(groups.size() == 1 && groups[0].name == "Drums" && groups[0].tracks == std::vector<int>({2, 3, 4}), "track groups that sound");
  Song only = playlistTracksSong(s, groups[0].tracks);
  expect(only.playlist.size() == 1 && only.playlist[0].track == 3, "group render keeps its tracks only");
  RenderJob job;
  job.sampleRate = 22050;
  int parts = 0;
  renderPlaylistTrackSets(s, job, groups, [&](const SplitPart& p, RenderResult&&) {
    parts += p.name == "Drums" && p.track == 2 ? 1 : 0;
    return true;
  });
  expect(parts == 1, "one file per track group");
}

void testSerialization() {
  Song s = makeEmptySong();
  s.patterns[0].notes[s.allocateId()] = {};
  Note n(0, 96, 60, 100);
  n.group = 4;
  ObjectId ch = kNoId;
  Song r = rackSong(ch);
  r.patterns[0].notes[ch] = {n};
  r.channels.back().swingTruncate = true;
  PatternGroup g;
  g.name = "Verse";
  g.patterns = {r.patterns[0].id};
  r.patternGroups.push_back(g);
  Clip c;
  c.id = r.allocateId();
  c.pattern = r.patterns[0].id;
  c.group = 77;
  r.playlist.push_back(c);
  Song back;
  std::string err;
  expect(songFromJson(songToJson(r), back, &err), "load J-31 fields");
  expect(back.patterns[0].notes[ch][0].group == 4 && back.channels.back().swingTruncate && back.patternGroups == r.patternGroups &&
             back.playlist[0].group == 77,
         "note groups, clip groups, pattern groups, truncated swing saved");
  expect(back == r, "whole song round trip");
}

}  // namespace

int main() {
  gl::test::hermeticTestEnv("j31");
  testChordVocabulary();
  testDiatonicProperty();
  testVoicing();
  testRender();
  testWrite();
  testEditing();
  testNoteGroups();
  testPatternGroups();
  testClipGroups();
  testStepLoops();
  testTruncatedSwing();
  testLayerSplit();
  testAltSolo();
  testKeyMarkers();
  testSmallHelpers();
  testQuantizedJump();
  testRecordPoints();
  testRecordingExtras();
  testAudioLogger();
  testMidiSnapAndKeyRange();
  testExportGroups();
  testSerialization();
  return finish();
}
