// Gridline: the OSC server (see OscServer.h).
#include "OscServer.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include "Net.h"

namespace gl {

namespace {

using net::Sock;
using net::kNoSock;

#if defined(_WIN32)
SOCKET raw(Sock s) { return static_cast<SOCKET>(s); }
bool inUse() {
  const int e = WSAGetLastError();
  return e == WSAEADDRINUSE || e == WSAEACCES;
}
#else
int raw(Sock s) { return static_cast<int>(s); }
bool inUse() { return errno == EADDRINUSE; }
#endif

Sock udpSocket() {
  net::startup();
#if defined(_WIN32)
  SOCKET s = ::WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
  return s == INVALID_SOCKET ? kNoSock : static_cast<Sock>(s);
#else
  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return kNoSock;
  net::setNoInherit(fd);
  return fd;
#endif
}

sockaddr_in addrOf(uint32_t ip, int port) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<uint16_t>(port));
  a.sin_addr.s_addr = htonl(ip);
  return a;
}

bool bindTo(Sock s, uint32_t ip, int port) {
  sockaddr_in a = addrOf(ip, port);
  return ::bind(raw(s), reinterpret_cast<const sockaddr*>(&a), sizeof a) == 0;
}

int boundPort(Sock s) {
  sockaddr_in a{};
  net::SockLen len = sizeof a;
  if (::getsockname(raw(s), reinterpret_cast<sockaddr*>(&a), &len) != 0) return 0;
  return ntohs(a.sin_port);
}

uint32_t readU32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

constexpr uint32_t kLoopback = 0x7F000001u;

// Allowed hosts entries: "a.b.c.d" or "a.b.c.d/nn".
bool parseEntry(const std::string& e, uint32_t& ip, uint32_t& mask) {
  const size_t slash = e.find('/');
  int bits = 32;
  if (slash != std::string::npos) {
    const std::string b = e.substr(slash + 1);
    if (b.empty() || b.size() > 2 || b.find_first_not_of("0123456789") != std::string::npos) return false;
    bits = std::stoi(b);
    if (bits > 32) return false;
  }
  if (!OscServer::parseIPv4(e.substr(0, slash), ip)) return false;
  mask = bits == 0 ? 0u : ~uint32_t(0) << (32 - bits);
  ip &= mask;
  return true;
}

std::vector<std::string> entries(const std::string& list) {
  std::vector<std::string> v;
  std::string cur;
  for (char c : list + ",") {
    if (c == ',' || c == ' ' || c == ';' || c == '\t' || c == '\n') {
      if (!cur.empty()) v.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  return v;
}

}  // namespace

// One TCP client: its framing is detected from the first byte (0xC0: SLIP, else a length prefix).
struct OscServer::Conn {
  Sock fd = kNoSock;
  uint32_t ip = 0;
  uint16_t port = 0;
  int mode = 0;  // 0 = not known yet, 1 = SLIP, 2 = length prefixed
  osc::SlipDecoder slip;
  std::vector<uint8_t> pending;  // length prefixed: bytes of the frame being read
};

double OscServer::clock() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool OscServer::parseIPv4(const std::string& text, uint32_t& ip) {
  if (text == "localhost") {
    ip = kLoopback;
    return true;
  }
  uint32_t v = 0;
  int parts = 0;
  size_t i = 0;
  while (parts < 4) {
    if (i >= text.size() || text[i] < '0' || text[i] > '9') return false;
    unsigned n = 0;
    size_t digits = 0;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9' && digits < 4) n = n * 10 + static_cast<unsigned>(text[i++] - '0'), ++digits;
    if (n > 255 || digits > 3) return false;
    v = (v << 8) | n;
    ++parts;
    if (parts < 4) {
      if (i >= text.size() || text[i] != '.') return false;
      ++i;
    }
  }
  if (i != text.size()) return false;
  ip = v;
  return true;
}

std::string OscServer::ipText(uint32_t ip) {
  return std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 255) + "." + std::to_string((ip >> 8) & 255) + "." +
         std::to_string(ip & 255);
}

bool OscServer::validHostList(const std::string& list, std::string* bad) {
  for (const auto& e : entries(list)) {
    uint32_t ip = 0, mask = 0;
    if (!parseEntry(e, ip, mask)) {
      if (bad) *bad = e;
      return false;
    }
  }
  return true;
}

bool OscServer::hostAllowed(const std::string& list, uint32_t ip) {
  if ((ip >> 24) == 127) return true;
  const auto v = entries(list);
  if (v.empty()) return true;
  for (const auto& e : v) {
    uint32_t a = 0, mask = 0;
    if (parseEntry(e, a, mask) && (ip & mask) == a) return true;
  }
  return false;
}

bool OscServer::allowed(uint32_t ip) const {
  if ((ip >> 24) == 127 || allow_.empty()) return true;
  for (const auto& [a, mask] : allow_)
    if ((ip & mask) == a) return true;
  return false;
}

bool OscServer::start(const OscServerConfig& cfg, std::string* err) {
  stop();
  auto failWith = [&](const std::string& m) {
    if (err) *err = m;
    net::closeSock(udp_);
    net::closeSock(tcp_);
    udp_ = tcp_ = kNoSock;
    return false;
  };
  if (cfg.port < 0 || cfg.port > 65535) return failWith("Port must be 0 to 65535");
  std::string bad;
  if (cfg.network && !validHostList(cfg.allowedHosts, &bad)) return failWith("Not an address or range in the allowed hosts: " + bad);
  cfg_ = cfg;
  allow_.clear();
  if (cfg.network)
    for (const auto& e : entries(cfg.allowedHosts)) {
      uint32_t a = 0, mask = 0;
      if (parseEntry(e, a, mask)) allow_.push_back({a, mask});
    }
  const uint32_t bindIp = cfg.network ? 0u /* INADDR_ANY */ : kLoopback;
  udp_ = udpSocket();
  if (udp_ == kNoSock) return failWith("Could not open a UDP socket: " + net::lastError());
  if (!bindTo(udp_, bindIp, cfg.port)) {
    const bool used = inUse();
    const std::string why = net::lastError();
    return failWith(used ? "Port " + std::to_string(cfg.port) + " is in use by another program" : "Could not listen on port " + std::to_string(cfg.port) + ": " + why);
  }
  net::setNonBlocking(udp_);
  port_ = boundPort(udp_);
  if (cfg.tcp) {
    tcp_ = net::tcpSocket();
    if (tcp_ == kNoSock) return failWith("Could not open a TCP socket: " + net::lastError());
#if !defined(_WIN32)
    int one = 1;
    ::setsockopt(raw(tcp_), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#endif
    if (!bindTo(tcp_, bindIp, port_) || ::listen(raw(tcp_), 8) != 0) {
      const bool used = inUse();
      const std::string why = net::lastError();
      return failWith(used ? "TCP port " + std::to_string(port_) + " is in use by another program" : "Could not listen for TCP on port " + std::to_string(port_) + ": " + why);
    }
    net::setNonBlocking(tcp_);
  }
  {
    std::lock_guard<std::mutex> lk(mu_);
    queue_.clear();
    status_ = Status{};
  }
  buf_.assign(osc::kMaxPacket + 4, 0);
  quit_ = false;
  thread_ = std::thread([this] { loop(); });
  return true;
}

OscServer::OscServer() = default;

OscServer::~OscServer() {
  stop();
  net::closeSock(send_);
}

void OscServer::stop() {
  if (thread_.joinable()) {
    quit_ = true;
    thread_.join();
  }
  for (auto& c : conns_) net::closeSock(c->fd);
  conns_.clear();
  net::closeSock(udp_);
  net::closeSock(tcp_);
  udp_ = tcp_ = kNoSock;
  port_ = 0;
}

void OscServer::poll(std::vector<OscIncoming>& out) {
  std::lock_guard<std::mutex> lk(mu_);
  if (queue_.empty()) return;
  for (auto& m : queue_) out.push_back(std::move(m));
  queue_.clear();
}

OscServer::Status OscServer::status() const {
  std::lock_guard<std::mutex> lk(mu_);
  return status_;
}

bool OscServer::sendTo(uint32_t ip, int port, const std::vector<uint8_t>& packet, std::string* err) {
  if (port <= 0 || port > 65535 || packet.empty() || packet.size() > osc::kMaxPacket) {
    if (err) *err = "nothing to send or no port";
    return false;
  }
  Sock s = udp_;
  if (s == kNoSock) {
    if (send_ == kNoSock) {
      send_ = udpSocket();
      if (send_ != kNoSock) net::setNonBlocking(send_);
    }
    s = send_;
  }
  if (s == kNoSock) {
    if (err) *err = "no socket";
    return false;
  }
  const sockaddr_in a = addrOf(ip, port);
  const auto r = ::sendto(raw(s), reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0,
                          reinterpret_cast<const sockaddr*>(&a), sizeof a);
  if (r < 0) {
    if (err) *err = net::lastError();
    return false;
  }
  return true;
}

void OscServer::packet(const uint8_t* data, size_t n, uint32_t ip, uint16_t port, bool tcp) {
  const std::string from = ipText(ip) + ":" + std::to_string(port);
  if (!allowed(ip)) {
    std::lock_guard<std::mutex> lk(mu_);
    ++status_.refused;
    status_.lastProblem = "Refused a packet from " + from + " (not an allowed host)";
    return;
  }
  parsed_.clear();
  std::string why;
  if (!osc::parse(data, n, parsed_, &why)) {
    std::lock_guard<std::mutex> lk(mu_);
    ++status_.malformed;
    status_.lastProblem = "Malformed packet from " + from + ": " + why;
    return;
  }
  std::lock_guard<std::mutex> lk(mu_);
  for (auto& t : parsed_) {
    ++status_.received;
    status_.lastAddress = osc::describe(t.msg);
    status_.lastFrom = from;
    status_.lastTime = clock();
    if (queue_.size() >= kMaxQueue) {
      ++status_.dropped;
      continue;
    }
    OscIncoming in;
    in.msg = std::move(t.msg);
    in.time = t.time;
    in.ip = ip;
    in.port = port;
    in.tcp = tcp;
    queue_.push_back(std::move(in));
  }
}

void OscServer::loop() {
  std::vector<net::PollFd> fds;
  std::vector<std::vector<uint8_t>> frames;
  while (!quit_.load()) {
    fds.clear();
    fds.push_back(net::pollEntry(udp_, POLLIN));
    if (tcp_ != kNoSock) fds.push_back(net::pollEntry(tcp_, POLLIN));
    const size_t firstConn = fds.size();
    for (const auto& c : conns_) fds.push_back(net::pollEntry(c->fd, POLLIN));
    const int r = net::pollSocks(fds.data(), fds.size(), 100);
    if (r <= 0) continue;
    // UDP: every waiting datagram (a bounded number per round).
    if (fds[0].revents & (POLLIN | POLLERR)) {
      for (int k = 0; k < 256 && !quit_.load(); ++k) {
        sockaddr_in from{};
        net::SockLen len = sizeof from;
        const auto got = ::recvfrom(raw(udp_), reinterpret_cast<char*>(buf_.data()), static_cast<int>(buf_.size()), 0,
                                    reinterpret_cast<sockaddr*>(&from), &len);
        if (got < 0) break;  // nothing more (or a transient error, e.g. an ICMP report on Windows)
        const size_t n = static_cast<size_t>(got);
        const uint32_t ip = ntohl(from.sin_addr.s_addr);
        const uint16_t port = ntohs(from.sin_port);
        if (n > osc::kMaxPacket) {
          std::lock_guard<std::mutex> lk(mu_);
          ++status_.malformed;
          status_.lastProblem = "Packet too large from " + ipText(ip);
          continue;
        }
        packet(buf_.data(), n, ip, port, false);
      }
    }
    // TCP: new clients.
    if (tcp_ != kNoSock && (fds[1].revents & POLLIN)) {
      for (;;) {
        sockaddr_in from{};
        net::SockLen len = sizeof from;
        const auto fd = ::accept(raw(tcp_), reinterpret_cast<sockaddr*>(&from), &len);
#if defined(_WIN32)
        if (fd == INVALID_SOCKET) break;
#else
        if (fd < 0) break;
#endif
        const Sock s = static_cast<Sock>(fd);
        const uint32_t ip = ntohl(from.sin_addr.s_addr);
        if (conns_.size() >= kMaxTcpClients || !allowed(ip)) {
          net::closeSock(s);
          std::lock_guard<std::mutex> lk(mu_);
          ++status_.refused;
          status_.lastProblem = conns_.size() >= kMaxTcpClients ? "Too many TCP clients" : "Refused a TCP client from " + ipText(ip) + " (not an allowed host)";
          continue;
        }
        net::setNoInherit(s);
        net::setNonBlocking(s);
        net::setNoSigPipe(s);
        auto c = std::make_unique<Conn>();
        c->fd = s;
        c->ip = ip;
        c->port = ntohs(from.sin_port);
        conns_.push_back(std::move(c));
      }
    }
    // TCP: data from clients.
    for (size_t k = 0; k < conns_.size() && firstConn + k < fds.size(); ++k) {
      Conn& c = *conns_[k];
      if (!(fds[firstConn + k].revents & (POLLIN | POLLERR | POLLHUP))) continue;
      const long got = net::recvSome(c.fd, reinterpret_cast<char*>(buf_.data()), buf_.size());
      if (got <= 0) {
        if (got < 0 && net::wouldBlock()) continue;
        net::closeSock(c.fd);
        c.fd = kNoSock;
        continue;
      }
      const uint8_t* p = buf_.data();
      size_t n = static_cast<size_t>(got);
      if (c.mode == 0) c.mode = p[0] == osc::kSlipEnd ? 1 : 2;
      bool bad = false;
      if (c.mode == 1) {
        frames.clear();
        c.slip.feed(p, n, frames);
        for (const auto& f : frames) packet(f.data(), f.size(), c.ip, c.port, true);
      } else {
        while (n > 0) {
          const size_t take = std::min(n, osc::kMaxPacket + 4 - c.pending.size());
          c.pending.insert(c.pending.end(), p, p + take);
          p += take;
          n -= take;
          while (c.pending.size() >= 4) {
            const uint32_t size = readU32(c.pending.data());
            if (size == 0 || size > osc::kMaxPacket) {
              bad = true;
              break;
            }
            if (c.pending.size() < 4 + size_t(size)) break;
            packet(c.pending.data() + 4, size, c.ip, c.port, true);
            c.pending.erase(c.pending.begin(), c.pending.begin() + 4 + static_cast<std::ptrdiff_t>(size));
          }
          if (bad) break;
        }
      }
      if (bad) {
        std::lock_guard<std::mutex> lk(mu_);
        ++status_.malformed;
        status_.lastProblem = "Bad packet size from " + ipText(c.ip) + " (TCP client closed)";
        net::closeSock(c.fd);
        c.fd = kNoSock;
      }
    }
    conns_.erase(std::remove_if(conns_.begin(), conns_.end(), [](const std::unique_ptr<Conn>& c) { return c->fd == kNoSock; }), conns_.end());
    std::lock_guard<std::mutex> lk(mu_);
    status_.tcpClients = static_cast<int>(conns_.size());
  }
}

}  // namespace gl
