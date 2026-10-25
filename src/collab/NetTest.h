// Gridline - J-111 collaboration: "Test my connection". Asks two STUN
// servers for this computer's public address from one UDP socket (same
// public port for both = the router keeps one mapping for every destination,
// hole punching works; different = symmetric NAT, direct connections to
// people behind similar routers fail), tries a router port mapping (UPnP-IGD /
// NAT-PMP / PCP) and looks for a global IPv6 address. Blocking: run it on a
// worker thread. Only the STUN servers and the router are contacted.
#pragma once
#include <string>
#include "Link.h"

namespace gl::collab {

struct NetReport {
  bool stunOk = false;
  std::string publicAddress;   // "203.0.113.7:51234"
  enum class Nat { Unknown, Open, Cone, Symmetric, Blocked } nat = Nat::Unknown;
  bool portMapping = false;
  std::string portMappingProtocol;  // "UPnP-IGD", "NAT-PMP", "PCP"
  std::string mappedAddress;
  bool ipv6 = false;
  std::string ipv6Address;
  std::string summary;              // plain words for the user
};

// Second STUN server used by the test (a different company than the default one).
inline constexpr const char* kSecondStunHost = "stun.cloudflare.com";
inline constexpr unsigned kSecondStunPort = 3478;

NetReport testConnection(const NetSettings& s);
// One STUN binding request over UDP: the public "ip:port" seen by the server ("" on failure).
std::string stunMappedAddress(const std::string& host, unsigned port, int timeoutMs, int socketFd = -1);

}  // namespace gl::collab
