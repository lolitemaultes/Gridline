// Gridline - J-111 collaboration: ICE endpoints (libjuice) and router port
// mapping (libplum). See Link.h. Compiled to stubs without GRIDLINE_COLLAB_NET.
#include "Link.h"
#include "Debug.h"
#if defined(GRIDLINE_COLLAB_NET)
#include <atomic>
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
std::unique_ptr<Fabric> makeJuiceFabric(const NetSettings&) { return nullptr; }

#else

bool networkBuiltIn() { return true; }

namespace {

// ---- router port mapping (one libplum instance for the process) ----
// Locking: libplum calls plumCallback from its own thread while holding its
// internal lock, and plum_cleanup() joins that thread (which reports every
// remaining mapping as destroyed through the callback). So no lock of ours may
// be held while calling into libplum: gMapsMu guards only the id -> state map
// and is never held across a plum_* call. gLifeMu orders init / cleanup; the
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

std::mutex gLifeMu;  // gPlumUsers, gCleanup, plum_init / plum_cleanup
int gPlumUsers = 0;
std::thread* gCleanup = nullptr;  // never destroyed: may still run at exit
std::mutex gMapsMu;  // gMaps, gEarly
std::map<int, std::weak_ptr<MapState>> gMaps;
struct Early {
  plum_state_t state;
  std::string host;
  uint16_t port;
};
std::map<int, Early> gEarly;  // results that arrived before mapPort registered the id

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
  {
    std::lock_guard<std::mutex> lk(gMapsMu);
    auto it = gMaps.find(id);
    if (it == gMaps.end()) {
      if (state == PLUM_STATE_DESTROYED) gEarly.erase(id);
      else gEarly[id] = Early{state, host, port};
      return;
    }
    st = it->second.lock();
    if (state == PLUM_STATE_DESTROYED) gMaps.erase(it);
  }
  if (st) applyResult(*st, state, host, port);
}

void plumAcquire() {
  std::lock_guard<std::mutex> lk(gLifeMu);
  if (gPlumUsers++ == 0) {
    if (gCleanup) {  // the previous instance is still being shut down
      gCleanup->join();
      delete gCleanup;
      gCleanup = nullptr;
    }
    plum_config_t cfg;
    std::memset(&cfg, 0, sizeof cfg);
    cfg.log_level = PLUM_LOG_LEVEL_NONE;
    cfg.discover_timeout = 4000;
    cfg.mapping_timeout = 4000;
    plum_init(&cfg);
  }
}

void plumRelease() {
  std::lock_guard<std::mutex> lk(gLifeMu);
  if (--gPlumUsers == 0) {
    {
      std::lock_guard<std::mutex> mk(gMapsMu);
      gMaps.clear();
      gEarly.clear();
    }
    gCleanup = new std::thread([] { plum_cleanup(); });  // removes the remaining mappings from the router
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
  {
    std::lock_guard<std::mutex> lk(gMapsMu);
    gMaps[id] = st;
    auto it = gEarly.find(id);
    if (it != gEarly.end()) {
      early = true;
      e = it->second;
      gEarly.erase(it);
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

uint16_t randomPort() {
  std::random_device rd;
  return static_cast<uint16_t>(49152 + rd() % (65535 - 49152));
}

class JuiceEndpoint : public Endpoint {
 public:
  JuiceEndpoint(const NetSettings& s, const EndpointConfig& cfg) : settings_(s), cfg_(cfg) {}
  ~JuiceEndpoint() override {
    if (agent_) juice_destroy(agent_);
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
    juice_config_t c;
    std::memset(&c, 0, sizeof c);
    c.concurrency_mode = JUICE_CONCURRENCY_MODE_POLL;
    if (!settings_.stunHost.empty()) {
      c.stun_server_host = settings_.stunHost.c_str();
      c.stun_server_port = settings_.stunPort;
    }
    if (!settings_.turnHost.empty()) {
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
      port_ = cfg_.localPort && attempt == 0 ? cfg_.localPort : randomPort();
      c.local_port_range_begin = port_;
      c.local_port_range_end = port_;
      agent_ = juice_create(&c);
    }
    if (!agent_) return false;
    debugf("%s ice agent on UDP port %u (asked %u)%s", cfg_.tag.c_str(), static_cast<unsigned>(port_), static_cast<unsigned>(cfg_.localPort),
           cfg_.controlling ? " controlling" : "");
    if (!cfg_.ufrag.empty()) juice_set_local_ice_attributes(agent_, cfg_.ufrag.c_str(), cfg_.pwd.c_str());
    if (!cfg_.remoteUfrag.empty()) {
      const std::string d = "a=ice-ufrag:" + cfg_.remoteUfrag + "\r\na=ice-pwd:" + cfg_.remotePwd + "\r\n";
      juice_set_remote_description(agent_, d.c_str());
      remoteSet_ = true;
    }
    if (settings_.portMapping) {
      plumAcquire();
      map_ = mapPort(port_);
    }
    return true;
  }

  void gather() override {
    if (agent_ && !gatherStarted_) {
      gatherStarted_ = true;
      juice_gather_candidates(agent_);
    }
  }
  bool gathered() const override {
    if (!gathered_) return false;
    if (!map_) return true;
    std::lock_guard<std::mutex> lk(map_->mu);
    return map_->done;
  }
  std::string localDescription() const override {
    char buf[JUICE_MAX_SDP_STRING_LEN];
    if (!agent_ || juice_get_local_description(agent_, buf, sizeof buf) < 0) return {};
    std::string out;
    // loopback candidates only for tests on one computer
    const std::string all = buf;
    size_t at = 0;
    while (at < all.size()) {
      size_t nl = all.find('\n', at);
      if (nl == std::string::npos) nl = all.size();
      std::string line = all.substr(at, nl - at + (nl < all.size() ? 1 : 0));
      at = nl + 1;
      const bool loop = line.find(" 127.0.0.1 ") != std::string::npos || line.find(" ::1 ") != std::string::npos;
      if (line.rfind("a=end-of-candidates", 0) == 0) continue;
      if (!loop || settings_.loopback) out += line;
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

 private:
  NetSettings s_;
};

}  // namespace

std::unique_ptr<Fabric> makeJuiceFabric(const NetSettings& s) { return std::make_unique<JuiceFabric>(s); }
void portMapperAcquire() { plumAcquire(); }
void portMapperRelease() { plumRelease(); }

#endif

}  // namespace gl::collab
