// Gridline - tracks from the playlist on the Stage (core/StagePick.h): the playlist selection and a
// marker area resolved to mixer tracks (audio and pattern clips through their channels' routing, buses,
// clips straddling the area, muted clips and tracks, channels into the master), the marker areas, free
// places across the front, the arrange presets, and the edits (add to the stage, move to a room or a new
// one named after a marker, arrange together): one undo step each, undone exactly.
#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <vector>
#include "Check.h"
#include "core/Document.h"
#include "core/Song.h"
#include "core/StagePick.h"
#include "core/StageRoomOps.h"

using namespace gl;
namespace sp = gl::stagepick;
namespace ro = gl::roomops;

namespace {

bool near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }
bool has(const std::vector<ObjectId>& v, ObjectId id) { return std::find(v.begin(), v.end(), id) != v.end(); }

void oneStep(Document& d, const char* what, const std::function<void()>& fn) {
  d.endGesture();
  const size_t at = d.history().position();
  const Song before = d.song();
  fn();
  d.endGesture();
  const bool one = d.history().position() == at + 1;
  const Song after = d.song();
  d.undo();
  const bool back = d.history().position() == at && d.song() == before;
  d.redo();
  expect(one && back && d.song() == after, what);
}

// A song: mixer 3 Kick (audio clips), 4 Keys (pattern notes), 5 Hats (pattern steps) -> 8 Drum bus,
// 6 an audio track's own mixer track, a channel into the master. Clips (ticks):
//   lane 0: audio Kick [0, 384), [384, 768), [768, 1152)
//   lane 1: pattern Keys+Hats [300, 500) (straddles 384), muted pattern [400, 600)
//   lane 2: automation [0, 768); lane 3: a channel into the master [400, 500)
//   lane 4 (muted): audio Hats [2000, 2100); lane 6: an audio track linked to mixer 6
struct Fix {
  Song s = makeEmptySong();
  ObjectId kick = kNoId, keys = kNoId, hats = kNoId, master = kNoId;
  ObjectId c0 = kNoId, c1 = kNoId, c2 = kNoId, pat = kNoId, patMuted = kNoId, autoClip = kNoId, toMaster = kNoId, onMutedLane = kNoId;
  ObjectId m(size_t i) const { return s.mixer[i].id; }
  ObjectId channel(const char* name, ObjectId track) {
    Channel c;
    c.id = s.allocateId();
    c.name = name;
    c.mixerTrack = track;
    s.channels.push_back(c);
    return c.id;
  }
  ObjectId clip(ClipKind k, ObjectId what, int lane, Tick start, Tick len, bool muted = false) {
    Clip c;
    c.id = s.allocateId();
    c.kind = k;
    if (k == ClipKind::Pattern) {
      c.pattern = what;
    } else {
      c.source = what;
    }
    c.track = lane;
    c.start = start;
    c.length = len;
    c.muted = muted;
    s.playlist.push_back(c);
    return c.id;
  }
  Fix() {
    kick = channel("Kick", m(3));
    keys = channel("Keys", m(4));
    hats = channel("Hats", m(5));
    master = channel("Straight", s.masterId());
    s.mixer[5].sends = {{m(8), 1.0f, false}};  // Hats -> Drum bus
    Pattern p;
    p.id = s.allocateId();
    p.notes[keys] = {Note{}};
    p.steps[hats] = {100, 0, 0, 0};
    s.patterns.push_back(p);
    pat = p.id;
    Pattern q;
    q.id = s.allocateId();
    q.notes[keys] = {Note{}};
    s.patterns.push_back(q);
    c0 = clip(ClipKind::Audio, kick, 0, 0, 384);
    c1 = clip(ClipKind::Audio, kick, 0, 384, 384);
    c2 = clip(ClipKind::Audio, kick, 0, 768, 384);
    clip(ClipKind::Pattern, pat, 1, 300, 200);
    patMuted = clip(ClipKind::Pattern, q.id, 1, 400, 200, true);
    AutomationClip ac;
    ac.id = s.allocateId();
    s.automations.push_back(ac);
    autoClip = clip(ClipKind::Automation, ac.id, 2, 0, 768);
    toMaster = clip(ClipKind::Audio, master, 3, 400, 100);
    s.tracks[4].muted = true;
    onMutedLane = clip(ClipKind::Audio, hats, 4, 2000, 100);
    s.tracks[6].mode = TrackMode::Audio;
    s.tracks[6].mixer = m(6);
  }
};

void testSelection() {
  Fix f;
  const Song& s = f.s;
  sp::Options o;
  sp::Found r = sp::fromClips(s, {f.c0}, o);
  expect(r.tracks == std::vector<ObjectId>{f.m(3)} && r.clips == 1 && r.buses.empty(), "an audio clip: its channel's mixer track");
  // The pattern clip: Keys (notes) and Hats (steps), and the bus Hats sends into.
  ObjectId patClip = kNoId;
  for (const Clip& c : s.playlist) {
    if (c.pattern == f.pat) {
      patClip = c.id;
    }
  }
  r = sp::fromClips(s, {patClip}, o);
  expect(r.tracks.size() == 3 && has(r.tracks, f.m(4)) && has(r.tracks, f.m(5)) && has(r.tracks, f.m(8)), "a pattern clip: its channels' tracks and their bus");
  expect(r.buses == std::vector<ObjectId>{f.m(8)}, "the bus is marked as one");
  o.buses = false;
  r = sp::fromClips(s, {patClip}, o);
  expect(r.tracks.size() == 2 && !has(r.tracks, f.m(8)), "without buses: only the tracks the channels play into");
  o.buses = true;
  r = sp::fromClips(s, {f.autoClip, f.toMaster}, o);
  expect(r.tracks.empty() && r.toMaster == 1 && r.clips == 0, "automation clips have no track; a channel into the master is counted, not found");
  r = sp::fromClips(s, {f.patMuted}, o);
  expect(r.tracks.empty() && r.mutedLeftOut == 1, "a muted clip is left out by default");
  o.muted = true;
  r = sp::fromClips(s, {f.patMuted}, o);
  expect(r.tracks == std::vector<ObjectId>{f.m(4)}, "a muted clip counts when asked for");
  o.muted = false;
  r = sp::fromClips(s, {f.c0, f.c1, f.c2}, o);
  expect(r.tracks.size() == 1 && r.clips == 3, "several clips on one track: the track once");
  // Playlist tracks: their clips, and the mixer track an audio track is linked to.
  r = sp::fromTracks(s, {0, 6}, o);
  expect(r.tracks.size() == 2 && has(r.tracks, f.m(3)) && has(r.tracks, f.m(6)), "playlist tracks: their clips' tracks and an audio track's own");
  r = sp::fromTracks(s, {4}, o);
  expect(r.tracks.empty() && r.mutedLeftOut == 1, "a muted playlist track's clips are left out");
  r = sp::fromClips(s, {f.c0, 999999}, o);
  expect(r.tracks.size() == 1, "a clip that is gone is skipped");
}

void testRange() {
  Fix f;
  const Song& s = f.s;
  const sp::Options o;
  // [384, 768): the second Kick clip, the pattern clip straddling 384, the muted one (left out).
  sp::Found r = sp::fromRange(s, 384, 768, o);
  expect(r.tracks.size() == 4 && has(r.tracks, f.m(3)) && has(r.tracks, f.m(4)) && has(r.tracks, f.m(5)) && has(r.tracks, f.m(8)),
         "a range: the clips inside and one straddling its start");
  expect(r.mutedLeftOut == 1 && r.toMaster == 1, "a range: the muted clip left out, the clip into the master counted");
  // [500, 768): the pattern clip ends at 500 (touches only): only Kick (and the muted clip, left out).
  r = sp::fromRange(s, 500, 768, o);
  expect(r.tracks == std::vector<ObjectId>{f.m(3)}, "a clip that only touches the range does not count");
  r = sp::fromRange(s, 1152, 1900, o);
  expect(r.tracks.empty() && r.clips == 0, "an empty part of the song: nothing");
  r = sp::fromRange(s, 768, 384, o);
  expect(r.tracks.empty(), "a backwards range: nothing");
  r = sp::fromRange(s, 1999, 2001, o);
  expect(r.tracks.empty() && r.mutedLeftOut == 1, "a clip on a muted playlist track does not play");
  sp::Options withMuted;
  withMuted.muted = true;
  r = sp::fromRange(s, 1999, 2001, withMuted);
  expect(r.tracks.size() == 2 && has(r.tracks, f.m(5)), "with muted clips: the muted track's clip and its bus");
}

void testMarkers() {
  Song s = makeEmptySong();
  expect(sp::markerAreas(s).empty() && sp::betweenMarkers(s, 0, -1).marker == -1, "no markers: no areas");
  Marker a, b, c;
  a.pos = 0, a.name = "Intro";
  b.pos = 384 * 8, b.name = "Verse";
  c.pos = 384 * 16;
  s.markers = {a, b, c};
  const std::vector<sp::Area> areas = sp::markerAreas(s);
  expect(areas.size() == 3 && areas[0].start == 0 && areas[0].end == 384 * 8 && areas[0].markerTo == 1 && areas[1].end == 384 * 16,
         "each marker to the next");
  expect(areas[2].start == 384 * 16 && areas[2].end > areas[2].start && areas[2].markerTo == -1, "the last marker: to the song end (at least a bar)");
  const sp::Area ac = sp::betweenMarkers(s, 0, 2);
  expect(ac.start == 0 && ac.end == 384 * 16 && ac.markerTo == 2, "between two markers");
  expect(sp::betweenMarkers(s, 1, 0).end == 384 * 16, "an end marker before the start: the next marker");
  expect(sp::areaName(s, areas[1]) == "Verse" && sp::areaName(s, areas[2]) == "Marker 3", "an area's name: its marker's, else Marker N");
  sp::Area sel;
  sel.start = 384 * 4, sel.end = 384 * 6;
  expect(sp::areaName(s, sel) == "Bars 5 to 6", "a time selection's name: its bars");
}

void testPlaces() {
  using P = stage::Pos;
  std::vector<P> taken = {{kNoId, 0.0, 0.0}};
  std::vector<P> to = sp::frontPlaces(taken, 2, false);
  expect(to.size() == 2 && near(to[0].pan, -0.2) && near(to[1].pan, 0.2) && near(to[0].depth, 0) && near(to[1].depth, 0),
         "free places: the front row beside what stands there, left to right");
  to = sp::frontPlaces({}, 1, true);
  expect(to.size() == 1 && !(near(to[0].pan, 0) && near(to[0].depth, 0)), "never the centre front for a track not in use");
  to = sp::frontPlaces({}, 1, false);
  expect(to.size() == 1 && near(to[0].pan, 0) && near(to[0].depth, 0), "an empty stage: the centre front first");
  to = sp::frontPlaces({}, 14, false);
  bool clear = true;
  for (size_t i = 0; i < to.size(); ++i) {
    for (size_t j = i + 1; j < to.size(); ++j) {
      clear = clear && !(near(to[i].depth, to[j].depth, 0.01) && std::fabs(to[i].pan - to[j].pan) < 0.15);
    }
  }
  expect(to.size() == 14 && clear && near(to.back().depth, 0.25), "a full front row: the next row back, never on top of each other");
  // Arrange.
  const std::vector<P> in = {{1, 0.1, 0.25}, {2, -0.5, 0.5}, {3, 0.3, 0.25}};
  std::vector<P> out = sp::arrange(in, sp::Arrange::SpreadLeftRight);
  expect(near(out[1].pan, -0.4) && near(out[0].pan, 0.0) && near(out[2].pan, 0.4) && near(out[1].depth, 0.5) && near(out[0].depth, 0.25),
         "spread left to right: even steps in the order they stood, distances kept");
  out = sp::arrange(in, sp::Arrange::LineUpFront);
  expect(near(out[0].depth, 0) && near(out[1].depth, 0) && near(out[2].depth, 0) && near(out[1].pan, -0.4), "line up at the front");
  out = sp::arrange(in, sp::Arrange::Forward);
  expect(near(out[0].depth, 0.0) && near(out[1].depth, 0.25) && near(out[0].pan, 0.1), "bring forward: one row, all together");
  out = sp::arrange(out, sp::Arrange::Forward);
  expect(near(out[0].depth, 0.0) && near(out[1].depth, 0.25), "bring forward stops at the front (the spacing kept)");
  out = sp::arrange(in, sp::Arrange::Back);
  expect(near(out[1].depth, 0.75) && near(out[0].depth, 0.5), "push back: one row");
  out = sp::arrange({{1, 0.0, 0.9}, {2, 0.0, 0.5}}, sp::Arrange::Back);
  expect(near(out[0].depth, 1.0) && near(out[1].depth, 0.6), "push back stops at the back (the spacing kept)");
  out = sp::arrange(in, sp::Arrange::Centre);
  expect(near(out[0].pan + out[1].pan + out[2].pan, 0.0) && near(out[2].pan - out[1].pan, 0.8), "gather around the centre keeps the spacing");
  out = sp::arrange({{1, -0.9, 0}, {2, -0.5, 0}}, sp::Arrange::Left);
  expect(near(out[0].pan, -1.0) && near(out[1].pan, -0.6), "move left stops at the edge (the spacing kept)");
}

void testEdits() {
  Fix f;
  Document d(f.s);
  const ObjectId kick = f.m(3), keys = f.m(4), bus = f.m(8), spare = f.m(9);
  // The bus has no channel, name or effects: not in use. Keys stands in a second room.
  int r1 = ro::addRoom(d, 0, nullptr, "Second");
  ro::moveTracksToRoom(d, {keys}, r1);
  d.setParam(mixerParam(kick, MixerParam::Pan), 0.0, false);
  d.endGesture();
  expect(ro::trackRoom(d.song(), keys) == 1 && !ro::trackInUse(d.song(), *d.song().mixerTrack(bus)), "the fixture: Keys in room 2, the bus not in use");
  int added = 0;
  oneStep(d, "add to the stage: one undo step", [&] { added = sp::addToStage(d, {kick, keys, bus, spare}, 0); });
  const Song& s = d.song();
  const MixerTrack* k = s.mixerTrack(keys);
  const MixerTrack* b = s.mixerTrack(bus);
  expect(added == 3 && ro::trackRoom(s, keys) == 0 && ro::trackRoom(s, *b) == 0, "added: the track from another room, the bus and the spare track");
  expect(ro::trackInUse(s, *b) && ro::trackInUse(s, *s.mixerTrack(spare)), "the added tracks are in use now (they have a place)");
  expect(near(s.mixerTrack(kick)->pan, 0) && near(k->depth, 0) && std::fabs(k->pan) > 0.1 && std::fabs(b->pan) > 0.1 && std::fabs(k->pan - b->pan) > 0.15,
         "they stand across the front, clear of the track already there and of each other");
  expect(sp::addToStage(d, {kick, keys}, 0) == 0, "tracks already on the stage are not added again");
  // Move to a new room named after a marker: one step (the room and the move).
  int to = -1;
  const int rooms = ro::roomCount(d.song());
  oneStep(d, "move to a new room: one undo step", [&] { sp::moveToRoom(d, {kick, keys}, -1, "Verse", 0, &to); });
  expect(ro::roomCount(d.song()) == rooms + 1 && to == rooms && ro::roomName(d.song(), to) == "Verse" && ro::trackRoom(d.song(), kick) == to &&
             ro::trackRoom(d.song(), keys) == to,
         "the new room is named after the marker and holds the tracks");
  d.undo();
  expect(ro::roomCount(d.song()) == rooms && ro::trackRoom(d.song(), kick) == 0, "undo: the room is gone, the tracks back");
  d.redo();
  oneStep(d, "move to an existing room: one undo step", [&] { sp::moveToRoom(d, {kick}, 1, "", 0, &to); });
  expect(to == 1 && ro::trackRoom(d.song(), kick) == 1, "moved to the room asked for");
  // Arrange together.
  d.setParam(mixerParam(keys, MixerParam::Depth), 0.5, false);
  d.setParam(mixerParam(bus, MixerParam::Depth), 0.25, false);
  d.endGesture();
  const double gap = d.song().mixerTrack(keys)->depth - d.song().mixerTrack(bus)->depth;
  oneStep(d, "bring forward: one undo step", [&] { sp::arrangeTracks(d, {keys, bus}, sp::Arrange::Forward); });
  expect(near(d.song().mixerTrack(bus)->depth, 0) && near(d.song().mixerTrack(keys)->depth - d.song().mixerTrack(bus)->depth, gap),
         "bring forward keeps their spacing");
  d.editMixerTrack(bus, "lock", [](MixerTrack& m) { m.locked = true; });
  d.endGesture();
  const double busPan = d.song().mixerTrack(bus)->pan;
  sp::arrangeTracks(d, {keys, bus}, sp::Arrange::Right);
  expect(near(d.song().mixerTrack(bus)->pan, busPan), "a locked track stays where it is");
  expect(sp::arrangeTracks(d, {}, sp::Arrange::Back) == 0, "no tracks: nothing to do");
}

}  // namespace

int main() {
  testSelection();
  testRange();
  testMarkers();
  testPlaces();
  testEdits();
  return finish();
}
