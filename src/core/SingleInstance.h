// Gridline — one Gridline per user on Linux, the BSDs and Windows (K-121).
//
// macOS opens a double-clicked file in the Gridline that is running (an open request, see
// app/OpenDocuments.h). Linux and Windows start a new process with the file as its argument
// instead, so the first Gridline listens on a local endpoint that only this user can reach and a
// later launch hands its files over and exits:
//
//   Linux, BSD  a Unix socket, $XDG_RUNTIME_DIR/gridline-<display>.sock (in Flatpak the app's
//               own runtime folder; when there is no private runtime folder, the config folder),
//               mode 0600, and the server only accepts a peer of the same user id;
//   Windows     a named pipe, \\.\pipe\Gridline-<user SID>-<session>, whose access list admits
//               only this user; remote clients are refused, and the client checks that the
//               pipe's server runs as this user too.
//
// The conversation, in bytes: the server says "GRIDLINE 1 <pid>\n"; the client sends
// "OPEN <count>\n" and per file "<byte length>\n<UTF-8 path>"; the server answers "OK\n" once the
// files are queued for the interface thread. A request without files brings the window forward.
//
// A socket left behind by a crash answers no connection: the next launch removes it and takes
// its place (under a lock file, so two launches at once never both take it).
#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace gl::single {

// ---- the conversation (pure, for tests) ----
std::string encodeRequest(const std::vector<std::string>& files);
enum class Parse { NeedMore, Done, Bad };
// Reads a request from the bytes received so far.
Parse decodeRequest(const std::string& in, std::vector<std::string>& files);
inline constexpr size_t kMaxFiles = 1024;
inline constexpr size_t kMaxPathBytes = 32 * 1024;

// ---- where ----
// The endpoint of this user (and display, on Linux): a socket path or a pipe name; "" when none
// can be made (then every launch runs on its own).
std::string defaultEndpoint(const std::string& configDir);
// Tests only: an endpoint of their own, from a name in GRIDLINE_TEST_INSTANCE_ENDPOINT (a socket
// path; on Windows the pipe's name after \\.\pipe\).
std::string testEndpoint(const std::string& value);

// ---- the running Gridline ----
struct Request {
  std::vector<std::string> files;  // empty: come to the front
};

class Server {
 public:
  enum class Claim { Taken, Busy, Failed };
  Server();
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;
  // Takes the endpoint (Taken); Busy when another Gridline answers on it; Failed (err) when it
  // cannot be made.
  Claim claim(const std::string& endpoint, std::string* err = nullptr);
  // Serves requests on its own thread; `wake` runs there after each one is queued.
  void start(std::function<void()> wake);
  // Requests waiting for the interface thread, in the order they came.
  std::vector<Request> take();
  // Stops serving and gives the endpoint up (the socket file is removed when it is still ours).
  void stop();
  bool listening() const { return claimed_; }
  const std::string& endpoint() const { return endpoint_; }

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
  std::string endpoint_;
  bool claimed_ = false;
  std::thread thread_;
  std::mutex mu_;
  std::vector<Request> queue_;
  std::function<void()> wake_;
  void serve();
  void push(Request r);
};

// ---- a later launch ----
enum class Handover { Sent, NoServer, Failed };
// Hands `files` (absolute paths) to the Gridline listening on `endpoint`. NoServer: nobody
// listens (the caller may take the endpoint); Failed: it did not answer in time or is not this
// user's (the caller runs on its own).
Handover handOver(const std::string& endpoint, const std::vector<std::string>& files, std::string* err = nullptr,
                  int timeoutMs = 4000);

}  // namespace gl::single
