// Gridline - K-112c: the Clips layout's two views list the same tracks. The arrangement's rows
// and the clip grid's columns are both trackmodes::usedTrackCount(song) tracks, in the song's
// order, so the checks here are on that count and on the song's tracks after each edit:
//  * a song with the default 32 tracks and three in use lists four: never fewer than kShownTracks,
//    so a new set shows four empty tracks with empty slots (as the reference's new set), without
//    making them in the song (the empty tracks after them stay in the song for Studio's playlist);
//  * an empty track between used ones is listed (its lane shows with no clips);
//  * a new track (addUsedTrack) comes right after the used ones, named, and is listed at once;
//    the list grows when every track is in use;
//  * insert, delete, move and rename (the ops both views' menus and drags use) keep the listed
//    set and order the same for both views;
//  * collaboration: each edit made by one person reaches the other with the same listed tracks.
//  * K-112c (round 43): a clip dropped into an empty row of the grid grows the live area
//    (ops::extendLiveArea): the song after it moves right by as much, one undo step, and the
//    other person gets it.
#include <string>
#include <vector>
#include "Check.h"
#include "collab/CollabSim.h"
#include "core/Demo.h"
#include "core/MenuOps.h"
#include "core/MenuParity.h"
#include "core/TrackModes.h"
#include "core/TrackOrder.h"

using namespace gl;
using namespace gl::collab;
using namespace sim;

namespace {

std::vector<std::string> listed(const Song& s) {
  std::vector<std::string> v;
  for (int t = 0; t < trackmodes::usedTrackCount(s); ++t) v.push_back(s.tracks[static_cast<size_t>(t)].name);
  return v;
}

std::string joined(const std::vector<std::string>& v) {
  std::string r;
  for (const std::string& n : v) r += (r.empty() ? "" : ",") + n;
  return r;
}

void name(Document& d, int t, const std::string& n) {
  d.editPlaylistTrack(t, "rename track", [&](PlaylistTrack& tr) { tr.name = n; });
}

Song threeTracks() {
  Song s = makeEmptySong();
  s.tracks.resize(kDefaultPlaylistTracks);
  s.tracks[0].name = "Drums";
  s.tracks[1].name = "Bass";
  s.tracks[2].name = "Chords";
  return s;
}

void testCount() {
  Song s = threeTracks();
  expect(static_cast<int>(s.tracks.size()) == kDefaultPlaylistTracks, "the song keeps its 32 tracks");
  expect(trackmodes::usedTrackCount(s) == 4, "three tracks in use, with one empty track: never fewer than four");
  Song e = makeEmptySong();
  e.tracks.resize(kDefaultPlaylistTracks);
  const std::string before = e.tracks[0].name + e.tracks[3].name;
  expect(trackmodes::usedTrackCount(e) == trackmodes::kShownTracks, "a new set shows four empty tracks");
  expect(before.empty() && e.tracks.size() == static_cast<size_t>(kDefaultPlaylistTracks), "shown, not made: the song is unchanged");
  Song few = makeEmptySong();
  few.tracks.resize(2);
  expect(trackmodes::usedTrackCount(few) == 2, "never more than the song has");
  s.tracks[4].name = "Keys";
  expect(trackmodes::usedTrackCount(s) == 5, "a used track after the four is listed");
  s.tracks[1].name.clear();
  expect(trackmodes::usedTrackCount(s) == 5, "an empty track between used ones is listed");
  s.tracks[4].name.clear();
  s.tracks[2].name.clear();
  expect(trackmodes::usedTrackCount(s) == 4, "empty tracks after the four are not listed");
  Clip c;
  c.track = 6;
  s.playlist.push_back(c);
  expect(trackmodes::usedTrackCount(s) == 7, "a track with a clip is listed, with the tracks before it");
  s.tracks.resize(2);
  s.playlist.clear();
  s.tracks[0].name = s.tracks[1].name = "x";
  const int t = trackmodes::addUsedTrack(s);
  expect(t == 2 && s.tracks.size() == 3 && trackmodes::usedTrackCount(s) == 3, "a new track grows a full list");
  expect(s.tracks[2].name == "Track 3", "the new track is named after its place");
}

void testEdits() {
  Document d;
  d.reset(threeTracks());
  expect(joined(listed(d.song())) == "Drums,Bass,Chords,", "the three tracks, in order, and one empty track");
  const int t = trackmodes::addUsedTrack(d);
  expect(t == 4 && joined(listed(d.song())) == "Drums,Bass,Chords,,Track 5", "a new track is listed at once, after the shown ones");
  name(d, 4, "Keys");
  expect(joined(listed(d.song())) == "Drums,Bass,Chords,,Keys", "a rename shows in the list");
  ops::insertPlaylistTrack(d, 1);
  expect(joined(listed(d.song())) == "Drums,,Bass,Chords,,Keys", "an inserted track is listed where it was put");
  ops::deletePlaylistTrack(d, 1);
  expect(joined(listed(d.song())) == "Drums,Bass,Chords,,Keys", "a deleted track leaves the list");
  trackorder::movePlaylistTracks(d, {0}, 2);
  expect(joined(listed(d.song())) == "Bass,Drums,Chords,,Keys", "a moved track moves in the list");
  ops::deletePlaylistTrack(d, 4);
  expect(joined(listed(d.song())) == "Bass,Drums,Chords,", "deleting the last used track shortens the list");
  d.undo();
  expect(joined(listed(d.song())) == "Bass,Drums,Chords,,Keys", "undo brings it back");
  expect(static_cast<int>(d.song().tracks.size()) >= kDefaultPlaylistTracks - 1, "the empty tracks stay in the song (Studio)");
  const size_t before = d.song().tracks.size();
  expect(trackmodes::addUsedTrack(d, 8) == 12 && trackmodes::usedTrackCount(d.song()) == 13, "eight new tracks at once, all listed");
  expect(d.song().tracks.size() == before, "made from the empty tracks after the used ones (the song keeps its count)");
  d.undo();
  expect(joined(listed(d.song())) == "Bass,Drums,Chords,,Keys", "the eight are one undo step");
}

// K-112c (round 43): a new project made in Clips starts with the reference's new set's tracks.
void testStarter() {
  Song s = makeEmptySong();
  s.tracks.resize(kDefaultPlaylistTracks);
  const size_t mixers = s.mixer.size();
  trackmodes::makeClipsStarterTracks(s);
  expect(joined(listed(s)) == "1 MIDI,2 MIDI,3 Audio,4 Audio", "a new Clips set: two MIDI tracks, then two audio tracks");
  expect(trackmodes::modeOf(s, 0) == TrackMode::Unassigned && trackmodes::modeOf(s, 1) == TrackMode::Unassigned, "the MIDI tracks are empty (they take pattern clips)");
  const MixerTrack* m3 = trackmodes::linkedMixer(s, 2);
  const MixerTrack* m4 = trackmodes::linkedMixer(s, 3);
  expect(trackmodes::modeOf(s, 2) == TrackMode::Audio && trackmodes::modeOf(s, 3) == TrackMode::Audio && m3 && m4 && m3 != m4,
         "the audio tracks each have a mixer track of their own");
  expect(m3 && m3->name == "3 Audio" && s.mixer.size() >= mixers, "the mixer track is named after its audio track");
  {  // round 45: the returns A Reverb and B Delay, each audio track sending to both at zero
    const MixerTrack* ra = nullptr;
    const MixerTrack* rb = nullptr;
    for (const MixerTrack& m : s.mixer) {
      if (m.name == "Reverb") ra = &m;
      if (m.name == "Delay") rb = &m;
    }
    expect(ra && rb && ra != rb && ra->slots[0].plugin.type == "gl.reverb" && rb->slots[0].plugin.type == "gl.delay",
           "a new Clips set has the returns A Reverb and B Delay (Gridline's reverb and delay)");
    int sends = 0;
    for (const MixerTrack* m : {m3, m4})
      if (m)
        for (const Send& sd : m->sends)
          if (ra && rb && (sd.to == ra->id || sd.to == rb->id) && sd.level == 0.0f && !sd.sidechain) ++sends;
    expect(sends == 4, "each audio track sends to both returns at zero");
    Song again = s;
    trackmodes::makeClipsStarterTracks(again);
    int count = 0;
    for (const MixerTrack& m : again.mixer) count += m.name == "Reverb" || m.name == "Delay";
    expect(count == 2, "made again, the returns are not doubled");
  }
  expect(static_cast<int>(s.tracks.size()) == kDefaultPlaylistTracks && trackmodes::usedTrackCount(s) == 4, "the song keeps its 32 tracks, four listed");
  Song old = threeTracks();
  expect(trackmodes::usedTrackCount(old) == 4 && old.tracks[3].name.empty(), "an older project is not changed (four shown, the fourth unnamed)");
  Song used = threeTracks();
  trackmodes::makeClipsStarterTracks(used);
  expect(used.tracks[0].name == "Drums" && used.tracks[3].name == "4 Audio", "tracks already in use keep their names");
}

// K-112c (round 45): Create > New audio track / New MIDI track (Ctrl+T, Ctrl+Shift+T in the Clips set).
void testNewTracks() {
  Document d;
  d.reset(threeTracks());  // Drums, Bass, Chords, and a fourth shown track, empty
  const size_t undo0 = d.history().position();
  expect(trackmodes::addAudioTrack(d) == 3, "a new audio track takes the shown empty fourth track");
  expect(d.song().tracks[3].name == "4 Audio" && trackmodes::modeOf(d.song(), 3) == TrackMode::Audio && trackmodes::linkedMixer(d.song(), 3),
         "it is named by its number and has a mixer track of its own");
  expect(d.history().position() == undo0 + 1, "one undo step");
  expect(trackmodes::addMidiTrack(d) == 4 && d.song().tracks[4].name == "5 MIDI" && trackmodes::modeOf(d.song(), 4) == TrackMode::Unassigned,
         "a new MIDI track comes after the shown tracks, empty (it takes pattern clips)");
  expect(joined(listed(d.song())) == "Drums,Bass,Chords,4 Audio,5 MIDI", "both are listed in order");
  d.undo();
  d.undo();
  expect(joined(listed(d.song())) == "Drums,Bass,Chords,", "two undos put the set back");
}

// K-112c (round 45): the demo, opened in Clips, has its clips in the grid (Prepare the live area);
// the demo itself (Studio's) has none.
void testDemoGrid() {
  const Song demo = makeDemoTrack();
  expect(demo.live.areaEnd == 0 && !demo.playlist.empty(), "the demo song has no live area of its own");
  Document d(demo);
  const Tick end = ops::prepareLiveArea(d);
  const Song& s = d.song();
  int inArea = 0;
  for (const Clip& c : s.playlist) inArea += c.start < s.live.areaEnd;
  expect(end > 0 && s.live.areaEnd == end && inArea > 0, "prepared, its grid has clips");
  expect(s.playlist.size() == demo.playlist.size() + static_cast<size_t>(inArea), "the arrangement keeps every clip (moved right)");
}

// K-112c (round 44): the mixer track a track plays through (its Clips strip).
void testPlayedMixer() {
  Document d;
  d.reset(threeTracks());
  const ObjectId pad = d.addChannel("Pad", "gl.synth"), lead = d.addChannel("Lead", "gl.synth");
  const ObjectId m3 = d.song().mixer[3].id, m4 = d.song().mixer[4].id;
  d.editChannel(pad, "", [&](Channel& c) { c.mixerTrack = m3; });
  d.editChannel(lead, "", [&](Channel& c) { c.mixerTrack = m4; });
  const ObjectId p1 = d.addPattern("Pad part"), p2 = d.addPattern("Both");
  d.editSong("notes", [&](Song& x) {
    x.pattern(p1)->notes[pad] = {Note{}};
    x.pattern(p2)->notes[pad] = {Note{}};
    x.pattern(p2)->notes[lead] = {Note{}};
  });
  d.addClip(p1, 1, 0);
  d.addClip(p2, 2, 0);
  const Song& s = d.song();
  const MixerTrack* a = trackmodes::playedMixer(s, 1);
  expect(a && a->id == m3, "a track whose clips all play one channel shows that channel's mixer track");
  expect(trackmodes::playedMixer(s, 2) == nullptr, "a track playing into two mixer tracks shows none");
  expect(trackmodes::playedMixer(s, 0) == nullptr, "a track with no clips shows none");
  Song t = makeEmptySong();
  t.tracks.resize(kDefaultPlaylistTracks);
  trackmodes::makeClipsStarterTracks(t);
  expect(trackmodes::playedMixer(t, 2) == trackmodes::linkedMixer(t, 2) && trackmodes::playedMixer(t, 2), "an audio track shows its own mixer track");
}

void testLiveArea() {
  Document d;
  d.reset(threeTracks());
  const Tick scene = d.song().barTicks() * 4;
  const ObjectId pat = d.addPattern("Beat");
  const ObjectId song = d.addClip(pat, 0, 0);  // the arrangement starts at bar 1
  d.editSong("marker", [&](Song& x) { x.markers.push_back(Marker{scene, "Verse"}); });
  const size_t undo0 = d.history().position();
  expect(ops::extendLiveArea(d, 3 * scene), "an empty set's live area grows to three scenes");
  const Song& s = d.song();
  expect(s.live.areaEnd == 3 * scene, "the live area ends after the third scene");
  expect(s.clip(song) && s.clip(song)->start == 3 * scene, "the arrangement's clip moves right by as much (it stays a song clip)");
  expect(!s.markers.empty() && s.markers.back().pos == 4 * scene, "its markers move with it");
  expect(!ops::extendLiveArea(d, 2 * scene) && d.song().live.areaEnd == 3 * scene, "a shorter end leaves the area alone");
  expect(d.history().position() == undo0 + 1, "one undo step");
  d.undo();
  expect(d.song().live.areaEnd == 0 && d.song().clip(song)->start == 0 && d.song().markers.back().pos == scene, "undo puts it all back");
}

void testCollab() {
  Clock clock;
  Net net(41);
  Peer a, b;
  a.label = "Alex";
  b.label = "Sam";
  a.id = crypto::Identity::generate();
  b.id = crypto::Identity::generate();
  a.doc.reset(threeTracks());
  a.folder = tmpDir("k112c_tracks_a");
  b.folder = tmpDir("k112c_tracks_b");
  std::string err;
  a.s = Session::create(a.doc, config(a.folder, a.id, a.label, &clock), &err);
  net.add(&a);
  b.s = Session::join(b.doc, config(b.folder, b.id, b.label, &clock), ticketOf(*a.s), &err);
  a.s->admitMember(memberOf(b));
  net.add(&b);
  net.connect(&a, &b);
  settle(net, {&a, &b}, clock);
  expect(joined(listed(b.doc.song())) == "Drums,Bass,Chords,", "collab: the joiner lists the same tracks");
  trackmodes::addUsedTrack(a.doc);  // Alex adds a track (in either view)
  settle(net, {&a, &b}, clock);
  expect(joined(listed(b.doc.song())) == "Drums,Bass,Chords,,Track 5", "collab: a new track reaches the other person");
  name(b.doc, 4, "Vox");  // Sam renames it
  settle(net, {&a, &b}, clock);
  expect(joined(listed(a.doc.song())) == "Drums,Bass,Chords,,Vox", "collab: a rename comes back");
  trackorder::movePlaylistTracks(b.doc, {4}, 0);  // Sam moves it to the front
  settle(net, {&a, &b}, clock);
  expect(joined(listed(a.doc.song())) == "Vox,Drums,Bass,Chords", "collab: a move reaches the other person");
  ops::deletePlaylistTrack(a.doc, 2);  // Alex removes Bass
  settle(net, {&a, &b}, clock);
  expect(joined(listed(b.doc.song())) == "Vox,Drums,Chords,", "collab: a removal reaches the other person");
  expect(songText(a) == songText(b), "collab: both songs are the same");
  const Tick scene = a.doc.song().barTicks() * 4;
  ops::extendLiveArea(b.doc, 2 * scene);  // Sam drops a file into the grid's second row
  settle(net, {&a, &b}, clock);
  expect(a.doc.song().live.areaEnd == 2 * scene, "collab: a grown live area reaches the other person");
  expect(songText(a) == songText(b), "collab: both songs are still the same");
  trackmodes::addAudioTrack(a.doc);  // Alex: Create > New audio track
  settle(net, {&a, &b}, clock);
  trackmodes::addMidiTrack(b.doc);   // then Sam: New MIDI track
  settle(net, {&a, &b}, clock);
  const std::vector<std::string> la = listed(a.doc.song());
  expect(la.size() == 5 && la[3] == "4 Audio" && la[4] == "5 MIDI", "collab: new audio and MIDI tracks reach the other person");
  expect(trackmodes::modeOf(b.doc.song(), 3) == TrackMode::Audio && songText(a) == songText(b), "collab: the audio track's mixer track too");
}

}  // namespace

int main() {
  testCount();
  testEdits();
  testStarter();
  testNewTracks();
  testDemoGrid();
  testPlayedMixer();
  testLiveArea();
  testCollab();
  return finish();
}
