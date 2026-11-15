// Gridline - K-112c: Document::onPluginPlaced, the Live layout's "open a plugin's window when it
// is put on a track". It fires for a plugin put on a track by an edit made here (a new channel,
// a replaced instrument, an effect in a mixer slot), and never:
//   - for undo or redo of those edits (they replay the recorded steps);
//   - for a removed effect (an empty type);
//   - for a collaborator's edits (they arrive through the session, patchSong / replaceSong);
//   - for an action that adds several plugins at once (a drum kit), nor in a quiet scope
//     (scripts, AI tools, drum kits), nor for a cancelled action.
// A cloned channel and the device band's Alt-drag copy (put in a free slot, then moved into place,
// one action) are reported, the copy where it landed.
//   cwd: the build dir (the collaboration folders go under collab_test_tmp).
#include <string>
#include <vector>
#include "Check.h"
#include "collab/CollabSim.h"
#include "core/Document.h"
#include "core/Mcp.h"
#include "core/ProjectScript.h"
#include "engine/Freeze.h"

using namespace gl;
using namespace gl::collab;
using namespace sim;

namespace {

struct Seen {
  ObjectId owner;
  int slot;
  std::string type;
};

void listen(Document& d, std::vector<Seen>& out) {
  d.onPluginPlaced = [&out](ObjectId owner, int slot, const std::string& type) { out.push_back(Seen{owner, slot, type}); };
}

// A new document has no mixer yet: give it the default one (before anything is counted).
void withMixer(Document& d) {
  if (d.song().mixer.size() > 1) return;
  Song s = d.song();
  ensureMixer(s);
  d.replaceSong(std::move(s));
}

ObjectId firstInsert(Document& d) {
  withMixer(d);
  return d.song().mixer[1].id;
}

}  // namespace

int main() {
  // ---- local edits, then undo and redo ----
  {
    Document d;
    withMixer(d);
    std::vector<Seen> seen;
    listen(d, seen);
    const ObjectId ch = d.addChannel("Lead", "gl.synth");
    expect(seen.size() == 1 && seen[0].owner == ch && seen[0].slot == -1 && seen[0].type == "gl.synth", "a new channel's instrument is reported");
    expect(d.setChannelPlugin(ch, "gl.acid"), "instrument replaced");
    expect(seen.size() == 2 && seen[1].owner == ch && seen[1].slot == -1 && seen[1].type == "gl.acid", "a replaced instrument is reported");
    const ObjectId mt = firstInsert(d);
    expect(d.setSlotPlugin(mt, 2, "gl.eq"), "effect inserted");
    expect(seen.size() == 3 && seen[2].owner == mt && seen[2].slot == 2 && seen[2].type == "gl.eq", "an inserted effect is reported");
    expect(d.setSlotPlugin(mt, 2, ""), "effect removed");
    expect(seen.size() == 3, "a removed effect is not reported");
    const size_t before = seen.size();
    for (int k = 0; k < 4; ++k) d.undo();
    expect(seen.size() == before, "undo reports nothing");
    for (int k = 0; k < 4; ++k) d.redo();
    expect(seen.size() == before, "redo reports nothing");
    expect(d.song().channel(ch) && d.song().channel(ch)->plugin.type == "gl.acid", "redo brought the edits back");
  }

  // ---- by hand, one at a time: a clone and an Alt-drag copy count; several at once do not ----
  {
    Document d;
    const ObjectId lead = d.addChannel("Lead", "gl.synth");
    const ObjectId mt = firstInsert(d);
    d.setSlotPlugin(mt, 0, "gl.eq");
    std::vector<Seen> seen;
    listen(d, seen);
    // A drum kit's way: several channels in one action.
    d.beginGroup("load drum kit");
    d.addChannel("Kick", "gl.kick");
    d.addChannel("Snare", "gl.synth");
    d.endGroup();
    expect(seen.empty(), "several plugins added in one action are not reported");
    // An insert and an instrument in one action: still several.
    d.beginGroup("two at once");
    d.setSlotPlugin(mt, 3, "gl.eq");
    d.setChannelPlugin(lead, "gl.acid");
    d.endGroup();
    expect(seen.empty(), "an effect and an instrument in one action are not reported");
    // A quiet scope (scripts, AI tools, drum kits) reports nothing, even for one plugin.
    {
      const Document::QuietPlacements quiet(d);
      d.addChannel("Quiet", "gl.synth");
      d.setSlotPlugin(mt, 4, "gl.eq");
    }
    expect(seen.empty(), "nothing is reported in a quiet scope");
    // A cancelled action reports nothing.
    d.beginGroup("cancelled");
    d.addChannel("Gone", "gl.synth");
    d.cancelGroup();
    expect(seen.empty(), "a cancelled action reports nothing");
    // A clone: one plugin, reported for the new channel.
    const ObjectId clone = d.cloneChannel(lead);
    expect(seen.size() == 1 && seen[0].owner == clone && seen[0].slot == -1, "a cloned channel's instrument is reported");
    // The device band's Alt-drag copy: into the first free slot, then moved into place, one action.
    seen.clear();
    int freeSlot = -1;
    for (int k = 0; k < kEffectSlots && freeSlot < 0; ++k)
      if (d.song().mixerTrack(mt)->slots[static_cast<size_t>(k)].plugin.type.empty()) freeSlot = k;
    expect(freeSlot >= 1, "a free slot for the copy");
    d.beginGroup("copy effect");
    d.setSlotPlugin(mt, freeSlot, "gl.eq");
    d.moveSlot(mt, freeSlot, 0);
    d.endGroup();
    expect(seen.size() == 1 && seen[0].owner == mt && seen[0].slot == 0 && seen[0].type == "gl.eq", "an Alt-drag copy is reported where it landed");
  }

  // ---- a script and an AI tool add plugins: never reported ----
  {
    Document d;
    d.addChannel("Lead", "gl.synth");
    std::vector<Seen> seen;
    listen(d, seen);
    script::SimpleProjectHost host(d);
    script::Vm vm;
    script::openProjectApi(vm, host);
    const size_t before = d.song().channels.size();
    const script::ProjectRunResult r = script::runProject(vm, host, "project.channels.add('From a script')", "test", "script: test");
    expect(r.ok && d.song().channels.size() == before + 1, "the script added a channel");
    expect(seen.empty(), "a script's channel is not reported");

    mcp::Environment env(d, host);
    double fakeNow = 1000.0;
    env.now = [&fakeNow] { return fakeNow += 0.5; };
    mcp::Session session(env);
    session.handle(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"test-client","version":"1"}}})");
    session.handle(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    const size_t before2 = d.song().channels.size();
    session.handle(R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"add_channel","arguments":{"name":"From a tool","plugin":"gl.kick"}}})");
    expect(d.song().channels.size() == before2 + 1, "the AI tool added a channel");
    expect(seen.empty(), "an AI tool's channel is not reported");
    d.onPluginPlaced = nullptr;
  }

  // ---- the device row's chosen parameters (PluginState::shown, part of operator==) ----
  {
    Document d;
    const ObjectId ch = d.addChannel("Lead", "gl.synth");
    const ObjectId mt = firstInsert(d);
    d.setSlotPlugin(mt, 0, "gl.eq");
    const std::string plain = jsonStringify(songToJson(d.song()), 0);
    expect(plain.find("\"shown\"") == std::string::npos, "no list: nothing about it is saved (older projects save byte-equal)");
    const std::string printBefore = freeze::fingerprint(d.song(), freeze::Unit{true, mt});
    const size_t pos = d.history().position();
    const bool edited = d.editMixerTrack(mt, "device parameters", [](MixerTrack& m) { m.slots[0].plugin.shown = {3, 1}; });
    expect(edited && d.history().position() == pos + 1, "a list-only edit is recorded as one step");
    expect(d.song().mixerTrack(mt)->slots[0].plugin.shown == std::vector<int>({3, 1}), "the list is set");
    expect(freeze::fingerprint(d.song(), freeze::Unit{true, mt}) == printBefore, "the list plays no part in a freeze");
    d.undo();
    expect(d.song().mixerTrack(mt)->slots[0].plugin.shown.empty(), "undo takes the list back");
    d.redo();
    expect(d.song().mixerTrack(mt)->slots[0].plugin.shown == std::vector<int>({3, 1}), "redo brings it back");
    d.editChannel(ch, "device parameters", [](Channel& c) { c.plugin.shown = {-1}; });  // "none chosen"
    Song back;
    const bool read = songFromJson(songToJson(d.song()), back);
    expect(read && back.channel(ch) && back.channel(ch)->plugin.shown == std::vector<int>({-1}), "{-1} (none chosen) saves and loads");
    expect(read && back.mixerTrack(mt) && back.mixerTrack(mt)->slots[0].plugin.shown == std::vector<int>({3, 1}), "a chosen list saves and loads");
    expect(read && back.mixerTrack(mt)->slots[0].plugin.params == d.song().mixerTrack(mt)->slots[0].plugin.params, "the parameters are untouched");
  }

  // ---- a collaborator's edits ----
  {
    Clock clock;
    Net net(112);
    Peer a, b;
    a.label = "a", b.label = "b";
    a.id = crypto::Identity::generate();
    b.id = crypto::Identity::generate();
    withMixer(a.doc);
    a.folder = tmpDir("k112_placed_a");
    b.folder = tmpDir("k112_placed_b");
    a.s = Session::create(a.doc, config(a.folder, a.id, "Alex", &clock));
    net.add(&a);
    b.s = Session::join(b.doc, config(b.folder, b.id, "Sam", &clock), ticketOf(*a.s));
    a.s->admitMember(memberOf(b));
    net.add(&b);
    net.connect(&a, &b);
    settle(net, {&a, &b}, clock);
    std::vector<Seen> seenA, seenB;
    listen(a.doc, seenA);
    listen(b.doc, seenB);
    const ObjectId ch = a.doc.addChannel("Kick", "gl.kick");
    const ObjectId mt = a.doc.song().mixer[1].id;
    a.doc.setSlotPlugin(mt, 0, "gl.eq");
    settle(net, {&a, &b}, clock);
    expect(seenA.size() == 2, "the person who added them is told twice");
    expect(b.doc.song().channel(ch) != nullptr, "the collaborator's copy has the new channel");
    const MixerTrack* bt = b.doc.song().mixerTrack(mt);
    expect(bt && bt->slots[0].plugin.type == "gl.eq", "the collaborator's copy has the new effect");
    expect(seenB.empty(), "the collaborator is not told (no window opens for someone else's add)");
    // The adder undoes and redoes: neither side is told again.
    a.doc.undo();
    settle(net, {&a, &b}, clock);
    a.doc.redo();
    settle(net, {&a, &b}, clock);
    expect(seenA.size() == 2 && seenB.empty(), "a session's undo and redo report nothing");
    // Sessions first: their folders and the documents they glue go last.
    a.doc.onPluginPlaced = nullptr;
    b.doc.onPluginPlaced = nullptr;
    b.s.reset();
    a.s.reset();
  }
  return finish();
}
