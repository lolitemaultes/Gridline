// Gridline — J-173: automation clips inside patterns (core side).
//  * model: one clip per target, names (own or the target's), new clips, removal;
//    conversions between playlist clips and pattern clips keep the curve;
//  * playback: song mode (offset, trimmed, looping and split clips) and pattern
//    mode give the clip's value (range applied) at the exact tick; a pattern clip
//    sounds sample for sample like the same curve as a playlist clip; a looping
//    clip repeats inside the pattern;
//  * documents: one undo step per edit (merged gestures), clone and make unique
//    copy the clips, moving a playlist clip into a pattern (and back);
//  * files: project save / load keeps every clip field; a plain lane writes the
//    pre-J-173 JSON and loads as before (unnamed, 0..1, the earlier curves);
//  * FL export / import and MIDI export (CC 7) play the range-mapped values;
//  * collaboration: a clip added and renamed on one side arrives on the other.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "Check.h"
#include "collab/CollabSim.h"
#include "core/Automation.h"
#include "core/Document.h"
#include "core/MenuOps.h"
#include "core/MenuParity.h"
#include "core/MidiFile.h"
#include "core/PatternAutoOps.h"
#include "core/PatternAutomation.h"
#include "core/PluginInfo.h"
#include "core/Project.h"
#include "core/flp/Flp.h"
#include "core/flp/FlpExport.h"
#include "engine/ClipOps.h"
#include "engine/Render.h"

using namespace gl;

namespace {

// One synth channel, a 1-bar pattern of four notes, no clips.
struct Fx {
  Song s;
  ObjectId ch = kNoId, pat = kNoId;
  Fx() {
    s = makeEmptySong();
    s.tempo = 132;
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
    for (int k = 0; k < 4; ++k) p.notes[ch].push_back(Note(k * b, b - b / 8, 57 + 5 * k, 100));
    s.patterns.push_back(p);
    pat = p.id;
  }
  Pattern& P() { return *s.pattern(pat); }
  ParamId vol() const { return channelParam(ch, ChannelParam::Volume); }
  ParamId pan() const { return channelParam(ch, ChannelParam::Pan); }
  // A named volume clip over the bar: a curve with tension, a hold and a ramp, range 0.2..0.9.
  PatternAutomation volClip() const {
    PatternAutomation c = newPatternClip(s, vol(), 0.5f, s.barTicks());
    c.name = "Swell";
    c.color = 0x33AA66;
    c.min = 0.2f, c.max = 0.9f;
    const Tick b = s.beatTicks();
    c.points = {{0, 0.1f, 0.4f, AutoShape::Curve}, {b, 0.9f, 0, AutoShape::Hold}, {2 * b, 0.3f, -0.5f, AutoShape::Curve}, {4 * b, 1.0f, 0, AutoShape::Curve}};
    return c;
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

// The clip's parameter value at content tick t (what playback must give).
double expectAt(const PatternAutomation& c, double t) { return std::clamp(automationToParam(c, automationValue(c, t)), 0.0, 1.0); }

const AutomationLane* laneFor(const std::vector<AutomationLane>& v, ParamId p) {
  for (const auto& l : v)
    if (l.targets(p.owner, p.index)) return &l;
  return nullptr;
}

void testModel() {
  Fx f;
  PatternAutomation c = f.volClip();
  expect(c.flCurves && c.id == kNoId && c.name == "Swell", "a new clip: named, today's curves, no playlist id");
  c.id = 77;
  expect(addPatternClip(f.P(), c) && f.P().automation.size() == 1 && f.P().automation[0].id == kNoId, "added (its id cleared)");
  expect(!addPatternClip(f.P(), f.volClip()), "one clip per target: a second one is refused");
  PatternAutomation p = patternLane(f.pan().owner, f.pan().index, {{0, 0.5f, 0, AutoShape::Hold}});
  expect(p.name.empty() && !p.flCurves && p.min == 0.0f && p.max == 1.0f, "a plain lane: unnamed, 0..1, the earlier curves");
  addPatternClip(f.P(), p);
  expect(patternAutoName(f.s, f.P().automation[0]) == "Swell" && patternAutoName(f.s, f.P().automation[1]) == paramDisplayName(f.s, f.pan()),
         "names: its own, else the target's");
  expect(removePatternClip(f.P(), f.pan().owner, f.pan().index) && !findPatternLane(f.P(), f.pan().owner, f.pan().index) &&
             !removePatternClip(f.P(), f.pan().owner, f.pan().index),
         "removal");
  // Playlist clip <-> pattern clip keep the curve (and name, colour, range, law).
  const PatternAutomation& in = f.P().automation[0];
  AutomationClip pl = playlistClipFromPattern(f.s, in);
  expect(pl.name == "Swell" && pl.color == in.color && pl.min == in.min && pl.max == in.max && pl.points == in.points && pl.flCurves,
         "as a playlist clip: everything kept");
  PatternAutomation back = patternClipFromPlaylist(f.s, pl, nullptr, 0);
  expect(back.points == in.points && back.min == in.min && back.name == in.name, "and back");
  // A placement's window: offset half a beat, a beat and a half long, moved to tick 96.
  Clip pc;
  pc.kind = ClipKind::Automation;
  pc.start = 0, pc.offset = f.s.beatTicks() / 2, pc.length = f.s.beatTicks() * 3 / 2;
  PatternAutomation win = patternClipFromPlaylist(f.s, pl, &pc, 96);
  bool same = true;
  for (Tick t = 0; t < pc.length; t += 7) same = same && std::fabs(automationValue(win, static_cast<double>(96 + t)) - automationValue(pl, static_cast<double>(pc.offset + t))) < 1e-6;
  expect(same, "a placement's window as a pattern clip: the same values, moved");
  // Exporters' points: the range applied, a loop unrolled.
  PatternAutomation lp = in;
  lp.loop = f.s.beatTicks() * 2;
  const auto pts = patternClipParamPoints(lp, f.s.barTicks());
  bool mapped = !pts.empty();
  for (const auto& q : pts) mapped = mapped && q.value >= 0.2f - 1e-6f && q.value <= 0.9f + 1e-6f;
  expect(mapped && pts.back().pos >= f.s.beatTicks() * 2, "parameter points: range applied, the loop unrolled");
  expect(patternClipEnd(in) == f.s.barTicks(), "content end: the last point");
}

void testPlayback() {
  const auto check = [](Fx& f, const char* what) {
    const auto lanes = patternAutomationLanes(f.s);
    const AutomationLane* l = laneFor(lanes, f.vol());
    const PatternAutomation& c = *findPatternLane(f.P(), f.vol().owner, f.vol().index);
    const Tick L = f.s.patternContentLength(f.P());
    double worst = l ? 0 : 1;
    for (const auto& k : f.s.playlist)
      if (l && k.kind == ClipKind::Pattern)
        for (Tick t = 0; t < f.s.clipLength(k); t += 5) {
          const double content = static_cast<double>((k.offset + t) % L);
          worst = std::max(worst, std::fabs(l->valueAt(static_cast<double>(k.start + t)) - expectAt(c, content)));
        }
    std::printf("# %s: largest difference %g\n", what, worst);
    expect(worst < 1e-9, what);
  };
  {
    Fx f;
    addPatternClip(f.P(), f.volClip());
    const Tick L = f.s.barTicks();
    f.clip(0, L, 0);
    f.clip(2 * L, 2 * L + L / 3, L / 4);  // trimmed start, looping, a partial last pass
    f.clip(6 * L, L / 2, 0, 1);           // trimmed end
    check(f, "song mode: the clip's value (range applied) at every tick of offset, trimmed and looping clips");
    // split: the right part plays on from where the left stopped
    Fx g;
    addPatternClip(g.P(), g.volClip());
    const ObjectId id = g.clip(0, 2 * L, 0);
    splitClipAt(g.s, id, L + L / 3);
    check(g, "song mode: a split clip's parts");
    // pattern mode / live: one pass, an offset, the loop
    const auto single = singlePatternLanes(g.P(), 2 * L, L / 4, L);
    const AutomationLane* sl = laneFor(single, g.vol());
    double worst = sl ? 0 : 1;
    for (Tick t = 0; sl && t < 2 * L; t += 3) worst = std::max(worst, std::fabs(sl->valueAt(static_cast<double>(t)) - expectAt(*findPatternLane(g.P(), g.vol().owner, g.vol().index), static_cast<double>((L / 4 + t) % L))));
    expect(worst < 1e-9, "pattern mode: the clip's value at every tick (an offset, the loop)");
  }
  {  // a clip that loops inside the pattern (a beat), the pattern two bars
    Fx f;
    f.P().bars = 2;
    PatternAutomation c = f.volClip();
    c.loop = f.s.beatTicks();
    addPatternClip(f.P(), c);
    f.clip(0, 2 * f.s.barTicks(), 0);
    const auto lanes = patternAutomationLanes(f.s);
    const AutomationLane* l = laneFor(lanes, f.vol());
    bool rep = l != nullptr;
    for (Tick t = 0; l && t < f.s.beatTicks(); t += 11)
      rep = rep && std::fabs(l->valueAt(static_cast<double>(t)) - l->valueAt(static_cast<double>(t + 5 * f.s.beatTicks()))) < 1e-12;
    expect(rep, "a looping clip repeats inside the pattern");
  }
  {  // sample for sample: the pattern clip sounds like the same curve as a playlist clip
    Fx a;
    addPatternClip(a.P(), a.volClip());
    const Tick L = a.s.barTicks();
    a.clip(L, 2 * L + L / 2, L / 4);
    Fx b;
    const ObjectId pc = b.clip(L, 2 * L + L / 2, L / 4);
    (void)pc;
    AutomationClip ac = playlistClipFromPattern(a.s, a.P().automation[0]);
    ac.id = b.s.allocateId();
    ac.loop = L;
    b.s.automations.push_back(ac);
    Clip k;
    k.id = b.s.allocateId();
    k.kind = ClipKind::Automation;
    k.source = ac.id;
    k.track = 2;
    k.start = L, k.length = 2 * L + L / 2, k.offset = L / 4;
    b.s.playlist.push_back(k);
    const AudioBuffer ra = render(a.s), rb = render(b.s);
    size_t at = 0;
    bool same = ra.frames == rb.frames && ra.frames > 0;
    for (size_t i = 0; same && i < ra.frames; ++i) same = ra.at(0, i) == rb.at(0, i) && ra.at(1, i) == rb.at(1, i), at = i;
    if (!same) std::printf("# first difference at frame %zu of %zu\n", at, ra.frames);
    double pk = 0;
    for (size_t i = 0; i < ra.frames; ++i) pk = std::max(pk, std::fabs(static_cast<double>(ra.at(0, i))));
    expect(same && pk > 0.01, "render: the pattern clip sounds sample for sample like the same curve as a playlist clip");
  }
}

void testDocument() {
  Fx f;
  Document d;
  d.reset(f.s);
  const ObjectId pat = f.pat;
  const size_t u0 = d.history().position();
  d.editPattern(pat, "add", [&](Pattern& p) { addPatternClip(p, f.volClip()); });
  expect(d.history().position() == u0 + 1 && d.song().pattern(pat)->automation.size() == 1, "adding a clip: one undo step");
  // a drag: many edits with one merge key, one step
  for (int i = 0; i < 12; ++i)
    d.editPattern(pat, "move point", [&](Pattern& p) { findPatternLane(p, f.vol().owner, f.vol().index)->points[1].value = 0.05f * static_cast<float>(i); }, 0x173A);
  expect(d.history().position() == u0 + 2, "a point dragged: one undo step");
  d.editPattern(pat, "rename", [&](Pattern& p) { findPatternLane(p, f.vol().owner, f.vol().index)->name = "Lift"; });
  d.undo();
  expect(findPatternLane(*d.song().pattern(pat), f.vol().owner, f.vol().index)->name == "Swell", "rename: undo");
  d.undo();
  expect(std::fabs(findPatternLane(*d.song().pattern(pat), f.vol().owner, f.vol().index)->points[1].value - 0.9f) < 1e-6f, "drag: undone in one step");
  d.redo();
  // clone and make unique copy the clips
  const ObjectId cl = d.clonePattern(pat);
  expect(cl != kNoId && d.song().pattern(cl)->automation == d.song().pattern(pat)->automation, "clone: the clips come along");
  const ObjectId k = d.addClip(pat, 0, 0);
  const ObjectId k2 = d.addClip(pat, 0, d.song().barTicks());
  (void)k;
  const ObjectId uq = ops::makeClipUnique(d, k2);
  expect(uq != kNoId && uq != pat && d.song().pattern(uq)->automation == d.song().pattern(pat)->automation, "make unique: the clips come along");
  d.editPattern(uq, "edit unique", [&](Pattern& p) { findPatternLane(p, f.vol().owner, f.vol().index)->points[0].value = 0.77f; });
  expect(findPatternLane(*d.song().pattern(pat), f.vol().owner, f.vol().index)->points[0].value != 0.77f, "... and are its own");
  // turned into a playlist clip and back into the pattern
  const ObjectId ac = ops::laneToAutomationClip(d, pat, f.vol().owner, f.vol().index);
  expect(ac != kNoId && !findPatternLane(*d.song().pattern(pat), f.vol().owner, f.vol().index), "moved to the playlist");
  ObjectId placement = kNoId;
  for (const auto& c : d.song().playlist)
    if (c.kind == ClipKind::Automation && c.source == ac && placement == kNoId) placement = c.id;
  const size_t before = d.history().position();
  const patauto::MoveResult r = patauto::placementIntoPattern(d, placement, pat, false);
  expect(r == patauto::MoveResult::Done && d.history().position() == before + 1, "a playlist clip moved into the pattern: one undo step");
  const PatternAutomation* again = findPatternLane(*d.song().pattern(pat), f.vol().owner, f.vol().index);
  expect(again && again->name == "Swell" && again->min == 0.2f && again->max == 0.9f, "... name and range kept");
  expect(patauto::placementIntoPattern(d, placement, pat, false) == patauto::MoveResult::NoSource, "... the placement is gone");
  expect(patauto::automationIntoPattern(d, ac, pat, 0) != patauto::MoveResult::Done, "a target the pattern already has is refused");
  d.undo();
  expect(!findPatternLane(*d.song().pattern(pat), f.vol().owner, f.vol().index) && d.song().clip(placement), "undo: back in the playlist");
}

void testFiles() {
  Fx f;
  PatternAutomation c = f.volClip();
  c.loop = f.s.beatTicks() * 3;
  addPatternClip(f.P(), c);
  addPatternClip(f.P(), patternLane(f.pan().owner, f.pan().index, {{0, 0.25f, 0, AutoShape::Hold}, {96, 0.75f, 0, AutoShape::Hold}}));
  const Json j = songToJson(f.s);
  const std::string text = jsonStringify(j, 0);
  Song back;
  std::string err;
  expect(songFromJson(j, back, &err), "project: saved and loaded");
  const Pattern* bp = back.pattern(f.pat);
  expect(bp && bp->automation == f.P().automation, "project: every clip field kept (name, colour, range, loop, curves)");
  // A plain lane writes what projects before J-173 held (only owner, index, points).
  const Json* pa = nullptr;
  for (const auto& jp : j.find("patterns")->array)
    if (jp.num("id", 0) == static_cast<double>(f.pat)) pa = jp.find("automation");
  bool plain = pa && pa->array.size() == 2;
  if (plain) plain = pa->array[1].object.size() == 3 && pa->array[0].find("name") && pa->array[0].find("flCurves");
  expect(plain, "project: a plain lane is written as before (old Gridline versions read it)");
  // An old project's lane: no clip fields -> unnamed, 0..1, the earlier curves; it plays as before.
  Json old = j;
  for (auto& jp : old.object["patterns"].array)
    if (auto it = jp.object.find("automation"); it != jp.object.end())
      for (auto& jl : it->second.array)
        for (const char* k : {"name", "color", "min", "max", "loop", "flCurves"}) jl.object.erase(k);
  Song os;
  expect(songFromJson(old, os, &err), "an old project loads");
  const PatternAutomation* ol = findPatternLane(*os.pattern(f.pat), f.vol().owner, f.vol().index);
  expect(ol && ol->name.empty() && !ol->flCurves && ol->min == 0.0f && ol->max == 1.0f && ol->loop == 0, "an old lane: unnamed, 0..1, the earlier curves");
  (void)text;
}

void testFlp() {
  Fx f;
  // Hold steps (FL's pattern events step): the round trip is exact up to the value grid.
  PatternAutomation c = newPatternClip(f.s, f.vol(), 0.5f, f.s.barTicks());
  c.name = "Steps";
  c.min = 0.25f, c.max = 0.75f;
  c.points = {{0, 0.0f, 0, AutoShape::Hold}, {96, 1.0f, 0, AutoShape::Hold}, {192, 0.5f, 0, AutoShape::Hold}, {288, 0.2f, 0, AutoShape::Hold}};
  addPatternClip(f.P(), c);
  f.clip(0, f.s.barTicks(), 0);
  flp::ExportOptions o;
  o.useOrigin = false;
  flp::Bytes bytes;
  flp::ExportReport rep;
  Song back;
  flp::Report r;
  std::string err;
  const bool ok = flp::exportFlp(f.s, bytes, rep, o) && importFlpBytes(bytes, back, r, &err, "", "", false);
  expect(ok, "FL export / import");
  const PatternAutomation* bl = nullptr;
  for (const auto& p : back.patterns)
    for (const auto& l : p.automation)
      if (l.index == static_cast<uint32_t>(ChannelParam::Volume)) bl = &l;
  double worst = bl ? 0 : 1;
  for (Tick t = 0; bl && t < f.s.barTicks(); t += 8)
    worst = std::max(worst, std::fabs(expectAt(*bl, static_cast<double>(t * back.ppq / f.s.ppq)) - expectAt(c, static_cast<double>(t))));
  std::printf("# FL round trip: largest volume difference %g\n", worst);
  expect(worst < 2e-3, "FL round trip: the volume plays the range-mapped values");
  // A curve exports as held steps close to it.
  Fx g;
  PatternAutomation src = g.volClip();
  src.max = 0.75f;  // (FL's pattern events stop at its own volume ceiling)
  addPatternClip(g.P(), src);
  g.clip(0, g.s.barTicks(), 0);
  Song back2;
  const bool ok2 = flp::exportFlp(g.s, bytes, rep, o) && importFlpBytes(bytes, back2, r, &err, "", "", false);
  const PatternAutomation* cl = nullptr;
  for (const auto& p : back2.patterns)
    for (const auto& l : p.automation)
      if (l.index == static_cast<uint32_t>(ChannelParam::Volume)) cl = &l;
  double w2 = cl ? 0 : 1;
  for (Tick t = 0; cl && t < g.s.barTicks(); t += g.s.ppq / 8)
    w2 = std::max(w2, std::fabs(expectAt(*cl, static_cast<double>(t * back2.ppq / g.s.ppq)) - expectAt(src, static_cast<double>(t))));
  std::printf("# FL round trip of a curve: largest difference on the step grid %g\n", w2);
  if (w2 >= 2e-3 && cl) {
    std::printf("# ppq %d -> %d, %zu points back:", g.s.ppq, back2.ppq, cl->points.size());
    for (const auto& q : cl->points) std::printf(" %lld:%.4f/%d", static_cast<long long>(q.pos), static_cast<double>(q.value), static_cast<int>(q.shape));
    std::printf("\n# expected:");
    for (Tick t = 0; t < g.s.barTicks(); t += g.s.ppq / 8) std::printf(" %lld:%.4f", static_cast<long long>(t), expectAt(src, static_cast<double>(t)));
    std::printf("\n");
  }
  expect(ok2 && w2 < 2e-3, "FL round trip: a curve comes back as steps on its values");
}

void testMidi() {
  Fx f;
  PatternAutomation c = newPatternClip(f.s, f.vol(), 0.5f, f.s.barTicks());
  c.min = 0.5f, c.max = 0.75f;
  c.points = {{0, 0.0f, 0, AutoShape::Hold}, {192, 1.0f, 0, AutoShape::Hold}};
  addPatternClip(f.P(), c);
  const MidiFileData m = patternToMidi(f.s, f.pat);
  std::vector<int> cc7;
  for (const auto& t : m.tracks)
    for (const auto& k : t.ctls)
      if (k.controller == 7) cc7.push_back(k.value);
  const auto desc = describeParam(f.s, f.vol());
  const int lo = desc ? static_cast<int>(std::lround(std::clamp(desc->fromNormalized(0.5), 0.0, 1.0) * 127.0)) : -1;
  const int hi = desc ? static_cast<int>(std::lround(std::clamp(desc->fromNormalized(0.75), 0.0, 1.0) * 127.0)) : -1;
  std::printf("# MIDI CC 7: %zu events (%d, %d expected)\n", cc7.size(), lo, hi);
  expect(cc7.size() == 2 && cc7[0] == lo && cc7[1] == hi, "MIDI export: CC 7 carries the range-mapped values");
}

void testCollab() {
  using namespace gl::collab;
  using namespace sim;
  Clock clock;
  Net net(173);
  Peer a, b;
  a.label = "Ann", b.label = "Ben";
  for (Peer* p : {&a, &b}) p->id = crypto::Identity::generate();
  Fx f;
  a.doc.reset(f.s);
  a.folder = tmpDir("j173_a");
  b.folder = tmpDir("j173_b");
  std::string err;
  a.s = Session::create(a.doc, config(a.folder, a.id, a.label, &clock), &err);
  expect(a.s != nullptr, "collab: a session");
  if (!a.s) return;
  net.add(&a);
  b.s = Session::join(b.doc, config(b.folder, b.id, b.label, &clock), ticketOf(*a.s), &err);
  a.s->admitMember(memberOf(b));
  net.add(&b);
  net.connect(&a, &b);
  settle(net, {&a, &b}, clock);
  a.doc.editPattern(f.pat, "add", [&](Pattern& p) { addPatternClip(p, f.volClip()); });
  settle(net, {&a, &b}, clock);
  const Pattern* bp = b.doc.song().pattern(f.pat);
  expect(bp && bp->automation == a.doc.song().pattern(f.pat)->automation, "collab: a clip added on one side arrives on the other (every field)");
  b.doc.editPattern(f.pat, "rename", [&](Pattern& p) { findPatternLane(p, f.vol().owner, f.vol().index)->name = "From Ben"; });
  settle(net, {&a, &b}, clock);
  const PatternAutomation* al = findPatternLane(*a.doc.song().pattern(f.pat), f.vol().owner, f.vol().index);
  expect(al && al->name == "From Ben" && songText(a) == songText(b), "collab: renamed on the other side, the songs converge");
}

}  // namespace

int main() {
  testModel();
  testPlayback();
  testDocument();
  testFiles();
  testFlp();
  testMidi();
  testCollab();
  return finish();
}
