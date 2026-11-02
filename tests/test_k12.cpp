// Gridline: K-12, the edits behind the playlist track "+" (core/TrackPlus).
//  * pattern names after the instrument ("Kick", "Kick 2", ...), a free spot on the track, the place tick;
//  * a new pattern on an instrument track: named and coloured after its instrument, one clip on the track at
//    the free spot, one undo step; refused (nothing recorded) on an ordinary track;
//  * an instrument (a rack channel or a new one) given to an empty track starts a first pattern there in the
//    same undo step; to a track with clips, or with the start switch off, no pattern; a refused assignment
//    (a track holding audio clips) changes nothing and records nothing.
#include <string>
#include "Check.h"
#include "core/Document.h"
#include "core/TrackModes.h"
#include "core/TrackPlus.h"

using namespace gl;
namespace tmo = gl::trackmodes;
namespace tp = gl::trackplus;

int main() {
  // ---- names, spots, the place tick
  {
    Document d(makeEmptySong());
    d.setPlaylistTrackCount(8);
    const Tick bar = d.song().barTicks();
    expect(tp::patternName(d.song(), "Kick") == "Kick", "name: the instrument's name when free");
    d.addPattern("Kick");
    d.addPattern("Kick 2");
    expect(tp::patternName(d.song(), "Kick") == "Kick 3", "name: numbered past the ones in use");
    expect(tp::patternName(d.song(), "").rfind("Pattern ", 0) == 0, "name: no instrument name: Pattern N");
    const ObjectId p = d.song().patterns.front().id;
    expect(tp::freeSpot(d.song(), 1, 2 * bar, bar) == 2 * bar, "spot: an empty track: where asked");
    d.addClip(p, 1, 2 * bar, bar);
    d.addClip(p, 1, 3 * bar, 2 * bar);
    expect(tp::freeSpot(d.song(), 1, 2 * bar, bar) == 5 * bar, "spot: past the clips in the way");
    expect(tp::freeSpot(d.song(), 1, 0, bar) == 0, "spot: free before them");
    expect(tp::freeSpot(d.song(), 1, bar + bar / 2, bar) == 5 * bar, "spot: a partly covered span moves on (to a bar)");
    expect(tp::freeSpot(d.song(), 2, 2 * bar, bar) == 2 * bar, "spot: other tracks do not count");
    expect(tp::placeTick(d.song(), 3 * bar + 5, 0, 10 * bar) == 3 * bar, "place: the song position in view, down to its bar");
    expect(tp::placeTick(d.song(), 20 * bar, 4 * bar + 1, 10 * bar) == 5 * bar, "place: out of view: the view's start, up to a bar");
    expect(tp::placeTick(d.song(), -1, 0, 10 * bar) == 0, "place: no song position: the view's start");
  }
  // ---- a new pattern on an instrument track
  {
    Document d(makeEmptySong());
    d.setPlaylistTrackCount(8);
    std::string why;
    const ObjectId ch = tmo::newInstrumentTrack(d, 3, "gl.synth", &why);
    expect(ch != kNoId, "pattern: an instrument track to start from");
    const size_t u0 = d.history().position(), n0 = d.song().patterns.size(), c0 = d.song().playlist.size();
    const Tick bar = d.song().barTicks();
    const tp::Started st = tp::newPatternOnTrack(d, 3, 2 * bar, &why);
    const Song& s = d.song();
    expect(st.pattern != kNoId && st.clip != kNoId && st.channel == ch, "pattern: a pattern and a clip for the track's channel");
    expect(s.patterns.size() == n0 + 1 && s.playlist.size() == c0 + 1, "pattern: ... one of each");
    expect(s.pattern(st.pattern)->name == s.channel(ch)->name && s.pattern(st.pattern)->color == s.channel(ch)->color,
           "pattern: named and coloured after the instrument");
    expect(s.clip(st.clip)->track == 3 && s.clip(st.clip)->start == 2 * bar && s.clip(st.clip)->pattern == st.pattern,
           "pattern: the clip on the track at the asked bar");
    expect(d.history().position() == u0 + 1, "pattern: one undo step");
    const tp::Started st2 = tp::newPatternOnTrack(d, 3, 2 * bar, &why);
    expect(st2.clip != kNoId && d.song().clip(st2.clip)->start == 3 * bar, "pattern: a second one goes after the first");
    expect(d.song().pattern(st2.pattern)->name == d.song().channel(ch)->name + " 2", "pattern: ... numbered");
    expect(d.undo() && d.undo() && d.song().patterns.size() == n0 && d.song().playlist.size() == c0, "pattern: undo takes each away in one step");
    const size_t u1 = d.history().position();
    expect(tp::newPatternOnTrack(d, 0, 0, &why).clip == kNoId && !why.empty(), "pattern: refused on an ordinary track (with a reason)");
    expect(d.history().position() == u1 && d.song().patterns.size() == n0, "pattern: ... nothing recorded");
  }
  // ---- an instrument for an empty track starts a pattern (one step)
  {
    Document d(makeEmptySong());
    d.setPlaylistTrackCount(8);
    std::string why;
    const size_t u0 = d.history().position(), ch0 = d.song().channels.size(), n0 = d.song().patterns.size();
    const Tick bar = d.song().barTicks();
    const tp::Started st = tp::assignNewInstrument(d, 4, "gl.kick", true, bar, &why);
    expect(st.channel != kNoId && st.pattern != kNoId && st.clip != kNoId, "new: channel, pattern and clip");
    expect(tmo::modeOf(d.song(), 4) == TrackMode::Instrument && tmo::linkedChannel(d.song(), 4)->id == st.channel, "new: the track plays it");
    expect(d.song().clip(st.clip)->track == 4 && d.song().clip(st.clip)->start == bar, "new: the first pattern on the track");
    expect(d.history().position() == u0 + 1, "new: one undo step");
    expect(d.undo() && d.song().channels.size() == ch0 && d.song().patterns.size() == n0 && d.song().playlist.empty() &&
               tmo::modeOf(d.song(), 4) == TrackMode::Unassigned,
           "new: undo restores everything");
    // switch off: the instrument only
    const tp::Started off = tp::assignNewInstrument(d, 4, "gl.kick", false, bar, &why);
    expect(off.channel != kNoId && off.pattern == kNoId && d.song().patterns.size() == n0, "new: start switch off: no pattern");
    // a rack channel: the existing channel, a first pattern
    const ObjectId rack = d.addChannel("Bass", "gl.synth");
    const size_t u1 = d.history().position();
    const tp::Started r = tp::assignChannel(d, 5, rack, true, 0, &why);
    expect(r.channel == rack && r.clip != kNoId && tmo::linkedChannel(d.song(), 5)->id == rack, "rack: the track plays the channel");
    expect(d.song().pattern(r.pattern)->name == "Bass", "rack: the pattern is named after it");
    expect(d.history().position() == u1 + 1, "rack: one undo step");
    // a track with clips already: no new pattern
    const ObjectId p = d.song().patterns.front().id;
    d.addClip(p, 6, 0);
    const size_t np = d.song().patterns.size();
    const ObjectId rack2 = d.addChannel("Keys", "gl.synth");
    const tp::Started k = tp::assignChannel(d, 6, rack2, true, 0, &why);
    expect(k.channel == rack2 && k.pattern == kNoId && d.song().patterns.size() == np, "rack: a track with clips gets no new pattern");
  }
  // ---- refused: a track holding audio clips
  {
    Document d(makeEmptySong());
    d.setPlaylistTrackCount(8);
    d.editSong("audio clip", [](Song& x) {
      Clip c;
      c.id = x.nextId++;
      c.kind = ClipKind::Audio;
      c.track = 2;
      c.start = 0;
      c.length = 96;
      x.playlist.push_back(c);
    });
    const size_t u0 = d.history().position(), ch0 = d.song().channels.size();
    std::string why;
    const tp::Started st = tp::assignNewInstrument(d, 2, "gl.synth", true, 0, &why);
    expect(st.channel == kNoId && !why.empty(), "refused: an audio track's clips (with a reason)");
    expect(d.history().position() == u0 && d.song().channels.size() == ch0 && tmo::modeOf(d.song(), 2) == TrackMode::Unassigned,
           "refused: nothing changed, nothing recorded");
  }
  return finish();
}
