// Gridline — MCP transports (SC-07).
//
//   * stdio: newline-delimited JSON-RPC on stdin / stdout (`gridline mcp <project>`).
//   * Streamable HTTP inside the app: POST /mcp on 127.0.0.1 only, every
//     request must carry "Authorization: Bearer <token>"; Host and Origin
//     headers must name the loopback host (DNS rebinding). Replies are plain
//     application/json (no server-sent events); GET answers 405. Sessions:
//     initialize returns an Mcp-Session-Id that later requests must send.
//   * attach bridge (`gridline mcp --attach`): a stdio server that forwards
//     every message to the running app's HTTP endpoint, found through the
//     endpoint file the app writes while the server runs (owner-only access).
//
// The HTTP server is polled from the caller's thread (the app's UI loop), so
// tools run on the thread that owns the Document.
#pragma once
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "Mcp.h"

namespace gl::mcp {

// 128 random bits as 32 hex characters.
std::string randomToken();
// Compares without an early exit (tokens).
bool constantTimeEqual(const std::string& a, const std::string& b);

// ---- minimal HTTP/1.1 server, loopback only --------------------------------------
class HttpServer {
 public:
  static constexpr size_t kMaxHeader = 16u << 10;
  static constexpr size_t kMaxBody = 8u << 20;
  static constexpr size_t kMaxConnections = 32;

  struct Request {
    std::string method, path;
    std::map<std::string, std::string> headers;  // lower-case names
    std::string body;
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
  };
  using Handler = std::function<Response(const Request&)>;

  HttpServer() = default;
  ~HttpServer() { stop(); }
  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

  // Listens on 127.0.0.1:port (0 = any free port). There is no way to bind
  // another address.
  bool start(int port, std::string* err = nullptr);
  void stop();
  bool running() const { return fd_ >= 0; }
  int port() const { return port_; }
  // The address the socket is bound to ("127.0.0.1").
  std::string boundAddress() const;
  // Accepts, reads and answers; waits at most timeoutMs for activity.
  void poll(const Handler& handler, int timeoutMs = 0);
  size_t connections() const { return conns_.size(); }

 private:
  struct Conn {
    int fd = -1;
    std::string in, out;
    size_t sent = 0;
    double opened = 0;
  };
  void closeConn(size_t i);
  int fd_ = -1, port_ = 0;
  std::vector<Conn> conns_;
};

std::string httpReason(int status);

// Blocking HTTP POST / DELETE to 127.0.0.1:port (the attach bridge, tests).
struct HttpReply {
  int status = 0;
  std::map<std::string, std::string> headers;  // lower-case names
  std::string body;
};
bool httpRequest(int port, const std::string& method, const std::string& path, const std::vector<std::pair<std::string, std::string>>& headers,
                 const std::string& body, HttpReply& reply, std::string* err = nullptr, int timeoutSeconds = 900);

// ---- MCP over HTTP ------------------------------------------------------------------
class HttpEndpoint {
 public:
  static constexpr size_t kMaxSessions = 16;
  HttpEndpoint(Environment& env, std::string token) : env_(env), token_(std::move(token)) {}
  bool start(int port, std::string* err = nullptr) { return server_.start(port, err); }
  void stop() {
    server_.stop();
    sessions_.clear();
  }
  bool running() const { return server_.running(); }
  int port() const { return server_.port(); }
  const std::string& token() const { return token_; }
  void setToken(std::string t) { token_ = std::move(t); }
  void poll(int timeoutMs = 0);
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
  HttpServer server_;
  std::map<std::string, Entry> sessions_;
};

// Endpoint file: {"port": N, "token": "...", "pid": N} (mode 0600).
std::string defaultEndpointFile();  // <config>/gridline/mcp-endpoint.json
bool writeEndpointFile(const std::string& path, int port, const std::string& token, std::string* err = nullptr);
bool readEndpointFile(const std::string& path, int& port, std::string& token);

// ---- stdio ------------------------------------------------------------------------------
// Serves one session on line-framed JSON until `in` ends. Lines longer than
// maxLine are answered with a parse error.
void serveStdio(Environment& env, std::FILE* in, std::FILE* out, size_t maxLine = 8u << 20);
// Forwards line-framed JSON between `in` / `out` and the app's HTTP endpoint
// (read from `endpointFile` for every connection attempt). Returns at end of input.
int attachBridge(std::FILE* in, std::FILE* out, const std::string& endpointFile);

}  // namespace gl::mcp
