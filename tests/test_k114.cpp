// Gridline — K-114: clips loop only when asked (per clip, every clip kind).
//  * pattern clips: a plain extension stays empty past the pattern, a looping
//    clip repeats it; two clips of one pattern loop or not on their own;
//  * audio clips: a plain extension stops at the end of the audio, a looping
//    clip repeats its window (pass by pass, sample-exact), its loop start
//    (offset) wraps on trim, slip and split;
//  * automation clips: a placement that loops repeats the curve, the others
//    hold its last value;
//  * old projects: clips that repeated keep repeating (render identical to
//    their passes), the others do not; the project file round trips;
//  * FL: imports give every clip its pattern's loop mode, export writes what
//    each clip plays (passes, or cut at the pattern's end).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "core/Automation.h"
#include "core/ClipLoop.h"
#include "core/PluginInfo.h"
#include "core/PluginParams.h"
#include "core/Project.h"
#include "core/WavWriter.h"
#include "core/flp/Flp.h"
#include "core/flp/FlpExport.h"
#include "engine/ClipOps.h"
#include "engine/PlaylistTools.h"
#include "engine/Render.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

constexpr double kSr = 44100.0;
constexpr double kPi = 3.14159265358979323846;

// One synth channel and a 1-bar pattern of four notes.
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
  ObjectId clip(Tick start, Tick len, Tick offset, bool loop, int track = 0) {
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
};

AudioBuffer render(const Song& s) {
  RenderOptions o;
  o.threads = 1;
  o.tail = s.barTicks();
  return renderSong(s, o);
}
bool sameAudio(const AudioBuffer& a, const AudioBuffer& b) { return a.frames == b.frames && a.frames > 0 && a.data == b.data; }
double peak(const AudioBuffer& b, size_t f0, size_t f1) {
  double p = 0;
  for (size_t i = f0; i < std::min(f1, b.frames); ++i) p = std::max({p, std::fabs(static_cast<double>(b.at(0, i))), std::fabs(static_cast<double>(b.at(1, i)))});
  return p;
}
size_t frameAt(const Song& s, Tick t) { return static_cast<size_t>(std::llround(static_cast<double>(t) * 60.0 / (s.tempo * s.ppq) * kSr)); }

// A 1.2 s file of clicks (one every 0.3 s, the first louder) on a sampler channel.
std::string clickFile() {
  const size_t n = static_cast<size_t>(1.2 * kSr);
  AudioBuffer b(2, n);
  for (int k = 0; k < 4; ++k) {
    const size_t a = static_cast<size_t>(k * 0.3 * kSr);
    for (size_t i = 0; i < static_cast<size_t>(0.01 * kSr) && a + i < n; ++i)
      b.at(0, a + i) = b.at(1, a + i) = static_cast<float>((k == 0 ? 0.9 : 0.4) * std::sin(2 * kPi * 1500 * static_cast<double>(i) / kSr));
  }
  const std::string path = (fs::temp_directory_path() / "gl_k114_clicks.wav").string();
  writeWav(path, b, static_cast<int>(kSr), 24);
  return path;
}
struct AudioFx {
  Song s;
  ObjectId ch = kNoId, clip = kNoId;
  explicit AudioFx(const std::string& path) {
    s = makeEmptySong();
    s.tempo = 120;
    Channel c;
    c.id = s.allocateId();
    c.name = "Clicks";
    c.plugin = defaultPluginState("gl.sampler");
    c.plugin.data = path;
    c.mixerTrack = s.masterId();
    c.audioClip = true;
    s.channels.push_back(c);
    ch = c.id;
    Clip k;
    k.id = clip = s.allocateId();
    k.kind = ClipKind::Audio;
    k.source = ch;
    k.start = s.barTicks();
    k.length = audioClipNaturalLength(s, k);
    s.playlist.push_back(k);
  }
  Clip& c() { return *s.clip(clip); }
};

void testPatternClips() {
  const Tick L = Fx().s.barTicks();
  {  // a plain extension: the clip gets longer, nothing past the pattern
    Fx a;
    const ObjectId id = a.clip(0, L, 0, false);
    const std::vector<Clip> out = pltools::resizeEnds(a.s, a.s.playlist, {id}, 2 * L, false);
    expect(out[0].length == 3 * L && !out[0].loop, "pattern: a plain right-edge drag extends the clip and does not loop it");
    a.s.playlist = out;
    expect(clipPasses(a.s, a.s.playlist[0]).size() == 1 && !clipRepeats(a.s, a.s.playlist[0]), "pattern: ... one pass, no repeat");
    const AudioBuffer r = render(a.s);
    Fx one;
    one.clip(0, L, 0, false);
    const AudioBuffer r1 = render(one.s);
    double diff = r.frames > 0 && r1.frames > 0 ? 0 : 1;
    for (size_t i = 0; i < std::min(r.frames, r1.frames); ++i)
      diff = std::max({diff, std::fabs(static_cast<double>(r.at(0, i) - r1.at(0, i))), std::fabs(static_cast<double>(r.at(1, i) - r1.at(1, i)))});
    expect(diff < 1e-6 && peak(r, r1.frames, r.frames) < 1e-6, "pattern: ... sounds like the unextended clip (nothing past the pattern's end)");
    // Notes added later past the old end fill the empty part.
    a.s.pattern(a.pat)->notes[a.ch].push_back(Note(L + L / 2, L / 8, 72, 100));
    a.s.pattern(a.pat)->bars = 2;
    const AudioBuffer r2 = render(a.s);
    expect(peak(r2, frameAt(a.s, L + L / 2), frameAt(a.s, L + L / 2) + 4000) > 0.01, "pattern: a note added past the old end plays there");
  }
  {  // a looping clip repeats its pattern, like its passes laid out as clips
    Fx a;
    a.clip(0, 3 * L + L / 4, L / 3, true);
    Fx b;
    for (const ClipPass& p : loopPasses(L / 3, 3 * L + L / 4, L)) b.clip(p.at, p.len, p.content, false);
    expect(sameAudio(render(a.s), render(b.s)), "pattern: a looping clip plays its passes, sample-exact");
  }
  {  // per clip: two clips of one pattern, one loops, one does not
    Fx a;
    const ObjectId lp = a.clip(0, 2 * L, 0, true, 0), pl = a.clip(0, 2 * L, 0, false, 1);
    expect(clipPasses(a.s, *a.s.clip(lp)).size() == 2 && clipPasses(a.s, *a.s.clip(pl)).size() == 1, "per clip: one pattern, one clip loops, the other does not");
    Fx b;
    b.clip(0, L, 0, false, 0);
    b.clip(L, L, 0, false, 0);
    b.clip(0, 2 * L, 0, false, 1);
    expect(sameAudio(render(a.s), render(b.s)), "per clip: ... and they play that way");
    expect(patternLoopMode(a.s, *a.s.pattern(a.pat)), "per clip: FL's pattern loop mode follows the clip that loops");
    a.s.clip(lp)->loop = false;
    expect(!patternLoopMode(a.s, *a.s.pattern(a.pat)), "per clip: ... and is off when only plain extensions run past the end");
  }
}

void testAudioClips() {
  const std::string path = clickFile();
  {
    AudioFx a(path);
    TempoMap tm;
    tm.build(a.s);
    const Tick nat = a.c().length;
    std::vector<Clip> out = pltools::resizeEnds(a.s, a.s.playlist, {a.clip}, 3 * nat, false, &tm, false);
    expect(out[0].length == nat && !out[0].loop, "audio: a plain right-edge drag stops at the end of the file");
    // With the loop key (the drag's start, withClipLoop): it repeats as far as dragged.
    std::vector<Clip> from = a.s.playlist;
    from[0] = withClipLoop(a.s, tm, from[0], true);
    out = pltools::resizeEnds(a.s, from, {a.clip}, 3 * nat, false, &tm, false);
    expect(out[0].length == 4 * nat && out[0].loop, "audio: with the loop key it grows past the end and loops");
    out = pltools::resizeEnds(a.s, from, {a.clip}, 3 * nat, true, &tm, false);
    expect(out[0].length == 4 * nat && out[0].stretch == 1.0, "audio: a looping clip in stretch mode loops instead of stretching");
    a.s.playlist = out;
    expect(audioLoopLength(a.s, a.c(), tm) == nat, "audio: one pass is the file");
    const std::vector<Clip> passes = audioLoopPasses(a.s, a.c(), tm);
    expect(passes.size() == 4 && passes[1].start == a.c().start + nat && passes[3].length == nat && !passes[2].loop, "audio: four passes");
    AudioFx b(path);
    b.s.playlist = passes;
    for (size_t k = 1; k < b.s.playlist.size(); ++k) b.s.playlist[k].id = b.s.allocateId();
    const AudioBuffer ra = render(a.s), rb = render(b.s);
    expect(sameAudio(ra, rb), "audio: a looping clip plays its passes as clips would, sample-exact");
    const size_t p3 = frameAt(a.s, a.c().start + 2 * nat);
    expect(peak(ra, p3, p3 + 400) > 0.5, "audio: the third pass starts with the file's first click");
  }
  {  // shorter than its file: it loops what it shows
    AudioFx a(path);
    TempoMap tm;
    tm.build(a.s);
    const Tick half = a.c().length / 2;
    a.c().length = half;
    Clip l = withClipLoop(a.s, tm, a.c(), true);
    expect(l.loop && l.audioEnd > 0.55 && l.audioEnd < 0.65, "audio: looping a trimmed clip loops the part it shows");
    l.length = 3 * half;
    a.c() = l;
    expect(audioLoopPasses(a.s, a.c(), tm).size() == 3, "audio: ... in passes of what it showed");
    // The loop start: Ctrl + left edge grows it to the left with the audio wrapping.
    std::vector<Clip> t = pltools::trimStarts(a.s, tm, a.s.playlist, {a.clip}, -half / 2, false, false);
    expect(t[0].start == a.c().start - half / 2 && t[0].offset == half - half / 2 && t[0].audioStart == a.c().audioStart,
           "audio: the left edge of a looping clip moves its loop start (the audio window stays)");
    // Slip moves where the passes start; split keeps the loop going.
    const Clip sl = pltools::slipClip(a.s, tm, a.c(), half / 4);
    expect(sl.offset == half - half / 4 && sl.audioStart == a.c().audioStart, "audio: slipping a looping clip wraps its loop start");
    const ObjectId r = splitClipAt(a.s, a.clip, a.c().start + half + half / 3);
    expect(r != kNoId && a.s.clip(r)->loop && a.s.clip(r)->offset == half / 3 && a.s.clip(r)->audioStart == a.c().audioStart,
           "audio: split, the right part loops on from inside the loop");
    // Loop off: it plays its audio once again, from where its first pass started.
    Clip off = withClipLoop(a.s, tm, t[0], false);
    expect(!off.loop && off.offset == 0 && off.audioStart > 0.2, "audio: loop off, the audio starts where the loop started");
  }
}

void testAutomationClips() {
  Song s = makeEmptySong();
  AutomationClip ac;
  ac.id = s.allocateId();
  ac.owner = kSongId;
  ac.index = static_cast<uint32_t>(SongParam::Tempo);
  const Tick L = s.barTicks();
  ac.points = {{0, 0.0f, 0, AutoShape::Curve}, {L, 1.0f, 0, AutoShape::Curve}};
  ac.loop = L;
  s.automations.push_back(ac);
  for (int k = 0; k < 2; ++k) {
    Clip c;
    c.id = s.allocateId();
    c.kind = ClipKind::Automation;
    c.source = ac.id;
    c.track = k;
    c.start = k == 0 ? 0 : 8 * L;
    c.length = 3 * L;
    c.loop = k == 0;
    s.playlist.push_back(c);
  }
  const auto lanes = automationLanes(s);
  expect(lanes.size() == 1 && lanes[0].inst.size() == 2, "automation: two placements");
  if (lanes.size() == 1) {
    const double loopV = lanes[0].valueAt(static_cast<double>(L + L / 4)), holdV = lanes[0].valueAt(static_cast<double>(8 * L + L + L / 4));
    expect(loopV < 0.4 && holdV > 0.99, "automation: the looping placement repeats the curve, the other holds its last value");
  }
  // Ctrl+drag (setClipsLoop) on a curve without a loop length gives it the placement's.
  s.automation(ac.id)->loop = 0;
  s.playlist[0].loop = false;
  expect(setClipsLoop(s, {s.playlist[0].id}, true) && s.playlist[0].loop && s.automation(ac.id)->loop == 3 * L,
         "automation: looping a placement gives its curve the placement's length to loop");
}

void testOldProjects() {
  const Tick L = Fx().s.barTicks();
  Fx a;
  const ObjectId rep = a.clip(0, 3 * L, L / 4, true);   // repeated in older Gridline (its pattern loops)
  const ObjectId one = a.clip(4 * L, L / 2, 0, false);  // shorter than its pattern: never repeated
  Json j = songToJson(a.s);
  // As an older Gridline wrote it: no per-clip loop, no "clipLoops" key.
  j.object.erase("clipLoops");
  for (Json& c : j.object["playlist"].array) c.object.erase("loop");
  Song old;
  std::string err;
  expect(songFromJson(j, old, &err), "old project: loads");
  expect(old.clip(rep) && old.clip(rep)->loop && old.clip(one) && !old.clip(one)->loop,
         "old project: the clip that repeated loops, the one that did not does not");
  expect(sameAudio(render(old), render(a.s)), "old project: renders exactly as it did (bit-identical)");
  // A pattern whose loop mode was off never repeated.
  Json j2 = j;
  Json no;
  no.type = Json::Type::Bool;
  no.boolean = false;
  for (Json& p : j2.object["patterns"].array) p.object["loop"] = no;
  Song old2;
  expect(songFromJson(j2, old2, &err) && !old2.clip(rep)->loop, "old project: a pattern whose loop mode was off: its clips do not loop");
  // New files keep each clip's own loop.
  Song back;
  expect(songFromJson(songToJson(a.s), back, &err) && back.playlist == a.s.playlist, "project: per-clip loops round trip");
  // An automation curve with a loop length looped every placement before.
  Song s = makeEmptySong();
  AutomationClip ac;
  ac.id = s.allocateId();
  ac.owner = kSongId;
  ac.index = static_cast<uint32_t>(SongParam::Tempo);
  ac.points = {{0, 0.2f, 0, AutoShape::Curve}, {96, 0.8f, 0, AutoShape::Curve}};
  ac.loop = 192;
  s.automations.push_back(ac);
  Clip c;
  c.id = s.allocateId();
  c.kind = ClipKind::Automation;
  c.source = ac.id;
  c.length = 4 * 192;
  s.playlist.push_back(c);
  Json ja = songToJson(s);
  ja.object.erase("clipLoops");
  Song olda;
  expect(songFromJson(ja, olda, &err) && olda.playlist[0].loop, "old project: placements of a looping automation curve loop");
}

void testFl() {
  const Tick L = Fx().s.barTicks();
  Fx a;
  a.clip(0, 3 * L, 0, true, 0);  // loops
  Pattern p2 = *a.s.pattern(a.pat);
  p2.id = a.s.allocateId();
  p2.name = "Plain";
  a.s.patterns.push_back(p2);
  Clip c2;
  c2.id = a.s.allocateId();
  c2.pattern = p2.id;
  c2.track = 1;
  c2.length = 2 * L;  // extended, empty past its pattern
  a.s.playlist.push_back(c2);
  flp::Bytes bytes;
  flp::ExportReport rep;
  flp::ExportOptions eo;
  eo.useOrigin = false;
  expect(flp::exportFlp(a.s, bytes, rep, eo), "flp: exported");
  Song imp;
  flp::Report ir;
  std::string err;
  expect(importFlpBytes(bytes, imp, ir, &err), "flp: re-imported");
  bool modes = true, clips = true;
  int named = 0;
  for (const Pattern& p : imp.patterns)
    if (p.name == "Riff" || p.name == "Plain") modes = modes && p.loop == (p.name == "Riff"), named++;  // the empty song's own pattern has no clips
  modes = modes && named == 2;
  for (const Clip& c : imp.playlist)
    if (const Pattern* p = imp.pattern(c.pattern)) clips = clips && c.loop == p->loop;
  std::string seen;
  for (const Pattern& p : imp.patterns) seen += " " + p.name + "=" + (p.loop ? "loop" : "plain");
  expect(modes, (std::string("flp: each pattern's loop mode is what its clips play:") + seen).c_str());
  expect(clips, "flp import: every pattern clip takes its pattern's loop mode");
  // A pattern with a looping clip and a plain extension: FL loops per pattern,
  // so the plain one is written cut at its pattern's end (it was silent past it).
  Fx m;
  m.clip(0, 2 * L, 0, true, 0);
  m.clip(4 * L, 3 * L, 0, false, 1);
  flp::Bytes mb;
  flp::ExportReport mr;
  expect(flp::exportFlp(m.s, mb, mr, eo) && mr.counts["pattern clips cut at their pattern's end (it loops in FL)"] == 1,
         "flp export: a plain extension of a pattern that loops in FL is cut at the pattern's end");
  Song mi;
  flp::Report mir;
  bool cut = importFlpBytes(mb, mi, mir, &err);
  for (const Clip& c : mi.playlist)
    if (c.track == 1) cut = cut && mi.clipLength(c) == L && c.start == 4 * L;
  expect(cut, "flp export: ... and comes back one pattern long");
  // One looping clip and two plain extensions of a pattern: the loop wins (FL
  // loops the pattern), the plain ones are cut, the looping one stays whole.
  Fx n;
  n.clip(0, 2 * L + L / 2, 0, true, 0);
  n.clip(4 * L, 2 * L, 0, false, 1);
  n.clip(8 * L, 2 * L, 0, false, 2);
  flp::Bytes nb;
  flp::ExportReport nr;
  expect(flp::exportFlp(n.s, nb, nr, eo) && nr.counts["pattern clips cut at their pattern's end (it loops in FL)"] == 2 &&
             nr.counts["looping pattern clips (written pass by pass)"] == 0,
         "flp export: a pattern with looping and plain clips loops in FL, the plain ones cut");
  Song ni;
  flp::Report nir;
  int track0 = 0;
  bool whole = importFlpBytes(nb, ni, nir, &err);
  for (const Clip& c : ni.playlist)
    if (c.track == 0) track0++, whole = whole && ni.clipLength(c) == 2 * L + L / 2 && c.loop;
  expect(whole && track0 == 1, "flp export: ... the looping clip comes back whole and looping");
}

// A plain right-edge drag back to a looping clip's own length (or shorter)
// stops its loop; further out it keeps looping; per clip of a selection.
void testUnloop() {
  const Tick L = Fx().s.barTicks();
  {  // pattern clips
    Fx a;
    const ObjectId id = a.clip(0, 3 * L, 0, true);
    const ObjectId late = a.clip(8 * L, 3 * L, L / 4, true, 1);  // its loop starts a quarter in
    TempoMap tm;
    tm.build(a.s);
    expect(loopFirstPass(a.s, a.s.playlist[0], tm) == L && loopFirstPass(a.s, a.s.playlist[1], tm) == L - L / 4,
           "unloop: a pattern clip's own length (from its loop start)");
    auto drag = [&](const std::vector<Clip>& from, std::set<ObjectId> sel, Tick d) {
      std::vector<Clip> out = pltools::resizeEnds(a.s, from, sel, d, false);
      unloopWithinFirstPass(a.s, out, sel, tm);
      return out;
    };
    std::vector<Clip> out = drag(a.s.playlist, {id}, -L);
    expect(out[0].length == 2 * L && out[0].loop, "unloop: pulled back but still past the pattern, it keeps looping");
    out = drag(a.s.playlist, {id}, -2 * L);
    expect(out[0].length == L && !out[0].loop, "unloop: pulled back to the pattern's end, it stops looping");
    out = drag(a.s.playlist, {id}, -2 * L - L / 2);
    expect(out[0].length == L / 2 && !out[0].loop, "unloop: ... and shorter");
    const std::vector<Clip> again = drag(out, {id}, 2 * L);
    expect(again[0].length == 2 * L + L / 2 && !again[0].loop, "unloop: a later plain extend just extends");
    // The same drag works from its base every frame (live): back out, it loops again.
    out = drag(a.s.playlist, {id}, 0);
    expect(out[0].loop, "unloop: back out in the same drag, it loops again");
    // Several selected: each clip on its own.
    out = drag(a.s.playlist, {id, late}, -2 * L);
    expect(!out[0].loop && out[1].loop, "unloop: per clip (one back within its pattern, the other still past it)");
    out = drag(a.s.playlist, {id, late}, -2 * L - L / 4);
    expect(!out[1].loop && out[1].offset == L / 4, "unloop: a clip whose loop starts inside the pattern: its own length is the rest of it");
  }
  {  // audio: back to the end of the file
    AudioFx a(clickFile());
    TempoMap tm;
    tm.build(a.s);
    const Tick nat = a.c().length;
    a.c() = withClipLoop(a.s, tm, a.c(), true);
    a.c().length = 3 * nat;
    std::vector<Clip> out = pltools::resizeEnds(a.s, a.s.playlist, {a.clip}, -nat, false, &tm, false);
    unloopWithinFirstPass(a.s, out, {a.clip}, tm);
    expect(out[0].loop, "unloop: audio still past its file keeps looping");
    out = pltools::resizeEnds(a.s, a.s.playlist, {a.clip}, -2 * nat, false, &tm, false);
    expect(unloopWithinFirstPass(a.s, out, {a.clip}, tm) == 1 && !out[0].loop && out[0].length == nat,
           "unloop: audio back to the end of its file stops looping");
    const std::vector<Clip> ext = pltools::resizeEnds(a.s, out, {a.clip}, 2 * nat, false, &tm, false);
    expect(!ext[0].loop && ext[0].length == nat, "unloop: ... a later plain drag stops at the end of the file again");
  }
  {  // automation: back to its curve's loop length
    Song s = makeEmptySong();
    AutomationClip ac;
    ac.id = s.allocateId();
    ac.owner = kSongId;
    ac.index = static_cast<uint32_t>(SongParam::Tempo);
    ac.points = {{0, 0.0f, 0, AutoShape::Curve}, {L, 1.0f, 0, AutoShape::Curve}};
    ac.loop = L;
    s.automations.push_back(ac);
    Clip c;
    c.id = s.allocateId();
    c.kind = ClipKind::Automation;
    c.source = ac.id;
    c.length = 3 * L;
    c.loop = true;
    s.playlist.push_back(c);
    TempoMap tm;
    tm.build(s);
    std::vector<Clip> out = pltools::resizeEnds(s, s.playlist, {c.id}, -L, false);
    expect(unloopWithinFirstPass(s, out, {c.id}, tm) == 0 && out[0].loop, "unloop: automation still past its curve keeps looping");
    out = pltools::resizeEnds(s, s.playlist, {c.id}, -2 * L, false);
    expect(unloopWithinFirstPass(s, out, {c.id}, tm) == 1 && !out[0].loop, "unloop: automation back to its curve's length stops looping");
  }
}
}  // namespace

int main() {
  test::hermeticTestEnv("k114");
  testPatternClips();
  testAudioClips();
  testAutomationClips();
  testOldProjects();
  testFl();
  testUnloop();
  return finish();
}
