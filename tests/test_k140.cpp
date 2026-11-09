// Gridline — K-140: a cut clip loops from its cut point.
//  * a pattern clip cut short (trimmed at either end) that is looped repeats the
//    part it showed (Clip::loopStart / loopLength), sample-exact as plain clips
//    of that cut length would play; an uncut clip keeps its pattern's loop;
//  * loop off again gives back exactly the clip it was (no loop fields left);
//  * left edge, slip and split keep the content inside the clip's own loop;
//  * automation clips cut short repeat their part of the curve (the curve's
//    own loop length is left alone);
//  * the project file round trips the fields, old files load without them;
//  * FLP export writes a loop of a cut clip as repeated clips of the cut length.
#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "core/Automation.h"
#include "core/ClipLoop.h"
#include "core/PluginInfo.h"
#include "core/Project.h"
#include "core/flp/Flp.h"
#include "core/flp/FlpExport.h"
#include "engine/ClipOps.h"
#include "engine/PlaylistTools.h"
#include "engine/Render.h"

using namespace gl;

namespace {

// One synth channel and a 1-bar pattern of four notes, one per beat.
struct Fx {
  Song s;
  ObjectId ch = kNoId, pat = kNoId;
  Fx() {
    s = makeEmptySong();
    s.tempo = 140;
    Channel c;
    c.id = s.allocateId();
    c.name = "Synth";
    c.plugin = defaultPluginState("gl.synth");
    c.mixerTrack = s.masterId();
    s.channels.push_back(c);
    ch = c.id;
    Pattern p;
    p.id = s.allocateId();
    p.name = "Riff";
    p.bars = 1;
    const Tick b = s.beatTicks();
    for (int k = 0; k < 4; ++k) p.notes[ch].push_back(Note(k * b, b / 2, 60 + 3 * k, 100));
    s.patterns.push_back(p);
    pat = p.id;
  }
  ObjectId clip(Tick start, Tick len, Tick offset, bool loop = false, int track = 0) {
    Clip c;
    c.id = s.allocateId();
    c.pattern = pat;
    c.track = track;
    c.start = start;
    c.length = len;
    c.offset = offset;
    c.loop = loop;
    s.playlist.push_back(c);
    return c.id;
  }
  Clip& at(ObjectId id) { return *s.clip(id); }
};

AudioBuffer render(const Song& s) {
  RenderOptions o;
  o.threads = 1;
  o.tail = s.barTicks();
  return renderSong(s, o);
}
bool sameAudio(const AudioBuffer& a, const AudioBuffer& b) { return a.frames == b.frames && a.frames > 0 && a.data == b.data; }
double energy(const AudioBuffer& b) {  // (sum of squares: something played)
  double e = 0;
  for (float v : b.data) e += static_cast<double>(v) * static_cast<double>(v);
  return e;
}

void testPatternCut() {
  const Tick L = Fx().s.barTicks(), beat = Fx().s.beatTicks();
  TempoMap tm;
  {  // cut at the end: the first two beats repeat
    Fx a;
    const ObjectId id = a.clip(0, 2 * beat, 0);
    tm.build(a.s);
    expect(clipCutShort(a.s, a.at(id)), "pattern: a clip shorter than its pattern is cut short");
    const Clip before = a.at(id);
    Clip l = withClipLoop(a.s, tm, before, true);
    expect(l.loop && l.loopStart == 0 && l.loopLength == 2 * beat, "pattern: looping a cut clip loops the part it shows");
    l.length = 3 * L;
    a.at(id) = l;
    const std::vector<ClipPass> ps = clipPasses(a.s, a.at(id));
    bool each = ps.size() == 6;
    for (const ClipPass& p : ps) each = each && p.len == 2 * beat && p.content == 0;
    expect(each, "pattern: ... it repeats every two beats (six passes of the cut length in three bars)");
    expect(clipRepeats(a.s, a.at(id)) && !clipPastContent(a.s, a.at(id)), "pattern: ... it repeats, never past its pattern");
    // The same as six plain clips of the cut length, sample by sample.
    Fx b;
    for (int k = 0; k < 6; ++k) b.clip(k * 2 * beat, 2 * beat, 0);
    const AudioBuffer ra = render(a.s), rb = render(b.s);
    expect(sameAudio(ra, rb), "pattern: a loop of a cut clip plays as repeated clips of the cut length, sample-exact");
    // Loop off again: exactly the clip before the drag, at its length.
    Clip back = withClipLoop(a.s, tm, a.at(id), false);
    back.length = before.length;
    expect(back == before, "pattern: loop off at the cut point gives back the clip as it was (no loop fields left)");
  }
  {  // cut at both ends: beats 2 and 3 repeat
    Fx a;
    const ObjectId id = a.clip(4 * L, 2 * beat, beat);
    tm.build(a.s);
    Clip l = withClipLoop(a.s, tm, a.at(id), true);
    expect(l.loopStart == beat && l.loopLength == 2 * beat && l.offset == beat, "pattern: a clip trimmed at both ends loops its middle");
    l.length = 4 * beat + beat;
    a.at(id) = l;
    const std::vector<ClipPass> ps = clipPasses(a.s, a.at(id));
    expect(ps.size() == 3 && ps[0].content == beat && ps[1].content == beat && ps[2].content == beat && ps[2].len == beat,
           "pattern: ... every pass starts at the cut start");
    const std::vector<Note> ns = clipChannelNotes(a.s, a.at(id), a.s.pattern(a.pat)->notes.at(a.ch));
    bool pitches = ns.size() == 5;
    const int want[5] = {63, 66, 63, 66, 63};
    for (size_t k = 0; k < ns.size() && k < 5; ++k) pitches = pitches && ns[k].pitch == want[k] && ns[k].start == static_cast<Tick>(k) * beat;
    expect(pitches, "pattern: ... the notes of the cut part, one per beat");
    // Left edge out by a beat: the content wraps inside the cut part.
    const std::vector<Clip> t = pltools::trimStarts(a.s, tm, a.s.playlist, {id}, -beat, false);
    expect(t[0].start == 4 * L - beat && t[0].offset == 2 * beat && t[0].loopStart == beat && t[0].loopLength == 2 * beat,
           "pattern: Ctrl + left edge out wraps the loop start inside the cut part");
    // Slip: wraps inside the cut part too.
    const Clip sl = pltools::slipClip(a.s, tm, a.at(id), -beat / 2);
    expect(sl.offset == beat + beat / 2 && sl.loopStart == beat, "pattern: slip wraps inside the cut part");
    // Split in the second pass: the right part goes on inside the cut part.
    const ObjectId r = splitClipAt(a.s, id, 4 * L + 2 * beat + beat / 2);
    const Clip* rc = a.s.clip(r);
    expect(rc && rc->loop && rc->loopStart == beat && rc->loopLength == 2 * beat && rc->offset == beat + beat / 2,
           "pattern: split, the right part loops on inside the cut part");
  }
  {  // an uncut clip keeps its pattern's loop, a longer one too
    Fx a;
    const ObjectId full = a.clip(0, L, 0), longer = a.clip(2 * L, 2 * L, 0), off = a.clip(5 * L, L, beat);
    tm.build(a.s);
    const Clip f = withClipLoop(a.s, tm, a.at(full), true), g = withClipLoop(a.s, tm, a.at(longer), true), h = withClipLoop(a.s, tm, a.at(off), true);
    expect(f.loop && f.loopLength == 0 && g.loop && g.loopLength == 0, "pattern: an uncut (or longer) clip loops its whole pattern");
    expect(h.loop && h.loopLength == 0, "pattern: a clip running past its pattern's end is not cut short (it loops its pattern)");
  }
  {  // the loop key's plain path: a looping cut clip pulled back within its first pass stops looping
    Fx a;
    const ObjectId id = a.clip(0, 2 * beat, 0);
    tm.build(a.s);
    Clip l = withClipLoop(a.s, tm, a.at(id), true);
    l.length = 2 * L;
    a.at(id) = l;
    expect(loopFirstPass(a.s, a.at(id), tm) == 2 * beat, "pattern: a looping cut clip's first pass is its cut length");
    std::vector<Clip> cs = a.s.playlist;
    cs[0].length = 2 * beat;
    expect(unloopWithinFirstPass(a.s, cs, {id}, tm) == 1 && !cs[0].loop && cs[0].loopLength == 0 && cs[0].loopStart == 0,
           "pattern: back to the cut length with a plain drag it stops looping, nothing left behind");
  }
}

void testAutomationCut() {
  Song s = makeEmptySong();
  const Tick L = s.barTicks();
  AutomationClip ac;
  ac.id = s.allocateId();
  ac.owner = kSongId;
  ac.index = static_cast<uint32_t>(SongParam::Tempo);
  ac.points = {{0, 0.0f, 0, AutoShape::Curve}, {L, 1.0f, 0, AutoShape::Curve}};
  s.automations.push_back(ac);
  Clip c;
  c.id = s.allocateId();
  c.kind = ClipKind::Automation;
  c.source = ac.id;
  c.length = L / 2;
  s.playlist.push_back(c);
  expect(clipCutShort(s, s.playlist[0]), "automation: a placement showing half its curve is cut short");
  expect(setClipsLoop(s, {c.id}, true), "automation: loop on");
  const Clip& l = s.playlist[0];
  expect(l.loop && l.loopStart == 0 && l.loopLength == L / 2 && s.automation(ac.id)->loop == 0,
         "automation: a cut placement loops its part, the curve's own loop length is left alone");
  s.playlist[0].length = 2 * L;
  const auto lanes = automationLanes(s);
  expect(lanes.size() == 1, "automation: one lane");
  if (!lanes.empty()) {
    const double v0 = lanes[0].valueAt(static_cast<double>(L / 8)), v1 = lanes[0].valueAt(static_cast<double>(L / 2 + L / 8)),
                 v3 = lanes[0].valueAt(static_cast<double>(3 * (L / 2) + L / 8));
    expect(std::fabs(v0 - v1) < 1e-9 && std::fabs(v0 - v3) < 1e-9 && v0 < 0.2, "automation: ... the first half of the curve repeats");
  }
  expect(setClipsLoop(s, {c.id}, false) && s.playlist[0].loopLength == 0, "automation: loop off clears its own loop");
}

void testProjectFile() {
  const Tick beat = Fx().s.beatTicks();
  Fx a;
  const ObjectId id = a.clip(0, 2 * beat, beat);
  TempoMap tm;
  tm.build(a.s);
  Clip l = withClipLoop(a.s, tm, a.at(id), true);
  l.length = 8 * beat;
  a.at(id) = l;
  const Json j = songToJson(a.s);
  Song back;
  std::string err;
  expect(songFromJson(j, back, &err) && back.playlist == a.s.playlist, "project: a cut clip's own loop round trips");
  const AudioBuffer ra = render(a.s), rb = render(back);
  expect(sameAudio(ra, rb) && energy(ra) > 0, "project: ... and plays the same after loading");
  // A file without the fields (every project before): the clip loops its whole pattern.
  Json k = j;
  for (Json& jc : k.object["playlist"].array) {
    jc.object.erase("loopStart");
    jc.object.erase("loopLength");
  }
  Song old;
  expect(songFromJson(k, old, &err) && old.playlist[0].loop && old.playlist[0].loopLength == 0 && clipLoopLength(old, old.playlist[0]) == 4 * beat,
         "project: files without the fields load as before (the whole pattern loops)");
  // Stale fields on a clip that does not loop are ignored.
  Json m = j;
  m.object["playlist"].array[0].object.erase("loop");
  Song plain;
  expect(songFromJson(m, plain, &err) && !plain.playlist[0].loop && plain.playlist[0].loopLength == 0, "project: no loop, no own loop");
}

void testFlpExport() {
  const Tick L = Fx().s.barTicks(), beat = Fx().s.beatTicks();
  Fx a;
  const ObjectId id = a.clip(0, 2 * beat, beat);
  a.clip(4 * L, L, 0);  // a plain clip of the same pattern
  TempoMap tm;
  tm.build(a.s);
  Clip l = withClipLoop(a.s, tm, a.at(id), true);
  l.length = 2 * L;
  a.at(id) = l;
  flp::Bytes bytes;
  flp::ExportReport rep;
  flp::ExportOptions eo;
  eo.useOrigin = false;
  expect(flp::exportFlp(a.s, bytes, rep, eo) && rep.counts["looping cut clips (written pass by pass)"] == 1, "flp: exported, the cut loop pass by pass");
  Song imp;
  flp::Report ir;
  std::string err;
  expect(importFlpBytes(bytes, imp, ir, &err), "flp: re-imported");
  const Tick ib = imp.beatTicks();
  int plain = 0;
  std::set<Tick> starts;
  std::string seen;
  bool ok = true;
  for (const Clip& c : imp.playlist) {
    if (c.kind != ClipKind::Pattern) continue;
    const Pattern* p = imp.pattern(c.pattern);
    if (!p || p->name != "Riff") continue;
    seen += " [" + std::to_string(c.start) + " " + std::to_string(imp.clipLength(c)) + " " + std::to_string(c.offset) + (c.loop ? " loop]" : "]");
    if (imp.clipLength(c) == 2 * ib) ok = ok && c.offset == ib, starts.insert(c.start);
    else ++plain;
  }
  expect(ok && plain == 1 && starts == std::set<Tick>{0, 2 * ib, 4 * ib, 6 * ib},
         ("flp: a loop of a cut clip is written as repeated clips of its cut length:" + seen).c_str());
}

void testFlImportUnchanged() {
  // FL import and old projects never set a clip's own loop.
  Fx a;
  a.clip(0, Fx().s.beatTicks(), 0, true);
  adoptPatternLoopModes(a.s);
  adoptOldClipLoops(a.s);
  expect(a.s.playlist[0].loopLength == 0 && a.s.playlist[0].loopStart == 0, "import: clips keep their pattern's loop window");
}

}  // namespace

int main() {
  test::hermeticTestEnv("k140");
  testPatternCut();
  testAutomationCut();
  testProjectFile();
  testFlpExport();
  testFlImportUnchanged();
  return finish();
}
