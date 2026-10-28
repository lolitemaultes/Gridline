// Gridline - J-146 tests: presence on the welcome screen. A collaborator's open project
// answers authenticated presence probes from a member whose project is closed (both
// handshake roles); strangers get silence; a removed member learns it; no answer stays
// "no answer" (never "offline"); hidden people are not shown; another version is told
// apart without the update popup; members linked to someone who answered count as
// working too; the card, the list model and a join through the controllers.
#include <algorithm>
#include <ctime>
#include <filesystem>
#include <functional>
#include <memory>
#include "Check.h"
#include "collab/CollabSim.h"
#include "collab/Controller.h"
#include "collab/Hub.h"
#include "collab/Presence.h"
#include "collab/ProjectList.h"
#include "core/Project.h"

namespace {

using namespace gl;
using namespace gl::collab;

struct Machine {
  std::string label;
  sim::Peer peer;
  std::unique_ptr<Hub> hub;
  std::string cfg;  // like <config>/collab: the Hub keeps its state in cfg/projects
  std::string version = "0.7.0";
  void newHub(const std::shared_ptr<MemFabric::Shared>& net, int64_t* wall) {
    Profile p;
    p.name = label;
    VersionInfo v;
    v.app = version;
    v.build = "test";
    hub = std::make_unique<Hub>(std::make_unique<MemFabric>(net, label), peer.id, p, v, cfg + "/projects");
    hub->wallSec = [wall] { return *wall; };
  }
};

struct World {
  sim::Clock clock;
  std::shared_ptr<MemFabric::Shared> net = MemFabric::makeNet();
  double now = 0;
  int64_t wall = 1'800'000'000;
  std::vector<Machine*> ms;
  std::vector<PresenceProber*> probers;
  // `lowerThan`: the identity's site is below (1) / above (-1) that machine's (0: any).
  void make(Machine& m, const std::string& label, const Machine* other = nullptr, int lowerThan = 0) {
    m.label = label;
    m.peer.label = label;
    for (int i = 0; i < 1000; ++i) {
      m.peer.id = crypto::Identity::generate();
      if (!other || lowerThan == 0) break;
      if ((m.peer.id.site() < other->peer.id.site()) == (lowerThan > 0)) break;
    }
    m.cfg = sim::tmpDir("presence_cfg_" + label + std::to_string(ms.size()));
    m.newHub(net, &wall);
    ms.push_back(&m);
  }
  void step() {
    now += 0.01;
    clock.ms += 10;
    if (static_cast<int64_t>(clock.ms / 1000) > wall) wall = static_cast<int64_t>(clock.ms / 1000);
    for (Machine* m : ms) {
      m->hub->poll(now);
      if (m->peer.s) m->peer.s->frame(now);
    }
    for (PresenceProber* p : probers) {
      p->want(now);
      p->poll(now);
    }
  }
  void run(double seconds) {
    const double end = now + seconds;
    while (now < end) step();
  }
  template <class F>
  bool until(double seconds, F f) {
    const double end = now + seconds;
    while (now < end) {
      step();
      if (f()) return true;
    }
    return false;
  }
};

void createProject(World&, Machine& m, const std::string& name, sim::Clock& clock) {
  m.peer.doc.reset(makeEmptySong());
  m.peer.doc.addChannel("Kick", "gl.kick");
  m.peer.folder = sim::tmpDir(name);
  m.peer.s = Session::create(m.peer.doc, sim::config(m.peer.folder, m.peer.id, m.label, &clock));
  m.hub->setSession(m.peer.s.get());
}

std::string readyCode(World& w, Machine& host) {
  const std::string id = host.hub->createInvite(60);
  for (int i = 0; i < 1000; ++i) {
    const auto* v = host.hub->invite(id);
    if (v && v->status == Hub::InviteStatus::Ready) return v->code;
    w.run(0.05);
  }
  return {};
}

bool join(World& w, Machine& m, Machine& host, const std::string& name) {
  InviteInfo info;
  if (!parseInviteCode(readyCode(w, host), info)) return false;
  m.peer.folder = sim::tmpDir(name);
  m.peer.s = Session::join(m.peer.doc, sim::config(m.peer.folder, m.peer.id, m.label, &w.clock), info.ticket());
  m.hub->setSession(m.peer.s.get());
  if (!m.hub->join(info)) return false;
  return w.until(30, [&] { return m.peer.s->ready() && host.hub->link(m.peer.id.site()).state == Hub::LinkView::State::Direct; });
}

// Closes `m`'s project the way the app does (the card is written first). Returns the card.
ProjectCard closeProject(World& w, Machine& m) {
  const ProjectCard c = cardOf(*m.peer.s, w.clock.ms);
  writeCard(m.cfg, c);
  m.hub->setSession(nullptr);
  m.peer.s.reset();
  w.run(0.5);
  return c;
}

ListContext ctxFor(const Machine& m, const World& w) {
  ListContext ctx;
  ctx.me = m.peer.id.site();
  ctx.myName = m.label;
  ctx.myVersion = m.version;
  ctx.nowSec = w.wall;
  return ctx;
}

std::vector<ProbeTarget> targetsOf(const Machine& m, const ProjectCard& card, const World& w) {
  ListProject lp;
  lp.id = card.id;
  lp.name = card.name;
  lp.folder = card.folder;
  std::map<std::string, ProjectCard> cards{{card.id, card}};
  const ListContext ctx = ctxFor(m, w);
  const auto rows = buildProjectList({lp}, cards, {}, {}, ctx);
  return rows.empty() ? std::vector<ProbeTarget>() : probeTargets(m.cfg, rows[0], card, ctx);
}

std::unique_ptr<PresenceProber> proberFor(World& w, Machine& m, const std::string& version = "") {
  Profile p;
  p.name = m.label;
  VersionInfo v;
  v.app = version.empty() ? m.version : version;
  v.build = "test";
  auto pr = std::make_unique<PresenceProber>(std::make_unique<MemFabric>(w.net, m.label), m.peer.id, p, v);
  int64_t* wall = &w.wall;
  pr->wallSec = [wall] { return *wall; };
  return pr;
}

// Alex keeps the project open; Sam (a member) closed it and looks from the welcome screen.
void testProbeBothRoles() {
  for (int proberLower : {1, -1}) {
    World w;
    Machine a, b;
    w.make(a, "Alex");
    w.make(b, "Sam", &a, proberLower);
    createProject(w, a, "pr_a" + std::to_string(proberLower), w.clock);
    expect(join(w, b, a, "pr_b" + std::to_string(proberLower)), "Sam joins Alex's project");
    const int64_t opened = w.wall;
    w.run(2.0);
    const ProjectCard card = closeProject(w, b);
    expect(card.valid() && card.members.size() == 2 && card.admin == a.peer.id.site() && card.me == b.peer.id.site(),
           "the card lists both members, Alex as admin");
    ProjectCard back;
    expect(readCard(b.cfg, card.id, back) && back.members.size() == 2 && back.members[0].dhPublic == card.members[0].dhPublic,
           "the card reads back from disk");
    const auto targets = targetsOf(b, card, w);
    expect(targets.size() == 1 && targets[0].site == a.peer.id.site() && !targets[0].lastDesc.empty(),
           "Sam's probe aims at Alex's last address (kept by the Hub)");
    auto pr = proberFor(w, b);
    pr->setTargets(targets);
    w.probers.push_back(pr.get());
    const int membersBefore = static_cast<int>(a.peer.s->members().size());
    const bool got = w.until(60, [&] {
      const PresenceResult* r = pr->result(card.id, a.peer.id.site());
      return r && r->state == PresenceState::Working;
    });
    const PresenceResult* r = pr->result(card.id, a.peer.id.site());
    const std::string role = proberLower > 0 ? " (the probe starts the handshake)" : " (Alex's waiting link starts it)";
    expect(got && r && r->since >= opened - 5 && r->since <= w.wall && r->profile.name == "Alex",
           ("Sam sees Alex working on it now, since when" + role).c_str());
    expect(static_cast<int>(a.peer.s->members().size()) == membersBefore && !a.peer.s->peer(b.peer.id.site())->connected,
           ("a probe opens nothing: Sam is not online in Alex's session" + role).c_str());
    // and nothing broke: Sam opens the project again and reconnects as usual
    w.probers.clear();
    pr.reset();
    b.peer.s = Session::open(b.peer.doc, sim::config(b.peer.folder, b.peer.id, "Sam", &w.clock));
    b.hub->setSession(b.peer.s.get());
    expect(w.until(60, [&] { return b.hub->link(a.peer.id.site()).state == Hub::LinkView::State::Direct; }),
           ("after probes, opening the project reconnects as before" + role).c_str());
  }
}

void testStrangerAndFailures() {
  World w;
  Machine a, b, c;
  w.make(a, "Alex");
  w.make(b, "Sam");
  w.make(c, "Eve");
  createProject(w, a, "st_a", w.clock);
  join(w, b, a, "st_b");
  w.run(1.0);
  const ProjectCard card = closeProject(w, b);
  auto targets = targetsOf(b, card, w);
  // a stranger who knows the project id, Alex's public key and address
  ProbeTarget fake = targets.at(0);
  auto eve = proberFor(w, c);
  eve->setTargets({fake});
  w.probers.push_back(eve.get());
  w.run(25.0);
  const PresenceResult* er = eve->result(card.id, a.peer.id.site());
  expect(er && er->state == PresenceState::NoAnswer && a.hub->probesAnswered() == 0 && a.peer.s->members().size() == 2,
         "a non-member's probe meets silence: nothing answered, nobody added");
  w.probers.clear();
  // no answer is "no answer", never "offline"; a member never met has no address: unknown
  MemFabric::block(w.net, "Alex", "Sam");
  auto pr = proberFor(w, b);
  ProbeTarget nowhere = targets[0];
  nowhere.site = 12345;
  nowhere.lastDesc.clear();
  nowhere.localPort = 0;
  pr->setTargets({targets[0], nowhere});
  w.probers.push_back(pr.get());
  w.run(25.0);
  const PresenceResult* r = pr->result(card.id, a.peer.id.site());
  expect(r && r->state == PresenceState::NoAnswer, "a blocked network gives \"no answer\" (unknown), not offline");
  const PresenceResult* u = pr->result(card.id, 12345);
  expect(!u || u->state == PresenceState::Unknown, "a member with no known address is not probed (unknown)");
  expect(pr->started() == 1, "only the member with an address was probed");
  w.run(kProbeListenSec - 20.0);  // (a responder keeps listening a while after saying "no answer")
  r = pr->result(card.id, a.peer.id.site());
  expect(r && r->state == PresenceState::NoAnswer && r->failures >= 1, "the silence counts as a failure: the next check comes later");
  // backoff: no answer is asked again later and later
  expect(nextProbeDelay(PresenceState::NoAnswer, 1) == 15 && nextProbeDelay(PresenceState::NoAnswer, 2) == 30 &&
             nextProbeDelay(PresenceState::NoAnswer, 9) == 300 && nextProbeDelay(PresenceState::Working, 0) == 20,
         "probe schedule: 20 s while answered, 15 s doubling to 5 min without an answer");
  // the view hides: probes stop and their sockets close
  w.probers.clear();
  pr->poll(w.now + 10);
  expect(!pr->running() && pr->inFlight() == 0, "probes stop when the view is not shown");
  // presence off here: nothing probed
  auto off = proberFor(w, b);
  off->setEnabled(false);
  off->setTargets(targets);
  w.probers.push_back(off.get());
  w.run(2.0);
  expect(off->started() == 0, "with \"Show that I am online\" off, this computer probes nobody");
  w.probers.clear();
}

void testRemovedHiddenVersion() {
  World w;
  Machine a, b;
  w.make(a, "Alex");
  w.make(b, "Sam");
  createProject(w, a, "rh_a", w.clock);
  join(w, b, a, "rh_b");
  w.run(1.0);
  const ProjectCard card = closeProject(w, b);
  const auto targets = targetsOf(b, card, w);
  // hidden: Alex chose not to show that he is online
  a.hub->setSharePresence(false);
  w.run(5.0);
  auto pr = proberFor(w, b);
  pr->setTargets(targets);
  w.probers.push_back(pr.get());
  w.run(30.0);
  const PresenceResult* r = pr->result(card.id, a.peer.id.site());
  expect(r && r->state != PresenceState::Working && (r->state == PresenceState::Hidden || r->state == PresenceState::NoAnswer) &&
             a.hub->probesAnswered() == 0,
         "someone who hides is never shown working on it (silence, or \"presence\": 0)");
  w.probers.clear();
  a.hub->setSharePresence(true);
  // another version: told apart, and Alex gets no update popup from a probe
  int popups = 0;
  a.hub->onVersionRefusal = [&](const Hub::Refusal&) { ++popups; };
  auto old = proberFor(w, b, "0.6.9");
  old->setTargets(targets);
  w.probers.push_back(old.get());
  const bool got = w.until(60, [&] {
    const PresenceResult* x = old->result(card.id, a.peer.id.site());
    return x && x->state == PresenceState::OtherVersion;
  });
  const PresenceResult* ov = old->result(card.id, a.peer.id.site());
  expect(got && ov && ov->app == "0.7.0" && popups == 0, "a probe from another version learns Alex's version; Alex sees no update popup");
  w.probers.clear();
  pr.reset();  // (one prober per computer: the earlier ones' sockets close, as when the app quits)
  old.reset();
  // removed while away: the probe brings the signed notice back
  std::string err;
  expect(a.peer.s->removeMember(b.peer.id.site(), "", &err), "Alex removes Sam");
  w.run(1.0);
  auto rp = proberFor(w, b);
  rp->setTargets(targets);
  w.probers.push_back(rp.get());
  const bool removed = w.until(150, [&] {
    const PresenceResult* x = rp->result(card.id, a.peer.id.site());
    return x && x->state == PresenceState::RemovedMe;
  });
  const PresenceResult* rm = rp->result(card.id, a.peer.id.site());
  expect(removed && rm && rm->byName == "Alex", "a removed member's probe learns the removal (by Alex), not presence");
  w.probers.clear();
}

void testTransitive() {
  World w;
  Machine a, b, c;
  w.make(a, "Alex");
  w.make(b, "Sam");
  w.make(c, "Kim");
  createProject(w, a, "tr_a", w.clock);
  join(w, b, a, "tr_b");
  join(w, c, a, "tr_c");
  w.run(15.0);
  const ProjectCard card = closeProject(w, b);
  MemFabric::block(w.net, "Sam", "Kim");  // Sam cannot reach Kim directly
  auto pr = proberFor(w, b);
  pr->setTargets(targetsOf(b, card, w));
  w.probers.push_back(pr.get());
  const bool got = w.until(60, [&] {
    const PresenceResult* x = pr->result(card.id, c.peer.id.site());
    return x && x->state == PresenceState::Working;
  });
  const PresenceResult* k = pr->result(card.id, c.peer.id.site());
  expect(got && k && (k->via == a.peer.id.site() || k->via == 0), "Kim (unreachable from Sam) shows as working: Alex's answer says Kim is with him");
  w.probers.clear();
}

// A member who keeps presence private is never shown online through someone else's answer.
void testHiddenNotTransitive() {
  World w;
  Machine a, b, c;
  w.make(a, "Alex");
  w.make(b, "Sam");
  w.make(c, "Kim");
  c.hub->setSharePresence(false);  // Kim: "Show that I am online" off
  createProject(w, a, "hn_a", w.clock);
  join(w, b, a, "hn_b");
  join(w, c, a, "hn_c");
  w.run(15.0);
  const ProjectCard card = closeProject(w, b);
  auto pr = proberFor(w, b);
  pr->setTargets(targetsOf(b, card, w));
  w.probers.push_back(pr.get());
  const bool alex = w.until(60, [&] {
    const PresenceResult* x = pr->result(card.id, a.peer.id.site());
    return x && x->state == PresenceState::Working;
  });
  w.run(30.0);
  const PresenceResult* x = pr->result(card.id, a.peer.id.site());
  const PresenceResult* k = pr->result(card.id, c.peer.id.site());
  const bool kimWith = x && std::find(x->with.begin(), x->with.end(), c.peer.id.site()) != x->with.end();
  expect(alex && !kimWith && (!k || k->state != PresenceState::Working), "Kim keeps presence private: Alex's answer does not list Kim, Kim is not shown working");
  w.probers.clear();
}

void testListModel() {
  // a card by hand: me (Sam), Alex (admin), Kim, Lee (removed), and "Sam" on another computer
  auto key = [](int i) {
    crypto::Identity id = crypto::Identity::generate();
    (void)i;
    return id;
  };
  const crypto::Identity me = key(0), alex = key(1), kim = key(2), lee = key(3), mine2 = key(4);
  ProjectCard c;
  c.id = "abc123";
  c.name = "Night Drive";
  c.folder = "/nowhere/Night Drive";
  c.me = me.site();
  c.admin = alex.site();
  c.pending = 3;
  c.minAppVersion = "0.7.0";
  auto add = [&](const crypto::Identity& id, const std::string& name, CardMember::State st) {
    CardMember m;
    m.site = id.site();
    m.signPublic = id.signPublic;
    m.dhPublic = id.dhPublic;
    m.name = name;
    m.state = st;
    c.members.push_back(m);
  };
  add(me, "Sam", CardMember::State::Active);
  add(alex, "Alex", CardMember::State::Active);
  add(kim, "Kim", CardMember::State::Active);
  add(lee, "Lee", CardMember::State::Removed);
  add(mine2, "Sam", CardMember::State::Active);
  ProjectCard rt;
  expect(cardFromJson(cardToJson(c), rt) && rt.members.size() == 5 && rt.pending == 3, "a card survives JSON");
  ListProject lp;
  lp.id = c.id;
  lp.name = c.name;
  lp.folder = c.folder;
  ListProject gone;
  gone.id = "def456";
  gone.name = "Old";
  gone.folder = "/nowhere/Old";
  gone.state = ListProject::State::Missing;
  std::map<std::string, std::map<SiteId, PresenceResult>> res;
  const int64_t now = 2'000'000'000;
  PresenceResult working;
  working.state = PresenceState::Working;
  working.checkedAt = now - 5;
  working.since = now - 600;
  res[c.id][alex.site()] = working;
  PresenceResult stale = working;
  stale.checkedAt = now - 600;  // ten minutes old
  res[c.id][kim.site()] = stale;
  ListContext ctx;
  ctx.me = me.site();
  ctx.myName = "Sam";
  ctx.myVersion = "0.7.0";
  ctx.nowSec = now;
  auto rows = buildProjectList({lp, gone}, {{c.id, c}}, res, {}, ctx);
  expect(rows.size() == 2 && rows[0].id == c.id && rows[1].state == ListProject::State::Missing && !rows[1].hasCard,
         "every known project is listed, in the known order; a missing folder stays listed");
  const ListRow& r = rows[0];
  expect(!r.admin && r.pending == 3 && r.needVersion.empty() && r.probing, "my role (member), pending changes, probes run");
  expect(r.members.size() == 4 && r.members[0].me, "removed members are not shown; me first");
  expect(r.workingNow.size() == 1 && r.workingNow[0] == "Alex" && r.workingSince == now - 600, "Alex works on it now (fresh answer), since then");
  bool kimNow = true, kimSeen = false, mineElsewhere = false;
  for (const auto& m : r.members) {
    if (m.name == "Kim") kimNow = m.now, kimSeen = m.seenAt == now - 600;
    if (!m.me && m.name == "Sam") mineElsewhere = m.mineElsewhere;
  }
  expect(!kimNow && kimSeen, "a ten-minute-old answer is not \"now\": Kim was seen working on it ten minutes ago");
  expect(mineElsewhere, "another computer with my name is marked as probably mine");
  // the version gate and a newer project
  ctx.gateLatest = "0.7.2";
  rows = buildProjectList({lp}, {{c.id, c}}, res, {}, ctx);
  expect(rows[0].needVersion == "0.7.2" && !rows[0].probing, "the version gate stops probes and says which update");
  ctx.gateLatest.clear();
  c.minAppVersion = "0.9.0";
  rows = buildProjectList({lp}, {{c.id, c}}, res, {}, ctx);
  expect(rows[0].needVersion == "0.9.0" && !rows[0].probing, "a project written by a newer Gridline needs that version");
  c.minAppVersion = "0.7.0";
  // the project open here: the live session decides
  LiveProject live;
  live.id = c.id;
  live.admin = false;
  live.connected[kim.site()] = true;
  ctx.live = &live;
  rows = buildProjectList({lp}, {{c.id, c}}, res, {}, ctx);
  expect(rows[0].openHere && !rows[0].probing && rows[0].workingNow.size() == 1 && rows[0].workingNow[0] == "Kim",
         "open here: no probes; connected members are working on it now");
  ctx.live = nullptr;
  // everyone silent: unreachable (unknown), never offline
  PresenceResult silent;
  silent.state = PresenceState::NoAnswer;
  silent.checkedAt = now;
  res[c.id][alex.site()] = silent;
  res[c.id][kim.site()] = silent;
  res[c.id][mine2.site()] = silent;
  rows = buildProjectList({lp}, {{c.id, c}}, res, {}, ctx);
  expect(rows[0].unreachable && rows[0].workingNow.empty(), "nobody answered: shown as not reachable");
  // removed notice from a probe
  PresenceResult rm;
  rm.state = PresenceState::RemovedMe;
  rm.byName = "Alex";
  res[c.id][alex.site()] = rm;
  rows = buildProjectList({lp}, {{c.id, c}}, res, {}, ctx);
  expect(rows[0].removedNotice && rows[0].byName == "Alex" && !rows[0].probing, "a removal learnt from a probe shows on the tile");
  expect(joinNames({"Sam"}, "and", "%d others") == "Sam" && joinNames({"Sam", "Kim"}, "and", "%d others") == "Sam and Kim" &&
             joinNames({"A", "B", "C", "D", "E"}, "and", "%d others") == "A, B and 3 others",
         "names are joined for the tile");
}

// The whole path through the controllers, as the app does it: join, close (the card is
// written), the welcome screen's list, and presence while the admin works on it.
void testJoinCardAndPresence() {
  auto net = MemFabric::makeNet();
  const std::string root = sim::tmpDir("presence_ctl");
  // The documents outlive the controllers: closing a session on teardown saves the document.
  Document da, db;
  Controller ca(root + "/cfgA"), cb(root + "/cfgB");
  ca.fabricFactory = [net] { return std::make_unique<MemFabric>(net, "Alex"); };
  cb.fabricFactory = [net] { return std::make_unique<MemFabric>(net, "Sam"); };
  Profile pa = ca.profiles().profile(), pb = cb.profiles().profile();
  pa.name = "Alex";
  pb.name = "Sam";
  ca.setProfile(pa);
  cb.setProfile(pb);
  da.reset(makeEmptySong());
  da.addChannel("Kick", "gl.kick");
  std::string err;
  expect(ca.create(da, root + "/Alex/Song", "Song", &err), "Alex makes a collaborative project");
  double t = 0;
  auto frames = [&](double sec, const std::function<bool()>& until = nullptr) {
    for (double end = t + sec; t < end; t += 0.01) {
      ca.frame(t);
      cb.frame(t);
      if (cb.hasPresence()) cb.presence().want(t);
      if (until && until()) return true;
    }
    return false;
  };
  const std::string id = ca.hub()->createInvite(60);
  frames(30, [&] { const auto* v = ca.hub()->invite(id); return v && v->status == Hub::InviteStatus::Ready; });
  InviteInfo info;
  parseInviteCode(ca.hub()->invite(id)->code, info);
  expect(cb.join(db, info, root + "/Sam/Song", &err), "Sam joins with the invite");
  expect(frames(60, [&] { return cb.session() && cb.session()->ready(); }), "the project arrives");
  frames(5);
  const std::string pid = cb.session()->projectId();
  cb.close();
  auto cards = cb.cards();
  expect(cards.count(pid) && cards[pid].members.size() == 2 && cards[pid].admin == ca.session()->site(), "closing writes the card (members, admin)");
  ListContext ctx;
  ctx.me = cb.profiles().identity().site();
  ctx.myName = "Sam";
  ctx.myVersion = cb.version.app;
  ctx.nowSec = static_cast<int64_t>(std::time(nullptr));
  auto rows = buildProjectList(cb.listProjects(), cards, {}, {}, ctx);
  expect(rows.size() == 1 && rows[0].name == "Song" && !rows[0].admin && rows[0].members.size() == 2 && rows[0].probing,
         "the welcome screen lists the joined project with its members");
  cb.presence().setTargets(probeTargets(cb.collabDir(), rows[0], cards[pid], ctx));
  const bool working = frames(60, [&] {
    const PresenceResult* r = cb.presence().result(pid, ca.session()->site());
    return r && r->state == PresenceState::Working;
  });
  expect(working, "Sam's welcome screen sees Alex working on it");
  frames(1);
  const auto seen = cb.seen();
  expect(seen.count(pid) && seen.at(pid).count(ca.session()->site()), "\"last seen working on it\" is kept");
  // leaving purges the card and the cache
  expect(cb.open(db, root + "/Sam/Song", &err), "Sam opens it again");
  frames(10, [&] { return cb.session()->peer(ca.session()->site()) && cb.session()->peer(ca.session()->site())->connected; });
  expect(cb.requestLeave(db, Controller::LeaveMode::Keep, &err), "Sam leaves");
  frames(10, [&] { return !cb.leaving(); });
  cb.takeLeaveResult();
  expect(!std::filesystem::exists(cardPath(cb.collabDir(), pid)) && !std::filesystem::exists(cb.collabDir() + "/projects/" + pid + ".seen.json"),
         "leaving deletes the card and who was seen (no names or keys stay behind)");
}

}  // namespace

int main() {
  testProbeBothRoles();
  testStrangerAndFailures();
  testRemovedHiddenVersion();
  testTransitive();
  testHiddenNotTransitive();
  testListModel();
  testJoinCardAndPresence();
  return finish();
}
