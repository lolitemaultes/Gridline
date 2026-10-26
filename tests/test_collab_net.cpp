// Gridline - J-111: collaboration transport and network hub tests:
// encrypted reliable link under loss / reordering / duplication, handshake
// authentication (wrong secret, tampering), invites (no reply, reply code,
// expiry, reuse), the version gate between peers, relaying through a third
// collaborator and direct links introduced through it.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <deque>
#include <random>
#include "Check.h"
#include "collab/CollabSim.h"
#include "collab/Hub.h"
#include "collab/Link.h"
#include "core/Platform.h"
#include "collab/Transport.h"
#include "collab/Controller.h"
#include "collab/VersionGate.h"
#include "core/Project.h"
#include "core/Settings.h"
#include "core/Update.h"

using namespace gl;
using namespace gl::collab;

namespace {

// Two links over a lossy, reordering, duplicating wire.
struct Wire {
  std::mt19937 rng{5};
  double loss = 0.1, dup = 0.05;
  struct Pkt {
    int to;
    std::string d;
    uint64_t at;
  };
  std::vector<Pkt> q;
  uint64_t now = 1000;
  void push(int to, const std::string& d) {
    std::uniform_real_distribution<double> u(0, 1);
    if (u(rng) < loss) return;
    q.push_back({to, d, now + 5 + rng() % 40});
    if (u(rng) < dup) q.push_back({to, d, now + 5 + rng() % 60});
  }
};

SecureLink::Hs hsFor(bool initiator, const crypto::Identity& me, const crypto::Identity& other, const crypto::Key& psk) {
  SecureLink::Hs h;
  h.initiator = initiator;
  h.psk = psk;
  h.hsid = std::string(16, 'h');
  h.me = me;
  h.remoteDh = other.dhPublic;
  h.remoteDhKnown = initiator;
  h.hello.object["name"] = Json::makeString(initiator ? "I" : "R");
  return h;
}

void testLink() {
  const auto a = crypto::Identity::generate(), b = crypto::Identity::generate();
  const crypto::Key psk = crypto::randomKey();
  Wire w;
  SecureLink la(hsFor(true, a, b, psk)), lb(hsFor(false, b, a, psk));
  la.output = [&](const std::string& d) { w.push(1, d); };
  lb.output = [&](const std::string& d) { w.push(0, d); };
  std::vector<std::string> got0, got1;
  std::string bigGot;
  lb.onMessage = [&](int s, const std::string& m) {
    if (s == 0) got0.push_back(m);
    if (s == 1) bigGot = m;
  };
  la.onMessage = [&](int s, const std::string& m) {
    if (s == 0) got1.push_back(m);
  };
  la.start(w.now);
  lb.start(w.now);
  std::string big(1u << 20, '\0');
  for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>((i * 131) ^ (i >> 7));
  bool sent = false;
  for (int step = 0; step < 60000; ++step) {
    w.now += 1;
    for (size_t i = 0; i < w.q.size();) {
      if (w.q[i].at <= w.now) {
        Wire::Pkt p = w.q[i];
        w.q.erase(w.q.begin() + static_cast<std::ptrdiff_t>(i));
        (p.to == 0 ? la : lb).input(p.d, w.now);
      } else {
        ++i;
      }
    }
    if (!sent && la.state() == SecureLink::State::Open && lb.state() == SecureLink::State::Open) {
      sent = true;
      for (int k = 0; k < 500; ++k) la.send(0, "op " + std::to_string(k));
      la.send(1, big);
      for (int k = 0; k < 50; ++k) lb.send(0, "back " + std::to_string(k));
    }
    if (step % 5 == 0) {
      la.tick(w.now);
      lb.tick(w.now);
    }
    if (got0.size() == 500 && !bigGot.empty() && got1.size() == 50) break;
  }
  expect(la.safetyCode() == lb.safetyCode() && la.safetyCode().size() == 19, "both sides show the same safety code");
  bool ordered = got0.size() == 500;
  for (size_t k = 0; ordered && k < got0.size(); ++k) ordered = got0[k] == "op " + std::to_string(k);
  expect(ordered, "500 messages arrive complete and in order over a link losing 10% of packets, reordering and duplicating");
  expect(bigGot == big, "a 1 MB message arrives intact (retransmissions)");
  expect(got1.size() == 50 && la.packetsResent() > 0, "both directions work; lost data was resent");
  // a tampered data packet is dropped silently
  std::string last;
  la.output = [&](const std::string& d) { last = d; };
  la.send(0, "genuine");
  la.tick(w.now + 100);
  size_t before = got0.size();
  std::string bad = last;
  if (bad.size() > 20) bad[bad.size() - 20] ^= 1;
  lb.input(bad, w.now + 101);
  expect(got0.size() == before, "a tampered packet is rejected (authentication tag)");
  lb.input(last, w.now + 102);
  lb.input(last, w.now + 103);  // a replay of the same packet
  expect(got0.size() == before + 1 && got0.back() == "genuine", "the genuine packet is accepted once, its replay is ignored");

  // a wrong pre-shared key (wrong invite secret) never opens a link
  SecureLink ia(hsFor(true, a, b, crypto::randomKey())), ib(hsFor(false, b, a, psk));
  std::deque<std::pair<int, std::string>> q;
  ia.output = [&](const std::string& d) { q.emplace_back(1, d); };
  ib.output = [&](const std::string& d) { q.emplace_back(0, d); };
  ia.start(0);
  ib.start(0);
  for (uint64_t t = 0; t < 5000; t += 10) {
    while (!q.empty()) {
      auto p = q.front();
      q.pop_front();
      (p.first == 0 ? ia : ib).input(p.second, t);
    }
    ia.tick(t);
    ib.tick(t);
  }
  expect(ia.state() == SecureLink::State::Handshake && ib.state() == SecureLink::State::Handshake,
         "a wrong invite secret is rejected silently (no link, no reply)");
}

// J-111 fix: the handshake no longer depends on what the network did before it
// started, on the initiator repeating H1, or on the responder's idle timer.
void testHandshakeRecovery() {
  const auto a = crypto::Identity::generate(), b = crypto::Identity::generate();
  const crypto::Key psk = crypto::randomKey();
  {  // ICE connects only after 40 s: nothing is sent and no budget is used before start()
    SecureLink ia(hsFor(true, a, b, psk)), ib(hsFor(false, b, a, psk));
    std::deque<std::pair<int, std::string>> q;
    int early = 0;
    bool started = false;
    ia.output = [&](const std::string& d) {
      if (!started) ++early;
      q.emplace_back(1, d);
    };
    ib.output = [&](const std::string& d) { q.emplace_back(0, d); };
    ib.start(0);
    for (uint64_t t = 0; t < 40000; t += 10) ia.tick(t);
    started = true;
    ia.start(40000);
    for (uint64_t t = 40000; t < 42000; t += 10) {
      while (!q.empty()) {
        auto p = q.front();
        q.pop_front();
        (p.first == 0 ? ia : ib).input(p.second, t);
      }
      ia.tick(t);
      ib.tick(t);
    }
    expect(early == 0, "the initiator sends nothing before start (ICE not connected yet)");
    expect(ia.state() == SecureLink::State::Open && ib.state() == SecureLink::State::Open,
           "a handshake that starts 40 s after the link was made still opens (the H1 budget starts with ICE)");
  }
  {  // the first H2 is lost and the initiator never repeats H1: the responder repeats H2
    SecureLink ia(hsFor(true, a, b, psk)), ib(hsFor(false, b, a, psk));
    std::deque<std::pair<int, std::string>> q;
    int fromB = 0;
    ia.output = [&](const std::string& d) { q.emplace_back(1, d); };
    ib.output = [&](const std::string& d) {
      if (fromB++ > 0) q.emplace_back(0, d);  // the first H2 is dropped (ICE not connected on that side yet)
    };
    ib.start(0);
    ia.start(0);
    bool sentData = false;
    for (uint64_t t = 0; t < 3000; t += 10) {
      while (!q.empty()) {
        auto p = q.front();
        q.pop_front();
        (p.first == 0 ? ia : ib).input(p.second, t);
      }
      ib.tick(t);  // (the initiator is not ticked: it never repeats H1)
      if (ia.state() == SecureLink::State::Open && !sentData) {
        sentData = true;
        ia.send(0, "hello");
      }
    }
    expect(ia.state() == SecureLink::State::Open && ib.heardData(), "a lost H2 is repeated by the responder until the initiator's data arrives");
  }
  {  // the initiator keeps repeating H1 (its H2s are lost): the responder does not time out at 15 s
    SecureLink ia(hsFor(true, a, b, psk)), ib(hsFor(false, b, a, psk));
    std::deque<std::pair<int, std::string>> q;
    ia.output = [&](const std::string& d) { q.emplace_back(1, d); };
    ib.output = [&](const std::string&) {};  // everything the responder sends is lost
    ib.start(0);
    ia.start(0);
    for (uint64_t t = 0; t < 25000; t += 10) {
      while (!q.empty()) {
        auto p = q.front();
        q.pop_front();
        (p.first == 0 ? ia : ib).input(p.second, t);
      }
      ia.tick(t);
      ib.tick(t);
    }
    expect(ib.state() == SecureLink::State::Open, "repeated H1s keep the responder's side open past its 15 s idle timeout");
  }
  {  // a link that gave up stays closed: starting it later sends nothing
    SecureLink ia(hsFor(true, a, b, psk));
    int sent = 0;
    ia.output = [&](const std::string&) { ++sent; };
    ia.start(0);
    for (uint64_t t = 0; t < 31000; t += 10) ia.tick(t);
    const int before = sent;
    ia.start(32000);
    ia.tick(33000);
    expect(ia.state() == SecureLink::State::Closed && sent == before, "a handshake that ran out stays closed (no late H1 the responder would accept alone)");
  }
}

// ------------------------------------------------------------------ hubs ----
struct Machine {
  std::string label;
  sim::Peer peer;
  std::unique_ptr<Hub> hub;
  std::string cfg;
};

struct World {
  sim::Clock clock;
  std::shared_ptr<MemFabric::Shared> net = MemFabric::makeNet();
  double now = 0;
  int64_t wall = 1'800'000'000;
  std::vector<Machine*> ms;
  void make(Machine& m, const std::string& label, const std::string& version = "0.7.0") {
    m.label = label;
    m.peer.label = label;
    m.peer.id = crypto::Identity::generate();
    m.cfg = sim::tmpDir("net_cfg_" + label);
    Profile p;
    p.name = label;
    VersionInfo v;
    v.app = version;
    v.build = "test";
    m.hub = std::make_unique<Hub>(std::make_unique<MemFabric>(net, label), m.peer.id, p, v, m.cfg);
    m.hub->wallSec = [this] { return wall; };
    ms.push_back(&m);
  }
  void run(double seconds) {
    const double end = now + seconds;
    while (now < end) {
      now += 0.01;
      clock.ms += 10;
      for (Machine* m : ms) {
        m->hub->poll(now);
        if (m->peer.s) m->peer.s->frame(now);
      }
    }
  }
};

void createProject(World& w, Machine& m, const std::string& name) {
  m.peer.doc.reset(makeEmptySong());
  m.peer.doc.addChannel("Kick", "gl.kick");
  m.peer.folder = sim::tmpDir(name);
  m.peer.s = Session::create(m.peer.doc, sim::config(m.peer.folder, m.peer.id, m.label, &w.clock));
  m.hub->setSession(m.peer.s.get());
}

bool joinWith(World& w, Machine& m, const std::string& code, const std::string& name, std::string* err = nullptr) {
  InviteInfo info;
  if (!parseInviteCode(code, info, err)) return false;
  m.peer.folder = sim::tmpDir(name);
  m.peer.s = Session::join(m.peer.doc, sim::config(m.peer.folder, m.peer.id, m.label, &w.clock), info.ticket());
  m.hub->setSession(m.peer.s.get());
  return m.hub->join(info, err);
}

std::string readyCode(World& w, Machine& host, const std::string& id) {
  for (int i = 0; i < 1000; ++i) {
    const auto* v = host.hub->invite(id);
    if (v && v->status == Hub::InviteStatus::Ready) return v->code;
    w.run(0.05);
  }
  return {};
}

void testInviteFlow() {
  World w;
  Machine a, b;
  w.make(a, "Alex");
  w.make(b, "Sam");
  createProject(w, a, "hub_a");
  const std::string id = a.hub->createInvite(60);
  const std::string code = readyCode(w, a, id);
  expect(codeKind(code) == 'i' && code.rfind(kInvitePrefix, 0) == 0, "an invite produces a code");
  InviteInfo info;
  expect(parseInviteCode("Hi! here is the code:\n" + code + "\nsee you", info) && info.inviterName == "Alex" &&
             info.inviterFingerprint() == a.peer.id.fingerprint(),
         "a code pasted with surrounding chat text still parses");
  std::string bad = code;
  bad[bad.size() / 2] = bad[bad.size() / 2] == 'A' ? 'B' : 'A';
  InviteInfo junk;
  expect(!parseInviteCode(bad, junk) || junk.inviteId != info.inviteId || junk.secret != info.secret || junk.ice != info.ice,
         "a damaged code is not taken for the original");
  std::string err;
  expect(joinWith(w, b, code, "hub_b", &err), "the invitee joins with the code");
  for (int i = 0; i < 400 && !(b.peer.s->ready() && sim::songText(b.peer) == sim::songText(a.peer)); ++i) w.run(0.05);
  expect(b.hub->joinView().status == Hub::JoinStatus::Connected && b.peer.s->ready(), "joined without a reply code (reachable inviter)");
  expect(sim::songText(b.peer) == sim::songText(a.peer), "the project syncs to the invitee");
  expect(a.hub->invite(id)->status == Hub::InviteStatus::Connected && a.hub->invite(id)->joinerName == "Sam" &&
             a.hub->invite(id)->safety == b.hub->joinView().safety && !b.hub->joinView().safety.empty(),
         "both see the same safety code and the inviter sees who joined");
  // live edits both ways through the encrypted link
  a.peer.doc.renameChannel(a.peer.doc.song().channels[0].id, "From Alex");
  a.peer.s->frame(w.now);
  b.peer.doc.addChannel("From Sam", "gl.hat");
  w.run(1.0);
  if (sim::songText(a.peer) != sim::songText(b.peer)) {
    std::printf("# A channels:");
    for (auto& c : a.peer.doc.song().channels) std::printf(" %u:%s", c.id, c.name.c_str());
    std::printf("\n# B channels:");
    for (auto& c : b.peer.doc.song().channels) std::printf(" %u:%s", c.id, c.name.c_str());
    std::printf("\n# pending A %zu B %zu\n", a.peer.s->pendingBatches(), b.peer.s->pendingBatches());
  }
  expect(sim::songText(a.peer) == sim::songText(b.peer) && b.peer.doc.song().channels[0].name == "From Alex", "edits flow both ways");
  // the invite is one-time: a second person with the same code is refused
  Machine c;
  w.make(c, "Kim");
  joinWith(w, c, code, "hub_c");
  w.run(125.0);  // (a join's ICE gets 2 min before it fails)
  expect(c.hub->joinView().status == Hub::JoinStatus::Refused || c.hub->joinView().status == Hub::JoinStatus::Failed,
         "a used invite does not let a second person in");
  expect(a.peer.s->members().size() == 2, "the refused person did not become a member");
}

void testReplyAndExpiry() {
  // both behind NAT: the reply code is needed
  World w;
  Machine a, b;
  w.make(a, "Alex");
  w.make(b, "Sam");
  MemFabric probe(w.net, "x");
  probe.natBoth = true;
  probe.shared();
  createProject(w, a, "reply_a");
  const std::string id = a.hub->createInvite(15);
  const std::string code = readyCode(w, a, id);
  joinWith(w, b, code, "reply_b");
  w.run(7.0);
  expect(b.hub->joinView().status == Hub::JoinStatus::NeedReply && codeKind(b.hub->joinView().reply) == 'r',
         "without a direct path the invitee is shown a reply code");
  std::string err;
  expect(a.hub->acceptReply(b.hub->joinView().reply, &err), "the inviter accepts the reply code");
  for (int i = 0; i < 200 && !b.peer.s->ready(); ++i) w.run(0.05);
  w.run(1.0);
  expect(b.peer.s->ready() && sim::songText(a.peer) == sim::songText(b.peer), "with the reply code both connect and sync");
  // expiry
  Machine c;
  w.make(c, "Kim");
  const std::string id2 = a.hub->createInvite(15);
  const std::string code2 = readyCode(w, a, id2);
  w.wall += 16 * 60;
  InviteInfo info;
  parseInviteCode(code2, info);
  expect(info.expired(w.wall), "invites expire");
  joinWith(w, c, code2, "reply_c", &err);
  expect(!err.empty() && err.find("expired") != std::string::npos, "an expired invite is refused before connecting");
  w.run(0.2);
  expect(a.hub->invite(id2)->status == Hub::InviteStatus::Expired, "the inviter's invite shows as expired");
}

void testVersionGate() {
  World w;
  Machine a, b;
  w.make(a, "Alex", "0.7.0");
  w.make(b, "Sam", "0.7.1");
  std::vector<Hub::Refusal> ra, rb;
  a.hub->onVersionRefusal = [&](const Hub::Refusal& r) { ra.push_back(r); };
  b.hub->onVersionRefusal = [&](const Hub::Refusal& r) { rb.push_back(r); };
  createProject(w, a, "ver_a");
  const std::string code = readyCode(w, a, a.hub->createInvite(60));
  joinWith(w, b, code, "ver_b");
  w.run(3.0);
  expect(b.hub->joinView().status == Hub::JoinStatus::Refused && !b.peer.s->ready(), "peers on different versions do not connect");
  expect(!ra.empty() && ra.back().weAreOlder && ra.back().who == "Sam" && fmtVersionOlder(ra.back()) == "Sam is on Gridline 0.7.1; update to collaborate.",
         "the older peer is told to update (\"Sam is on Gridline 0.7.1; update to collaborate.\")");
  expect(!rb.empty() && !rb.back().weAreOlder && fmtVersionNewer(rb.back()).find("Alex needs to update") == 0,
         "the newer peer is told the other one needs to update");
  expect(a.peer.s->members().size() == 1, "nobody was admitted");
  // same version string, different build
  World w2;
  Machine c, d;
  w2.make(c, "Alex", "0.7.0");
  w2.make(d, "Sam", "0.7.0");
  Hub::Refusal last;
  bool got = false;
  d.hub->onVersionRefusal = [&](const Hub::Refusal& r) {
    last = r;
    got = true;
  };
  // rebuild d's hub with another build id
  VersionInfo v;
  v.app = "0.7.0";
  v.build = "other";
  Profile p;
  p.name = "Sam";
  d.hub = std::make_unique<Hub>(std::make_unique<MemFabric>(w2.net, "Sam"), d.peer.id, p, v, d.cfg);
  d.hub->wallSec = [&w2] { return w2.wall; };
  d.hub->onVersionRefusal = [&](const Hub::Refusal& r) {
    last = r;
    got = true;
  };
  createProject(w2, c, "ver_c");
  const std::string code2 = readyCode(w2, c, c.hub->createInvite(60));
  joinWith(w2, d, code2, "ver_d");
  w2.run(3.0);
  expect(got && last.sameVersionOtherBuild && d.hub->joinView().status == Hub::JoinStatus::Refused,
         "two different builds with the same version string are refused too");
}

void testRelayAndMesh() {
  World w;
  Machine a, b, c;
  w.make(a, "Alex");
  w.make(b, "Sam");
  w.make(c, "Kim");
  MemFabric::block(w.net, "Alex", "Kim");  // Alex and Kim cannot reach each other directly
  createProject(w, a, "mesh_a");
  joinWith(w, b, readyCode(w, a, a.hub->createInvite(60)), "mesh_b");
  for (int i = 0; i < 200 && !b.peer.s->ready(); ++i) w.run(0.05);
  // Sam invites Kim
  joinWith(w, c, readyCode(w, b, b.hub->createInvite(60)), "mesh_c");
  for (int i = 0; i < 400 && !c.peer.s->ready(); ++i) w.run(0.05);
  w.run(10.0);
  expect(c.peer.s->ready() && a.peer.s->members().size() == 3, "an invitee of an invitee becomes a member everywhere");
  const auto lv = a.hub->link(c.peer.id.site());
  expect(lv.state == Hub::LinkView::State::Relayed && lv.via == "Sam", "Alex reaches Kim through Sam when no direct path exists");
  a.peer.doc.renameChannel(a.peer.doc.song().channels[0].id, "Relayed edit");
  w.run(1.0);
  expect(c.peer.doc.song().channels[0].name == "Relayed edit", "edits travel through the collaborator in between");
  // presence travels through the relay as well
  Presence p;
  p.window = "Playlist";
  p.x = 0.25f;
  p.y = 0.5f;
  p.kind = 2;  // J-111d: an anchor (a playlist track: tick, fraction of the row)
  p.a = 3;
  p.b = 0xFFFFFFF0u;
  p.cx = 1536.5f;
  p.cy = 0.25f;
  a.peer.s->setPresence(p);
  w.run(0.5);
  const PeerView* pv = c.peer.s->peer(a.peer.id.site());
  expect(pv && pv->presence.window == "Playlist" && pv->presence.x == 0.25f, "cursor positions travel through the relay");
  expect(pv && pv->presence.kind == 2 && pv->presence.a == 3 && pv->presence.b == 0xFFFFFFF0u && pv->presence.cx == 1536.5f &&
             pv->presence.cy == 0.25f,
         "cursor anchors travel with the position (kind, ids, content coordinates)");

  // without the block, members reachable through a third one connect directly (introduction)
  World w2;
  Machine x, y, z;
  w2.make(x, "Alex");
  w2.make(y, "Sam");
  w2.make(z, "Kim");
  createProject(w2, x, "intro_a");
  joinWith(w2, y, readyCode(w2, x, x.hub->createInvite(60)), "intro_b");
  for (int i = 0; i < 200 && !y.peer.s->ready(); ++i) w2.run(0.05);
  joinWith(w2, z, readyCode(w2, y, y.hub->createInvite(60)), "intro_c");
  for (int i = 0; i < 400 && !z.peer.s->ready(); ++i) w2.run(0.05);
  w2.run(15.0);
  expect(x.hub->link(z.peer.id.site()).state == Hub::LinkView::State::Direct, "members introduced through a third one connect directly without a code");
  // reconnect after a restart: the last known address is enough
  y.hub->setSession(nullptr);
  const std::string folder = y.peer.folder;
  y.peer.s.reset();
  w2.run(1.0);
  y.peer.s = Session::open(y.peer.doc, sim::config(folder, y.peer.id, "Sam", &w2.clock));
  y.hub->setSession(y.peer.s.get());
  for (int i = 0; i < 600 && y.hub->link(x.peer.id.site()).state != Hub::LinkView::State::Direct; ++i) w2.run(0.05);
  expect(y.hub->link(x.peer.id.site()).state == Hub::LinkView::State::Direct, "after a restart members reconnect by themselves");
}


// ------------------------------------------------------------ version gate ----
update::Fetcher listFetcher(const std::vector<update::Release>& rels, int status = 200, bool offline = false) {
  std::string body = "[";
  for (size_t i = 0; i < rels.size(); ++i) body += (i ? "," : "") + update::releaseJson(rels[i]);
  body += "]";
  return [body, status, offline](const std::string&) {
    update::HttpResult r;
    r.offline = offline;
    r.status = offline ? 0 : status;
    r.body = status == 200 ? body : std::string("{\"message\":\"API rate limit exceeded\"}");
    return r;
  };
}

void testVersionGateRule() {
  const update::Target mac{"macos", "aarch64"};
  auto rel = [](const std::string& v) { return update::fakeRelease(v); };
  // behind, equal, ahead
  expect(evaluate({rel("0.7.0"), rel("0.6.1")}, "0.6.1", mac).kind == GateState::Kind::UpdateRequired, "behind the newest release: update required");
  expect(evaluate({rel("0.7.0")}, "0.7.0", mac).kind == GateState::Kind::UpToDate, "on the newest release: allowed");
  expect(evaluate({rel("0.6.1")}, "0.7.0", mac).kind == GateState::Kind::UpToDate, "a build newer than the newest release (preview build) is allowed");
  // prereleases never count; a beta newer than the newest stable passes, one older than it does not
  update::Release beta = rel("0.8.0-beta.1");
  beta.prerelease = true;
  expect(evaluate({beta, rel("0.7.0")}, "0.7.0", mac).kind == GateState::Kind::UpToDate, "a prerelease on GitHub does not lock stable users out");
  expect(evaluate({rel("0.7.0")}, "0.8.0-beta.2", mac).kind == GateState::Kind::UpToDate, "a beta user newer than the newest stable passes");
  expect(evaluate({rel("0.7.0")}, "0.7.0-beta.3", mac).kind == GateState::Kind::UpdateRequired, "a beta of an already released version must update");
  // a fresh tag whose installers are not attached yet is ignored
  update::Release bare = rel("0.7.1");
  bare.assets.clear();
  const GateState g = evaluate({bare, rel("0.7.0")}, "0.7.0", mac);
  expect(g.kind == GateState::Kind::UpToDate && g.latest == "0.7.0", "a release without a download for this computer is not required yet");
  update::Release otherOnly = rel("0.7.2");
  otherOnly.assets.erase(std::remove_if(otherOnly.assets.begin(), otherOnly.assets.end(),
                                        [](const update::Asset& x) { return x.name.find("macos") != std::string::npos; }),
                         otherOnly.assets.end());
  expect(evaluate({otherOnly, rel("0.7.0")}, "0.7.0", mac).latest == "0.7.0", "...also when only other platforms have it");

  // the check, its cache and failures
  Settings st;
  VersionGate gate(st, "0.7.0");
  const int64_t t0 = 1'800'000'000;
  expect(gate.state(t0).kind == GateState::Kind::Unknown && gate.due(t0), "never checked: unknown (allowed) and due");
  GateState r = gate.check(listFetcher({rel("0.7.1"), rel("0.7.0")}), t0, mac);
  expect(r.kind == GateState::Kind::UpdateRequired && r.latest == "0.7.1" && st.getString("collab.latestVersion") == "0.7.1",
         "a check finds 0.7.1 and remembers it");
  expect(!gate.due(t0 + 600) && gate.due(t0 + 3700), "checked at most once an hour");
  r = gate.check(listFetcher({}, 200, true), t0 + 4000, mac);
  expect(r.couldNotCheck && r.kind == GateState::Kind::UpdateRequired, "offline: the last known latest version still decides");
  r = gate.check(listFetcher({}, 403), t0 + 5000, mac);
  expect(r.couldNotCheck && r.detail == "rate limited" && r.kind == GateState::Kind::UpdateRequired, "rate limited: the cache decides, noted as not checked");
  Settings fresh;
  VersionGate never(fresh, "0.7.0");
  r = never.check(listFetcher({}, 200, true), t0, mac);
  expect(r.couldNotCheck && r.allows(), "offline without any cache: allowed (the peer handshake decides)");
  // "Skip this version" / "Remind me later" only govern the popup
  Settings sk;
  sk.set("update.skipVersion", std::string("0.7.1"));
  sk.set("update.remindAfter", static_cast<double>(t0 + 86400));
  sk.set("update.auto", false);
  VersionGate g2(sk, "0.7.0");
  expect(g2.check(listFetcher({rel("0.7.1")}), t0, mac).kind == GateState::Kind::UpdateRequired, "skipping a version in the popup does not bypass the gate");

  // a project written by a newer Gridline refuses an older one
  Document doc;
  doc.reset(makeEmptySong());
  const std::string cfgDir = sim::tmpDir("gate_cfg");
  Controller newer(cfgDir + "/newer");
  newer.fabricFactory = [] { return std::make_unique<MemFabric>(); };
  newer.version.app = "0.7.3";
  Profile p;
  p.name = "Alex";
  newer.setProfile(p);
  const std::string folder = sim::tmpDir("gate_project");
  expect(newer.create(doc, folder, "Song", nullptr), "a newer Gridline makes a collaborative project");
  newer.close();
  expect(Session::minAppVersionOf(folder) == "0.7.3", "the project records the newest version that wrote it");
  Controller older(cfgDir + "/newer");  // the same person, another Gridline
  older.fabricFactory = [] { return std::make_unique<MemFabric>(); };
  older.version.app = "0.7.0";
  older.setProfile(p);
  std::string err, need;
  Document doc2;
  expect(!older.open(doc2, folder, &err, &need) && need == "0.7.3", "an older Gridline is blocked from opening it (update to 0.7.3)");
  Controller same(cfgDir + "/newer");  // the same person, another Gridline
  same.fabricFactory = [] { return std::make_unique<MemFabric>(); };
  same.version.app = "0.7.3";
  same.setProfile(p);
  expect(same.open(doc2, folder, &err, &need), "the same version opens it");
  // the minimum never goes down
  same.close();
  Controller later(cfgDir + "/newer");  // the same person, another Gridline
  later.fabricFactory = [] { return std::make_unique<MemFabric>(); };
  later.version.app = "0.8.0";
  later.setProfile(p);
  later.open(doc2, folder, &err, &need);
  later.session()->saveNow();
  later.close();
  expect(Session::minAppVersionOf(folder) == "0.8.0", "opening with a newer version raises the minimum (it never goes down)");
  // someone who is not a member (a copied folder) cannot open it as a collaborative project
  Controller stranger(cfgDir + "/stranger");
  stranger.fabricFactory = [] { return std::make_unique<MemFabric>(); };
  stranger.version.app = "0.8.0";
  stranger.setProfile(p);
  expect(!stranger.open(doc2, folder, &err, &need) && !err.empty(), "a stranger with a copy of the folder is refused");
}


// ------------------------------------------------------- membership (J-111e) ----
void testRemovalNet() {
  // relay cut-off: Alex reaches Kim only through Sam; Alex removes Kim
  World w;
  Machine a, b, c;
  w.make(a, "Alex");
  w.make(b, "Sam");
  w.make(c, "Kim");
  MemFabric::block(w.net, "Alex", "Kim");
  createProject(w, a, "rm_a");
  joinWith(w, b, readyCode(w, a, a.hub->createInvite(60)), "rm_b");
  for (int i = 0; i < 200 && !b.peer.s->ready(); ++i) w.run(0.05);
  joinWith(w, c, readyCode(w, b, b.hub->createInvite(60)), "rm_c");
  for (int i = 0; i < 400 && !c.peer.s->ready(); ++i) w.run(0.05);
  w.run(10.0);
  const SiteId kim = c.peer.id.site(), sam = b.peer.id.site();
  expect(a.hub->link(kim).state == Hub::LinkView::State::Relayed, "Alex reaches Kim through Sam");
  std::string err;
  expect(a.peer.s->removeMember(kim, "Thanks for the help", &err), "the admin removes Kim");
  w.run(5.0);
  expect(c.peer.s->exitKind() == ExitKind::Removed && c.peer.s->myRemoval().reason == "Thanks for the help",
         "the removed person's open session hears it at once, even through a relay");
  auto open = [](Hub::LinkView::State st) { return st == Hub::LinkView::State::Direct || st == Hub::LinkView::State::Relayed; };
  expect(b.peer.s->memberState(kim) == MemberState::Removed && !open(b.hub->link(kim).state) && !open(a.hub->link(kim).state) &&
             a.hub->links().size() == 1,
         "nobody relays for them or keeps a link to them");
  a.peer.doc.renameChannel(a.peer.doc.song().channels[0].id, "After Kim");
  w.run(2.0);
  expect(b.peer.doc.song().channels[0].name == "After Kim" && c.peer.doc.song().channels[0].name != "After Kim", "the others continue; the removed copy stays as it was");

  // offline: Sam's computer is off while he is removed; he learns it when he comes back
  b.hub->setSession(nullptr);
  const std::string samFolder = b.peer.folder;
  b.peer.s.reset();
  w.run(1.0);
  expect(a.peer.s->removeMember(sam, "", &err), "the admin removes someone who is offline");
  w.run(1.0);
  b.peer.s = Session::open(b.peer.doc, sim::config(samFolder, b.peer.id, "Sam", &w.clock), &err);
  b.hub->setSession(b.peer.s.get());
  for (int i = 0; i < 1200 && b.peer.s->exitKind() == ExitKind::None; ++i) w.run(0.05);
  expect(b.peer.s->exitKind() == ExitKind::Removed && b.peer.s->myRemoval().byName == "Alex",
         "reopening, a removed member learns it from the handshake refusal (reason \"removed\")");
  // an old-style invite can't bring them back
  b.hub->setSession(nullptr);
  b.peer.s.reset();
  b.peer.doc.reset(makeEmptySong());
  const std::string code = readyCode(w, a, a.hub->createInvite(60));
  joinWith(w, b, code, "rm_b2");
  for (int i = 0; i < 400 && b.hub->joinView().status != Hub::JoinStatus::Refused; ++i) w.run(0.05);
  expect(b.hub->joinView().status == Hub::JoinStatus::Refused && b.hub->joinView().message.find("removed") != std::string::npos &&
             a.peer.s->memberState(sam) == MemberState::Removed,
         "a removed member can't rejoin with an ordinary invite");
  // the admin's re-invite: an explicit re-add
  b.hub->setSession(nullptr);
  b.peer.s.reset();
  InviteInfo info;
  parseInviteCode(readyCode(w, a, a.hub->createInvite(60, sam)), info);
  Session::Config cfg = sim::config(samFolder, b.peer.id, "Sam", &w.clock);
  cfg.expectReadmit = true;
  b.peer.s = Session::open(b.peer.doc, cfg, &err);
  b.hub->setSession(b.peer.s.get());
  b.hub->join(info, &err);
  for (int i = 0; i < 600 && !(b.peer.s->exitKind() == ExitKind::None && a.peer.s->isActive(sam) && sim::songText(a.peer) == sim::songText(b.peer)); ++i)
    w.run(0.05);
  expect(a.peer.s->isActive(sam) && b.peer.s->exitKind() == ExitKind::None && sim::songText(a.peer) == sim::songText(b.peer),
         "an admin's re-invite brings them back (removed state cleared, synced)");
  // stop sharing: nobody can start it again
  a.peer.s->stopSharing();
  w.run(3.0);
  expect(b.peer.s->exitKind() == ExitKind::Ended, "stop sharing reaches the open sessions");
  expect(a.hub->createInvite(60).empty(), "no invite can be made after stopping sharing");
}

void testLifecycle() {
  // the whole file lifecycle through the controllers, as the app does it
  auto net = MemFabric::makeNet();
  const std::string root = sim::tmpDir("life");
  Controller ca(root + "/cfgA"), cb(root + "/cfgB");
  ca.fabricFactory = [net] { return std::make_unique<MemFabric>(net, "Alex"); };
  cb.fabricFactory = [net] { return std::make_unique<MemFabric>(net, "Sam"); };
  Profile pa = ca.profiles().profile(), pb = cb.profiles().profile();
  pa.name = "Alex";
  pb.name = "Sam";
  ca.setProfile(pa);
  cb.setProfile(pb);
  Document da, db;
  da.reset(makeEmptySong());
  da.addChannel("Kick", "gl.kick");
  std::string err;
  expect(ca.create(da, root + "/Alex/Song", "Song", &err), "create");
  double t = 0;
  auto frames = [&](double sec, const std::function<bool()>& until = nullptr) {
    for (double end = t + sec; t < end; t += 0.01) {
      ca.frame(t);
      cb.frame(t);
      if (until && until()) return true;
    }
    return false;
  };
  auto joinB = [&](const std::string& folder) {
    const std::string id = ca.hub()->createInvite(60);
    frames(30, [&] { const auto* v = ca.hub()->invite(id); return v && v->status == Hub::InviteStatus::Ready; });
    InviteInfo info;
    parseInviteCode(ca.hub()->invite(id)->code, info);
    if (!cb.join(db, info, folder, &err)) return false;
    return frames(60, [&] { return cb.session() && cb.session()->ready(); });
  };
  joinB(root + "/Sam/Song");
  frames(5);
  expect(cb.session() && cb.session()->ready(), "Sam joined");
  const std::string projectId = ca.session()->projectId();
  // 6. a copied folder: never two copies syncing as the same member
  cb.close();
  std::filesystem::copy(root + "/Sam/Song", root + "/Sam/Song copy", std::filesystem::copy_options::recursive);
  expect(!cb.open(db, root + "/Sam/Song copy", &err) && !cb.duplicateOf().empty(), "a copy of the folder is detected and does not sync as the same member");
  // moved / renamed outside Gridline: the list shows it missing, the new place opens
  std::filesystem::remove_all(root + "/Sam/Song copy");
  std::filesystem::rename(root + "/Sam/Song", root + "/Sam/Moved");
  bool missing = false;
  for (const auto& k : cb.knownProjects()) missing = missing || (k.id == projectId && k.state == Controller::KnownProject::State::Missing);
  expect(missing, "a moved folder shows as missing (Locate / Forget)");
  expect(cb.open(db, root + "/Sam/Moved", &err) && cb.knownFolder(projectId).find("Moved") != std::string::npos, "it opens from its new place and the list follows");
  frames(10, [&] { return cb.session()->peer(ca.session()->site()) && cb.session()->peer(ca.session()->site())->connected; });
  // 2a. leave and keep a normal project
  db.addChannel("Sam's last edit", "gl.hat");
  frames(1);
  expect(cb.requestLeave(db, Controller::LeaveMode::Keep, &err), "leave (keep)");
  frames(10, [&] { return !cb.leaving(); });
  auto r = cb.takeLeaveResult();
  Song kept;
  expect(r.done && r.error.empty() && loadProject(r.file, kept) && !std::filesystem::exists(root + "/Sam/Moved/.gridline-collab"),
         "after leaving, the folder is a normal project without the shared history");
  frames(3);
  expect(ca.session()->memberState(cb.profiles().identity().site()) == MemberState::Left, "the others see a signed 'left'");
  bool sawLast = false;
  for (const auto& c : da.song().channels) sawLast = sawLast || c.name == "Sam's last edit";
  expect(sawLast, "the last edit before leaving reached the others (Leave waits for it)");
  expect(!std::filesystem::exists(root + "/cfgB/projects/" + projectId + ".json"), "per-project network data of the others is purged");
  bool listed = false;
  for (const auto& k : cb.knownProjects()) listed = listed || k.id == projectId;
  expect(!listed, "and it is off the collaborative projects list");
  // 2b. join again, then leave and delete (to the trash; the test trash in the scratch folder)
  joinB(root + "/Sam/Again");
  frames(5);
  platform::setEnv("GRIDLINE_TEST_TRASH_DIR", (root + "/trash").c_str());
  platform::setEnv("GRIDLINE_TEST_TRASH_ROOT", root.c_str());
  expect(cb.session() && cb.requestLeave(db, Controller::LeaveMode::Delete, &err), "leave (delete)");
  frames(10, [&] { return !cb.leaving(); });
  r = cb.takeLeaveResult();
  expect(r.deleted && !std::filesystem::exists(root + "/Sam/Again") && std::filesystem::exists(root + "/trash/Again"),
         "leave and delete moves the copy to the trash");
  // 4. removed while open: keep a private copy
  joinB(root + "/Sam/Third");
  frames(5);
  expect(ca.session()->removeMember(cb.profiles().identity().site(), "", &err), "remove Sam");
  frames(5, [&] { return cb.exited() != nullptr; });
  expect(cb.exited() && cb.exited()->kind == ExitKind::Removed && !cb.session() && cb.exited()->byName == "Alex", "Sam's app shows the removal");
  std::string file;
  expect(cb.keepPrivateCopy(db, root + "/Sam/Private", &file, &err) && !Session::isCollabFolder(root + "/Sam/Private") && loadProject(file, kept),
         "keep a private copy: a normal project that opens without collaboration");
  bool removedRow = false;
  for (const auto& k : cb.knownProjects()) removedRow = removedRow || (k.id == projectId && k.state == Controller::KnownProject::State::Removed);
  expect(removedRow, "the recent list shows the project as removed");
  cb.dismissExit();
  cb.forgetProject(root + "/Sam/Third");
  expect(!std::filesystem::exists(root + "/Sam/Third/.gridline-collab") && std::filesystem::exists(root + "/Sam/Third/Song.gridline.json"),
         "forget: the folder keeps the music as a normal project, the shared history goes");
  // 3. stop sharing: the admin's copy becomes a normal project
  expect(ca.requestStopSharing(da, &err), "stop sharing");
  frames(10, [&] { return !ca.leaving(); });
  r = ca.takeLeaveResult();
  expect(r.error.empty() && !std::filesystem::exists(root + "/Alex/Song/.gridline-collab") && loadProject(r.file, kept), "the admin's copy is a normal project");
}

// J-111f: invites carry the name they were made with; the project's own name wins after the first sync;
// the controller's Recent list follows renames; renaming the folder is local and explicit.
void testInviteName() {
  World w;
  Machine a, b;
  w.make(a, "Alex");
  w.make(b, "Sam");
  createProject(w, a, "name_a");
  const std::string code = readyCode(w, a, a.hub->createInvite(60));
  a.peer.s->rename("Renamed after the invite");
  w.run(0.5);
  InviteInfo info;
  expect(parseInviteCode(code, info) && info.projectName == "Song", "an invite shows the name it was made with");
  std::string err;
  int told = 0;
  expect(joinWith(w, b, code, "name_b", &err), "joining with the older invite works");
  b.peer.s->onRenamed = [&](const std::string&, const std::string&, SiteId) { ++told; };
  for (int i = 0; i < 400 && !b.peer.s->ready(); ++i) w.run(0.05);
  w.run(1.0);
  b.peer.s->saveNow();
  expect(b.peer.s->ready() && b.peer.s->projectName() == "Renamed after the invite" && a.peer.s->projectName() == "Renamed after the invite" &&
             sim::samePath(b.peer.s->projectFile(), b.peer.folder + "/Renamed after the invite.gridline.json") && std::filesystem::exists(b.peer.s->projectFile()) &&
             told == 0,
         "after the first sync the joiner has the project's current name (the invite's never overwrites it, no rename notice)");
  InviteInfo fresh;
  expect(parseInviteCode(readyCode(w, a, a.hub->createInvite(60)), fresh) && fresh.projectName == "Renamed after the invite",
         "a new invite carries the new name");
  // a live rename through the encrypted link
  b.peer.s->rename("Live over the link");
  for (int i = 0; i < 100 && a.peer.s->projectName() != "Live over the link"; ++i) w.run(0.05);
  expect(a.peer.s->projectName() == "Live over the link", "a rename travels over the network link");
  // an invite name with control characters is cleaned for display
  InviteInfo odd = info;
  odd.projectName = "Bad\nName\xE2\x80\x8B\x01";
  InviteInfo back;
  expect(parseInviteCode(makeInviteCode(odd), back) && back.projectName == "Bad Name", "an invite's name is cleaned before it is shown");
}

void testControllerRename() {
  Document doc;
  doc.reset(makeEmptySong());
  doc.addChannel("Kick", "gl.kick");
  const std::string cfgDir = sim::tmpDir("rename_cfg");
  Controller c(cfgDir + "/alex");
  c.fabricFactory = [] { return std::make_unique<MemFabric>(); };
  Profile p;
  p.name = "Alex";
  c.setProfile(p);
  const std::string parent = sim::tmpDir("rename_ctl");
  const std::string folder = parent + "/Song";
  expect(c.create(doc, folder, "Song", nullptr), "a collaborative project is made");
  std::string err;
  expect(c.rename("Album opener", &err), "the controller renames the open project");
  c.frame(0.1);
  auto known = c.knownProjects();
  expect(!known.empty() && known.front().name == "Album opener" && sim::samePath(known.front().folder, folder), "Recent collaborative projects shows the new name");
  expect(sim::samePath(c.folderForName(), parent + "/Album opener"), "the folder keeps its name unless asked");
  std::string now;
  expect(c.renameFolder(doc, &now, &err) && sim::samePath(now, parent + "/Album opener") && c.session() && c.session()->config().folder == now &&
             std::filesystem::exists(now + "/Album opener.gridline.json") && !std::filesystem::exists(folder),
         "asked to, the folder takes the name; the project reopens from it");
  known = c.knownProjects();
  expect(known.size() == 1 && known.front().folder == now, "Recent collaborative projects points at the renamed folder (one entry)");
  expect(doc.song().channels.size() == 1 && doc.song().channels[0].name == "Kick", "the song is the same after the folder rename");
  // case-only folder rename
  c.rename("ALBUM OPENER", &err);
  c.frame(0.2);
  expect(c.renameFolder(doc, &now, &err) && std::filesystem::path(now).filename() == "ALBUM OPENER" && c.session(),
         "a case-only folder rename works");
  // another folder already has the name: refused, the project stays open where it is
  std::filesystem::create_directories(parent + "/Taken");
  c.rename("Taken", &err);
  c.frame(0.3);
  err.clear();
  expect(!c.renameFolder(doc, &now, &err) && !err.empty() && c.session() && sim::samePath(c.session()->config().folder, parent + "/ALBUM OPENER"),
         "a folder of that name is never replaced");
  c.close();
}

}  // namespace

int main() {
  testLink();
  testHandshakeRecovery();
  testInviteFlow();
  testReplyAndExpiry();
  testVersionGate();
  testRelayAndMesh();
  testVersionGateRule();
  testRemovalNet();
  testLifecycle();
  testInviteName();
  testControllerRename();
  return finish();
}
