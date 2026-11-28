// Gridline: JACK / PipeWire audio host (K-357).
// Threading model (mirroring AsioHost.h):
//   - Every Host method runs on the UI thread (port registration, aliases, connections, poll, close).
//   - The server's RT process callback (onProcess) calls ONLY jack_port_get_buffer on the fixed port
//     table and invokes the shared gl::audio::RenderFn: zero heap allocations, zero locks, zero I/O.
//   - Server notification callbacks (shutdown, rate, buffer, latency, xrun, freewheel, registration,
//     connect, graph order) only touch atomics (plus a fixed char buffer on shutdown).
//   - RetryWorker probes for a returning server off the UI thread when JACK stopped.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "DeviceRender.h"
#include "JackApi.h"
#include "JackCore.h"

namespace gl::jack {

struct Config {
  int inputs = 2;              // 0..kMaxInputs
  int extraPairs = 0;          // 0..kMaxExtraPairs
  std::string clientName = "Gridline";
  int requestFrames = 0;       // 0 = server default
  std::string library;         // optional explicit libjack path
  bool autoConnectOutputs = true;
  bool autoConnectInputs = false;
};

struct PortInfo {
  std::string shortName;
  std::string fullName;
  std::string alias;
  bool input = false;
  int channel = 0;
  std::vector<std::string> connected;
};

class Host {
 public:
  enum Event : int {
    None = 0,
    Shutdown = 1,
    RateChanged = 2,
    BufferChanged = 4,
    Latency = 8,
    Graph = 16,
    ClientAppeared = 32,
    Xrun = 64,
    Freewheel = 128,
  };

  Host() = default;
  ~Host() { close(); }
  Host(const Host&) = delete;
  Host& operator=(const Host&) = delete;

  // Opens a client and registers the planned ports (not activated yet: caller sets engine rate first).
  bool open(const Api& api, const Config& cfg, audio::RenderFn fn, void* user, std::string* err);
  // Activates the client so the server starts calling the process callback.
  bool activate(std::string* err);
  // Safe to call when closed or after a server shutdown.
  void close();

  bool isOpen() const { return client_ != nullptr; }
  bool active() const { return active_; }
  bool viaPipeWire() const { return api_ && api_->viaPipeWire; }
  bool isFake() const { return api_ && api_->fake; }
  bool freewheeling() const { return freewheel_.load(std::memory_order_relaxed); }
  const std::atomic<bool>& freewheelFlag() const { return freewheel_; }

  const std::string& clientName() const { return clientName_; }
  const PortPlan& plan() const { return plan_; }
  int sampleRate() const { return rate_; }
  int bufferFrames() const { return bufferFrames_; }
  int inputLatency() const { return inLatency_; }
  int outputLatency() const { return outLatency_; }
  long long cycles() const { return cycles_.load(std::memory_order_relaxed); }
  int xruns() const { return xruns_.load(std::memory_order_relaxed); }
  std::string shutdownReason() const;

  // UI thread: consumes pending event bits from server callbacks and refreshes cached rate/buffer/latency.
  int poll();

  // UI thread: updates the engine's plugin delay compensation reported on our playback ports.
  void setEngineOutputLatency(int frames);
  // UI thread: reads capture/playback latency ranges of our connected ports.
  void refreshLatencies();

  // UI thread: graph inspection and patching.
  std::vector<PortInfo> queryPorts() const;
  std::vector<std::string> listExternalPorts(bool wantOutputs) const;
  bool connect(const std::string& ownShortName, const std::string& peerFullName);
  bool disconnect(const std::string& ownShortName, const std::string& peerFullName);
  std::vector<Link> snapshotLinks() const;
  int restoreLinks(const std::vector<Link>& saved);
  void autoConnect(bool connectOutputs, bool connectInputs);

  // UI thread: sets a port's alias and JACK pretty-name metadata.
  bool setPortAlias(const std::string& ownShortName, const std::string& newAlias);
  // UI thread: updates input port aliases to follow track names ("In 1 (Vocal)").
  void updateInputAliases(const std::vector<TrackInputRef>& tracks);

 private:
  static int onProcess(Nframes nframes, void* arg);
  static int onBufferSize(Nframes nframes, void* arg);
  static int onSampleRate(Nframes nframes, void* arg);
  static int onXrun(void* arg);
  static void onFreewheel(int starting, void* arg);
  static void onShutdown(void* arg);
  static void onInfoShutdown(int code, const char* reason, void* arg);
  static void onLatency(int mode, void* arg);
  static void onClientRegistration(const char* name, int reg, void* arg);
  static void onPortRegistration(PortId port, int reg, void* arg);
  static void onPortConnect(PortId a, PortId b, int connect, void* arg);
  static int onGraphOrder(void* arg);

  Port* findOwnPort(const std::string& shortName, bool* isInput = nullptr) const;
  void applyAliasAndMeta(Port* port, std::string& currentAlias, const std::string& nextAlias);

  const Api* api_ = nullptr;
  Client* client_ = nullptr;
  std::string clientName_;
  PortPlan plan_;

  // Fixed while active: read on RT thread without locks.
  std::vector<Port*> inPort_;
  std::vector<Port*> outPort_;
  std::vector<std::string> inAlias_;
  std::vector<std::string> outAlias_;

  audio::RenderFn fn_ = nullptr;
  void* user_ = nullptr;

  int rate_ = 0;
  int bufferFrames_ = 0;
  int inLatency_ = 0;
  int outLatency_ = 0;
  bool active_ = false;
  bool serverShutDown_ = false;

  std::atomic<bool> live_{false};
  std::atomic<bool> freewheel_{false};
  std::atomic<int> events_{0};
  std::atomic<int> xruns_{0};
  std::atomic<long long> cycles_{0};
  std::atomic<int> pendingRate_{0};
  std::atomic<int> pendingBuffer_{0};
  std::atomic<int> engineOutLatency_{0};
  std::atomic<int> rawPlayLatency_{0};

  // Fixed-size shutdown reason written once on server shutdown (no heap allocation in callback).
  std::atomic<bool> reasonReady_{false};
  char shutdownReason_[256] = {};
};

// Background probe worker used after a server shutdown to detect when JACK / PipeWire returns
// (never calls jack_client_open from the UI thread).
class RetryWorker {
 public:
  RetryWorker() = default;
  ~RetryWorker() { stop(); }
  RetryWorker(const RetryWorker&) = delete;
  RetryWorker& operator=(const RetryWorker&) = delete;

  void start(const Api* api, int intervalMs = 2000);
  void stop();
  bool running() const { return running_.load(std::memory_order_acquire); }
  bool takeServerBack() { return serverBack_.exchange(false, std::memory_order_acq_rel); }

 private:
  void run(const Api* api, int intervalMs);
  std::atomic<bool> running_{false};
  std::atomic<bool> serverBack_{false};
  std::mutex mu_;
  std::condition_variable cv_;
  std::thread thread_;
};

}  // namespace gl::jack
