// Gridline - K-141: undo and redo in a collaborative project. Each person has their own
// history: Ctrl+Z takes back one of your own steps per press, newest first, and only what is
// still yours (a collaborator's later edit stays and you are told); redo puts back what you
// undid. Two and three peers wired together in memory (tests/collab/CollabSim.h), random
// delays, reordering and partitions with fixed seeds.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>
#include "Check.h"
#include "collab/CollabSim.h"
#include "collab/LocalState.h"
#include "core/Project.h"

using namespace gl;
using namespace gl::collab;
using namespace sim;

namespace {

struct Trio {
  Clock clock;
  Net net;
  Peer a, b, c;
  std::vector<std::string> notices;  // what A was told
  explicit Trio(uint32_t seed, const std::string& name, bool three = false) : net(seed) {
    a.label = "Alex";
    b.label = "Sam";
    c.label = "Kim";
    for (Peer* p : {&a, &b, &c}) p->id = crypto::Identity::generate();
    a.doc.reset(makeEmptySong());
    a.doc.addChannel("Kick", "gl.kick");
    a.doc.addChannel("Lead", "gl.synth");
    a.folder = tmpDir("k141_" + name + "_a");
    b.folder = tmpDir("k141_" + name + "_b");
    c.folder = tmpDir("k141_" + name + "_c");
    std::string err;
    a.s = Session::create(a.doc, config(a.folder, a.id, a.label, &clock), &err);
    a.s->onNotice = [this](const std::string& t, bool) { notices.push_back(t); };
    net.add(&a);
    join(b);
    if (three) join(c);
    sync();
  }
  void join(Peer& p) {
    std::string err;
    p.s = Session::join(p.doc, config(p.folder, p.id, p.label, &clock), ticketOf(*a.s), &err);
    a.s->admitMember(memberOf(p));
    net.add(&p);
    net.connect(&a, &p);
    if (&p == &c) net.connect(&b, &c);
  }
  std::vector<Peer*> all() { return c.s ? std::vector<Peer*>{&a, &b, &c} : std::vector<Peer*>{&a, &b}; }
  void sync() { settle(net, all(), clock); }
  ObjectId lead() const { return a.doc.song().channels[1].id; }
  ObjectId kick() const { return a.doc.song().channels[0].id; }
  std::string lastNotice() const { return notices.empty() ? std::string() : notices.back(); }
};

bool same(const Peer& x, const Peer& y) { return songText(x) == songText(y); }
bool near(double a, double b) { return std::abs(a - b) < 1e-5; }
double vol(const Peer& p, ObjectId ch) {
  const Channel* c = p.doc.song().channel(ch);
  return c ? c->gain : -1;
}
double pan(const Peer& p, ObjectId ch) {
  const Channel* c = p.doc.song().channel(ch);
  return c ? c->pan : -9;
}
const Clip* clipOf(const Peer& p, ObjectId id) { return p.doc.song().clip(id); }
bool has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }
void setVol(Peer& p, ObjectId ch, double v) { p.doc.setParam(channelParam(ch, ChannelParam::Volume), v, false); }
void setPan(Peer& p, ObjectId ch, double v) { p.doc.setParam(channelParam(ch, ChannelParam::Pan), v, false); }

// C1: the same knob changed several times by one person: every press walks one step.
void testSameRegisterChain() {
  Trio t(101, "chain");
  const ObjectId L = t.lead();
  const double v0 = vol(t.a, L);
  for (double v : {0.1, 0.2, 0.3}) {
    setVol(t.a, L, v);
    t.sync();
  }
  std::vector<double> seen;
  for (int i = 0; i < 3; ++i) {
    t.a.doc.undo();
    t.sync();
    seen.push_back(vol(t.b, L));
  }
  for (int i = 0; i < 3; ++i) {
    t.a.doc.redo();
    t.sync();
    seen.push_back(vol(t.b, L));
  }
  expect(near(seen[0], 0.2) && near(seen[1], 0.1) && near(seen[2], v0) && near(seen[3], 0.1) && near(seen[4], 0.2) && near(seen[5], 0.3),
         "undo and redo walk the same knob one press at a time, for everyone");
  expect(t.notices.empty(), "nothing to tell when every step undoes and redoes whole");
}

// C1 for a clip's placement, an object's presence and a list order.
void testOtherRegisterChains() {
  {
    Trio t(102, "clipchain");
    const ObjectId pat = t.a.doc.song().patterns[0].id;
    const ObjectId cl = t.a.doc.addClip(pat, 0, 0);
    t.sync();
    t.a.doc.moveClip(cl, 1, 96 * 4);
    t.sync();
    t.a.doc.moveClip(cl, 2, 96 * 8);
    t.sync();
    t.a.doc.undo();
    t.a.doc.undo();
    t.sync();
    const Clip* c = clipOf(t.b, cl);
    expect(c && c->track == 0 && c->start == 0 && same(t.a, t.b), "a clip moved twice, two undos: back where it was");
  }
  {
    Trio t(103, "createdelete");
    const ObjectId pat = t.a.doc.song().patterns[0].id;
    const ObjectId cl = t.a.doc.addClip(pat, 0, 0);
    t.sync();
    t.a.doc.removeClip(cl);
    t.sync();
    t.a.doc.undo();
    t.sync();
    const bool back = clipOf(t.b, cl) != nullptr;
    t.a.doc.undo();
    t.sync();
    const bool gone = !clipOf(t.b, cl);
    t.a.doc.redo();
    t.sync();
    expect(back && gone && clipOf(t.b, cl), "add then delete a clip: undo brings it back, undo again removes it, redo adds it");
  }
  {
    Trio t(119, "order");
    t.a.doc.addChannel("Third", "gl.hat");
    t.sync();
    auto order = [](const Peer& p) {
      std::string s;
      for (const auto& c : p.doc.song().channels) s += c.name + ",";
      return s;
    };
    const std::string o0 = order(t.a);
    const ObjectId k = t.kick();
    t.a.doc.moveChannel(k, 2);
    t.sync();
    t.a.doc.moveChannel(k, 1);
    t.sync();
    t.a.doc.undo();
    t.a.doc.undo();
    t.sync();
    expect(order(t.b) == o0 && same(t.a, t.b), "a channel moved twice in the rack, two undos: back in place");
  }
  {
    Trio t(121, "jump");
    const ObjectId L = t.lead(), K = t.kick();
    setVol(t.a, L, 0.1);
    t.sync();
    setVol(t.b, K, 0.6);
    t.sync();
    setVol(t.a, L, 0.2);
    setPan(t.a, L, 0.2);
    t.sync();
    t.a.doc.jumpTo(0);
    t.sync();
    expect(near(vol(t.b, L), 0.78) && near(pan(t.b, L), 0) && near(vol(t.b, K), 0.6), "jumping to the start of your history undoes only yours");
    t.a.doc.jumpTo(3);
    t.sync();
    expect(near(vol(t.b, L), 0.2) && near(pan(t.b, L), 0.2) && near(vol(t.b, K), 0.6), "jumping back to the end redoes only yours");
  }
}

// Edits of two people interleaved on different things.
void testInterleaved() {
  Trio t(104, "interleave");
  const ObjectId L = t.lead(), K = t.kick();
  const double l0 = vol(t.a, L), k0 = vol(t.a, K);
  setVol(t.a, L, 0.3);
  t.sync();
  setVol(t.b, K, 0.4);
  t.sync();
  setPan(t.a, L, 0.5);
  t.sync();
  setPan(t.b, K, -0.5);
  t.sync();
  t.a.doc.undo();
  t.sync();
  expect(near(pan(t.a, L), 0) && near(pan(t.a, K), -0.5), "your undo takes back your latest edit; the collaborator's later one stays");
  t.b.doc.undo();
  t.sync();
  expect(near(pan(t.b, K), 0) && near(vol(t.b, L), 0.3), "their undo takes back their latest edit only");
  t.a.doc.undo();
  t.b.doc.undo();
  t.sync();
  expect(near(vol(t.a, L), l0) && near(vol(t.a, K), k0) && same(t.a, t.b), "both undo again: everything back to the start");
  t.a.doc.redo();
  t.sync();
  expect(near(vol(t.b, L), 0.3) && near(vol(t.b, K), k0), "your redo puts back only your edit");
  t.b.doc.redo();
  t.b.doc.redo();
  t.a.doc.redo();
  t.sync();
  expect(near(vol(t.a, L), 0.3) && near(vol(t.a, K), 0.4) && near(pan(t.a, L), 0.5) && near(pan(t.a, K), -0.5) && same(t.a, t.b),
         "all redos: every edit back");
}

// D2 / D6: a collaborator's later edit of the same thing stays, for undo and for redo, with a message.
void testTheirsStays() {
  {
    Trio t(105, "redoafter");
    const ObjectId L = t.lead();
    setVol(t.a, L, 0.2);
    t.sync();
    t.a.doc.undo();
    t.sync();
    setVol(t.b, L, 0.6);
    t.sync();
    expect(t.a.doc.history().canRedo(), "a collaborator's edit keeps your redo");
    t.a.doc.redo();
    t.sync();
    expect(near(vol(t.a, L), 0.6) && near(vol(t.b, L), 0.6), "redo never clobbers a newer edit by someone else");
    expect(has(t.lastNotice(), "Nothing left to redo") && has(t.lastNotice(), "Sam"), "and says who changed it");
  }
  {
    Trio t(115, "nothingleft");
    const ObjectId L = t.lead();
    t.a.doc.renameChannel(L, "Mine");
    t.sync();
    setVol(t.a, L, 0.2);
    t.sync();
    setVol(t.b, L, 0.9);
    t.sync();
    const bool ok = t.a.doc.undo();
    t.sync();
    expect(ok && near(vol(t.a, L), 0.9) && t.a.doc.song().channel(L)->name == "Mine", "D5: the press uses your overwritten step and changes nothing");
    expect(has(t.lastNotice(), "Nothing left to undo") && has(t.lastNotice(), "Sam"), "D5: you are told who changed it after you");
    const auto* st = dynamic_cast<const CollabStep*>(t.a.doc.stepNote(1, NoteSlot::Collab));
    expect(st && st->state == CollabStep::State::NothingUndone, "the history panel shows the step as overwritten");
    t.a.doc.undo();
    t.sync();
    expect(t.b.doc.song().channel(L)->name == "Lead", "the next press takes back your step before it");
  }
  {
    Trio t(120, "partial");
    const ObjectId L = t.lead();
    t.a.doc.beginGroup("two knobs");
    setVol(t.a, L, 0.2);
    setPan(t.a, L, 0.4);
    t.a.doc.endGroup();
    t.sync();
    setPan(t.b, L, -0.7);
    t.sync();
    t.a.doc.undo();
    t.sync();
    expect(near(vol(t.b, L), 0.78) && near(pan(t.b, L), -0.7), "a partly overwritten step undoes the rest and keeps the collaborator's part");
    expect(has(t.lastNotice(), "except what Sam changed"), "and says so");
  }
  {
    Trio t(130, "putback");
    const ObjectId L = t.lead();
    setVol(t.a, L, 0.2);
    t.sync();
    setVol(t.b, L, 0.6);
    t.sync();
    t.b.doc.undo();  // Sam takes his edit back: the knob is as Alex left it again
    t.sync();
    t.a.doc.undo();
    t.sync();
    expect(near(vol(t.b, L), 0.78), "a collaborator who undid their change leaves your step undoable");
  }
  {
    Trio t(131, "notputback");
    const ObjectId L = t.lead();
    setVol(t.a, L, 0.2);
    t.sync();
    setVol(t.a, L, 0.5);
    t.sync();
    setVol(t.b, L, 0.2);  // Sam sets a value that happens to equal Alex's older one
    t.sync();
    t.a.doc.undo();
    t.a.doc.undo();
    t.sync();
    expect(near(vol(t.b, L), 0.2), "a collaborator's later value stays even when it equals one of yours");
  }
}

// D3: undo of something you created that a collaborator has since used: kept, with a message.
void testCreatedThenUsed() {
  {
    Trio t(106, "clipused");
    const ObjectId pat = t.a.doc.song().patterns[0].id;
    const ObjectId cl = t.a.doc.addClip(pat, 0, 0);
    t.sync();
    t.b.doc.moveClip(cl, 3, 96 * 16);
    t.sync();
    t.a.doc.undo();
    t.sync();
    const Clip* c = clipOf(t.b, cl);
    expect(c && c->track == 3 && clipOf(t.a, cl), "undo of a clip a collaborator has since moved keeps it");
    expect(has(t.lastNotice(), "Kept") && has(t.lastNotice(), "Sam"), "and says why");
  }
  {
    Trio t(117, "chanused");
    const ObjectId pat = t.a.doc.song().patterns[0].id;
    const ObjectId ch = t.a.doc.addChannel("New", "gl.synth");
    t.sync();
    t.b.doc.addNote(pat, ch, Note(0, 96, 60, 100));
    t.sync();
    t.a.doc.undo();
    t.sync();
    const Pattern* p = t.b.doc.song().pattern(pat);
    expect(t.b.doc.song().channel(ch) && p && p->notes.count(ch) && !p->notes.at(ch).empty(),
           "undo of a channel a collaborator has since written notes for keeps it and their notes");
    expect(has(t.lastNotice(), "channel New"), "the message names what was kept");
  }
  {
    Trio t(132, "patused");
    const ObjectId pat = t.a.doc.addPattern("Mine");
    t.sync();
    const ObjectId cl = t.b.doc.addClip(pat, 1, 0);
    t.sync();
    t.a.doc.undo();
    t.sync();
    expect(t.b.doc.song().pattern(pat) && clipOf(t.b, cl), "undo of a pattern a collaborator has since placed keeps it and their clip");
  }
  {
    Trio t(133, "unused");
    const ObjectId pat = t.a.doc.song().patterns[0].id;
    const ObjectId cl = t.a.doc.addClip(pat, 0, 0);
    t.sync();
    t.b.doc.moveClip(cl, 3, 96 * 16);
    t.sync();
    t.b.doc.undo();  // Sam puts it back where it was
    t.sync();
    t.a.doc.undo();
    t.sync();
    expect(!clipOf(t.b, cl), "an object a collaborator touched and put back is yours to remove");
  }
  {
    Trio t(112, "notes");
    const ObjectId L = t.lead();
    const ObjectId pat = t.a.doc.song().patterns[0].id;
    t.a.doc.addNote(pat, L, Note(0, 96, 60, 100));
    t.sync();
    t.b.doc.addNote(pat, L, Note(96, 96, 62, 100));
    t.sync();
    t.a.doc.addNote(pat, L, Note(192, 96, 64, 100));
    t.sync();
    t.a.doc.undo();
    t.a.doc.undo();
    t.sync();
    const Pattern* p = t.b.doc.song().pattern(pat);
    const auto it = p->notes.find(L);
    expect(it != p->notes.end() && it->second.size() == 1 && it->second[0].pitch == 62 && same(t.a, t.b),
           "your two undos remove your two notes and keep the collaborator's note between them");
    t.a.doc.redo();
    t.a.doc.redo();
    t.sync();
    const Pattern* q = t.b.doc.song().pattern(pat);
    expect(q->notes.count(L) && q->notes.at(L).size() == 3 && same(t.a, t.b), "two redos bring your notes back next to theirs");
  }
}

// D3 for deletes, and D4: a clip's placement is undone whole or not at all.
void testDeletesAndPlacement() {
  {
    Trio t(107, "deleteedit");
    const ObjectId L = t.lead();
    t.net.disconnect(&t.a, &t.b);
    t.a.doc.removeChannel(L);
    run(t.net, t.all(), t.clock, 0.5);
    t.b.doc.renameChannel(L, "Sam's lead");
    run(t.net, t.all(), t.clock, 1.0);
    t.net.connect(&t.a, &t.b);
    t.sync();
    t.a.doc.undo();
    t.sync();
    const Channel* c = t.b.doc.song().channel(L);
    expect(c && c->name == "Sam's lead" && same(t.a, t.b), "undo of a delete brings it back with the collaborator's rename made meanwhile");
  }
  {
    Trio t(134, "deleteddelete");
    const ObjectId pat = t.a.doc.song().patterns[0].id;
    const ObjectId cl = t.a.doc.addClip(pat, 0, 0);
    t.sync();
    t.a.doc.removeClip(cl);
    t.sync();
    t.a.doc.undo();  // back
    t.sync();
    t.b.doc.removeClip(cl);  // Sam deletes it again
    t.sync();
    t.a.doc.redo();  // redo of Alex's delete: already gone
    t.a.doc.undo();  // undo of Alex's delete: Sam's delete stays
    t.sync();
    expect(!clipOf(t.b, cl) && same(t.a, t.b), "undo of your delete leaves an object a collaborator deleted since");
  }
  {
    Trio t(135, "placement");
    const ObjectId pat = t.a.doc.song().patterns[0].id;
    const ObjectId cl = t.a.doc.addClip(pat, 0, 0);
    t.sync();
    t.a.doc.moveClip(cl, 2, 96 * 4);  // track and start
    t.sync();
    t.b.doc.moveClip(cl, 2, 96 * 6);  // Sam slides it along the same track
    t.sync();
    t.a.doc.undo();
    t.sync();
    const Clip* c = clipOf(t.b, cl);
    expect(c && c->track == 2 && c->start == 96 * 6, "D4: a clip never lands half moved (the whole placement stays as Sam left it)");
  }
}

// C2: an edit that merges is closed by a collaborator's edit of the same thing.
void testMergeAcrossPeer() {
  Trio t(108, "merge");
  const ObjectId L = t.lead();
  t.a.doc.setParam(channelParam(L, ChannelParam::Volume), 0.2, true);
  t.sync();
  setVol(t.b, L, 0.9);
  t.sync();
  t.a.doc.setParam(channelParam(L, ChannelParam::Volume), 0.3, true);  // no gesture end in between
  t.sync();
  t.a.doc.undo();
  t.sync();
  expect(near(vol(t.b, L), 0.9), "your later edit is a step of its own when a collaborator edited in between");
  t.a.doc.undo();
  t.sync();
  expect(near(vol(t.b, L), 0.9), "and your first edit stays overwritten by theirs");
}

// Gestures and drags.
void testGestures() {
  {
    Trio t(113, "gesture");
    const ObjectId L = t.lead(), K = t.kick();
    t.a.doc.beginPointerGesture();
    t.a.s->setGesture(true);
    for (int i = 1; i <= 10; ++i) {
      t.a.doc.setParam(channelParam(L, ChannelParam::Volume), 0.3 + i * 0.01, true);
      t.a.doc.setParam(channelParam(L, ChannelParam::Pan), i * 0.05, true);
      run(t.net, t.all(), t.clock, 0.016);
    }
    t.a.doc.endPointerGesture();
    t.a.s->setGesture(false);
    t.sync();
    setVol(t.b, K, 0.33);
    t.sync();
    t.a.doc.undo();
    t.sync();
    expect(near(vol(t.b, L), 0.78) && near(pan(t.b, L), 0) && near(vol(t.b, K), 0.33), "one gesture, one undo; the collaborator's later edit stays");
    expect(!t.a.doc.history().canUndo(), "the gesture was a single step (and the history started when sharing did)");
  }
  {
    Trio t(109, "peerdrag");
    const ObjectId L = t.lead();
    setVol(t.a, L, 0.2);
    t.sync();
    for (int i = 1; i <= 30; ++i) {
      t.b.s->setGesture(true);
      t.b.doc.setParam(channelParam(L, ChannelParam::Volume), 0.3 + i * 0.01, true);
      if (i == 10) t.a.doc.undo();
      run(t.net, t.all(), t.clock, 0.016);
    }
    t.b.doc.endGesture();
    t.b.s->setGesture(false);
    t.sync();
    expect(near(vol(t.a, L), 0.6) && near(vol(t.b, L), 0.6), "a collaborator's drag wins over an undo made during it");
  }
  {
    Trio t(116, "sameframe");
    const ObjectId L = t.lead();
    setVol(t.a, L, 0.2);
    t.a.doc.renameChannel(L, "X");
    setPan(t.a, L, 0.3);
    t.a.doc.undo();
    t.a.doc.undo();
    t.sync();
    const Channel* c = t.b.doc.song().channel(L);
    expect(c && near(c->gain, 0.2) && c->name == "Lead" && near(c->pan, 0) && same(t.a, t.b), "edits and undos within one frame");
  }
}

// Three peers: A first, then the others; and everyone in reverse order of the edits.
void testThreePeers() {
  auto rounds = [](Trio& t) {
    const ObjectId L = t.lead(), K = t.kick();
    std::vector<Peer*> order;
    for (int round = 0; round < 3; ++round) {
      setVol(t.a, L, 0.1 + round * 0.1);
      t.sync();
      order.push_back(&t.a);
      setPan(t.b, L, -0.1 - round * 0.1);
      t.sync();
      order.push_back(&t.b);
      t.c.doc.renameChannel(L, "C" + std::to_string(round));
      t.sync();
      order.push_back(&t.c);
      setVol(t.a, K, 0.2 + round * 0.1);
      t.sync();
      order.push_back(&t.a);
      setVol(t.b, K, 0.25 + round * 0.1);
      t.sync();
      order.push_back(&t.b);
    }
    return order;
  };
  {
    Trio t(110, "three", true);
    const ObjectId L = t.lead(), K = t.kick();
    rounds(t);
    for (int i = 0; i < 6; ++i) t.a.doc.undo();
    t.sync();
    expect(near(vol(t.a, L), 0.78) && near(vol(t.a, K), 0.45), "three peers: Alex undoes all of his: his Lead volume is back, Sam's Kick volume stays");
    for (int i = 0; i < 6; ++i) t.b.doc.undo();
    for (int i = 0; i < 3; ++i) t.c.doc.undo();
    t.sync();
    const Channel* c = t.a.doc.song().channel(L);
    expect(c && near(c->pan, 0) && c->name == "Lead" && same(t.a, t.b) && same(t.a, t.c), "then Sam and Kim undo all of theirs: pan and name back");
  }
  {
    Trio t(140, "reverse", true);
    const std::string start = songText(t.a);
    const auto order = rounds(t);
    const std::string end = songText(t.a);
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
      (*it)->doc.undo();
      t.sync();
    }
    expect(songText(t.a) == start && same(t.a, t.b) && same(t.a, t.c), "everyone undoing in reverse order of the edits gives back the start");
    for (Peer* p : order) {
      p->doc.redo();
      t.sync();
    }
    expect(songText(t.a) == end && same(t.a, t.c), "and redoing in order gives back the end");
  }
}

// Offline edits and reconnecting.
void testReconnect() {
  Trio t(111, "reconnect");
  const ObjectId L = t.lead();
  setVol(t.a, L, 0.2);
  t.sync();
  t.net.disconnect(&t.a, &t.b);
  setPan(t.a, L, 0.4);
  run(t.net, t.all(), t.clock, 1.0);
  setPan(t.b, L, -0.4);  // later: wins the merge
  run(t.net, t.all(), t.clock, 1.0);
  t.net.connect(&t.a, &t.b);
  t.sync();
  t.a.doc.undo();  // the offline pan lost the merge: nothing of it to take back
  t.sync();
  expect(near(pan(t.a, L), -0.4) && near(vol(t.a, L), 0.2), "undo of an offline edit that lost the merge changes nothing else");
  t.a.doc.undo();
  t.sync();
  expect(near(vol(t.b, L), 0.78) && same(t.a, t.b), "the next undo takes back the edit made before going offline");
}

// Redo kept and cleared.
void testRedoList() {
  Trio t(114, "redolist");
  const ObjectId L = t.lead(), K = t.kick();
  setVol(t.a, L, 0.2);
  t.sync();
  t.a.doc.undo();
  t.sync();
  t.b.doc.setParam(channelParam(K, ChannelParam::Volume), 0.5, false);
  t.b.doc.undo();
  t.b.doc.redo();
  t.sync();
  expect(t.a.doc.history().canRedo(), "a collaborator's edits, undos and redos keep your redo");
  t.a.doc.redo();
  t.sync();
  expect(near(vol(t.b, L), 0.2), "the redo still applies");
  t.a.doc.undo();
  setPan(t.a, K, 0.5);
  t.sync();
  expect(!t.a.doc.history().canRedo(), "your own new edit clears your redo");
}

// C5: settings that stay on this computer (solo, arm ...) undo in a collaborative project.
void testLocalOnly() {
  Trio t(122, "local");
  const ObjectId mx = t.a.doc.song().mixer[1].id;
  t.a.doc.editMixerTrack(mx, "solo", [](MixerTrack& m) { m.solo = true; });
  t.sync();
  t.a.doc.undo();
  t.sync();
  expect(!t.a.doc.song().mixerTrack(mx)->solo, "undo of a solo works in a collaborative project");
  t.a.doc.redo();
  t.sync();
  expect(t.a.doc.song().mixerTrack(mx)->solo && !t.b.doc.song().mixerTrack(mx)->solo, "redo solos again, on this computer only");
  t.a.doc.editMixerTrack(mx, "solo and volume", [](MixerTrack& m) {
    m.solo = false;
    m.armed = true;
    m.volume = 0.3f;
  });
  t.sync();
  t.a.doc.undo();
  t.sync();
  const MixerTrack* m = t.a.doc.song().mixerTrack(mx);
  expect(m->solo && !m->armed && near(m->volume, 0.8) && near(t.b.doc.song().mixerTrack(mx)->volume, 0.8),
         "a step with a shared and a local part undoes both");
  const ObjectId pat = t.a.doc.song().patterns[0].id;
  const ObjectId cl = t.a.doc.addClip(pat, 0, 0);
  t.sync();
  t.a.doc.editClip(cl, "launch", [](Clip& c) { c.launched = true; });
  t.a.doc.editPlaylistTrack(0, "fold", [](PlaylistTrack& tr) { tr.folded = true; });
  t.sync();
  t.a.doc.undo();
  t.a.doc.undo();
  t.sync();
  expect(!clipOf(t.a, cl)->launched && !t.a.doc.song().tracks[0].folded, "a clip's launch and a track's fold undo too");
}

// C6, C7, C8: the records last as long as the steps; the history starts when sharing does.
void testLimits() {
  {
    Trio t(123, "levels");
    t.a.doc.setMaxUndoLevels(400);
    const ObjectId L = t.lead();
    for (int i = 0; i < 300; ++i) {
      setPan(t.a, L, (i % 2 ? 0.1 : -0.1) * (1 + i % 5));
      t.a.doc.renameChannel(L, "N" + std::to_string(i));
      if (i % 25 == 0) t.sync();
    }
    t.sync();
    for (int i = 0; i < 600; ++i) t.a.doc.undo();
    t.sync();
    expect(t.b.doc.song().channel(L)->name == "N99" && t.a.doc.history().position() == 0, "with 400 levels, all 400 kept steps undo");
  }
  {
    Trio t(124, "cap");
    t.a.doc.setMaxUndoLevels(400);
    const ObjectId L = t.lead();
    const ObjectId pat = t.a.doc.song().patterns[0].id;
    t.a.doc.addNote(pat, L, Note(0, 12, 30, 100));
    t.sync();
    for (int i = 1; i <= 300; ++i) {
      t.a.doc.addNote(pat, L, Note(i * 24, 12, 40 + i % 40, 100));
      if (i % 20 == 0) t.sync();
    }
    t.sync();
    for (int i = 0; i < 300; ++i) t.a.doc.undo();
    t.sync();
    const Pattern* p = t.b.doc.song().pattern(pat);
    expect(p->notes.count(L) && p->notes.at(L).size() == 1, "300 note adds above the old 256 record cap all undo");
  }
  {
    Trio t(125, "share");
    expect(!t.a.doc.history().canUndo() && t.a.doc.history().size() == 0, "D8: the history starts fresh when a project is shared");
    expect(!t.b.doc.history().canUndo(), "and a joiner's history starts when the project arrives");
  }
  {
    // a joiner who edits before the project arrived: those edits never reached anyone
    Trio t(126, "early");
    Peer& c = t.c;
    c.label = "Kim";
    std::string err;
    c.s = Session::join(c.doc, config(c.folder, c.id, c.label, &t.clock), ticketOf(*t.a.s), &err);
    c.doc.addChannel("Too early", "gl.kick");
    t.a.s->admitMember(memberOf(c));
    t.net.add(&c);
    t.net.connect(&t.a, &c);
    t.sync();
    expect(c.s->ready() && !c.doc.history().canUndo() && same(t.a, c), "C8: a joiner's edits from before the project arrived are not in their history");
  }
}

// D5b: skipping steps with nothing left, when turned on.
void testSkipSetting() {
  Trio t(127, "skip");
  const ObjectId L = t.lead();
  t.a.s->setSkipEmptyUndo(true);
  t.a.doc.renameChannel(L, "Mine");
  t.sync();
  setVol(t.a, L, 0.2);
  t.sync();
  setVol(t.b, L, 0.9);
  t.sync();
  t.a.doc.undo();
  t.sync();
  expect(t.b.doc.song().channel(L)->name == "Lead" && near(vol(t.b, L), 0.9), "with skipping on, one press goes on to your step that still changes something");
  expect(has(t.lastNotice(), "Skipped 1"), "and says what it skipped");
}

// D9: your own history survives closing and reopening a collaborative project.
void testPersistence() {
  Trio t(128, "persist");
  const ObjectId L = t.lead();
  setVol(t.a, L, 0.2);
  t.sync();
  t.a.doc.renameChannel(L, "Before close");
  t.sync();
  t.a.doc.undo();  // one step in the redo list
  t.sync();
  const std::string folder = t.a.folder;
  t.net.disconnect(&t.a, &t.b);
  t.a.s.reset();
  Peer a2;
  a2.label = "Alex";
  a2.id = t.a.id;
  a2.folder = folder;
  std::string err;
  a2.s = Session::open(a2.doc, config(folder, a2.id, "Alex", &t.clock), &err);
  t.a.s.reset();
  expect(a2.s && a2.doc.history().size() == 2 && a2.doc.history().position() == 1 && a2.doc.history().canRedo(),
         "D9: the history (and the redo list) come back after reopening");
  if (!a2.s) return;
  t.net.add(&a2);
  t.net.connect(&a2, &t.b);
  settle(t.net, {&a2, &t.b}, t.clock);
  a2.doc.redo();
  settle(t.net, {&a2, &t.b}, t.clock);
  expect(t.b.doc.song().channel(L)->name == "Before close", "redo after reopening reaches the collaborator");
  a2.doc.undo();
  a2.doc.undo();
  settle(t.net, {&a2, &t.b}, t.clock);
  expect(t.b.doc.song().channel(L)->name == "Lead" && near(vol(t.b, L), 0.78) && songText(a2) == songText(t.b), "and so do two undos");
  a2.s.reset();
}

// C8: steps made while previewing a side of a conflict are not kept.
void testPreviewEdits() {
  Trio t(129, "preview");
  const ObjectId L = t.lead();
  t.net.disconnect(&t.a, &t.b);
  t.a.doc.renameChannel(L, "Lead by Alex");
  run(t.net, t.all(), t.clock, 0.05);
  t.clock.ms += 60000;
  t.b.doc.renameChannel(L, "Lead by Sam");
  run(t.net, t.all(), t.clock, 0.2);
  t.net.connect(&t.a, &t.b);
  t.sync();
  const ConflictObject* chan = nullptr;
  for (const ConflictObject* c : t.a.s->conflicts().open()) chan = c;
  if (!chan) {
    expect(false, "the offline renames make a conflict to preview");
    return;
  }
  const size_t before = t.a.doc.history().size();
  t.a.s->preview(chan->object, t.a.s->site());
  t.a.doc.renameChannel(t.kick(), "Edited on the preview");
  t.a.s->endPreview();
  t.sync();
  expect(t.a.doc.history().size() == before && t.b.doc.song().channels[0].name != "Edited on the preview",
         "an edit made on a preview is not kept in the history");
  expect(!t.a.doc.undo() || t.a.doc.history().size() <= before, "undo after the preview works on your real steps");
}

// Random edits by three peers, then everyone undoes in reverse order: always the start again.
void testRandomReverse(uint32_t seed) {
  Trio t(seed, "random" + std::to_string(seed), true);
  std::mt19937 r(seed);
  const std::string start = songText(t.a);
  const ObjectId pat = t.a.doc.song().patterns[0].id;
  std::vector<Peer*> order;
  std::vector<ObjectId> clips;
  for (int i = 0; i < 40; ++i) {
    Peer* p = t.all()[r() % 3];
    const ObjectId ch = p->doc.song().channels[r() % p->doc.song().channels.size()].id;
    const uint64_t before = p->doc.pushCount();
    switch (r() % 7) {
      case 0: setVol(*p, ch, (r() % 100) / 100.0); break;
      case 1: setPan(*p, ch, (static_cast<int>(r() % 200) - 100) / 100.0); break;
      case 2: p->doc.renameChannel(ch, "N" + std::to_string(r() % 50)); break;
      case 3: p->doc.addNote(pat, ch, Note(static_cast<Tick>(r() % 16) * 24, 24, 48 + static_cast<int>(r() % 24), 100)); break;
      case 4: clips.push_back(p->doc.addClip(pat, static_cast<int>(r() % 4), static_cast<Tick>(r() % 16) * 96)); break;
      case 5:
        if (!clips.empty()) p->doc.moveClip(clips[r() % clips.size()], static_cast<int>(r() % 4), static_cast<Tick>(r() % 16) * 96);
        break;
      case 6: p->doc.setStep(pat, ch, static_cast<int>(r() % 16), r() % 2 ? 100 : 0); break;
    }
    if (p->doc.pushCount() != before) order.push_back(p);
    t.sync();
  }
  const std::string end = songText(t.a);
  for (auto it = order.rbegin(); it != order.rend(); ++it) {
    (*it)->doc.undo();
    t.sync();
  }
  char name[160];
  std::snprintf(name, sizeof name, "random edits by 3 peers (seed %u, %zu steps): undo in reverse order gives back the start", seed, order.size());
  expect(songText(t.a) == start && same(t.a, t.b) && same(t.a, t.c), name);
  const size_t told = t.notices.size();
  for (Peer* p : order) {
    p->doc.redo();
    t.sync();
  }
  std::snprintf(name, sizeof name, "random edits by 3 peers (seed %u): redo in order gives back the end", seed);
  const std::string now = songText(t.a);
  expect(now == end && same(t.a, t.b) && same(t.a, t.c), name);
  if (now != end) {  // where it differs, for the log
    size_t i = 0;
    while (i < now.size() && i < end.size() && now[i] == end[i]) ++i;
    const size_t from = i > 160 ? i - 160 : 0;
    std::printf("    # expected ...%s\n    # got      ...%s\n", end.substr(from, 320).c_str(), now.substr(from, 320).c_str());
    for (size_t k = told; k < t.notices.size(); ++k) std::printf("    # told: %s\n", t.notices[k].c_str());
  }
}

// Random edits, undos and redos with concurrency and partitions: every peer converges, and an
// undo never takes back a value someone else wrote after it.
void testRandomMixed(uint32_t seed) {
  Trio t(seed, "mixed" + std::to_string(seed), true);
  t.net.dupChance = 0.05;
  std::mt19937 r(seed);
  const ObjectId L = t.lead(), K = t.kick();
  bool neverClobbered = true;
  std::map<SiteId, std::set<std::string>> wrote;  // field=value each peer has written
  auto fieldPath = [](ObjectId c, const char* f) { return Path{"song", "channels", "#" + std::to_string(c), f}; };
  for (int i = 0; i < 200; ++i) {
    Peer* p = t.all()[r() % 3];
    const ObjectId ch = r() % 2 ? L : K;
    const char* edited = nullptr;
    switch (r() % 6) {
      case 0: setVol(*p, ch, (r() % 100) / 100.0), edited = "gain"; break;
      case 1: setPan(*p, ch, (static_cast<int>(r() % 200) - 100) / 100.0), edited = "pan"; break;
      case 2: p->doc.renameChannel(ch, "N" + std::to_string(r() % 50)), edited = "name"; break;
      case 3:
      case 4: {
        // a settled undo only changes values whose newest writer is the person undoing
        t.sync();
        struct Field {
          Path path;
          SiteId writer;
          std::string value;
        };
        std::vector<Field> fields;
        for (ObjectId c : {L, K})
          for (const char* f : {"gain", "pan", "name"}) {
            Path path = {"song", "channels", "#" + std::to_string(c), f};
            const Node* n = p->s->replica().find(path);
            fields.push_back({path, n ? n->valS.site : 0, jsonCompact(p->s->replica().materialize(path))});
          }
        p->doc.undo();
        t.sync();
        for (const auto& f : fields)
          if (jsonCompact(p->s->replica().materialize(f.path)) != f.value && f.writer != p->s->site() &&
              !wrote[p->s->site()].count(joinPath(f.path) + "=" + f.value)) {  // (theirs, unless they put yours back)
            std::printf("    # seed %u step %d: %s undid %s=%s written by someone else\n", seed, i, p->label.c_str(), f.path[3].c_str(),
                        f.value.c_str());
            neverClobbered = false;
          }
        break;
      }
      case 5: p->doc.redo(); break;
    }
    if (edited) {
      p->s->flushLocal();
      const Path fp = fieldPath(ch, edited);
      wrote[p->s->site()].insert(joinPath(fp) + "=" + jsonCompact(p->s->replica().materialize(fp)));
    }
    run(t.net, t.all(), t.clock, (r() % 10) * 0.005);
    if (r() % 40 == 0) {
      Peer* x = t.all()[r() % 3];
      Peer* y = t.all()[r() % 3];
      if (x != y) {
        if (t.net.linked(x->s->site(), y->s->site())) t.net.disconnect(x, y);
        else t.net.connect(x, y);
      }
    }
  }
  t.net.connect(&t.a, &t.b);
  t.net.connect(&t.a, &t.c);
  t.net.connect(&t.b, &t.c);
  t.sync();
  char name[160];
  std::snprintf(name, sizeof name, "200 random edits, undos and redos by 3 peers with partitions converge (seed %u)", seed);
  expect(same(t.a, t.b) && same(t.a, t.c) && neverClobbered, name);
}

}  // namespace

int main() {
  testSameRegisterChain();
  testOtherRegisterChains();
  testInterleaved();
  testTheirsStays();
  testCreatedThenUsed();
  testDeletesAndPlacement();
  testMergeAcrossPeer();
  testGestures();
  testThreePeers();
  testReconnect();
  testRedoList();
  testLocalOnly();
  testLimits();
  testSkipSetting();
  testPersistence();
  testPreviewEdits();
  for (uint32_t seed : {201u, 202u, 203u, 204u, 205u}) testRandomReverse(seed);
  for (uint32_t seed : {301u, 302u, 303u}) testRandomMixed(seed);
  return finish();
}
