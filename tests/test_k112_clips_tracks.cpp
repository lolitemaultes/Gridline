// Gridline - K-112c: the Clips layout's two views list the same tracks. The arrangement's rows
// and the clip grid's columns are both trackmodes::usedTrackCount(song) tracks, in the song's
// order, so the checks here are on that count and on the song's tracks after each edit:
//  * a song with the default 32 tracks and three in use lists those three (checkpoint 53, N52-05: no
//    grey padding tracks); a song with none in use shows kShownTracks empty tracks with empty slots
//    (as the reference's new set), without making them in the song (the empty tracks after them stay
//    in the song for Studio's playlist);
//  * an empty track between used ones is listed (its lane shows with no clips);
//  * a new track (addUsedTrack) comes right after the used ones, named, and is listed at once;
//    the list grows when every track is in use;
//  * insert, delete, move and rename (the ops both views' menus and drags use) keep the listed
//    set and order the same for both views;
//  * collaboration: each edit made by one person reaches the other with the same listed tracks.
//  * K-112c (round 43): a clip dropped into an empty row of the grid grows the live area
//    (ops::extendLiveArea): the song after it moves right by as much, one undo step, and the
//    other person gets it.
#include <cmath>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "Check.h"
#include "collab/CollabSim.h"
#include "core/BrowserIndex.h"
#include "core/Demo.h"
#include "core/MenuOps.h"
#include "core/MenuParity.h"
#include "core/PluginInfo.h"
#include "core/TrackModes.h"
#include "core/TrackOrder.h"
#include "core/MixerOps.h"
#include "core/Params.h"
#include "core/Project.h"
#include "core/WavWriter.h"
#include "engine/LiveRender.h"
#include "engine/Render.h"
#include "engine/Sample.h"

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
  expect(trackmodes::usedTrackCount(s) == 3, "three tracks in use: the three are listed, no empty padding (N52-05)");
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
  expect(trackmodes::usedTrackCount(s) == 1, "empty tracks after the used ones are not listed");
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
  expect(joined(listed(d.song())) == "Drums,Bass,Chords", "the three tracks, in order");
  const int t = trackmodes::addUsedTrack(d);
  expect(t == 3 && joined(listed(d.song())) == "Drums,Bass,Chords,Track 4", "a new track is listed at once, after the shown ones");
  name(d, 3, "Keys");
  expect(joined(listed(d.song())) == "Drums,Bass,Chords,Keys", "a rename shows in the list");
  ops::insertPlaylistTrack(d, 1);
  expect(joined(listed(d.song())) == "Drums,,Bass,Chords,Keys", "an inserted track is listed where it was put");
  ops::deletePlaylistTrack(d, 1);
  expect(joined(listed(d.song())) == "Drums,Bass,Chords,Keys", "a deleted track leaves the list");
  trackorder::movePlaylistTracks(d, {0}, 2);
  expect(joined(listed(d.song())) == "Bass,Drums,Chords,Keys", "a moved track moves in the list");
  ops::deletePlaylistTrack(d, 3);
  expect(joined(listed(d.song())) == "Bass,Drums,Chords", "deleting the last used track shortens the list");
  d.undo();
  expect(joined(listed(d.song())) == "Bass,Drums,Chords,Keys", "undo brings it back");
  expect(static_cast<int>(d.song().tracks.size()) >= kDefaultPlaylistTracks - 1, "the empty tracks stay in the song (Studio)");
  const size_t before = d.song().tracks.size();
  expect(trackmodes::addUsedTrack(d, 8) == 11 && trackmodes::usedTrackCount(d.song()) == 12, "eight new tracks at once, all listed");
  expect(d.song().tracks.size() == before, "made from the empty tracks after the used ones (the song keeps its count)");
  d.undo();
  expect(joined(listed(d.song())) == "Bass,Drums,Chords,Keys", "the eight are one undo step");
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
  expect(trackmodes::usedTrackCount(old) == 3 && old.tracks[3].name.empty(), "an older project is not changed (its three tracks shown, none added)");
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
  expect(joined(listed(d.song())) == "Drums,Bass,Chords", "two undos put the set back");
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

// C44-07 / C44-20: the Clips way to fill the grid puts one clip in each slot (scene i of a track
// holds its i-th distinct clip, at the scene's start, whatever its length), the live area is whole
// scenes past every clip's end, and the arrangement after it is the demo's, moved right as a whole.
void testClipGrid() {
  const Song demo = makeDemoTrack();
  Document d(demo);
  const Tick scene = std::max<Tick>(1, demo.barTicks()) * 4;
  const Tick grew = ops::prepareClipGrid(d, scene);
  const Song& s = d.song();
  expect(grew > 0 && s.live.areaEnd == grew && s.live.areaEnd % scene == 0, "the clip grid's area is whole scenes");
  bool oneEach = true, atStart = true, insideArea = true;
  std::map<std::pair<int, Tick>, int> slots;
  for (const Clip& c : s.playlist) {
    if (c.start >= s.live.areaEnd) continue;
    atStart = atStart && c.start % scene == 0;
    oneEach = oneEach && ++slots[{c.track, c.start / scene}] == 1;
    insideArea = insideArea && c.start + s.clipLength(c) <= s.live.areaEnd;
  }
  expect(!slots.empty() && oneEach, "one clip to a slot");
  expect(atStart, "each clip starts at its scene's start");
  expect(insideArea, "no grid clip reaches into the arrangement");
  size_t arr = 0;
  bool same = true;
  for (const Clip& c : s.playlist)
    if (c.start >= s.live.areaEnd) {
      ++arr;
      const Clip* o = demo.clip(c.id);
      same = same && o && o->start + s.live.areaEnd == c.start && o->track == c.track;
    }
  expect(arr == demo.playlist.size() && same, "the arrangement is the demo's, moved right by the area");
  expect(d.undo() && d.song().live.areaEnd == 0 && d.song().playlist.size() == demo.playlist.size(), "one undo step takes it back");
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

// C47 / N45-05: a bus is not a return. The demo routes its drum, bass, music and weather sounds
// into four buses (each sound's output) and sends to three effect returns: the strips show the
// three returns, and a track whose sounds all go into one bus shows that bus's fader.
void testBusesAndReturns() {
  const Song demo = makeDemoTrack();
  std::vector<std::string> names;
  for (size_t j : trackmodes::returnTracks(demo)) names.push_back(demo.mixer[j].name);
  expect(joined(names) == "Hall,Echo,Space", ("the demo's returns are its three effect returns, not its buses (" + joined(names) + ")").c_str());
  int drums = -1;
  for (int t = 0; t < static_cast<int>(demo.tracks.size()); ++t)
    if (demo.tracks[static_cast<size_t>(t)].name == "Drums") drums = t;
  const MixerTrack* bus = drums >= 0 ? trackmodes::playedMixer(demo, drums) : nullptr;
  expect(trackmodes::playedMixers(demo, drums).size() > 1 && bus && bus->name == "Drum Bus",
         "a track playing several drum sounds, all routed into Drum Bus, shows Drum Bus");
  Song s = demo;
  for (MixerTrack& m : s.mixer)
    if (m.name == "Snare") m.sends.front().to = s.masterId();  // one sound out of the bus
  expect(trackmodes::playedMixer(s, drums) == nullptr, "when one of its sounds goes elsewhere, it shows none");
  Song n = makeEmptySong();
  n.tracks.resize(kDefaultPlaylistTracks);
  trackmodes::makeClipsStarterTracks(n);
  names.clear();
  for (size_t j : trackmodes::returnTracks(n)) names.push_back(n.mixer[j].name);
  expect(joined(names) == "Reverb,Delay", "a new set's returns are A Reverb and B Delay");
}

// N45-08: a new track goes right after the selected one and takes the next track colour; the
// tracks named by their place follow their new place, as the reference numbers its tracks.
void testInsertAfter() {
  Document d;
  Song st = makeEmptySong();
  st.tracks.resize(kDefaultPlaylistTracks);
  trackmodes::makeClipsStarterTracks(st);
  const ObjectId pat = st.patterns.empty() ? kNoId : st.patterns[0].id;
  d.reset(st);
  if (pat != kNoId) d.addClip(pat, 1, 0);  // a clip on 2 MIDI
  const size_t undo0 = d.history().position();
  const int t = trackmodes::addMidiTrack(d, 0);
  const Song& s = d.song();
  expect(t == 1, "a new MIDI track goes right after the selected first track");
  expect(joined(listed(s)) == "1 MIDI,2 MIDI,3 MIDI,4 Audio,5 Audio", ("the tracks after it are numbered for their place (" + joined(listed(s)) + ")").c_str());
  bool moved = true;
  for (const Clip& c : s.playlist) moved = moved && c.track == 2;
  expect(pat == kNoId || (!s.playlist.empty() && moved), "the clips of the tracks after it move down with them");
  expect(s.tracks[1].color != PlaylistTrack{}.color && s.tracks[1].color != s.tracks[0].color && s.tracks[1].color != s.tracks[3].color,
         "it has a colour of its own");
  expect(trackmodes::modeOf(s, 3) == TrackMode::Audio && trackmodes::linkedMixer(s, 3) && trackmodes::linkedMixer(s, 3)->name == "4 Audio" &&
             trackmodes::linkedMixer(s, 4) && trackmodes::linkedMixer(s, 4)->name == "5 Audio",
         "an audio track keeps its mixer track as it moves, and the mixer track takes its new name (N47-09)");
  expect(d.history().position() == undo0 + 1, "one undo step");
  const int e = trackmodes::addAudioTrack(d, 4);
  expect(e == 5 && d.song().tracks[5].name == "6 Audio", "after the last track it goes at the end");
  {  // N47-09: no two mixer tracks share a track's name after the renumbering
    std::set<std::string> names;
    bool unique = true;
    for (int i = 2; i <= 5; ++i)
      if (const MixerTrack* m = trackmodes::linkedMixer(d.song(), i)) unique = unique && names.insert(m->name).second;
    expect(unique, "the audio tracks' mixer tracks have one name each");
  }
  expect(trackmodes::addAudioTrack(d) == 6, "with nothing selected it goes after the tracks in use");
  d.undo();
  d.undo();
  d.undo();
  expect(joined(listed(d.song())) == "1 MIDI,2 MIDI,3 Audio,4 Audio", "undo puts the set back");
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
  expect(joined(listed(b.doc.song())) == "Drums,Bass,Chords", "collab: the joiner lists the same tracks");
  trackmodes::addUsedTrack(a.doc);  // Alex adds a track (in either view)
  settle(net, {&a, &b}, clock);
  expect(joined(listed(b.doc.song())) == "Drums,Bass,Chords,Track 4", "collab: a new track reaches the other person");
  name(b.doc, 3, "Vox");  // Sam renames it
  settle(net, {&a, &b}, clock);
  expect(joined(listed(a.doc.song())) == "Drums,Bass,Chords,Vox", "collab: a rename comes back");
  trackorder::movePlaylistTracks(b.doc, {3}, 0);  // Sam moves it to the front
  settle(net, {&a, &b}, clock);
  expect(joined(listed(a.doc.song())) == "Vox,Drums,Bass,Chords", "collab: a move reaches the other person");
  ops::deletePlaylistTrack(a.doc, 2);  // Alex removes Bass
  settle(net, {&a, &b}, clock);
  expect(joined(listed(b.doc.song())) == "Vox,Drums,Chords", "collab: a removal reaches the other person");
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

// Checkpoint 51: Cmd+G. Tracks 3 Audio and 1 MIDI (with an instrument on Main) of a new Clips set
// are grouped: they move together under a group track on a mixer track of its own, which they play
// into; the group sends to the returns; undo puts it all back; ungroup restores the routing.
void testGroups() {
  Song s0 = makeEmptySong();
  s0.tracks.resize(kDefaultPlaylistTracks);
  trackmodes::makeClipsStarterTracks(s0);
  Document d;
  d.reset(s0);
  const ObjectId ch = d.addChannel("Keys", "gl.sine");
  d.editSong("link", [&](Song& x) {
    x.channel(ch)->mixerTrack = x.masterId();
    x.tracks[0].mode = TrackMode::Instrument;
    x.tracks[0].channel = ch;
  });
  const std::string before = jsonStringify(songToJson(d.song()), 0);
  const ObjectId audioMixer = trackmodes::linkedMixer(d.song(), 2)->id;
  std::string why;
  const int g = trackmodes::groupTracks(d, {2, 0}, &why);
  const Song& s = d.song();
  expect(g == 0, ("the group track goes at the first track's place: " + why).c_str());
  expect(joined(listed(s)) == "1 Group,2 MIDI,3 Audio,4 MIDI,5 Audio", ("the members moved together under it: " + joined(listed(s))).c_str());
  expect(trackmodes::isGroupTrack(s, 0) && s.tracks[0].groupSize == 2, "a group track of two");
  expect(trackmodes::groupTrackOf(s, 1) == 0 && trackmodes::groupTrackOf(s, 2) == 0 && trackmodes::groupTrackOf(s, 3) == -1, "its members");
  const MixerTrack* gm = trackmodes::linkedMixer(s, 0);
  expect(gm && gm->name == "1 Group" && trackmodes::outputOf(*gm) == s.masterId(), "the group's mixer track plays into Main");
  const MixerTrack* am = s.mixerTrack(audioMixer);
  expect(gm && am && trackmodes::outputOf(*am) == gm->id, "the audio member plays into the group");
  const Channel* kc = s.channel(ch);
  const MixerTrack* km = kc ? s.mixerTrack(kc->mixerTrack) : nullptr;
  expect(gm && km && km->id != s.masterId() && km->id != gm->id && trackmodes::outputOf(*km) == gm->id,
         "the instrument on Main gets a mixer track of its own, into the group");
  int rs = 0;
  if (gm)
    for (const Send& sd : gm->sends) rs += sd.level == 0.0f ? 1 : 0;
  expect(rs == 2, "the group sends to both returns at zero");
  expect(trackmodes::groupTracks(d, {1, 3}) < 0, "tracks of different groups are not grouped together");
  d.undo();
  expect(jsonStringify(songToJson(d.song()), 0) == before, "one undo puts the set back");
  d.redo();
  expect(trackmodes::ungroupTracks(d, 0, &why), "ungroup");
  const Song& u = d.song();
  expect(joined(listed(u)) == "1 MIDI,2 Audio,3 MIDI,4 Audio",
         ("the group track goes, its tracks stay: " + joined(listed(u))).c_str());
  const MixerTrack* am2 = u.mixerTrack(audioMixer);
  expect(am2 && trackmodes::outputOf(*am2) == u.masterId(), "the audio member plays into Main again");
  expect(!trackmodes::isGroupTrack(u, 0) && u.tracks[0].groupSize == 0, "no group is left");
  d.undo();
  expect(trackmodes::isGroupTrack(d.song(), 0), "undo brings the group back");
}

// Checkpoint 52: groups inside groups (as the reference's), a member given its sound later plays
// into its group, a new track next to a member joins the group, the group lane takes no clips.
void testNestedGroups() {
  Song s0 = makeEmptySong();
  s0.tracks.resize(kDefaultPlaylistTracks);
  trackmodes::makeClipsStarterTracks(s0);
  Document d;
  d.reset(s0);
  std::string why;
  expect(trackmodes::groupTracks(d, {0, 1, 2, 3}, &why) == 0, ("outer group: " + why).c_str());
  const int inner = trackmodes::groupTracks(d, {2, 3}, &why);
  const Song& s = d.song();
  expect(inner == 2, ("a group inside the group: " + why).c_str());
  expect(joined(listed(s)) == "1 Group,2 MIDI,3 Group,4 MIDI,5 Audio,6 Audio", ("nested order: " + joined(listed(s))).c_str());
  expect(s.tracks[0].groupSize == 5 && s.tracks[2].groupSize == 2, "the outer group holds the inner one and its tracks");
  expect(trackmodes::groupTrackOf(s, 3) == 2 && trackmodes::groupTrackOf(s, 2) == 0 && trackmodes::groupTrackOf(s, 1) == 0 &&
             trackmodes::groupTrackOf(s, 5) == 0,
         "each track's own group is the innermost");
  const MixerTrack* om = trackmodes::linkedMixer(s, 0);
  const MixerTrack* im = trackmodes::linkedMixer(s, 2);
  const MixerTrack* am = trackmodes::linkedMixer(s, 4);
  expect(om && im && am && trackmodes::outputOf(*im) == om->id && trackmodes::outputOf(*am) == im->id && trackmodes::outputOf(*om) == s.masterId(),
         "audio track -> inner group -> outer group -> Main");
  expect(trackmodes::groupTracks(d, {1, 3}) < 0, "tracks of different groups are not grouped together");
  // A new track after the inner group's last track joins it (and so the outer one).
  const int nt = trackmodes::addAudioTrack(d, 4);
  const Song& s2 = d.song();
  expect(nt == 5 && s2.tracks[2].groupSize == 3 && s2.tracks[0].groupSize == 6, "the new track is in both groups");
  const MixerTrack* nm = trackmodes::linkedMixer(s2, 5);
  const MixerTrack* im2 = trackmodes::linkedMixer(s2, 2);
  expect(nm && im2 && trackmodes::outputOf(*nm) == im2->id, "the new audio track plays into its group");
  // A MIDI track of the group given an instrument plays into the group.
  const ObjectId ch = trackmodes::newInstrumentTrack(d, 1, "gl.synth", &why);
  const Song& s3 = d.song();
  const Channel* c = s3.channel(ch);
  const MixerTrack* cm = c ? s3.mixerTrack(c->mixerTrack) : nullptr;
  const MixerTrack* om3 = trackmodes::linkedMixer(s3, 0);
  expect(cm && om3 && cm->id != om3->id && trackmodes::outputOf(*cm) == om3->id, ("the instrument added later plays into the group: " + why).c_str());
  // Ungrouping the inner group: its tracks play into the outer group, which is a track shorter.
  expect(trackmodes::ungroupTracks(d, 2, &why), "ungroup the inner group");
  const Song& s4 = d.song();
  const MixerTrack* om4 = trackmodes::linkedMixer(s4, 0);
  const MixerTrack* am4 = trackmodes::linkedMixer(s4, 3);
  expect(s4.tracks[0].groupSize == 5 && trackmodes::isGroupTrack(s4, 0), "the outer group keeps its tracks");
  expect(om4 && am4 && trackmodes::outputOf(*am4) == om4->id, "the inner group's tracks play into the outer group");
  // An automation clip on the group lane leaves it a group.
  d.editSong("auto", [](Song& x) {
    Clip a;
    a.id = x.allocateId();
    a.kind = ClipKind::Automation;
    a.track = 0;
    a.length = 384;
    x.playlist.push_back(a);
  });
  expect(trackmodes::isGroupTrack(d.song(), 0), "an automation clip does not undo the group");
}

// Checkpoint 52 (N51-03, N51-06): Duplicate and Delete of a group bring its tracks; a track moved into
// or out of a group plays into the group it is in (syncGroupRouting).
void testGroupEdits() {
  Song s0 = makeEmptySong();
  s0.tracks.resize(kDefaultPlaylistTracks);
  trackmodes::makeClipsStarterTracks(s0);
  Document d;
  d.reset(s0);
  std::string why;
  expect(trackmodes::groupTracks(d, {1, 2}, &why) == 1, ("group: " + why).c_str());
  expect(joined(listed(d.song())) == "1 MIDI,2 Group,3 MIDI,4 Audio,5 Audio", ("grouped: " + joined(listed(d.song()))).c_str());
  const size_t n0 = d.song().tracks.size();
  const int c = trackmodes::duplicateTracks(d, {1}, &why);
  const Song& s = d.song();
  expect(c == 4, ("the copy goes after the group: " + why).c_str());
  expect(joined(listed(s)) == "1 MIDI,2 Group,3 MIDI,4 Audio,5 Group,6 MIDI,7 Audio,8 Audio", ("duplicated: " + joined(listed(s))).c_str());
  expect(trackmodes::isGroupTrack(s, 1) && s.tracks[1].groupSize == 2 && trackmodes::isGroupTrack(s, 4) && s.tracks[4].groupSize == 2,
         "two groups of two, side by side (no overlap)");
  const MixerTrack* g2 = trackmodes::linkedMixer(s, 4);
  const MixerTrack* a2 = trackmodes::linkedMixer(s, 6);
  const MixerTrack* g1 = trackmodes::linkedMixer(s, 1);
  expect(g2 && a2 && g1 && g2->id != g1->id && trackmodes::outputOf(*a2) == g2->id && trackmodes::outputOf(*g2) == s.masterId(),
         "the copied audio track plays into the copied group, which plays into Main");
  int rs = 0;
  if (g2)
    for (const Send& sd : g2->sends) rs += sd.level == 0.0f ? 1 : 0;
  expect(rs == 2, "the copied group sends to both returns");
  expect(trackmodes::deleteTracks(d, {4}, &why) == 3, ("deleting a group deletes its tracks: " + why).c_str());
  expect(joined(listed(d.song())) == "1 MIDI,2 Group,3 MIDI,4 Audio,5 Audio", ("after delete: " + joined(listed(d.song()))).c_str());
  expect(d.song().tracks.size() == n0, "as many tracks as before the copy");
  // 5 Audio (on Main) moved in among the group's tracks plays into the group; moved out, into Main again.
  const ObjectId am = trackmodes::linkedMixer(d.song(), 4)->id;
  expect(trackorder::movePlaylistTracks(d, {4}, 3), "move in");
  trackmodes::syncGroupRouting(d);
  const Song& s2 = d.song();
  const MixerTrack* gm = trackmodes::linkedMixer(s2, 1);
  expect(s2.tracks[1].groupSize == 3, "it joined the group");
  expect(gm && s2.mixerTrack(am) && trackmodes::outputOf(*s2.mixerTrack(am)) == gm->id, "moved in: it plays into the group");
  expect(joined(listed(s2)) == "1 MIDI,2 Group,3 MIDI,4 Audio,5 Audio" && s2.tracks[3].mixer == am, ("renumbered after the move: " + joined(listed(s2))).c_str());
  expect(trackorder::movePlaylistTracks(d, {3}, 0), "move out");
  trackmodes::syncGroupRouting(d);
  const Song& s3 = d.song();
  expect(s3.mixerTrack(am) && trackmodes::outputOf(*s3.mixerTrack(am)) == s3.masterId(), "moved out: it plays into Main again");
}

// Checkpoint 53 (K-300 round 09): copies are independent (N52-01), Delete leaves nothing (N52-05), clips
// of a dropped file play through their track (N52-02), renames follow (N52-06), any layout's move keeps
// the group routing, automation lanes (N52-10), instrument-named tracks renumber (N52-11).
ObjectId addAudioClip(Document& d, int track, ObjectId mixer, const char* name) {
  ObjectId ch = kNoId;
  d.editSong("audio clip", [&](Song& x) {
    Channel c;
    c.id = ch = x.allocateId();
    c.name = name;
    c.plugin.type = "gl.sampler";
    c.audioClip = true;
    c.mixerTrack = mixer;
    x.channels.push_back(c);
    Clip k;
    k.id = x.allocateId();
    k.kind = ClipKind::Audio;
    k.source = ch;
    k.track = track;
    k.start = 0;
    k.length = 1536;
    x.playlist.push_back(k);
  });
  return ch;
}

void testCheckpoint53() {
  Song s0 = makeEmptySong();
  s0.tracks.resize(kDefaultPlaylistTracks);
  trackmodes::makeClipsStarterTracks(s0);
  Document d;
  d.reset(s0);
  std::string why;
  const ObjectId m3 = trackmodes::linkedMixer(d.song(), 2)->id, m4 = trackmodes::linkedMixer(d.song(), 3)->id;
  const ObjectId loop = addAudioClip(d, 2, m3, "Loop 120"), lng = addAudioClip(d, 3, m4, "Long 120");
  d.editMixerTrack(m4, "fx", [](MixerTrack& m) {
    m.slots[0].plugin.type = "gl.eq";
    m.volume = 0.5f;
  });
  const ObjectId syn = trackmodes::newInstrumentTrack(d, 0, "gl.synth", &why);
  expect(syn != kNoId, ("an instrument on 1 MIDI: " + why).c_str());
  const ObjectId pat = d.addPattern("Notes");
  d.editPattern(pat, "notes", [syn](Pattern& p) { p.notes[syn] = {Note{}}; });
  d.addClip(pat, 0, 0, 768);
  expect(trackmodes::groupTracks(d, {2, 3}, &why) == 2, ("group 3 and 4: " + why).c_str());
  const Song before = d.song();
  const size_t chans0 = before.channels.size(), pats0 = before.patterns.size();

  // N52-01: Duplicate of the group: own mixer tracks, own clip sources, devices and fader copied.
  const int c = trackmodes::duplicateTracks(d, {2}, &why);
  {
    const Song& s = d.song();
    expect(c == 5, ("the copy after the group: " + why).c_str());
    const MixerTrack* gc = trackmodes::linkedMixer(s, 5);
    const MixerTrack* a3 = trackmodes::linkedMixer(s, 6);
    const MixerTrack* a4 = trackmodes::linkedMixer(s, 7);
    expect(gc && a3 && a4 && trackmodes::outputOf(*a3) == gc->id && trackmodes::outputOf(*a4) == gc->id, "the copies play into the copied group");
    expect(a4 && a4->slots[0].plugin.type == "gl.eq" && a4->volume == 0.5f, "the member's device and fader are copied");
    ObjectId src3 = kNoId, src4 = kNoId;
    for (const Clip& k : s.playlist) {
      if (k.track == 6 && k.kind == ClipKind::Audio) src3 = k.source;
      if (k.track == 7 && k.kind == ClipKind::Audio) src4 = k.source;
    }
    const Channel* s3 = s.channel(src3);
    const Channel* s4 = s.channel(src4);
    expect(src3 != loop && src4 != lng && s3 && s4, "the copied clips play copies of their sources");
    expect(s3 && s4 && a3 && a4 && s3->mixerTrack == a3->id && s4->mixerTrack == a4->id, "through the copies' own mixer tracks");
    expect(s.channel(loop) && s.channel(loop)->mixerTrack == m3 && s.channel(lng)->mixerTrack == m4, "the originals are untouched");
  }
  // A copied instrument track: its own instrument and mixer track, its notes on the copy.
  const int ci = trackmodes::duplicateTracks(d, {0}, &why);
  {
    const Song& s = d.song();
    const Channel* oc = trackmodes::linkedChannel(s, 0);
    const Channel* cc = trackmodes::linkedChannel(s, ci);
    expect(ci == 1 && oc && cc && cc->id != oc->id && cc->plugin.type == "gl.synth", ("the copied MIDI track has an instrument of its own: " + why).c_str());
    expect(oc && cc && cc->mixerTrack != oc->mixerTrack && cc->mixerTrack != s.masterId(), "and a mixer track of its own");
    const Pattern* cp = nullptr;
    for (const Clip& k : s.playlist)
      if (k.track == ci && k.kind == ClipKind::Pattern) cp = s.pattern(k.pattern);
    expect(cp && cp->id != pat && cc && cp->notes.count(cc->id) && !cp->notes.count(syn), "its clip is a copy, its notes on the copied instrument");
  }
  d.undo();
  d.undo();
  expect(d.song() == before, "two undos: the set as before the copies");

  // N52-05: Delete of the group frees its bus, its tracks' mixer tracks and their clip sources.
  const ObjectId gid = trackmodes::linkedMixer(d.song(), 2)->id;
  expect(trackmodes::deleteTracks(d, {2}, &why) == 3, ("delete the group: " + why).c_str());
  {
    const Song& s = d.song();
    expect(joined(listed(s)) == "1 Synth,2 MIDI" || joined(listed(s)).find("Group") == std::string::npos, ("after delete: " + joined(listed(s))).c_str());
    expect(trackmodes::usedTrackCount(s) == 2, "two tracks shown, no grey padding tracks");
    expect(!s.channel(loop) && !s.channel(lng) && s.channels.size() == chans0 - 2, "the clip sources went with their tracks");
    bool into = false;
    for (const MixerTrack& m : s.mixer)
      for (const Send& sd : m.sends) into = into || sd.to == gid || sd.to == m3 || sd.to == m4;
    const MixerTrack* g = s.mixerTrack(gid);
    expect(!into && g && g->name.empty() && s.mixerTrack(m4)->slots[0].plugin.type.empty(), "the bus and the tracks' mixer tracks are free again");
    expect(s.patterns.size() == pats0, "patterns other tracks play stay");
  }
  d.undo();
  expect(d.song() == before, "one undo: the group is back");

  // N52-06: a rename takes the group's mixer track along (the members' Out names it).
  expect(trackmodes::renameTrack(d, 2, "Drums"), "rename the group");
  expect(trackmodes::linkedMixer(d.song(), 2)->name == "Drums", "its mixer track is renamed too");
  d.undo();

  // Checkpoint 53: a move made in any layout (no Clips call after it) keeps the routing: 1 MIDI's
  // instrument moved into the group plays into it, moved out plays into Main again.
  {
    const ObjectId im = d.song().channel(syn)->mixerTrack;
    expect(trackorder::movePlaylistTracks(d, {0}, 4), "move the instrument track into the group");
    const Song& s = d.song();
    const MixerTrack* g = trackmodes::linkedMixer(s, 1);
    expect(s.tracks[1].groupSize == 3 && g && trackmodes::outputOf(*s.mixerTrack(im)) == g->id, "moved in: it plays into the group");
    expect(trackorder::movePlaylistTracks(d, {3}, 0), "move it out again");
    expect(trackmodes::outputOf(*d.song().mixerTrack(im)) == d.song().masterId(), "moved out: into Main");
    d.undo();
    d.undo();
  }
  // N52-11: an instrument-named track takes its new place's number after a move.
  {
    const std::string nm = std::string("1 ") + findPlugin("gl.synth")->name;
    d.editPlaylistTrack(0, "name", [&](PlaylistTrack& t) { t.name = nm; });
    Song s = d.song();
    trackorder::reorderPlaylist(s, trackorder::playlistOrderAfterMove(s, {0}, 2));
    trackmodes::renumberTrackNames(s);
    expect(s.tracks[1].name == std::string("2 ") + findPlugin("gl.synth")->name, ("renumbered: " + s.tracks[1].name).c_str());
  }
  // N52-10: a lane for the group's fader; Ungroup takes the lane too. (Checkpoint 54, N53-03: it sits
  // right under the group's title, inside the group.)
  {
    Song s = d.song();
    const int lane = trackmodes::addAutomationLane(s, gid, "3 Group \xE2\x80\xBA Volume");
    expect(lane == 3 && s.tracks[2].groupSize == 3, "the lane right under the group's title, in the group");
    AutomationClip ac;
    ac.id = s.allocateId();
    ac.owner = gid;
    s.automations.push_back(ac);
    Clip k;
    k.id = s.allocateId();
    k.kind = ClipKind::Automation;
    k.source = ac.id;
    k.track = lane;
    k.length = 768;
    s.playlist.push_back(k);
    expect(trackmodes::isAutomationLane(s, lane) && !trackmodes::isAutomationLane(s, 2) && trackmodes::automationLanes(s, 6)[3], "it is an automation lane");
    expect(trackmodes::laneOwner(s, lane) == 2, "owned by the group");
    const size_t n = s.tracks.size();
    expect(trackmodes::ungroupTracks(s, 2, &why), "ungroup");
    expect(s.tracks.size() == n - 2, "the group and its emptied lane are gone");
  }
  // N52-02 / N52-08: a file dropped on an empty MIDI track makes it an audio track named as one, its
  // clip playing through it.
  {
    Song s = d.song();
    Channel c1;
    c1.id = s.allocateId();
    c1.plugin.type = "gl.sampler";
    c1.mixerTrack = s.masterId();
    s.channels.push_back(c1);
    Clip k;
    k.id = s.allocateId();
    k.kind = ClipKind::Audio;
    k.source = c1.id;
    k.track = 1;
    s.playlist.push_back(k);
    expect(trackmodes::makeNamedAudioTrack(s, 1), "the MIDI track becomes an audio track");
    const MixerTrack* m = trackmodes::linkedMixer(s, 1);
    expect(s.tracks[1].name == "2 Audio" && m && s.channel(c1.id)->mixerTrack == m->id, "named 2 Audio, its clip through its own strip");
  }
}

// Checkpoint 54 (K-300 round 10).
// An automation clip of `owner`'s control `index` on track `lane`.
void laneClip(Song& s, int lane, ObjectId owner, uint32_t index) {
  AutomationClip ac;
  ac.id = s.allocateId();
  ac.owner = owner;
  ac.index = index;
  s.automations.push_back(ac);
  Clip k;
  k.id = s.allocateId();
  k.kind = ClipKind::Automation;
  k.source = ac.id;
  k.track = lane;
  k.start = 0;
  k.length = 768;
  s.playlist.push_back(k);
}

void testCheckpoint54() {
  std::string why;
  Song s0 = makeEmptySong();
  s0.tracks.resize(kDefaultPlaylistTracks);
  trackmodes::makeClipsStarterTracks(s0);  // 1 MIDI, 2 MIDI, 3 Audio, 4 Audio
  // N53-03: a lane sits right under its track, takes no number, and the tracks after it keep counting on.
  {
    Song s = s0;
    const ObjectId m3 = trackmodes::linkedMixer(s, 2)->id;
    const int lane = trackmodes::addAutomationLane(s, m3, "3 Audio \xE2\x80\xBA Volume");
    laneClip(s, lane, m3, 0);
    expect(lane == 3 && trackmodes::laneOwner(s, 3) == 2, "the lane right under 3 Audio");
    trackmodes::renumberTrackNames(s);
    expect(s.tracks[4].name == "4 Audio", ("the track after the lane keeps its number: " + s.tracks[4].name).c_str());
    const std::vector<int> nums = trackmodes::trackNumbers(s, 5);
    expect(nums[2] == 3 && nums[3] == 0 && nums[4] == 4, "Clips' numbers: the lane has none");
    expect(trackmodes::withLanes(s, {2}) == std::vector<int>({2, 3}), "the track brings its lane along");
    expect(trackmodes::pastLanes(s, 3) == 4, "nothing drops between a track and its lane");
    // A move in Clips: the lane goes with its track (and both land as one block).
    Document d(s);
    std::vector<int> now;
    expect(trackorder::movePlaylistTracks(d, trackmodes::withLanes(d.song(), {2}), 0, &now), "move 3 Audio and its lane to the top");
    const Song& m = d.song();
    expect(trackmodes::laneOwner(m, 1) == 0 && m.tracks[0].mixer == m3, "the lane is still under its track");
    expect(m.tracks[0].name == "1 Audio" && m.tracks[2].name == "2 MIDI",
           ("renumbered by the move (any layout): " + m.tracks[0].name + ", " + m.tracks[2].name).c_str());
    // Duplicate: the copy brings a lane of its own, its automation on the copy's fader.
    Song x = d.song();
    const int c = trackmodes::duplicateTracks(x, {0}, &why);
    expect(c == 2, ("the copy after the track and its lane: " + why).c_str());
    const MixerTrack* cm = trackmodes::linkedMixer(x, 2);
    expect(cm && cm->id != m3 && trackmodes::isAutomationLane(x, 3) && trackmodes::laneOwner(x, 3) == 2, "the copy has its own lane, under it");
    ObjectId own = kNoId;
    for (const Clip& k : x.playlist)
      if (k.track == 3 && k.kind == ClipKind::Automation)
        if (const AutomationClip* ac = x.automation(k.source)) own = ac->owner;
    expect(cm && own == cm->id, "the copy's automation moves the copy's fader");
  }
  // A lane of a group's last track joins the group (folding hides it with its track).
  {
    Song s = s0;
    expect(trackmodes::groupTracks(s, {2, 3}, &why) == 2, ("group: " + why).c_str());
    const ObjectId m4 = trackmodes::linkedMixer(s, 4)->id;
    const int lane = trackmodes::addAutomationLane(s, m4, "4 Audio \xE2\x80\xBA Volume");
    laneClip(s, lane, m4, 0);
    expect(lane == 5 && s.tracks[2].groupSize == 3, "the last track's lane is in the group");
  }
  // N53-04: an empty unnamed track between tracks in use (Studio's) takes no row and no number.
  {
    Song s = s0;
    Document d(s);
    expect(trackorder::movePlaylistTracks(d, {3}, 7), "Studio: 4 Audio down to row 7");
    const Song& m = d.song();
    const std::vector<char> gaps = trackmodes::gapTracks(m, trackmodes::usedTrackCount(m));
    expect(gaps.size() == 7 && gaps[3] && gaps[4] && gaps[5] && !gaps[6] && !gaps[2], "rows 4 to 6 are gaps");
    expect(m.tracks[6].name == "4 Audio", ("numbered as Clips shows it: " + m.tracks[6].name).c_str());
    Song e = makeEmptySong();
    e.tracks.resize(kDefaultPlaylistTracks);
    expect(trackmodes::gapTracks(e, trackmodes::usedTrackCount(e)) == std::vector<char>(4, 0), "a new set's empty tracks are no gaps");
  }
  // N53-08: an instrument track's own mixer track sends to the returns.
  {
    Song s = s0;
    Document d(s);
    const ObjectId ch = trackmodes::newInstrumentTrack(d, 0, "gl.synth", &why);
    const ObjectId mix = d.song().channel(ch)->mixerTrack;
    expect(trackmodes::missingReturnSends(d.song(), mix), "a new instrument's mixer track has no return sends yet");
    Song x = d.song();
    trackmodes::addReturnSendsTo(x, mix);
    expect(!trackmodes::missingReturnSends(x, mix) && x.mixerTrack(mix)->sends.size() == 3, "then it sends to both returns (at zero) and Main");
  }
  // N53-02: a one-shot grid clip plays once: saved only when set, rendered as one pass.
  {
    Song s = s0;
    Channel src;
    src.id = s.allocateId();
    src.name = "Tick";
    src.plugin.type = "gl.sampler";
    src.audioClip = true;
    src.mixerTrack = trackmodes::linkedMixer(s, 2)->id;
    s.channels.push_back(src);
    Clip k;
    k.id = s.allocateId();
    k.kind = ClipKind::Audio;
    k.source = src.id;
    k.track = 2;
    k.start = 0;
    k.length = 19;
    k.launched = true;
    s.playlist.push_back(k);
    s.live.areaEnd = 1536;
    const std::string plain = projectToText(s);
    expect(plain.find("launchOnce") == std::string::npos, "not written when off");
    s.playlist.back().launchOnce = true;
    Song back;
    std::string err;
    const bool loaded = loadProjectText(projectToText(s), "", back, &err);
    bool once = false;
    for (const Clip& c : back.playlist) once = once || (c.id == k.id && c.launchOnce);
    expect(loaded && once, ("written and read back " + err).c_str());
    Song r = s;
    prepareOfflineLive(r);
    int passes = 0;
    for (const Clip& c : r.playlist) passes += c.kind == ClipKind::Audio ? 1 : 0;
    expect(passes == 1, ("a launched one-shot renders once: " + std::to_string(passes)).c_str());
    Song l = s;
    l.playlist.back().launchOnce = false;
    prepareOfflineLive(l);
    passes = 0;
    for (const Clip& c : l.playlist) passes += c.kind == ClipKind::Audio ? 1 : 0;
    expect(passes > 10, "a looping one repeats (as before)");
  }
  // N53-10: send level automation keeps its destination across mixer inserts, moves and deletes (its
  // parameter names the destination by mixer position: core/TrackOrder.h remaps it).
  {
    Song s = s0;
    const std::vector<size_t> rets = trackmodes::returnTracks(s);
    expect(rets.size() == 2, "two returns");
    const ObjectId src = trackmodes::linkedMixer(s, 2)->id, rB = s.mixer[rets[1]].id;
    AutomationClip ac;
    ac.id = s.allocateId();
    ac.owner = src;
    ac.index = sendParam(src, static_cast<int>(rets[1])).index;
    s.automations.push_back(ac);
    Document d(s);
    auto target = [&] { return static_cast<int>(d.song().automations.back().index - kSendBase); };
    mixops::insertTracks(d, static_cast<int>(rets[0]), 2);  // two strips before the returns, as the demo's FX Bus and Swells
    expect(d.song().mixerIndex(rB) == target(), "inserted before the returns: the send still moves B's send");
    trackorder::moveMixerTracks(d, {rB}, 1);
    expect(d.song().mixerIndex(rB) == target(), "moved: still B");
    d.removeMixerTrack(d.song().mixer[2].id);
    expect(d.song().mixerIndex(rB) == target(), "a track before it deleted: still B");
    d.undo();
    d.undo();
    d.undo();
    expect(projectToText(d.song()) == projectToText(s), "undone: the same bytes");
  }
}

// Checkpoint 55: N54-01 (looping short grid samples found, nothing changed), N54-02 (the warp tempo of
// a dropped file), N54-03 (names made from an owner's follow it, in the edit's own step; a song where
// nothing changed saves the same bytes), N54-04 (the number Clips shows).
void testCheckpoint55() {
  std::string why;
  Song s0 = makeEmptySong();
  s0.tracks.resize(kDefaultPlaylistTracks);
  trackmodes::makeClipsStarterTracks(s0);  // 1 MIDI, 2 MIDI, 3 Audio, 4 Audio
  // N54-02: the name first, then whole bars at the Set's tempo, then the detector nearest the Set's tempo.
  {
    expect(std::fabs(browse::warpTempo("Long 120.wav", 20.0, 120.0) - 120.0) < 1e-9, "Long 120 (20 s): 120 from its name");
    expect(std::fabs(browse::warpTempo("Long.wav", 20.0, 120.0) - 120.0) < 1e-9, "a 20 s file: 10 whole bars at the Set's 120");
    expect(std::fabs(browse::warpTempo("Loop 120.wav", 4.0, 120.0) - 120.0) < 1e-9, "Loop 120 (4 s): 120");
    expect(std::fabs(browse::warpTempo("Groove 96bpm.wav", 10.0, 120.0) - 96.0) < 1e-9, "a stated 96bpm wins over the Set's tempo");
    expect(std::fabs(browse::warpTempo("Pad 120.wav", 7.3, 120.0) - 120.0) > 1e-3, "a bare number the length disagrees with is no tempo (7.3 s)");
    expect(std::fabs(browse::warpTempo("Break.wav", 9.0, 120.0, 85.0) - 85.0) < 1e-9, "no name, no whole bars: the detector's 85");
    expect(std::fabs(browse::warpTempo("Break.wav", 9.0, 120.0, 60.0) - 120.0) < 1e-9, "the detector's half-time 60 read as 120 (nearest the Set)");
    expect(browse::warpTempo("Tick.wav", 0.1, 120.0) == 0.0, "a 0.1 s tick: no tempo (a one-shot)");
    expect(std::fabs(browse::warpTempo("Kick 808.wav", 0.4, 120.0) - 0.0) < 1e-9, "Kick 808: 808 is no tempo");
  }
  // N54-01: a grid clip under a beat that loops (an older set's) is found; a one-shot, a warped clip
  // or an arrangement clip is not; finding changes nothing.
  {
    Song s = s0;
    s.live.areaEnd = 1536;
    auto audio = [&](Tick start, Tick len, bool once, double sync) {
      Clip c;
      c.id = s.allocateId();
      c.kind = ClipKind::Audio;
      c.track = 2;
      c.start = start;
      c.length = len;
      c.launchOnce = once;
      c.syncBpm = sync;
      s.playlist.push_back(c);
      return c.id;
    };
    const ObjectId old = audio(0, 19, false, 0);
    audio(384, 19, true, 0);       // a one-shot already
    audio(768, 3072, false, 120);  // a warped loop
    audio(2000, 19, false, 0);     // in the arrangement
    const std::string before = projectToText(s);
    const std::vector<ObjectId> found = trackmodes::loopingShortSlotClips(s);
    expect(found == std::vector<ObjectId>({old}), "only the older set's looping short grid sample");
    expect(projectToText(s) == before, "looking changes nothing (the save stays as it was)");
  }
  // N54-03: duplicate and move: the lane and its automation take the owner's new name, in the same step.
  {
    Song s = s0;
    expect(trackmodes::groupTracks(s, {2, 3}, &why) == 2, ("group 3 Audio and 4 Audio: " + why).c_str());
    const ObjectId gm = s.tracks[2].mixer;
    const std::string gname = s.tracks[2].name;
    const std::string label = paramDisplayName(s, ParamId{gm, 0});
    const int lane = trackmodes::addAutomationLane(s, gm, label);
    laneClip(s, lane, gm, 0);
    s.automations.back().name = label;
    Document d(s);
    const std::string base = projectToText(d.song());
    const int c = trackmodes::duplicateTracks(d, {2}, &why);
    expect(c > 2, ("duplicate the group: " + why).c_str());
    const Song& x = d.song();
    const std::string copyName = x.tracks[static_cast<size_t>(c)].name;
    int copyLane = -1;
    for (int t = c + 1; t < static_cast<int>(x.tracks.size()) && copyLane < 0; ++t) {
      if (trackmodes::isAutomationLane(x, t) && trackmodes::laneOwner(x, t) == c) {
        copyLane = t;
      }
    }
    expect(copyLane > c, "the copy has its lane");
    std::string copyAuto;
    for (const Clip& k : x.playlist) {
      if (k.track == copyLane && k.kind == ClipKind::Automation) {
        if (const AutomationClip* ac = x.automation(k.source)) {
          copyAuto = ac->name;
        }
      }
    }
    expect(copyName != gname, ("the copy is named for its place: " + copyName).c_str());
    expect(copyAuto.rfind(copyName + " \xE2\x80\xBA ", 0) == 0, ("the copy's automation names the copy: " + copyAuto).c_str());
    expect(x.tracks[static_cast<size_t>(copyLane)].name == copyAuto, ("the copy's lane too: " + x.tracks[static_cast<size_t>(copyLane)].name).c_str());
    expect(x.automations.front().name == label, "the original's automation keeps its name");
    d.undo();
    expect(projectToText(d.song()) == base, "one undo: the same bytes as before the duplicate");
    // Move the group to the top: it is renumbered, its lane and automation follow.
    std::vector<int> now;
    expect(trackorder::movePlaylistTracks(d, trackmodes::withLanes(d.song(), {2}), 0, &now), "move the group to the top");
    const Song& m = d.song();
    expect(m.tracks[0].name != gname, ("renumbered: " + m.tracks[0].name).c_str());
    std::string movedAuto;
    for (const AutomationClip& ac : m.automations) {
      movedAuto = ac.name;
    }
    expect(movedAuto.rfind(m.tracks[0].name + " \xE2\x80\xBA ", 0) == 0, ("the automation follows the move: " + movedAuto).c_str());
    int movedLane = -1;
    for (int t = 0; t < trackmodes::usedTrackCount(m) && movedLane < 0; ++t) {
      if (trackmodes::isAutomationLane(m, t)) {
        movedLane = t;
      }
    }
    expect(movedLane >= 0 && m.tracks[static_cast<size_t>(movedLane)].name == movedAuto, "and its lane");
    d.undo();
    expect(projectToText(d.song()) == base, "one undo: the same bytes as before the move");
    // A rename that leaves every name as it was changes nothing.
    Song y = d.song();
    expect(trackmodes::followAutomationNames(y) == 0 && projectToText(y) == base, "nothing to follow: the same bytes");
  }
  // N54-03: a take named after its track ("3 Audio rec") follows the track's rename.
  {
    Song s = s0;
    const ObjectId src = s.allocateId();
    Channel ch;
    ch.id = src;
    ch.name = "3 Audio rec";
    ch.plugin.type = "gl.sampler";
    ch.mixerTrack = s.tracks[2].mixer;
    s.channels.push_back(ch);
    Clip k;
    k.id = s.allocateId();
    k.kind = ClipKind::Audio;
    k.source = src;
    k.track = 2;
    k.start = 2000;
    k.length = 768;
    s.playlist.push_back(k);
    Document d(s);
    expect(trackmodes::renameTrack(d, 2, "Vox"), "rename 3 Audio");
    expect(d.song().channel(src)->name == "Vox rec", ("the take follows: " + d.song().channel(src)->name).c_str());
    d.undo();
    expect(d.song().channel(src)->name == "3 Audio rec" && d.song().tracks[2].name == "3 Audio", "one undo puts both back");
  }
  // N54-04: the number Clips shows a track by, after another layout left gaps.
  {
    Song s = s0;
    Document d(s);
    expect(trackorder::movePlaylistTracks(d, {3}, 7), "Studio: 4 Audio down to row 7");
    expect(trackmodes::shownNumber(d.song(), 6) == 4, ("row 7 shows as 4: " + std::to_string(trackmodes::shownNumber(d.song(), 6))).c_str());
    expect(trackmodes::shownNumber(d.song(), 0) == 1, "row 1 is 1");
  }
}

// Checkpoint 56: the tester's round 12.
void testCheckpoint56() {
  Song s0 = makeEmptySong();
  s0.tracks.resize(kDefaultPlaylistTracks);
  trackmodes::makeClipsStarterTracks(s0);  // 1 MIDI, 2 MIDI, 3 Audio, 4 Audio; returns A Reverb, B Delay
  // N55-01: a track's To set to a return keeps its send to that return (and its level); undo and a save
  // round trip keep it too; the return stays a return.
  {
    Song s = s0;
    const std::vector<size_t> rets = trackmodes::returnTracks(s);
    expect(rets.size() == 2, "the starter Set has two returns");
    const ObjectId A = s.mixer[rets[0]].id;
    const ObjectId from = s.tracks[2].mixer;
    MixerTrack* m = s.mixerTrack(from);
    expect(m && trackmodes::outputOf(*m) == s.masterId(), "3 Audio plays into Main");
    const int k = m ? trackmodes::auxSendIndex(*m, A) : -1;
    expect(k >= 0, "3 Audio sends to A");
    if (m && k >= 0) {
      m->sends[static_cast<size_t>(k)].level = 0.4f;
    }
    Document d(s);
    const std::string before = projectToText(d.song());
    d.editSong("track output", [&](Song& x) { trackmodes::setOutput(x, from, A); });
    const MixerTrack* m2 = d.song().mixerTrack(from);
    expect(m2 && trackmodes::outputOf(*m2) == A, "To: A Reverb");
    // Checkpoint 57 (N56-01): kept aside (Song::keptSends), not in the playing sends.
    const int k2 = m2 ? trackmodes::auxSendIndex(*m2, A) : -1;
    const KeptSend* kept = trackmodes::keptSend(d.song(), from, A);
    expect(k2 < 0 && kept && std::fabs(kept->level - 0.4f) < 1e-6f, "the send to A and its 0.4 are kept aside, not playing");
    int toA = 0;
    for (const Send& sd : m2 ? m2->sends : std::vector<Send>{}) {
      toA += sd.to == A ? 1 : 0;
    }
    expect(toA == 1, "one route to A: the output only");
    expect(trackmodes::returnTracks(d.song()).size() == 2, "A is still a return");
    expect(!trackmodes::missingReturnSends(d.song(), from), "no second send to A is added");
    Song back;
    std::string err;
    const std::string saved = projectToText(d.song());
    expect(loadProjectText(saved, "", back, &err) && projectToText(back) == saved, "the save round trip keeps the send (byte for byte)");
    d.editSong("track output", [&](Song& x) { trackmodes::setOutput(x, from, x.masterId()); });
    const MixerTrack* m3 = d.song().mixerTrack(from);
    const int k3 = m3 ? trackmodes::auxSendIndex(*m3, A) : -1;
    expect(m3 && trackmodes::outputOf(*m3) == d.song().masterId() && k3 >= 0 && std::fabs(m3->sends[static_cast<size_t>(k3)].level - 0.4f) < 1e-6f,
           "To: Main again: the send to A plays at its 0.4 again");
    d.undo();
    d.undo();
    expect(projectToText(d.song()) == before, "undo restores the Set as it was");
  }
  // N55-03: a file with no tempo in its name: whole bars nearest the Set's tempo, only close to it.
  {
    expect(browse::warpTempo("Odd.wav", 3.3, 120.0) == 0.0, "3.3 s at 120: no tempo (145.45 is no reading)");
    const double four124 = 4 * 4 * 60.0 / 124.0;  // four bars at 124
    expect(std::fabs(browse::warpTempo("Groove.wav", four124, 120.0) - 124.0) < 1e-6, "four bars at 124 in a 120 Set: 124");
    expect(std::fabs(browse::warpTempo("Long.wav", 16.0, 120.0) - 120.0) < 1e-9, "16 s at 120: eight bars, 120");
    expect(browse::warpTempo("Pad.wav", 7.3, 120.0) == 0.0, "7.3 s at 120: no tempo");
  }
  // N55-08: the Session export plays the chosen clips together for the longest one's length: a loop
  // repeats, a one-shot plays once; the other slots and the Arrangement go.
  {
    Song s = s0;
    s.live.areaEnd = 4 * 1536;
    auto clip = [&](int track, Tick start, Tick len, bool once) {
      Clip c;
      c.id = s.allocateId();
      c.kind = ClipKind::Audio;
      c.track = track;
      c.start = start;
      c.length = len;
      c.launchOnce = once;
      s.playlist.push_back(c);
      return c.id;
    };
    const ObjectId loop = clip(2, 0, 384, false);   // a beat that loops (scene 1)
    const ObjectId once = clip(3, 0, 1536, true);   // a bar that plays once
    clip(2, 1536, 1536, false);                     // scene 2: not chosen
    clip(3, 5 * 1536, 1536, false);                 // in the Arrangement: not chosen
    Song r = s;
    const Tick len = prepareSessionExport(r, {loop, once});
    expect(len == 1536, "as long as the longest chosen clip (a bar)");
    int loops = 0, onces = 0;
    bool placed = true;
    for (const Clip& c : r.playlist) {
      placed = placed && c.start >= s.live.areaEnd && c.start < s.live.areaEnd + len;
      loops += c.track == 2 ? 1 : 0;
      onces += c.track == 3 ? 1 : 0;
    }
    expect(placed && loops == 4 && onces == 1, "the beat four times, the one-shot once, from the Arrangement's start");
    expect(r.live.areaEnd == s.live.areaEnd, "the live area stays (empty): the render starts at its end");
    Song none = s;
    expect(prepareSessionExport(none, {}) == 0 && projectToText(none) == projectToText(s), "nothing chosen: the song unchanged");
  }
  // N55-12: a Set whose automation lane carries a stale name ("<old owner> > control") opens and saves
  // byte for byte: names are derived when drawn, never rewritten on load.
  {
    Song s = s0;
    const ObjectId gm = s.tracks[2].mixer;
    const int lane = trackmodes::addAutomationLane(s, gm, "Old name \xE2\x80\xBA Volume");
    laneClip(s, lane, gm, 0);
    s.automations.back().name = "Old name \xE2\x80\xBA Volume";
    const std::string text = projectToText(s);
    Song back;
    std::string err;
    expect(loadProjectText(text, "", back, &err), "the stale Set opens");
    expect(projectToText(back) == text, "an untouched open and save is byte for byte the same");
    expect(back.tracks[static_cast<size_t>(lane)].name == "Old name \xE2\x80\xBA Volume", "the lane keeps its stored name");
  }
}

// Checkpoint 57: the tester's round 13.
// A steady 0.5 sample on a sampler channel into insert 1 (output Main); insert 2 is a plain bus to Main.
Song sendSong(const std::vector<Send>& sends) {
  static std::string dc;
  if (dc.empty()) {
    AudioBuffer b(2, 44100);
    for (size_t i = 0; i < b.frames; ++i) {
      b.at(0, i) = b.at(1, i) = 0.5f;
    }
    dc = (std::filesystem::temp_directory_path() / "gl_k112c_cp57_dc.wav").string();
    writeWav(dc, b, 44100, 16);
    sampleCache().clear();
  }
  Song s = makeEmptySong();
  Channel ch;
  ch.id = s.allocateId();
  ch.name = "Audio";
  ch.plugin = defaultPluginState("gl.sampler");
  ch.plugin.data = dc;
  ch.mixerTrack = s.mixer[1].id;
  s.channels.push_back(ch);
  Clip c;
  c.id = s.allocateId();
  c.kind = ClipKind::Audio;
  c.source = ch.id;
  c.start = 0;
  c.length = 192;
  s.playlist.push_back(c);
  s.mixer[1].sends = sends;
  return s;
}

float maxDiff(const AudioBuffer& x, const AudioBuffer& y) {
  if (x.frames != y.frames || x.data.size() != y.data.size()) {
    return 1e9f;
  }
  float d = 0;
  for (size_t i = 0; i < x.data.size(); ++i) {
    d = std::max(d, std::fabs(x.data[i] - y.data[i]));
  }
  return d;
}

void testCheckpoint57() {
  // N56-01: every send plays, as on main: two sends to one destination add up (a hand-made song, a
  // DAWproject import); the engine no longer drops the second.
  {
    const Song probe = makeEmptySong();
    const ObjectId M = probe.masterId(), R = probe.mixer[2].id;
    RenderOptions o;
    const AudioBuffer two = renderSong(sendSong({{M, 1.0f}, {R, 0.5f}, {R, 0.3f}}), o);
    const AudioBuffer sum = renderSong(sendSong({{M, 1.0f}, {R, 0.8f}}), o);
    const AudioBuffer first = renderSong(sendSong({{M, 1.0f}, {R, 0.5f}}), o);
    expect(two.frames > 0 && two.peak() > 0.1f, "the send song sounds");
    expect(maxDiff(two, sum) < 1e-5f, ("sends of 0.5 and 0.3 to one bus play as 0.8: " + std::to_string(maxDiff(two, sum))).c_str());
    expect(maxDiff(two, first) > 0.05f, "the second send to the bus is heard (not dropped)");
    const AudioBuffer outDup = renderSong(sendSong({{R, 1.0f}, {R, 0.5f}}), o);
    const AudioBuffer outOne = renderSong(sendSong({{R, 1.0f}}), o);
    expect(maxDiff(outDup, outOne) > 0.05f, "an output plus a plain send to the same bus: both routes play");
  }
  Song s0 = makeEmptySong();
  s0.tracks.resize(kDefaultPlaylistTracks);
  trackmodes::makeClipsStarterTracks(s0);  // returns A Reverb, B Delay
  const std::vector<size_t> rets = trackmodes::returnTracks(s0);
  const ObjectId A = s0.mixer[rets[0]].id, B = s0.mixer[rets[1]].id;
  const ObjectId from = s0.tracks[2].mixer;
  if (MixerTrack* m = s0.mixerTrack(from); m && trackmodes::auxSendIndex(*m, A) >= 0) {
    m->sends[static_cast<size_t>(trackmodes::auxSendIndex(*m, A))].level = 0.4f;
  }
  // N56-01: To a return keeps the send aside (not in the playing sends); To back to Main, or on to the
  // other return, puts it back where it was, at its level: byte for byte.
  {
    Document d(s0);
    const std::string before = projectToText(d.song());
    d.editSong("track output", [&](Song& x) { trackmodes::setOutput(x, from, A); });
    const MixerTrack* m = d.song().mixerTrack(from);
    expect(m && trackmodes::outputOf(*m) == A && trackmodes::auxSendIndex(*m, A) < 0, "To A: A is the output, no send to A plays");
    expect(d.song().keptSends.size() == 1 && trackmodes::keptSend(d.song(), from, A) &&
               std::fabs(trackmodes::keptSend(d.song(), from, A)->level - 0.4f) < 1e-6f,
           "the send to A is kept aside at 0.4");
    expect(projectToText(d.song()).find("\"keptSends\"") != std::string::npos, "the kept send is saved (live.keptSends)");
    Song back;
    std::string err;
    const std::string saved = projectToText(d.song());
    expect(loadProjectText(saved, "", back, &err) && back.keptSends == d.song().keptSends && projectToText(back) == saved,
           "the kept send survives a save round trip, byte for byte");
    d.editSong("track output", [&](Song& x) { trackmodes::setOutput(x, from, B); });
    const MixerTrack* m2 = d.song().mixerTrack(from);
    expect(m2 && trackmodes::outputOf(*m2) == B && trackmodes::auxSendIndex(*m2, A) >= 0 && trackmodes::keptSend(d.song(), from, B) &&
               !trackmodes::keptSend(d.song(), from, A),
           "To B: the send to A plays again, the send to B steps aside");
    d.editSong("track output", [&](Song& x) { trackmodes::setOutput(x, from, x.masterId()); });
    expect(projectToText(d.song()) == before && d.song().keptSends.empty(), "To Main: the sends are back where they were, at their levels (byte for byte)");
    d.undo();
    d.undo();
    d.undo();
    expect(projectToText(d.song()) == before, "undo restores the Set");
  }
  // An older Set (no keptSends) saves byte for byte, with no "keptSends" written; a song with a second
  // route to one bus keeps it untouched.
  {
    Song s = sendSong({{s0.masterId(), 1.0f}, {s0.mixer[2].id, 0.5f}, {s0.mixer[2].id, 0.3f}});
    const std::string text = projectToText(s);
    Song back;
    std::string err;
    expect(loadProjectText(text, "", back, &err) && projectToText(back) == text, "an older song: open and save byte for byte");
    expect(text.find("keptSends") == std::string::npos && back.keptSends.empty(), "nothing new is written");
    expect(back.mixer[1].sends.size() == 3, "both sends to the bus stay");
  }
  // Collaboration: the kept send reaches the other person with the To change.
  {
    Clock clock;
    Net net(57);
    Peer a, b;
    a.label = "Alex";
    b.label = "Sam";
    a.id = crypto::Identity::generate();
    b.id = crypto::Identity::generate();
    a.doc.reset(s0);
    a.folder = tmpDir("k112c_cp57_a");
    b.folder = tmpDir("k112c_cp57_b");
    std::string err;
    a.s = Session::create(a.doc, config(a.folder, a.id, a.label, &clock), &err);
    net.add(&a);
    b.s = Session::join(b.doc, config(b.folder, b.id, b.label, &clock), ticketOf(*a.s), &err);
    a.s->admitMember(memberOf(b));
    net.add(&b);
    net.connect(&a, &b);
    settle(net, {&a, &b}, clock);
    a.doc.editSong("track output", [&](Song& x) { trackmodes::setOutput(x, from, A); });
    settle(net, {&a, &b}, clock);
    const KeptSend* kb = trackmodes::keptSend(b.doc.song(), from, A);
    expect(kb && std::fabs(kb->level - 0.4f) < 1e-6f, "collab: the kept send reaches the other person");
    expect(songText(a) == songText(b), "collab: both songs are the same");
    b.doc.editSong("track output", [&](Song& x) { trackmodes::setOutput(x, from, x.masterId()); });
    settle(net, {&a, &b}, clock);
    const MixerTrack* ma = a.doc.song().mixerTrack(from);
    expect(ma && trackmodes::auxSendIndex(*ma, A) >= 0 && a.doc.song().keptSends.empty() && songText(a) == songText(b),
           "collab: To Main from the other side puts the send back on both");
  }
  // N56-03: from the length alone, whole bars at a likely whole tempo, the one nearest the Set's.
  {
    expect(std::fabs(browse::warpTempo("Beat.wav", 4 * 4 * 60.0 / 90.0, 120.0) - 90.0) < 1e-6, "Beat.wav, 4 bars at 90 in a 120 Set: 90");
    expect(browse::warpTempo("Odd.wav", 3.3, 120.0) == 0.0, "Odd.wav, 3.3 s: still no tempo");
    expect(std::fabs(browse::warpTempo("Two.wav", 2 * 4 * 60.0 / 100.0, 120.0) - 100.0) < 1e-6, "2 bars at 100 in a 120 Set: 100");
    expect(std::fabs(browse::warpTempo("Fast.wav", 8 * 4 * 60.0 / 174.0, 120.0) - 87.0) < 1e-6, "8 bars at 174 in a 120 Set: 4 bars at 87, the nearer whole tempo");
    expect(std::fabs(browse::warpTempo("Fast.wav", 8 * 4 * 60.0 / 174.0, 160.0) - 174.0) < 1e-6, "the same file in a 160 Set: 174");
    expect(std::fabs(browse::warpTempo("Slow.wav", 4 * 4 * 60.0 / 90.0, 170.0) - 180.0) < 1e-6, "the same 4 bars at 90 in a 170 Set: 8 bars at 180, nearer");
    expect(browse::warpTempo("Pad.wav", 7.3, 120.0) == 0.0, "7.3 s: still no tempo");
  }
}

}  // namespace

int main() {
  testCount();
  testEdits();
  testStarter();
  testNewTracks();
  testDemoGrid();
  testClipGrid();
  testPlayedMixer();
  testBusesAndReturns();
  testInsertAfter();
  testLiveArea();
  testCollab();
  testGroups();
  testNestedGroups();
  testGroupEdits();
  testCheckpoint53();
  testCheckpoint54();
  testCheckpoint55();
  testCheckpoint56();
  testCheckpoint57();
  return finish();
}
