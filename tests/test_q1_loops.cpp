// Gridline — Q-1 looping pattern clips and Q-3 Shift+drag copies (core side).
//  * loop passes: offsets, wraps, partial first / last passes;
//  * playback: a stretched clip of a looping pattern renders sample-exactly
//    like the same passes laid out as separate clips (any loop start), a
//    pattern whose loop mode is off plays nothing past its end;
//  * pattern automation repeats with the notes;
//  * editing: split, slip, left-edge trim and resize keep the loop wrapping;
//    chop onsets and stutter see every pass;
//  * project and FLP round trips keep the loop mode and the clips exactly;
//    FL demo projects keep FL's loop setting;
//  * Shift+drag copies: clip copies keep every setting, grouped copies form
//    new groups; note copies keep every property, new note groups.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "core/ClipLoop.h"
#include "core/Document.h"
#include "core/Grouping.h"
#include "core/PatternAutomation.h"
#include "core/PluginInfo.h"
#include "core/Project.h"
#include "core/flp/Flp.h"
#include "core/flp/FlpExport.h"
#include "engine/Chop.h"
#include "engine/ClipOps.h"
#include "engine/PlaylistTools.h"
#include "engine/Render.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

// One synth channel, one 1-bar pattern (four notes, the last one ringing to
// the bar's end), no clips.
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
    for (int k = 0; k < 4; ++k) {
      Note n;
      n.start = k * b;
      n.length = k == 3 ? b : b / 2;
      n.pitch = 60 + 3 * k;
      n.velocity = 90 + k * 5;
      p.notes[ch].push_back(n);
    }
    s.patterns.push_back(p);
    pat = p.id;
  }
  ObjectId clip(Tick start, Tick len, Tick offset, int track = 0) {
    Clip c;
    c.id = s.allocateId();
    c.pattern = pat;
    c.track = track;
    c.start = start;
    c.length = len;
    c.offset = offset;
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

double peak(const AudioBuffer& b, size_t f0, size_t f1) {
  double p = 0;
  for (size_t i = f0; i < std::min(f1, b.frames); ++i) p = std::max({p, std::fabs(static_cast<double>(b.at(0, i))), std::fabs(static_cast<double>(b.at(1, i)))});
  return p;
}

bool sameAudio(const AudioBuffer& a, const AudioBuffer& b) { return a.frames == b.frames && a.frames > 0 && a.data == b.data; }

void testPasses() {
  auto v = loopPasses(0, 1000, 384);
  expect(v.size() == 3 && v[0].at == 0 && v[0].len == 384 && v[1].at == 384 && v[2].at == 768 && v[2].len == 232 && v[2].content == 0,
         "passes: three passes, the last one partial");
  v = loopPasses(100, 700, 384);
  expect(v.size() == 3 && v[0].content == 100 && v[0].len == 284 && v[1].at == 284 && v[1].content == 0 && v[1].len == 384 && v[2].at == 668 &&
             v[2].len == 32,
         "passes: an offset starts the first pass mid-pattern and wraps");
  v = loopPasses(384 * 3 + 50, 100, 384);
  expect(v.size() == 1 && v[0].content == 50, "passes: an offset past the loop wraps into it");
  v = loopPasses(-10, 20, 384);
  expect(v.size() == 2 && v[0].content == 374 && v[0].len == 10 && v[1].content == 0, "passes: a negative offset wraps from the end");
  v = loopPasses(100, 5000, 0);
  expect(v.size() == 1 && v[0].content == 100 && v[0].len == 5000, "passes: no loop = one pass");
  expect(wrapLoopOffset(-1, 384) == 383 && wrapLoopOffset(384, 384) == 0 && wrapLoopOffset(7, 0) == 7, "wrap: offsets wrap into the loop");
}

void testPlayback() {
  const Tick L = Fx().s.barTicks();
  for (Tick off : {Tick(0), L / 4, L / 3 + 7, L - 1}) {
    Fx a;
    const Tick len = 3 * L + L / 2 + 13;
    a.clip(L, len, off);
    Fx b;
    // The same passes as separate clips.
    for (const ClipPass& p : loopPasses(off, len, L)) b.clip(L + p.at, p.len, p.content);
    const AudioBuffer ra = render(a.s), rb = render(b.s);
    char name[96];
    std::snprintf(name, sizeof name, "playback: a looping clip (loop start %lld) = its passes as clips, sample-exact", static_cast<long long>(off));
    expect(sameAudio(ra, rb), name);
  }
  {  // a repeat sounds the same as the first pass
    Fx a;
    a.clip(0, 3 * L, 0);
    const AudioBuffer r = render(a.s);
    const double spt = 60.0 / (a.s.tempo * a.s.ppq) * 44100.0;
    const size_t p1 = static_cast<size_t>(std::llround(L * spt)), p2 = static_cast<size_t>(std::llround(2 * L * spt));
    expect(peak(r, p1, p1 + 2000) > 0.01 && peak(r, p2, p2 + 2000) > 0.01, "playback: the second and third passes sound");
  }
  {  // loop mode off: nothing past the pattern's end (FL)
    Fx a;
    (*a.s.pattern(a.pat)).loop = false;
    a.clip(0, 3 * L, 0);
    Fx b;
    (*b.s.pattern(b.pat)).loop = false;
    b.clip(0, L, 0);
    const AudioBuffer ra = render(a.s), rb = render(b.s);
    const double spt = 60.0 / (a.s.tempo * a.s.ppq) * 44100.0;
    const size_t p2 = static_cast<size_t>(std::llround(2 * L * spt));
    expect(peak(ra, p2, p2 + 20000) < 1e-6, "playback: a pattern whose loop mode is off is silent past its end");
    bool same = true;
    size_t at = 0;
    for (size_t i = 0; i < std::min(ra.frames, rb.frames) && same; ++i) same = ra.at(0, i) == rb.at(0, i), at = i;
    if (!same) std::printf("# loop off: first difference at frame %zu\n", at);
    expect(same, "playback: ... and plays its single pass as before");
  }
  {  // the clip's end cuts a note of the last pass
    Fx a;
    a.clip(0, L + a.s.beatTicks() * 3 + a.s.beatTicks() / 4, 0);
    Fx b;
    b.clip(0, L, 0);
    b.clip(L, a.s.beatTicks() * 3 + a.s.beatTicks() / 4, 0);
    expect(sameAudio(render(a.s), render(b.s)), "playback: the last partial pass ends with the clip");
  }
}

void testAutomation() {
  Fx a;
  setPatternLane((*a.s.pattern(a.pat)), a.ch, 0, {{0, 0.0f, 0, AutoShape::Curve}, {a.s.barTicks(), 1.0f, 0, AutoShape::Curve}});
  const Tick L = a.s.barTicks();
  a.clip(0, 3 * L, L / 2);
  const auto lanes = patternAutomationLanes(a.s);
  expect(lanes.size() == 1 && lanes[0].inst.size() == 4, "automation: one instance per pass");
  if (lanes.size() == 1 && lanes[0].inst.size() == 4) {
    const auto& l = lanes[0];
    const double atWrap = l.valueAt(static_cast<double>(L / 2) + 1), beforeWrap = l.valueAt(static_cast<double>(L / 2) - 1);
    expect(atWrap < 0.05 && beforeWrap > 0.9, "automation: the curve starts again at the loop point");
  }
  const auto single = singlePatternLanes((*a.s.pattern(a.pat)), 2 * L, 0, L);
  expect(single.size() == 1 && single[0].inst.size() == 2, "automation: live / pattern-mode lanes repeat too");
}

void testEditing() {
  Fx a;
  const Tick L = a.s.barTicks();
  const ObjectId id = a.clip(0, 3 * L, 0);
  // Split in the second pass: the right part starts mid-pattern.
  const ObjectId r = splitClipAt(a.s, id, L + L / 4);
  expect(r != kNoId && a.s.clip(r)->offset == L / 4, "split: the right part's loop start wraps into the pattern");
  // Slip wraps.
  TempoMap tm;
  tm.build(a.s);
  Clip sl = pltools::slipClip(a.s, tm, *a.s.clip(id), L / 8);
  expect(sl.offset == L - L / 8, "slip: dragging the content right wraps the loop start");
  // Left edge further left than the content's start: the loop start follows.
  Fx b;
  const ObjectId t = b.clip(2 * L, L, 0);
  std::vector<Clip> out = pltools::trimStarts(b.s, tm, b.s.playlist, {t}, -L / 4, false, false);
  expect(out[0].start == 2 * L - L / 4 && out[0].length == L + L / 4 && out[0].offset == L - L / 4,
         "trim: the left edge grows a looping clip and moves its loop start");
  (*b.s.pattern(b.pat)).loop = false;
  out = pltools::trimStarts(b.s, tm, b.s.playlist, {t}, -L / 4, false, false);
  expect(out[0].start == 2 * L && out[0].offset == 0, "trim: a pattern that does not loop cannot grow past its start");
  // Pattern length change reflows the repeats.
  Fx c;
  const ObjectId lc = c.clip(0, 4 * L, 0);
  expect(clipPasses(c.s, *c.s.clip(lc)).size() == 4, "reflow: four passes of a one-bar pattern");
  (*c.s.pattern(c.pat)).bars = 2;
  expect(clipPasses(c.s, *c.s.clip(lc)).size() == 2, "reflow: two passes once the pattern is two bars");
  // Chop onsets see every pass.
  Fx d;
  const ObjectId oc = d.clip(0, 2 * L, 0);
  expect(chop::patternOnsets(d.s, *d.s.clip(oc)).size() == 7, "chop: note onsets of every pass (the clip start excluded)");
}

void testFiles() {
  Fx a;
  const Tick L = a.s.barTicks();
  a.clip(0, 3 * L + 5, L / 3);
  Pattern p2 = (*a.s.pattern(a.pat));
  p2.id = a.s.allocateId();
  p2.name = "Once";
  p2.loop = false;
  a.s.patterns.push_back(p2);
  Clip c2;
  c2.id = a.s.allocateId();
  c2.pattern = p2.id;
  c2.track = 1;
  c2.start = 0;
  c2.length = 2 * L;
  a.s.playlist.push_back(c2);
  Song back;
  std::string err;
  expect(songFromJson(songToJson(a.s), back, &err) && back.patterns == a.s.patterns && back.playlist == a.s.playlist,
         "project: loop mode and looping clips round trip");
  flp::Bytes bytes;
  flp::ExportReport rep;
  flp::ExportOptions eo;
  eo.useOrigin = false;
  expect(flp::exportFlp(a.s, bytes, rep, eo), "flp: exported");
  Song imp;
  flp::Report ir;
  expect(importFlpBytes(bytes, imp, ir, &err), "flp: re-imported");
  bool ok = imp.patterns.size() >= 2;
  for (const Pattern& p : imp.patterns)
    if (p.name == "Riff") ok = ok && p.loop;
    else if (p.name == "Once") ok = ok && !p.loop;
  expect(ok, "flp: each pattern keeps its loop mode (event 26)");
  bool clips = imp.playlist.size() == 2;
  for (const Clip& c : imp.playlist) {
    const Clip& o = c.track == 0 ? a.s.playlist[0] : a.s.playlist[1];
    clips = clips && c.start == o.start && imp.clipLength(c) == a.s.clipLength(o) && c.offset == o.offset;
  }
  expect(clips, "flp: looping clips keep start, length and loop start");
  bool passes = true;
  for (const Clip& c : imp.playlist) {
    const Clip& o = c.track == 0 ? a.s.playlist[0] : a.s.playlist[1];
    const auto x = clipPasses(imp, c), y = clipPasses(a.s, o);
    passes = passes && x.size() == y.size();
    for (size_t i = 0; passes && i < x.size(); ++i) passes = x[i].at == y[i].at && x[i].len == y[i].len && x[i].content == y[i].content;
  }
  expect(passes, "flp: the re-imported clips play the same passes");
  // A second export of the import is byte-identical (stable encoding).
  flp::Bytes again;
  flp::ExportReport rep2;
  expect(flp::exportFlp(imp, again, rep2, flp::ExportOptions{}) && again == bytes, "flp: export → import → export is byte-identical");
}

void testDemo() {
  const std::string root = "/Applications/FL Studio 2026.app/Contents/Resources/FL/Data";
  std::string path;
  std::error_code ec;
  if (fs::exists(root, ec))
    for (auto it = fs::recursive_directory_iterator(root, ec); it != fs::recursive_directory_iterator(); it.increment(ec))
      if (it->path().filename().string().find("Song For You") != std::string::npos && it->path().extension() == ".flp") path = it->path().string();
  if (path.empty()) {
    std::printf("# FL demo not found: skipped\n");
    return;
  }
  Song s;
  flp::Report rep;
  std::string err;
  expect(importFlp(path, s, rep, &err), "demo: imports");
  int looping = 0;
  for (const Pattern& p : s.patterns) looping += p.loop;
  expect(looping == 1, "demo: exactly FL's one looping pattern loops");
}

void testCopies() {
  Fx a;
  const Tick L = a.s.barTicks();
  const ObjectId c1 = a.clip(0, 2 * L, L / 4), c2 = a.clip(0, L, 0, 1), c3 = a.clip(4 * L, L, 0, 2);
  for (Clip& c : a.s.playlist) c.group = 777, c.muted = c.id == c2;
  a.s.playlist[0].gain = 0.5f;
  const auto copies = pltools::cloneClips(a.s, {c1, c2});
  expect(copies.size() == 2, "copies: two clips copied");
  const Clip *k1 = a.s.clip(copies[0]), *k2 = a.s.clip(copies[1]);
  expect(k1 && k2 && k1->offset == L / 4 && k1->length == 2 * L && k1->pattern == a.pat && k1->gain == 0.5f && k2->muted,
         "copies: every clip setting kept, the same pattern");
  expect(k1 && k2 && k1->group == k2->group && k1->group != 777 && k1->group != kNoId && a.s.clip(c3)->group == 777,
         "copies: grouped clips form one new group, the originals keep theirs");
  // Notes.
  std::vector<Note> ns(3);
  for (int i = 0; i < 3; ++i) {
    ns[static_cast<size_t>(i)].start = i * 96;
    ns[static_cast<size_t>(i)].length = 48;
    ns[static_cast<size_t>(i)].pitch = 60 + i;
    ns[static_cast<size_t>(i)].velocity = 30 + i;
    ns[static_cast<size_t>(i)].pan = -20;
    ns[static_cast<size_t>(i)].color = 5;
    ns[static_cast<size_t>(i)].slide = i == 1;
    ns[static_cast<size_t>(i)].porta = i == 2;
    ns[static_cast<size_t>(i)].group = i < 2 ? 4 : 0;
  }
  std::vector<bool> sel{true, true, true};
  const std::vector<int> map = tools::cloneNotes(ns, sel);
  expect(ns.size() == 6 && map[0] == 3 && map[2] == 5 && !sel[0] && sel[3] && sel[5], "note copies: appended and selected");
  bool props = true;
  for (int i = 0; i < 3; ++i) {
    const Note &o = ns[static_cast<size_t>(i)], &c = ns[static_cast<size_t>(map[static_cast<size_t>(i)])];
    props = props && o.start == c.start && o.pitch == c.pitch && o.velocity == c.velocity && o.pan == c.pan && o.color == c.color &&
            o.slide == c.slide && o.porta == c.porta;
  }
  expect(props, "note copies: velocity, pan, colour, slide and porta kept");
  expect(ns[3].group == ns[4].group && ns[3].group != 4 && ns[3].group != 0 && ns[5].group == 0, "note copies: grouped notes form a new group");
  // One undo step for a Shift+drag copy (clone + move in a group); cancel leaves nothing.
  Document d(a.s);
  const Song before = d.song();
  d.beginGroup("copy clips");
  std::vector<ObjectId> cp;
  d.editSong("copy clips", [&](Song& x) { cp = pltools::cloneClips(x, {c1}); });
  d.editClips("move clips", [&](std::vector<Clip>& v) {
    for (Clip& c : v)
      if (c.id == cp[0]) c.start += L, c.track += 3;
  }, 99);
  d.endGroup();
  expect(d.song().playlist.size() == before.playlist.size() + 1, "copy drag: a copy placed");
  d.undo();
  expect(d.song() == before, "copy drag: one undo step removes it");
  d.beginGroup("copy clips");
  d.editSong("copy clips", [&](Song& x) { pltools::cloneClips(x, {c1, c2}); });
  d.cancelGroup();
  expect(d.song() == before && !d.undo(), "copy drag: a cancelled drag leaves the song and the history as they were");
}

// GRIDLINE_Q1_SHOWCASE=<file>: a project for screenshots (looping clips with
// and without a loop start, a pattern that does not loop, a grouped pair).
void writeShowcase(const char* path) {
  Fx a;
  const Tick L = a.s.barTicks();
  Pattern* p = a.s.pattern(a.pat);
  p->name = "Arp loop";
  p->color = 0x5B8DEF;
  for (int k = 0; k < 4; ++k) {  // a busier riff
    Note n;
    n.start = k * a.s.beatTicks() + a.s.beatTicks() / 2;
    n.length = a.s.beatTicks() / 4;
    n.pitch = 72 + 2 * k;
    n.velocity = 80;
    p->notes[a.ch].push_back(n);
  }
  std::stable_sort(p->notes[a.ch].begin(), p->notes[a.ch].end(), [](const Note& x, const Note& y) { return x.start < y.start; });
  Pattern once = *p;
  once.id = a.s.allocateId();
  once.name = "One shot (loop off)";
  once.color = 0xD98A3A;
  once.loop = false;
  a.s.patterns.push_back(once);
  a.clip(0, 4 * L, 0, 0);                 // four passes
  a.clip(0, 3 * L + L / 2, L / 4, 1);     // starts mid-pattern, ends mid-pass
  a.clip(L, 2 * L, L / 2, 2);
  Clip c;
  c.id = a.s.allocateId();
  c.pattern = once.id;
  c.track = 3;
  c.start = 0;
  c.length = 3 * L;
  a.s.playlist.push_back(c);
  const ObjectId g1 = a.clip(5 * L, 2 * L, 0, 0), g2 = a.clip(5 * L, L + L / 2, 0, 1);
  for (Clip& k : a.s.playlist)
    if (k.id == g1 || k.id == g2) k.group = 9001;
  a.s.tracks.resize(std::max<size_t>(a.s.tracks.size(), 8));
  saveProject(path, a.s);
}

}  // namespace

int main() {
  if (const char* sc = std::getenv("GRIDLINE_Q1_SHOWCASE")) writeShowcase(sc);
  test::hermeticTestEnv("q1");
  testPasses();
  testPlayback();
  testAutomation();
  testEditing();
  testFiles();
  testDemo();
  testCopies();
  return finish();
}
