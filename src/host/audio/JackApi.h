// Gridline: runtime-loaded JACK / PipeWire C API table (K-357).
// Declares the stable public JACK C ABI constants, opaque handle types and function pointer table
// locally so Gridline compiles and runs with zero JACK development headers or compile-time linking.
// On Linux, loadApi() loads libjack.so.0 (or PipeWire's libjack.so.0) at runtime via dlopen().
#pragma once

#include <cstdint>
#include <string>

namespace gl::jack {

using Nframes = uint32_t;
using DefaultSample = float;
using PortId = uint32_t;
using Uuid = uint64_t;

struct Client;  // opaque jack_client_t
struct Port;    // opaque jack_port_t

// Public JACK C ABI constants (stable across JACK1, JACK2 and pipewire-jack).
inline constexpr int kNullOption = 0x00;
inline constexpr int kNoStartServer = 0x01;
inline constexpr int kUseExactName = 0x02;
inline constexpr int kServerName = 0x04;

inline constexpr int kStatusFailure = 0x01;
inline constexpr int kStatusInvalidOption = 0x02;
inline constexpr int kStatusNameNotUnique = 0x04;
inline constexpr int kStatusServerStarted = 0x08;
inline constexpr int kStatusServerFailed = 0x10;
inline constexpr int kStatusServerError = 0x20;
inline constexpr int kStatusNoSuchClient = 0x40;
inline constexpr int kStatusLoadFailure = 0x80;
inline constexpr int kStatusInitFailure = 0x100;
inline constexpr int kStatusShmFailure = 0x200;
inline constexpr int kStatusVersionError = 0x400;
inline constexpr int kStatusBackendError = 0x800;
inline constexpr int kStatusClientZombie = 0x1000;

inline constexpr unsigned long kPortIsInput = 0x1;
inline constexpr unsigned long kPortIsOutput = 0x2;
inline constexpr unsigned long kPortIsPhysical = 0x4;
inline constexpr unsigned long kPortCanMonitor = 0x8;
inline constexpr unsigned long kPortIsTerminal = 0x10;

inline constexpr int kCaptureLatency = 0;
inline constexpr int kPlaybackLatency = 1;

inline constexpr const char* kDefaultAudioType = "32 bit float mono audio";
inline constexpr const char* kMetadataPrettyName = "http://jackaudio.org/metadata/pretty-name";

struct LatencyRange {
  Nframes min = 0;
  Nframes max = 0;
};

using ProcessCallback = int (*)(Nframes nframes, void* arg);
using BufferSizeCallback = int (*)(Nframes nframes, void* arg);
using SampleRateCallback = int (*)(Nframes nframes, void* arg);
using XrunCallback = int (*)(void* arg);
using FreewheelCallback = void (*)(int starting, void* arg);
using ShutdownCallback = void (*)(void* arg);
using InfoShutdownCallback = void (*)(int code, const char* reason, void* arg);
using LatencyCallback = void (*)(int mode, void* arg);
using ClientRegistrationCallback = void (*)(const char* name, int reg, void* arg);
using PortRegistrationCallback = void (*)(PortId port, int reg, void* arg);
using PortConnectCallback = void (*)(PortId a, PortId b, int connect, void* arg);
using GraphOrderCallback = int (*)(void* arg);

// Function pointer table matching libjack.so.0 (or an in-process fake for tests / dev runs).
struct Api {
  // Required lifecycle & graph functions.
  Client* (*client_open)(const char* client_name, int options, int* status, ...) = nullptr;
  int (*client_close)(Client* client) = nullptr;
  char* (*get_client_name)(Client* client) = nullptr;
  int (*activate)(Client* client) = nullptr;
  int (*deactivate)(Client* client) = nullptr;
  Nframes (*get_sample_rate)(Client* client) = nullptr;
  Nframes (*get_buffer_size)(Client* client) = nullptr;
  int (*set_process_callback)(Client* client, ProcessCallback cb, void* arg) = nullptr;
  int (*set_buffer_size_callback)(Client* client, BufferSizeCallback cb, void* arg) = nullptr;
  int (*set_sample_rate_callback)(Client* client, SampleRateCallback cb, void* arg) = nullptr;
  int (*set_xrun_callback)(Client* client, XrunCallback cb, void* arg) = nullptr;
  void (*on_shutdown)(Client* client, ShutdownCallback cb, void* arg) = nullptr;
  Port* (*port_register)(Client* client, const char* port_name, const char* port_type, unsigned long flags, unsigned long buffer_size) = nullptr;
  int (*port_unregister)(Client* client, Port* port) = nullptr;
  void* (*port_get_buffer)(Port* port, Nframes nframes) = nullptr;
  const char* (*port_name)(const Port* port) = nullptr;
  const char* (*port_short_name)(const Port* port) = nullptr;
  int (*port_flags)(const Port* port) = nullptr;
  Port* (*port_by_name)(Client* client, const char* port_name) = nullptr;
  const char** (*port_get_all_connections)(const Client* client, const Port* port) = nullptr;
  const char** (*get_ports)(Client* client, const char* port_name_pattern, const char* type_name_pattern, unsigned long flags) = nullptr;
  int (*connect)(Client* client, const char* source_port, const char* destination_port) = nullptr;
  int (*disconnect)(Client* client, const char* source_port, const char* destination_port) = nullptr;
  void (*free)(void* ptr) = nullptr;

  // Optional callbacks & extensions (null-checked before every call).
  int (*set_buffer_size)(Client* client, Nframes nframes) = nullptr;
  int (*set_freewheel_callback)(Client* client, FreewheelCallback cb, void* arg) = nullptr;
  void (*on_info_shutdown)(Client* client, InfoShutdownCallback cb, void* arg) = nullptr;
  int (*set_latency_callback)(Client* client, LatencyCallback cb, void* arg) = nullptr;
  int (*set_client_registration_callback)(Client* client, ClientRegistrationCallback cb, void* arg) = nullptr;
  int (*set_port_registration_callback)(Client* client, PortRegistrationCallback cb, void* arg) = nullptr;
  int (*set_port_connect_callback)(Client* client, PortConnectCallback cb, void* arg) = nullptr;
  int (*set_graph_order_callback)(Client* client, GraphOrderCallback cb, void* arg) = nullptr;
  int (*port_set_alias)(Port* port, const char* alias) = nullptr;
  int (*port_unset_alias)(Port* port, const char* alias) = nullptr;
  void (*port_get_latency_range)(Port* port, int mode, LatencyRange* range) = nullptr;
  void (*port_set_latency_range)(Port* port, int mode, LatencyRange* range) = nullptr;
  int (*recompute_total_latencies)(Client* client) = nullptr;
  Uuid (*port_uuid)(const Port* port) = nullptr;
  int (*set_property)(Client* client, Uuid subject, const char* key, const char* value, const char* type) = nullptr;
  int (*remove_property)(Client* client, Uuid subject, const char* key) = nullptr;
  const char* (*get_version_string)() = nullptr;
  Nframes (*frame_time)(const Client* client) = nullptr;

  // Metadata filled by loadApi() or the fake server.
  std::string libraryPath;
  bool viaPipeWire = false;
  bool fake = false;
};

// True when every required function pointer in `api` is populated.
bool apiComplete(const Api& api);

// True when GRIDLINE_FAKE_JACK is set to a non-empty, non-"0" value (dev runs, screenshots, tests).
bool fakeEnabled();

// True when the JACK / PipeWire driver option should be offered in Settings > Audio
// (Linux builds, or any platform with GRIDLINE_FAKE_JACK=1).
bool backendAvailable();

// Loads libjack at runtime via dlopen (Linux only; returns nullptr elsewhere with a readable error).
// Search order when `explicitPath` is empty:
//   1. already-loaded libjack.so.0 if a server answers;
//   2. default libjack.so.0 if a server answers;
//   3. PipeWire's pipewire-0.3/jack/libjack.so.0 in standard distro / Snap library directories;
//   4. default libjack.so.0 (even if no server is running yet, so open() reports "no server").
// When `explicitPath` is non-empty, only that path is tried.
// The returned pointer stays valid for the lifetime of the process.
const Api* loadApi(const std::string& explicitPath, std::string* err);

// Convenience overload using the default search order.
inline const Api* loadApi(std::string* err) { return loadApi(std::string(), err); }

}  // namespace gl::jack
