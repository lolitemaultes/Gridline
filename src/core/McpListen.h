// Gridline - K-264: where the in-app MCP server listens (Settings > Integrations > Listen on).
//
//   This computer only (default)   127.0.0.1; peers on this computer only.
//   This computer, WSL and VMs     every interface; peers on this computer or in a private
//                                  range (10/8, 172.16/12, 192.168/16, 169.254/16 link-local,
//                                  100.64/10 shared): WSL, virtual machines, containers.
//   A specific address             one of this computer's own IPv4 addresses; peers on this
//                                  computer or in a private range.
//   All networks                   every interface; any peer.
//
// The safety model stays: every request needs the bearer token whatever the address; the
// Host header must name the loopback host or one of this computer's own IP literals (never a
// host name, so a web page cannot rebind a name to this computer); browser origins must be
// loopback; repeated wrong tokens from one address are refused for a while (AuthThrottle);
// the activity log shows each client's address. Anything beyond loopback is opt-in, shown in
// red, and asks once for a confirmation. Pure functions (tests/test_k264.cpp).
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace gl::mcp::listen {

enum class Mode { Loopback, LoopbackAndVms, Address, All };

struct Policy {
  Mode mode = Mode::Loopback;
  uint32_t address = 0;  // Mode::Address: host-order IPv4
};

// Settings values ("loopback", "vms", "address", "all") and back; unknown = loopback.
Mode modeFromId(const std::string& id);
const char* modeId(Mode m);

// Dotted IPv4 to host order (false when it is not exactly a.b.c.d).
bool parseIpv4(const std::string& text, uint32_t& out);
std::string ipv4Text(uint32_t host);
bool isLoopback(uint32_t host);
bool isPrivate(uint32_t host);  // the private, link-local and shared ranges above

// The address to bind (host order): 127.0.0.1, 0.0.0.0, or the chosen address.
uint32_t bindAddress(const Policy& p);
// Whether a peer may connect at all.
bool peerAllowed(const Policy& p, uint32_t peer);
// Whether a Host header names this server: loopback names always; beyond loopback also an IP
// literal that is one of `ownAddresses` (this computer's IPv4 addresses, host order).
bool hostAllowed(const Policy& p, const std::string& hostHeader, const std::vector<uint32_t>& ownAddresses);
// This computer's IPv4 addresses (loopback included).
std::vector<uint32_t> localAddresses();
// The host clients use in the connect command: 127.0.0.1, the chosen address, or for the other
// modes the first non-loopback private address of this computer (127.0.0.1 when none).
std::string clientHost(const Policy& p, const std::vector<uint32_t>& ownAddresses);

// Wrong tokens: after kMaxFails within kWindow seconds an address is refused for kBlock seconds.
class AuthThrottle {
 public:
  static constexpr int kMaxFails = 5;
  static constexpr double kWindow = 60, kBlock = 60;
  bool blocked(uint32_t peer, double now) const;
  void failed(uint32_t peer, double now);
  void succeeded(uint32_t peer);

 private:
  struct Entry {
    int fails = 0;
    double first = 0, until = 0;
  };
  std::map<uint32_t, Entry> e_;
};

}  // namespace gl::mcp::listen
