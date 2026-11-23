// Gridline — hiding, narrowing and folding mixer strips (GitHub #58): which strips the views show,
// one undo step per edit, the project and collaboration round trip of the new fields, track state
// files leaving them out, groups taking a folded state, the folded group's mute and solo, and
// Mackie Control banks skipping hidden tracks.
#include <string>
#include <utility>
#include <vector>
#include "Check.h"
#include "core/ControllerScript.h"
#include "core/Document.h"
#include "core/MackieControl.h"
#include "core/MixerHide.h"
#include "core/MixerOps.h"
#include "core/Project.h"

using namespace gl;

namespace {

Document makeDoc(int inserts) {
  Song s = makeEmptySong();
  if (s.mixer.size() > static_cast<size_t>(inserts + 1)) s.mixer.resize(static_cast<size_t>(inserts + 1));  // (the empty song has more)
  Document doc(std::move(s));
  while (static_cast<int>(doc.song().mixer.size()) < inserts + 1) doc.addMixerTrack();
  return doc;
}

ObjectId idOf(const Document& d, size_t i) { return d.song().mixer[i].id; }

std::vector<size_t> allOrder(const Song& s) { return mixops::displayOrder(s); }

std::string shape(const Song& s) {  // "0 1 n2 g[G:3]@3 5" for the arranged entries
  std::string out;
  for (const mixhide::Entry& e : mixhide::arrange(s, allOrder(s))) {
    if (!out.empty()) out += " ";
    if (e.show == mixhide::Show::Narrow) out += "n";
    if (e.show == mixhide::Show::Group) out += "g[" + e.group + ":" + std::to_string(e.members) + "]@";
    out += std::to_string(e.index);
  }
  return out;
}

void testArrangeAndUndo() {
  Document d = makeDoc(6);
  expect(shape(d.song()) == "0 1 2 3 4 5 6", "every strip shown at first");
  expect(mixhide::hiddenCount(d.song()) == 0, "nothing hidden at first");
  const size_t h0 = d.history().position();
  expect(mixhide::setHidden(d, {idOf(d, 2), idOf(d, 4)}, true), "hide two tracks");
  expect(d.history().position() == h0 + 1, "hiding two tracks is one undo step");
  expect(shape(d.song()) == "0 1 3 5 6", "hidden tracks have no strip");
  expect(mixhide::hiddenCount(d.song()) == 2, "the count says two hidden");
  expect(!mixhide::setHidden(d, {idOf(d, 2)}, true), "hiding a hidden track changes nothing");
  expect(!mixhide::setHidden(d, {idOf(d, 0)}, true) && !d.song().mixer[0].hidden, "the master is never hidden");
  expect(mixhide::setNarrow(d, {idOf(d, 3)}, true), "narrow a track");
  expect(shape(d.song()) == "0 1 n3 5 6", "a narrow track shows as a slim strip");
  expect(!mixhide::setNarrow(d, {idOf(d, 0)}, true), "the master is never narrowed");
  d.undo();
  expect(shape(d.song()) == "0 1 3 5 6", "undo brings the full strip back");
  d.undo();
  expect(shape(d.song()) == "0 1 2 3 4 5 6", "undo shows both hidden tracks again");
  d.redo();
  expect(mixhide::hiddenTracks(d.song()).size() == 2, "redo hides them again");
  // The hidden tracks keep their routing and level.
  const MixerTrack& t2 = d.song().mixer[2];
  expect(t2.sends.size() == 1 && t2.sends[0].to == d.song().masterId() && !t2.muted, "a hidden track still sends to the master, unmuted");
  expect(mixhide::showAll(d), "Show all");
  expect(shape(d.song()) == "0 1 2 3 4 5 6" && mixhide::hiddenCount(d.song()) == 0, "Show all shows every track");
}

void testGroups() {
  Document d = makeDoc(6);
  mixops::createGroup(d, {idOf(d, 2), idOf(d, 3), idOf(d, 4)}, "Drums", false);
  const size_t h0 = d.history().position();
  expect(mixhide::setGroupFolded(d, "Drums", true), "collapse a group");
  expect(d.history().position() == h0 + 1, "collapsing is one undo step");
  expect(mixhide::groupFolded(d.song(), "Drums"), "the group is folded");
  expect(shape(d.song()) == "0 1 g[Drums:3]@2 5 6", "a folded group shows as one strip where its first member stood");
  expect(mixhide::hiddenCount(d.song()) == 3, "its members count as hidden");
  expect(mixhide::foldedGroups(d.song()) == std::vector<std::string>{"Drums"}, "the folded groups are listed");
  // A member hidden by itself stays hidden when the group opens again.
  mixhide::setHidden(d, {idOf(d, 3)}, true);
  mixhide::setGroupFolded(d, "Drums", false);
  expect(shape(d.song()) == "0 1 2 4 5 6", "expanded: the member hidden by itself stays hidden");
  mixhide::setGroupFolded(d, "Drums", true);
  // A track joining a folded group folds with it; one leaving it shows again.
  mixops::setGroup(d, {idOf(d, 5)}, "Drums");
  expect(d.song().mixer[5].groupFolded && shape(d.song()) == "0 1 g[Drums:4]@2 6", "a track added to a folded group folds into it");
  mixops::setGroup(d, {idOf(d, 2)}, "");
  expect(!d.song().mixer[2].groupFolded && shape(d.song()) == "0 1 2 g[Drums:3]@3 6", "a track taken out of the group shows again");
  // The group strip's mute and solo act on every member, one step each.
  const size_t h1 = d.history().position();
  expect(mixhide::setGroupMute(d, "Drums", true) && d.history().position() == h1 + 1, "mute the folded group in one step");
  expect(mixhide::groupMuted(d.song(), "Drums") && d.song().mixer[4].muted && d.song().mixer[5].muted, "every member is muted");
  expect(mixhide::setGroupSolo(d, "Drums", true) && mixhide::groupSoloed(d.song(), "Drums"), "solo the folded group");
  mixhide::setGroupMute(d, "Drums", false);
  expect(!mixhide::groupMuted(d.song(), "Drums") && !d.song().mixer[3].muted, "unmute every member");
  // Show all opens the folded groups too.
  mixhide::showAll(d);
  expect(mixhide::foldedGroups(d.song()).empty() && mixhide::hiddenCount(d.song()) == 0, "Show all opens folded groups");
}

void testRoundTrips() {
  Document d = makeDoc(4);
  mixops::createGroup(d, {idOf(d, 3), idOf(d, 4)}, "Keys", true);
  mixhide::setHidden(d, {idOf(d, 1)}, true);
  mixhide::setNarrow(d, {idOf(d, 2)}, true);
  mixhide::setGroupFolded(d, "Keys", true);
  // The project file.
  Song back;
  std::string err;
  expect(loadProjectText(projectToText(d.song()), "", back, &err), "the project loads back");
  expect(back.mixer.size() == 5 && back.mixer[1].hidden && back.mixer[2].narrow && back.mixer[3].groupFolded && back.mixer[4].groupFolded,
         "hidden, narrow and folded are saved with the project");
  expect(!back.mixer[0].hidden && !back.mixer[3].hidden && !back.mixer[1].narrow, "the other tracks stay as they were");
  expect(back.mixer[1] == d.song().mixer[1] && back.mixer[3] == d.song().mixer[3], "the tracks read back equal");
  // Collaboration sends a changed mixer track as its JSON element.
  MixerTrack peer;
  mixerTrackFromJson(mixerTrackToJson(d.song().mixer[1]), peer);
  expect(peer.hidden && !peer.narrow, "a collaborator's copy of a hidden track is hidden");
  MixerTrack plain;
  const Json pj = mixerTrackToJson(plain);
  expect(!pj.find("hidden") && !pj.find("narrow") && !pj.find("groupFolded"), "a plain track writes none of the new fields");
  // Track state files are about the sound: they leave the view state out and never change it.
  const std::string state = mixops::trackStateText(d.song().mixer[1]);
  expect(state.find("hidden") == std::string::npos && state.find("groupFolded") == std::string::npos, "a track state file leaves the view state out");
  expect(mixops::applyTrackState(d, idOf(d, 2), state) && d.song().mixer[2].narrow && !d.song().mixer[2].hidden,
         "loading a track state keeps the track's own view state");
}

void testMackieBanks() {
  Document d = makeDoc(14);
  script::SimpleHost host(d);
  mackie::Surface s(host, mackie::Options{mackie::Protocol::Mcu, 0, false}, [](int, const std::vector<uint8_t>&) {});
  expect(s.strips() == 8 && s.mixerOf(0) == 1 && s.mixerOf(7) == 8, "eight strips on inserts 1..8");
  mixhide::setHidden(d, {idOf(d, 2), idOf(d, 3)}, true);
  expect(s.mixerOf(0) == 1 && s.mixerOf(1) == 4 && s.mixerOf(7) == 10, "the strips skip the hidden tracks");
  mixops::createGroup(d, {idOf(d, 5), idOf(d, 6)}, "Fx", false);
  mixhide::setGroupFolded(d, "Fx", true);
  expect(s.mixerOf(2) == 7 && s.mixerOf(5) == 10 && s.mixerOf(7) == 12, "and a folded group's members");
  s.onMidi(0, MidiMessage{0x90, 0x2F, 0x7F});  // bank right: limited to the last full bank of what is shown
  expect(s.mixerOf(0) == 7 && s.mixerOf(7) == 14, "bank right ends on the last insert shown");
  s.onMidi(0, MidiMessage{0xE0, 0x00, 0x00});  // fader 1 down: insert 7
  expect(d.song().mixer[7].volume < 1e-6f && d.song().mixer[2].volume > 0.5f, "fader 1 moves the track it shows, never a hidden one");
  mixhide::showAll(d);
  s.onMidi(0, MidiMessage{0x90, 0x2E, 0x7F});  // bank left
  expect(s.mixerOf(0) == 1 && s.mixerOf(1) == 2, "shown again, the tracks are back on the surface");
}

}  // namespace

int main() {
  testArrangeAndUndo();
  testGroups();
  testRoundTrips();
  testMackieBanks();
  return finish();
}
