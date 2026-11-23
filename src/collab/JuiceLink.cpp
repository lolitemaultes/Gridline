// Gridline - J-111 collaboration: ICE endpoints (libjuice) and router port
// mapping (libplum). See Link.h. Compiled to stubs without GRIDLINE_COLLAB_NET.
#include "Link.h"
#include "../core/ThreadName.h"
#include "Debug.h"
#include "NetTest.h"
#include "../core/I18n.h"
#if defined(GRIDLINE_COLLAB_NET)
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>
#include "juice/juice.h"
#include "plum/plum.h"
#endif

namespace gl::collab {

#if !defined(GRIDLINE_COLLAB_NET)

bool networkBuiltIn() { return false; }
void portMapperAcquire() {}
void portMapperRelease() {}
void portMapperCapture(bool) {}
std::vector<std::string> portMapperLog() { return {}; }
bool tryTurnAllocation(const NetSettings&, int, std::string* why) {
  if (why) *why = "Networking is not built in.";
  return false;
}
std::unique_ptr<Fabric> makeJuiceFabric(const NetSettings&) { return nullptr; }

#else

bool networkBuiltIn() { return true; }

namespace {

// ---- router port mapping (one libplum instance for the process) ----
// Locking: libplum calls plumCallback from its own thread while holding its
// internal lock, and plum_cleanup() joins that thread (which reports every
// remaining mapping as destroyed through the callback). So no lock of ours may
// be held while calling into libplum: mapsMu guards only the id -> state map
// and is never held across a plum_* call. lifeMu orders init / cleanup; the
// cleanup itself (it asks the router to remove mappings, which can take
// seconds) runs on a background thread so closing a link never blocks the
// interface, and the next init waits for it.
struct MapState {
  std::mutex mu;
  int id = -1;
  bool done = false, ok = false;
  std::string host;
  uint16_t port = 0;
};

struct Early {
  plum_state_t state;
  std::string host;
  uint16_t port;
};

// K-328: everything libplum's threads reach through the callbacks below. The cleanup thread is
// never joined at quit (it may wait seconds on the router) and libplum logs from it, so it can
// still run while the quitting process destroys its statics: plain statics here were destroyed
// under it ("mutex lock failed: Invalid argument", abort, exit code 134 now and then for a peer
// quitting a collaboration). Allocated once and never destroyed, so it outlives every thread.
struct PlumGlue {
  std::mutex lifeMu;  // users, cleanup, plum_init / plum_cleanup
  int users = 0;
  std::thread* cleanup = nullptr;  // never destroyed: may still run at exit
  std::mutex mapsMu;  // maps, early
  std::map<int, std::weak_ptr<MapState>> maps;
  std::map<int, Early> early;  // results that arrived before mapPort registered the id
  // K-312: libplum's own lines while Test my connection runs (which protocol answered, and why
  // one failed). The log level is fixed when libplum starts, so it always reports and we keep
  // only what a capture asks for.
  std::mutex logMu;  // capture, log
  bool capture = false;
  std::vector<std::string> log;
};
PlumGlue& glue() {
  static PlumGlue* const g = new PlumGlue;  // intentionally never deleted (see above)
  return *g;
}

void plumLog(plum_log_level_t, const char* message) {
  if (!message) {
    return;
  }
  PlumGlue& g = glue();
  {
    std::lock_guard<std::mutex> lk(g.logMu);
    if (g.capture && g.log.size() < 400) {
      g.log.emplace_back(message);
    }
  }
  if (debugOn()) {
    debugf("libplum: %s", message);
  }
}

void applyResult(MapState& st, plum_state_t state, const std::string& host, uint16_t port) {
  std::lock_guard<std::mutex> lk(st.mu);
  if (state == PLUM_STATE_SUCCESS) {
    st.ok = true;
    st.host = host;
    st.port = port;
  }
  if (state == PLUM_STATE_SUCCESS || state == PLUM_STATE_FAILURE) st.done = true;
}

void plumCallback(int id, plum_state_t state, const plum_mapping_t* mapping) {
  const std::string host = state == PLUM_STATE_SUCCESS && mapping ? mapping->external_host : "";
  const uint16_t port = state == PLUM_STATE_SUCCESS && mapping ? mapping->external_port : 0;
  std::shared_ptr<MapState> st;
  PlumGlue& g = glue();
  {
    std::lock_guard<std::mutex> lk(g.mapsMu);
    auto it = g.maps.find(id);
    if (it == g.maps.end()) {
      if (state == PLUM_STATE_DESTROYED) {
        g.early.erase(id);
      } else {
        g.early[id] = Early{state, host, port};
      }
      return;
    }
    st = it->second.lock();
    if (state == PLUM_STATE_DESTROYED) {
      g.maps.erase(it);
    }
  }
  if (st) applyResult(*st, state, host, port);
}

void plumAcquire() {
  PlumGlue& g = glue();
  std::lock_guard<std::mutex> lk(g.lifeMu);
  if (g.users++ == 0) {
    if (g.cleanup) {  // the previous instance is still being shut down
      g.cleanup->join();
      delete g.cleanup;
      g.cleanup = nullptr;
    }
    plum_config_t cfg;
    std::memset(&cfg, 0, sizeof cfg);
    cfg.log_level = PLUM_LOG_LEVEL_DEBUG;
    cfg.log_callback = plumLog;
    cfg.discover_timeout = 4000;
    cfg.mapping_timeout = 4000;
    plum_init(&cfg);
  }
}

void plumRelease() {
  PlumGlue& g = glue();
  std::lock_guard<std::mutex> lk(g.lifeMu);
  if (--g.users == 0) {
    {
      std::lock_guard<std::mutex> mk(g.mapsMu);
      g.maps.clear();
      g.early.clear();
    }
    g.cleanup = new std::thread([] { gl::threads::setName("gl-plum-clean"); plum_cleanup(); });  // removes the remaining mappings from the router
  }
}

std::shared_ptr<MapState> mapPort(uint16_t port) {
  auto st = std::make_shared<MapState>();
  plum_mapping_t m;
  std::memset(&m, 0, sizeof m);
  m.protocol = PLUM_IP_PROTOCOL_UDP;
  m.internal_port = port;
  const int id = plum_create_mapping(&m, plumCallback);  // no lock of ours held (see above)
  if (id < 0) {
    st->done = true;
    return st;
  }
  st->id = id;
  bool early = false;
  Early e{};
  PlumGlue& g = glue();
  {
    std::lock_guard<std::mutex> lk(g.mapsMu);
    g.maps[id] = st;
    auto it = g.early.find(id);
    if (it != g.early.end()) {
      early = true;
      e = it->second;
      g.early.erase(it);
    }
  }
  if (early) applyResult(*st, e.state, e.host, e.port);
  return st;
}

std::string candidateType(const char* sdp) {
  const char* t = std::strstr(sdp, " typ ");
  if (!t) return {};
  t += 5;
  std::string r;
  while (*t && *t != ' ' && *t != '\r' && *t != '\n') r += *t++;
  return r;
}

std::atomic<bool> gFixedPortUsed{false};

// the n-th space-separated field of a candidate line
std::string field(const std::string& line, size_t n) {
  size_t p = 0;
  for (size_t i = 0; i < n && p != std::string::npos; ++i) {
    p = line.find(' ', p);
    if (p != std::string::npos) ++p;
  }
  if (p == std::string::npos) return {};
  const size_t e = line.find_first_of(" \r\n", p);
  return line.substr(p, e == std::string::npos ? std::string::npos : e - p);
}
bool isGlobalIpv6Line(const std::string& line) {
  std::string a = field(line, 4);
  if (a.find(':') == std::string::npos) return false;
  for (auto& ch : a) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return !(a.rfind("fe8", 0) == 0 || a.rfind("fe9", 0) == 0 || a.rfind("fea", 0) == 0 || a.rfind("feb", 0) == 0 || a.rfind("fc", 0) == 0 ||
           a.rfind("fd", 0) == 0 || a == "::1");
}  // K-312: the fixed port went to one agent (per process)

uint16_t randomPort() {
  std::random_device rd;
  return static_cast<uint16_t>(49152 + rd() % (65535 - 49152));
}

class JuiceEndpoint : public Endpoint {
 public:
  JuiceEndpoint(const NetSettings& s, const EndpointConfig& cfg) : settings_(s), cfg_(cfg), stuns_(stunServers(s)) {}
  ~JuiceEndpoint() override {
    if (agent_) juice_destroy(agent_);
    if (usedFixed_) gFixedPortUsed = false;
    if (map_) {
      int id = -1;
      {
        std::lock_guard<std::mutex> lk(map_->mu);
        id = map_->id;
      }
      if (id >= 0) plum_destroy_mapping(id);
      plumRelease();
    }
  }

  bool init() {
    // K-312: "Use this UDP port": the first attempt that asks for any port takes it (taken: any)
    uint16_t want = cfg_.localPort;
    if (!want && settings_.fixedPort && !gFixedPortUsed.exchange(true)) want = settings_.fixedPort, usedFixed_ = true;
    if (!createAgent(want)) return false;
    if (usedFixed_ && port_ != settings_.fixedPort) usedFixed_ = false, gFixedPortUsed = false;  // taken by another program
    debugf("%s ice agent on UDP port %u (asked %u)%s", cfg_.tag.c_str(), static_cast<unsigned>(port_), static_cast<unsigned>(cfg_.localPort),
           cfg_.controlling ? " controlling" : "");
    if (settings_.portMapping) {
      plumAcquire();
      map_ = mapPort(port_);
    }
    return true;
  }

  // K-174: a fresh agent on the same port, credentials and remote descriptions (the router
  // port mapping stays). When the last attempt found no public address with the default
  // address server, this one asks the fallback server.
  bool restart() override {
    if (!agent_) return false;
    const bool hadPublic = hasCandidateType("srflx");
    if (!hadPublic && stuns_.size() > 1) stunIndex_ = (stunIndex_ + 1) % stuns_.size();  // K-312: the next address server
    stunTries_ = 0;
    ++restarts_;
    return renew(true, "restarted");
  }

  // A fresh agent on the same port (see restart); `gather`: start gathering at once.
  bool renew(bool gather, const char* why) {
    juice_destroy(agent_);
    agent_ = nullptr;
    state_ = JUICE_STATE_DISCONNECTED;
    gathered_ = false;
    gatherStarted_ = false;
    sendFailures_ = 0;
    const uint16_t before = port_;
    if (!createAgent(port_)) return false;
    debugf("%s ice %s (attempt %d) on UDP port %u%s%s", cfg_.tag.c_str(), why, restarts_ + 1, static_cast<unsigned>(port_),
           port_ != before ? " (the old port was taken)" : "", stunIndex_ ? (", address server " + stuns_[stunIndex_].host).c_str() : "");
    if (!gather) return true;
    // The agent opens its socket when it gathers. On Linux the old agent's socket keeps its port
    // for a moment after it is closed (its network thread may still be waiting on it), so binding
    // the same port can fail at once: try again shortly, as the codes already sent name this port.
    for (int tries = 0; !gatherNow() && tries < 10; ++tries) {
      juice_destroy(agent_);
      agent_ = nullptr;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      state_ = JUICE_STATE_DISCONNECTED;
      if (!createAgent(before)) return false;
    }
    return true;
  }

  // The agent on `wantPort` (a random one when 0 or taken), with our credentials and every
  // remote description given so far.
  bool createAgent(uint16_t wantPort) {
    juice_config_t c;
    std::memset(&c, 0, sizeof c);
    c.concurrency_mode = JUICE_CONCURRENCY_MODE_POLL;
#if defined(__OpenBSD__) || defined(__DragonFly__)
    // These systems have no dual-stack sockets: the IPv6 socket the agent opens by default
    // can never send to an IPv4 address, so it would reach no STUN server and no IPv4 peer
    // (no public address in an invite). IPv4 is what home connections and STUN servers have.
    c.bind_address = "0.0.0.0";
#endif
    if (!stuns_.empty()) {
      const NetSettings::Stun& st = stuns_[stunIndex_ % stuns_.size()];
      c.stun_server_host = st.host.c_str();
      c.stun_server_port = st.port;
    }
    if (settings_.turnHost.empty()) {
      // K-312: a user name or password without a server: say so (the agent works without TURN)
      if ((!settings_.turnUser.empty() || !settings_.turnPass.empty()) && !turnWarned_) {
        turnWarned_ = true;
        debugf("%s TURN server missing: a TURN user or password is set but no server, so no relay is used", cfg_.tag.c_str());
      }
    } else {
      turn_.host = settings_.turnHost.c_str();
      turn_.port = settings_.turnPort;
      turn_.username = settings_.turnUser.c_str();
      turn_.password = settings_.turnPass.c_str();
      c.turn_servers = &turn_;
      c.turn_servers_count = 1;
    }
    c.cb_state_changed = [](juice_agent_t* ag, juice_state_t st, void* u) {
      auto* e = static_cast<JuiceEndpoint*>(u);
      e->state_ = st;
      if (debugOn()) {
        char local[JUICE_MAX_CANDIDATE_SDP_STRING_LEN] = "", remote[JUICE_MAX_CANDIDATE_SDP_STRING_LEN] = "";
        if (st == JUICE_STATE_CONNECTED || st == JUICE_STATE_COMPLETED) juice_get_selected_candidates(ag, local, sizeof local, remote, sizeof remote);
        debugf("%s ice state %s%s%s%s%s", e->cfg_.tag.c_str(), juice_state_to_string(st), *local ? " local " : "", local, *remote ? " remote " : "", remote);
      }
    };
    c.cb_gathering_done = [](juice_agent_t*, void* u) {
      auto* e = static_cast<JuiceEndpoint*>(u);
      e->gathered_ = true;
      debugf("%s ice gathering done", e->cfg_.tag.c_str());
    };
    c.cb_recv = [](juice_agent_t*, const char* data, size_t size, void* u) {
      auto* e = static_cast<JuiceEndpoint*>(u);
      if (e->onReceive) e->onReceive(std::string(data, size));
    };
    c.user_ptr = this;
    for (int attempt = 0; attempt < 8 && !agent_; ++attempt) {
      port_ = wantPort && attempt < 2 ? wantPort : randomPort();
      c.local_port_range_begin = port_;
      c.local_port_range_end = port_;
      agent_ = juice_create(&c);
    }
    if (!agent_) return false;
    if (!cfg_.ufrag.empty()) juice_set_local_ice_attributes(agent_, cfg_.ufrag.c_str(), cfg_.pwd.c_str());
    remoteSet_ = false;
    if (!cfg_.remoteUfrag.empty()) {
      const std::string d = "a=ice-ufrag:" + cfg_.remoteUfrag + "\r\na=ice-pwd:" + cfg_.remotePwd + "\r\n";
      juice_set_remote_description(agent_, d.c_str());
      remoteSet_ = true;
    }
    for (const auto& d : remotes_) applyRemote(d);
    return true;
  }

  bool hasCandidateType(const char* type) const {
    char buf[JUICE_MAX_SDP_STRING_LEN];
    if (!agent_ || juice_get_local_description(agent_, buf, sizeof buf) < 0) return false;
    return std::strstr(buf, (std::string(" typ ") + type).c_str()) != nullptr;
  }

  void applyRemote(const std::string& d) {
    if (!remoteSet_) {
      juice_set_remote_description(agent_, d.c_str());
      remoteSet_ = true;
      return;
    }
    size_t at = 0;
    while (at < d.size()) {
      size_t nl = d.find('\n', at);
      if (nl == std::string::npos) nl = d.size();
      std::string line = d.substr(at, nl - at);
      at = nl + 1;
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
      if (line.rfind("a=candidate:", 0) == 0) juice_add_remote_candidate(agent_, line.c_str());
    }
    juice_set_remote_gathering_done(agent_);
  }

  void gather() override { gatherNow(); }
  // K-312: gathering ended without a public address: the address server gave no answer, so
  // ask the next one in the list (each once per attempt), unless already connected.
  bool tick() override {
    // done gathering, or still waiting on an address server that answers within a second when it answers at all
    const bool slow = gatherStarted_ && std::chrono::steady_clock::now() - gatherAt_ > std::chrono::milliseconds(2500);
    if (!agent_ || !(gathered_ || slow) || stunTries_ + 1 >= stuns_.size() || hasCandidateType("srflx")) return false;
    if (state_ == JUICE_STATE_CONNECTED || state_ == JUICE_STATE_COMPLETED) return false;
    ++stunTries_;
    stunIndex_ = (stunIndex_ + 1) % stuns_.size();
    debugf("%s no answer from the address server: asking %s", cfg_.tag.c_str(), stuns_[stunIndex_].host.c_str());
    return renew(true, "renewed for the next address server");
  }
  // False when the agent could not open its socket (its port taken): it then counts as failed,
  // so the hub starts a fresh attempt instead of waiting on an agent that never gathers.
  bool gatherNow() {
    if (!agent_ || gatherStarted_) return true;
    gatherStarted_ = true;
    gatherAt_ = std::chrono::steady_clock::now();
    if (juice_gather_candidates(agent_) >= 0) return true;
    gatherStarted_ = false;
    state_ = JUICE_STATE_FAILED;
    debugf("%s ice agent could not open UDP port %u", cfg_.tag.c_str(), static_cast<unsigned>(port_));
    return false;
  }
  bool gathered() const override {
    if (!gathered_) return false;
    // K-312: no public address yet and another address server to ask (tick does): not done
    if (stunTries_ + 1 < stuns_.size() && !hasCandidateType("srflx") && state_ != JUICE_STATE_CONNECTED && state_ != JUICE_STATE_COMPLETED)
      return false;
    if (!map_) return true;
    std::lock_guard<std::mutex> lk(map_->mu);
    return map_->done;
  }
  std::string localDescription() const override {
    char buf[JUICE_MAX_SDP_STRING_LEN];
    if (!agent_ || juice_get_local_description(agent_, buf, sizeof buf) < 0) return {};
    std::string out, forwardedIp;
    // loopback candidates only for tests on one computer
    const std::string all = buf;
    size_t at = 0;
    int hosts = 0;
    while (at < all.size()) {
      size_t nl = all.find('\n', at);
      if (nl == std::string::npos) nl = all.size();
      std::string line = all.substr(at, nl - at + (nl < all.size() ? 1 : 0));
      at = nl + 1;
      const bool loop = line.find(" 127.0.0.1 ") != std::string::npos || line.find(" ::1 ") != std::string::npos;
      if (line.rfind("a=end-of-candidates", 0) == 0) continue;
      // K-174: shorter codes (chat apps cut long messages): link-local IPv6 addresses never
      // reach another network, and a computer with many interfaces (VPNs, virtual machines)
      // offers at most 8 of its own addresses
      const bool host = line.find(" typ host") != std::string::npos;
      if (host && (line.find(" fe80:") != std::string::npos || line.find(" FE80:") != std::string::npos)) continue;
      // K-312: a global IPv6 address never counts against the 8 (it needs no router to cross)
      const bool v6 = host && isGlobalIpv6Line(line);
      if (host && !v6 && line.rfind("a=candidate:", 0) == 0 && ++hosts > 8) continue;
      if (settings_.portForwarded && line.find(" typ srflx") != std::string::npos && forwardedIp.empty()) forwardedIp = field(line, 4);
      if (!loop || settings_.loopback) out += line;
    }
    // K-312: a port the user forwarded on the router, at the public address (opens a way in, as
    // a router port mapping does)
    if (settings_.portForwarded && settings_.fixedPort && port_ == settings_.fixedPort && !forwardedIp.empty() &&
        forwardedIp.find(':') == std::string::npos) {
      char c[256];
      std::snprintf(c, sizeof c, "a=candidate:9 1 UDP 1694498815 %s %u typ srflx raddr 0.0.0.0 rport 0\r\n", forwardedIp.c_str(), static_cast<unsigned>(port_));
      out += c;
    }
    if (map_) {
      std::lock_guard<std::mutex> lk(map_->mu);
      if (map_->ok && !map_->host.empty() && map_->port) {
        char c[256];
        std::snprintf(c, sizeof c, "a=candidate:9 1 UDP 1694498815 %s %u typ srflx raddr 0.0.0.0 rport 0\r\n", map_->host.c_str(),
                      static_cast<unsigned>(map_->port));
        out += c;
      }
    }
    out += "a=end-of-candidates\r\n";
    return out;
  }
  void setRemote(const std::string& d) override {
    if (!agent_) return;
    if (debugOn()) {
      int n = 0;
      for (size_t at = d.find("a=candidate:"); at != std::string::npos; at = d.find("a=candidate:", at + 1)) ++n;
      debugf("%s remote description: %d candidates%s", cfg_.tag.c_str(), n, remoteSet_ ? " (added)" : "");
    }
    // K-312: libjuice takes no more candidates once the remote side's gathering is marked done
    // (applyRemote does that), so a second reply code with other addresses was dropped, and a
    // restart replayed the first one first. When a description brings addresses the agent does
    // not have, a fresh agent (same port and credentials) gets only the newest description.
    if (!remotes_.empty()) {
      const auto have = addressSet(remotes_.back()), add = addressSet(d);
      if (std::includes(have.begin(), have.end(), add.begin(), add.end())) return;  // nothing new
      remotes_.assign(1, d);
      renew(gatherStarted_, "renewed for the newest remote description");
      return;
    }
    remotes_.push_back(d);  // given again to a restarted agent
    applyRemote(d);
  }

  // "address port type" of every candidate in a description, sorted
  static std::vector<std::string> addressSet(const std::string& d) {
    std::vector<std::string> out;
    size_t at = 0;
    while ((at = d.find("a=candidate:", at)) != std::string::npos) {
      size_t nl = d.find('\n', at);
      if (nl == std::string::npos) nl = d.size();
      std::vector<std::string> f;
      size_t p = at;
      while (p < nl && f.size() < 8) {
        size_t sp = d.find(' ', p);
        if (sp == std::string::npos || sp > nl) sp = nl;
        std::string w = d.substr(p, sp - p);
        while (!w.empty() && (w.back() == '\r' || w.back() == '\n')) w.pop_back();
        f.push_back(w);
        p = sp + 1;
      }
      if (f.size() >= 8) out.push_back(f[4] + " " + f[5] + " " + f[7]);
      at = nl;
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
  }
  void send(const std::string& d) override {
    if (!agent_) return;
    if (juice_send(agent_, d.data(), d.size()) < 0) {
      ++sendFailures_;
      // first failures one by one, then every 50th
      if (sendFailures_ <= 5 || sendFailures_ % 50 == 0)
        debugf("%s send of %zu bytes failed (ice state %s, %d failures)", cfg_.tag.c_str(), d.size(), juice_state_to_string(state_.load()), sendFailures_);
    }
  }
  State state() const override {
    switch (state_.load()) {
      case JUICE_STATE_GATHERING: return State::Gathering;
      case JUICE_STATE_CONNECTING: return State::Connecting;
      case JUICE_STATE_CONNECTED:
      case JUICE_STATE_COMPLETED: return State::Connected;
      case JUICE_STATE_FAILED: return State::Failed;
      default: return gatherStarted_ ? State::Gathering : State::New;
    }
  }
  std::string path() const override {
    if (!agent_ || state() != State::Connected) return {};
    char local[JUICE_MAX_CANDIDATE_SDP_STRING_LEN], remote[JUICE_MAX_CANDIDATE_SDP_STRING_LEN];
    if (juice_get_selected_candidates(agent_, local, sizeof local, remote, sizeof remote) < 0) return {};
    const std::string l = candidateType(local), r = candidateType(remote);
    if (l == "relay" || r == "relay") return "relay";
    if (std::strstr(remote, "candidate:9 ")) return "mapped";
    if (l == "host" && r == "host") return "host";
    return r.empty() ? l : r;
  }
  uint16_t localPort() const override { return port_; }

 private:
  NetSettings settings_;
  EndpointConfig cfg_;
  juice_turn_server_t turn_{};
  juice_agent_t* agent_ = nullptr;
  std::atomic<juice_state_t> state_{JUICE_STATE_DISCONNECTED};
  std::atomic<bool> gathered_{false};
  bool gatherStarted_ = false, remoteSet_ = false;
  int sendFailures_ = 0;
  uint16_t port_ = 0;
  std::shared_ptr<MapState> map_;
  std::vector<std::string> remotes_;  // the newest remote description, for restarts (K-174, K-312)
  bool turnWarned_ = false;
  std::vector<NetSettings::Stun> stuns_;  // K-312: the address servers, in order
  size_t stunIndex_ = 0, stunTries_ = 0;
  bool usedFixed_ = false;
  std::chrono::steady_clock::time_point gatherAt_{};  // this agent holds the "Use this UDP port" port
  int restarts_ = 0;
};

class JuiceFabric : public Fabric {
 public:
  explicit JuiceFabric(const NetSettings& s) : s_(s) {
    if (debugOn()) {
      juice_set_log_handler([](juice_log_level_t level, const char* message) { debugf("libjuice %d: %s", static_cast<int>(level), message); });
      juice_set_log_level(JUICE_LOG_LEVEL_INFO);
    } else {
      juice_set_log_level(JUICE_LOG_LEVEL_NONE);
    }
  }
  std::unique_ptr<Endpoint> create(const EndpointConfig& cfg) override {
    auto e = std::make_unique<JuiceEndpoint>(s_, cfg);
    if (!e->init()) return nullptr;
    return e;
  }
  bool real() const override { return true; }
  void probeNat() override {
    std::lock_guard<std::mutex> lk(nat_->mu);
    const auto now = std::chrono::steady_clock::now();
    if (nat_->running || (nat_->result && now - nat_->at < std::chrono::minutes(10))) return;
    nat_->running = true;
    auto st = nat_;
    const NetSettings s = s_;
    std::thread([st, s] { gl::threads::setName("gl-nat-probe");
      const char r = probeNatType(s, 1500);
      std::lock_guard<std::mutex> lk2(st->mu);
      st->result = r;
      st->running = false;
      st->at = std::chrono::steady_clock::now();
      debugf("router behaviour: %s", r == 'c' ? "cone" : r == 's' ? "symmetric" : "unknown");
    }).detach();
  }
  char natType() const override {
    std::lock_guard<std::mutex> lk(nat_->mu);
    if (nat_->result) return nat_->result;
    return nat_->running ? 0 : 'u';
  }

 private:
  struct NatProbe {
    std::mutex mu;
    char result = 0;
    bool running = false;
    std::chrono::steady_clock::time_point at;
  };
  NetSettings s_;
  std::shared_ptr<NatProbe> nat_ = std::make_shared<NatProbe>();  // shared with a probe thread that may outlive us
};

}  // namespace

std::unique_ptr<Fabric> makeJuiceFabric(const NetSettings& s) { return std::make_unique<JuiceFabric>(s); }
void portMapperAcquire() { plumAcquire(); }
void portMapperRelease() { plumRelease(); }
void portMapperCapture(bool on) {
  PlumGlue& g = glue();
  std::lock_guard<std::mutex> lk(g.logMu);
  g.capture = on;
  if (on) {
    g.log.clear();
  }
}
std::vector<std::string> portMapperLog() {
  PlumGlue& g = glue();
  std::lock_guard<std::mutex> lk(g.logMu);
  std::vector<std::string> out;
  out.swap(g.log);
  return out;
}

bool tryTurnAllocation(const NetSettings& s, int timeoutMs, std::string* why) {
  if (s.turnHost.empty()) {
    if (why) *why = i18n::tr("No TURN server is set.");
    return false;
  }
  struct Wait {
    std::atomic<bool> done{false};
  } w;
  juice_config_t c;
  std::memset(&c, 0, sizeof c);
  c.concurrency_mode = JUICE_CONCURRENCY_MODE_POLL;
  juice_turn_server_t t{};
  t.host = s.turnHost.c_str();
  t.port = s.turnPort;
  t.username = s.turnUser.c_str();
  t.password = s.turnPass.c_str();
  c.turn_servers = &t;
  c.turn_servers_count = 1;
  c.cb_gathering_done = [](juice_agent_t*, void* u) { static_cast<Wait*>(u)->done = true; };
  c.user_ptr = &w;
  juice_agent_t* ag = juice_create(&c);
  if (!ag) {
    if (why) *why = i18n::tr("Could not open a network socket for the test.");
    return false;
  }
  juice_gather_candidates(ag);
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  bool relay = false;
  while (!relay && std::chrono::steady_clock::now() < end) {
    char buf[JUICE_MAX_SDP_STRING_LEN];
    if (juice_get_local_description(ag, buf, sizeof buf) >= 0 && std::strstr(buf, " typ relay")) relay = true;
    if (w.done) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (!relay) {
    char buf[JUICE_MAX_SDP_STRING_LEN];
    relay = juice_get_local_description(ag, buf, sizeof buf) >= 0 && std::strstr(buf, " typ relay");
  }
  juice_destroy(ag);
  if (!relay && why)
    *why = w.done ? i18n::tr("The TURN server gave no relay address: check its port, the user name and the password.")
                  : i18n::tr("The TURN server did not answer in time: check its address and port, and that this network allows UDP to it.");
  return relay;
}

#endif

}  // namespace gl::collab
