// K-112D: the Smooth layout's window state saved with the project (Song::smooth): written only once
// set, read back as written, and an older project (none written) opens with the Library showing.
// In a shared project it stays on each computer: never sent, never overwritten by a collaborator's
// edit (made in any layout), and still in this computer's save afterwards.
// D6 (R12-02, R12-03): the Smart Control mappings (smooth.controls) are song data: part of the song's
// comparison (an undo step that marks the project modified), sent to collaborators and taken from
// theirs, while the Library pane stays each computer's own; both saves end equal.
#include <string>
#include "Check.h"
#include "collab/CollabSim.h"
#include "collab/LocalState.h"
#include "core/Document.h"
#include "core/Project.h"
#include "core/Song.h"

using namespace gl;

namespace {

void testShared() {
  // the local parts alone: masked going out, kept coming in
  Song s = makeEmptySong();
  s.smooth.library = "closed";
  Json j = songToJson(s, false);
  expect(j.find("smooth") != nullptr, "the project carries the state");
  collab::maskSessionJson(j);
  expect(j.find("smooth") == nullptr, "masked: the state is never sent");
  Song in = makeEmptySong();
  collab::keepSessionState(in, s);
  expect(in.smooth.library == "closed", "an incoming song keeps this computer's state");

  // two people in one project (in memory): Alex in Smooth hides the Library, Sam (another layout)
  // changes the tempo and renames a channel
  sim::Clock clock;
  sim::Net net{11};
  sim::Peer a, b;
  a.label = "Alex";
  b.label = "Sam";
  a.id = collab::crypto::Identity::generate();
  b.id = collab::crypto::Identity::generate();
  a.doc.reset(makeEmptySong());
  const ObjectId kick = a.doc.addChannel("Kick", "gl.kick");
  a.folder = sim::tmpDir("k112d_a");
  b.folder = sim::tmpDir("k112d_b");
  a.s = collab::Session::create(a.doc, sim::config(a.folder, a.id, "Alex", &clock));
  net.add(&a);
  b.s = collab::Session::join(b.doc, sim::config(b.folder, b.id, "Sam", &clock), sim::ticketOf(*a.s));
  a.s->admitMember(sim::memberOf(b));
  net.add(&b);
  net.connect(&a, &b);
  auto step = [&](double seconds) {
    for (double t = 0; t < seconds; t += 0.02) {
      net.now += 0.02;
      clock.ms += 20;
      net.deliver();
      a.s->frame(net.now);
      b.s->frame(net.now);
    }
  };
  step(5.0);
  expect(b.doc.song().channel(kick) != nullptr, "Sam joined with the project");
  const uint64_t opsA = a.s->opCount();
  a.doc.patchSong([](Song& x) { x.smooth.library = "closed"; });
  step(2.0);
  expect(a.s->opCount() == opsA, "hiding the Library makes no op");
  expect(b.doc.song().smooth.library.empty(), "Sam's song never gets Alex's window state");
  b.doc.editSong("Tempo", [](Song& x) { x.tempo = 133.0; });
  b.doc.editChannel(kick, "Rename", [](Channel& c) { c.name = "Kick (Sam)"; });
  step(3.0);
  expect(a.doc.song().tempo == 133.0 && a.doc.song().channel(kick) && a.doc.song().channel(kick)->name == "Kick (Sam)",
         "Sam's edits reach Alex");
  expect(a.doc.song().smooth.library == "closed", "Alex's Library stays hidden after Sam's edits");
  Song back;
  expect(loadProjectText(projectToText(a.doc.song()), "shared.gridline", back) && back.smooth.library == "closed" && back.tempo == 133.0,
         "Alex's save keeps both the shared edits and the window state");
  a.doc.patchSong([](Song& x) { x.smooth.library = "open"; });
  b.doc.editSong("Tempo", [](Song& x) { x.tempo = 140.0; });
  step(3.0);
  expect(a.doc.song().tempo == 140.0 && a.doc.song().smooth.library == "open", "shown again: kept through the next edit too");

  // D6 (R12-03): Alex maps a Smart Control (an undo step, as the Smooth pane records it): Sam gets it
  const uint64_t opsB = a.s->opCount();
  a.doc.editSong("map smart control", [kick](Song& x) { x.smooth.controls.push_back(SmartControlMap{kick, 1, kick, 1005}); });
  step(3.0);
  expect(a.s->opCount() > opsB, "a mapping makes ops");
  expect(b.doc.song().smooth.controls.size() == 1 && b.doc.song().smooth.controls[0] == a.doc.song().smooth.controls[0],
         "Sam gets Alex's mapping");
  expect(b.doc.song().smooth.library.empty() && a.doc.song().smooth.library == "open", "the Library pane stays each one's own");
  auto sharedText = [](Song x) {
    x.smooth.library.clear();
    return projectToText(x);
  };
  expect(sharedText(a.doc.song()) == sharedText(b.doc.song()), "both saves equal (but for the Library pane)");
  // Sam (any layout) edits something else: Alex's mapping stays, and Sam's save keeps it
  b.doc.editSong("Tempo", [](Song& x) { x.tempo = 128.0; });
  step(3.0);
  expect(a.doc.song().smooth.controls.size() == 1 && b.doc.song().smooth.controls.size() == 1, "kept through Sam's next edit");
  Song samBack;
  expect(loadProjectText(projectToText(b.doc.song()), "sam.gridline", samBack) && samBack.smooth.controls.size() == 1,
         "Sam's save carries the mapping");
  // Alex takes it back (undo): gone on both sides
  expect(a.doc.undo(), "Alex's undo");
  step(3.0);
  expect(a.doc.song().smooth.controls.empty() && b.doc.song().smooth.controls.empty(), "the undone mapping goes on both sides");
  expect(a.doc.song().tempo == 128.0, "Sam's tempo stays");
  expect(sharedText(a.doc.song()) == sharedText(b.doc.song()), "both saves equal after the undo");
  // Sam maps one: Alex gets it
  b.doc.editSong("map smart control", [kick](Song& x) { x.smooth.controls.push_back(SmartControlMap{kick, 2, kick, 1003}); });
  step(3.0);
  expect(a.doc.song().smooth.controls.size() == 1 && a.doc.song().smooth.controls[0].pos == 2, "Alex gets Sam's mapping");
  expect(a.doc.song().smooth.library == "open", "and keeps the Library pane");
}

void testUndo() {
  // D6 (R12-02): a mapping compares (an edit), is an undo step, and marks the project modified
  Song x = makeEmptySong(), y = x;
  y.smooth.controls.push_back(SmartControlMap{3, 0, 4, 1001});
  expect(!(x == y), "a mapping is part of the song's comparison");
  Song z = x;
  z.smooth.library = "closed";
  expect(x == z, "the Library pane is not");
  Document d;
  d.reset(makeEmptySong());
  d.markSaved();
  d.editSong("map smart control", [](Song& s) { s.smooth.controls.push_back(SmartControlMap{3, 0, 4, 1001}); });
  expect(d.song().smooth.controls.size() == 1 && d.modified(), "mapped: modified");
  expect(d.undo() && d.song().smooth.controls.empty() && !d.modified(), "undo takes the mapping back");
  expect(d.redo() && d.song().smooth.controls.size() == 1 && d.modified(), "redo puts it back");
  d.editSong("reset smart control", [](Song& s) { s.smooth.controls.clear(); });
  expect(d.song().smooth.controls.empty() && d.modified(), "a reset is a step too");
  expect(d.undo() && d.song().smooth.controls.size() == 1, "and undoes");
  // masked going out: the mappings go, the Library pane does not; incoming mappings are taken
  Song m = makeEmptySong();
  m.smooth.library = "closed";
  m.smooth.controls.push_back(SmartControlMap{3, 0, 4, 1001});
  Json j = songToJson(m, false);
  collab::maskSessionJson(j);
  const Json* js = j.find("smooth");
  expect(js && js->find("controls") && !js->find("library"), "sent: the mappings, not the Library pane");
  Song in = makeEmptySong(), cur = makeEmptySong();
  in.smooth.controls = m.smooth.controls;
  cur.smooth.library = "open";
  collab::keepSessionState(in, cur);
  expect(in.smooth.controls.size() == 1 && in.smooth.library == "open", "received: their mappings, this computer's pane");
  expect(collab::sharedSongDigest(m) != collab::sharedSongDigest(makeEmptySong()), "the digest counts the mappings");
  Song lib = makeEmptySong();
  lib.smooth.library = "closed";
  expect(collab::sharedSongDigest(lib) == collab::sharedSongDigest(makeEmptySong()), "but not the Library pane");
}

}  // namespace

int main() {
  Song s = makeEmptySong();
  const std::string plain = projectToText(s);
  expect(plain.find("\"smooth\"") == std::string::npos, "never set: nothing written (older saves stay byte for byte)");

  Song back;
  expect(loadProjectText(plain, "a.gridline", back), "plain project loads");
  expect(back.smooth.library.empty(), "an older project: no state (the Library opens)");

  s.smooth.library = "closed";
  const std::string closed = projectToText(s);
  expect(closed.find("\"smooth\"") != std::string::npos, "set: written");
  Song back2;
  expect(loadProjectText(closed, "b.gridline", back2) && back2.smooth.library == "closed", "closed reads back");
  expect(back2 == s, "view state takes no part in the comparison");

  s.smooth.library = "open";
  Song back3;
  expect(loadProjectText(projectToText(s), "c.gridline", back3) && back3.smooth.library == "open", "open reads back");

  // An unknown value is dropped (the Library opens).
  std::string odd = closed;
  odd.replace(odd.find("\"closed\""), 8, "\"sideways\"");
  Song back4;
  expect(loadProjectText(odd, "d.gridline", back4) && back4.smooth.library.empty(), "an unknown value reads as unset");
  // D5 (R11-06): Smart Controls mapped by hand: written only when there are some, read back as written.
  Song m = makeEmptySong();
  m.smooth.controls.push_back(SmartControlMap{12, 1, 30, 1005});
  m.smooth.controls.push_back(SmartControlMap{12, 6, 31, 103});
  const std::string mapped = projectToText(m);
  expect(mapped.find("\"controls\"") != std::string::npos && mapped.find("\"library\"") == std::string::npos, "mappings written alone");
  Song back5;
  expect(loadProjectText(mapped, "e.gridline", back5) && back5.smooth.controls.size() == 2 && back5.smooth.controls[0].strip == 12 &&
             back5.smooth.controls[0].pos == 1 && back5.smooth.controls[0].owner == 30 && back5.smooth.controls[0].index == 1005 &&
             back5.smooth.controls[1].pos == 6 && back5.smooth.controls[1].index == 103,
         "mappings read back");
  expect(projectToText(back5) == mapped, "mappings saved again byte for byte");
  Song badSong = m;
  badSong.smooth.controls[0].pos = 99;
  Song back6;
  expect(loadProjectText(projectToText(badSong), "f.gridline", back6) && back6.smooth.controls.size() == 1, "a position out of range is dropped");
  testShared();
  testUndo();
  return finish();
}
