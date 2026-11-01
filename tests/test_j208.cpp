// Gridline: J-208, the edits behind the playlist picker's "+" and a plugin dropped on a playlist track.
//  * "+" makes a pattern exactly as the New pattern command: the next free "Pattern N" name, one undo step;
//  * an instrument dropped on a track that holds pattern clips makes it an instrument track playing a
//    new channel of it: the clips stay, the track takes the channel's name and colour when it has none
//    (a named track keeps its name), the channel gets a mixer track of its own, one undo step;
//  * a track holding audio clips is refused and nothing changes (no undo step);
//  * dropped on an instrument track, an instrument replaces the channel's plugin: the track keeps its link.
#include <string>
#include "Check.h"
#include "core/Document.h"
#include "core/MenuOps.h"
#include "core/PluginInfo.h"
#include "core/TrackModes.h"

using namespace gl;
namespace tmo = gl::trackmodes;

int main() {
  // ---- "+": the next pattern, named as the New pattern command names it
  {
    Document d(makeEmptySong());
    const size_t n0 = d.song().patterns.size();
    const size_t u0 = d.history().position();
    const std::string name = ops::nextPatternName(d.song());
    const ObjectId id = d.addPattern(name);
    expect(id != kNoId && d.song().patterns.size() == n0 + 1 && d.song().patterns.back().id == id, "plus: a new pattern after the last");
    expect(d.song().pattern(id)->name == "Pattern " + std::to_string(n0 + 1), "plus: named Pattern N+1");
    expect(d.history().position() == u0 + 1, "plus: one undo step");
    d.renamePattern(id, "Pattern 9");
    expect(ops::nextPatternName(d.song()) == "Pattern " + std::to_string(n0 + 2), "plus: names skip the ones in use");
    expect(d.undo() && d.undo() && d.song().patterns.size() == n0, "plus: undo takes it away");
  }
  // ---- an instrument dropped on a track with pattern clips
  {
    Document d(makeEmptySong());
    if (d.song().tracks.size() < 4) d.setPlaylistTrackCount(8);
    const ObjectId pat = d.song().patterns.empty() ? d.addPattern("Pattern 1") : d.song().patterns.front().id;
    const ObjectId clip = d.addClip(pat, 2, 0);
    expect(clip != kNoId, "drop: a pattern clip on track 3");
    const size_t ch0 = d.song().channels.size(), u0 = d.history().position();
    std::string why;
    const ObjectId ch = tmo::newInstrumentTrack(d, 2, "gl.synth", &why);
    const Song& s = d.song();
    expect(ch != kNoId && tmo::modeOf(s, 2) == TrackMode::Instrument && tmo::linkedChannel(s, 2)->id == ch, "drop: the track plays a new channel");
    expect(s.channels.size() == ch0 + 1 && s.channel(ch)->plugin.type == "gl.synth", "drop: ... of the dropped instrument");
    expect(s.clip(clip) && s.clip(clip)->track == 2, "drop: ... the pattern clips stay");
    expect(s.tracks[2].name == s.channel(ch)->name && s.tracks[2].color == s.channel(ch)->color, "drop: ... named and coloured after it");
    expect(tmo::linkedMixer(s, 2) && tmo::linkedMixer(s, 2)->id != s.masterId(), "drop: ... on a mixer track of its own");
    expect(d.history().position() == u0 + 1, "drop: one undo step");
    expect(d.undo() && d.song().channels.size() == ch0 && tmo::modeOf(d.song(), 2) == TrackMode::Unassigned, "drop: undo restores it");
    // a named track keeps its name
    d.editSong("name", [](Song& x) { x.tracks[2].name = "Lead"; });
    expect(tmo::newInstrumentTrack(d, 2, "gl.synth", &why) != kNoId && d.song().tracks[2].name == "Lead", "drop: a named track keeps its name");
    // replace: the linked channel's plugin changes, the link stays
    const ObjectId linked = tmo::linkedChannel(d.song(), 2)->id;
    d.editChannel(linked, "replace instrument track", [](Channel& c) { c.plugin = defaultPluginState("gl.kick"); });
    expect(tmo::modeOf(d.song(), 2) == TrackMode::Instrument && tmo::linkedChannel(d.song(), 2)->id == linked &&
               tmo::linkedChannel(d.song(), 2)->plugin.type == "gl.kick",
           "replace: same channel, new instrument, still the track's");
  }
  // ---- a track holding audio clips is refused
  {
    Document d(makeEmptySong());
    if (d.song().tracks.size() < 4) d.setPlaylistTrackCount(8);
    const ObjectId src = d.addChannel("Take", "gl.sampler");
    d.editSong("audio clip", [&](Song& x) {
      Clip c;
      c.id = x.allocateId();
      c.kind = ClipKind::Audio;
      c.source = src;
      c.track = 1;
      c.length = 96;
      x.playlist.push_back(c);
    });
    const Song before = d.song();
    const size_t u0 = d.history().position();
    std::string why;
    expect(tmo::newInstrumentTrack(d, 1, "gl.synth", &why) == kNoId && !why.empty(), "refused: a track with audio clips");
    expect(d.song().channels.size() == before.channels.size() && d.song().tracks == before.tracks && d.history().position() == u0,
           "refused: nothing changed, no undo step");
  }
  return finish();
}
