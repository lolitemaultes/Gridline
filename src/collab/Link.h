// Gridline - J-111 collaboration: datagram endpoints between two computers.
// JuiceFabric (JuiceLink.cpp): libjuice ICE agents (host, server-reflexive,
// peer-reflexive and TURN relayed candidates, IPv4 and IPv6) plus router port
// mapping (libplum: UPnP-IGD, NAT-PMP, PCP). MemFabric: in-process endpoints
// for the tests (optionally "behind NAT": both sides must learn each other).
// Callbacks may come from a network thread; the Hub queues them.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace gl::collab {

struct EndpointConfig {
  std::string ufrag, pwd;              // our ICE credentials ("" = random)
  std::string remoteUfrag, remotePwd;  // known in advance (derived from a shared secret)
  uint16_t localPort = 0;              // 0 = any
  bool controlling = false;
  std::string tag;                     // names the link in GRIDLINE_COLLAB_DEBUG lines
};

struct NetSettings {
  bool enabled = true;
  std::string stunHost = "stun.l.google.com";
  uint16_t stunPort = 19302;
  // K-312: backup address servers, asked in order when the one before gave no public address
  // (an attempt that found none moves on to the next) and as the second server of the router
  // type check. Public servers whose operators allow general use (Settings > Collaboration
  // edits the list; kDefaultStunBackups ships).
  struct Stun {
    std::string host;
    uint16_t port = 3478;
  };
  std::vector<Stun> stunBackups = {{"stun.cloudflare.com", 3478}, {"stun1.l.google.com", 19302}};
  // K-312: "Use this UDP port" (0 = any): the first connection attempt listens on it, and
  // `portForwarded` says the router forwards it here, so the codes offer it at the public address.
  uint16_t fixedPort = 0;
  bool portForwarded = false;
  std::string turnHost;   // "" = no TURN relay
  uint16_t turnPort = 3478;
  std::string turnUser, turnPass;
  bool portMapping = true;  // ask the router (UPnP-IGD / NAT-PMP / PCP) for a temporary mapping
  bool loopback = false;    // also offer 127.0.0.1 / ::1 candidates (tests on one computer)
};

// K-312: stunHost then the backups, without repeats.
std::vector<NetSettings::Stun> stunServers(const NetSettings& s);
// K-312: "host:port, host:port" (a missing port is 3478) to a list and back (Settings).
std::vector<NetSettings::Stun> parseStunList(const std::string& text);
std::string formatStunList(const std::vector<NetSettings::Stun>& list);
inline constexpr const char* kDefaultStunBackups = "stun.cloudflare.com:3478, stun1.l.google.com:19302";

class Endpoint {
 public:
  enum class State { New, Gathering, Connecting, Connected, Failed };
  virtual ~Endpoint() = default;
  // Starts gathering candidates; onGathered when done (with or without STUN).
  virtual void gather() = 0;
  virtual bool gathered() const = 0;
  // Our description (credentials + candidates) for the other side.
  virtual std::string localDescription() const = 0;
  // The other side's description (candidates may be empty: they are learnt
  // from its connectivity checks when we are reachable).
  virtual void setRemote(const std::string& description) = 0;
  virtual void send(const std::string& datagram) = 0;
  virtual State state() const = 0;
  // "host", "srflx", "prflx", "relay", "mapped" (router port mapping), "" when not connected.
  virtual std::string path() const = 0;
  // The local UDP port (0 when not applicable); reused on reconnect.
  virtual uint16_t localPort() const { return 0; }
  // K-174: a fresh ICE attempt on the same UDP port with the same credentials and the remote
  // descriptions given so far (the codes already sent stay valid). An ICE agent sends its
  // checks for about 40 s and then gives up (Failed); a router forgets an outgoing hole about
  // 30 s after the last packet. A join that waits for a reply code restarts its agent so both
  // computers are still sending when the code arrives. false: not supported.
  virtual bool restart() { return false; }
  // K-312: housekeeping from the hub's network step (the next address server when one did
  // not answer). True when the description changed (codes are renewed).
  virtual bool tick() { return false; }
  // Datagrams in, from any thread.
  std::function<void(const std::string&)> onReceive;
};

class Fabric {
 public:
  virtual ~Fabric() = default;
  virtual std::unique_ptr<Endpoint> create(const EndpointConfig& cfg) = 0;
  virtual bool real() const { return false; }  // a real network (ICE)
  // K-312: this computer's router behaviour, from two address servers asked from one socket:
  // 'c' cone (one public port for every destination: hole punching can work), 's' symmetric
  // (a new public port per destination), 'u' unknown; 0 while the probe still runs.
  // probeNat starts one (again when the last result is 10 minutes old); it never blocks.
  virtual void probeNat() {}
  virtual char natType() const { return 'u'; }
};

// The process-wide router port mapper (libplum), reference counted.
void portMapperAcquire();
void portMapperRelease();
// K-312: collect the port mapper's own report lines (which protocol answered, and why one
// failed) between capture(true) and capture(false); portMapperLog returns and clears them.
void portMapperCapture(bool on);
std::vector<std::string> portMapperLog();
// K-312: one TURN allocation with the settings' server (Test my connection). True when the
// server gave a relay address; `why` says what failed in plain words.
bool tryTurnAllocation(const NetSettings& s, int timeoutMs, std::string* why);

// libjuice (nullptr when collaboration networking is not built in).
std::unique_ptr<Fabric> makeJuiceFabric(const NetSettings& s);
bool networkBuiltIn();

// In-process endpoints (tests).
class MemFabric : public Fabric {
 public:
  // natBoth: like two routers that only accept packets from addresses they sent
  // to: a connection needs both descriptions (invite + reply code).
  bool natBoth = false;
  // A datagram is lost with this probability (seeded).
  double loss = 0.0;
  uint32_t seed = 1;
  char nat = 'u';  // K-312: what natType() reports (tests)
  char natType() const override { return nat; }
  std::unique_ptr<Endpoint> create(const EndpointConfig& cfg) override;
  // Several fabrics (one per simulated computer) share one "internet";
  // `label` names this computer for block().
  struct Shared;
  std::shared_ptr<Shared> shared();
  MemFabric() = default;
  MemFabric(std::shared_ptr<Shared> net, std::string label) : shared_(std::move(net)), label_(std::move(label)) {}
  static std::shared_ptr<Shared> makeNet();
  // No direct path between the two computers (both behind strict NATs).
  static void block(const std::shared_ptr<Shared>& net, const std::string& a, const std::string& b);
  // K-174: ICE timing like a real agent: an endpoint sends checks for `sec` after it starts,
  // restarts or learns a remote description, then fails; behind NAT both must be sending at
  // the same time to connect. 0 (default): endpoints never give up. `clock` is the test's time.
  static void setIceBudget(const std::shared_ptr<Shared>& net, double sec, std::function<double()> clock);
  static void setNatBoth(const std::shared_ptr<Shared>& net, bool on);
  // K-174 test hook: every connected path of `label`'s endpoints fails (like an agent that
  // connected moments before its time ran out); they report Failed until they restart.
  static void breakPaths(const std::shared_ptr<Shared>& net, const std::string& label);

 private:
  std::shared_ptr<Shared> shared_;
  std::string label_;
};

}  // namespace gl::collab
