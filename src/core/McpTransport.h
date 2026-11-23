// Gridline — MCP transports (SC-07).
//
//   * stdio: newline-delimited JSON-RPC on stdin / stdout (`gridline mcp <project>`).
//   * Streamable HTTP inside the app: POST /mcp on 127.0.0.1 only, every
//     request must carry "Authorization: Bearer <token>"; Host and Origin
//     headers must name the loopback host (DNS rebinding). Replies are
//     application/json; a tools/call that runs a background job (J-61) is
//     answered when the job finishes, as a text/event-stream with
//     notifications/progress first when the request carries a progressToken
//     and accepts event streams. GET answers 405. Sessions: initialize
//     returns an Mcp-Session-Id that later requests must send.
//   * attach bridge (`gridline mcp --attach`): a stdio server that forwards
//     every message to the running app's HTTP endpoint, found through the
//     endpoint file the app writes while the server runs (owner-only access).
//
// The HTTP server is polled from the caller's thread (the app's UI loop), so
// tools run on the thread that owns the Document; renders and exports run on
// worker threads (Environment::jobs) and their replies are sent by later polls.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "Mcp.h"
#include "McpListen.h"

namespace gl::mcp {

// 128 random bits as 32 hex characters.
std::string randomToken();
// Compares without an early exit (tokens).
bool constantTimeEqual(const std::string& a, const std::string& b);

// ---- minimal HTTP/1.1 server, loopback unless the listen policy says otherwise (K-264) ----
class HttpServer {
 public:
  static constexpr size_t kMaxHeader = 16u << 10;
  static constexpr size_t kMaxBody = 8u << 20;
  static constexpr size_t kMaxConnections = 32;

  struct Request {
    std::string method, path;
    std::map<std::string, std::string> headers;  // lower-case names
    std::string body;
    uint32_t peer = 0x7F000001u;  // K-264: the client's IPv4 address (host order)
    std::string header(const std::string& name) const {
      auto it = headers.find(name);
      return it == headers.end() ? std::string() : it->second;
    }
  };
  struct Response {
    int status = 200;
    std::string contentType = "application/json";
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    // Streamed / deferred reply (J-61): when set, the fields above are
    // ignored; every poll calls it to append raw bytes (status line and
    // headers first) and it returns true once everything is appended. The
    // connection closes after that.
    std::function<bool(std::string& out)> pump;
  };
  using Handler = std::function<Response(const Request&)>;

  HttpServer() = default;
  ~HttpServer() { stop(); }
  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

  // Listens on 127.0.0.1:port (0 = any free port). K-264: a listen policy beyond loopback
  // (opt-in, Settings > Integrations) binds as it says and accepts only the peers it allows.
  bool start(int port, std::string* err = nullptr, const listen::Policy& policy = {});
  void stop();
  bool running() const { return fd_ >= 0; }
  int port() const { return port_; }
  // The address the socket is bound to ("127.0.0.1").
  std::string boundAddress() const;
  // Accepts, reads and answers; waits at most timeoutMs for activity.
  void poll(const Handler& handler, int timeoutMs = 0);
  size_t connections() const { return conns_.size(); }
  // K-142 (0.7.8 follow-up): a thread of its own (gl-mcp-watch) waits while the server runs for what
  // a poll would take (a client connecting, a request arriving, a reply ready to go out) and calls
  // `knock` from there (the app wakes its interface, which polls the server in its next turn, also
  // while it draws nothing); then it waits for that poll before it looks again. Once started, until
  // stop(). A streamed reply (a job's progress) still needs the poller's own turns.
  bool watch(std::function<void()> knock);
  uint64_t knocks() const { return knocks_.load(std::memory_order_relaxed); }  // (tests)

 private:
  struct Conn {
    std::intptr_t fd = -1;
    std::string in, out;
    size_t sent = 0;
    double opened = 0;
    std::function<bool(std::string&)> pump;  // streamed reply in progress
    bool pumpDone = false;
    uint32_t peer = 0x7F000001u;
  };
  void closeConn(size_t i);
  void stopWatch();
  static short watchEvents(const Conn& c);
  listen::Policy policy_;
  // K-142 (follow-up): the watcher, its wake pair (a byte on wakeW_ ends its wait) and the count of
  // polls it waits on after a knock.
  std::thread watch_;
  std::atomic<bool> watchStop_{false};
  std::intptr_t wakeR_ = -1, wakeW_ = -1;
  std::mutex watchMu_;
  std::condition_variable watchCv_;
  uint64_t polls_ = 0;
  bool watchWaiting_ = false;
  std::vector<std::pair<std::intptr_t, short>> watchConns_;  // the open connections, as the last poll left them
  uint64_t connGen_ = 0, watchGen_ = 0;  // connections opened and closed (a number used again is another one)
  std::atomic<uint64_t> knocks_{0};
  std::intptr_t fd_ = -1;
  int port_ = 0;
  std::vector<Conn> conns_;
};

std::string httpReason(int status);

// Blocking HTTP POST / DELETE to host:port (the attach bridge, tests); host is an IPv4 literal.
struct HttpReply {
  int status = 0;
  std::map<std::string, std::string> headers;  // lower-case names
  std::string body;
};
// `onBody` (optional) receives the body as it arrives (event streams).
bool httpRequest(int port, const std::string& method, const std::string& path, const std::vector<std::pair<std::string, std::string>>& headers,
                 const std::string& body, HttpReply& reply, std::string* err = nullptr, int timeoutSeconds = 900,
                 const std::function<void(const std::string& chunk)>& onBody = nullptr, const std::string& host = "127.0.0.1");
// Splits text/event-stream data into the data payloads of complete events;
// `buffer` keeps an unfinished event between calls.
std::vector<std::string> sseEvents(std::string& buffer, const std::string& chunk);

// ---- MCP over HTTP ------------------------------------------------------------------
class HttpEndpoint {
 public:
  static constexpr size_t kMaxSessions = 16;
  HttpEndpoint(Environment& env, std::string token) : env_(env), token_(std::move(token)) {}
  bool start(int port, std::string* err = nullptr) { return server_.start(port, err, policy_); }
  // K-264: where to listen (takes effect at the next start) and this computer's own addresses
  // (the Host header may name one of them beyond loopback).
  void setListen(const listen::Policy& p, std::vector<uint32_t> ownAddresses) {
    policy_ = p;
    own_ = std::move(ownAddresses);
  }
  const listen::Policy& listenPolicy() const { return policy_; }
  void stop() {
    server_.stop();
    sessions_.clear();
  }
  bool running() const { return server_.running(); }
  int port() const { return server_.port(); }
  const std::string& token() const { return token_; }
  void setToken(std::string t) { token_ = std::move(t); }
  void poll(int timeoutMs = 0);
  bool watch(std::function<void()> knock) { return server_.watch(std::move(knock)); }  // K-142 (follow-up)
  // One request through the endpoint logic (also used by tests without sockets).
  HttpServer::Response handle(const HttpServer::Request& r);
  size_t sessionCount() const { return sessions_.size(); }
  // Client names of the open sessions.
  std::vector<std::string> clients() const;
  const HttpServer& server() const { return server_; }

 private:
  struct Entry {
    std::unique_ptr<Session> session;
    double lastUse = 0;
  };
  Environment& env_;
  std::string token_;
  listen::Policy policy_;
  std::vector<uint32_t> own_;
  listen::AuthThrottle throttle_;
  HttpServer server_;
  std::map<std::string, Entry> sessions_;
};

// Endpoint file: {"port": N, "token": "...", "pid": N, "host": "..."} (mode 0600). K-264: host is
// the address clients use (127.0.0.1 unless the server listens on one chosen address).
std::string defaultEndpointFile();  // <config>/gridline/mcp-endpoint.json
bool writeEndpointFile(const std::string& path, int port, const std::string& token, std::string* err = nullptr,
                       const std::string& host = "127.0.0.1");
bool readEndpointFile(const std::string& path, int& port, std::string& token);
bool readEndpointFile(const std::string& path, int& port, std::string& token, std::string& host);

// ---- stdio ------------------------------------------------------------------------------
// Serves one session on line-framed JSON until `in` ends. Lines longer than
// maxLine are answered with a parse error.
void serveStdio(Environment& env, std::FILE* in, std::FILE* out, size_t maxLine = 8u << 20);
// Forwards line-framed JSON between `in` / `out` and the app's HTTP endpoint
// (read from `endpointFile` for every connection attempt). Returns at end of input.
int attachBridge(std::FILE* in, std::FILE* out, const std::string& endpointFile);

}  // namespace gl::mcp
