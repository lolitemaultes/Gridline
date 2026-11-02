// Gridline: ASIO audio backend (K-6). Optional on Windows next to the system audio (SDL's WASAPI
// and DirectSound paths, which stay the default); not built into the audio paths of other
// platforms. A simulated driver (same interface, its own thread or stepped by tests) lets the
// host logic run and be tested everywhere; GRIDLINE_FAKE_ASIO=1 lists it in Settings > Audio.
//
// Threads: every Host method runs on the UI thread (ASIO drivers expect the thread that loaded
// them). The driver's bufferSwitch thread only converts samples, calls the render function and
// raises event flags (atomics): no allocation, no locks, no I/O. Resets, resyncs, latency and
// sample rate changes the driver reports are picked up by poll() on the UI thread.
// Licence notes: AsioAbi.h and docs/asio.md.
#pragma once
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "AsioAbi.h"
#include "AsioCore.h"

namespace gl::asio {

// One installed driver.
struct DriverEntry {
  std::string name;   // registry key name (what the settings store)
  std::string clsid;  // "{...}" (empty for simulated drivers)
  bool simulated = false;
};

// Installed drivers: HKEY_LOCAL_MACHINE\SOFTWARE\ASIO on Windows (none elsewhere), plus the
// simulated drivers when GRIDLINE_FAKE_ASIO is set (development, screenshots).
std::vector<DriverEntry> listDrivers();
std::vector<DriverEntry> listInstalledDrivers();  // the registry only
bool backendAvailable();                           // Windows, or the simulated drivers are on

// The calls Gridline makes on a driver (the driver ABI's methods, in Gridline's words).
class Driver {
 public:
  virtual ~Driver() = default;
  virtual bool init(void* sysHandle) = 0;
  virtual std::string errorMessage() = 0;
  virtual abi::Error start() = 0;
  virtual abi::Error stop() = 0;
  virtual abi::Error getChannels(abi::Long* in, abi::Long* out) = 0;
  virtual abi::Error getLatencies(abi::Long* in, abi::Long* out) = 0;
  virtual abi::Error getBufferSize(abi::Long* minSize, abi::Long* maxSize, abi::Long* preferred, abi::Long* granularity) = 0;
  virtual abi::Error canSampleRate(abi::SampleRate rate) = 0;
  virtual abi::Error getSampleRate(abi::SampleRate* rate) = 0;
  virtual abi::Error setSampleRate(abi::SampleRate rate) = 0;
  virtual abi::Error getChannelInfo(abi::ChannelInfo* info) = 0;
  virtual abi::Error createBuffers(abi::BufferInfo* infos, abi::Long count, abi::Long frames, abi::Callbacks* cb) = 0;
  virtual abi::Error disposeBuffers() = 0;
  virtual abi::Error controlPanel() = 0;
  virtual abi::Error outputReady() = 0;
};

// Simulated driver: a device with the given channels, formats and limits. Runs its own clock
// thread when started with `threaded`, or is stepped by step() (tests, deterministic).
struct SimSpec {
  int inputs = 8, outputs = 8;
  BufferCaps buffers{32, 2048, 256, -1};
  std::vector<int> rates{44100, 48000, 88200, 96000};
  double rate = 48000;
  abi::SampleType type = abi::kInt32LSB;
  int inLatencyExtra = 24, outLatencyExtra = 32;  // reported latency = buffer + extra
  bool threaded = true;
  bool supportsOutputReady = true;
  bool present = true;  // false: the device is unplugged (no channels)
  std::string name = "Gridline Simulated Interface";
};
SimSpec simSpecFor(const std::string& name);  // the simulated drivers listDrivers() offers

class SimDriver : public Driver {
 public:
  explicit SimDriver(SimSpec s);
  ~SimDriver() override;
  bool init(void*) override { return true; }
  std::string errorMessage() override { return ""; }
  abi::Error start() override;
  abi::Error stop() override;
  abi::Error getChannels(abi::Long* in, abi::Long* out) override;
  abi::Error getLatencies(abi::Long* in, abi::Long* out) override;
  abi::Error getBufferSize(abi::Long* minSize, abi::Long* maxSize, abi::Long* preferred, abi::Long* granularity) override;
  abi::Error canSampleRate(abi::SampleRate rate) override;
  abi::Error getSampleRate(abi::SampleRate* rate) override;
  abi::Error setSampleRate(abi::SampleRate rate) override;
  abi::Error getChannelInfo(abi::ChannelInfo* info) override;
  abi::Error createBuffers(abi::BufferInfo* infos, abi::Long count, abi::Long frames, abi::Callbacks* cb) override;
  abi::Error disposeBuffers() override;
  abi::Error controlPanel() override;
  abi::Error outputReady() override;

  // Tests: one buffer period (fills the inputs with a ramp, calls bufferSwitch); a message to the
  // host as the driver would send it (reset request, latency change, ...).
  void step();
  long message(abi::Long selector, abi::Long value = 0);
  void changeRate(double rate);  // the clock changed (sampleRateDidChange)
  void setLatencyExtra(int in, int out) { spec_.inLatencyExtra = in, spec_.outLatencyExtra = out; }
  // Device buffers of channel k (after createBuffers), for checking what the host wrote.
  const void* buffer(int k, int half) const;
  int frames() const { return frames_; }
  int created() const { return static_cast<int>(infos_.size()); }
  long outputReadyCalls() const { return outputReadyCalls_.load(); }
  long panelCalls() const { return panelCalls_; }

 private:
  void run();
  SimSpec spec_;
  abi::Callbacks* cb_ = nullptr;
  std::vector<abi::BufferInfo> infos_;
  std::vector<std::vector<unsigned char>> mem_;
  int frames_ = 0;
  int half_ = 0;
  long long pos_ = 0;
  std::atomic<bool> running_{false};
  std::atomic<long> outputReadyCalls_{0};
  long panelCalls_ = 0;
  struct Thread;
  std::unique_ptr<Thread> thread_;
};

// Opens the driver of an entry (COM on Windows, SimDriver for simulated entries).
std::unique_ptr<Driver> loadDriver(const DriverEntry& e, std::string* err);

// Called on the driver's thread for every buffer: `in[k]` = engine input k (device input
// plan.inputs[k]), `out[k]` = device output plan.outputs[k], each `frames` floats.
using RenderFn = void (*)(void* user, const float* const* in, int numIn, float* const* out, int numOut, int frames);

struct Config {
  std::string driver;  // DriverEntry::name
  int buffer = 0;      // 0 = the driver's preferred size
  int rate = 0;        // 0 = the device's current rate
  ChannelRequest channels;
};

// What an open driver offers (filled by open()).
struct DeviceInfo {
  std::string driver;
  int numIn = 0, numOut = 0;
  std::vector<std::string> inNames, outNames;
  std::vector<abi::SampleType> inTypes, outTypes;
  BufferCaps buffers;
  std::vector<int> bufferChoices;
  std::vector<int> rates;  // supported rates among candidateRates()
};

class Host {
 public:
  enum Event : int { None = 0, Reset = 1, Resync = 2, Latency = 4, RateChanged = 8, Stalled = 16, Overload = 32 };

  Host() = default;
  ~Host() { close(); }
  Host(const Host&) = delete;
  Host& operator=(const Host&) = delete;

  // Loads and configures the driver (not started yet: set the engine's rate first).
  bool open(const Config& cfg, void* sysHandle, RenderFn fn, void* user, std::string* err);
  // Same with a driver object (tests).
  bool openDriver(std::unique_ptr<Driver> d, const std::string& name, const Config& cfg, void* sysHandle, RenderFn fn, void* user, std::string* err);
  bool start(std::string* err);
  void close();  // stop, dispose the buffers, release the driver (safe when nothing is open)
  bool isOpen() const { return driver_ != nullptr; }
  bool running() const { return running_; }

  const DeviceInfo& device() const { return dev_; }
  const ChannelPlan& plan() const { return plan_; }
  int bufferFrames() const { return frames_; }
  int sampleRate() const { return rate_; }
  int inputLatency() const { return inLatency_; }
  int outputLatency() const { return outLatency_; }
  bool usesOutputReady() const { return postOutput_; }
  long long callbacks() const { return switches_.load(std::memory_order_relaxed); }
  int overloads() const { return overloads_.load(std::memory_order_relaxed); }

  bool controlPanel();  // the driver's own settings window
  // UI thread, every frame: events since the last poll (Event bits). Latency changes are
  // re-read here; Reset / Resync / RateChanged / Stalled want the caller to reopen.
  int poll();

  // The driver thread's entry points (static: the driver ABI's callbacks carry no context).
  static void onBufferSwitch(abi::Long half, abi::Bool direct);
  static void onRateChanged(abi::SampleRate rate);
  static abi::Long onMessage(abi::Long selector, abi::Long value, void* message, double* opt);
  static abi::Time* onBufferSwitchTimeInfo(abi::Time* t, abi::Long half, abi::Bool direct);

 private:
  void process(int half);
  void refreshLatencies();

  std::unique_ptr<Driver> driver_;
  DeviceInfo dev_;
  ChannelPlan plan_;
  int frames_ = 0, rate_ = 0, inLatency_ = 0, outLatency_ = 0;
  bool created_ = false, running_ = false, postOutput_ = false;
  std::atomic<bool> live_{false};  // the driver thread may process (between start and stop)
  RenderFn fn_ = nullptr;
  void* user_ = nullptr;
  std::vector<abi::BufferInfo> infos_;
  std::vector<abi::SampleType> types_;  // per created buffer
  std::vector<float> scratch_;          // (inputs + outputs) * frames floats
  std::vector<const float*> inPtr_;
  std::vector<float*> outPtr_;
  abi::Callbacks cb_{};
  std::atomic<int> events_{0};
  std::atomic<long long> switches_{0};
  std::atomic<int> overloads_{0};
  long long lastSwitches_ = 0;
  std::chrono::steady_clock::time_point lastProgress_{};
  bool stallReported_ = false;

  static std::atomic<Host*> active_;
};

}  // namespace gl::asio
