// Gridline - K-264: the MCP server's listen address (see McpListen.h).
#include "McpListen.h"
#include <cctype>
#include <cstdio>
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace gl::mcp::listen {

Mode modeFromId(const std::string& id) {
  if (id == "vms") return Mode::LoopbackAndVms;
  if (id == "address") return Mode::Address;
  if (id == "all") return Mode::All;
  return Mode::Loopback;
}

const char* modeId(Mode m) {
  switch (m) {
    case Mode::LoopbackAndVms: return "vms";
    case Mode::Address: return "address";
    case Mode::All: return "all";
    default: return "loopback";
  }
}

bool parseIpv4(const std::string& text, uint32_t& out) {
  uint32_t v = 0;
  int parts = 0;
  size_t i = 0;
  while (parts < 4) {
    if (i >= text.size() || !std::isdigit(static_cast<unsigned char>(text[i]))) return false;
    uint32_t n = 0;
    size_t digits = 0;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
      n = n * 10 + static_cast<uint32_t>(text[i] - '0');
      if (++digits > 3 || n > 255) return false;
      ++i;
    }
    v = (v << 8) | n;
    ++parts;
    if (parts < 4) {
      if (i >= text.size() || text[i] != '.') return false;
      ++i;
    }
  }
  if (i != text.size()) return false;
  out = v;
  return true;
}

std::string ipv4Text(uint32_t h) {
  char b[20];
  std::snprintf(b, sizeof b, "%u.%u.%u.%u", (h >> 24) & 255, (h >> 16) & 255, (h >> 8) & 255, h & 255);
  return b;
}

bool isLoopback(uint32_t h) { return (h >> 24) == 127; }

bool isPrivate(uint32_t h) {
  return (h >> 24) == 10 || (h >> 20) == (172u << 4 | 1u) || (h >> 16) == (192u << 8 | 168u) || (h >> 16) == (169u << 8 | 254u) ||
         (h >> 22) == (100u << 2 | 1u);  // 100.64.0.0/10
}

uint32_t bindAddress(const Policy& p) {
  switch (p.mode) {
    case Mode::Loopback: return 0x7F000001u;
    case Mode::Address: return p.address;
    default: return 0;
  }
}

bool peerAllowed(const Policy& p, uint32_t peer) {
  if (isLoopback(peer)) return true;
  switch (p.mode) {
    case Mode::Loopback: return false;
    case Mode::All: return true;
    default: return isPrivate(peer);
  }
}

bool hostAllowed(const Policy& p, const std::string& hostHeader, const std::vector<uint32_t>& own) {
  std::string h;
  for (char c : hostHeader) h += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (!h.empty() && h[0] == '[') {  // [::1]:port
    const size_t e = h.find(']');
    return e != std::string::npos && h.substr(1, e - 1) == "::1";
  }
  const size_t c = h.find(':');
  if (c != std::string::npos) h = h.substr(0, c);
  if (h == "127.0.0.1" || h == "localhost") return true;
  if (p.mode == Mode::Loopback) return false;
  uint32_t ip = 0;
  if (!parseIpv4(h, ip)) return false;  // a host name: never (DNS rebinding)
  for (uint32_t o : own)
    if (o == ip) return true;
  return false;
}

std::vector<uint32_t> localAddresses() {
  std::vector<uint32_t> out = {0x7F000001u};
#if defined(_WIN32)
  char name[256] = {};
  if (gethostname(name, sizeof name) == 0) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    if (getaddrinfo(name, nullptr, &hints, &res) == 0) {
      for (addrinfo* r = res; r; r = r->ai_next)
        if (r->ai_family == AF_INET) out.push_back(ntohl(reinterpret_cast<sockaddr_in*>(r->ai_addr)->sin_addr.s_addr));
      freeaddrinfo(res);
    }
  }
#else
  ifaddrs* list = nullptr;
  if (getifaddrs(&list) == 0) {
    for (ifaddrs* i = list; i; i = i->ifa_next)
      if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET) {
        const uint32_t a = ntohl(reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr.s_addr);
        bool dup = false;
        for (uint32_t o : out) dup = dup || o == a;
        if (!dup) out.push_back(a);
      }
    freeifaddrs(list);
  }
#endif
  return out;
}

std::string clientHost(const Policy& p, const std::vector<uint32_t>& own) {
  if (p.mode == Mode::Loopback) return "127.0.0.1";
  if (p.mode == Mode::Address) return ipv4Text(p.address);
  for (uint32_t o : own)
    if (!isLoopback(o) && isPrivate(o)) return ipv4Text(o);
  return "127.0.0.1";
}

bool AuthThrottle::blocked(uint32_t peer, double now) const {
  const auto it = e_.find(peer);
  return it != e_.end() && now < it->second.until;
}

void AuthThrottle::failed(uint32_t peer, double now) {
  Entry& e = e_[peer];
  if (now - e.first > kWindow) e.fails = 0, e.first = now;
  if (++e.fails >= kMaxFails) e.until = now + kBlock, e.fails = 0, e.first = now;
  if (e_.size() > 4096) e_.clear();  // bounded memory under a flood of addresses
}

void AuthThrottle::succeeded(uint32_t peer) { e_.erase(peer); }

}  // namespace gl::mcp::listen
