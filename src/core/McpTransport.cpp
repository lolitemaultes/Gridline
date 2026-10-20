// Gridline — MCP transports: loopback HTTP server / client, stdio loop,
// attach bridge (see McpTransport.h).
#include "McpTransport.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include "Settings.h"

namespace gl::mcp {

namespace fs = std::filesystem;

namespace {
double monoNow() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
std::string lowerStr(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}
void noSigPipe(int fd) {
#ifdef SO_NOSIGPIPE
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#else
  (void)fd;
#endif
}
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif
// Parses "Name: value" lines after the start line (names lower-cased).
void parseHeaders(const std::string& head, std::string& first, std::map<std::string, std::string>& out) {
  std::istringstream is(head);
  std::string line;
  bool start = true;
  while (std::getline(is, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (start) {
      first = line;
      start = false;
      continue;
    }
    const size_t c = line.find(':');
    if (c == std::string::npos) continue;
    out[lowerStr(trim(line.substr(0, c)))] = trim(line.substr(c + 1));
  }
}
}  // namespace

std::string randomToken() {
  std::random_device rd;
  std::string s;
  char b[9];
  for (int i = 0; i < 4; ++i) {
    std::snprintf(b, sizeof b, "%08x", static_cast<unsigned>(rd()));
    s += b;
  }
  return s;
}

bool constantTimeEqual(const std::string& a, const std::string& b) {
  unsigned diff = static_cast<unsigned>(a.size() ^ b.size());
  const size_t n = std::max(a.size(), b.size());
  for (size_t i = 0; i < n; ++i) {
    const unsigned char x = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
    const unsigned char y = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
    diff |= static_cast<unsigned>(x ^ y);
  }
  return diff == 0;
}

std::string httpReason(int s) {
  switch (s) {
    case 200: return "OK";
    case 202: return "Accepted";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 431: return "Request Header Fields Too Large";
    case 503: return "Service Unavailable";
    default: return "Error";
  }
}

// ============================================================ HTTP server ====

bool HttpServer::start(int port, std::string* err) {
  stop();
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    if (err) *err = std::string("socket: ") + std::strerror(errno);
    return false;
  }
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<uint16_t>(port));
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 127.0.0.1 only, never INADDR_ANY
  if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(fd, 16) != 0) {
    if (err) *err = "cannot listen on 127.0.0.1:" + std::to_string(port) + ": " + std::strerror(errno);
    ::close(fd);
    return false;
  }
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
  fcntl(fd, F_SETFD, FD_CLOEXEC);
  socklen_t len = sizeof a;
  getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
  fd_ = fd;
  port_ = ntohs(a.sin_port);
  return true;
}

void HttpServer::stop() {
  for (auto& c : conns_) ::close(c.fd);
  conns_.clear();
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
  port_ = 0;
}

std::string HttpServer::boundAddress() const {
  if (fd_ < 0) return "";
  sockaddr_in a{};
  socklen_t len = sizeof a;
  if (getsockname(fd_, reinterpret_cast<sockaddr*>(&a), &len) != 0) return "";
  char buf[INET_ADDRSTRLEN] = {};
  inet_ntop(AF_INET, &a.sin_addr, buf, sizeof buf);
  return buf;
}

void HttpServer::closeConn(size_t i) {
  ::close(conns_[i].fd);
  conns_.erase(conns_.begin() + static_cast<long>(i));
}

namespace {
std::string serialize(const HttpServer::Response& r) {
  std::string s = "HTTP/1.1 " + std::to_string(r.status) + " " + httpReason(r.status) + "\r\n";
  if (!r.body.empty() || r.status == 200) s += "Content-Type: " + r.contentType + "\r\n";
  s += "Content-Length: " + std::to_string(r.body.size()) + "\r\nConnection: close\r\nCache-Control: no-store\r\n";
  for (const auto& [k, v] : r.headers) s += k + ": " + v + "\r\n";
  s += "\r\n";
  s += r.body;
  return s;
}
HttpServer::Response simple(int status, const std::string& msg) {
  HttpServer::Response r;
  r.status = status;
  r.body = dump(jobj({{"error", jstr(msg)}}));
  return r;
}
}  // namespace

void HttpServer::poll(const Handler& handler, int timeoutMs) {
  if (fd_ < 0) return;
  std::vector<pollfd> fds;
  fds.push_back({fd_, POLLIN, 0});
  for (const auto& c : conns_) fds.push_back({c.fd, static_cast<short>(c.out.empty() ? POLLIN : POLLOUT), 0});
  if (::poll(fds.data(), static_cast<nfds_t>(fds.size()), timeoutMs) < 0) return;
  // New connections (loopback peers only).
  if (fds[0].revents & POLLIN)
    for (int k = 0; k < 8; ++k) {
      sockaddr_in peer{};
      socklen_t len = sizeof peer;
      const int c = ::accept(fd_, reinterpret_cast<sockaddr*>(&peer), &len);
      if (c < 0) break;
      const bool loop = peer.sin_family == AF_INET && (ntohl(peer.sin_addr.s_addr) >> 24) == 127;
      if (!loop || conns_.size() >= kMaxConnections) {
        ::close(c);
        continue;
      }
      fcntl(c, F_SETFL, fcntl(c, F_GETFL, 0) | O_NONBLOCK);
      fcntl(c, F_SETFD, FD_CLOEXEC);
      noSigPipe(c);
      Conn n;
      n.fd = c;
      n.opened = monoNow();
      conns_.push_back(std::move(n));
    }
  const double now = monoNow();
  for (size_t i = conns_.size(); i-- > 0;) {
    Conn& c = conns_[i];
    const short rev = i + 1 < fds.size() && fds[i + 1].fd == c.fd ? fds[i + 1].revents : 0;
    if (!c.out.empty()) {
      if (rev & (POLLOUT | POLLERR | POLLHUP)) {
        const ssize_t n = ::send(c.fd, c.out.data() + c.sent, c.out.size() - c.sent, kSendFlags);
        if (n > 0) c.sent += static_cast<size_t>(n);
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
          closeConn(i);
          continue;
        }
        if (c.sent >= c.out.size()) {
          closeConn(i);
          continue;
        }
      }
    } else if (rev & (POLLIN | POLLERR | POLLHUP)) {
      char buf[65536];
      const ssize_t n = ::recv(c.fd, buf, sizeof buf, 0);
      if (n <= 0) {
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
          closeConn(i);
          continue;
        }
      } else {
        c.in.append(buf, static_cast<size_t>(n));
      }
      const size_t he = c.in.find("\r\n\r\n");
      Response resp;
      bool answer = false;
      if (he == std::string::npos) {
        if (c.in.size() > kMaxHeader) resp = simple(431, "request headers too large"), answer = true;
      } else {
        Request rq;
        std::string first;
        parseHeaders(c.in.substr(0, he), first, rq.headers);
        std::istringstream fl(first);
        std::string version;
        fl >> rq.method >> rq.path >> version;
        const std::string te = rq.header("transfer-encoding");
        const std::string cl = rq.header("content-length");
        size_t len = 0;
        bool bad = false;
        if (!te.empty()) {
          resp = simple(411, "send a Content-Length (chunked bodies are not supported)"), answer = true, bad = true;
        } else if (!cl.empty()) {
          char* end = nullptr;
          const unsigned long long v = std::strtoull(cl.c_str(), &end, 10);
          if (!end || *end || v > kMaxBody) resp = simple(413, "request body too large"), answer = true, bad = true;
          else len = static_cast<size_t>(v);
        }
        if (!bad && c.in.size() >= he + 4 + len) {
          rq.body = c.in.substr(he + 4, len);
          resp = handler(rq);
          answer = true;
        }
      }
      if (answer) {
        c.in.clear();
        c.out = serialize(resp);
        c.sent = 0;
        continue;
      }
    }
    if (now - c.opened > 30.0 && c.out.empty()) closeConn(i);  // idle / slow client
    else if (now - c.opened > 120.0) closeConn(i);
  }
}

// ============================================================ HTTP client ====

bool httpRequest(int port, const std::string& method, const std::string& path, const std::vector<std::pair<std::string, std::string>>& headers,
                 const std::string& body, HttpReply& reply, std::string* err, int timeoutSeconds) {
  reply = HttpReply();
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    if (err) *err = std::strerror(errno);
    return false;
  }
  noSigPipe(fd);
  timeval tv{};
  tv.tv_sec = timeoutSeconds;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<uint16_t>(port));
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
    if (err) *err = std::string("cannot connect to 127.0.0.1:") + std::to_string(port) + ": " + std::strerror(errno);
    ::close(fd);
    return false;
  }
  std::string req = method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\nConnection: close\r\n";
  for (const auto& [k, v] : headers) req += k + ": " + v + "\r\n";
  req += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
  size_t sent = 0;
  while (sent < req.size()) {
    const ssize_t n = ::send(fd, req.data() + sent, req.size() - sent, kSendFlags);
    if (n <= 0) {
      if (err) *err = "send failed";
      ::close(fd);
      return false;
    }
    sent += static_cast<size_t>(n);
  }
  std::string in;
  char buf[65536];
  for (;;) {
    const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
    if (n <= 0) break;
    in.append(buf, static_cast<size_t>(n));
    if (in.size() > (64u << 20)) break;
  }
  ::close(fd);
  const size_t he = in.find("\r\n\r\n");
  if (he == std::string::npos) {
    if (err) *err = "no reply";
    return false;
  }
  std::string first;
  parseHeaders(in.substr(0, he), first, reply.headers);
  std::istringstream fl(first);
  std::string version;
  fl >> version >> reply.status;
  reply.body = in.substr(he + 4);
  if (auto it = reply.headers.find("content-length"); it != reply.headers.end()) {
    const size_t len = static_cast<size_t>(std::strtoull(it->second.c_str(), nullptr, 10));
    if (reply.body.size() > len) reply.body.resize(len);
  }
  return reply.status > 0;
}

// ========================================================== HTTP endpoint ====

namespace {
bool loopbackHost(const std::string& hostPort) {
  std::string h = lowerStr(hostPort);
  if (!h.empty() && h[0] == '[') {  // [::1]:port
    const size_t e = h.find(']');
    return e != std::string::npos && h.substr(1, e - 1) == "::1";
  }
  const size_t c = h.find(':');
  if (c != std::string::npos) h = h.substr(0, c);
  return h == "127.0.0.1" || h == "localhost";
}
}  // namespace

HttpServer::Response HttpEndpoint::handle(const HttpServer::Request& r) {
  std::string path = r.path.substr(0, r.path.find('?'));
  if (path != "/mcp" && path != "/mcp/") return simple(404, "not found (the MCP endpoint is /mcp)");
  // DNS rebinding: only requests addressed to the loopback host, and no
  // foreign web page origins.
  if (!loopbackHost(r.header("host"))) return simple(403, "forbidden host");
  const std::string origin = lowerStr(r.header("origin"));
  if (!origin.empty() && origin != "null") {
    const std::string o = origin.substr(origin.find("://") == std::string::npos ? 0 : origin.find("://") + 3);
    if (!loopbackHost(o)) return simple(403, "forbidden origin");
  }
  const std::string auth = r.header("authorization");
  if (token_.empty() || auth.size() < 7 || lowerStr(auth.substr(0, 7)) != "bearer " || !constantTimeEqual(trim(auth.substr(7)), token_)) {
    HttpServer::Response resp = simple(401, "missing or wrong token (Settings > Integrations shows it)");
    resp.headers.push_back({"WWW-Authenticate", "Bearer realm=\"gridline\""});
    return resp;
  }
  const std::string pv = r.header("mcp-protocol-version");
  if (!pv.empty()) {
    const auto& sup = supportedProtocols();
    if (std::find(sup.begin(), sup.end(), pv) == sup.end()) return simple(400, "unsupported MCP-Protocol-Version " + pv);
  }
  const std::string sid = r.header("mcp-session-id");
  if (r.method == "DELETE") {
    if (sessions_.erase(sid) == 0) return simple(404, "unknown session");
    HttpServer::Response resp;
    resp.body = "{}";
    return resp;
  }
  if (r.method != "POST") {
    HttpServer::Response resp = simple(405, "use POST (this server does not open event streams)");
    resp.headers.push_back({"Allow", "POST, DELETE"});
    return resp;
  }
  Json msg;
  if (!parse(r.body, msg)) {
    HttpServer::Response resp;
    resp.status = 400;
    resp.body = dump(Session::errorReply(Json(), kParseError, "Parse error"));
    return resp;
  }
  const bool init = msg.is(Json::Type::Object) && msg.text("method") == "initialize";
  Entry* e = nullptr;
  std::string newId;
  if (init) {
    if (sessions_.size() >= kMaxSessions) {  // drop the least recently used
      auto old = std::min_element(sessions_.begin(), sessions_.end(), [](const auto& x, const auto& y) { return x.second.lastUse < y.second.lastUse; });
      sessions_.erase(old);
    }
    newId = randomToken();
    Entry& n = sessions_[newId];
    n.session = std::make_unique<Session>(env_);
    e = &n;
  } else {
    if (sid.empty()) return simple(400, "missing Mcp-Session-Id header (send initialize first)");
    auto it = sessions_.find(sid);
    if (it == sessions_.end()) return simple(404, "unknown or expired session: initialize again");
    e = &it->second;
  }
  e->lastUse = monoNow();
  Json reply = e->session->handleJson(msg);
  HttpServer::Response resp;
  if (!newId.empty()) resp.headers.push_back({"Mcp-Session-Id", newId});
  if (reply.is(Json::Type::Null)) {
    resp.status = 202;
    resp.contentType.clear();
    return resp;
  }
  resp.body = dump(reply);
  return resp;
}

void HttpEndpoint::poll(int timeoutMs) {
  server_.poll([this](const HttpServer::Request& r) { return handle(r); }, timeoutMs);
}

std::vector<std::string> HttpEndpoint::clients() const {
  std::vector<std::string> v;
  for (const auto& [id, e] : sessions_)
    if (e.session->initialized() || !e.session->clientName().empty()) v.push_back(e.session->clientName());
  return v;
}

// ========================================================= endpoint file ====

std::string defaultEndpointFile() { return (fs::path(Settings::defaultPath()).parent_path() / "mcp-endpoint.json").string(); }

bool writeEndpointFile(const std::string& path, int port, const std::string& token, std::string* err) {
  std::error_code ec;
  fs::create_directories(fs::path(path).parent_path(), ec);
  const std::string tmp = path + ".tmp";
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    if (err) *err = "cannot write " + path;
    return false;
  }
  fchmod(fd, 0600);
  const std::string text = dump(jobj({{"port", jnum(port)}, {"token", jstr(token)}, {"pid", jnum(static_cast<double>(::getpid()))}})) + "\n";
  const bool ok = ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
  ::close(fd);
  if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
    if (err) *err = "cannot write " + path;
    std::remove(tmp.c_str());
    return false;
  }
  return true;
}

bool readEndpointFile(const std::string& path, int& port, std::string& token) {
  std::ifstream is(path, std::ios::binary);
  if (!is) return false;
  std::stringstream ss;
  ss << is.rdbuf();
  Json j;
  if (!parse(ss.str(), j, 1 << 16) || !j.is(Json::Type::Object)) return false;
  port = static_cast<int>(j.num("port", 0));
  token = j.text("token");
  return port > 0 && port < 65536 && !token.empty();
}

// ================================================================= stdio ====

namespace {
// Reads one line (without the newline); false at end of input. Longer lines
// than `maxLine` are consumed and flagged.
bool readLine(std::FILE* in, std::string& line, size_t maxLine, bool& tooLong) {
  line.clear();
  tooLong = false;
  int c;
  bool any = false;
  while ((c = std::fgetc(in)) != EOF) {
    any = true;
    if (c == '\n') return true;
    if (line.size() < maxLine) line += static_cast<char>(c);
    else tooLong = true;
  }
  return any;
}
void writeLine(std::FILE* out, const std::string& s) {
  std::fwrite(s.data(), 1, s.size(), out);
  std::fputc('\n', out);
  std::fflush(out);
}
}  // namespace

void serveStdio(Environment& env, std::FILE* in, std::FILE* out, size_t maxLine) {
  Session session(env);
  std::string line;
  bool tooLong = false;
  while (readLine(in, line, maxLine, tooLong)) {
    if (tooLong) {
      writeLine(out, dump(Session::errorReply(Json(), kParseError, "Parse error: message too large")));
      continue;
    }
    if (trim(line).empty()) continue;
    const std::string reply = session.handle(line);
    if (!reply.empty()) writeLine(out, reply);
  }
}

int attachBridge(std::FILE* in, std::FILE* out, const std::string& endpointFile) {
  std::string session, initLine;
  std::string line;
  bool tooLong = false;
  auto post = [&](const std::string& body, HttpReply& rep, std::string& err) {
    int port = 0;
    std::string token;
    if (!readEndpointFile(endpointFile, port, token)) {
      err = "Gridline is not running, or \"Allow AI assistants to control Gridline\" is off (Settings > Integrations)";
      return false;
    }
    std::vector<std::pair<std::string, std::string>> h = {{"Content-Type", "application/json"},
                                                          {"Accept", "application/json, text/event-stream"},
                                                          {"Authorization", "Bearer " + token}};
    if (!session.empty()) h.push_back({"Mcp-Session-Id", session});
    if (!httpRequest(port, "POST", "/mcp", h, body, rep, &err)) {
      err = "Gridline is not reachable (" + err + "): start it and enable Settings > Integrations > \"Allow AI assistants to control Gridline\"";
      return false;
    }
    if (auto it = rep.headers.find("mcp-session-id"); it != rep.headers.end()) session = it->second;
    return true;
  };
  while (readLine(in, line, 8u << 20, tooLong)) {
    if (tooLong) {
      writeLine(out, dump(Session::errorReply(Json(), kParseError, "Parse error: message too large")));
      continue;
    }
    line = trim(line);
    if (line.empty()) continue;
    Json msg;
    const bool parsed = parse(line, msg);
    const Json* idp = parsed && msg.is(Json::Type::Object) ? msg.find("id") : nullptr;
    const Json id = idp ? *idp : Json();
    if (parsed && msg.is(Json::Type::Object) && msg.text("method") == "initialize") {
      session.clear();
      initLine = line;
    }
    HttpReply rep;
    std::string err;
    bool ok = post(line, rep, err);
    if (ok && rep.status == 404 && !initLine.empty() && line != initLine) {  // the app restarted: open a new session
      session.clear();
      HttpReply r2;
      if (post(initLine, r2, err) && r2.status == 200) {
        post(dump(jobj({{"jsonrpc", jstr("2.0")}, {"method", jstr("notifications/initialized")}})), r2, err);
        ok = post(line, rep, err);
      }
    }
    if (!ok || rep.status == 401 || rep.status == 403) {
      if (ok) err = rep.status == 401 ? "Gridline refused the token: the endpoint file is stale, restart the AI integration in Gridline" : "Gridline refused the request";
      if (idp) writeLine(out, dump(Session::errorReply(id, kInternalError, err)));
      continue;
    }
    if (rep.status == 202) continue;
    std::string body = trim(rep.body);
    Json bj;
    if (!body.empty() && parse(body, bj) && (bj.find("jsonrpc") || bj.is(Json::Type::Array))) {
      writeLine(out, dump(bj));
    } else if (idp) {
      Json ej;
      std::string m = parse(body, ej) ? ej.text("error", body) : body;
      writeLine(out, dump(Session::errorReply(id, kInternalError, "Gridline: HTTP " + std::to_string(rep.status) + " " + m)));
    }
  }
  return 0;
}

}  // namespace gl::mcp
