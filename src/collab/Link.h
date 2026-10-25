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
  std::string turnHost;   // "" = no TURN relay
  uint16_t turnPort = 3478;
  std::string turnUser, turnPass;
  bool portMapping = true;  // ask the router (UPnP-IGD / NAT-PMP / PCP) for a temporary mapping
  bool loopback = false;    // also offer 127.0.0.1 / ::1 candidates (tests on one computer)
};

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
  // Datagrams in, from any thread.
  std::function<void(const std::string&)> onReceive;
};

class Fabric {
 public:
  virtual ~Fabric() = default;
  virtual std::unique_ptr<Endpoint> create(const EndpointConfig& cfg) = 0;
  virtual bool real() const { return false; }  // a real network (ICE)
};

// The process-wide router port mapper (libplum), reference counted.
void portMapperAcquire();
void portMapperRelease();

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

 private:
  std::shared_ptr<Shared> shared_;
  std::string label_;
};

}  // namespace gl::collab
