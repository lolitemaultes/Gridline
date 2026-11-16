// K-112D: the Smooth layout's window state saved with the project (Song::smooth): written only once
// set, read back as written, and an older project (none written) opens with the Library showing.
// In a shared project it stays on each computer: never sent, never overwritten by a collaborator's
// edit (made in any layout), and still in this computer's save afterwards.
#include <string>
#include "Check.h"
#include "collab/CollabSim.h"
#include "collab/LocalState.h"
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
  testShared();
  return finish();
}
