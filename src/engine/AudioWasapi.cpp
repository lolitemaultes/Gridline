// Gridline — K-95: WASAPI Exclusive backend implementation. See AudioWasapi.h.
#include "AudioWasapi.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "../core/ThreadName.h"

#ifdef _WIN32
#include <windows.h>
#include <initguid.h>  // defines PKEY_Device_FriendlyName and the KSDATAFORMAT GUIDs used here
#include <audioclient.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <objbase.h>
#endif

namespace gl::wasapi {

namespace {

inline float sanitizeClamp(float v) {
  if (!std::isfinite(v)) return 0.0f;
  return std::clamp(v, -1.0f, 1.0f);
}

inline int32_t floatToPcm32(float v) {
  const double d = static_cast<double>(sanitizeClamp(v)) * 2147483648.0;
  if (d >= 2147483647.0) return 2147483647;
  if (d <= -2147483648.0) return static_cast<int32_t>(-2147483647 - 1);
  return static_cast<int32_t>(std::llround(d));
}

inline int32_t floatToPcm24(float v) {
  const double d = static_cast<double>(sanitizeClamp(v)) * 8388608.0;
  if (d >= 8388607.0) return 8388607;
  if (d <= -8388608.0) return -8388608;
  return static_cast<int32_t>(std::llround(d));
}

inline int16_t floatToPcm16(float v) {
  const double d = static_cast<double>(sanitizeClamp(v)) * 32768.0;
  if (d >= 32767.0) return 32767;
  if (d <= -32768.0) return -32768;
  return static_cast<int16_t>(std::llround(d));
}

inline void writeSample(unsigned char* dst, SampleFormat fmt, float v) {
  switch (fmt) {
    case SampleFormat::Float32: {
      const float f = std::isfinite(v) ? v : 0.0f;
      std::memcpy(dst, &f, 4);
      break;
    }
    case SampleFormat::Pcm32: {
      const int32_t s = floatToPcm32(v);
      const uint32_t u = static_cast<uint32_t>(s);
      dst[0] = static_cast<unsigned char>(u & 0xFFu);
      dst[1] = static_cast<unsigned char>((u >> 8) & 0xFFu);
      dst[2] = static_cast<unsigned char>((u >> 16) & 0xFFu);
      dst[3] = static_cast<unsigned char>((u >> 24) & 0xFFu);
      break;
    }
    case SampleFormat::Pcm24In32: {
      const int32_t s = floatToPcm24(v);
      const uint32_t u = static_cast<uint32_t>(s) << 8;
      dst[0] = static_cast<unsigned char>(u & 0xFFu);
      dst[1] = static_cast<unsigned char>((u >> 8) & 0xFFu);
      dst[2] = static_cast<unsigned char>((u >> 16) & 0xFFu);
      dst[3] = static_cast<unsigned char>((u >> 24) & 0xFFu);
      break;
    }
    case SampleFormat::Pcm24: {
      const int32_t s = floatToPcm24(v);
      const uint32_t u = static_cast<uint32_t>(s);
      dst[0] = static_cast<unsigned char>(u & 0xFFu);
      dst[1] = static_cast<unsigned char>((u >> 8) & 0xFFu);
      dst[2] = static_cast<unsigned char>((u >> 16) & 0xFFu);
      break;
    }
    case SampleFormat::Pcm16: {
      const int16_t s = floatToPcm16(v);
      const uint16_t u = static_cast<uint16_t>(s);
      dst[0] = static_cast<unsigned char>(u & 0xFFu);
      dst[1] = static_cast<unsigned char>((u >> 8) & 0xFFu);
      break;
    }
  }
}

inline float readSample(const unsigned char* src, SampleFormat fmt) {
  switch (fmt) {
    case SampleFormat::Float32: {
      float f = 0.0f;
      std::memcpy(&f, src, 4);
      return f;
    }
    case SampleFormat::Pcm32: {
      const uint32_t u = static_cast<uint32_t>(src[0]) | (static_cast<uint32_t>(src[1]) << 8) | (static_cast<uint32_t>(src[2]) << 16) |
                         (static_cast<uint32_t>(src[3]) << 24);
      return static_cast<float>(static_cast<double>(static_cast<int32_t>(u)) / 2147483648.0);
    }
    case SampleFormat::Pcm24In32: {
      const uint32_t u = static_cast<uint32_t>(src[0]) | (static_cast<uint32_t>(src[1]) << 8) | (static_cast<uint32_t>(src[2]) << 16) |
                         (static_cast<uint32_t>(src[3]) << 24);
      const int32_t s = static_cast<int32_t>(u) >> 8;
      return static_cast<float>(static_cast<double>(s) / 8388608.0);
    }
    case SampleFormat::Pcm24: {
      uint32_t u = static_cast<uint32_t>(src[0]) | (static_cast<uint32_t>(src[1]) << 8) | (static_cast<uint32_t>(src[2]) << 16);
      if (u & 0x00800000u) u |= 0xFF000000u;
      return static_cast<float>(static_cast<double>(static_cast<int32_t>(u)) / 8388608.0);
    }
    case SampleFormat::Pcm16: {
      const uint16_t u = static_cast<uint16_t>(src[0]) | static_cast<uint16_t>(static_cast<uint16_t>(src[1]) << 8);
      return static_cast<float>(static_cast<double>(static_cast<int16_t>(u)) / 32768.0);
    }
  }
  return 0.0f;
}

}  // namespace

int formatValidBits(SampleFormat fmt) {
  switch (fmt) {
    case SampleFormat::Float32: return 32;
    case SampleFormat::Pcm24In32: return 24;
    case SampleFormat::Pcm24: return 24;
    case SampleFormat::Pcm32: return 32;
    case SampleFormat::Pcm16: return 16;
  }
  return 0;
}

int formatContainerBytes(SampleFormat fmt) {
  switch (fmt) {
    case SampleFormat::Float32: return 4;
    case SampleFormat::Pcm24In32: return 4;
    case SampleFormat::Pcm24: return 3;
    case SampleFormat::Pcm32: return 4;
    case SampleFormat::Pcm16: return 2;
  }
  return 0;
}

const char* formatName(SampleFormat fmt) {
  switch (fmt) {
    case SampleFormat::Float32: return "32-bit float";
    case SampleFormat::Pcm24In32: return "24-bit PCM (32-bit container)";
    case SampleFormat::Pcm24: return "24-bit PCM";
    case SampleFormat::Pcm32: return "32-bit PCM";
    case SampleFormat::Pcm16: return "16-bit PCM";
  }
  return "Unknown";
}

void packFromFloat(const float* inL, const float* inR, int frames, SampleFormat fmt, int channels, void* dst) {
  if (!dst || frames <= 0 || channels <= 0) return;
  const int bps = formatContainerBytes(fmt);
  auto* out = static_cast<unsigned char*>(dst);
  for (int i = 0; i < frames; ++i) {
    const float l = inL ? inL[i] : 0.0f;
    const float r = inR ? inR[i] : l;
    if (channels == 1) {
      writeSample(out, fmt, 0.5f * (l + r));
      out += bps;
    } else {
      writeSample(out, fmt, l);
      out += bps;
      writeSample(out, fmt, r);
      out += bps;
      for (int c = 2; c < channels; ++c) {
        writeSample(out, fmt, 0.0f);
        out += bps;
      }
    }
  }
}

void unpackToFloat(const void* src, SampleFormat fmt, int channels, int frames, float* outL, float* outR) {
  if (!src || frames <= 0 || channels <= 0) return;
  const int bps = formatContainerBytes(fmt);
  const auto* in = static_cast<const unsigned char*>(src);
  for (int i = 0; i < frames; ++i) {
    const float l = readSample(in, fmt);
    const float r = channels >= 2 ? readSample(in + bps, fmt) : l;
    if (outL) outL[i] = l;
    if (outR) outR[i] = r;
    in += static_cast<size_t>(bps) * static_cast<size_t>(channels);
  }
}

bool negotiateFormat(const FormatRequest& req, const std::function<bool(const FormatCandidate&)>& probe, NegotiatedFormat& out) {
  if (!probe || req.channels <= 0) return false;
  static constexpr SampleFormat kOrder[] = {
      SampleFormat::Float32, SampleFormat::Pcm24In32, SampleFormat::Pcm24, SampleFormat::Pcm32, SampleFormat::Pcm16,
  };
  std::vector<int> rates;
  if (req.sampleRate > 0) rates.push_back(req.sampleRate);
  for (int r : {48000, 44100, 96000, 88200}) {
    if (std::find(rates.begin(), rates.end(), r) == rates.end()) rates.push_back(r);
  }
  for (int rate : rates) {
    for (SampleFormat fmt : kOrder) {
      const FormatCandidate c{fmt, rate, req.channels};
      if (probe(c)) {
        out.format = fmt;
        out.sampleRate = rate;
        out.channels = req.channels;
        out.periodFrames = req.bufferFrames;
        out.exactRate = (req.sampleRate > 0 && rate == req.sampleRate);
        return true;
      }
    }
  }
  return false;
}

int hnsToFrames(int64_t hns, int sampleRate) {
  if (hns <= 0 || sampleRate <= 0) return 0;
  const double frames = static_cast<double>(hns) * static_cast<double>(sampleRate) / static_cast<double>(kHnsPerSecond);
  return std::max(1, static_cast<int>(std::llround(frames)));
}

int64_t framesToHns(int frames, int sampleRate) {
  if (frames <= 0 || sampleRate <= 0) return 0;
  const double hns = static_cast<double>(frames) * static_cast<double>(kHnsPerSecond) / static_cast<double>(sampleRate);
  return std::max<int64_t>(1, static_cast<int64_t>(std::llround(hns)));
}

int64_t chooseExclusivePeriodHns(int requestedFrames, int sampleRate, const PeriodInfo& period) {
  const int64_t floorHns = period.minPeriodHns > 0 ? period.minPeriodHns : std::max<int64_t>(0, period.defaultPeriodHns);
  if (requestedFrames <= 0 || sampleRate <= 0) return floorHns;
  const int64_t wantHns = framesToHns(requestedFrames, sampleRate);
  return std::max(wantHns, floorHns);
}

LatencyInfo computeLatency(int bufferFrames, int64_t streamLatencyHns, int sampleRate, bool exclusiveEventDriven, int inputFrames) {
  LatencyInfo info;
  const int sr = std::max(1, sampleRate);
  info.bufferFrames = std::max(0, bufferFrames);
  info.streamLatencyFrames = hnsToFrames(streamLatencyHns, sr);
  info.totalOutputFrames = (exclusiveEventDriven ? 2 * info.bufferFrames : info.bufferFrames) + info.streamLatencyFrames;
  info.inputFrames = std::max(0, inputFrames);
  info.outputMs = 1000.0 * static_cast<double>(info.totalOutputFrames) / static_cast<double>(sr);
  info.roundTripMs = 1000.0 * static_cast<double>(info.totalOutputFrames + info.inputFrames) / static_cast<double>(sr);
  return info;
}

OpenFailure classifyHResult(uint32_t hr) {
  if (hr == 0u) return OpenFailure::None;
  switch (hr) {
    case 0x8889000Au: return OpenFailure::DeviceInUse;         // AUDCLNT_E_DEVICE_IN_USE
    case 0x8889000Eu: return OpenFailure::ExclusiveNotAllowed; // AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED
    case 0x88890008u: return OpenFailure::FormatUnsupported;   // AUDCLNT_E_UNSUPPORTED_FORMAT
    case 0x88890016u:                                          // AUDCLNT_E_BUFFER_SIZE_ERROR
    case 0x88890019u: return OpenFailure::BufferInvalid;       // AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED
    case 0x88890004u: return OpenFailure::DeviceInvalidated;   // AUDCLNT_E_DEVICE_INVALIDATED
    case 0x80070490u: return OpenFailure::NoDevice;            // E_NOTFOUND
    default: break;
  }
  if ((hr & 0xFFFF0000u) == 0x88890000u) return OpenFailure::StreamInitFailed;
  return OpenFailure::ComError;
}

std::string describeFailure(OpenFailure code, const std::string& detail) {
  std::string msg;
  switch (code) {
    case OpenFailure::None: return {};
    case OpenFailure::NotAvailableOnPlatform:
      msg = "WASAPI Exclusive is available only on Windows. Using shared system audio.";
      break;
    case OpenFailure::NoDevice:
      msg = "WASAPI Exclusive unavailable: audio output device was not found. Fell back to shared audio.";
      break;
    case OpenFailure::DeviceInUse:
      msg = "WASAPI Exclusive unavailable: the device is in exclusive use by another application. Fell back to shared audio.";
      break;
    case OpenFailure::ExclusiveNotAllowed:
      msg = "WASAPI Exclusive is disabled in Windows Sound settings for this device. Fell back to shared audio.";
      break;
    case OpenFailure::FormatUnsupported:
      msg = "WASAPI Exclusive unavailable: the device rejected the requested sample rate and format. Fell back to shared audio.";
      break;
    case OpenFailure::BufferInvalid:
      msg = "WASAPI Exclusive unavailable: the device rejected the requested buffer alignment. Fell back to shared audio.";
      break;
    case OpenFailure::DeviceInvalidated:
      msg = "WASAPI Exclusive device was disconnected or reset. Fell back to shared audio.";
      break;
    case OpenFailure::StreamInitFailed:
      msg = "WASAPI Exclusive stream failed to initialize. Fell back to shared audio.";
      break;
    case OpenFailure::ComError:
      msg = "WASAPI Exclusive could not be initialized. Fell back to shared audio.";
      break;
  }
  if (!detail.empty()) msg += " (" + detail + ")";
  return msg;
}

BackendStatus resolveFallbackStatus(bool exclusiveRequested, OpenFailure exclusiveFailure, const std::string& exclusiveDetail,
                                    const std::string& fallbackBackendName, const std::string& deviceName, int sampleRate, int bufferFrames,
                                    int inputLatencyFrames) {
  BackendStatus st;
  const bool fellBack = exclusiveRequested && exclusiveFailure != OpenFailure::None;
  st.mode = fellBack ? ActiveMode::WasapiSharedFallback : ActiveMode::SdlShared;
  st.backendName = fallbackBackendName.empty() ? "System audio (SDL)" : fallbackBackendName;
  st.deviceName = deviceName.empty() ? "System default" : deviceName;
  st.sampleFormat = formatName(SampleFormat::Float32);
  st.sampleRate = std::max(1, sampleRate);
  st.bufferFrames = std::max(0, bufferFrames);
  const LatencyInfo lat = computeLatency(st.bufferFrames, 0, st.sampleRate, false, inputLatencyFrames);
  st.outputLatencyFrames = lat.totalOutputFrames;
  st.inputLatencyFrames = lat.inputFrames;
  st.outputLatencyMs = lat.outputMs;
  st.roundTripLatencyMs = lat.roundTripMs;
  st.exclusiveRequested = exclusiveRequested;
  st.exclusiveActive = false;
  st.fellBack = fellBack;
  st.fallbackCode = fellBack ? exclusiveFailure : OpenFailure::None;
  st.fallbackReason = fellBack ? describeFailure(exclusiveFailure, exclusiveDetail) : std::string();
  return st;
}

#ifdef _WIN32

namespace {

std::string wideToUtf8(const wchar_t* w) {
  if (!w || !*w) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return {};
  std::string s(static_cast<size_t>(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

std::string endpointFriendlyName(IMMDevice* dev) {
  if (!dev) return {};
  IPropertyStore* props = nullptr;
  if (FAILED(dev->OpenPropertyStore(STGM_READ, &props)) || !props) return {};
  PROPVARIANT var;
  PropVariantInit(&var);
  std::string name;
  if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &var)) && var.vt == VT_LPWSTR && var.pwszVal) {
    name = wideToUtf8(var.pwszVal);
  }
  PropVariantClear(&var);
  props->Release();
  return name;
}

WAVEFORMATEXTENSIBLE makeWaveFormat(const FormatCandidate& c) {
  WAVEFORMATEXTENSIBLE wfx{};
  const int bytes = formatContainerBytes(c.format);
  const int validBits = formatValidBits(c.format);
  wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  wfx.Format.nChannels = static_cast<WORD>(c.channels);
  wfx.Format.nSamplesPerSec = static_cast<DWORD>(c.sampleRate);
  wfx.Format.wBitsPerSample = static_cast<WORD>(bytes * 8);
  wfx.Format.nBlockAlign = static_cast<WORD>(c.channels * bytes);
  wfx.Format.nAvgBytesPerSec = static_cast<DWORD>(c.sampleRate) * wfx.Format.nBlockAlign;
  wfx.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
  wfx.Samples.wValidBitsPerSample = static_cast<WORD>(validBits);
  wfx.dwChannelMask = (c.channels == 1) ? SPEAKER_FRONT_CENTER : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
  wfx.SubFormat = (c.format == SampleFormat::Float32) ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
  return wfx;
}

std::string hexHResult(HRESULT hr) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "HRESULT 0x%08lX", static_cast<unsigned long>(hr));
  return buf;
}

}  // namespace

struct ExclusiveHost::Impl {
  IMMDevice* device = nullptr;
  IAudioClient* client = nullptr;
  IAudioRenderClient* render = nullptr;
  HANDLE event = nullptr;
  HANDLE stopEvent = nullptr;
  std::thread thread;
  std::atomic<bool> running{false};
  std::atomic<bool> lost{false};
  RenderFn fn = nullptr;
  void* user = nullptr;
  SampleFormat format = SampleFormat::Float32;
  int channels = 2;
  int bufferFrames = 0;
  std::vector<float> scratchL;
  std::vector<float> scratchR;

  void releaseAll() {
    if (running.exchange(false)) {
      if (stopEvent) SetEvent(stopEvent);
      if (thread.joinable()) thread.join();
      if (client) client->Stop();
    } else if (thread.joinable()) {
      if (stopEvent) SetEvent(stopEvent);
      thread.join();
    }
    if (render) render->Release(), render = nullptr;
    if (client) client->Release(), client = nullptr;
    if (device) device->Release(), device = nullptr;
    if (event) CloseHandle(event), event = nullptr;
    if (stopEvent) CloseHandle(stopEvent), stopEvent = nullptr;
    lost.store(false, std::memory_order_relaxed);
  }

  void audioLoop() {
    gl::threads::setName("gl-wasapi-excl");
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    HANDLE waits[2] = {stopEvent, event};
    while (running.load(std::memory_order_acquire)) {
      const DWORD r = WaitForMultipleObjects(2, waits, FALSE, 2000);
      if (r == WAIT_OBJECT_0 || !running.load(std::memory_order_acquire)) break;
      if (r != WAIT_OBJECT_0 + 1) continue;
      BYTE* dst = nullptr;
      const HRESULT hr = render->GetBuffer(static_cast<UINT32>(bufferFrames), &dst);
      if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
        lost.store(true, std::memory_order_release);
        break;
      }
      if (FAILED(hr) || !dst) continue;
      if (fn) {
        fn(user, scratchL.data(), scratchR.data(), bufferFrames);
      } else {
        std::memset(scratchL.data(), 0, static_cast<size_t>(bufferFrames) * sizeof(float));
        std::memset(scratchR.data(), 0, static_cast<size_t>(bufferFrames) * sizeof(float));
      }
      packFromFloat(scratchL.data(), scratchR.data(), bufferFrames, format, channels, dst);
      const HRESULT rhr = render->ReleaseBuffer(static_cast<UINT32>(bufferFrames), 0);
      if (rhr == AUDCLNT_E_DEVICE_INVALIDATED) {
        lost.store(true, std::memory_order_release);
        break;
      }
    }
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    if (SUCCEEDED(comHr)) CoUninitialize();
  }
};

ExclusiveHost::ExclusiveHost() : impl_(std::make_unique<Impl>()) {}
ExclusiveHost::~ExclusiveHost() { close(); }

bool ExclusiveHost::backendAvailable() { return true; }

std::vector<std::string> ExclusiveHost::listOutputDevices() {
  std::vector<std::string> out;
  const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  IMMDeviceEnumerator* en = nullptr;
  if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                 reinterpret_cast<void**>(&en))) &&
      en) {
    IMMDeviceCollection* col = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col)) && col) {
      UINT count = 0;
      if (SUCCEEDED(col->GetCount(&count))) {
        for (UINT i = 0; i < count; ++i) {
          IMMDevice* dev = nullptr;
          if (SUCCEEDED(col->Item(i, &dev)) && dev) {
            std::string nm = endpointFriendlyName(dev);
            if (!nm.empty()) out.push_back(std::move(nm));
            dev->Release();
          }
        }
      }
      col->Release();
    }
    en->Release();
  }
  if (SUCCEEDED(comHr)) CoUninitialize();
  return out;
}

bool ExclusiveHost::open(const Config& cfg, RenderFn fn, void* user, OpenFailure* failOut, std::string* errOut) {
  close();
  auto fail = [&](OpenFailure code, const std::string& detail) {
    impl_->releaseAll();
    if (failOut) *failOut = code;
    if (errOut) *errOut = describeFailure(code, detail);
    return false;
  };
  static const bool comInit = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
  (void)comInit;
  IMMDeviceEnumerator* en = nullptr;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&en));
  if (FAILED(hr) || !en) return fail(classifyHResult(static_cast<uint32_t>(hr)), hexHResult(hr));

  IMMDevice* dev = nullptr;
  if (cfg.deviceName.empty()) {
    hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
  } else {
    IMMDeviceCollection* col = nullptr;
    hr = en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col);
    if (SUCCEEDED(hr) && col) {
      UINT count = 0;
      col->GetCount(&count);
      for (UINT i = 0; i < count && !dev; ++i) {
        IMMDevice* item = nullptr;
        if (SUCCEEDED(col->Item(i, &item)) && item) {
          if (endpointFriendlyName(item) == cfg.deviceName) dev = item;
          else item->Release();
        }
      }
      col->Release();
    }
    if (!dev) hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
  }
  en->Release();
  if (FAILED(hr) || !dev) return fail(OpenFailure::NoDevice, hexHResult(hr));
  impl_->device = dev;
  const std::string friendlyName = endpointFriendlyName(dev);

  hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&impl_->client));
  if (FAILED(hr) || !impl_->client) return fail(classifyHResult(static_cast<uint32_t>(hr)), hexHResult(hr));

  REFERENCE_TIME defPeriod = 0, minPeriod = 0;
  hr = impl_->client->GetDevicePeriod(&defPeriod, &minPeriod);
  if (FAILED(hr)) return fail(classifyHResult(static_cast<uint32_t>(hr)), hexHResult(hr));

  NegotiatedFormat neg;
  const FormatRequest req{cfg.sampleRate > 0 ? cfg.sampleRate : 48000, std::clamp(cfg.channels, 1, 2), cfg.bufferFrames};
  const bool fmtOk = negotiateFormat(
      req,
      [&](const FormatCandidate& c) {
        WAVEFORMATEXTENSIBLE wfx = makeWaveFormat(c);
        return impl_->client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &wfx.Format, nullptr) == S_OK;
      },
      neg);
  if (!fmtOk) return fail(OpenFailure::FormatUnsupported, std::to_string(req.sampleRate) + " Hz");

  WAVEFORMATEXTENSIBLE wfx = makeWaveFormat(FormatCandidate{neg.format, neg.sampleRate, neg.channels});
  REFERENCE_TIME periodHns = chooseExclusivePeriodHns(cfg.bufferFrames, neg.sampleRate, PeriodInfo{defPeriod, minPeriod});
  hr = impl_->client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, periodHns, periodHns, &wfx.Format, nullptr);
  if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
    UINT32 alignedFrames = 0;
    if (SUCCEEDED(impl_->client->GetBufferSize(&alignedFrames)) && alignedFrames > 0) {
      periodHns = framesToHns(static_cast<int>(alignedFrames), neg.sampleRate);
      impl_->client->Release();
      impl_->client = nullptr;
      hr = impl_->device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&impl_->client));
      if (SUCCEEDED(hr) && impl_->client) {
        hr = impl_->client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, periodHns, periodHns, &wfx.Format, nullptr);
      }
    }
  }
  if (FAILED(hr)) return fail(classifyHResult(static_cast<uint32_t>(hr)), hexHResult(hr));

  UINT32 actualFrames = 0;
  hr = impl_->client->GetBufferSize(&actualFrames);
  if (FAILED(hr) || actualFrames == 0) return fail(OpenFailure::BufferInvalid, hexHResult(hr));

  REFERENCE_TIME streamLatencyHns = 0;
  (void)impl_->client->GetStreamLatency(&streamLatencyHns);

  impl_->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  impl_->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!impl_->event || !impl_->stopEvent) return fail(OpenFailure::StreamInitFailed, "CreateEventW failed");

  hr = impl_->client->SetEventHandle(impl_->event);
  if (FAILED(hr)) return fail(classifyHResult(static_cast<uint32_t>(hr)), hexHResult(hr));

  hr = impl_->client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&impl_->render));
  if (FAILED(hr) || !impl_->render) return fail(classifyHResult(static_cast<uint32_t>(hr)), hexHResult(hr));

  impl_->fn = fn;
  impl_->user = user;
  impl_->format = neg.format;
  impl_->channels = neg.channels;
  impl_->bufferFrames = static_cast<int>(actualFrames);
  // Pre-allocate scratch buffers on the UI thread (zero allocations on the audio thread).
  impl_->scratchL.assign(static_cast<size_t>(actualFrames), 0.0f);
  impl_->scratchR.assign(static_cast<size_t>(actualFrames), 0.0f);

  const LatencyInfo lat = computeLatency(impl_->bufferFrames, streamLatencyHns, neg.sampleRate, true, 0);
  status_ = BackendStatus{};
  status_.mode = ActiveMode::WasapiExclusive;
  status_.backendName = "WASAPI Exclusive";
  status_.deviceName = friendlyName.empty() ? (cfg.deviceName.empty() ? "System default" : cfg.deviceName) : friendlyName;
  status_.sampleFormat = formatName(neg.format);
  status_.sampleRate = neg.sampleRate;
  status_.bufferFrames = impl_->bufferFrames;
  status_.outputLatencyFrames = lat.totalOutputFrames;
  status_.inputLatencyFrames = 0;
  status_.outputLatencyMs = lat.outputMs;
  status_.roundTripLatencyMs = lat.roundTripMs;
  status_.exclusiveRequested = true;
  status_.exclusiveActive = true;
  status_.fellBack = false;
  status_.fallbackCode = OpenFailure::None;
  if (failOut) *failOut = OpenFailure::None;
  if (errOut) errOut->clear();
  return true;
}

bool ExclusiveHost::start(std::string* errOut) {
  if (!impl_->client || !impl_->render) {
    if (errOut) *errOut = "WASAPI Exclusive device is not open.";
    return false;
  }
  if (impl_->running.load()) return true;
  // Pre-fill initial silent buffer before Start() as required by WASAPI exclusive event mode.
  BYTE* dst = nullptr;
  if (SUCCEEDED(impl_->render->GetBuffer(static_cast<UINT32>(impl_->bufferFrames), &dst)) && dst) {
    std::memset(dst, 0,
                static_cast<size_t>(impl_->bufferFrames) * static_cast<size_t>(impl_->channels) *
                    static_cast<size_t>(formatContainerBytes(impl_->format)));
    impl_->render->ReleaseBuffer(static_cast<UINT32>(impl_->bufferFrames), AUDCLNT_BUFFERFLAGS_SILENT);
  }
  const HRESULT hr = impl_->client->Start();
  if (FAILED(hr)) {
    if (errOut) *errOut = describeFailure(classifyHResult(static_cast<uint32_t>(hr)), hexHResult(hr));
    return false;
  }
  impl_->running.store(true, std::memory_order_release);
  impl_->thread = std::thread([this]() { impl_->audioLoop(); });
  return true;
}

void ExclusiveHost::close() {
  if (impl_) impl_->releaseAll();
  status_ = BackendStatus{};
}

bool ExclusiveHost::isOpen() const { return impl_ && impl_->client != nullptr; }
bool ExclusiveHost::running() const { return impl_ && impl_->running.load(std::memory_order_acquire); }
bool ExclusiveHost::deviceLost() const { return impl_ && impl_->lost.load(std::memory_order_acquire); }

#else  // Non-Windows platforms (macOS / Linux / BSD)

struct ExclusiveHost::Impl {
  bool open = false;
};

ExclusiveHost::ExclusiveHost() : impl_(std::make_unique<Impl>()) {}
ExclusiveHost::~ExclusiveHost() { close(); }

bool ExclusiveHost::backendAvailable() {
  const char* e = std::getenv("GRIDLINE_FAKE_WASAPI");
  return e && *e && *e != '0';
}

std::vector<std::string> ExclusiveHost::listOutputDevices() { return {}; }

bool ExclusiveHost::open(const Config&, RenderFn, void*, OpenFailure* failOut, std::string* errOut) {
  close();
  const OpenFailure code = OpenFailure::NotAvailableOnPlatform;
  if (failOut) *failOut = code;
  if (errOut) *errOut = describeFailure(code);
  return false;
}

bool ExclusiveHost::start(std::string* errOut) {
  if (errOut) *errOut = describeFailure(OpenFailure::NotAvailableOnPlatform);
  return false;
}

void ExclusiveHost::close() {
  if (impl_) impl_->open = false;
  status_ = BackendStatus{};
}

bool ExclusiveHost::isOpen() const { return false; }
bool ExclusiveHost::running() const { return false; }
bool ExclusiveHost::deviceLost() const { return false; }

#endif

}  // namespace gl::wasapi
