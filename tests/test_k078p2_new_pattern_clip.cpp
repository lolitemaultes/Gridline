// Gridline - Clips' and Smooth's track menu "Copy to a new pattern" places the new pattern's clip
// (ops::channelToNewPatternClip):
//  * replace (the core option; the menus no longer use it): a new pattern after the clip's pattern holding just the
//    channel's notes; the clip's place (track, start, length, offset, loop, launch settings, playlist
//    order) now plays the new pattern; the original pattern keeps its notes; the other clips of that
//    pattern stay; one undo step, undone exactly;
//  * lie on it (the track menus in Smooth, user 2026-11-21): the original clip stays, the new one lies on it
//    with the same place; in Clips a clip of the clip grid gets the slot under it (ownSlot);
//  * not a pattern clip: nothing;
//  * the edit reaches a collaborator, and one undo takes it back for both.
#include <string>
#include <vector>
#include "Check.h"
#include "collab/CollabSim.h"
#include "core/Document.h"
#include "core/NoteTools.h"
#include "core/Project.h"

using namespace gl;
using namespace gl::collab;
using namespace sim;

namespace {

struct Rig {
  ObjectId lead = kNoId, bass = kNoId, pat = kNoId, clipA = kNoId, clipB = kNoId;
  void build(Document& d) {
    d.reset(makeEmptySong());
    lead = d.addChannel("Lead", "gl.synth");
    bass = d.addChannel("Bass", "gl.synth");
    pat = d.song().patterns[0].id;
    d.editPattern(pat, "notes", [&](Pattern& p) {
      for (int i = 0; i < 4; ++i) {
        Note n;
        n.start = i * 96, n.length = 96, n.pitch = 60 + i;
        p.notes[lead].push_back(n);
        n.pitch = 36 + i;
        p.notes[bass].push_back(n);
      }
    });
    clipA = d.addClip(pat, 2, 768, 192);
    clipB = d.addClip(pat, 2, 1536);
    d.editClips("settings", [&](std::vector<Clip>& cs) {
      for (Clip& c : cs)
        if (c.id == clipA) {
          c.offset = 48, c.loop = true, c.name = "Verse";
          c.launchSet.mode = 2, c.launchSet.follow.on = true, c.launchSet.follow.loops = 3;
        }
    });
  }
};

size_t notesIn(const Song& s, ObjectId pat, ObjectId chan) {
  const Pattern* p = s.pattern(pat);
  if (!p) return 0;
  auto it = p->notes.find(chan);
  return it == p->notes.end() ? 0 : it->second.size();
}

std::string songJson(Song s) {
  s.nextId = 0;  // the id counter may stay on after an undo
  return jsonStringify(songToJson(s), 0);
}

bool samePlace(const Clip& a, const Clip& b) {
  return a.track == b.track && a.start == b.start && a.length == b.length && a.offset == b.offset && a.loop == b.loop && a.name == b.name &&
         a.launchSet == b.launchSet;
}

void testReplace() {
  Document d;
  Rig r;
  r.build(d);
  const Song s0 = d.song();
  const Clip orig = *s0.clip(r.clipA);
  const size_t undo0 = d.history().position();
  ObjectId placed = kNoId;
  const ObjectId np = ops::channelToNewPatternClip(d, r.clipA, r.lead, true, &placed);
  const Song& s = d.song();
  expect(np != kNoId && placed != kNoId, "a new pattern and its clip");
  expect(s.patterns.size() == s0.patterns.size() + 1 && s.patternIndex(np) == s0.patternIndex(r.pat) + 1, "the pattern comes after the original");
  expect(notesIn(s, np, r.lead) == 4 && notesIn(s, np, r.bass) == 0, "it holds just the channel's notes");
  expect(notesIn(s, r.pat, r.lead) == 4 && notesIn(s, r.pat, r.bass) == 4, "the original pattern keeps its notes");
  expect(!s.clip(r.clipA), "the original clip gives way");
  const Clip* k = s.clip(placed);
  expect(k && k->pattern == np && samePlace(*k, orig), "the new clip has the original's place and settings");
  expect(s.playlist.size() == s0.playlist.size(), "no clip more");
  size_t at = 0;
  for (size_t i = 0; i < s0.playlist.size(); ++i)
    if (s0.playlist[i].id == r.clipA) at = i;
  expect(s.playlist[at].id == placed, "in the original's place in the playlist order");
  expect(s.clip(r.clipB) && s.clip(r.clipB)->pattern == r.pat, "the pattern's other clip stays as it was");
  expect(d.history().position() == undo0 + 1, "one undo step");
  d.undo();
  expect(songJson(d.song()) == songJson(s0), "undo puts everything back");
  d.redo();
  expect(d.song().clip(placed) && d.song().clip(placed)->pattern == np, "redo places it again");
}

void testOverlap() {
  Document d;
  Rig r;
  r.build(d);
  const Song s0 = d.song();
  ObjectId placed = kNoId;
  const ObjectId np = ops::channelToNewPatternClip(d, r.clipA, r.bass, false, &placed);
  const Song& s = d.song();
  const Clip* k = s.clip(placed);
  expect(np != kNoId && k && k->pattern == np && samePlace(*k, *s0.clip(r.clipA)), "the new clip lies at the original's place");
  expect(s.clip(r.clipA) && s.clip(r.clipA)->pattern == r.pat, "the original clip stays");
  expect(s.playlist.size() == s0.playlist.size() + 1 && s.playlist.back().id == placed, "on top of it (the later clip)");
  expect(notesIn(s, np, r.bass) == 4 && notesIn(s, np, r.lead) == 0, "with just the bass notes");
}

void testNotAPattern() {
  Document d;
  Rig r;
  r.build(d);
  const size_t undo0 = d.history().position();
  expect(ops::channelToNewPatternClip(d, 999999, r.lead, true) == kNoId, "no clip: nothing");
  expect(d.history().position() == undo0, "and no undo step");
}

void testCollab() {
  Clock clock;
  Net net(7852);
  Peer a, b;
  a.label = "Alex", b.label = "Sam";
  a.id = crypto::Identity::generate(), b.id = crypto::Identity::generate();
  Rig r;
  r.build(a.doc);
  a.folder = tmpDir("k078p2_np_a"), b.folder = tmpDir("k078p2_np_b");
  std::string err;
  a.s = Session::create(a.doc, config(a.folder, a.id, a.label, &clock), &err);
  net.add(&a);
  b.s = Session::join(b.doc, config(b.folder, b.id, b.label, &clock), ticketOf(*a.s), &err);
  a.s->admitMember(memberOf(b));
  net.add(&b);
  net.connect(&a, &b);
  settle(net, {&a, &b}, clock);
  const size_t pats = b.doc.song().patterns.size();
  ObjectId placed = kNoId;
  const ObjectId np = ops::channelToNewPatternClip(a.doc, r.clipA, r.lead, true, &placed);
  settle(net, {&a, &b}, clock);
  const Clip* k = b.doc.song().clip(placed);
  expect(np != kNoId && b.doc.song().patterns.size() == pats + 1 && k && k->pattern == np && !b.doc.song().clip(r.clipA),
         "the collaborator gets the new pattern and its clip in the original's place");
  expect(songText(a) == songText(b), "both have the same song");
  a.doc.undo();
  settle(net, {&a, &b}, clock);
  expect(b.doc.song().patterns.size() == pats && b.doc.song().clip(r.clipA) && songText(a) == songText(b), "one undo takes it back for both");
}

// User 2026-11-21 (Clips): the original stays; a clip of the clip grid gets the slot under it (an
// empty scene put in when that slot is taken or past the last scene); an arrangement clip lies on it.
void testOwnSlot() {
  const Tick st = 768;
  auto rig = [&](Document& d, Rig& r) {
    r.build(d);  // clipA scene 1 (768), clipB scene 2 (1536), both on track 2
    d.editSong("live area", [&](Song& s) {
      s.live.sceneLen = st;
      s.live.areaEnd = 4 * st;
    });
  };
  {  // the slot under it is taken: a scene goes in
    Document d;
    Rig r;
    rig(d, r);
    const Song s0 = d.song();
    const size_t undo0 = d.history().position();
    ObjectId placed = kNoId;
    const ObjectId np = ops::channelToNewPatternClip(d, r.clipA, r.lead, false, &placed, true);
    const Song& s = d.song();
    const Clip* k = s.clip(placed);
    const Clip* o = s.clip(r.clipA);
    expect(np != kNoId && k && k->pattern == np, "Clips: a new pattern and its clip");
    expect(o && o->pattern == r.pat && o->start == 768 && o->track == 2, "Clips: the original clip stays where it was");
    expect(k && k->track == 2 && k->start == 768 + st && k->length == o->length && k->offset == o->offset, "Clips: the new clip in the slot under it");
    expect(s.clip(r.clipB) && s.clip(r.clipB)->start == 1536 + st, "Clips: the slot's clip moves down with the scene put in");
    expect(s.live.areaEnd == 5 * st && s.playlist.size() == s0.playlist.size() + 1, "Clips: one scene more, one clip more");
    expect(d.history().position() == undo0 + 1, "Clips: one undo step");
    d.undo();
    expect(songJson(d.song()) == songJson(s0), "Clips: undo puts everything back");
  }
  {  // the slot under it is free
    Document d;
    Rig r;
    rig(d, r);
    ObjectId placed = kNoId;
    ops::channelToNewPatternClip(d, r.clipB, r.lead, false, &placed, true);
    const Song& s = d.song();
    const Clip* k = s.clip(placed);
    expect(s.clip(r.clipB) && s.clip(r.clipB)->start == 1536 && k && k->start == 1536 + st && s.live.areaEnd == 4 * st,
           "Clips: a free slot under it takes the new clip, no scene added");
  }
  {  // the last scene: the area grows by a scene
    Document d;
    Rig r;
    rig(d, r);
    const ObjectId last = d.addClip(r.pat, 3, 3 * st + 10, 192);
    ObjectId placed = kNoId;
    ops::channelToNewPatternClip(d, last, r.lead, false, &placed, true);
    const Song& s = d.song();
    const Clip* k = s.clip(placed);
    expect(s.clip(last) && k && k->start == 4 * st + 10 && k->track == 3 && s.live.areaEnd == 5 * st,
           "Clips: under the last scene a scene goes in for it");
  }
  {  // an arrangement clip (after the clip grid) lies on the original
    Document d;
    Rig r;
    rig(d, r);
    const ObjectId arr = d.addClip(r.pat, 2, 6 * st, 192);
    ObjectId placed = kNoId;
    ops::channelToNewPatternClip(d, arr, r.lead, false, &placed, true);
    const Song& s = d.song();
    const Clip* k = s.clip(placed);
    expect(s.clip(arr) && k && k->start == 6 * st && k->track == 2 && s.live.areaEnd == 4 * st, "Clips: an arrangement clip: the new one lies on it");
  }
}

}  // namespace

int main() {
  testReplace();
  testOverlap();
  testOwnSlot();
  testNotAPattern();
  testCollab();
  return finish();
}
