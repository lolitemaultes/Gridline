// Gridline - J-111: collaboration core tests (replicated document, sessions,
// undo locality, op log crash safety and compaction, file transfer, plugin
// policy, store-and-forward relay, conflicts and admin review, signatures,
// profiles). Network transport and handshake: test_collab_net.cpp.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <string>
#include <vector>
#include "Check.h"
#include "collab/CollabSim.h"
#include "collab/Crdt.h"
#include "collab/Lifecycle.h"
#include "collab/Log.h"
#include "collab/Profile.h"
#include "collab/Session.h"
#include "core/Png.h"
#include "core/Project.h"
#include "core/ProjectInterop.h"
#include "core/Png.h"
#include "core/Update.h"
#include "core/WavWriter.h"

using namespace gl;
using namespace gl::collab;
using namespace sim;

namespace {

// ------------------------------------------------------------------ CRDT ----
Json randomJson(std::mt19937& r, int depth) {
  std::uniform_int_distribution<int> k(0, depth > 2 ? 2 : 4);
  switch (k(r)) {
    case 0: return Json::makeNumber(static_cast<double>(r() % 100));
    case 1: return Json::makeString(std::string(1, static_cast<char>('a' + r() % 5)));
    case 2: {
      Json j;
      j.type = Json::Type::Bool;
      j.boolean = r() % 2;
      return j;
    }
    case 3: {
      Json o = Json::makeObject();
      const int n = static_cast<int>(r() % 4);
      for (int i = 0; i < n; ++i) o.object[std::string(1, static_cast<char>('k' + r() % 4))] = randomJson(r, depth + 1);
      return o;
    }
    default: {
      Json a = Json::makeArray();
      const int n = static_cast<int>(r() % 5);
      const bool keyed = r() % 2;
      for (int i = 0; i < n; ++i) {
        if (keyed) {
          Json o = Json::makeObject();
          o.object["id"] = Json::makeNumber(static_cast<double>(2 + r() % 6));
          o.object["v"] = randomJson(r, depth + 1);
          bool dup = false;
          for (const auto& e : a.array) dup = dup || e.num("id") == o.num("id");
          if (!dup) a.array.push_back(o);
        } else {
          a.array.push_back(randomJson(r, depth + 1));
        }
      }
      return a;
    }
  }
}

void testKeys() {
  bool ok = true;
  std::mt19937 r(7);
  std::vector<std::string> keys = {""};
  std::vector<std::string> sorted;
  for (int i = 0; i < 2000; ++i) {
    std::vector<std::string> cur = sorted;
    const size_t at = cur.empty() ? 0 : r() % (cur.size() + 1);
    const std::string a = at == 0 ? std::string() : cur[at - 1];
    const std::string b = at == cur.size() ? std::string() : cur[at];
    const std::string k = keyBetween(a, b);
    if (!(k > a) || (!b.empty() && !(k < b)) || k.back() == '0') ok = false;
    sorted.insert(sorted.begin() + static_cast<std::ptrdiff_t>(at), k);
  }
  expect(ok && std::is_sorted(sorted.begin(), sorted.end()), "fractional keys stay strictly ordered (2000 random inserts)");
  auto many = keysBetween("", "", 1000);
  expect(many.size() == 1000 && std::is_sorted(many.begin(), many.end()) &&
             std::adjacent_find(many.begin(), many.end()) == many.end(),
         "bulk keys are ordered and unique");
}

void testDiffRoundTrip() {
  std::mt19937 r(11);
  int good = 0;
  const int rounds = 400;
  uint64_t t = 1;
  for (int i = 0; i < rounds; ++i) {
    Replica rep;
    Json a = Json::makeObject();
    a.object["x"] = randomJson(r, 0);
    Op ins;
    ins.kind = OpKind::Ins;
    ins.t = t++;
    ins.path = {"doc"};
    ins.value = a;
    rep.apply(ins, 1);
    bool ok = jsonStringify(rep.materialize({"doc"}), 0) == jsonStringify(a, 0);
    for (int step = 0; step < 6 && ok; ++step) {
      Json b = Json::makeObject();
      b.object["x"] = randomJson(r, 0);
      if (r() % 3 == 0) b = a;  // no change
      std::vector<Op> ops;
      rep.diff({"doc"}, b, 1, [&] { return t++; }, ops);
      for (const auto& op : ops) rep.apply(op, 1);
      ok = jsonStringify(rep.materialize({"doc"}), 0) == jsonStringify(b, 0);
      a = b;
    }
    good += ok;
  }
  expect(good == rounds, "diff + apply reproduces every random JSON edit (400 x 6 edits)");
}

void testReplicaConvergence() {
  // Two authors edit concurrently; three replicas apply all ops in different orders, with duplicates.
  std::mt19937 r(23);
  int good = 0;
  const int rounds = 200;
  for (int i = 0; i < rounds; ++i) {
    Replica base;
    Json start = Json::makeObject();
    start.object["x"] = randomJson(r, 0);
    Op ins;
    ins.kind = OpKind::Ins;
    ins.t = 1;
    ins.path = {"doc"};
    ins.value = start;
    std::vector<std::pair<Op, SiteId>> all = {{ins, 1}};
    base.apply(ins, 1);
    uint64_t ta = 100, tb = 100;
    Replica ra, rb;
    ra.apply(ins, 1);
    rb.apply(ins, 1);
    for (int s = 0; s < 4; ++s) {
      Json ja = Json::makeObject(), jb = Json::makeObject();
      ja.object["x"] = randomJson(r, 0);
      jb.object["x"] = randomJson(r, 0);
      std::vector<Op> oa, ob;
      ra.diff({"doc"}, ja, 1, [&] { return ta += 3; }, oa);
      rb.diff({"doc"}, jb, 2, [&] { return tb += 2; }, ob);
      for (auto& o : oa) {
        ra.apply(o, 1);
        all.push_back({o, 1});
      }
      for (auto& o : ob) {
        rb.apply(o, 2);
        all.push_back({o, 2});
      }
    }
    std::string ref;
    bool ok = true;
    for (int k = 0; k < 3; ++k) {
      auto ops = all;
      std::shuffle(ops.begin(), ops.end(), r);
      const size_t n = ops.size();
      for (size_t d = 0; d < n / 3; ++d) ops.push_back(ops[r() % n]);  // duplicates
      Replica x;
      for (const auto& [op, site] : ops) x.apply(op, site);
      const std::string txt = jsonStringify(x.materialize({"doc"}), 0);
      if (k == 0) ref = txt;
      else ok = ok && txt == ref;
    }
    good += ok;
  }
  expect(good == rounds, "replicas converge under reordering and duplication (200 random concurrent edit sets)");
}

// ---------------------------------------------------------------- sessions ----
struct Trio {
  Clock clock;
  Net net;
  Peer a, b, c;
  explicit Trio(uint32_t seed, const std::string& name, bool three = true) : net(seed) {
    a.label = "Alex";
    b.label = "Sam";
    c.label = "Kim";
    for (Peer* p : {&a, &b, &c}) p->id = crypto::Identity::generate();
    a.doc.reset(makeEmptySong());
    a.doc.addChannel("Kick", "gl.kick");
    a.doc.addChannel("Lead", "gl.synth");
    a.folder = tmpDir(name + "_a");
    b.folder = tmpDir(name + "_b");
    c.folder = tmpDir(name + "_c");
    std::string err;
    a.s = Session::create(a.doc, config(a.folder, a.id, a.label, &clock), &err);
    if (!a.s) std::printf("create failed: %s\n", err.c_str());
    net.add(&a);
    join(b);
    if (three) join(c);
  }
  void join(Peer& p) {
    std::string err;
    p.s = Session::join(p.doc, config(p.folder, p.id, p.label, &clock), ticketOf(*a.s), &err);
    a.s->admitMember(memberOf(p));
    net.add(&p);
    net.connect(&a, &p);
  }
  std::vector<Peer*> all() { return {&a, &b, &c}; }
};

ObjectId pick(std::mt19937& r, const std::vector<ObjectId>& v) { return v.empty() ? kNoId : v[r() % v.size()]; }

void randomEdit(Peer& p, std::mt19937& r) {
  Document& d = p.doc;
  const Song& s = d.song();
  std::vector<ObjectId> chans, pats, clips, mix;
  for (const auto& c : s.channels) chans.push_back(c.id);
  for (const auto& x : s.patterns) pats.push_back(x.id);
  for (const auto& c : s.playlist) clips.push_back(c.id);
  for (size_t i = 1; i < s.mixer.size(); ++i) mix.push_back(s.mixer[i].id);
  const ObjectId ch = pick(r, chans), pat = pick(r, pats), clip = pick(r, clips), mx = pick(r, mix);
  switch (r() % 16) {
    case 0: d.addChannel("Ch" + std::to_string(r() % 100), r() % 2 ? "gl.kick" : "gl.hat"); break;
    case 1: if (ch) d.setParam(channelParam(ch, ChannelParam::Volume), (r() % 100) / 100.0, false); break;
    case 2: if (ch && pat) d.addNote(pat, ch, Note(static_cast<Tick>(r() % 16) * 24, 24, 48 + static_cast<int>(r() % 24), 100)); break;
    case 3:
      if (ch && pat) {
        const Pattern* pp = s.pattern(pat);
        auto it = pp->notes.find(ch);
        if (it != pp->notes.end() && !it->second.empty()) {
          auto notes = it->second;
          auto& n = notes[r() % notes.size()];
          n.start = static_cast<Tick>(r() % 16) * 24;
          n.velocity = static_cast<int>(r() % 128);
          d.setNotes(pat, ch, notes);
        }
      }
      break;
    case 4: if (ch && chans.size() > 1 && r() % 3 == 0) d.removeChannel(ch); break;
    case 5: if (ch) d.renameChannel(ch, "N" + std::to_string(r() % 1000)); break;
    case 6: if (pat) d.addClip(pat, static_cast<int>(r() % 8), static_cast<Tick>(r() % 32) * 96); break;
    case 7: if (clip) d.moveClip(clip, static_cast<int>(r() % 8), static_cast<Tick>(r() % 32) * 96); break;
    case 8: if (clip) d.removeClip(clip); break;
    case 9: if (mx) d.setSlotPlugin(mx, static_cast<int>(r() % 3), r() % 2 ? "gl.eq" : "gl.comp"); break;
    case 10: if (mx) d.setParam(mixerParam(mx, MixerParam::Volume), (r() % 100) / 100.0, false); break;
    case 11: d.undo(); break;
    case 12: d.redo(); break;
    case 13: if (r() % 4 == 0) d.addPattern("P" + std::to_string(r() % 50)); break;
    case 14: if (pat && pats.size() > 1 && r() % 4 == 0) d.removePattern(pat); break;
    case 15: if (pat && ch) d.setStep(pat, ch, static_cast<int>(r() % 16), r() % 2 ? 100 : 0); break;
  }
}

bool converged(Trio& t, bool three = true) {
  const std::string a = songText(t.a), b = songText(t.b);
  bool ok = a == b && t.a.s->versionVector() == t.b.s->versionVector();
  if (three) ok = ok && songText(t.c) == a && t.c.s->versionVector() == t.a.s->versionVector();
  if (!ok) {
    std::ofstream("collab_test_tmp/diverged_a.json") << a;
    std::ofstream("collab_test_tmp/diverged_b.json") << b;
    if (three) std::ofstream("collab_test_tmp/diverged_c.json") << songText(t.c);
  }
  return ok;
}

void testJoinSync() {
  Trio t(1, "join");
  settle(t.net, t.all(), t.clock);
  expect(t.b.s->ready() && t.c.s->ready(), "invitees receive the project");
  expect(converged(t), "a fresh join converges to the creator's project");
  expect(t.b.doc.song().channels.size() == 2 && t.b.doc.song().channels[1].name == "Lead", "invitee sees the channels");
  // live edits both ways
  const ObjectId lead = t.a.doc.song().channels[1].id;
  t.b.doc.setParam(channelParam(lead, ChannelParam::Volume), 0.33, false);
  t.a.doc.renameChannel(lead, "Lead A");
  settle(t.net, t.all(), t.clock);
  expect(std::abs(t.a.doc.song().channel(lead)->gain - 0.33f) < 1e-6f && t.c.doc.song().channel(lead)->name == "Lead A",
         "edits travel both ways");
  expect(t.b.s->incrementalApplies() > 0 && t.b.s->corrections() == 0 && t.a.s->corrections() == 0,
         "element edits are applied element by element, matching the whole-project result");
  // no lost edits: edits made while the other side's ops are in flight all survive
  for (int round = 0; round < 20; ++round) {
    const ObjectId kick = t.a.doc.song().channels[0].id;
    t.a.doc.renameChannel(kick, "A" + std::to_string(round));
    t.b.doc.setParam(channelParam(kick, ChannelParam::Pan), round / 40.0, false);
    t.c.doc.setParam(channelParam(lead, ChannelParam::Pitch), round % 12, false);
    run(t.net, t.all(), t.clock, 0.03);
  }
  settle(t.net, t.all(), t.clock);
  {
    const Channel* k = t.c.doc.song().channel(t.a.doc.song().channels[0].id);
    expect(k && k->name == "A19" && std::abs(k->pan - 19 / 40.0f) < 1e-6f && t.a.doc.song().channel(lead)->pitch == 7 && converged(t),
           "concurrent edits of different properties all survive (no lost edits)");
  }
  // ids made concurrently never collide
  const ObjectId x = t.a.doc.addChannel("A1", "gl.kick");
  const ObjectId y = t.b.doc.addChannel("B1", "gl.kick");
  const ObjectId z = t.c.doc.addChannel("C1", "gl.kick");
  settle(t.net, t.all(), t.clock);
  expect(x != y && y != z && x != z && t.a.doc.song().channels.size() == 5 && converged(t), "concurrent adds keep distinct ids");
}

void testFuzz(uint32_t seed) {
  Trio t(seed, "fuzz" + std::to_string(seed));
  t.net.dupChance = 0.1;
  t.net.maxDelay = 0.3;
  settle(t.net, t.all(), t.clock);
  std::mt19937 r(seed * 7 + 1);
  Peer* ps[3] = {&t.a, &t.b, &t.c};
  for (int step = 0; step < 400; ++step) {
    randomEdit(*ps[r() % 3], r);
    if (r() % 25 == 0) {  // partitions come and go
      Peer* x = ps[r() % 3];
      Peer* y = ps[r() % 3];
      if (x != y) {
        if (t.net.linked(x->s->site(), y->s->site())) t.net.disconnect(x, y);
        else t.net.connect(x, y);
      }
    }
    if (r() % 40 == 0) t.clock.ms += 30000;  // time passes (offline edits -> conflicts)
    run(t.net, t.all(), t.clock, 0.02 + (r() % 10) * 0.01);
  }
  t.net.connect(&t.a, &t.b);
  t.net.connect(&t.b, &t.c);
  t.net.connect(&t.a, &t.c);
  settle(t.net, t.all(), t.clock, 60);
  char name[200];
  std::snprintf(name, sizeof name, "3 peers converge after 400 random edits with delays, reordering, duplicates, partitions (seed %u)", seed);
  expect(converged(t), name);
  const bool sameQueue = t.a.s->conflicts().openCount() == t.b.s->conflicts().openCount() &&
                         t.b.s->conflicts().openCount() == t.c.s->conflicts().openCount();
  std::snprintf(name, sizeof name, "every peer derives the same conflict queue (seed %u: %zu open)", seed, t.a.s->conflicts().openCount());
  expect(sameQueue, name);
}

void testUndoLocality() {
  Trio t(5, "undo", false);
  settle(t.net, {&t.a, &t.b}, t.clock);
  const ObjectId lead = t.a.doc.song().channels[1].id;
  const ObjectId kick = t.a.doc.song().channels[0].id;
  // A changes the volume, then B changes the pan of the same channel; A undoes: only A's change goes
  t.a.doc.setParam(channelParam(lead, ChannelParam::Volume), 0.2, false);
  settle(t.net, {&t.a, &t.b}, t.clock);
  t.b.doc.setParam(channelParam(lead, ChannelParam::Pan), -0.5, false);
  t.b.doc.renameChannel(kick, "Kick B");
  settle(t.net, {&t.a, &t.b}, t.clock);
  t.a.doc.undo();
  settle(t.net, {&t.a, &t.b}, t.clock);
  const Channel* la = t.a.doc.song().channel(lead);
  expect(la && std::abs(la->gain - 0.78f) < 1e-6f && std::abs(la->pan + 0.5f) < 1e-6f && t.a.doc.song().channel(kick)->name == "Kick B",
         "undo reverts only your own change (the collaborator's pan and rename stay)");
  expect(converged(t, false), "undo converges on both peers");
  t.a.doc.redo();
  settle(t.net, {&t.a, &t.b}, t.clock);
  expect(std::abs(t.b.doc.song().channel(lead)->gain - 0.2f) < 1e-6f, "redo brings it back for everyone");
  // B overwrites A's value later: A's undo leaves B's value alone
  t.a.doc.setParam(channelParam(lead, ChannelParam::Volume), 0.4, false);
  settle(t.net, {&t.a, &t.b}, t.clock);
  t.b.doc.setParam(channelParam(lead, ChannelParam::Volume), 0.9, false);
  settle(t.net, {&t.a, &t.b}, t.clock);
  t.a.doc.undo();
  settle(t.net, {&t.a, &t.b}, t.clock);
  expect(std::abs(t.a.doc.song().channel(lead)->gain - 0.9f) < 1e-6f, "undo never clobbers a newer edit by someone else");
  // a drag = many ops, one undo step
  for (int i = 1; i <= 20; ++i) {
    t.a.s->setGesture(true);
    t.a.doc.setParam(channelParam(kick, ChannelParam::Volume), i / 25.0, true);
    run(t.net, {&t.a, &t.b}, t.clock, 0.016);
  }
  t.a.doc.endGesture();
  t.a.s->setGesture(false);
  settle(t.net, {&t.a, &t.b}, t.clock);
  const uint64_t ops = t.a.s->opCount();
  expect(std::abs(t.b.doc.song().channel(kick)->gain - 0.8f) < 1e-6f, "a drag streams to the collaborator and ends on the final value");
  t.a.doc.undo();
  settle(t.net, {&t.a, &t.b}, t.clock);
  expect(std::abs(t.b.doc.song().channel(kick)->gain - 0.78f) < 1e-6f && t.a.s->opCount() > ops, "the whole drag is one undo step");
  // removing and undoing a channel with notes brings it back whole
  const ObjectId pat = t.a.doc.song().patterns[0].id;
  t.a.doc.addNote(pat, lead, Note(0, 96, 60, 100));
  settle(t.net, {&t.a, &t.b}, t.clock);
  t.a.doc.removeChannel(lead);
  settle(t.net, {&t.a, &t.b}, t.clock);
  expect(!t.b.doc.song().channel(lead), "a delete reaches the collaborator");
  t.a.doc.undo();
  settle(t.net, {&t.a, &t.b}, t.clock);
  const Pattern* pb = t.b.doc.song().pattern(pat);
  expect(t.b.doc.song().channel(lead) && pb && pb->notes.count(lead) && pb->notes.at(lead).size() == 1 && converged(t, false),
         "undo of a delete restores the channel and its notes everywhere");
}

void testOpLog() {
  const std::string dir = tmpDir("oplog");
  {
    OpLog log;
    Json snap;
    std::vector<std::string> recs;
    log.open(dir, snap, recs);
    for (int i = 0; i < 100; ++i) log.append("record " + std::to_string(i));
    log.sync();
  }
  // a torn write at the end: half a record and garbage
  {
    std::ofstream f(dir + "/oplog.0.bin", std::ios::binary | std::ios::app);
    f.write("\x40\x00\x00\x00\x12\x34", 6);
  }
  {
    OpLog log;
    Json snap;
    std::vector<std::string> recs;
    log.open(dir, snap, recs);
    expect(recs.size() == 100 && recs.back() == "record 99" && log.truncatedBytes() == 6, "a torn tail is dropped, every whole record survives");
    log.append("after");
  }
  // a flipped bit in the middle: records after it go (CRC)
  {
    std::fstream f(dir + "/oplog.0.bin", std::ios::binary | std::ios::in | std::ios::out);
    f.seekp(8 + 3);
    f.put('X');
  }
  {
    OpLog log;
    Json snap;
    std::vector<std::string> recs;
    log.open(dir, snap, recs);
    expect(recs.empty(), "a corrupt record stops the replay at that record (CRC)");
  }
  // compaction: snapshot + new generation, old log gone, kept records survive
  {
    const std::string d2 = tmpDir("oplog2");
    OpLog log;
    Json snap;
    std::vector<std::string> recs;
    log.open(d2, snap, recs);
    for (int i = 0; i < 50; ++i) log.append("r" + std::to_string(i));
    Json s = Json::makeObject();
    s.object["state"] = Json::makeString("compacted");
    expect(log.compact(s, {"r48", "r49"}) && log.generation() == 1, "compaction writes a snapshot and the next log");
    log.append("r50");
    log.close();
    expect(!std::filesystem::exists(d2 + "/oplog.0.bin") && std::filesystem::exists(d2 + "/oplog.1.bin"), "the old log is removed");
    OpLog again;
    again.open(d2, snap, recs);
    expect(snap.text("state") == "compacted" && recs.size() == 3 && recs[2] == "r50", "reopen: snapshot + kept + new records");
    // a crash between writing the next log and the snapshot: the old state stays valid
    std::ofstream(d2 + "/oplog.2.bin") << "partial";
    OpLog third;
    third.open(d2, snap, recs);
    expect(recs.size() == 3 && !std::filesystem::exists(d2 + "/oplog.2.bin"), "a log of an unfinished compaction is ignored and removed");
  }
}

void testReopen() {
  Trio t(9, "reopen", false);
  settle(t.net, {&t.a, &t.b}, t.clock);
  t.a.doc.renameChannel(t.a.doc.song().channels[0].id, "Persisted");
  t.a.doc.addPattern("Second");
  run(t.net, {&t.a, &t.b}, t.clock, 0.5);
  const std::string before = songText(t.a);
  const std::string folder = t.a.folder;
  t.net.disconnect(&t.a, &t.b);
  t.a.s.reset();  // closes
  Document doc2;
  std::string err;
  auto s2 = Session::open(doc2, config(folder, t.a.id, "Alex", &t.clock), &err);
  Peer tmp;
  expect(s2 && jsonStringify(songToJson([&] { Song x = doc2.song(); x.nextId = 0; return x; }()), 0) == before,
         "a collaborative project reopens from its op log exactly");
  expect(std::filesystem::exists(folder + "/Song.gridline.json"), "the folder also holds a plain project file");
  Song plain;
  expect(loadProject(folder + "/Song.gridline.json", plain) && plain.channels[0].name == "Persisted", "the plain project file is current");
  // compaction keeps the state
  if (s2) {
    expect(s2->compactNow(&err), "compaction succeeds");
    s2.reset();
    Document doc3;
    auto s3 = Session::open(doc3, config(folder, t.a.id, "Alex", &t.clock), &err);
    expect(s3 && jsonStringify(songToJson([&] { Song x = doc3.song(); x.nextId = 0; return x; }()), 0) == before,
           "a compacted project reopens with the same song");
  }
}

std::string writeTestWav(const std::string& path, int seconds, float freq) {
  AudioBuffer buf(2, static_cast<size_t>(44100 * seconds));
  for (size_t i = 0; i < buf.frames; ++i) buf.plane(0)[i] = buf.plane(1)[i] = 0.3f * std::sin(6.2831853f * freq * static_cast<float>(i) / 44100.0f);
  writeWav(path, buf, 44100, 16);
  return path;
}

void testFiles() {
  Trio t(13, "files", false);
  t.a.s->fileBudget = 48 * 1024;
  settle(t.net, {&t.a, &t.b}, t.clock);
  const std::string src = writeTestWav(tmpDir("files_src") + "/loop.wav", 2, 220.0f);
  const ObjectId ch = t.a.doc.addChannel("Loop", "gl.sampler");
  t.a.doc.editChannel(ch, "sample", [&](Channel& c) { c.plugin.data = src; });
  t.a.s->frame(t.net.now);
  const std::string localA = t.a.doc.song().channel(ch)->plugin.data;
  expect(samePath(std::filesystem::path(localA).parent_path().string(), t.a.folder + "/Audio"), "an added file is copied into the project's Audio folder");
  const std::string hashName = std::filesystem::path(localA).filename().string();
  expect(hashName.size() == 64 + 4 && hashName.substr(64) == ".wav", "content addressed by SHA-256");
  // the collaborator sees the reference first: placeholder until the file is verified
  t.net.maxDelay = 0.01;
  bool sawLoading = false;
  float lastProgress = 0;
  for (int i = 0; i < 400; ++i) {
    run(t.net, {&t.a, &t.b}, t.clock, 0.01);
    const Channel* cb = t.b.doc.song().channel(ch);
    float pr = 0;
    if (cb && t.b.s->fileLoading(cb->plugin.data, &pr)) {
      sawLoading = true;
      lastProgress = pr;
    }
    if (cb && std::filesystem::exists(cb->plugin.data)) break;
  }
  const Channel* cb = t.b.doc.song().channel(ch);
  expect(sawLoading, "the collaborator shows a loading placeholder while the file arrives");
  (void)lastProgress;
  expect(cb && std::filesystem::exists(cb->plugin.data) && update::sha256File(cb->plugin.data) == hashName.substr(0, 64),
         "the file arrives and is verified");
  expect(cb && !t.b.s->fileLoading(cb->plugin.data), "the placeholder resolves");
  // dedupe: the same file again adds no second copy
  const ObjectId ch2 = t.a.doc.addChannel("Loop 2", "gl.sampler");
  t.a.doc.editChannel(ch2, "sample", [&](Channel& c) { c.plugin.data = src; });
  run(t.net, {&t.a, &t.b}, t.clock, 0.2);
  size_t files = 0;
  for (const auto& e : std::filesystem::directory_iterator(t.a.folder + "/Audio")) files += e.is_regular_file();
  expect(files == 1 && t.a.doc.song().channel(ch2)->plugin.data == localA, "files are deduplicated by hash");
  // resume: a partial download continues where it stopped
  const std::string src2 = writeTestWav(tmpDir("files_src2") + "/long.wav", 6, 330.0f);
  t.net.disconnect(&t.a, &t.b);
  const ObjectId ch3 = t.a.doc.addChannel("Long", "gl.sampler");
  t.a.doc.editChannel(ch3, "sample", [&](Channel& c) { c.plugin.data = src2; });
  run(t.net, {&t.a}, t.clock, 0.1);
  const std::string name3 = std::filesystem::path(t.a.doc.song().channel(ch3)->plugin.data).filename().string();
  t.net.connect(&t.a, &t.b);
  // let some chunks through, then cut the link
  std::uintmax_t partial = 0;
  for (int i = 0; i < 300 && partial < 100000; ++i) {
    run(t.net, {&t.a, &t.b}, t.clock, 0.01);
    std::error_code ec;
    partial = std::filesystem::file_size(t.b.folder + "/.gridline-collab/partial/" + name3 + ".part", ec);
    if (ec) partial = 0;
  }
  t.net.disconnect(&t.a, &t.b);
  run(t.net, {&t.a, &t.b}, t.clock, 0.5);
  expect(partial > 0 && partial < std::filesystem::file_size(t.a.folder + "/Audio/" + name3), "a transfer can stop halfway (partial file kept)");
  // corrupt the partial file: the hash check must catch it and fetch again
  {
    std::fstream f(t.b.folder + "/.gridline-collab/partial/" + name3 + ".part", std::ios::binary | std::ios::in | std::ios::out);
    f.seekp(100);
    f.put('\x7f');
  }
  t.net.connect(&t.a, &t.b);
  for (int i = 0; i < 3000; ++i) {
    run(t.net, {&t.a, &t.b}, t.clock, 0.02);
    if (std::filesystem::exists(t.b.folder + "/Audio/" + name3)) break;
  }
  expect(std::filesystem::exists(t.b.folder + "/Audio/" + name3) &&
             update::sha256File(t.b.folder + "/Audio/" + name3) == name3.substr(0, 64),
         "resumed transfer completes; a damaged part fails the hash check and is fetched again");
}

void testPolicy() {
  Song s = makeEmptySong();
  Json j = songToJson(s, false);
  expect(foreignPlugins(j).empty(), "a native project may become collaborative");
  Channel c;
  c.id = s.allocateId();
  c.name = "Lead";
  c.plugin.type = "vst3:ABCDEF";
  s.channels.push_back(c);
  s.mixer[1].slots[2].plugin.type = "clap:com.example.verb";
  const auto bad = foreignPlugins(songToJson(s, false));
  expect(bad.size() == 2 && bad[0].second.find("Lead") != std::string::npos && bad[1].second.find("slot 3") != std::string::npos,
         "every external plugin is listed with where it is");
  Trio t(17, "policy", false);
  settle(t.net, {&t.a, &t.b}, t.clock);
  std::string notice;
  t.a.s->onNotice = [&](const std::string& n, bool) { notice = n; };
  const auto mixId = t.a.doc.song().mixer[1].id;
  t.a.doc.editMixerTrack(mixId, "load plugin", [](MixerTrack& m) { m.slots[0].plugin.type = "vst3:0123456789ABCDEF"; });
  run(t.net, {&t.a, &t.b}, t.clock, 0.2);
  expect(t.a.doc.song().mixer[1].slots[0].plugin.empty() && t.b.doc.song().mixer[1].slots[0].plugin.empty() &&
             notice.find("External plugins") != std::string::npos,
         "an external plugin added to a collaborative project is refused and never synced");
  t.a.doc.editSong("script", [](Song& x) { x.channels[0].plugin.type = "au:aufx:dely:appl"; });
  run(t.net, {&t.a, &t.b}, t.clock, 0.2);
  expect(t.a.doc.song().channels[0].plugin.type == "gl.kick", "scripts and other edit paths are refused too");
}

void testRelay() {
  // A and B are never online together; C carries the changes.
  Trio t(19, "relay");
  settle(t.net, t.all(), t.clock);
  t.net.disconnect(&t.a, &t.b);
  t.net.disconnect(&t.b, &t.c);
  t.a.doc.renameChannel(t.a.doc.song().channels[0].id, "From A");
  settle(t.net, t.all(), t.clock);  // A -> C
  t.net.disconnect(&t.a, &t.c);
  t.a.s.reset();  // A's computer is off
  t.net.connect(&t.b, &t.c);
  settle(t.net, {&t.b, &t.c}, t.clock);
  expect(t.b.doc.song().channels[0].name == "From A", "a change travels A -> C -> B while A is offline (store and forward)");
  // a tampered relayed batch is rejected
  Batch forged;
  const auto& held = t.c.s->versionVector();
  (void)held;
  Trio u(21, "tamper", false);
  settle(u.net, {&u.a, &u.b}, u.clock);
  u.net.disconnect(&u.a, &u.b);
  u.a.doc.renameChannel(u.a.doc.song().channels[0].id, "Genuine");
  run(u.net, {&u.a}, u.clock, 0.1);
  // take A's newest batch and change it
  const SiteId as = u.a.s->site();
  const uint64_t seq = u.a.s->versionVector().at(as);
  Batch b;
  bool got = false;
  // the batch text as A sent it (rebuild through the log on disk)
  {
    Json snap;
    std::vector<std::string> recs;
    OpLog log;
    log.open(u.a.folder + "/.gridline-collab", snap, recs);
    for (const auto& r : recs)
      if (Batch::parse(r, b) && b.site == as && b.seq == seq) got = true;
  }
  bool rejected = false;
  if (got) {
    for (auto& op : b.ops)
      if (op.kind == OpKind::Set) op.value = Json::makeString("Forged");
    Message m;
    m.head.object["m"] = Json::makeString("batches");
    Json list = Json::makeArray();
    list.array.push_back(Json::makeString(b.text()));
    m.head.object["b"] = std::move(list);
    u.b.s->receive(u.c.id.site(), kStreamControl, m.encode());  // as if relayed by someone
    run(u.net, {&u.b}, u.clock, 0.1);
    rejected = u.b.doc.song().channels[0].name != "Forged" && u.b.s->versionVector().count(as) && u.b.s->versionVector().at(as) < seq;
  }
  expect(got && rejected, "a relayed batch whose content was changed fails its signature and is dropped");
  u.net.connect(&u.a, &u.b);
  settle(u.net, {&u.a, &u.b}, u.clock);
  expect(u.b.doc.song().channels[0].name == "Genuine", "the genuine batch still arrives afterwards");
}

void testConflicts() {
  // live co-editing: concurrent edits of the same knob within latency -> no prompt
  {
    Trio t(31, "live", false);
    settle(t.net, {&t.a, &t.b}, t.clock);
    const ObjectId lead = t.a.doc.song().channels[1].id;
    for (int i = 0; i < 30; ++i) {
      t.a.doc.setParam(channelParam(lead, ChannelParam::Volume), 0.01 * i, false);
      t.b.doc.setParam(channelParam(lead, ChannelParam::Volume), 0.5 + 0.01 * i, false);
      run(t.net, {&t.a, &t.b}, t.clock, 0.05);
    }
    settle(t.net, {&t.a, &t.b}, t.clock);
    expect(t.a.s->conflicts().openCount() == 0 && t.b.s->conflicts().openCount() == 0 && converged(t, false),
           "live co-editing of the same knob never prompts and converges");
  }
  // offline: both change the same knob, rename the same channel, one deletes a clip the other moved
  Trio t(37, "offline");
  settle(t.net, t.all(), t.clock);
  const ObjectId lead = t.a.doc.song().channels[1].id;
  const ObjectId pat = t.a.doc.song().patterns[0].id;
  const ObjectId clip = t.a.doc.addClip(pat, 0, 0);
  settle(t.net, t.all(), t.clock);
  t.net.disconnect(&t.a, &t.b);
  t.net.disconnect(&t.a, &t.c);
  t.net.disconnect(&t.b, &t.c);
  t.a.doc.setParam(channelParam(lead, ChannelParam::Volume), 0.3, false);
  t.a.doc.renameChannel(lead, "Lead by Alex");
  t.a.doc.removeClip(clip);
  run(t.net, t.all(), t.clock, 0.05);
  t.clock.ms += 60000;
  t.b.doc.setParam(channelParam(lead, ChannelParam::Volume), 0.55, false);
  t.b.doc.renameChannel(lead, "Lead by Sam");
  t.b.doc.moveClip(clip, 2, 384);
  // and an unrelated change: merges without a prompt
  t.b.doc.renameChannel(t.b.doc.song().channels[0].id, "Kick by Sam");
  run(t.net, t.all(), t.clock, 0.2);
  t.net.connect(&t.a, &t.b);
  settle(t.net, {&t.a, &t.b, &t.c}, t.clock);
  const size_t open = t.a.s->conflicts().openCount();
  for (const ConflictObject* c : t.a.s->conflicts().open()) {
    std::printf("# conflict %s:", c->object.c_str());
    for (const auto& o : c->ops) std::printf(" [%s %s %s]", o.name.c_str(), opKindName(o.kind).c_str(), joinPath(o.path).c_str());
    std::printf("\n");
  }
  expect(open == 2 && t.b.s->conflicts().openCount() == 2, "offline edits of the same channel and clip become 2 conflicts on both peers");
  expect(converged(t, false) && t.a.doc.song().channel(lead)->name == "Lead by Sam" &&
             std::abs(t.a.doc.song().channel(lead)->gain - 0.55f) < 1e-6f && !t.a.doc.song().clip(clip),
         "the provisional result is deterministic (later wins; delete wins over edit)");
  expect(t.a.doc.song().channels[0].name == "Kick by Sam", "non-conflicting offline changes merge without a prompt");
  expect(t.a.s->takeReviewRequest() && !t.b.s->takeReviewRequest(), "the review opens for the admin only");
  // sides: who and when, ordered by time
  const ConflictObject* chan = nullptr;
  for (const ConflictObject* c : t.a.s->conflicts().open())
    if (c->object.find("channels") != std::string::npos) chan = c;
  auto sides = chan ? ConflictTracker::sides(*chan) : std::vector<ConflictSide>{};
  expect(sides.size() == 2 && sides[0].name == "Alex" && sides[1].name == "Sam" && sides[0].firstT < sides[1].firstT,
         "each conflict shows every side with author and time, oldest first");
  // a non-admin cannot resolve
  std::string err;
  expect(chan && !t.b.s->resolve(chan->object, t.b.s->site(), &err), "only the admin resolves");
  // C is offline while the admin resolves: keep Alex's channel edits, keep Sam's clip move
  if (chan) {
    const std::string chanObj = chan->object;
    expect(t.a.s->preview(chanObj, t.a.s->site()) && t.a.doc.song().channel(lead)->name == "Lead by Alex", "preview applies a side temporarily");
    t.a.s->endPreview();
    expect(t.a.doc.song().channel(lead)->name == "Lead by Sam", "ending the preview restores the provisional result");
    expect(t.a.s->resolve(chanObj, t.a.s->site(), &err), "the admin keeps one side");
    std::string clipObj;
    for (const ConflictObject* c : t.a.s->conflicts().open()) clipObj = c->object;
    expect(!clipObj.empty() && t.a.s->resolve(clipObj, t.b.s->site(), &err), "the admin keeps the other person's side of another conflict");
    settle(t.net, {&t.a, &t.b}, t.clock);
    expect(t.a.s->conflicts().openCount() == 0 && t.b.s->conflicts().openCount() == 0, "resolved conflicts leave the queue everywhere");
    expect(converged(t, false) && t.b.doc.song().channel(lead)->name == "Lead by Alex" &&
               std::abs(t.b.doc.song().channel(lead)->gain - 0.3f) < 1e-6f && t.b.doc.song().clip(clip) &&
               t.b.doc.song().clip(clip)->track == 2,
           "every peer converges on the admin's choices (kept edit, undeleted moved clip)");
    // C comes back later and catches up with the resolution
    t.net.connect(&t.c, &t.b);
    settle(t.net, t.all(), t.clock);
    expect(converged(t) && t.c.s->conflicts().openCount() == 0, "a peer offline during the resolution converges on it later");
    expect(!t.a.s->conflicts().settled().empty(), "resolved conflicts stay in history (Resolved tab)");
    // restore the losing side later
    expect(t.a.s->resolve(chanObj, t.b.s->site(), &err), "a losing side can be restored later");
    settle(t.net, t.all(), t.clock);
    expect(t.c.doc.song().channel(lead)->name == "Lead by Sam" && converged(t), "the restored side reaches everyone");
  }
}

void testProfile() {
  expect(cleanName("  Sam \t Lee\n ") == "Sam Lee" && cleanName("\x01\x02") == "" &&
             cleanName(std::string(100, 'x')).size() == kMaxNameChars,
         "display names are trimmed, cleaned and limited");
  expect(validEmail("sam@example.org") && validEmail("") && !validEmail("sam@") && !validEmail("a b@c.de") && !validEmail("x@y"),
         "emails are validated");
  expect(initials("Sam Lee") == "SL" && initials("alex") == "A", "initials for the fallback avatar");
  expect(initials("Max Wolf (studio B)") == "MB" && initials("(dj) Kai") == "DK", "initials skip brackets and punctuation");
  // avatar: a real PNG -> square JPEG <= 64 KB
  png::Image img;
  img.width = 900;
  img.height = 500;
  img.channels = 3;
  img.pixels.resize(900 * 500 * 3);
  std::mt19937 r(3);
  for (auto& p : img.pixels) p = static_cast<uint8_t>(r());  // noise: the hardest case to compress
  const auto pngBytes = png::encode(img);
  std::string jpg, err;
  const bool ok = sanitizeAvatar(std::string(pngBytes.begin(), pngBytes.end()), jpg, &err);
  int w = 0, h = 0;
  std::vector<uint8_t> rgba;
  expect(ok && jpg.size() <= kMaxAvatarBytes && decodeAvatar(jpg, w, h, rgba) && w == h && w <= kAvatarSize,
         "a picture becomes a square JPEG of at most 256 px and 64 KB");
  expect(jpg.size() > 3 && static_cast<unsigned char>(jpg[0]) == 0xFF && static_cast<unsigned char>(jpg[1]) == 0xD8 &&
             jpg.find("Exif") == std::string::npos,
         "re-encoded without metadata");
  expect(!sanitizeAvatar("not an image at all", jpg, &err), "malformed input is refused");
  std::string truncated(pngBytes.begin(), pngBytes.begin() + 200);
  expect(!sanitizeAvatar(truncated, jpg, &err), "a truncated picture is refused");
  // a header claiming a huge picture
  png::Image tiny;
  tiny.width = 1;
  tiny.height = 1;
  tiny.channels = 3;
  tiny.pixels = {1, 2, 3};
  auto big = png::encode(tiny);
  // patch IHDR width/height to 60000 x 60000
  for (int k = 0; k < 2; ++k) {
    big[16 + 4 * k] = 0;
    big[17 + 4 * k] = 0;
    big[18 + 4 * k] = 0xEA;
    big[19 + 4 * k] = 0x60;
  }
  expect(!sanitizeAvatar(std::string(big.begin(), big.end()), jpg, &err), "a picture claiming 60000 x 60000 pixels is refused");
  expect(!sanitizeAvatar(std::string(kMaxAvatarInputBytes + 1, 'x'), jpg, &err), "an oversize file is refused");
  // wire: email only when shared
  Profile p;
  p.name = "Sam";
  p.email = "sam@example.org";
  p.shareEmail = false;
  expect(jsonStringify(profileWire(p), 0).find("example.org") == std::string::npos, "the email is not sent unless sharing is on");
  p.shareEmail = true;
  Profile back;
  expect(profileFromWire(profileWire(p), back) && back.email == "sam@example.org", "a shared email reaches collaborators");
  // store round trip
  const std::string dir = tmpDir("profile");
  {
    ProfileStore st(dir);
    st.load();
    st.profile().name = "Sam";
    st.profile().email = "sam@example.org";
    st.save();
    ProfileStore again(dir);
    again.load();
    expect(again.profile().name == "Sam" && again.identity().fingerprint() == st.identity().fingerprint(),
           "profile and identity persist in the user's config");
    const std::string fp = again.identity().fingerprint();
    expect(fp.size() == 24 && fp[4] == '-', "the fingerprint reads in groups");
  }
  // email never lands in a project folder (handshake + edits + saves)
  Trio t(41, "email", false);
  t.a.s->setProfile([&] {
    Profile x;
    x.name = "Alex";
    x.email = "alex.private@example.org";
    x.shareEmail = true;
    return x;
  }());
  settle(t.net, {&t.a, &t.b}, t.clock);
  t.a.doc.renameChannel(t.a.doc.song().channels[0].id, "Edited");
  settle(t.net, {&t.a, &t.b}, t.clock);
  t.a.s->saveNow();
  t.b.s->saveNow();
  t.a.s->compactNow();
  bool leaked = false;
  for (const auto& folder : {t.a.folder, t.b.folder})
    for (const auto& e : std::filesystem::recursive_directory_iterator(folder)) {
      if (!e.is_regular_file()) continue;
      std::ifstream f(e.path(), std::ios::binary);
      std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      if (text.find("alex.private@example.org") != std::string::npos) leaked = true;
    }
  expect(!leaked, "a shared email never appears in either project folder");
  const PeerView* pv = t.b.s->peer(t.a.s->site());
  expect(pv && pv->profile.email == "alex.private@example.org" && pv->profile.name == "Alex", "the profile travels in the session");
  // J-111g: the picture travels too: the profile names it by hash, the other side asks
  // for the bytes (bulk stream: more than a datagram) and keeps them; a new picture follows
  auto picture = [](uint8_t r, uint8_t g, uint8_t b) {
    png::Image img;
    img.width = img.height = 300;
    img.channels = 3;
    for (int i = 0; i < 300 * 300; ++i) {  // noisy, so the JPEG is several kilobytes
      const uint8_t n = static_cast<uint8_t>((static_cast<uint32_t>(i) * 2654435761u) >> 27);
      img.pixels.push_back(static_cast<uint8_t>(r ^ n));
      img.pixels.push_back(g);
      img.pixels.push_back(static_cast<uint8_t>(b ^ n));
    }
    const std::vector<uint8_t> enc = png::encode(img);
    std::string jpg;
    sanitizeAvatar(std::string(enc.begin(), enc.end()), jpg);
    return jpg;
  };
  Profile withPic = t.a.s->config().profile;
  withPic.avatar = picture(200, 60, 160);
  expect(withPic.avatar.size() > 1200, "the test picture needs more than one datagram");
  t.a.s->setProfile(withPic);
  settle(t.net, {&t.a, &t.b}, t.clock);
  pv = t.b.s->peer(t.a.s->site());
  expect(pv && pv->profile.avatarHash() == withPic.avatarHash(), "the picture's hash reaches the collaborator");
  expect(pv && t.b.s->avatarFor(pv->profile.avatarHash()) == withPic.avatar && pv->profile.avatar == withPic.avatar,
         "the picture's bytes reach the collaborator, the same bytes");
  withPic.avatar = picture(60, 160, 200);
  t.a.s->setProfile(withPic);
  settle(t.net, {&t.a, &t.b}, t.clock);
  pv = t.b.s->peer(t.a.s->site());
  expect(pv && pv->profile.avatar == withPic.avatar && t.b.s->avatarFor(withPic.avatarHash()) == withPic.avatar, "a changed picture replaces it");
  withPic.avatar.clear();
  t.a.s->setProfile(withPic);
  settle(t.net, {&t.a, &t.b}, t.clock);
  pv = t.b.s->peer(t.a.s->site());
  expect(pv && pv->profile.avatarHash().empty() && pv->profile.avatar.empty(), "a removed picture is removed");
  // the wire: the hash without the bytes stays with the profile (it used to be dropped)
  Profile named;
  named.name = "Sam";
  named.avatar = picture(10, 20, 30);
  Profile got;
  expect(profileFromWire(profileWire(named), got) && got.avatar.empty() && got.avatarRef == named.avatarHash() && got.avatarHash() == named.avatarHash(),
         "a profile names its picture by hash until the bytes are here");
  expect(profileFromWire(profileWire(named, true), got) && got.avatar == named.avatar, "with the bytes (kept profiles) the picture comes back whole");
}

void testVersionRecord() {
  Trio t(43, "version", false);
  settle(t.net, {&t.a, &t.b}, t.clock);
  expect(t.a.s->minAppVersion() == "0.7.0" && Session::minAppVersionOf(t.a.folder) == "0.7.0", "the project records the app version that wrote it");
}


// ------------------------------------------------------- membership (J-111e) ----
// A batch of `p`'s site with the given ops, signed with `signer` (forgeries).
std::string forgedBatch(const Session& author, SiteId site, const crypto::Identity& signer, std::vector<Op> ops) {
  Batch b;
  b.site = site;
  const VersionVector& vv = author.versionVector();
  b.seq = (vv.count(site) ? vv.at(site) : 0) + 1;
  b.deps = vv;
  b.name = "Forger";
  b.ops = std::move(ops);
  b.signWith(signer);
  Message m;
  m.head.object["m"] = Json::makeString("batches");
  Json list = Json::makeArray();
  list.array.push_back(Json::makeString(b.text()));
  m.head.object["b"] = std::move(list);
  return m.encode();
}

Op removalOp(SiteId target, SiteId by, uint64_t t) {
  Op op;
  op.kind = OpKind::Ins;
  op.t = t;
  op.path = {"meta", "removal:" + siteHex(target) + ":forged"};
  Json v = Json::makeObject();
  v.object["by"] = Json::makeString(siteHex(by));
  v.object["byName"] = Json::makeString("Forger");
  v.object["upTo"] = Json::makeNumber(0);
  op.value = std::move(v);
  return op;
}

void testRemoval() {
  Trio t(41, "remove");
  settle(t.net, t.all(), t.clock);
  const SiteId sam = t.b.s->site(), kim = t.c.s->site();
  // an edit the admin has received before removing them stays
  t.b.doc.renameChannel(t.b.doc.song().channels[0].id, "Before removal");
  settle(t.net, t.all(), t.clock);
  std::string err;
  expect(!t.b.s->removeMember(kim, "", &err) && !err.empty(), "only the admin can remove people");
  expect(!t.a.s->removeMember(t.a.s->site(), "", &err), "the admin can't remove themselves (Leave instead)");
  expect(t.a.s->removeMember(sam, "Finished his part", &err), "the admin removes a member");
  settle(t.net, t.all(), t.clock);
  expect(t.a.s->memberState(sam) == MemberState::Removed && t.c.s->memberState(sam) == MemberState::Removed, "every peer applies the removal");
  expect(t.b.s->exitKind() == ExitKind::Removed && t.b.s->stopped() && t.b.s->myRemoval().byName == "Alex" &&
             t.b.s->myRemoval().reason == "Finished his part",
         "the removed person's open session learns it at once (by whom, the reason) and stops syncing");
  expect(t.b.doc.song().channels[0].name == "Before removal", "their local copy keeps the project as it was");
  expect(t.a.doc.song().channels[0].name == "Before removal" && t.c.doc.song().channels[0].name == "Before removal",
         "edits the admin had received before the removal are kept");
  expect(t.a.s->activeMembers().size() == 2 && t.a.s->members().size() == 3 && t.a.s->removals().size() == 1 &&
             t.a.s->removals()[0].name == "Sam" && t.a.s->removals()[0].byName == "Alex",
         "the member list keeps them as removed (who, by whom, when) for the history");
  // after the removal nothing travels to or from them
  t.a.doc.renameChannel(t.a.doc.song().channels[1].id, "After removal");
  t.b.doc.addChannel("Sam after removal", "gl.hat");
  settle(t.net, t.all(), t.clock);
  expect(t.b.doc.song().channels[1].name != "After removal", "the removed person receives nothing new");
  bool samEdit = false;
  for (const auto& c : t.c.doc.song().channels) samEdit = samEdit || c.name == "Sam after removal";
  expect(!samEdit && songText(t.a) == songText(t.c), "the others continue and converge without them");
  expect(!t.a.s->transferAdmin(sam), "admin can't go to someone removed");
  expect(t.a.s->admitMember(memberOf(t.b)) == -1, "an ordinary invite can't bring a removed member back");
  // a notice for them proves itself with the admin's signed record
  const Json notice = t.a.s->noticeFor(sam);
  expect(notice.text("reason") == "removed" && !notice.text("batch").empty() && t.a.s->noticeFor(kim).is(Json::Type::Null),
         "peers refuse a removed identity with the reason and the signed record, nobody else");
  Json bad = notice;
  std::string bt = bad.text("batch");
  const size_t at = bt.find("Finished");
  if (at != std::string::npos) bt[at] = 'f';
  bad.object["batch"] = Json::makeString(bt);
  Document d2;
  auto again = Session::open(d2, config(t.b.folder, t.b.id, "Sam", &t.clock), &err);
  expect(again && !again->learnNotice(bad), "a notice whose signed record was changed is not believed");
  // a forged removal: Kim (not admin) signs one; the admin's site signed by someone else
  const std::string forged1 = forgedBatch(*t.c.s, kim, t.c.id, {removalOp(t.a.s->site(), kim, (1ull << 62))});
  t.a.s->receive(kim, kStreamControl, forged1);
  run(t.net, {&t.a}, t.clock, 0.1);
  expect(t.a.s->memberState(t.a.s->site()) == MemberState::Active && !t.a.s->stopped(), "a removal signed by a non-admin is ignored");
  Op o = removalOp(kim, t.a.s->site(), (1ull << 62));
  const std::string forged2 = forgedBatch(*t.a.s, t.a.s->site(), t.c.id, {o});
  t.c.s->receive(t.a.s->site(), kStreamControl, forged2);
  run(t.net, {&t.c}, t.clock, 0.1);
  expect(t.c.s->memberState(kim) == MemberState::Active && t.c.s->exitKind() == ExitKind::None, "a removal claiming to be the admin's but signed by someone else fails its signature");
}

void testRemovalLateOps() {
  // Sam edits while the admin can't see him; Kim gets the edit; the admin removes Sam
  // (cut: what the admin had): Kim drops the late edit again and converges with the admin.
  for (int variant = 0; variant < 2; ++variant) {
    Trio t(43 + variant, variant ? "late_taint" : "late");
    settle(t.net, t.all(), t.clock);
    const SiteId sam = t.b.s->site();
    t.net.disconnect(&t.a, &t.b);
    t.net.disconnect(&t.a, &t.c);
    t.net.connect(&t.b, &t.c);
    const ObjectId kick = t.a.doc.song().channels[0].id;
    t.b.doc.renameChannel(kick, "Late by Sam");
    settle(t.net, t.all(), t.clock);
    expect(t.c.doc.song().channel(kick)->name == "Late by Sam", "the late edit reached Kim before the removal");
    if (variant) t.c.s->compactNow();  // the snapshot on disk now holds the late edit
    std::string err;
    t.a.s->removeMember(sam, "", &err);
    t.a.doc.addChannel("Alex meanwhile", "gl.kick");
    t.net.disconnect(&t.b, &t.c);
    t.net.connect(&t.a, &t.c);
    settle(t.net, {&t.a, &t.c}, t.clock);
    settle(t.net, {&t.a, &t.c}, t.clock);
    const char* name = variant ? "a removal after compaction: a clean snapshot from the admin replaces the tainted replica"
                               : "a removed member's ops after the cut are dropped by everyone, also where they already arrived (replayed)";
    expect(t.c.doc.song().channel(kick)->name != "Late by Sam" && songText(t.a) == songText(t.c) && t.c.s->rebuilds() > 0 && !t.c.s->tainted(), name);
    // Sam's queued batches arriving later change nothing
    t.net.connect(&t.b, &t.c);
    t.b.doc.addChannel("Queued by Sam", "gl.hat");
    settle(t.net, t.all(), t.clock);
    bool queued = false;
    for (const auto& c : t.c.doc.song().channels) queued = queued || c.name == "Queued by Sam";
    expect(!queued && songText(t.a) == songText(t.c), "queued store-and-forward ops of a removed member arriving after the removal are dropped");
  }
}

void testReadmitAndLeave() {
  Trio t(47, "readmit");
  settle(t.net, t.all(), t.clock);
  const SiteId sam = t.b.s->site();
  std::string err;
  t.net.disconnect(&t.a, &t.b);
  t.net.disconnect(&t.b, &t.c);
  t.a.s->removeMember(sam, "", &err);
  settle(t.net, t.all(), t.clock);
  // Sam, offline, keeps editing (dropped everywhere), then learns of the removal on reconnecting
  t.b.doc.addChannel("While removed", "gl.hat");
  run(t.net, {&t.b}, t.clock, 0.2);
  expect(t.b.s->exitKind() == ExitKind::None, "offline, the removed person does not know yet");
  expect(t.b.s->learnNotice(t.a.s->noticeFor(sam)), "a handshake refusal tells them (offline catch-up)");
  run(t.net, {&t.b}, t.clock, 0.1);
  expect(t.b.s->exitKind() == ExitKind::Removed && t.b.s->myRemoval().byName == "Alex", "and their session stops with the same details");
  // the admin re-invites Sam: an explicit re-add clears the removed state
  expect(t.a.s->readmit(sam) && t.a.s->memberState(sam) == MemberState::Active, "an admin's re-invite clears the removed state");
  t.net.disconnect(&t.a, &t.b);
  t.b.s.reset();
  Session::Config cfg = config(t.b.folder, t.b.id, "Sam", &t.clock);
  cfg.expectReadmit = true;
  t.b.s = Session::open(t.b.doc, cfg, &err);
  expect(t.b.s != nullptr, "the removed copy opens again for the re-invite");
  t.net.wire(&t.b);
  t.net.add(&t.b);
  t.net.connect(&t.a, &t.b);
  settle(t.net, t.all(), t.clock);
  settle(t.net, t.all(), t.clock);
  t.b.doc.addChannel("Back again", "gl.kick");
  settle(t.net, t.all(), t.clock);
  bool back = false, during = false;
  for (const auto& c : t.a.doc.song().channels) {
    back = back || c.name == "Back again";
    during = during || c.name == "While removed";
  }
  expect(t.b.s->exitKind() == ExitKind::None && back && !during && converged(t, false),
         "back after a re-add: new edits sync, what they made while removed stays dropped (their copy follows)");
  // leaving is a signed record too (consistent with removal)
  expect(t.c.s->announceLeave(), "a member announces leaving");
  settle(t.net, t.all(), t.clock);
  expect(t.a.s->memberState(t.c.s->site()) == MemberState::Left && t.a.s->activeMembers().size() == 2, "everyone sees they left");
  expect(t.a.s->admitMember(memberOf(t.c)) >= 0 && t.a.s->memberState(t.c.s->site()) == MemberState::Active, "an ordinary invite brings someone who left back");
}

void testRemovalPurge() {
  // the log keeps batches a member has not seen; once removed, nobody waits for them
  Trio t(53, "purge");
  settle(t.net, t.all(), t.clock);
  t.net.disconnect(&t.a, &t.b);
  t.net.disconnect(&t.b, &t.c);
  for (int i = 0; i < 20; ++i) {
    t.a.doc.addChannel("C" + std::to_string(i), "gl.kick");
    run(t.net, {&t.a, &t.c}, t.clock, 0.05);
  }
  run(t.net, {&t.a, &t.c}, t.clock, 6);
  t.a.s->compactNow();
  const size_t kept = t.a.s->log().records();
  std::string err;
  t.a.s->removeMember(t.b.s->site(), "", &err);
  run(t.net, {&t.a, &t.c}, t.clock, 12);
  t.a.s->compactNow();
  expect(kept >= 20 && t.a.s->log().records() < kept, "removing someone purges the batches kept only for them");
  // the only other member removed: nothing waits for anyone
  Trio u(55, "purge_two", false);
  settle(u.net, {&u.a, &u.b}, u.clock);
  u.a.s->removeMember(u.b.s->site(), "", &err);
  settle(u.net, {&u.a, &u.b}, u.clock);
  expect(u.a.s->activeMembers().size() == 1 && u.a.s->unackedBatches() == 0 && u.b.s->exitKind() == ExitKind::Removed,
         "removing the only other member leaves the admin alone with nothing to sync");
}

void testStopSharing() {
  Trio t(59, "ended");
  settle(t.net, t.all(), t.clock);
  t.net.disconnect(&t.a, &t.c);
  t.net.disconnect(&t.b, &t.c);
  expect(!t.b.s->stopSharing(), "only the admin stops sharing");
  expect(t.a.s->stopSharing(), "the admin stops sharing");
  settle(t.net, t.all(), t.clock);
  expect(t.b.s->exitKind() == ExitKind::Ended && t.b.s->stopped() && t.b.s->endInfo().byName == "Alex", "an open session learns it at once");
  const Json n = t.a.s->noticeFor(t.c.s->site());
  expect(n.text("reason") == "ended", "a peer that was offline is told when it connects");
  expect(t.c.s->learnNotice(n), "and believes the signed record");
  run(t.net, {&t.c}, t.clock, 0.1);
  expect(t.c.s->exitKind() == ExitKind::Ended, "its session stops too");
  // never resurrected: reopening a copy that holds the record stops at once
  t.b.s.reset();
  std::string err;
  Document d;
  auto re = Session::open(d, config(t.b.folder, t.b.id, "Sam", &t.clock), &err);
  if (re) re->frame(1000);
  expect(re && re->exitKind() == ExitKind::Ended && re->stopped(), "a copy holding the end record never syncs again");
}

void testUnusedAudio() {
  Trio t(61, "cleanup", false);
  settle(t.net, {&t.a, &t.b}, t.clock);
  const std::string keep = writeTestWav(tmpDir("cleanup_src") + "/keep.wav", 1, 220.0f);
  const std::string gone = writeTestWav(tmpDir("cleanup_src2") + "/gone.wav", 1, 330.0f);
  const ObjectId ch = t.a.doc.addChannel("Keep", "gl.sampler");
  t.a.doc.editChannel(ch, "sample", [&](Channel& c) { c.plugin.data = keep; });
  const ObjectId ch2 = t.a.doc.addChannel("Deleted", "gl.sampler");
  t.a.doc.editChannel(ch2, "sample", [&](Channel& c) { c.plugin.data = gone; });
  run(t.net, {&t.a, &t.b}, t.clock, 0.2);
  t.a.doc.removeChannel(ch2);  // still in the undo history and the replica's tombstones
  run(t.net, {&t.a, &t.b}, t.clock, 0.2);
  // a stray file nothing ever referenced
  const std::string stray = t.a.folder + "/Audio/" + std::string(64, 'a') + ".wav";
  std::filesystem::copy_file(keep, stray);
  const auto unused = t.a.s->unusedAudio();
  expect(unused.size() == 1 && unused[0].first == "Audio/" + std::string(64, 'a') + ".wav", "only a file nothing refers to (project, history, undo, review) counts as unused");
  const uint64_t freed = t.a.s->cleanUnusedAudio();
  size_t files = 0;
  for (const auto& e : std::filesystem::directory_iterator(t.a.folder + "/Audio")) files += e.is_regular_file();
  expect(freed > 0 && !std::filesystem::exists(stray) && files == 2, "clean up removes it and keeps the deleted channel's file (undo can bring it back)");
  t.a.doc.undo();
  run(t.net, {&t.a, &t.b}, t.clock, 0.2);
  const Channel* back = t.a.doc.song().channel(ch2);
  expect(back && std::filesystem::exists(back->plugin.data), "undo after the clean-up finds its audio");
}

void testExport() {
  // the original project before Make collaborative is never touched
  const std::string orig = tmpDir("export_orig") + "/Original.gridline.json";
  Document doc;
  doc.reset(makeEmptySong());
  doc.addChannel("Kick", "gl.kick");
  const std::string wav = writeTestWav(tmpDir("export_src") + "/loop.wav", 1, 220.0f);
  const ObjectId ch = doc.addChannel("Loop", "gl.sampler");
  doc.editChannel(ch, "sample", [&](Channel& c) { c.plugin.data = wav; });
  saveProject(orig, doc.song());
  std::string before, err;
  readFileText(orig, before);
  Clock clock;
  const auto id = crypto::Identity::generate();
  const std::string folder = tmpDir("export_collab");
  auto s = Session::create(doc, config(folder, id, "Alex", &clock), &err);
  s->frame(1);
  doc.renameChannel(doc.song().channels[0].id, "Edited in the shared copy");
  s->frame(2);
  s->saveNow();
  std::string after;
  readFileText(orig, after);
  expect(s && before == after && std::filesystem::exists(wav), "the original project (and its audio) is never modified by Make collaborative");
  // a private copy: a normal project with the same music, its own audio
  const std::string dest = tmpDir("export_copy") + "/Private";
  std::string file;
  expect(exportPlainProject(doc.song(), folder, dest, "Song", false, &file, &err), "export to a private copy");
  Song loaded;
  expect(loadProject(file, loaded) && !std::filesystem::exists(dest + "/.gridline-collab") && !Session::isCollabFolder(dest),
         "the copy opens as a normal project (no collaboration)");
  Song a = doc.song(), b = loaded;
  a.nextId = b.nextId = 0;
  a.idSlot = b.idSlot = 0;
  a.idStride = b.idStride = 1;
  // the shared copy's audio folder becomes the private copy's (by path elements: native separators on Windows)
  const std::string from = folder + "/Audio", to = dest + "/Audio";
  forEachFileRef(a, [&](std::string& p) {
    const std::filesystem::path f(p);
    if (samePath(f.parent_path().string(), from)) p = (std::filesystem::path(to) / f.filename()).lexically_normal().string();
  });
  const std::string ta = jsonStringify(songToJson(a), 0), tb = jsonStringify(songToJson(b), 0);
  expect(ta == tb, "its musical content is identical (only the audio folder differs)");
  const Channel* lc = loaded.channel(ch);
  expect(lc && samePath(std::filesystem::path(lc->plugin.data).parent_path().string(), to) && std::filesystem::exists(lc->plugin.data),
         "its audio files were copied with it");
  expect(tb.find(id.fingerprint()) == std::string::npos && tb.find(siteHex(id.site())) == std::string::npos, "no member identity in the project file");
  // in place (Leave > keep): the history goes, the music stays
  s.reset();
  expect(exportPlainProject(doc.song(), folder, folder, "Song", true, &file, &err) && !std::filesystem::exists(folder + "/.gridline-collab") &&
             loadProject(file, loaded),
         "converted in place: a normal project without the shared history");
}

// ------------------------------------------------------------ the name ----
std::vector<std::string> projectFilesIn(const std::string& folder) {
  std::vector<std::string> out;
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(folder, ec)) {
    const std::string f = e.path().filename().string();
    if (f.size() > 14 && f.compare(f.size() - 14, 14, ".gridline.json") == 0) out.push_back(f);
  }
  std::sort(out.begin(), out.end());
  return out;
}

void testNameRules() {
  expect(cleanProjectName("  Night\tDrive \n ") == "Night Drive" && cleanProjectName("\x01\x02 \x7f") == "" &&
             cleanProjectName("A\xE2\x80\x8B" "B\xE2\x80\xAE" "C") == "ABC" && cleanProjectName("Bad\xC2\x85Name") == "BadName" &&
             cleanProjectName("bad \xFF\xFE utf8") == "bad utf8",
         "a project name is trimmed; control, invisible and malformed characters go");
  const std::string intl = "Nuit \xC3\xA9toil\xC3\xA9" "e \xE5\xA4\x9C \xF0\x9F\x8E\xB9";
  expect(cleanProjectName(intl) == intl && validProjectName(intl), "any script and symbols are kept");
  std::string longName;
  for (int i = 0; i < 100; ++i) longName += "\xC3\xA9";  // 100 x e-acute
  const std::string capped = cleanProjectName(longName);
  expect(capped.size() == kMaxProjectNameChars * 2 && !validProjectName(longName) && validProjectName(capped), "names are capped at 80 characters (not bytes)");
  expect(!validProjectName("") && !validProjectName(" x") && !validProjectName("a\nb"), "only clean names are valid as written");
  // file names
  expect(projectFileStem("a/b:c*d?e\"f<g>h|i\\j") == "a-b-c-d-e-f-g-h-i-j", "path and reserved characters become dashes");
  expect(projectFileStem("..") == "Project" && projectFileStem("") == "Project" && projectFileStem(" . ") == "Project", "nothing usable: Project");
  expect(projectFileStem("../../etc/passwd") == "-..-etc-passwd" && projectFileStem(".hidden") == "hidden", "no hidden files, no way out of the folder");
  expect(projectFileStem("Song. ") == "Song" && projectFileStem("Song...") == "Song", "no trailing dots or spaces (Windows drops them)");
  expect(projectFileStem("CON") == "CON_" && projectFileStem("con.mix") == "con_.mix" && projectFileStem("Lpt1") == "Lpt1_" &&
             projectFileStem("Console") == "Console" && projectFileStem("COM10") == "COM10",
         "reserved Windows device names are changed");
  const std::string stem = projectFileStem(capped);
  expect(stem.size() <= 120 && stem.size() % 2 == 0 && cleanProjectName(stem) == stem, "a long name's file name is cut on a character boundary");
  expect(projectFileStem(intl) == intl, "a name in any script is its own file name");
}

void testRenameLive() {
  Trio t(51, "rename");
  settle(t.net, t.all(), t.clock);
  std::vector<std::string> told;
  SiteId by = 0;
  t.b.s->onRenamed = [&](const std::string& before, const std::string& after, SiteId who) {
    told.push_back(before + " -> " + after);
    by = who;
  };
  for (Peer* p : t.all()) p->s->saveNow();
  expect(t.a.s->projectName() == "Song" && projectFilesIn(t.b.folder) == std::vector<std::string>{"Song.gridline.json"}, "every copy starts as Song");
  // a member who is not the admin renames: everyone gets it, live
  std::string err;
  expect(t.b.s->rename("  Night   Drive ", &err), "any member can rename the project");
  settle(t.net, t.all(), t.clock);
  for (Peer* p : t.all()) p->s->saveNow();
  expect(t.a.s->projectName() == "Night Drive" && t.c.s->projectName() == "Night Drive", "the new name reaches everyone");
  expect(projectFilesIn(t.a.folder) == std::vector<std::string>{"Night Drive.gridline.json"} &&
             projectFilesIn(t.c.folder) == std::vector<std::string>{"Night Drive.gridline.json"} &&
             samePath(t.c.s->projectFile(), t.c.folder + "/Night Drive.gridline.json") && Session::projectNameOf(t.c.folder) == "Night Drive" &&
             Session::projectFileOf(t.c.folder) == t.c.s->projectFile(),
         "each computer's project file is renamed to match (and collab.json records it)");
  Song plain;
  expect(loadProject(t.c.s->projectFile(), plain) && plain.channels.size() == 2, "the renamed file is the current project");
  expect(told.size() == 1 && told[0] == "Song -> Night Drive" && by == t.b.s->site(), "the renamer is told once (who and what)");
  // the same name: nothing is sent
  const uint64_t ops = t.a.s->opCount();
  expect(t.a.s->rename("Night Drive", &err) && t.a.s->rename(" Night Drive\n", &err), "renaming to the same name is accepted");
  settle(t.net, t.all(), t.clock);
  expect(t.a.s->opCount() == ops && told.size() == 1, "...and changes nothing");
  expect(!t.a.s->rename(" \t\x01", &err) && !err.empty() && t.a.s->projectName() == "Night Drive", "an empty name is refused");
  // simultaneous live renames: one wins everywhere (last writer by the clock), no review item
  t.a.s->rename("From Alex");
  t.c.s->rename("From Kim");
  settle(t.net, t.all(), t.clock);
  const std::string won = t.a.s->projectName();
  expect((won == "From Alex" || won == "From Kim") && t.b.s->projectName() == won && t.c.s->projectName() == won,
         "simultaneous renames converge on one name");
  expect(t.a.s->conflicts().openCount() == 0, "renames made while connected never go to the review");
  // undo: one step of its own; it leaves a newer rename by someone else alone
  t.a.s->rename("Undo me");
  settle(t.net, t.all(), t.clock);
  t.a.doc.undo();
  settle(t.net, t.all(), t.clock);
  expect(t.b.s->projectName() == won, "undo of a rename writes the previous name back for everyone");
  t.a.doc.redo();
  settle(t.net, t.all(), t.clock);
  expect(t.c.s->projectName() == "Undo me", "redo renames again");
  t.b.s->rename("Sam's later name");
  settle(t.net, t.all(), t.clock);
  t.a.doc.undo();
  settle(t.net, t.all(), t.clock);
  expect(t.a.s->projectName() == "Sam's later name", "undo never clobbers a newer rename by someone else");
  // a case-only rename: the file takes the new spelling also on case-insensitive file systems
  t.a.s->rename("SAM'S LATER NAME");
  settle(t.net, t.all(), t.clock);
  for (Peer* p : t.all()) p->s->saveNow();
  expect(projectFilesIn(t.b.folder) == std::vector<std::string>{"SAM'S LATER NAME.gridline.json"}, "a case-only rename renames the file");
  // a name that is not a file name: the file is sanitised, the name is kept whole
  t.a.s->rename("CON: a/b ...");
  settle(t.net, t.all(), t.clock);
  for (Peer* p : t.all()) p->s->saveNow();
  expect(t.b.s->projectName() == "CON: a/b ..." && projectFilesIn(t.b.folder) == std::vector<std::string>{"CON- a-b.gridline.json"},
         "the display keeps the name, the file name is made safe");
  // another file has the name: never replaced, the project takes "(2)"
  std::ofstream(t.c.folder + "/Taken.gridline.json") << "{\"mine\":1}";
  t.a.s->rename("Taken");
  settle(t.net, t.all(), t.clock);
  for (Peer* p : t.all()) p->s->saveNow();
  std::string mine;
  readFileText(t.c.folder + "/Taken.gridline.json", mine);
  expect(mine == "{\"mine\":1}" && samePath(t.c.s->projectFile(), t.c.folder + "/Taken (2).gridline.json") &&
             std::filesystem::exists(t.c.s->projectFile()) && projectFilesIn(t.a.folder) == std::vector<std::string>{"Taken.gridline.json"},
         "a file of another name is never replaced (the project file takes \"Taken (2)\")");
  expect(converged(t), "the songs stay the same throughout");
}

void testRenameOffline() {
  // B is offline (Gridline closed) while A renames; B catches up when they are online together
  Trio t(53, "rename_off", false);
  settle(t.net, {&t.a, &t.b}, t.clock);
  t.net.disconnect(&t.a, &t.b);
  const std::string bFolder = t.b.folder;
  t.b.s.reset();  // closed
  t.a.s->rename("While you were away");
  run(t.net, {&t.a}, t.clock, 0.5);
  std::string err;
  t.b.s = Session::open(t.b.doc, config(bFolder, t.b.id, "Sam", &t.clock), &err);
  expect(t.b.s && t.b.s->projectName() == "Song" && samePath(t.b.s->projectFile(), bFolder + "/Song.gridline.json"), "B reopens under the old name");
  if (!t.b.s) return;
  std::vector<std::string> told;
  t.b.s->onRenamed = [&](const std::string&, const std::string& after, SiteId) { told.push_back(after); };
  t.net.wire(&t.b);
  t.net.connect(&t.a, &t.b);
  settle(t.net, {&t.a, &t.b}, t.clock);
  t.b.s->saveNow();
  expect(t.b.s->projectName() == "While you were away" && told.size() == 1 && projectFilesIn(bFolder) == std::vector<std::string>{"While you were away.gridline.json"} &&
             Session::projectNameOf(bFolder) == "While you were away",
         "an offline rename arrives with the catch-up (name, file, collab.json)");
  // the file is found by the folder, whatever it is called: renamed by hand while closed, with a stale record
  t.b.s.reset();
  std::filesystem::rename(bFolder + "/While you were away.gridline.json", bFolder + "/renamed by hand.gridline.json");
  expect(samePath(Session::projectFileOf(bFolder), bFolder + "/renamed by hand.gridline.json"), "the project file is found in its folder after a rename");
  t.b.s = Session::open(t.b.doc, config(bFolder, t.b.id, "Sam", &t.clock), &err);
  expect(t.b.s && t.b.s->projectName() == "While you were away" && projectFilesIn(bFolder) == std::vector<std::string>{"While you were away.gridline.json"},
         "opening it renames it back after the project's name");
  // projects made before the shared file record: collab.json without "file", a file named after the name
  t.b.s.reset();
  std::string cj;
  readFileText(bFolder + "/.gridline-collab/collab.json", cj);
  Json j;
  jsonParse(cj, j);
  j.object.erase("file");
  j.object.erase("name");
  writeFileDurable(bFolder + "/.gridline-collab/collab.json", jsonStringify(j, 2));
  expect(Session::projectNameOf(bFolder).empty() && samePath(Session::projectFileOf(bFolder), bFolder + "/While you were away.gridline.json"),
         "a folder without a name record still finds its file");
  t.b.s = Session::open(t.b.doc, config(bFolder, t.b.id, "Sam", &t.clock), &err);
  expect(t.b.s && t.b.s->projectName() == "While you were away", "...and opens with the project's name");
  // the static fallback: no record at all, nothing written yet
  const std::string empty = tmpDir("rename_none");
  expect(samePath(Session::projectFileOf(empty), empty + "/Project.gridline.json"), "no record, no file: Project.gridline.json");
}

void testRenameConflict() {
  // both rename while apart (minutes apart): the review gets it, like any property
  Trio t(55, "rename_conf", false);
  settle(t.net, {&t.a, &t.b}, t.clock);
  t.net.disconnect(&t.a, &t.b);
  t.a.s->rename("Alex's name");
  run(t.net, {&t.a, &t.b}, t.clock, 0.1);
  t.clock.ms += 60000;
  t.b.s->rename("Sam's name");
  run(t.net, {&t.a, &t.b}, t.clock, 0.1);
  t.net.connect(&t.a, &t.b);
  settle(t.net, {&t.a, &t.b}, t.clock);
  const ConflictObject* name = nullptr;
  for (const ConflictObject* c : t.a.s->conflicts().open())
    if (c->object == joinPath({"meta", "project", "name"})) name = c;
  expect(name && t.b.s->conflicts().openCount() == 1, "offline renames on both sides become one review item on every peer");
  expect(t.a.s->projectName() == "Sam's name" && t.b.s->projectName() == "Sam's name", "until the admin chooses, the later rename is in place");
  if (!name) return;
  const auto sides = ConflictTracker::sides(*name);
  expect(sides.size() == 2 && sides[0].ops.size() == 1 && sides[0].ops[0]->value.str == "Alex's name" && sides[0].ops[0]->prev.str == "Song" &&
             sides[1].ops[0]->value.str == "Sam's name",
         "each side shows its rename from the old name to its own");
  std::string err;
  expect(t.a.s->resolve(name->object, t.a.s->site(), &err), "the admin keeps one side");
  settle(t.net, {&t.a, &t.b}, t.clock);
  for (Peer* p : {&t.a, &t.b}) p->s->saveNow();
  expect(t.b.s->projectName() == "Alex's name" && t.b.s->conflicts().openCount() == 0 &&
             projectFilesIn(t.b.folder) == std::vector<std::string>{"Alex's name.gridline.json"},
         "the admin's choice renames the project (and the files) everywhere");
}

void testRenameJoinAndStop() {
  // a rename while someone is joining: the joiner ends with the current name, not the invite's
  Trio t(57, "rename_join", false);
  settle(t.net, {&t.a, &t.b}, t.clock);
  const Session::Ticket stale = ticketOf(*t.a.s);  // an invite made before the rename
  t.a.s->rename("Current name");
  settle(t.net, {&t.a, &t.b}, t.clock);
  int told = 0;
  std::string err;
  t.c.s = Session::join(t.c.doc, config(t.c.folder, t.c.id, "Kim", &t.clock), stale, &err);
  t.c.s->onRenamed = [&](const std::string&, const std::string&, SiteId) { ++told; };
  expect(t.c.s->projectName() == "Song", "before the first sync the joiner shows the invite's name");
  t.a.s->admitMember(memberOf(t.c));
  t.net.add(&t.c);
  t.net.connect(&t.a, &t.c);
  run(t.net, t.all(), t.clock, 0.03);  // mid-join: another rename goes out
  t.b.s->rename("Renamed mid-join");
  settle(t.net, t.all(), t.clock);
  t.c.s->saveNow();
  expect(t.c.s->ready() && t.c.s->projectName() == "Renamed mid-join" && t.a.s->projectName() == "Renamed mid-join" &&
             projectFilesIn(t.c.folder) == std::vector<std::string>{"Renamed mid-join.gridline.json"},
         "the joiner ends with the current name and file (a stale invite name never wins)");
  expect(told <= 1, "the name a join receives is not announced as a rename (at most the rename made during the join)");
  // a rename racing Stop sharing: nothing crashes, the stopped copy keeps its name
  t.net.connect(&t.b, &t.c);
  t.a.s->leave(true);
  expect(!t.a.s->rename("After stop", &err) && !err.empty(), "a stopped project can't be renamed for everyone");
  t.b.s->rename("Racing the stop");
  settle(t.net, t.all(), t.clock);
  expect(t.a.s->projectName() == "Renamed mid-join" && t.c.s->projectName() == "Racing the stop", "a rename after Stop sharing does not reach the stopped copy");
}

}  // namespace

int main() {
  testKeys();
  testDiffRoundTrip();
  testReplicaConvergence();
  testJoinSync();
  for (uint32_t seed : {101u, 202u, 303u}) testFuzz(seed);
  // GRIDLINE_COLLAB_SEEDS=n: n more random seeds (long runs)
  if (const char* more = std::getenv("GRIDLINE_COLLAB_SEEDS"))
    for (uint32_t k = 1; k <= static_cast<uint32_t>(std::atoi(more)); ++k) testFuzz(1000u + k * 7919u);
  testUndoLocality();
  testOpLog();
  testReopen();
  testFiles();
  testPolicy();
  testRelay();
  testConflicts();
  testProfile();
  testVersionRecord();
  testRemoval();
  testRemovalLateOps();
  testReadmitAndLeave();
  testRemovalPurge();
  testStopSharing();
  testUnusedAudio();
  testExport();
  testNameRules();
  testRenameLive();
  testRenameOffline();
  testRenameConflict();
  testRenameJoinAndStop();
  return finish();
}
