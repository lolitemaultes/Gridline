// Gridline - K-112B (checkpoint 52, ahead of the cross-layout audit K-314): the project data only the
// Smooth arrange writes. For each field:
//   1. additive: a song that never used it saves without its key (older files and the other layouts'
//      saves stay byte for byte as before);
//   2. it survives a save and a load, and a second save is byte-identical to the first;
//   3. it travels to a collaborating peer (and back), and both peers' songs converge.
// The fields: the cycle area ("cycle"), the Project End marker ("projectEnd"), a track's automation
// lane ("lane"), automation mode ("autoMode") and kept sound ("sound"), a region's own name ("name")
// and its playback settings (playTranspose, playVelocity, playDelay, playQuantize).
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>
#include "Check.h"
#include "collab/CollabSim.h"
#include "core/Project.h"

using namespace gl;
using namespace sim;

namespace {

const char* const kSongKeys[] = {"cycle", "projectEnd"};
const char* const kTrackKeys[] = {"lane", "autoMode", "sound"};
const char* const kClipKeys[] = {"name", "playTranspose", "playVelocity", "playDelay", "playQuantize"};

bool anyKey(const Json& j, const char* key, const char* list = nullptr) {
  if (!list) {
    return j.find(key) != nullptr;
  }
  const Json* a = j.find(list);
  if (!a || !a->is(Json::Type::Array)) {
    return false;
  }
  for (const Json& e : a->array) {
    if (e.is(Json::Type::Object) && e.find(key)) {
      return true;
    }
  }
  return false;
}

// Every Smooth-only field set on `s` (track 0, track 1 and the first clip).
void setSmoothFields(Song& s, ObjectId owner) {
  const Tick bar = s.barTicks();
  s.cycleStart = 4 * bar, s.cycleEnd = 8 * bar, s.cycleOn = true;
  s.projectEnd = 40 * bar;
  PlaylistTrack& t0 = s.tracks[0];
  t0.laneSet = true, t0.laneShown = true, t0.laneOwner = owner, t0.laneIndex = 2;
  t0.autoMode = 3;  // Latch
  s.tracks[1].sound = owner;
  Clip& c = s.playlist[0];
  c.name = "Hook";
  c.playTranspose = -5, c.playVelocity = 12, c.playDelay = 7, c.playQuantize = s.ppq / 4;
}

bool sameSmoothFields(const Song& a, const Song& b) {
  const PlaylistTrack &x = a.tracks[0], &y = b.tracks[0];
  const Clip &c = a.playlist[0], &d = b.playlist[0];
  return a.cycleStart == b.cycleStart && a.cycleEnd == b.cycleEnd && a.cycleOn == b.cycleOn && a.projectEnd == b.projectEnd &&
         x.laneSet == y.laneSet && x.laneShown == y.laneShown && x.laneOwner == y.laneOwner && x.laneIndex == y.laneIndex &&
         x.autoMode == y.autoMode && a.tracks[1].sound == b.tracks[1].sound && c.name == d.name && c.playTranspose == d.playTranspose &&
         c.playVelocity == d.playVelocity && c.playDelay == d.playDelay && c.playQuantize == d.playQuantize;
}

Song baseSong(ObjectId* owner) {
  Document d(makeEmptySong());
  *owner = d.addChannel("Keys", "gl.synth");
  const ObjectId pat = d.song().patterns[0].id;
  d.addNote(pat, *owner, {0, 96, 60, 100});
  d.addClip(pat, 0, 0);
  return d.song();
}

}  // namespace

int main() {
  ObjectId owner = kNoId;
  const Song plain = baseSong(&owner);
  expect(plain.tracks.size() >= 2 && !plain.playlist.empty(), "the base song has tracks and a region");

  // 1. Additive: none of the keys in a song that never used them.
  {
    const Json j = songToJson(plain);
    bool none = true;
    for (const char* k : kSongKeys) {
      none = none && !anyKey(j, k);
    }
    for (const char* k : kTrackKeys) {
      none = none && !anyKey(j, k, "tracks");
    }
    for (const char* k : kClipKeys) {
      none = none && !anyKey(j, k, "playlist");
    }
    expect(none, "a song that never used the Smooth fields saves none of their keys");
  }

  // 2. A save and a load keep them; the second save is byte-identical.
  Song full = plain;
  setSmoothFields(full, owner);
  {
    const Json j = songToJson(full);
    bool all = true;
    for (const char* k : kSongKeys) {
      all = all && anyKey(j, k);
    }
    for (const char* k : kTrackKeys) {
      all = all && anyKey(j, k, "tracks");
    }
    for (const char* k : kClipKeys) {
      all = all && anyKey(j, k, "playlist");
    }
    expect(all, "every Smooth field is written once set");
    Song back;
    std::string err;
    expect(songFromJson(j, back, &err), "the save loads");
    expect(sameSmoothFields(full, back), "every Smooth field survives a save and a load");
    expect(jsonStringify(songToJson(back), 2) == jsonStringify(j, 2), "the second save is byte-identical to the first");
    // Cleared again, the keys go: the song saves as if they never were.
    Song cleared = back;
    cleared.cycleStart = cleared.cycleEnd = 0, cleared.cycleOn = false, cleared.projectEnd = 0;
    cleared.tracks[0].laneSet = cleared.tracks[0].laneShown = false, cleared.tracks[0].laneOwner = kNoId, cleared.tracks[0].laneIndex = 0;
    cleared.tracks[0].autoMode = 1, cleared.tracks[1].sound = kNoId;
    Clip& c = cleared.playlist[0];
    c.name.clear(), c.playTranspose = c.playVelocity = 0, c.playDelay = c.playQuantize = 0;
    expect(jsonStringify(songToJson(cleared), 2) == jsonStringify(songToJson(plain), 2), "cleared, the song saves as before");
  }

  // 3. Collab: Alex sets every field, Sam receives them; Sam changes them, Alex receives that.
  {
    Clock clock;
    Net net(52);
    Peer a, b;
    a.label = "Alex", b.label = "Sam";
    a.id = crypto::Identity::generate(), b.id = crypto::Identity::generate();
    a.doc.reset(plain);
    a.folder = tmpDir("k112b_compat_a"), b.folder = tmpDir("k112b_compat_b");
    std::string err;
    a.s = Session::create(a.doc, config(a.folder, a.id, a.label, &clock), &err);
    expect(a.s != nullptr, "the session starts");
    if (a.s) {
      net.add(&a);
      b.s = Session::join(b.doc, config(b.folder, b.id, b.label, &clock), ticketOf(*a.s), &err);
      a.s->admitMember(memberOf(b));
      net.add(&b);
      net.connect(&a, &b);
      settle(net, {&a, &b}, clock);
      expect(b.s && b.s->ready(), "Sam joins");
      a.doc.editSong("smooth fields", [&](Song& x) { setSmoothFields(x, owner); });
      settle(net, {&a, &b}, clock);
      expect(sameSmoothFields(a.doc.song(), b.doc.song()) && b.doc.song().playlist[0].name == "Hook", "every Smooth field reaches Sam");
      expect(songText(a) == songText(b), "both peers' songs are the same");
      if (songText(a) != songText(b)) {
        std::ofstream("k112b_compat_alex.json") << songText(a);
        std::ofstream("k112b_compat_sam.json") << songText(b);
      }
      b.doc.editSong("smooth fields b", [](Song& x) {
        x.cycleOn = false;
        x.tracks[0].autoMode = 4;
        x.playlist[0].name = "Hook B";
      });
      settle(net, {&a, &b}, clock);
      expect(!a.doc.song().cycleOn && a.doc.song().tracks[0].autoMode == 4 && a.doc.song().playlist[0].name == "Hook B",
             "Sam's changes to them reach Alex");
      const Tick bar = a.doc.song().barTicks();
      expect(a.doc.song().cycleEnd == 8 * bar && a.doc.song().projectEnd == 40 * bar && a.doc.song().tracks[1].sound == owner,
             "Sam's edit keeps the fields it did not touch on Alex's side");
      expect(songText(a) == songText(b), "the peers converge");
    }
  }
  return finish();
}
