// Gridline - J-111 collaboration: connection test. See NetTest.h.
#include "NetTest.h"
#include <chrono>
#include <cstdlib>
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

std::vector<NetSettings::Stun> stunServers(const NetSettings& s) {
  std::vector<NetSettings::Stun> out;
  auto add = [&](const std::string& h, uint16_t p) {
    if (h.empty()) return;
    for (const auto& x : out)
      if (x.host == h && x.port == p) return;
    out.push_back({h, p});
  };
  add(s.stunHost, s.stunPort);
  for (const auto& b : s.stunBackups) add(b.host, b.port);
  return out;
}

std::vector<NetSettings::Stun> parseStunList(const std::string& text) {
  std::vector<NetSettings::Stun> out;
  std::string cur;
  auto flush = [&] {
    while (!cur.empty() && (cur.back() == ' ' || cur.back() == '\t')) cur.pop_back();
    size_t b = 0;
    while (b < cur.size() && (cur[b] == ' ' || cur[b] == '\t')) ++b;
    std::string e = cur.substr(b);
    cur.clear();
    if (e.rfind("stun:", 0) == 0) e = e.substr(5);
    if (e.empty()) return;
    unsigned port = 3478;
    const size_t c = e.rfind(':');
    if (c != std::string::npos && e.find(':') == c) {  // host:port (an IPv6 address alone has several colons)
      port = static_cast<unsigned>(std::atoi(e.c_str() + c + 1));
      e = e.substr(0, c);
    }
    if (e.empty() || port == 0 || port > 65535) return;
    out.push_back({e, static_cast<uint16_t>(port)});
  };
  for (char ch : text) {
    if (ch == ',' || ch == ';' || ch == '\n' || ch == '\r') flush();
    else cur += ch;
  }
  flush();
  return out;
}

std::string formatStunList(const std::vector<NetSettings::Stun>& list) {
  std::string out;
  for (const auto& x : list) out += (out.empty() ? "" : ", ") + x.host + ":" + std::to_string(x.port);
  return out;
}

namespace {
// K-312: the public address from the servers in order, from one socket, until `want` answered
// (a server that gives no answer is skipped). `answered` / `silent` name them.
std::vector<std::string> askServers(const NetSettings& st, int timeoutMs, size_t want, std::vector<std::string>* answered,
                                    std::vector<std::string>* silent) {
  std::vector<std::string> addrs;
  Sock s = udpSocket(AF_INET);
  if (s == kBad) return addrs;
  sockaddr_in any;
  std::memset(&any, 0, sizeof any);
  any.sin_family = AF_INET;
  bind(s, reinterpret_cast<sockaddr*>(&any), sizeof any);
  for (const auto& srv : stunServers(st)) {
    if (addrs.size() >= want) break;
    const std::string name = srv.host + ":" + std::to_string(srv.port);
    const std::string a = stunOn(s, srv.host, srv.port, timeoutMs);
    if (a.empty()) {
      if (silent) silent->push_back(name);
      continue;
    }
    addrs.push_back(a);
    if (answered) answered->push_back(name);
  }
  closeS(s);
  return addrs;
}
}  // namespace

char probeNatType(const NetSettings& st, int timeoutMs) {
#if defined(_WIN32)
  static WsaInit wsa;
#endif
  const std::vector<std::string> a = askServers(st, timeoutMs, 2, nullptr, nullptr);
  if (a.size() < 2) return 'u';
  return a[0] == a[1] ? 'c' : 's';
}

std::string turnConfigProblem(const NetSettings& s) {
  if (s.turnHost.empty() && (!s.turnUser.empty() || !s.turnPass.empty()))
    return i18n::tr("TURN server missing: a TURN user name or password is filled in, but no server, so no relay is used. Add the server's address, or clear the user name and password.");
  if (!s.turnHost.empty() && s.turnUser.empty())
    return i18n::tr("The TURN server has no user name: most servers refuse a relay without a user name and password.");
  return {};
}

std::string describePortMapping(const std::vector<std::string>& log, bool timedOut) {
  auto has = [&](const char* t) {
    for (const auto& l : log)
      if (l.find(t) != std::string::npos) return true;
    return false;
  };
  auto find = [&](const char* t) -> std::string {
    for (const auto& l : log)
      if (l.find(t) != std::string::npos) return l;
    return {};
  };
  if (has("Local address is public")) return i18n::tr("This computer has a public address itself: there is no router to ask.");
  std::string out;
  auto add = [&](const std::string& t) {
    if (!out.empty()) out += " ";
    out += t;
  };
  // PCP, then NAT-PMP (the same router service), then UPnP-IGD
  if (has("Success probing PCP")) add(i18n::tr("PCP: the router answered but did not open the port."));
  else if (has("Probing PCP")) add(i18n::tr("PCP: no answer."));
  const std::string pmpErr = find("NAT-PMP error response");
  if (!pmpErr.empty()) {
    const size_t eq = pmpErr.rfind('=');
    add(std::string(i18n::tr("NAT-PMP: the router refused")) + (eq != std::string::npos ? " (" + pmpErr.substr(eq + 1) + ")." : "."));
  } else if (has("Success probing NAT-PMP")) {
    add(i18n::tr("NAT-PMP: the router answered but did not open the port."));
  } else if (has("Probing NAT-PMP") || has("Falling back to NAT-PMP")) {
    add(i18n::tr("NAT-PMP: no answer."));
  }
  if (has("Found UPnP-IGD device") || has("Success probing UPnP")) {
    if (has("WANIPConnection not found")) add(i18n::tr("UPnP: the router answered but offers no port opening service."));
    else if (has("UPnP error") || has("request to UPnP-IGD device failed")) add(i18n::tr("UPnP: the router refused the port (UPnP may be limited to some devices in its settings)."));
    else if (has("Timed-out sending HTTP request")) add(i18n::tr("UPnP: the router stopped answering."));
    else add(i18n::tr("UPnP: the router answered but did not open the port."));
  } else if (has("SSDP") || has("Probing UPnP")) {
    add(i18n::tr("UPnP: no router answered (UPnP is off on the router, or this network blocks it)."));
  }
  if (out.empty()) out = timedOut ? i18n::tr("The router did not answer in time.") : i18n::tr("The router does not offer automatic port opening.");
  else if (timedOut) add(i18n::tr("The test stopped waiting after 15 seconds."));
  return out;
}

NetReport testConnection(const NetSettings& st) {
#if defined(_WIN32)
  static WsaInit wsa;
#endif
  NetReport rep;
  // 1. public address and NAT behaviour: two servers from the list, one socket (K-312: a server
  // that does not answer is skipped, and the report names which answered)
  const std::vector<std::string> addrs = askServers(st, 3000, 2, &rep.stunAnswered, &rep.stunSilent);
  rep.stunOk = !addrs.empty();
  if (rep.stunOk) rep.publicAddress = addrs[0];
  if (!rep.stunOk) {
    rep.nat = NetReport::Nat::Blocked;
  } else if (addrs.size() >= 2) {
    rep.nat = addrs[0] == addrs[1] ? NetReport::Nat::Cone : NetReport::Nat::Symmetric;
  }
  // 2. router port mapping
#if defined(GRIDLINE_COLLAB_NET)
  if (st.portMapping) {
    portMapperCapture(true);
    portMapperAcquire();
    plum_mapping_t m;
    std::memset(&m, 0, sizeof m);
    m.protocol = PLUM_IP_PROTOCOL_UDP;
    std::random_device rd;
    m.internal_port = static_cast<uint16_t>(49152 + rd() % 16000);
    const int id = plum_create_mapping(&m, nullptr);
    // K-312: up to 15 s: the router is asked with PCP, then NAT-PMP, then UPnP-IGD (about
    // 4 s each), so a router that only speaks UPnP answers after about 12 s
    bool settled = id < 0;
    for (int i = 0; i < 150 && id >= 0; ++i) {
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
        settled = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (id >= 0) plum_destroy_mapping(id);
    portMapperCapture(false);
    const std::vector<std::string> log = portMapperLog();
    if (!rep.portMapping) rep.portMappingDetail = describePortMapping(log, !settled);
    portMapperRelease();
  }
#endif
  // 2b. K-312: the TURN relay
  rep.turnProblem = turnConfigProblem(st);
  if (!st.turnHost.empty()) {
    rep.turnTried = true;
    sockaddr_storage ta;
    socklen_t tl = 0;
    if (!resolve(st.turnHost, st.turnPort, AF_UNSPEC, ta, tl)) rep.turnDetail = i18n::tr("The TURN server's name could not be found: check its address.");
    else rep.turnOk = tryTurnAllocation(st, 8000, &rep.turnDetail);
  }
  // 3. IPv6
  rep.ipv6Address = localIpv6();
  rep.ipv6 = !rep.ipv6Address.empty();
  // summary
  std::string sum;
  switch (rep.nat) {
    case NetReport::Nat::Cone:
      sum = i18n::tr("Your router keeps one public address for every destination (cone NAT). Direct connections work with a collaborator whose router does the same. If theirs is symmetric (their test says so), a direct connection fails unless one of you opens a port: then use a TURN relay or a forwarded port.");
      break;
    case NetReport::Nat::Symmetric:
      sum = i18n::tr("Your router gives every destination a different public port (symmetric NAT). A direct connection only works when the other side lets connections in (an opened or forwarded port); with most home routers it fails. A TURN relay, a forwarded port, or a collaborator who reaches you both still works.");
      break;
    case NetReport::Nat::Blocked:
      sum = i18n::tr("No STUN server answered: this network may block UDP. Collaborators on the same network can still connect; others need a TURN server (Settings > Collaboration).");
      break;
    default: sum = i18n::tr("Your public address is known; the router's behaviour could not be tested."); break;
  }
  sum += "\n";
  if (rep.portMapping) sum += i18n::tr("Automatic port opening works on this router.");
  else if (!st.portMapping) sum += i18n::tr("Automatic port opening is off in these settings.");
  else sum += std::string(i18n::tr("The router did not open a port automatically.")) + (rep.portMappingDetail.empty() ? "" : " " + rep.portMappingDetail);
  sum += "\n";
  sum += rep.ipv6 ? i18n::tr("IPv6 is available: connections to collaborators with IPv6 often need no traversal at all.") : i18n::tr("No IPv6 on this network.");
  if (!rep.stunAnswered.empty()) {
    sum += "\n" + std::string(i18n::tr("Address servers that answered:")) + " ";
    for (size_t i = 0; i < rep.stunAnswered.size(); ++i) sum += (i ? ", " : "") + rep.stunAnswered[i];
    sum += ".";
  }
  if (!rep.stunSilent.empty()) {
    sum += "\n" + std::string(i18n::tr("No answer from:")) + " ";
    for (size_t i = 0; i < rep.stunSilent.size(); ++i) sum += (i ? ", " : "") + rep.stunSilent[i];
    sum += ".";
  }
  if (!rep.turnProblem.empty()) sum += "\n" + rep.turnProblem;
  if (rep.turnTried)
    sum += "\n" + (rep.turnOk ? std::string(i18n::tr("The TURN relay works: when a direct connection fails, the connection goes through it."))
                               : std::string(i18n::tr("The TURN relay did not work.")) + " " + rep.turnDetail);
  rep.summary = sum;
  return rep;
}

}  // namespace gl::collab
