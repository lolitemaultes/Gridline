// Gridline: the OSC server. One network thread receives OSC over UDP (and, when switched on, over
// TCP on the same port, framed with SLIP or with a 32-bit length per packet, detected per
// connection), parses each packet with the codec (Osc.h) and queues the messages for the UI thread,
// which applies them to the project (OscControl.h). Nothing here touches the audio thread.
//
// It listens on 127.0.0.1 unless the network is switched on; then it listens on every interface
// and takes packets only from the allowed hosts (loopback is always allowed). The queue is bounded:
// when the UI thread falls behind, extra messages are dropped and counted.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "Osc.h"

namespace gl {

struct OscServerConfig {
  int port = 9000;           // 0 = any free port (tests)
  bool network = false;      // false: this computer only (127.0.0.1)
  bool tcp = false;          // also accept OSC over TCP on the same port
  std::string allowedHosts;  // network on: IPv4 addresses or a.b.c.d/nn ranges, comma or space separated; empty = any
};

struct OscIncoming {
  osc::Message msg;
  uint64_t time = osc::kImmediately;  // the bundle's time tag
  uint32_t ip = 0;                    // sender, host byte order
  uint16_t port = 0;
  bool tcp = false;
};

class OscServer {
 public:
  static constexpr size_t kMaxQueue = 4096;
  static constexpr size_t kMaxTcpClients = 16;

  struct Status {
    std::string lastAddress;  // last message received (osc::describe)
    std::string lastFrom;     // "127.0.0.1:53000"
    double lastTime = -1;     // steady seconds (OscServer::clock) of the last message, -1 = none yet
    uint64_t received = 0, dropped = 0, malformed = 0, refused = 0;
    std::string lastProblem;  // why the last bad packet was refused
    int tcpClients = 0;
  };

  OscServer();
  ~OscServer();
  OscServer(const OscServer&) = delete;
  OscServer& operator=(const OscServer&) = delete;

  // Binds and starts the network thread. False with a message ("Port 9000 is in use ...").
  bool start(const OscServerConfig& cfg, std::string* err = nullptr);
  void stop();
  bool running() const { return thread_.joinable(); }
  int port() const { return port_; }
  const OscServerConfig& config() const { return cfg_; }
  // Messages received since the last call (UI thread).
  void poll(std::vector<OscIncoming>& out);
  Status status() const;
  // One UDP packet to ip:port (feedback, the test button). From the server's own port while it
  // runs, else from a socket of its own. Never blocks.
  bool sendTo(uint32_t ip, int port, const std::vector<uint8_t>& packet, std::string* err = nullptr);

  static double clock();  // steady seconds
  // "127.0.0.1", "localhost" -> address in host byte order.
  static bool parseIPv4(const std::string& text, uint32_t& ip);
  static std::string ipText(uint32_t ip);
  // Checks an allowed hosts list; `bad` gets the first entry that is not an address or range.
  static bool validHostList(const std::string& list, std::string* bad = nullptr);
  // True when `ip` may send: loopback always, else an empty list or a matching entry.
  static bool hostAllowed(const std::string& list, uint32_t ip);

 private:
  struct Conn;
  void loop();
  void packet(const uint8_t* data, size_t n, uint32_t ip, uint16_t port, bool tcp);
  bool allowed(uint32_t ip) const;

  OscServerConfig cfg_;
  std::vector<std::pair<uint32_t, uint32_t>> allow_;  // (address, mask)
  std::intptr_t udp_ = -1, tcp_ = -1, send_ = -1;
  int port_ = 0;
  std::thread thread_;
  std::atomic<bool> quit_{false};
  mutable std::mutex mu_;  // queue_ and status_
  std::vector<OscIncoming> queue_;
  Status status_;
  std::vector<uint8_t> buf_;  // receive buffer (network thread)
  std::vector<std::unique_ptr<Conn>> conns_;
  std::vector<osc::Timed> parsed_;
};

}  // namespace gl
