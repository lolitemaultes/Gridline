// Gridline — K-95 unit tests: WASAPI Exclusive format negotiation, sample packing
// (F32 / PCM32 / PCM24 / PCM24-in-32 / PCM16), period & latency math, HRESULT
// classification, exclusive->shared fallback state machine, and settings persistence.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "Check.h"
#include "core/Settings.h"
#include "engine/AudioWasapi.h"

namespace fs = std::filesystem;
using namespace gl;
using namespace gl::wasapi;

namespace {

bool near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

void testSamplePacking() {
  constexpr int kN = 64;
  float srcL[kN], srcR[kN], outL[kN], outR[kN];
  for (int i = 0; i < kN; ++i) {
    srcL[i] = static_cast<float>(0.85 * std::sin(0.23 * i));
    srcR[i] = static_cast<float>(0.85 * std::cos(0.31 * i));
  }
  const SampleFormat fmts[] = {
      SampleFormat::Float32, SampleFormat::Pcm24In32, SampleFormat::Pcm24, SampleFormat::Pcm32, SampleFormat::Pcm16,
  };
  for (SampleFormat f : fmts) {
    const int bps = formatContainerBytes(f);
    expect(bps >= 2 && bps <= 4 && formatValidBits(f) >= 16, (std::string("pack: metadata valid for ") + formatName(f)).c_str());
    std::vector<unsigned char> buf(static_cast<size_t>(kN * 2 * bps), 0xAA);
    packFromFloat(srcL, srcR, kN, f, 2, buf.data());
    unpackToFloat(buf.data(), f, 2, kN, outL, outR);
    const float tol = (f == SampleFormat::Pcm16) ? (1.5f / 32768.0f) : (f == SampleFormat::Float32 || f == SampleFormat::Pcm32) ? 1e-6f : (1.5f / 8388608.0f);
    bool ok = true;
    for (int i = 0; i < kN; ++i) {
      if (!near(srcL[i], outL[i], tol) || !near(srcR[i], outR[i], tol)) ok = false;
    }
    expect(ok, (std::string("pack: stereo round-trip accurate for ") + formatName(f)).c_str());
  }

  // Known bit patterns, clipping, and NaN/Inf safety.
  const float edgeL[4] = {0.5f, -1.0f, 1.5f, std::nanf("")};
  const float edgeR[4] = {-0.5f, 1.0f, -2.5f, std::numeric_limits<float>::infinity()};
  {
    int16_t i16[8] = {};
    packFromFloat(edgeL, edgeR, 4, SampleFormat::Pcm16, 2, i16);
    expect(i16[0] == 16384 && i16[1] == -16384, "pack: PCM16 +-0.5 exact");
    expect(i16[2] == -32768 && i16[3] == 32767, "pack: PCM16 full scale");
    expect(i16[4] == 32767 && i16[5] == -32768, "pack: PCM16 clips out-of-range");
    expect(i16[6] == 0 && i16[7] == 0, "pack: PCM16 writes silence for NaN and Inf");
  }
  {
    unsigned char b24[4 * 2 * 3] = {};
    packFromFloat(edgeL, edgeR, 4, SampleFormat::Pcm24, 2, b24);
    // frame 1 L = -1.0 -> 0x800000 little-endian: 00 00 80
    expect(b24[6] == 0x00 && b24[7] == 0x00 && b24[8] == 0x80, "pack: PCM24 -1.0 is 0x800000 LE");
    // frame 1 R = +1.0 -> 0x7FFFFF little-endian: FF FF 7F
    expect(b24[9] == 0xFF && b24[10] == 0xFF && b24[11] == 0x7F, "pack: PCM24 +1.0 clips to 0x7FFFFF LE");
    // frame 3 NaN / Inf -> 00 00 00
    expect(b24[18] == 0 && b24[19] == 0 && b24[20] == 0 && b24[21] == 0 && b24[22] == 0 && b24[23] == 0, "pack: PCM24 NaN/Inf -> 0");
  }
  {
    unsigned char b24in32[4 * 2 * 4] = {};
    packFromFloat(edgeL, edgeR, 4, SampleFormat::Pcm24In32, 2, b24in32);
    // Low byte must always be 0 (24 valid bits MSB-aligned in 32-bit container)
    bool lowZero = true;
    for (int s = 0; s < 8; ++s) lowZero = lowZero && (b24in32[s * 4] == 0);
    expect(lowZero, "pack: PCM24-in-32 keeps low byte zero");
    float uL[4], uR[4];
    unpackToFloat(b24in32, SampleFormat::Pcm24In32, 2, 4, uL, uR);
    expect(near(uL[0], 0.5f, 1e-6f) && near(uL[1], -1.0f, 1e-6f) && uL[3] == 0.0f, "pack: PCM24-in-32 unpack matches");
  }
  {
    // 4-channel packing zeroes extra channels 3 and 4.
    float f4[4] = {9.0f, 9.0f, 9.0f, 9.0f};
    const float one = 0.25f, two = -0.25f;
    packFromFloat(&one, &two, 1, SampleFormat::Float32, 4, f4);
    expect(f4[0] == 0.25f && f4[1] == -0.25f && f4[2] == 0.0f && f4[3] == 0.0f, "pack: extra channels > 2 are zero-filled");
  }
}

void testFormatNegotiation() {
  NegotiatedFormat neg;
  {
    // Device accepts everything -> prefers Float32 at requested sample rate.
    FormatRequest req{96000, 2, 128};
    const bool ok = negotiateFormat(req, [](const FormatCandidate&) { return true; }, neg);
    expect(ok && neg.format == SampleFormat::Float32 && neg.sampleRate == 96000 && neg.exactRate,
           "negotiate: prefers 32-bit float at requested rate");
  }
  {
    // Device rejects Float32 & 24-in-32, accepts packed PCM24 at 48 kHz.
    FormatRequest req{48000, 2, 64};
    const bool ok = negotiateFormat(
        req,
        [](const FormatCandidate& c) { return c.sampleRate == 48000 && (c.format == SampleFormat::Pcm24 || c.format == SampleFormat::Pcm16); },
        neg);
    expect(ok && neg.format == SampleFormat::Pcm24 && neg.sampleRate == 48000 && neg.exactRate,
           "negotiate: falls back from Float32 to PCM24");
  }
  {
    // Device only supports PCM16 at 44100 Hz while user requested 96000 Hz.
    FormatRequest req{96000, 2, 256};
    const bool ok = negotiateFormat(
        req, [](const FormatCandidate& c) { return c.sampleRate == 44100 && c.format == SampleFormat::Pcm16; }, neg);
    expect(ok && neg.format == SampleFormat::Pcm16 && neg.sampleRate == 44100 && !neg.exactRate,
           "negotiate: falls back to supported rate and PCM16 with exactRate=false");
  }
  {
    // Device rejects all candidates -> returns false.
    FormatRequest req{48000, 2, 128};
    const bool ok = negotiateFormat(req, [](const FormatCandidate&) { return false; }, neg);
    expect(!ok, "negotiate: returns false when no format is supported");
  }
}

void testLatencyMath() {
  expect(framesToHns(480, 48000) == 100000, "latency: 480 frames @ 48kHz = 10 ms (100,000 HNS)");
  expect(hnsToFrames(100000, 48000) == 480, "latency: 100,000 HNS @ 48kHz = 480 frames");
  expect(hnsToFrames(30000, 48000) == 144, "latency: 3 ms min period @ 48kHz = 144 frames");

  const PeriodInfo period{100000, 30000};  // 10 ms default, 3 ms min
  expect(chooseExclusivePeriodHns(0, 48000, period) == 30000, "period: 0 frames selects device minimum period");
  expect(chooseExclusivePeriodHns(64, 48000, period) == 30000, "period: request below min clamps up to minPeriodHns");
  expect(chooseExclusivePeriodHns(256, 48000, period) == framesToHns(256, 48000), "period: request above min is honoured");

  const LatencyInfo excl = computeLatency(144, 20000, 48000, true, 144);
  // 2 * 144 (double buffer) + 96 (2 ms stream latency) = 384 frames = 8.0 ms; round-trip = 528 frames = 11.0 ms.
  expect(excl.streamLatencyFrames == 96, "latency: 2 ms stream latency = 96 frames @ 48kHz");
  expect(excl.totalOutputFrames == 384, "latency: exclusive total output = 2*buf + streamLatency");
  expect(std::fabs(excl.outputMs - 8.0) < 1e-6, "latency: exclusive outputMs = 8.0 ms");
  expect(std::fabs(excl.roundTripMs - 11.0) < 1e-6, "latency: exclusive roundTripMs = 11.0 ms");

  const LatencyInfo shared = computeLatency(256, 0, 44100, false, 0);
  expect(shared.totalOutputFrames == 256 && std::fabs(shared.outputMs - (1000.0 * 256.0 / 44100.0)) < 1e-6,
         "latency: shared mode outputMs matches buffer/rate");
}

void testFallbackStateMachine() {
  expect(classifyHResult(0u) == OpenFailure::None, "hresult: S_OK -> None");
  expect(classifyHResult(0x8889000Au) == OpenFailure::DeviceInUse, "hresult: AUDCLNT_E_DEVICE_IN_USE");
  expect(classifyHResult(0x8889000Eu) == OpenFailure::ExclusiveNotAllowed, "hresult: AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED");
  expect(classifyHResult(0x88890008u) == OpenFailure::FormatUnsupported, "hresult: AUDCLNT_E_UNSUPPORTED_FORMAT");
  expect(classifyHResult(0x88890019u) == OpenFailure::BufferInvalid, "hresult: AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED");
  expect(classifyHResult(0x88890004u) == OpenFailure::DeviceInvalidated, "hresult: AUDCLNT_E_DEVICE_INVALIDATED");
  expect(classifyHResult(0x80070490u) == OpenFailure::NoDevice, "hresult: E_NOTFOUND -> NoDevice");
  expect(classifyHResult(0x80004005u) == OpenFailure::ComError, "hresult: E_FAIL -> ComError");

  // Exclusive requested, device in use -> automatic fallback with clear reason.
  const BackendStatus fb = resolveFallbackStatus(true, OpenFailure::DeviceInUse, "", "WASAPI Shared (SDL)", "Speakers", 48000, 256, 256);
  expect(fb.mode == ActiveMode::WasapiSharedFallback && fb.fellBack && !fb.exclusiveActive && fb.exclusiveRequested,
         "fallback: exclusive busy transitions to WasapiSharedFallback");
  expect(fb.fallbackCode == OpenFailure::DeviceInUse && fb.fallbackReason.find("another application") != std::string::npos,
         "fallback: human-readable reason explains device busy");
  expect(fb.outputLatencyFrames == 256 && std::fabs(fb.outputLatencyMs - (1000.0 * 256.0 / 48000.0)) < 1e-6,
         "fallback: shared output latency reported accurately");
  expect(std::fabs(fb.roundTripLatencyMs - (1000.0 * 512.0 / 48000.0)) < 1e-6, "fallback: round-trip latency includes input");

  // Normal shared mode (exclusive NOT requested) -> no fallback banner.
  const BackendStatus sh = resolveFallbackStatus(false, OpenFailure::None, "", "CoreAudio (SDL)", "", 44100, 256, 0);
  expect(sh.mode == ActiveMode::SdlShared && !sh.fellBack && sh.fallbackReason.empty() && sh.deviceName == "System default",
         "fallback: normal shared mode reports SdlShared with empty fallbackReason");

#ifndef _WIN32
  // Non-Windows open cleanly reports NotAvailableOnPlatform without crashing.
  ExclusiveHost host;
  OpenFailure fail = OpenFailure::None;
  std::string err;
  expect(!host.open(Config{}, nullptr, nullptr, &fail, &err) && fail == OpenFailure::NotAvailableOnPlatform && !err.empty(),
         "host: non-Windows open fails cleanly with NotAvailableOnPlatform");
#endif
}

void testSettingsPersistence() {
  Settings s;
  expect(!s.getBool("audio.wasapi.exclusive"), "settings: audio.wasapi.exclusive defaults to false");
  expect(s.set("audio.wasapi.exclusive", true) && s.getBool("audio.wasapi.exclusive"), "settings: audio.wasapi.exclusive toggles true");
  const fs::path tmp = fs::temp_directory_path() / "gl_k95_wasapi_settings.json";
  std::string err;
  expect(s.save(tmp.string(), &err), "settings: save succeeds");
  Settings loaded;
  expect(loaded.load(tmp.string(), &err) && loaded.getBool("audio.wasapi.exclusive"), "settings: audio.wasapi.exclusive persists across load");
  std::error_code ec;
  fs::remove(tmp, ec);
}

}  // namespace

int main() {
  testSamplePacking();
  testFormatNegotiation();
  testLatencyMath();
  testFallbackStateMachine();
  testSettingsPersistence();
  return finish();
}
