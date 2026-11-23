// Gridline: K-254, render MIDI to audio in a collaboration session (two replicas in memory).
//  * Ann renders the Kick row into a slicer channel on her computer: the placement is one edit,
//    so it reaches Ben, and the rendered file reaches him as a file of his own (the same sound);
//  * while she renders, her presence says what she renders, so Ben sees the row as busy, and the
//    busy mark clears when she is done;
//  * Ann's undo takes the slicer away on Ben's computer too; her redo brings it back.
#include <filesystem>
#include <string>
#include "Check.h"
#include "Hermetic.h"
#include "collab/CollabSim.h"
#include "core/PluginInfo.h"
#include "engine/Sample.h"
#include "engine/SourceRender.h"

using namespace gl;
namespace sr = gl::srcrender;
namespace fs = std::filesystem;

namespace {

struct Fixture {
  Song s;
  ObjectId kick = kNoId, pattern = kNoId;
};

Fixture fixture() {
  Fixture f;
  f.s = makeEmptySong();
  Channel k;
  k.id = f.s.allocateId();
  k.name = "Kick";
  k.plugin = defaultPluginState("gl.synth");
  f.s.channels.push_back(k);
  f.kick = k.id;
  Pattern p;
  p.id = f.s.allocateId();
  p.name = "Break A";
  std::vector<uint8_t> steps(16, 0);
  for (int i : {0, 4, 7, 10, 12}) steps[static_cast<size_t>(i)] = 110;
  p.steps[k.id] = steps;
  f.s.patterns.push_back(p);
  f.pattern = p.id;
  Clip c;
  c.id = f.s.allocateId();
  c.pattern = p.id;
  f.s.playlist.push_back(c);
  return f;
}

void collaboration() {
  using namespace gl::collab;
  using namespace sim;
  Clock clock;
  Net net(254);
  Peer a, b;
  a.label = "Ann", b.label = "Ben";
  for (Peer* p : {&a, &b}) p->id = crypto::Identity::generate();
  Fixture f = fixture();
  a.doc.reset(f.s);
  a.folder = tmpDir("k254_a");
  b.folder = tmpDir("k254_b");
  std::string err;
  a.s = Session::create(a.doc, config(a.folder, a.id, a.label, &clock), &err);
  expect(a.s != nullptr, "a session");
  if (!a.s) return;
  net.add(&a);
  b.s = Session::join(b.doc, config(b.folder, b.id, b.label, &clock), ticketOf(*a.s), &err);
  a.s->admitMember(memberOf(b));
  net.add(&b);
  net.connect(&a, &b);
  settle(net, {&a, &b}, clock);
  // Ann starts rendering the Kick row: Ben sees it as busy.
  Presence busy;
  busy.busyKind = 1;
  busy.busyId = static_cast<uint32_t>(f.kick);
  a.s->setPresence(busy);
  run(net, {&a, &b}, clock, 1.0);
  const auto& pb = b.s->peers();
  auto annSeen = [&]() -> const Presence* {
    auto it = pb.find(a.s->site());
    return it == pb.end() ? nullptr : &it->second.presence;
  };
  expect(annSeen() && annSeen()->busyKind == 1 && annSeen()->busyId == f.kick, "Ben sees Ann rendering the Kick row");
  // The render runs on Ann's computer; its file lives in her project's Rendered folder.
  const sr::Source src = sr::channelSource(f.pattern, f.kick);
  sr::Options o = sr::defaultOptions(src);
  o.sampleRate = 44100;
  const sr::Output out = sr::render(a.doc.song(), src, o);
  const std::string path = sr::writeRender(out, (fs::path(a.folder) / "Rendered").string());
  expect(!path.empty(), "Ann's render is written");
  sr::PlaceOptions po;
  po.dest = sr::Dest::Slicer;
  po.muteSource = true;
  sr::Placed placed;
  a.doc.editSong("render to slicer", [&](Song& x) { placed = sr::place(x, src, out, path, po); });
  a.s->setPresence(Presence{});
  settle(net, {&a, &b}, clock);
  run(net, {&a, &b}, clock, 1.0);
  expect(annSeen() && annSeen()->busyKind == 0, "the busy mark clears when the render has landed");
  const Channel* cb = b.doc.song().channel(placed.channel);
  expect(cb && cb->plugin.type == "gl.slicer", "the slicer channel reaches Ben");
  expect(b.doc.song().channel(f.kick) && b.doc.song().channel(f.kick)->muted, "the muted Kick row reaches Ben");
  std::error_code ec;
  const bool fileHere = cb && fs::is_regular_file(cb->plugin.data, ec) && !samePath(cb->plugin.data, path);
  expect(fileHere, "the rendered file arrives as a file of Ben's own");
  if (fileHere) {
    SamplePtr mine = loadSample(path), theirs = loadSample(cb->plugin.data);
    expect(mine && theirs && mine->left == theirs->left && mine->right.vec() == theirs->right.vec(), "with the same sound");
  }
  const Pattern* pbp = b.doc.song().pattern(f.pattern);
  expect(pbp && pbp->notes.count(placed.channel) && static_cast<int>(pbp->notes.at(placed.channel).size()) == placed.slices,
         "the slices' notes reach Ben");
  // Ann's undo takes it away on Ben's computer; redo brings it back.
  a.doc.undo();
  settle(net, {&a, &b}, clock);
  expect(!b.doc.song().channel(placed.channel) && !b.doc.song().channel(f.kick)->muted, "Ann's undo removes the render on Ben's computer");
  a.doc.redo();
  settle(net, {&a, &b}, clock);
  expect(b.doc.song().channel(placed.channel) != nullptr, "and her redo brings it back");
}

}  // namespace

int main() {
  gl::test::hermeticTestEnv("k254collab");
  collaboration();
  return finish();
}
