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
// a router that lets nothing in (symmetric on one side), and a direct join.
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
  sockaddr_in agent{};
  bool agentKnown = false;
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
  Outside& outsideFor(uint16_t dest, double now) {
    for (auto& o : outside)
      if (kind != Kind::Symmetric || o.dest == dest) {
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
    outside.push_back(o);
    return outside.back();
  }
};

class Net {
 public:
  explicit Net(int routers) : routers_(static_cast<size_t>(routers)) {
    juice_server_config_t c;
    std::memset(&c, 0, sizeof c);
    c.bind_address = "127.0.0.1";
    c.port = 0;
    server_ = juice_server_create(&c);
    stunPort_ = server_ ? juice_server_get_port(server_) : 0;
    thread_ = std::thread([this] { loop(); });
  }
  ~Net() {
    stop_ = true;
    thread_.join();
    if (server_) juice_server_destroy(server_);
    for (auto& r : routers_) {
      for (auto& i : r.inside) close(i.fd);
      for (auto& o : r.outside) close(o.fd);
    }
  }
  bool ok() const { return server_ != nullptr; }
  Router& router(int i) { return routers_[static_cast<size_t>(i)]; }
  std::mutex mu;
  // The STUN server as computer `i` sees it (through its router).
  uint16_t stunVia(int i) {
    std::lock_guard<std::mutex> lk(mu);
    return router(i).insideFor(stunPort_);
  }
  // Computer `from`'s description as computer `to` receives it over the internet: only its
  // public (server-reflexive) candidates, addressed through `to`'s router.
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
        if (line.find(" typ srflx") == std::string::npos) continue;
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
          R.agent = from;
          R.agentKnown = true;
          const uint16_t dest = R.inside[refs[k].index].dest;
          Router::Outside& o = R.outsideFor(dest, now);
          sockaddr_in to = loopAddr(dest);
          sendto(o.fd, buf, static_cast<size_t>(n), 0, reinterpret_cast<sockaddr*>(&to), sizeof to);
          o.lastOut = now;
          R.sent[{o.fd, dest}] = now;
        } else {  // from the internet
          if (refs[k].index >= R.outside.size() || !R.agentKnown) continue;
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
          for (auto& i : R.inside)
            if (i.port == in) sendto(i.fd, buf, static_cast<size_t>(n), 0, reinterpret_cast<sockaddr*>(&R.agent), sizeof R.agent);
        }
      }
    }
  }

  std::vector<Router> routers_;
  juice_server_t* server_ = nullptr;
  uint16_t stunPort_ = 0;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

struct Computer {
  std::string label;
  sim::Peer peer;
  std::unique_ptr<Hub> hub;
};

sim::Clock gClock;

void make(Computer& c, Net& net, int index, const std::string& label) {
  c.label = label;
  c.peer.label = label;
  c.peer.id = crypto::Identity::generate();
  NetSettings s;
  s.stunHost = "127.0.0.1";
  s.stunPort = net.stunVia(index);
  s.stunFallbackHost.clear();
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
};

// Runs one join; returns whether it connected and synced. `jv` gets the joiner's last view.
bool runScenario(const Scenario& sc, Hub::JoinView* jv = nullptr) {
  Net net(2);
  if (!net.ok()) return false;
  net.router(0).kind = sc.inviter;
  net.router(1).kind = sc.joiner;
  net.router(0).remap = net.router(1).remap = sc.remap;
  Computer a, b;
  make(a, net, 0, "Alex");
  make(b, net, 1, "Sam");
  a.hub->freshAttempts = b.hub->freshAttempts = sc.fresh;
  b.hub->joinWaitSec = sc.joinWait;
  a.peer.doc.reset(makeEmptySong());
  a.peer.doc.addChannel("Kick", "gl.kick");
  a.peer.folder = sim::tmpDir(std::string("nat_a_") + sc.name);
  a.peer.s = Session::create(a.peer.doc, sim::config(a.peer.folder, a.peer.id, "Alex", &gClock));
  a.hub->setSession(a.peer.s.get());
  const std::string id = a.hub->createInvite(60);
  until({&a}, 30, [&] { return a.hub->invite(id) && a.hub->invite(id)->status == Hub::InviteStatus::Ready; });
  const std::string code = a.hub->invite(id) ? a.hub->invite(id)->code : std::string();
  if (sc.inviteAge > 0) frame({&a}, sc.inviteAge);
  InviteInfo info;
  if (!parseInviteCode(code, info)) return false;
  info.ice = net.carry(info.ice, 1);
  if (info.ice.find("typ srflx") == std::string::npos) {
    std::printf("# %s: the invite has no public candidate\n", sc.name);
    return false;
  }
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
  if (jv) {
    if (!ok && sc.joinWait < 600) until({&a, &b}, sc.joinWait + 10, [&] { return b.hub->joinView().status == Hub::JoinStatus::Failed; });
    *jv = b.hub->joinView();
  }
  std::printf("# %s: %s after %.0f s, the routers dropped %d and %d packets, %d join attempts\n", sc.name, ok ? "connected" : "not connected", secs(),
              net.router(0).dropped, net.router(1).dropped, b.hub->joinView().attempts);
  if (!ok && std::getenv("GRIDLINE_COLLAB_DEBUG")) std::printf("# inviter links:\n%s# joiner links:\n%s", a.hub->describeLinks().c_str(), b.hub->describeLinks().c_str());
  return ok;
}

}  // namespace

// GRIDLINE_NAT_ONLY=<scenario>: just that one (diagnosis).
bool want(const char* name) {
  const char* only = std::getenv("GRIDLINE_NAT_ONLY");
  return !only || std::strcmp(only, name) == 0;
}

int main() {
  if (std::getenv("GRIDLINE_SKIP_NAT_TEST")) {
    std::printf("# skipped (GRIDLINE_SKIP_NAT_TEST)\n");
    return 0;
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
  return finish();
}

#else
int main() {
  std::printf("# skipped: needs the network build on a POSIX system\n");
  return 0;
}
#endif
