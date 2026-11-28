// Gridline — K-95: Low-latency Windows WASAPI Exclusive backend, sample format
// negotiation/packing, exact latency math, and cross-platform backend status /
// automatic fallback reporting.
//
// Threading & real-time safety:
//   * open(), start(), close(), listOutputDevices() run on the UI thread.
//   * The WASAPI event-driven render thread runs with MMCSS "Pro Audio"
//     characteristics and performs zero allocations, zero locks, and zero I/O:
//     all scratch buffers are sized in open() before the thread starts.
//   * All format negotiation, packing, latency math, HRESULT classification,
//     and exclusive->shared fallback state-machine helpers are platform-
//     independent and covered by cross-platform unit tests.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gl::wasapi {

// 100-nanosecond REFERENCE_TIME units per second (WASAPI / Windows Multimedia).
inline constexpr int64_t kHnsPerSecond = 10'000'000;

enum class SampleFormat : uint8_t {
  Float32 = 0,   // 32-bit IEEE float (4 bytes/sample)
  Pcm24In32,     // 24-bit valid signed PCM in MSB of 32-bit container (4 bytes/sample)
  Pcm24,         // 24-bit signed packed PCM little-endian (3 bytes/sample)
  Pcm32,         // 32-bit signed PCM little-endian (4 bytes/sample)
  Pcm16,         // 16-bit signed PCM little-endian (2 bytes/sample)
};

int formatValidBits(SampleFormat fmt);
int formatContainerBytes(SampleFormat fmt);
const char* formatName(SampleFormat fmt);

// Converts planar stereo float buffers [-1, 1] into interleaved output samples.
// Out-of-range values are clamped; NaN / Inf write silence. Audio-thread safe
// (zero allocations, zero locks). Channels > 2 are zero-filled beyond L/R.
void packFromFloat(const float* inL, const float* inR, int frames, SampleFormat fmt, int channels, void* dst);

// Symmetric unpacker (interleaved output buffer -> planar stereo float) for
// round-trip verification and tests.
void unpackToFloat(const void* src, SampleFormat fmt, int channels, int frames, float* outL, float* outR);

// ---- format negotiation ----
struct FormatRequest {
  int sampleRate = 48000;
  int channels = 2;
  int bufferFrames = 0;  // 0 = device minimum exclusive period
};

struct FormatCandidate {
  SampleFormat format = SampleFormat::Float32;
  int sampleRate = 48000;
  int channels = 2;
};

struct NegotiatedFormat {
  SampleFormat format = SampleFormat::Float32;
  int sampleRate = 48000;
  int channels = 2;
  int periodFrames = 0;
  bool exactRate = false;
};

// Probes supported formats in priority order:
//   Float32 -> Pcm24In32 -> Pcm24 -> Pcm32 -> Pcm16
// trying the requested sample rate first, then standard fallback rates
// (48000, 44100, 96000, 88200). Returns false if the device accepts none.
bool negotiateFormat(const FormatRequest& req, const std::function<bool(const FormatCandidate&)>& probe, NegotiatedFormat& out);

// ---- period & latency math ----
struct PeriodInfo {
  int64_t defaultPeriodHns = 0;
  int64_t minPeriodHns = 0;
};

int hnsToFrames(int64_t hns, int sampleRate);
int64_t framesToHns(int frames, int sampleRate);

// Chooses the exclusive period in 100-ns units: requestedFrames clamped to
// >= minPeriodHns (or minPeriodHns / defaultPeriodHns when requestedFrames <= 0).
int64_t chooseExclusivePeriodHns(int requestedFrames, int sampleRate, const PeriodInfo& period);

struct LatencyInfo {
  int bufferFrames = 0;
  int streamLatencyFrames = 0;
  int totalOutputFrames = 0;
  int inputFrames = 0;
  double outputMs = 0.0;
  double roundTripMs = 0.0;
};

// Computes exact output & round-trip latency in frames and milliseconds.
// Event-driven exclusive mode double-buffers (2 * bufferFrames) plus any
// driver-reported hardware stream latency (streamLatencyHns).
LatencyInfo computeLatency(int bufferFrames, int64_t streamLatencyHns, int sampleRate, bool exclusiveEventDriven, int inputFrames = 0);

// ---- failure classification & automatic fallback state machine ----
enum class OpenFailure : uint8_t {
  None = 0,
  NotAvailableOnPlatform,
  NoDevice,
  DeviceInUse,
  ExclusiveNotAllowed,
  FormatUnsupported,
  BufferInvalid,
  DeviceInvalidated,
  StreamInitFailed,
  ComError,
};

enum class ActiveMode : uint8_t {
  SdlShared = 0,
  WasapiExclusive,
  WasapiSharedFallback,
  Asio,
};

// Cross-platform audio backend status & latency readout model (displayed in
// Settings > Audio on all platforms).
struct BackendStatus {
  ActiveMode mode = ActiveMode::SdlShared;
  std::string backendName;     // e.g. "WASAPI Exclusive", "WASAPI Shared (SDL)", "CoreAudio (SDL)", "ASIO"
  std::string deviceName;      // active device name or "System default"
  std::string sampleFormat;    // e.g. "32-bit float", "24-bit PCM", "16-bit PCM"
  int sampleRate = 44100;
  int bufferFrames = 256;
  int outputLatencyFrames = 0;
  int inputLatencyFrames = 0;
  double outputLatencyMs = 0.0;
  double roundTripLatencyMs = 0.0;
  bool exclusiveRequested = false;
  bool exclusiveActive = false;
  bool fellBack = false;
  OpenFailure fallbackCode = OpenFailure::None;
  std::string fallbackReason;  // non-empty human-readable reason when exclusive fell back
};

// Maps a Windows HRESULT (uint32_t so tests run on every OS) to OpenFailure.
OpenFailure classifyHResult(uint32_t hresult);

// Human-readable reason explaining why WASAPI Exclusive could not open and why
// Gridline fell back to shared/system audio.
std::string describeFailure(OpenFailure code, const std::string& detail = {});

// Builds a BackendStatus for shared/system audio (or automatic fallback after
// an exclusive open failure).
BackendStatus resolveFallbackStatus(bool exclusiveRequested, OpenFailure exclusiveFailure, const std::string& exclusiveDetail,
                                    const std::string& fallbackBackendName, const std::string& deviceName, int sampleRate, int bufferFrames,
                                    int inputLatencyFrames = 0);

// ---- WASAPI Exclusive audio host ----
using RenderFn = void (*)(void* user, float* outL, float* outR, int frames);

struct Config {
  std::string deviceName;  // "" = system default render endpoint
  int sampleRate = 48000;
  int bufferFrames = 0;    // 0 = device minimum period
  int channels = 2;
};

class ExclusiveHost {
 public:
  ExclusiveHost();
  ~ExclusiveHost();
  ExclusiveHost(const ExclusiveHost&) = delete;
  ExclusiveHost& operator=(const ExclusiveHost&) = delete;

  // True on Windows, or when GRIDLINE_FAKE_WASAPI=1 (development / layout checks).
  static bool backendAvailable();
  // Active render endpoint friendly names on Windows (empty elsewhere).
  static std::vector<std::string> listOutputDevices();

  // Opens and configures the device in WASAPI Exclusive event-driven mode
  // (not streaming yet: set engine sample rate first, then call start()).
  bool open(const Config& cfg, RenderFn fn, void* user, OpenFailure* failOut = nullptr, std::string* errOut = nullptr);
  bool start(std::string* errOut = nullptr);
  void close();

  bool isOpen() const;
  bool running() const;
  bool deviceLost() const;  // true if AUDCLNT_E_DEVICE_INVALIDATED occurred on the audio thread
  const BackendStatus& status() const { return status_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  BackendStatus status_;
};

}  // namespace gl::wasapi
