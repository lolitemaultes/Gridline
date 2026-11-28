// Gridline - tests: a small HTTP/1.1 server on 127.0.0.1 standing in for the release host, with
// faults on demand: errors, delays, stalls, dropped connections, cut-off and corrupted files,
// ranges and ETags. POSIX only (the update tests that use it are skipped on Windows).
#pragma once
#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace testsrv {

struct Route {
  int status = 200;
  std::string body;
  std::string contentType = "application/octet-stream";
  std::string etag;            // answered; If-None-Match equal to it gets 304
  int delayMs = 0;             // before the answer
  int chunkDelayMs = 0;        // between 16 KB chunks
  long dropAfter = -1;         // close the connection after this many body bytes ...
  int dropTimes = 0;           // ... this many times (then the whole file)
  long stallAfter = -1;        // stop sending after this many bytes, for stallMs
  int stallMs = 0;
  int stallTimes = 0;
  long corruptAt = -1;         // flip this byte
  bool ignoreRange = false;    // answer 200 with the whole file even to a Range request
  long lieLength = -1;         // Content-Length to claim (truncated file: fewer bytes than claimed)
};

struct Seen {
  std::string path, range, ifNoneMatch, userAgent;
};

class Server {
 public:
  Server() {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    ::bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a);
    socklen_t len = sizeof a;
    ::getsockname(fd_, reinterpret_cast<sockaddr*>(&a), &len);
    port_ = ntohs(a.sin_port);
    ::listen(fd_, 16);
    thread_ = std::thread([this] { loop(); });
  }
  ~Server() { stop(); }
  void stop() {
    if (stopped_.exchange(true)) return;
    ::shutdown(fd_, SHUT_RDWR);
    ::close(fd_);
    if (thread_.joinable()) thread_.join();
    std::vector<std::thread> ts;
    {
      std::lock_guard<std::mutex> lk(mu_);
      ts.swap(workers_);
    }
    for (std::thread& t : ts)
      if (t.joinable()) t.join();
  }
  int port() const { return port_; }
  std::string base() const { return "http://127.0.0.1:" + std::to_string(port_); }
  void set(const std::string& path, const Route& r) {
    std::lock_guard<std::mutex> lk(mu_);
    routes_[path] = r;
  }
  Route get(const std::string& path) {
    std::lock_guard<std::mutex> lk(mu_);
    return routes_[path];
  }
  void clear() {
    std::lock_guard<std::mutex> lk(mu_);
    routes_.clear();
  }
  std::vector<Seen> seen() {
    std::lock_guard<std::mutex> lk(mu_);
    return seen_;
  }
  void forget() {
    std::lock_guard<std::mutex> lk(mu_);
    seen_.clear();
  }
  int count(const std::string& path) {
    int n = 0;
    for (const Seen& s : seen())
      if (s.path == path) ++n;
    return n;
  }

 private:
  void loop() {
    while (!stopped_) {
      const int c = ::accept(fd_, nullptr, nullptr);
      if (c < 0) {
        if (stopped_) return;
        continue;
      }
      std::lock_guard<std::mutex> lk(mu_);
      workers_.emplace_back([this, c] { serve(c); });
    }
  }
  static bool sendAll(int c, const char* p, size_t n) {
    while (n > 0) {
#if defined(MSG_NOSIGNAL)
      const ssize_t w = ::send(c, p, n, MSG_NOSIGNAL);
#else
      const ssize_t w = ::send(c, p, n, 0);
#endif
      if (w <= 0) return false;
      p += w;
      n -= static_cast<size_t>(w);
    }
    return true;
  }
  static std::string header(const std::string& req, const char* name) {
    const std::string n = std::string("\r\n") + name + ":";
    std::string low = req, ln = n;
    for (char& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    for (char& ch : ln) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    const size_t at = low.find(ln);
    if (at == std::string::npos) return {};
    size_t a = at + ln.size();
    while (a < req.size() && req[a] == ' ') ++a;
    const size_t b = req.find("\r\n", a);
    return req.substr(a, b - a);
  }
  void serve(int c) {
#if defined(SO_NOSIGPIPE)
    int one = 1;
    ::setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    std::string req;
    char buf[4096];
    while (req.find("\r\n\r\n") == std::string::npos) {
      const ssize_t n = ::recv(c, buf, sizeof buf, 0);
      if (n <= 0) {
        ::close(c);
        return;
      }
      req.append(buf, static_cast<size_t>(n));
      if (req.size() > 65536) break;
    }
    const size_t sp1 = req.find(' '), sp2 = req.find(' ', sp1 + 1);
    const std::string path = req.substr(sp1 + 1, sp2 - sp1 - 1);
    Seen s{path, header(req, "Range"), header(req, "If-None-Match"), header(req, "User-Agent")};
    Route r;
    bool found;
    {
      std::lock_guard<std::mutex> lk(mu_);
      seen_.push_back(s);
      auto it = routes_.find(path);
      found = it != routes_.end();
      if (found) {
        r = it->second;
        // Faults that happen a limited number of times count down.
        if (it->second.dropTimes > 0) --it->second.dropTimes;
        if (it->second.stallTimes > 0) --it->second.stallTimes;
      }
    }
    if (!found) r.status = 404, r.body = "not found", r.contentType = "text/plain";
    if (r.delayMs) sleepMs(r.delayMs, c);
    if (stopped_) {
      ::close(c);
      return;
    }
    std::string body = r.body;
    if (r.corruptAt >= 0 && r.corruptAt < static_cast<long>(body.size())) body[static_cast<size_t>(r.corruptAt)] ^= 0x5a;
    int status = r.status;
    long from = 0;
    if (!r.etag.empty() && s.ifNoneMatch == r.etag) status = 304, body.clear();
    if (status == 200 && !s.range.empty() && !r.ignoreRange) {
      long a = -1;
      if (std::sscanf(s.range.c_str(), "bytes=%ld-", &a) == 1 && a >= 0) {
        if (a >= static_cast<long>(body.size())) {
          status = 416;
          body.clear();
        } else {
          status = 206;
          from = a;
        }
      }
    }
    const std::string payload = status == 206 ? body.substr(static_cast<size_t>(from)) : body;
    std::string h = "HTTP/1.1 " + std::to_string(status) + (status == 200 ? " OK" : status == 206 ? " Partial Content" : " Status") + "\r\n";
    h += "Content-Type: " + r.contentType + "\r\n";
    const long claimed = r.lieLength >= 0 ? r.lieLength - from : static_cast<long>(payload.size());
    h += "Content-Length: " + std::to_string(claimed) + "\r\n";
    if (status == 206) h += "Content-Range: bytes " + std::to_string(from) + "-" + std::to_string(body.size() - 1) + "/" + std::to_string(body.size()) + "\r\n";
    if (status == 416) h += "Content-Range: bytes */" + std::to_string(r.body.size()) + "\r\n";
    if (!r.etag.empty()) h += "ETag: " + r.etag + "\r\n";
    h += "Accept-Ranges: bytes\r\nConnection: close\r\n\r\n";
    if (!sendAll(c, h.data(), h.size())) {
      ::close(c);
      return;
    }
    size_t at = 0;
    const size_t chunk = 16 * 1024;
    bool stalled = false;
    while (at < payload.size() && !stopped_) {
      size_t n = std::min(chunk, payload.size() - at);
      const long sent = from + static_cast<long>(at);
      if (r.dropAfter >= 0 && r.dropTimes > 0 && sent + static_cast<long>(n) > r.dropAfter) {
        n = static_cast<size_t>(std::max(0L, r.dropAfter - sent));
        if (n) sendAll(c, payload.data() + at, n);
        break;  // the connection drops
      }
      if (r.stallAfter >= 0 && r.stallTimes > 0 && !stalled && sent + static_cast<long>(n) > r.stallAfter) {
        n = static_cast<size_t>(std::max(0L, r.stallAfter - sent));
        if (n && !sendAll(c, payload.data() + at, n)) break;
        at += n;
        stalled = true;
        sleepMs(r.stallMs, c);
        continue;
      }
      if (!sendAll(c, payload.data() + at, n)) break;
      at += n;
      if (r.chunkDelayMs) sleepMs(r.chunkDelayMs, c);
    }
    ::shutdown(c, SHUT_RDWR);
    ::close(c);
  }
  void sleepMs(int ms, int) {
    for (int i = 0; i < ms / 10 && !stopped_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  int fd_ = -1;
  int port_ = 0;
  std::atomic<bool> stopped_{false};
  std::thread thread_;
  std::mutex mu_;
  std::map<std::string, Route> routes_;
  std::vector<Seen> seen_;
  std::vector<std::thread> workers_;
};

}  // namespace testsrv
#endif
