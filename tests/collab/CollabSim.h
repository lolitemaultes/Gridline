// Gridline - J-111 tests: collaboration sessions wired together in memory,
// with random delays, reordering, duplication and partitions (fixed seeds).
#pragma once
#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>
#include "collab/Session.h"
#include "core/Project.h"

namespace sim {

namespace fs = std::filesystem;
using namespace gl;
using namespace gl::collab;

inline std::string tmpDir(const std::string& name) {
  const fs::path p = fs::current_path() / "collab_test_tmp" / name;
  std::error_code ec;
  fs::remove_all(p, ec);
  fs::create_directories(p, ec);
  return p.string();
}

// Paths compared by their elements, not their spelling: on Windows the
// product builds native paths (backslashes) while tests append "/name".
inline bool samePath(const std::string& a, const std::string& b) {
  return fs::path(a).lexically_normal() == fs::path(b).lexically_normal();
}

struct Clock {
  uint64_t ms = 1'800'000'000'000ull;
};

struct Peer {
  std::string label;
  Document doc;
  crypto::Identity id;
  std::unique_ptr<Session> s;
  std::string folder;
};

class Net {
 public:
  explicit Net(uint32_t seed) : rng_(seed) {}
  double minDelay = 0.001, maxDelay = 0.05;
  double dupChance = 0.0;
  double dropUnreliable = 0.0;
  double now = 0;

  void add(Peer* p) { peers_[p->s->site()] = p; wire(p); }
  void wire(Peer* p) {
    const SiteId me = p->s->site();
    p->s->sendFn = [this, me](SiteId to, int stream, const std::string& w) { send(me, to, stream, w); };
  }
  bool linked(SiteId a, SiteId b) const { return links_.count(key(a, b)) > 0; }
  void connect(Peer* a, Peer* b) {
    if (linked(a->s->site(), b->s->site())) return;
    links_.insert(key(a->s->site(), b->s->site()));
    a->s->peerUp(b->s->site());
    b->s->peerUp(a->s->site());
  }
  void disconnect(Peer* a, Peer* b) {
    if (!linked(a->s->site(), b->s->site())) return;
    links_.erase(key(a->s->site(), b->s->site()));
    // in-flight messages of a broken link are lost
    q_.erase(std::remove_if(q_.begin(), q_.end(), [&](const Msg& m) { return key(m.from, m.to) == key(a->s->site(), b->s->site()); }), q_.end());
    a->s->peerDown(b->s->site());
    b->s->peerDown(a->s->site());
  }
  void send(SiteId from, SiteId to, int stream, const std::string& w) {
    if (!linked(from, to)) return;
    if (stream == kStreamUnreliable && std::uniform_real_distribution<double>(0, 1)(rng_) < dropUnreliable) return;
    std::uniform_real_distribution<double> d(minDelay, maxDelay);
    // reliable streams keep their order per link and stream (like the transport); across streams anything goes
    double at = now + d(rng_);
    double& last = lastAt_[{from, to, stream}];
    if (stream != kStreamUnreliable && at < last) at = last;
    last = at;
    q_.push_back({from, to, stream, w, at});
    if (std::uniform_real_distribution<double>(0, 1)(rng_) < dupChance) q_.push_back({from, to, stream, w, at + d(rng_)});
  }
  // Delivers everything due up to `now`.
  size_t deliver() {
    size_t n = 0;
    for (;;) {
      auto it = std::min_element(q_.begin(), q_.end(), [](const Msg& a, const Msg& b) { return a.at < b.at; });
      if (it == q_.end() || it->at > now) break;
      Msg m = std::move(*it);
      q_.erase(it);
      auto p = peers_.find(m.to);
      if (p != peers_.end() && p->second->s) p->second->s->receive(m.from, m.stream, m.wire);
      ++n;
    }
    return n;
  }
  bool idle() const { return q_.empty(); }
  std::mt19937& rng() { return rng_; }

 private:
  struct Msg {
    SiteId from, to;
    int stream;
    std::string wire;
    double at;
  };
  static std::pair<SiteId, SiteId> key(SiteId a, SiteId b) { return a < b ? std::make_pair(a, b) : std::make_pair(b, a); }
  std::mt19937 rng_;
  std::vector<Msg> q_;
  std::set<std::pair<SiteId, SiteId>> links_;
  std::map<SiteId, Peer*> peers_;
  std::map<std::tuple<SiteId, SiteId, int>, double> lastAt_;
};

inline Session::Config config(const std::string& folder, const crypto::Identity& id, const std::string& name, Clock* clock) {
  Session::Config c;
  c.folder = folder;
  c.name = "Song";
  c.identity = id;
  c.profile.name = name;
  c.appVersion = "0.7.0";
  c.durable = false;
  if (clock) c.wallMs = [clock] { return clock->ms; };
  return c;
}

inline Session::Ticket ticketOf(const Session& host) {
  Session::Ticket t;
  t.projectId = host.projectId();
  t.projectName = host.projectName();
  t.creator = host.creator();
  t.creatorSign = host.config().identity.signPublic;
  return t;
}

inline Member memberOf(const Peer& p) {
  Member m;
  m.site = p.id.site();
  m.signPublic = p.id.signPublic;
  m.dhPublic = p.id.dhPublic;
  m.name = p.label;
  return m;
}

// Runs frames (and deliveries) for `seconds` of simulated time.
inline void run(Net& net, std::vector<Peer*> peers, Clock& clock, double seconds, double step = 0.01) {
  const double end = net.now + seconds;
  while (net.now < end) {
    net.now += step;
    clock.ms += static_cast<uint64_t>(step * 1000);
    net.deliver();
    for (Peer* p : peers)
      if (p->s) p->s->frame(net.now);
  }
}

inline void settle(Net& net, std::vector<Peer*> peers, Clock& clock, double maxSeconds = 30) {
  for (double t = 0; t < maxSeconds; t += 0.5) {
    run(net, peers, clock, 0.5);
    if (net.idle()) {
      run(net, peers, clock, 0.5);
      if (net.idle()) return;
    }
  }
}

inline std::string songText(const Peer& p) {
  Song s = p.doc.song();
  s.nextId = 0;  // local allocation counters may differ
  return jsonStringify(songToJson(s), 0);
}

}  // namespace sim
