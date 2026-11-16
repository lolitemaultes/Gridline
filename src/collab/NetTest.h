// Gridline - J-111 collaboration: "Test my connection". Asks two STUN
// servers for this computer's public address from one UDP socket (same
// public port for both = the router keeps one mapping for every destination,
// hole punching works; different = symmetric NAT, direct connections to
// people behind similar routers fail), tries a router port mapping (UPnP-IGD /
// NAT-PMP / PCP) and looks for a global IPv6 address. Blocking: run it on a
// worker thread. Only the STUN servers and the router are contacted.
#pragma once
#include <string>
#include <vector>
#include "Link.h"

namespace gl::collab {

struct NetReport {
  bool stunOk = false;
  std::string publicAddress;   // "203.0.113.7:51234"
  std::vector<std::string> stunAnswered, stunSilent;  // K-312: "host:port" of the address servers asked
  enum class Nat { Unknown, Open, Cone, Symmetric, Blocked } nat = Nat::Unknown;
  bool portMapping = false;
  std::string portMappingProtocol;  // "UPnP-IGD", "NAT-PMP", "PCP"
  std::string mappedAddress;
  std::string portMappingDetail;    // K-312: which protocol failed and why (when it did not work)
  // K-312: the TURN relay from the settings
  std::string turnProblem;          // a half-filled TURN setting ("" = none)
  bool turnTried = false, turnOk = false;
  std::string turnDetail;           // why the allocation failed
  bool ipv6 = false;
  std::string ipv6Address;
  std::string summary;              // plain words for the user
};


NetReport testConnection(const NetSettings& s);
// K-312: the router behaviour alone (see Fabric::natType): the first two address servers of the
// list that answer (stunServers), asked from one socket. Blocking.
char probeNatType(const NetSettings& s, int timeoutMs);
// K-312: a TURN setting that can't work as filled in, in plain words ("" = fine or unused).
std::string turnConfigProblem(const NetSettings& s);
// K-312: libplum's report lines from one port mapping try, as what each protocol did.
std::string describePortMapping(const std::vector<std::string>& log, bool timedOut);
// One STUN binding request over UDP: the public "ip:port" seen by the server ("" on failure).
std::string stunMappedAddress(const std::string& host, unsigned port, int timeoutMs, int socketFd = -1);

}  // namespace gl::collab
