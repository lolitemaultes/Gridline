// Gridline - J-111 collaboration: connection test. See NetTest.h.
#include "NetTest.h"
#include <chrono>
#include <cstring>
#include <random>
#include <thread>
#include "../core/I18n.h"
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#if defined(GRIDLINE_COLLAB_NET)
#include "plum/plum.h"
#endif

namespace gl::collab {

namespace {

#if defined(_WIN32)
using Sock = SOCKET;
const Sock kBad = INVALID_SOCKET;
void closeS(Sock s) { closesocket(s); }
struct WsaInit {
  WsaInit() {
    WSADATA d;
    WSAStartup(MAKEWORD(2, 2), &d);
  }
};
#else
using Sock = int;
const Sock kBad = -1;
void closeS(Sock s) { ::close(s); }
#endif

bool resolve(const std::string& host, unsigned port, int family, sockaddr_storage& out, socklen_t& len) {
  addrinfo hints;
  std::memset(&hints, 0, sizeof hints);
  hints.ai_family = family;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* res = nullptr;
  const std::string ps = std::to_string(port);
  if (getaddrinfo(host.c_str(), ps.c_str(), &hints, &res) != 0 || !res) return false;
  std::memcpy(&out, res->ai_addr, res->ai_addrlen);
  len = static_cast<socklen_t>(res->ai_addrlen);
  freeaddrinfo(res);
  return true;
}

std::string stunOn(Sock s, const std::string& host, unsigned port, int timeoutMs) {
  sockaddr_storage to;
  socklen_t tolen = 0;
  if (!resolve(host, port, AF_INET, to, tolen)) return {};
  unsigned char req[20];
  std::memset(req, 0, sizeof req);
  req[0] = 0x00;
  req[1] = 0x01;  // binding request
  req[4] = 0x21;
  req[5] = 0x12;
  req[6] = 0xA4;
  req[7] = 0x42;  // magic cookie
  std::random_device rd;
  for (int i = 8; i < 20; ++i) req[i] = static_cast<unsigned char>(rd());
  const auto start = std::chrono::steady_clock::now();
  for (int attempt = 0; attempt < 3; ++attempt) {
    sendto(s, reinterpret_cast<const char*>(req), sizeof req, 0, reinterpret_cast<sockaddr*>(&to), tolen);
    for (;;) {
      const int waited = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
      const int left = timeoutMs * (attempt + 1) / 3 - waited;
      if (left <= 0) break;
#if defined(_WIN32)
      WSAPOLLFD p{s, POLLIN, 0};
      if (WSAPoll(&p, 1, left) <= 0) break;
#else
      pollfd p{s, POLLIN, 0};
      if (poll(&p, 1, left) <= 0) break;
#endif
      unsigned char buf[512];
      sockaddr_storage from;
      socklen_t flen = sizeof from;
      const int n = static_cast<int>(recvfrom(s, reinterpret_cast<char*>(buf), sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &flen));
      if (n < 20 || buf[0] != 0x01 || buf[1] != 0x01 || std::memcmp(buf + 8, req + 8, 12) != 0) continue;
      size_t at = 20;
      const size_t total = 20 + ((buf[2] << 8) | buf[3]);
      while (at + 4 <= total && at + 4 <= static_cast<size_t>(n)) {
        const unsigned type = (buf[at] << 8) | buf[at + 1], len = (buf[at + 2] << 8) | buf[at + 3];
        const unsigned char* v = buf + at + 4;
        if ((type == 0x0020 || type == 0x0001) && len >= 8 && v[1] == 0x01) {
          unsigned p = (v[2] << 8) | v[3];
          unsigned char a[4] = {v[4], v[5], v[6], v[7]};
          if (type == 0x0020) {
            p ^= 0x2112;
            a[0] ^= 0x21;
            a[1] ^= 0x12;
            a[2] ^= 0xA4;
            a[3] ^= 0x42;
          }
          char text[40];
          std::snprintf(text, sizeof text, "%u.%u.%u.%u:%u", a[0], a[1], a[2], a[3], p);
          return text;
        }
        at += 4 + ((len + 3) & ~3u);
      }
    }
  }
  return {};
}

Sock udpSocket(int family) {
  Sock s = socket(family, SOCK_DGRAM, IPPROTO_UDP);
  return s;
}

std::string localIpv6() {
  Sock s = udpSocket(AF_INET6);
  if (s == kBad) return {};
  sockaddr_in6 a;
  std::memset(&a, 0, sizeof a);
  a.sin6_family = AF_INET6;
  a.sin6_port = htons(53);
  inet_pton(AF_INET6, "2001:4860:4860::8888", &a.sin6_addr);
  std::string out;
  // a UDP connect sends nothing: it only picks the source address the system would use
  if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0) {
    sockaddr_in6 me;
    socklen_t len = sizeof me;
    if (getsockname(s, reinterpret_cast<sockaddr*>(&me), &len) == 0) {
      const unsigned char* b = reinterpret_cast<const unsigned char*>(&me.sin6_addr);
      const bool linkLocal = b[0] == 0xfe && (b[1] & 0xc0) == 0x80;
      const bool ula = (b[0] & 0xfe) == 0xfc;
      bool zero = true;
      for (int i = 0; i < 16; ++i) zero = zero && b[i] == 0;
      char text[64];
      if (!zero && !linkLocal && !ula && inet_ntop(AF_INET6, &me.sin6_addr, text, sizeof text)) out = text;
    }
  }
  closeS(s);
  return out;
}

}  // namespace

std::string stunMappedAddress(const std::string& host, unsigned port, int timeoutMs, int) {
#if defined(_WIN32)
  static WsaInit wsa;
#endif
  Sock s = udpSocket(AF_INET);
  if (s == kBad) return {};
  const std::string r = stunOn(s, host, port, timeoutMs);
  closeS(s);
  return r;
}

NetReport testConnection(const NetSettings& st) {
#if defined(_WIN32)
  static WsaInit wsa;
#endif
  NetReport rep;
  // 1. public address and NAT behaviour: two servers, one socket
  Sock s = udpSocket(AF_INET);
  std::string a1, a2;
  if (s != kBad) {
    sockaddr_in any;
    std::memset(&any, 0, sizeof any);
    any.sin_family = AF_INET;
    bind(s, reinterpret_cast<sockaddr*>(&any), sizeof any);
    if (!st.stunHost.empty()) a1 = stunOn(s, st.stunHost, st.stunPort, 3000);
    a2 = stunOn(s, kSecondStunHost, kSecondStunPort, 3000);
    sockaddr_in me;
    socklen_t len = sizeof me;
    getsockname(s, reinterpret_cast<sockaddr*>(&me), &len);
    closeS(s);
  }
  const std::string first = a1.empty() ? a2 : a1;
  rep.stunOk = !first.empty();
  rep.publicAddress = first;
  if (!rep.stunOk) {
    rep.nat = NetReport::Nat::Blocked;
  } else if (!a1.empty() && !a2.empty()) {
    rep.nat = a1 == a2 ? NetReport::Nat::Cone : NetReport::Nat::Symmetric;
  }
  // 2. router port mapping
#if defined(GRIDLINE_COLLAB_NET)
  if (st.portMapping) {
    portMapperAcquire();
    plum_mapping_t m;
    std::memset(&m, 0, sizeof m);
    m.protocol = PLUM_IP_PROTOCOL_UDP;
    std::random_device rd;
    m.internal_port = static_cast<uint16_t>(49152 + rd() % 16000);
    const int id = plum_create_mapping(&m, nullptr);
    for (int i = 0; i < 90 && id >= 0; ++i) {
      plum_state_t state;
      plum_mapping_t out;
      if (plum_query_mapping(id, &state, &out) == 0 && (state == PLUM_STATE_SUCCESS || state == PLUM_STATE_FAILURE)) {
        if (state == PLUM_STATE_SUCCESS) {
          rep.portMapping = true;
          rep.mappedAddress = std::string(out.external_host) + ":" + std::to_string(out.external_port);
          rep.portMappingProtocol = out.mapping_protocol == PLUM_MAPPING_PROTOCOL_PCP      ? "PCP"
                                    : out.mapping_protocol == PLUM_MAPPING_PROTOCOL_NATPMP ? "NAT-PMP"
                                    : out.mapping_protocol == PLUM_MAPPING_PROTOCOL_UPNP   ? "UPnP-IGD"
                                                                                           : "direct";
        }
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (id >= 0) plum_destroy_mapping(id);
    portMapperRelease();
  }
#endif
  // 3. IPv6
  rep.ipv6Address = localIpv6();
  rep.ipv6 = !rep.ipv6Address.empty();
  // summary
  std::string sum;
  switch (rep.nat) {
    case NetReport::Nat::Cone:
      sum = i18n::tr("Your router keeps one public address for every destination: direct connections should work with most collaborators.");
      break;
    case NetReport::Nat::Symmetric:
      sum = i18n::tr("Your router gives every destination a different public port (symmetric NAT): direct connections to people behind similar routers will fail. A collaborator who reaches you both, or a TURN server, still works.");
      break;
    case NetReport::Nat::Blocked:
      sum = i18n::tr("No STUN server answered: this network may block UDP. Collaborators on the same network can still connect; others need a TURN server (Settings > Collaboration).");
      break;
    default: sum = i18n::tr("Your public address is known; the router's behaviour could not be tested."); break;
  }
  sum += "\n";
  sum += rep.portMapping ? i18n::tr("Automatic port opening works on this router.") : i18n::tr("The router does not open ports automatically (or the option is off).");
  sum += "\n";
  sum += rep.ipv6 ? i18n::tr("IPv6 is available: connections to collaborators with IPv6 often need no traversal at all.") : i18n::tr("No IPv6 on this network.");
  rep.summary = sum;
  return rep;
}

}  // namespace gl::collab
