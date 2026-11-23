// Gridline - GitHub #58 and #74: channel rack and note tools.
//  * Clone with every note, without notes, or with one pattern's notes (one undo step).
//  * Channels hidden from the rack and shown again: one undo step, saved and loaded, and sent to
//    a collaborator.
//  * A channel's sequence copied or moved to a new pattern of its own.
//  * Every selected note resized at once (scale, set, longer or shorter, ends on the grid).
//  * The colour new channels and patterns take (varied, one colour, the channel's).
#include <string>
#include <vector>
#include "Check.h"
#include "collab/CollabSim.h"
#include "core/Document.h"
#include "core/MenuOps.h"
#include "core/NoteTools.h"
#include "core/Project.h"

using namespace gl;
using namespace gl::collab;
using namespace sim;

namespace {

Note note(Tick start, Tick len, int pitch = 60) {
  Note n;
  n.start = start;
  n.length = len;
  n.pitch = pitch;
  return n;
}

struct Rig {
  Document d;
  ObjectId pat = kNoId, pat2 = kNoId, kick = kNoId, lead = kNoId;
  Rig() {
    d.reset(makeEmptySong());
    kick = d.addChannel("Kick", "gl.kick");
    lead = d.addChannel("Lead", "gl.synth");
    pat = d.song().patterns[0].id;
    pat2 = d.addPattern("Second");
    d.setStep(pat, kick, 0, 100);
    d.setStep(pat, kick, 4, 100);
    d.setNotes(pat, lead, {note(0, 24), note(48, 24, 64)});
    d.setNotes(pat2, lead, {note(96, 48, 67)});
  }
  const Pattern& p(ObjectId id) const { return *d.song().pattern(id); }
};

size_t notesOf(const Pattern& p, ObjectId ch) {
  auto it = p.notes.find(ch);
  return it == p.notes.end() ? 0 : it->second.size();
}

void testClone() {
  {
    Rig r;
    const size_t before = r.d.song().channels.size();
    const ObjectId c = r.d.cloneChannel(r.lead);
    expect(r.d.song().channels.size() == before + 1 && notesOf(r.p(r.pat), c) == 2 && notesOf(r.p(r.pat2), c) == 1,
           "Clone copies the notes of every pattern");
    r.d.undo();
    expect(r.d.song().channels.size() == before && !r.d.song().channel(c), "one undo takes the clone back");
  }
  {
    Rig r;
    const ObjectId c = r.d.cloneChannel(r.lead, Document::CloneNotes::None);
    const Channel* src = r.d.song().channel(r.lead);
    const Channel* cl = r.d.song().channel(c);
    expect(cl && cl->plugin.type == src->plugin.type && cl->mixerTrack == src->mixerTrack, "the clone keeps the instrument and routing");
    expect(notesOf(r.p(r.pat), c) == 0 && notesOf(r.p(r.pat2), c) == 0, "Clone without notes copies no notes");
    const ObjectId k = r.d.cloneChannel(r.kick, Document::CloneNotes::None);
    auto it = r.p(r.pat).steps.find(k);
    bool lit = false;
    if (it != r.p(r.pat).steps.end())
      for (uint8_t v : it->second) lit = lit || v > 0;
    expect(!lit, "and no lit steps");
    r.d.undo();
    expect(!r.d.song().channel(k) && r.d.song().channel(c), "each clone is one undo step");
  }
  {
    Rig r;
    const ObjectId c = r.d.cloneChannel(r.lead, Document::CloneNotes::OnePattern, r.pat2);
    expect(notesOf(r.p(r.pat), c) == 0 && notesOf(r.p(r.pat2), c) == 1, "Clone with this pattern's notes copies just that pattern");
  }
  {
    Rig r;
    const auto ids = ops::cloneChannels(r.d, {r.kick, r.lead}, Document::CloneNotes::None);
    expect(ids.size() == 2 && notesOf(r.p(r.pat), ids[1]) == 0, "several channels cloned without notes");
    r.d.undo();
    expect(r.d.song().channels.size() == 2, "several clones are one undo step");
  }
}

void testHide() {
  Rig r;
  expect(ops::rackHiddenChannels(r.d.song()).empty(), "nothing hidden at first");
  expect(ops::setChannelsRackHidden(r.d, {r.kick, r.lead}, true), "hide two channels");
  expect(ops::rackHiddenChannels(r.d.song()).size() == 2, "both hidden");
  r.d.undo();
  expect(ops::rackHiddenChannels(r.d.song()).empty(), "hiding two is one undo step");
  r.d.redo();
  expect(!ops::setChannelsRackHidden(r.d, {r.kick}, true), "hiding a hidden channel changes nothing");
  // Hidden from the rack only: it still plays and keeps its sequence.
  expect(r.d.song().channel(r.kick)->muted == false && notesOf(r.p(r.pat), r.lead) == 2, "a hidden channel still plays");

  Song loaded;
  std::string err;
  expect(loadProjectText(projectToText(r.d.song()), "", loaded, &err), "the project loads");
  expect(loaded.channel(r.kick) && loaded.channel(r.kick)->rackHidden && loaded.channel(r.lead)->rackHidden, "hidden channels are saved with the song");

  ops::setChannelsRackHidden(r.d, {r.lead}, false);
  expect(ops::rackHiddenChannels(r.d.song()) == std::vector<ObjectId>{r.kick}, "show one again");
  const ObjectId c = r.d.cloneChannel(r.kick);
  expect(!r.d.song().channel(c)->rackHidden, "a clone of a hidden channel shows");
  Song plain;
  expect(loadProjectText(projectToText(makeEmptySong()), "", plain, &err) && ops::rackHiddenChannels(plain).empty(), "older songs show every channel");
}

void testNewPattern() {
  Rig r;
  expect(ops::channelHasSequence(r.p(r.pat), r.kick) && ops::channelHasSequence(r.p(r.pat), r.lead), "a lit step or a note is a sequence");
  expect(!ops::channelHasSequence(r.p(r.pat2), r.kick), "no steps and no notes: none");
  const size_t nPat = r.d.song().patterns.size();
  const ObjectId q = ops::channelsToNewPattern(r.d, r.pat, {r.kick}, false);
  expect(q != kNoId && r.d.song().patterns.size() == nPat + 1, "a new pattern");
  const Pattern& np = r.p(q);
  expect(ops::channelHasSequence(np, r.kick) && !ops::channelHasSequence(np, r.lead), "it holds just that channel's sequence");
  expect(np.bars == r.p(r.pat).bars && np.stepsPerBar == r.p(r.pat).stepsPerBar, "the same length");
  expect(ops::channelHasSequence(r.p(r.pat), r.kick), "copied: the source keeps it");
  r.d.undo();
  expect(r.d.song().patterns.size() == nPat, "one undo step");
  const ObjectId m = ops::channelsToNewPattern(r.d, r.pat, {r.lead}, true);
  expect(m != kNoId && notesOf(r.p(m), r.lead) == 2 && notesOf(r.p(r.pat), r.lead) == 0, "moved: the source no longer has it");
}

void testLengths() {
  std::vector<Note> n = {note(0, 24), note(24, 48), note(96, 12)};
  {
    auto x = n;
    ops::NoteSel sel = {true, true, false};
    tools::LengthOpts o;
    o.mode = tools::LengthOpts::Scale, o.factor = 2.0;
    tools::setLengths(x, sel, o);
    expect(x[0].length == 48 && x[1].length == 96 && x[2].length == 12, "twice as long: only the selected notes");
  }
  {
    auto x = n;
    ops::NoteSel sel;
    tools::LengthOpts o;
    o.mode = tools::LengthOpts::Set, o.length = 36;
    tools::setLengths(x, sel, o);
    expect(x[0].length == 36 && x[1].length == 36 && x[2].length == 36, "none selected: every note set to one length");
  }
  {
    auto x = n;
    ops::NoteSel sel;
    tools::LengthOpts o;
    o.mode = tools::LengthOpts::Add, o.delta = -24;
    tools::setLengths(x, sel, o);
    expect(x[0].length == 24 && x[1].length == 24 && x[2].length == 12, "shorter by an amount: a short note stays as it is");
    o.delta = 24;
    tools::setLengths(x, sel, o);
    expect(x[0].length == 48 && x[2].length == 36, "longer by an amount");
  }
  {
    auto x = n;
    ops::NoteSel sel;
    tools::LengthOpts o;
    o.mode = tools::LengthOpts::Scale, o.factor = 0.7, o.snapEnds = true, o.grid = 24;
    tools::setLengths(x, sel, o);
    expect(x[0].length == 24 && x[1].length == 24 && x[2].length == 24, "ends on the grid, a note keeps at least one cell");
  }
}

void testColours() {
  Rig r;
  const Rgb c1 = r.d.song().channel(r.kick)->color, c2 = r.d.song().channel(r.lead)->color;
  expect(c1 != c2, "varied (the default): neighbours differ");
  Document::NewColors nc;
  nc.channels = Document::NewColors::One, nc.channel = 0x123456;
  nc.patterns = Document::NewColors::One, nc.pattern = 0x654321;
  r.d.setNewColors(nc);
  const ObjectId a = r.d.addChannel("A", "gl.synth"), b = r.d.addChannel("B", "gl.synth");
  expect(r.d.song().channel(a)->color == 0x123456 && r.d.song().channel(b)->color == 0x123456, "one colour for every new channel");
  const ObjectId p = r.d.addPattern("P");
  expect(r.d.song().pattern(p)->color == 0x654321, "one colour for every new pattern");
  expect(r.d.song().channel(r.kick)->color == c1, "existing channels keep their colours");
  nc.patterns = Document::NewColors::Channel, nc.channelHint = 0xABCDEF, nc.hasHint = true;
  r.d.setNewColors(nc);
  expect(r.d.song().pattern(r.d.addPattern("Q"))->color == 0xABCDEF, "a new pattern takes the selected channel's colour");
  const ObjectId q = ops::channelsToNewPattern(r.d, r.pat, {r.lead}, false);
  expect(r.d.song().pattern(q)->color == r.d.song().channel(r.lead)->color, "a channel's sequence in a new pattern takes that channel's colour");
}

// Hiding and showing again reach a collaborator; one person's undo takes it back for both.
void testCollab() {
  Clock clock;
  Net net(7801);
  Peer a, b;
  a.label = "Alex", b.label = "Sam";
  a.id = crypto::Identity::generate(), b.id = crypto::Identity::generate();
  a.doc.reset(makeEmptySong());
  const ObjectId kick = a.doc.addChannel("Kick", "gl.kick");
  a.folder = tmpDir("rack_tools_a"), b.folder = tmpDir("rack_tools_b");
  std::string err;
  a.s = Session::create(a.doc, config(a.folder, a.id, a.label, &clock), &err);
  net.add(&a);
  b.s = Session::join(b.doc, config(b.folder, b.id, b.label, &clock), ticketOf(*a.s), &err);
  a.s->admitMember(memberOf(b));
  net.add(&b);
  net.connect(&a, &b);
  settle(net, {&a, &b}, clock);
  ops::setChannelsRackHidden(a.doc, {kick}, true);
  settle(net, {&a, &b}, clock);
  expect(b.doc.song().channel(kick) && b.doc.song().channel(kick)->rackHidden, "a hidden channel is hidden for the collaborator");
  const ObjectId c = b.doc.cloneChannel(kick, Document::CloneNotes::None);
  settle(net, {&a, &b}, clock);
  expect(a.doc.song().channel(c) && !a.doc.song().channel(c)->rackHidden, "a clone made by the collaborator arrives");
  a.doc.undo();
  settle(net, {&a, &b}, clock);
  expect(!b.doc.song().channel(kick)->rackHidden && songText(a) == songText(b), "undo shows it again for both");
}

}  // namespace

int main() {
  testClone();
  testHide();
  testNewPattern();
  testLengths();
  testColours();
  testCollab();
  return finish();
}
