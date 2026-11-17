// Gridline - K-174: joining through home routers, with the real ICE agents (libjuice), on one
// computer and without any special rights. Each collaborator sits behind a simulated router
// made of plain UDP sockets on 127.0.0.1:
//  * the agent only knows addresses of "inside" sockets of its own router; a packet the agent
//    sends to one leaves through the router's outside socket towards the real destination;
//  * the outside socket lets a packet in only from an address it sent to within the filter
//    time (a port-restricted cone router, the most common home router), or never for an
//    address it did not send to from that very socket (symmetric: one outside socket per
//    destination);
//  * optionally the router forgets an idle mapping and gives the next packet a new outside
//    port (no port preservation, as carrier-grade NAT does).
// A STUN server (libjuice's own) answers on 127.0.0.1, so each agent learns its router's
// outside address. Codes are passed with only those public candidates, as across the internet.
// The scenarios: a reply code that comes back late, an invite that sat in a chat for minutes,
// a router that lets nothing in (symmetric on one side), and a direct join. K-312: a TURN relay
// between two symmetric routers, a second reply code that replaces the first, and the router
// type diagnosis (each side probes its router with two address servers; codes carry the type).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "Check.h"
#if defined(GRIDLINE_COLLAB_NET) && !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#include "collab/CollabSim.h"
#include "collab/Hub.h"
#include "collab/Link.h"
#include "collab/NetTest.h"
#include "juice/juice.h"

using namespace gl;
using namespace gl::collab;

namespace {

double secs() {
  static const auto t0 = std::chrono::steady_clock::now();
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

sockaddr_in loopAddr(uint16_t port) {
  sockaddr_in a;
  std::memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  return a;
}

int udp(uint16_t* port) {
  const int fd = socket(AF_INET, SOCK_DGRAM, 0);
  sockaddr_in a = loopAddr(0);
  bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
  socklen_t len = sizeof a;
  getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
  if (port) *port = ntohs(a.sin_port);
  return fd;
}

struct Router {
  enum class Kind { Open, PortRestricted, Symmetric } kind = Kind::PortRestricted;
  double filterSec = 20;   // an outgoing hole stays open this long after the last packet out
  bool remap = false;      // an idle mapping is forgotten: the next packet gets a new port
  struct Inside {
    int fd;
    uint16_t port;
    uint16_t dest;  // destination port on 127.0.0.1
  };
  struct Outside {
    int fd;
    uint16_t port;
    uint16_t dest;  // symmetric: its one destination
    double lastOut = -1;
    sockaddr_in src{};  // the computer's socket it belongs to (K-312: an ICE agent and a router probe)
  };
  std::vector<Inside> inside;
  std::vector<Outside> outside;
  std::map<std::pair<int, uint16_t>, double> sent;  // (outside fd, remote port) -> last packet out
  int dropped = 0;

  uint16_t insideFor(uint16_t dest) {
    for (auto& i : inside)
      if (i.dest == dest) return i.port;
    Inside i{};
    i.fd = udp(&i.port);
    i.dest = dest;
    inside.push_back(i);
    return i.port;
  }
  static bool same(const sockaddr_in& a, const sockaddr_in& b) { return a.sin_port == b.sin_port && a.sin_addr.s_addr == b.sin_addr.s_addr; }
  Outside& outsideFor(const sockaddr_in& src, uint16_t dest, double now) {
    for (auto& o : outside)
      if (same(o.src, src) && (kind != Kind::Symmetric || o.dest == dest)) {
        if (remap && o.lastOut >= 0 && now - o.lastOut > filterSec) {  // forgotten: a new port
          close(o.fd);
          o.fd = udp(&o.port);
          o.lastOut = -1;
        }
        return o;
      }
    Outside o{};
    o.fd = udp(&o.port);
    o.dest = dest;
    o.src = src;
    outside.push_back(o);
    return outside.back();
  }
};

class Net {
 public:
  explicit Net(int routers) : routers_(static_cast<size_t>(routers)) {
    // the address server, also a TURN relay (K-312), and a second address server: asked from
    // one socket, the two tell a symmetric router from a cone one
    juice_server_credentials_t cred{};
    cred.username = "relayuser";
    cred.password = "relaypass";
    cred.allocations_quota = 16;
    juice_server_config_t c;
    std::memset(&c, 0, sizeof c);
    c.bind_address = "127.0.0.1";
    c.external_address = "127.0.0.1";
    c.port = 0;
    c.credentials = &cred;
    c.credentials_count = 1;
    c.max_allocations = 16;
    c.realm = "gridline-test";
    server_ = juice_server_create(&c);
    stunPort_ = server_ ? juice_server_get_port(server_) : 0;
    juice_server_config_t c2;
    std::memset(&c2, 0, sizeof c2);
    c2.bind_address = "127.0.0.1";
    c2.port = 0;
    server2_ = juice_server_create(&c2);
    stun2Port_ = server2_ ? juice_server_get_port(server2_) : 0;
    deadFd_ = udp(&deadPort_);
    thread_ = std::thread([this] { loop(); });
  }
  ~Net() {
    stop_ = true;
    thread_.join();
    if (server_) juice_server_destroy(server_);
    if (server2_) juice_server_destroy(server2_);
    if (deadFd_ >= 0) close(deadFd_);
    for (auto& r : routers_) {
      for (auto& i : r.inside) close(i.fd);
      for (auto& o : r.outside) close(o.fd);
    }
  }
  bool ok() const { return server_ != nullptr && server2_ != nullptr; }
  Router& router(int i) { return routers_[static_cast<size_t>(i)]; }
  std::mutex mu;
  // The STUN server as computer `i` sees it (through its router).
  uint16_t stunVia(int i) {
    std::lock_guard<std::mutex> lk(mu);
    return router(i).insideFor(stunPort_);
  }
  uint16_t deadVia(int i) {  // K-312: an address server that is down (a socket that never answers)
    std::lock_guard<std::mutex> lk(mu);
    return router(i).insideFor(deadPort_);
  }
  uint16_t stun2Via(int i) {
    std::lock_guard<std::mutex> lk(mu);
    return router(i).insideFor(stun2Port_);
  }
  // Computer `from`'s description as computer `to` receives it over the internet: only its
  // public (server-reflexive) and relayed candidates, addressed through `to`'s router.
  std::string carry(const std::string& desc, int to) {
    std::lock_guard<std::mutex> lk(mu);
    std::string out;
    size_t at = 0;
    while (at < desc.size()) {
      size_t nl = desc.find('\n', at);
      if (nl == std::string::npos) nl = desc.size();
      std::string line = desc.substr(at, nl - at);
      at = nl + 1;
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
      if (line.rfind("a=candidate:", 0) == 0) {
        if (line.find(" typ srflx") == std::string::npos && line.find(" typ relay") == std::string::npos) continue;
        std::vector<std::string> f;
        size_t p = 0;
        while (p <= line.size()) {
          size_t sp = line.find(' ', p);
          if (sp == std::string::npos) sp = line.size();
          f.push_back(line.substr(p, sp - p));
          p = sp + 1;
        }
        if (f.size() < 8) continue;
        f[5] = std::to_string(router(to).insideFor(static_cast<uint16_t>(std::atoi(f[5].c_str()))));
        line.clear();
        for (size_t k = 0; k < f.size(); ++k) line += (k ? " " : "") + f[k];
      }
      if (!line.empty()) out += line + "\r\n";
    }
    return out;
  }

 private:
  void loop() {
    std::vector<pollfd> fds;
    struct Ref {
      int router;
      bool inside;
      size_t index;
    };
    std::vector<Ref> refs;
    char buf[2048];
    while (!stop_) {
      fds.clear();
      refs.clear();
      {
        std::lock_guard<std::mutex> lk(mu);
        for (size_t r = 0; r < routers_.size(); ++r) {
          for (size_t i = 0; i < routers_[r].inside.size(); ++i) {
            fds.push_back({routers_[r].inside[i].fd, POLLIN, 0});
            refs.push_back({static_cast<int>(r), true, i});
          }
          for (size_t i = 0; i < routers_[r].outside.size(); ++i) {
            fds.push_back({routers_[r].outside[i].fd, POLLIN, 0});
            refs.push_back({static_cast<int>(r), false, i});
          }
        }
      }
      if (fds.empty() || poll(fds.data(), static_cast<nfds_t>(fds.size()), 5) <= 0) {
        if (fds.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;
      }
      std::lock_guard<std::mutex> lk(mu);
      const double now = secs();
      for (size_t k = 0; k < fds.size(); ++k) {
        if (!(fds[k].revents & POLLIN)) continue;
        sockaddr_in from;
        socklen_t fl = sizeof from;
        const ssize_t n = recvfrom(fds[k].fd, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl);
        if (n <= 0) continue;
        Router& R = routers_[static_cast<size_t>(refs[k].router)];
        if (refs[k].inside) {  // the agent sends out
          if (refs[k].index >= R.inside.size()) continue;
          const uint16_t dest = R.inside[refs[k].index].dest;
          Router::Outside& o = R.outsideFor(from, dest, now);
          sockaddr_in to = loopAddr(dest);
          sendto(o.fd, buf, static_cast<size_t>(n), 0, reinterpret_cast<sockaddr*>(&to), sizeof to);
          o.lastOut = now;
          R.sent[{o.fd, dest}] = now;
        } else {  // from the internet
          if (refs[k].index >= R.outside.size()) continue;
          const Router::Outside& o = R.outside[refs[k].index];
          if (o.fd != fds[k].fd) continue;  // remapped meanwhile
          const uint16_t src = ntohs(from.sin_port);
          auto it = R.sent.find({o.fd, src});
          const bool allowed = R.kind == Router::Kind::Open || (it != R.sent.end() && now - it->second <= R.filterSec);
          if (!allowed) {
            ++R.dropped;
            continue;
          }
          const uint16_t in = R.insideFor(src);
          const sockaddr_in to = o.src;
          for (auto& i : R.inside)
            if (i.port == in) sendto(i.fd, buf, static_cast<size_t>(n), 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
        }
      }
    }
  }

  std::vector<Router> routers_;
  juice_server_t* server_ = nullptr;
  juice_server_t* server2_ = nullptr;
  uint16_t stunPort_ = 0, stun2Port_ = 0, deadPort_ = 0;
  int deadFd_ = -1;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

struct Computer {
  std::string label;
  sim::Peer peer;
  std::unique_ptr<Hub> hub;
};

sim::Clock gClock;

void make(Computer& c, Net& net, int index, const std::string& label, bool turn = false, bool deadFirstStun = false) {
  c.label = label;
  c.peer.label = label;
  c.peer.id = crypto::Identity::generate();
  NetSettings s;
  s.stunHost = "127.0.0.1";
  s.stunPort = net.stunVia(index);
  // K-312: the second address server tells the router's type (Fabric::natType)
  s.stunBackups = {{"127.0.0.1", net.stun2Via(index)}};
  if (deadFirstStun) {  // K-312: the first address server is down: the backups answer
    s.stunPort = net.deadVia(index);
    s.stunBackups = {{"127.0.0.1", net.stunVia(index)}, {"127.0.0.1", net.stun2Via(index)}};
  }
  if (turn) {
    s.turnHost = "127.0.0.1";
    s.turnPort = net.stunVia(index);
    s.turnUser = "relayuser";
    s.turnPass = "relaypass";
  }
  s.loopback = true;  // the simulated internet lives on 127.0.0.1
  s.portMapping = false;
  Profile p;
  p.name = label;
  VersionInfo v;
  v.app = "0.7.0";
  v.build = "test";
  c.hub = std::make_unique<Hub>(makeJuiceFabric(s), c.peer.id, p, v, sim::tmpDir("nat_cfg_" + label));
}

void frame(std::initializer_list<Computer*> cs, double seconds) {
  const double end = secs() + seconds;
  while (secs() < end) {
    gClock.ms += 20;
    for (Computer* c : cs) {
      c->hub->poll(secs());
      if (c->peer.s) c->peer.s->frame(secs());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

bool until(std::initializer_list<Computer*> cs, double seconds, const std::function<bool()>& done) {
  const double end = secs() + seconds;
  while (secs() < end) {
    if (done()) return true;
    frame(cs, 0.1);
  }
  return done();
}

bool synced(Computer& a, Computer& b) { return b.peer.s && b.peer.s->ready() && sim::songText(a.peer) == sim::songText(b.peer); }

struct Scenario {
  const char* name;
  Router::Kind inviter = Router::Kind::PortRestricted, joiner = Router::Kind::PortRestricted;
  bool remap = false;
  double inviteAge = 0;   // seconds between making the invite and joining
  double replyAfter = 0;  // seconds between the reply code appearing and the inviter using it; < 0: never needed
  bool fresh = true;      // K-174 fresh attempts on
  double joinWait = 15 * 60;
  bool turnInviter = false;  // K-312: the inviter has a TURN relay set
  bool staleReply = false;   // K-312: a reply code with dead addresses is used first, the right one later
  bool deadStun = false;     // K-312: the inviter's first address server is down
};

// K-312: `desc` with every candidate's port replaced by `port` (a reply code from an earlier try)
std::string withPort(const std::string& desc, uint16_t port) {
  std::string out;
  size_t at = 0;
  while (at < desc.size()) {
    size_t nl = desc.find('\n', at);
    if (nl == std::string::npos) nl = desc.size();
    std::string line = desc.substr(at, nl - at);
    at = nl + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.rfind("a=candidate:", 0) == 0) {
      std::vector<std::string> f;
      size_t p = 0;
      while (p <= line.size()) {
        size_t sp = line.find(' ', p);
        if (sp == std::string::npos) sp = line.size();
        f.push_back(line.substr(p, sp - p));
        p = sp + 1;
      }
      if (f.size() >= 8) f[5] = std::to_string(port);
      line.clear();
      for (size_t k = 0; k < f.size(); ++k) line += (k ? " " : "") + f[k];
    }
    if (!line.empty()) out += line + "\r\n";
  }
  return out;
}

// Runs one join; returns whether it connected and synced. `jv` gets the joiner's last view,
// `path` the inviter's path to the joiner.
bool runScenario(const Scenario& sc, Hub::JoinView* jv = nullptr, std::string* path = nullptr) {
  Net net(2);
  if (!net.ok()) return false;
  net.router(0).kind = sc.inviter;
  net.router(1).kind = sc.joiner;
  net.router(0).remap = net.router(1).remap = sc.remap;
  Computer a, b;
  make(a, net, 0, "Alex", sc.turnInviter, sc.deadStun);
  make(b, net, 1, "Sam");
  a.hub->freshAttempts = b.hub->freshAttempts = sc.fresh;
  b.hub->joinWaitSec = sc.joinWait;
  a.peer.doc.reset(makeEmptySong());
  a.peer.doc.addChannel("Kick", "gl.kick");
  a.peer.folder = sim::tmpDir(std::string("nat_a_") + sc.name);
  a.peer.s = Session::create(a.peer.doc, sim::config(a.peer.folder, a.peer.id, "Alex", &gClock));
  a.hub->setSession(a.peer.s.get());
  // K-326: an invite is ready once its candidates are in, or after a few seconds without them; on a
  // loaded machine the address server's answer (through the simulated router) can miss that, and the
  // invite then has no public candidate. As a user would, make a new invite (three at most).
  std::string code;
  InviteInfo info;
  for (int tries = 0; tries < 3; ++tries) {
    const std::string id = a.hub->createInvite(60);
    until({&a}, 30, [&] { return a.hub->invite(id) && a.hub->invite(id)->status == Hub::InviteStatus::Ready; });
    code = a.hub->invite(id) ? a.hub->invite(id)->code : std::string();
    if (!parseInviteCode(code, info)) return false;
    if (info.ice.find(" typ srflx") != std::string::npos) break;
    std::printf("# %s: the invite has no public candidate (try %d)\n", sc.name, tries + 1);
    a.hub->cancelInvite(id);
    code.clear();
  }
  if (code.empty()) return false;
  if (sc.inviteAge > 0) frame({&a}, sc.inviteAge);
  info.ice = net.carry(info.ice, 1);
  b.peer.folder = sim::tmpDir(std::string("nat_b_") + sc.name);
  b.peer.s = Session::join(b.peer.doc, sim::config(b.peer.folder, b.peer.id, "Sam", &gClock), info.ticket());
  b.hub->setSession(b.peer.s.get());
  if (!b.hub->join(info)) return false;
  bool ok = false;
  if (sc.replyAfter < 0) {
    ok = until({&a, &b}, 40, [&] { return synced(a, b); });
  } else {
    until({&a, &b}, 30, [&] { return b.hub->joinView().status == Hub::JoinStatus::NeedReply; });
    frame({&a, &b}, sc.replyAfter);
    ReplyInfo r;
    if (b.hub->joinView().status == Hub::JoinStatus::NeedReply && parseReplyCode(b.hub->joinView().reply, r)) {
      r.ice = net.carry(r.ice, 0);
      uint16_t deadPort = 0;
      const int dead = sc.staleReply ? udp(&deadPort) : -1;  // open, never answers
      if (sc.staleReply) {
        ReplyInfo stale = r;
        stale.ice = withPort(r.ice, deadPort);
        std::string err;
        if (!a.hub->acceptReply(makeReplyCode(stale), &err)) std::printf("# %s: stale reply: %s\n", sc.name, err.c_str());
        frame({&a, &b}, 8);
      }
      if (dead >= 0) close(dead);
      if (std::getenv("GRIDLINE_COLLAB_DEBUG")) std::printf("# reply as carried:\n%s# inviter links:\n%s", r.ice.c_str(), a.hub->describeLinks().c_str());
      std::string err;
      if (!a.hub->acceptReply(makeReplyCode(r), &err)) std::printf("# %s: %s\n", sc.name, err.c_str());
      // The fixed hub keeps trying for 15 minutes; give it a few restart cycles here so a loaded
      // machine doesn't fail the check. If the joiner's reply code is renewed meanwhile, paste the
      // new one, as the join window asks a user to.
      const double wait = sc.fresh ? 150.0 : std::min(70.0, sc.joinWait);
      std::string used = b.hub->joinView().reply;
      ok = until({&a, &b}, wait, [&] {
        if (synced(a, b)) return true;
        const Hub::JoinView v = b.hub->joinView();
        ReplyInfo nr;
        if (sc.fresh && v.status == Hub::JoinStatus::NeedReply && v.reply != used && parseReplyCode(v.reply, nr)) {
          used = v.reply;
          nr.ice = net.carry(nr.ice, 0);
          std::string e2;
          if (!a.hub->acceptReply(makeReplyCode(nr), &e2)) std::printf("# %s: renewed reply: %s\n", sc.name, e2.c_str());
        }
        return false;
      });
    }
  }
  if (path && ok) {
    for (const auto& lv : a.hub->links())
      if (!lv.path.empty()) *path = lv.path;
  }
  if (jv) {
    if (!ok && sc.joinWait < 600) until({&a, &b}, sc.joinWait + 10, [&] { return b.hub->joinView().status == Hub::JoinStatus::Failed; });
    *jv = b.hub->joinView();
  }
  std::printf("# %s: %s after %.0f s, the routers dropped %d and %d packets, %d join attempts\n", sc.name, ok ? "connected" : "not connected", secs(),
              net.router(0).dropped, net.router(1).dropped, b.hub->joinView().attempts);
  if (!ok && std::getenv("GRIDLINE_COLLAB_DEBUG")) std::printf("# inviter links:\n%s# joiner links:\n%s", a.hub->describeLinks().c_str(), b.hub->describeLinks().c_str());
  return ok;
}

// K-312: an inviter behind a symmetric router, a joiner behind a port-restricted one: each
// side learns its router's type, the invite carries it, and the join says at once that a
// direct connection can't work (instead of trying for 15 minutes).
void runDiagnosis() {
  Net net(2);
  if (!net.ok()) {
    expect(false, "the test network starts");
    return;
  }
  net.router(0).kind = Router::Kind::Symmetric;
  net.router(1).kind = Router::Kind::PortRestricted;
  Computer a, b;
  make(a, net, 0, "Alex");
  make(b, net, 1, "Sam");
  // the probe alone, through each router
  {
    NetSettings s0, s1;
    s0.stunHost = s1.stunHost = "127.0.0.1";
    s0.stunPort = net.stunVia(0);
    s0.stunBackups = {{"127.0.0.1", net.stun2Via(0)}};
    s1.stunPort = net.stunVia(1);
    s1.stunBackups = {{"127.0.0.1", net.stun2Via(1)}};
    expect(probeNatType(s0, 1500) == 's', "the router probe tells a symmetric router");
    expect(probeNatType(s1, 1500) == 'c', "the router probe tells a cone router");
    // a server that is down is skipped: the next two in the list answer
    NetSettings d0 = s0;
    d0.stunPort = net.deadVia(0);
    d0.stunBackups = {{"127.0.0.1", net.stunVia(0)}, {"127.0.0.1", net.stun2Via(0)}};
    expect(probeNatType(d0, 700) == 's', "with the first address server down, the router type comes from the next two");
  }
  a.peer.doc.reset(makeEmptySong());
  a.peer.folder = sim::tmpDir("nat_a_diag");
  a.peer.s = Session::create(a.peer.doc, sim::config(a.peer.folder, a.peer.id, "Alex", &gClock));
  a.hub->setSession(a.peer.s.get());
  const std::string id = a.hub->createInvite(60);
  until({&a}, 30, [&] { return a.hub->invite(id) && a.hub->invite(id)->status == Hub::InviteStatus::Ready; });
  const Hub::InviteView* iv = a.hub->invite(id);
  InviteInfo info;
  expect(iv && parseInviteCode(iv->code, info) && info.nat == 's', "the invite code says the inviter's router is symmetric");
  expect(iv && !iv->natNote.empty() && !iv->natBlocked, "the inviter is warned with the code that most joiners can't reach a symmetric router");
  info.ice = net.carry(info.ice, 1);
  b.peer.folder = sim::tmpDir("nat_b_diag");
  b.peer.s = Session::join(b.peer.doc, sim::config(b.peer.folder, b.peer.id, "Sam", &gClock), info.ticket());
  b.hub->setSession(b.peer.s.get());
  const double t0 = secs();
  expect(b.hub->join(info), "the join starts");
  until({&a, &b}, 40, [&] { return b.hub->joinView().status != Hub::JoinStatus::Connecting; });
  const Hub::JoinView jv = b.hub->joinView();
  std::printf("# symmetric_diagnosis: join status %d after %.1f s\n# %s\n# %s\n", static_cast<int>(jv.status), secs() - t0, jv.message.c_str(),
              jv.help.c_str());
  expect(jv.status == Hub::JoinStatus::Failed && secs() - t0 < 20, "the joiner is told within seconds, not after 15 minutes");
  expect(jv.message == joinProblemText(JoinProblem::DirectImpossible, "Alex") && jv.help.find("TURN") != std::string::npos &&
             jv.help.find("private network") != std::string::npos && jv.help.find("VPN") == std::string::npos && jv.help.find("forwarded") != std::string::npos,
         "it says the two routers can't connect directly and what works instead");
  // the inviter learns it from a reply code (a joiner on a cone router)
  ReplyInfo r;
  r.inviteId = iv->id;
  r.ice = "a=candidate:1 1 UDP 1686052607 127.0.0.1 40000 typ srflx raddr 0.0.0.0 rport 0\r\n";
  r.nat = 'c';
  ReplyInfo back;
  expect(parseReplyCode(makeReplyCode(r), back) && back.nat == 'c', "a reply code carries the joiner's router type");
  expect(a.hub->acceptReply(makeReplyCode(r)) && a.hub->invite(id)->natBlocked, "the inviter sees at once that this joiner can't connect directly");
}

// K-312: the diagnosis rules without a network
void unitChecks() {
  const std::string srflx = "a=candidate:1 1 UDP 1686052607 198.51.100.7 40000 typ srflx raddr 0.0.0.0 rport 0\r\n";
  const std::string mapped = "a=candidate:9 1 UDP 1694498815 198.51.100.7 40001 typ srflx raddr 0.0.0.0 rport 0\r\n";
  const std::string relay = "a=candidate:2 1 UDP 16777215 192.0.2.10 50000 typ relay raddr 0.0.0.0 rport 0\r\n";
  const std::string v6 = "a=candidate:3 1 UDP 2122260223 2001:db8::5 40002 typ host\r\n";
  // the other home's router
  const std::string far = "a=candidate:1 1 UDP 1686052607 203.0.113.9 40000 typ srflx raddr 0.0.0.0 rport 0\r\n";
  expect(directImpossible('s', srflx, 'c', far) && directImpossible('c', srflx, 's', far) && directImpossible('s', srflx, 's', far),
         "a symmetric router against a known router, nothing open: direct can't work");
  expect(!directImpossible('c', srflx, 'c', far), "two cone routers: hole punching can work");
  expect(!directImpossible('s', srflx, 'u', far) && !directImpossible('u', srflx, 's', far), "an unknown router (an older Gridline) is never ruled out");
  expect(!directImpossible('s', srflx, 'c', far + mapped) && !directImpossible('s', srflx + mapped, 'c', far), "a router port mapping on either side lets it in");
  expect(!directImpossible('s', srflx + relay, 'c', far) && !directImpossible('s', srflx, 'c', far + relay), "a TURN relay on either side carries it");
  expect(!directImpossible('s', srflx + v6, 'c', far + v6) && directImpossible('s', srflx + v6, 'c', far), "IPv6 on both sides avoids the routers");
  // CI (a FreeBSD VM): both apps behind one symmetric NAT, each mapped to its own port
  const std::string samePort2 = "a=candidate:1 1 UDP 1686052607 198.51.100.7 53868 typ srflx raddr 0.0.0.0 rport 0\r\n";
  expect(!directImpossible('s', srflx, 's', samePort2), "two computers behind the same router (one public address) connect locally");
  const std::string loop = "a=candidate:1 1 UDP 1686052607 127.0.0.1 40000 typ srflx raddr 0.0.0.0 rport 0\r\n";
  expect(directImpossible('s', loop, 'c', loop), "a loopback address (a simulated internet) never counts as one router");
  InviteInfo i;
  i.projectId = "p";
  i.creator = 1;
  crypto::Identity id = crypto::Identity::generate();
  i.inviter = id.site();
  i.inviterSign = id.signPublic;
  i.creatorSign = id.signPublic;
  i.inviteId = std::string(16, 'x');
  i.nat = 's';
  InviteInfo back;
  expect(parseInviteCode(makeInviteCode(i), back) && back.nat == 's', "an invite code carries the inviter's router type");
  i.nat = 'u';
  expect(parseInviteCode(makeInviteCode(i), back) && back.nat == 'u', "an unknown router type reads back as unknown (as codes from older versions do)");
  // K-312: the address server list
  const auto l = parseStunList(" stun.example.org:19302, stun:other.example.net ;198.51.100.4:3479\n bad:0, ");
  expect(l.size() == 3 && l[0].host == "stun.example.org" && l[0].port == 19302 && l[1].host == "other.example.net" && l[1].port == 3478 &&
             l[2].port == 3479,
         "the backup list reads host:port entries (3478 by default), drops bad ones");
  expect(parseStunList(formatStunList(l)).size() == 3 && formatStunList(parseStunList(kDefaultStunBackups)) == kDefaultStunBackups, "and writes them back");
  NetSettings ns;
  ns.stunBackups = {{ns.stunHost, ns.stunPort}, {"stun.cloudflare.com", 3478}};
  expect(stunServers(ns).size() == 2 && stunServers(ns)[0].host == ns.stunHost, "the server list starts with the address server, without repeats");
  ns.stunHost.clear();
  expect(stunServers(ns).size() == 2, "no main address server: the backups still answer");
  // half-filled TURN settings
  NetSettings t;
  t.turnUser = "me";
  t.turnPass = "secret";
  expect(turnConfigProblem(t).find("TURN server missing") == 0, "a TURN user and password without a server: TURN server missing");
  t.turnHost = "turn.example.org";
  expect(turnConfigProblem(t).empty(), "a complete TURN setting: no warning");
  t.turnUser.clear();
  t.turnPass.clear();
  expect(!turnConfigProblem(t).empty(), "a TURN server without a user name: a warning");
  expect(turnConfigProblem(NetSettings()).empty(), "no TURN at all: no warning");
  // what the port mapper said, per protocol
  const std::string none = describePortMapping({"Probing PCP...", "Falling back to NAT-PMP", "Probing NAT-PMP...", "Probing gateway with SSDP"}, true);
  expect(none.find("PCP: no answer") != std::string::npos && none.find("NAT-PMP: no answer") != std::string::npos &&
             none.find("UPnP: no router answered") != std::string::npos && none.find("15 seconds") != std::string::npos,
         "no answer on any protocol: each one named");
  const std::string refused = describePortMapping({"Probing PCP...", "Falling back to NAT-PMP", "Got NAT-PMP error response, result=2", "Found UPnP-IGD device: x",
                                                   "Got unknown UPnP error"},
                                                  false);
  expect(refused.find("NAT-PMP: the router refused (2)") != std::string::npos && refused.find("UPnP: the router refused") != std::string::npos,
         "a refusal says which protocol refused");
  // a TURN allocation against a server that is not there fails in plain words
  NetSettings dead;
  dead.turnHost = "127.0.0.1";
  dead.turnPort = 9;
  dead.turnUser = "u";
  dead.turnPass = "p";
  std::string why;
  expect(!tryTurnAllocation(dead, 3000, &why) && !why.empty(), "a TURN server that does not answer: the test says so");
}

}  // namespace

// GRIDLINE_NAT_ONLY=<scenario>: just that one (diagnosis).
bool want(const char* name) {
  const char* only = std::getenv("GRIDLINE_NAT_ONLY");
  return !only || std::strcmp(only, name) == 0;
}

int main() {
  unitChecks();
  if (std::getenv("GRIDLINE_SKIP_NAT_TEST")) {
    std::printf("# skipped (GRIDLINE_SKIP_NAT_TEST)\n");
    return finish();
  }
  {
    Scenario s;
    s.name = "direct";
    s.inviter = Router::Kind::Open;
    s.replyAfter = -1;
    if (want(s.name)) expect(runScenario(s), "an inviter whose router lets connections in: joined without a reply code");
  }
  {
    Scenario s;
    s.name = "late_reply";
    s.replyAfter = 75;
    s.fresh = false;
    s.joinWait = 120;
    if (want(s.name)) expect(!runScenario(s), "before: a reply code used 75 s later did not connect (the joiner's agent had given up)");
    s.name = "late_reply_fixed";
    s.fresh = true;
    s.joinWait = 15 * 60;
    if (want(s.name)) expect(runScenario(s), "both routers strict: the reply code used 75 s later connects");
  }
  {
    Scenario s;
    s.name = "old_invite";
    s.remap = true;
    s.inviteAge = 100;
    s.replyAfter = 5;
    s.fresh = false;
    s.joinWait = 120;
    if (want(s.name)) expect(!runScenario(s), "before: an invite used 100 s later behind a router that forgets idle ports did not connect");
    s.name = "old_invite_fixed";
    s.fresh = true;
    s.joinWait = 15 * 60;
    if (want(s.name)) expect(runScenario(s), "an invite used 100 s later behind a router that forgets idle ports connects");
  }
  {
    Scenario s;
    s.name = "symmetric";
    s.inviter = Router::Kind::Symmetric;
    s.replyAfter = 5;
    s.joinWait = 100;
    Hub::JoinView jv;
    if (want(s.name)) expect(!runScenario(s, &jv), "a symmetric router on one side and a strict one on the other can't connect directly");
    if (want(s.name)) expect(jv.status == Hub::JoinStatus::Failed && jv.message.find("Alex") != std::string::npos && jv.help.find("TURN") != std::string::npos,
           "that join ends with what happened and what to try (a relay)");
  }
  {
    Scenario s;
    s.name = "turn_relay";
    s.inviter = Router::Kind::Symmetric;
    s.joiner = Router::Kind::Symmetric;
    s.turnInviter = true;
    s.replyAfter = 5;
    std::string path;
    if (want(s.name)) {
      expect(runScenario(s, nullptr, &path), "two symmetric routers connect through the inviter's TURN relay");
      expect(path == "relay", "and the link says it is relayed");
    }
  }
  {
    Scenario s;
    s.name = "second_reply";
    s.replyAfter = 5;
    s.staleReply = true;
    if (want(s.name)) expect(runScenario(s), "a second reply code with other addresses replaces the first one and connects");
  }
  if (want("symmetric_diagnosis")) runDiagnosis();
  {
    Scenario s;
    s.name = "stun_down";
    s.inviter = Router::Kind::Open;
    s.replyAfter = -1;
    s.deadStun = true;
    if (want(s.name)) expect(runScenario(s), "the inviter's first address server is down: the next one answers and the join connects");
  }
  return finish();
}

#else
int main() {
  std::printf("# skipped: needs the network build on a POSIX system\n");
  return 0;
}
#endif
