// Gridline: GitHub issue 58, "Automation crashes" when creating automation clips. A whole-song
// edit (Document::editSong: the playlist "Create automation clip" from a knob's menu, and many
// more) copied every large container of the song (the delta command's apply chose between a
// stored container and the current one with ?:, which made a copy), so a pattern or channel an
// editor held across the edit pointed at freed memory: the channel rack crashed when its volume
// knob's menu created an automation clip. Checks:
//  * a whole-song edit keeps the buffers of the containers it does not change (patterns,
//    channels, mixer, ...), and the changed ones hold the edit;
//  * undo and redo of that edit keep the unchanged buffers too, and give back the right songs;
//  * an edit that changes a container still records and restores it (before / after);
//  * a song equal to its copy (the load path) is unchanged by an edit that changes nothing.
#include <string>
#include <vector>
#include "Check.h"
#include "core/Document.h"
#include "core/PluginInfo.h"
#include "core/Song.h"

using namespace gl;

namespace {

Song fixture() {
  Song s = makeEmptySong();
  for (int i = 0; i < 4; ++i) {
    Channel c;
    c.id = s.allocateId();
    c.name = "Ch " + std::to_string(i);
    c.plugin = defaultPluginState("gl.synth");
    c.mixerTrack = s.masterId();
    s.channels.push_back(c);
  }
  for (int i = 0; i < 3; ++i) {
    Pattern p;
    p.id = s.allocateId();
    p.name = "P" + std::to_string(i);
    p.steps[s.channels[0].id] = std::vector<uint8_t>(16, 0);
    p.steps[s.channels[0].id][0] = 100;
    s.patterns.push_back(p);
  }
  Clip k;
  k.id = s.allocateId();
  k.pattern = s.patterns[0].id;
  k.length = s.barTicks();
  s.playlist.push_back(k);
  return s;
}

// What createAutomationClip (AutomationEditor.cpp) does to the song.
void addAutomationClip(Song& x, ObjectId owner) {
  AutomationClip c;
  c.id = x.allocateId();
  c.name = "Volume";
  c.owner = owner;
  c.index = static_cast<uint32_t>(ChannelParam::Volume);
  c.points = {AutoPoint{0, 0.5f, 0, AutoShape::Curve}, AutoPoint{x.barTicks(), 0.5f, 0, AutoShape::Curve}};
  Clip k;
  k.id = x.allocateId();
  k.kind = ClipKind::Automation;
  k.source = c.id;
  k.track = 1;
  k.length = x.barTicks();
  x.automations.push_back(c);
  x.playlist.push_back(k);
}

struct Buffers {
  const void *patterns, *channels, *mixer, *tracks;
  explicit Buffers(const Song& s) : patterns(s.patterns.data()), channels(s.channels.data()), mixer(s.mixer.data()), tracks(s.tracks.data()) {}
  bool same(const Song& s) const {
    return patterns == s.patterns.data() && channels == s.channels.data() && mixer == s.mixer.data() && tracks == s.tracks.data();
  }
};

}  // namespace

int main() {
  Document d(fixture());
  const Song before = d.song();
  const Buffers b(d.song());
  const ObjectId patId = before.patterns.back().id;  // "P2" (makeEmptySong may add a pattern of its own)
  const Pattern* pat = d.song().pattern(patId);       // what the channel rack holds
  const Channel* ch = &d.song().channels.back();      // "Ch 3"
  d.editSong("create automation clip", [&](Song& x) { addAutomationClip(x, x.channels[0].id); });
  expect(b.same(d.song()), "a whole-song edit keeps the buffers of the containers it does not change");
  expect(pat == d.song().pattern(patId) && pat->name == "P2" && pat->bars == before.patterns.back().bars,
         "a pattern held across the edit is still the song's pattern");
  expect(ch == &d.song().channels.back() && ch->name == "Ch 3", "a channel held across the edit is still the song's channel");
  expect(d.song().automations.size() == 1 && d.song().playlist.size() == 2, "the edit added the clip and its placement");
  const Song after = d.song();

  d.undo();
  expect(b.same(d.song()), "undo keeps the unchanged buffers");
  expect(d.song() == before, "undo gives back the song before the edit");
  d.redo();
  expect(b.same(d.song()), "redo keeps the unchanged buffers");
  expect(d.song() == after, "redo gives back the edited song");

  // An edit that changes patterns: recorded both ways, other containers kept.
  const Buffers b2(d.song());
  d.editSong("rename pattern", [&](Song& x) { x.patterns[0].name = "Renamed"; });
  expect(d.song().patterns[0].name == "Renamed", "a changed container holds the edit");
  expect(b2.channels == d.song().channels.data() && b2.mixer == d.song().mixer.data(), "other containers keep their buffers");
  d.undo();
  expect(d.song() == after, "undo of a changed container restores it");
  d.redo();
  expect(d.song().patterns[0].name == "Renamed", "redo of a changed container applies it again");

  // No change: no undo step, nothing moved.
  const size_t pos = d.history().position();
  const Buffers b3(d.song());
  d.editSong("nothing", [](Song&) {});
  expect(d.history().position() == pos && b3.same(d.song()), "an edit that changes nothing leaves the song alone");

  // The smaller lists (channel groups, pattern groups, the other Stage rooms) keep their buffers
  // too: the Stage rooms list held a room across a whole-song edit.
  d.editSong("add rooms and groups", [](Song& x) {
    x.stageRooms.resize(2);
    x.channelGroups = {"Drums", "Keys"};
    x.patternGroups.resize(1);
  });
  const void* rooms = d.song().stageRooms.data();
  const void* chGroups = d.song().channelGroups.data();
  const void* patGroups = d.song().patternGroups.data();
  d.editSong("create automation clip", [&](Song& x) { addAutomationClip(x, x.channels[1].id); });
  expect(rooms == d.song().stageRooms.data() && chGroups == d.song().channelGroups.data() && patGroups == d.song().patternGroups.data(),
         "a whole-song edit keeps the rooms and groups lists it does not change");
  d.undo();
  expect(rooms == d.song().stageRooms.data() && d.song().stageRooms.size() == 2, "undo keeps them too");
  d.editSong("rename room", [](Song& x) { x.stageRooms[1].name = "Hall"; });
  expect(d.song().stageRooms[1].name == "Hall" && chGroups == d.song().channelGroups.data(), "a room edit holds, the groups stay put");
  d.undo();
  expect(d.song().stageRooms[1].name.empty(), "undo of a room edit restores it");
  return finish();
}
