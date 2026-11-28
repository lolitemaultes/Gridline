// Gridline: in-process fake JACK / PipeWire server (K-357).
// Implements the full jack::Api C function table in memory with clients, named ports,
// connection graph, per-port latency ranges, aliases, pretty-name metadata, stepped or
// threaded audio cycles, and fault/event injection (shutdown, rate change, buffer change,
// xrun, freewheel, peer client appearing/leaving).
// Used by cross-platform unit tests and by the app when GRIDLINE_FAKE_JACK=1.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "JackApi.h"

namespace gl::jack {

class FakeServer {
 public:
  static constexpr Nframes kMaxBufferFrames = 16384;

  struct Spec {
    Nframes sampleRate = 48000;
    Nframes bufferSize = 256;
    Nframes captureLatency = 128;
    Nframes playbackLatency = 256;
    bool viaPipeWire = true;
    bool addDefaultPhysical = true;  // system:capture_1/2, system:playback_1/2
    bool threaded = false;           // false = deterministic cycle() calls (tests); true = clock thread (app)
  };

  FakeServer();
  explicit FakeServer(Spec spec);
  ~FakeServer();
  FakeServer(const FakeServer&) = delete;
  FakeServer& operator=(const FakeServer&) = delete;

  // Returns a jack::Api table bound to this server (marks this server active for client_open).
  Api api();
  void makeActive();

  // Server availability (false simulates no daemon running -> client_open fails with kStatusServerFailed).
  void setServerRunning(bool on);
  bool serverRunning() const { return serverRunning_.load(std::memory_order_acquire); }

  // Deterministic audio cycle (zero heap allocations on the audio path).
  void cycle(Nframes frames = 0);

  // Start/stop background clock thread (used when threaded == true, e.g. GRIDLINE_FAKE_JACK=1).
  void startClock();
  void stopClock();

  // Peer client simulation (e.g. a network audio app sending audio into Gridline).
  using SignalFn = std::function<void(float* out, Nframes frames, uint64_t pos)>;
  void addPeerClient(const std::string& clientName, const std::vector<std::string>& outPorts,
                     const std::vector<std::string>& inPorts = {});
  void removePeerClient(const std::string& clientName);
  void setPeerSignal(const std::string& fullPortName, SignalFn fn);
  void setPhysicalLatencies(Nframes capture, Nframes playback);

  // External connection helpers (as if a user patched cables in qpwgraph).
  bool connectPorts(const std::string& srcFull, const std::string& dstFull);
  bool disconnectPorts(const std::string& srcFull, const std::string& dstFull);
  bool isConnected(const std::string& srcFull, const std::string& dstFull) const;

  // Event & fault injections.
  void injectShutdown(int code = kStatusFailure, const std::string& reason = "Server stopped");
  void injectRateChange(Nframes newRate);
  void injectBufferSizeChange(Nframes newBufferSize);
  void injectXrun();
  void injectFreewheel(bool starting);
  void injectLatencyChange();

  // Inspection helpers for tests.
  std::vector<std::string> clientNames() const;
  std::vector<std::string> portNames(const std::string& clientPrefix = {}) const;
  std::vector<std::string> portAliases(const std::string& fullPortName) const;
  std::string portProperty(const std::string& fullPortName, const std::string& key) const;
  LatencyRange portLatency(const std::string& fullPortName, int mode) const;
  std::vector<float> lastBuffer(const std::string& fullPortName, Nframes frames) const;
  Nframes sampleRate() const { return rate_.load(std::memory_order_relaxed); }
  Nframes bufferSize() const { return bufSize_.load(std::memory_order_relaxed); }
  uint64_t cycleCount() const { return cycleCount_.load(std::memory_order_relaxed); }

  // Internal C-ABI entry points (called via the static trampolines in api()).
  struct FakeClient;
  struct FakePort;

  Client* openClient(const char* name, int options, int* status);
  int closeClient(FakeClient* c);
  int activateClient(FakeClient* c);
  int deactivateClient(FakeClient* c);
  Port* registerPort(FakeClient* c, const char* shortName, const char* type, unsigned long flags);
  int unregisterPort(FakeClient* c, FakePort* p);
  const char** getAllConnections(const FakePort* p) const;
  const char** getPorts(const char* namePattern, const char* typePattern, unsigned long flags) const;
  int connectImpl(const char* src, const char* dst);
  int disconnectImpl(const char* src, const char* dst);
  int setBufferSizeImpl(Nframes nframes);
  int recomputeLatenciesImpl();
  int setAliasImpl(FakePort* p, const char* alias);
  int unsetAliasImpl(FakePort* p, const char* alias);
  int setPropertyImpl(Uuid subject, const char* key, const char* value);
  int removePropertyImpl(Uuid subject, const char* key);
  FakePort* portByNameImpl(const char* fullName) const;

 private:
  void createPhysicalPortsLocked();
  std::string uniqueClientNameLocked(const std::string& requested, bool* notUnique) const;
  FakePort* findPortLocked(const std::string& fullName) const;
  FakePort* findPortByUuidLocked(Uuid id) const;
  void recomputeLatenciesLocked();
  void runClock();

  Spec spec_;
  mutable std::mutex mu_;
  std::atomic<bool> serverRunning_{true};
  std::atomic<Nframes> rate_{48000};
  std::atomic<Nframes> bufSize_{256};
  std::atomic<uint64_t> cycleCount_{0};
  std::atomic<uint64_t> framePos_{0};
  std::atomic<bool> clockRunning_{false};
  std::thread clockThread_;

  PortId nextPortId_ = 1;
  Uuid nextUuid_ = 100;
  std::vector<std::unique_ptr<FakeClient>> clients_;
  std::vector<std::unique_ptr<FakePort>> ports_;
  std::vector<std::pair<FakePort*, FakePort*>> edges_;  // (source/output, dest/input)
  std::map<Uuid, std::map<std::string, std::string>> props_;
};

// Shared process-wide fake server used when GRIDLINE_FAKE_JACK=1 in gridline-app:
// comes pre-populated with physical ports and a "Tone" peer client emitting a test signal.
FakeServer& appFakeServer();

}  // namespace gl::jack
