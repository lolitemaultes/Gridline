// K-6: the platform-independent parts of the ASIO backend (every platform): sample format
// conversion, buffer size choice, the channel plan, and the host driven by the simulated driver
// (reset / latency / rate messages, a stalled or missing device, switching drivers).
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "Check.h"
#include "host/audio/AsioCore.h"
#include "host/audio/AsioHost.h"

using namespace gl::asio;
namespace abi = gl::asio::abi;

namespace {

bool near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

void testFormats() {
  const abi::SampleType types[] = {abi::kInt16LSB,   abi::kInt24LSB,   abi::kInt32LSB,   abi::kFloat32LSB, abi::kFloat64LSB,
                                   abi::kInt32LSB16, abi::kInt32LSB18, abi::kInt32LSB20, abi::kInt32LSB24, abi::kInt16MSB,
                                   abi::kInt24MSB,   abi::kInt32MSB,   abi::kFloat32MSB, abi::kFloat64MSB};
  const int bits[] = {16, 24, 32, 32, 64, 16, 18, 20, 24, 16, 24, 32, 32, 64};
  float src[64], back[64];
  for (int i = 0; i < 64; ++i) src[i] = static_cast<float>(std::sin(0.37 * i) * 0.9);
  bool roundTrip = true;
  for (size_t t = 0; t < sizeof types / sizeof types[0]; ++t) {
    if (!formatSupported(types[t]) || formatBytes(types[t]) <= 0) roundTrip = false;
    unsigned char buf[64 * 8] = {};
    fromFloat(src, types[t], buf, 64);
    toFloat(buf, types[t], back, 64);
    const float tol = bits[t] >= 32 ? 1e-6f : 1.5f / static_cast<float>(1 << (bits[t] - 1));
    for (int i = 0; i < 64; ++i)
      if (!near(src[i], back[i], tol)) roundTrip = false;
  }
  expect(roundTrip, "every supported format converts float -> device -> float within one step");

  // Known bit patterns (little endian containers).
  unsigned char b[16] = {};
  const float half = 0.5f, minus1 = -1.0f, plus1 = 1.0f;
  fromFloat(&half, abi::kInt16LSB, b, 1);
  expect(b[0] == 0x00 && b[1] == 0x40, "int16 LSB: 0.5 is 0x4000");
  fromFloat(&minus1, abi::kInt24LSB, b, 1);
  expect(b[0] == 0x00 && b[1] == 0x00 && b[2] == 0x80, "int24 LSB: -1 is 0x800000");
  fromFloat(&plus1, abi::kInt24LSB, b, 1);
  expect(b[0] == 0xff && b[1] == 0xff && b[2] == 0x7f, "int24 LSB: +1 clips to 0x7fffff");
  fromFloat(&plus1, abi::kInt32LSB16, b, 1);
  int32_t v32 = 0;
  std::memcpy(&v32, b, 4);
  expect(v32 == 32767, "int32 LSB16: +1 is 32767 in a 32-bit container");
  fromFloat(&half, abi::kInt16MSB, b, 1);
  expect(b[0] == 0x40 && b[1] == 0x00, "int16 MSB: 0.5 is 0x40 0x00");
  // Clipping, NaN.
  const float wild[4] = {1.5f, -3.0f, std::nanf(""), 0.25f};
  int16_t i16[4];
  fromFloat(wild, abi::kInt16LSB, i16, 4);
  expect(i16[0] == 32767 && i16[1] == -32768 && i16[2] == 0 && i16[3] == 8192, "int16: overs clip, NaN writes silence");
  float f[4];
  toFloat(i16, abi::kInt16LSB, f, 4);
  expect(f[1] == -1.0f && near(f[0], 1.0f, 1e-4f), "int16 full scale reads back as +-1");
  // A negative int24 sign-extends.
  const unsigned char neg24[3] = {0x00, 0x00, 0xc0};  // 0xc00000 = -0.5
  toFloat(neg24, abi::kInt24LSB, f, 1);
  expect(f[0] == -0.5f, "int24 LSB: 0xc00000 reads as -0.5");
  // Unsupported (DSD): silence, no crash.
  expect(!formatSupported(32) && formatBytes(32) == 0, "DSD formats are not supported");
  float z[4] = {1, 1, 1, 1};
  toFloat(b, 32, z, 4);
  expect(z[0] == 0 && z[3] == 0, "an unsupported input reads as silence");
}

void testBuffers() {
  const BufferCaps pow2{32, 512, 256, -1};
  const std::vector<int> c = bufferSizeChoices(pow2);
  expect(c == std::vector<int>({32, 64, 128, 256, 512}), "powers of two 32..512");
  expect(chooseBufferSize(pow2, 0) == 256, "0 = the preferred size");
  expect(chooseBufferSize(pow2, 64) == 64, "a valid size is kept");
  expect(chooseBufferSize(pow2, 100) == 128, "an invalid size goes to the nearest valid one");
  expect(chooseBufferSize(pow2, 4096) == 512 && chooseBufferSize(pow2, 8) == 32, "out of range clamps to the extremes");
  const BufferCaps fixed{256, 256, 256, 0};
  expect(bufferSizeChoices(fixed) == std::vector<int>({256}) && chooseBufferSize(fixed, 64) == 256, "granularity 0: one size");
  const BufferCaps step{64, 1024, 256, 32};
  expect(bufferSizeValid(step, 96) && !bufferSizeValid(step, 100), "granularity 32 from 64");
  expect(chooseBufferSize(step, 100) == 96, "nearest step");
  const BufferCaps fine{1, 8192, 441, 1};
  const std::vector<int> fc = bufferSizeChoices(fine);
  bool hasPref = false, has32 = false, has512 = false;
  for (int s : fc) hasPref |= s == 441, has32 |= s == 32, has512 |= s == 512;
  expect(fc.size() <= 24 && hasPref && has32 && has512 && fc.front() == 1 && fc.back() == 8192, "a fine step is trimmed to round sizes");
  expect(chooseBufferSize(fine, 300) == 300, "a fine step takes any size");
  const BufferCaps odd{48, 480, 480, 48};  // the max is valid even off the step
  expect(bufferSizeValid(odd, 480) && chooseBufferSize(odd, 470) == 480, "the maximum is a valid size");
}

void testChannels() {
  ChannelRequest r;
  r.inFirst = 6, r.inCount = 4, r.outMain = 7;
  ChannelPlan p = planChannels(8, 8, r);
  expect(p.inputs == std::vector<int>({6, 7}), "inputs clamp to the device");
  expect(p.mainL == 6 && p.mainR == 7, "the main pair stays inside the device");
  r = ChannelRequest{};
  r.inCount = 32;
  p = planChannels(32, 2, r);
  expect(static_cast<int>(p.inputs.size()) == kMaxInputs, "at most 16 inputs (the engine's)");
  r = ChannelRequest{};
  r.inCount = 0;
  r.outMain = 2;
  r.extras = {{3, 0}, {4, 2}, {5, 6}, {6, 7}, {7, 4}, {-1, 4}};
  p = planChannels(4, 8, r);
  expect(p.inputs.empty(), "inCount 0 opens no inputs");
  // {4,2} overlaps the main pair, {6,7} would need output 8, {-1,4} has no mixer track.
  expect(p.extras == std::vector<ExtraOut>({{3, 0}, {5, 6}, {7, 4}}), "extra pairs: overlapping and out-of-range ones are dropped");
  expect(p.outputs == std::vector<int>({2, 3, 0, 1, 6, 7, 4, 5}), "opened outputs: main pair, then each extra pair");
  expect(p.extraOut(1) == 4 && p.extraOut(5) == -1, "extra pair index into the outputs");
  p = planChannels(1, 1, ChannelRequest{});
  expect(p.mainL == 0 && p.mainR == 0 && p.outputs.size() == 1 && p.inputs.size() == 1, "one-output device: mono main");
  p = planChannels(0, 0, ChannelRequest{});
  expect(p.outputs.empty() && p.inputs.empty(), "no channels: nothing opened");
  expect(parseExtraOuts("3:4, 5:6,bad,7:-1,9:10") == std::vector<ExtraOut>({{3, 4}, {5, 6}, {9, 10}}), "extra outputs parse (bad items skipped)");
  expect(formatExtraOuts({{3, 4}, {5, 6}}) == "3:4,5:6" && parseExtraOuts(formatExtraOuts({{1, 2}})) == std::vector<ExtraOut>({{1, 2}}),
         "extra outputs round trip");
  expect(pairLabel({}, 2, false) == "Out 3 - Out 4" && pairLabel({"A", "B", "C"}, 0, true) == "A - B" && pairLabel({"A", "B", "C"}, 2, true) == "C",
         "pair labels");
}

// Render function of the host tests: out[k] = in[k % nIn] * 0.5 (or a constant without inputs).
struct Probe {
  int calls = 0, lastFrames = 0, lastIn = 0, lastOut = 0;
};
void probeRender(void* user, const float* const* in, int nIn, float* const* out, int nOut, int frames) {
  auto* p = static_cast<Probe*>(user);
  ++p->calls;
  p->lastFrames = frames, p->lastIn = nIn, p->lastOut = nOut;
  for (int k = 0; k < nOut; ++k)
    for (int i = 0; i < frames; ++i) out[k][i] = nIn > 0 ? 0.5f * in[k % nIn][i] : 0.25f;
}

float simInput(long long pos, int channel) { return static_cast<float>((pos + 7 * channel) % 100 - 50) / 100.0f; }

void testHost() {
  SimSpec spec;
  spec.threaded = false;
  spec.inputs = 4, spec.outputs = 6;
  spec.type = abi::kInt32LSB;
  auto owned = std::make_unique<SimDriver>(spec);
  SimDriver* sim = owned.get();
  Probe probe;
  Config cfg;
  cfg.buffer = 100;  // not a power of two: 128
  cfg.rate = 96000;
  cfg.channels.inFirst = 1, cfg.channels.inCount = 2, cfg.channels.outMain = 2;
  cfg.channels.extras = {{5, 4}};
  Host h;
  std::string err;
  expect(h.openDriver(std::move(owned), "sim", cfg, nullptr, probeRender, &probe, &err), "the simulated driver opens");
  expect(h.bufferFrames() == 128 && sim->frames() == 128, "the buffer size is a valid one near the request");
  expect(h.sampleRate() == 96000, "a supported sample rate is set on the device");
  expect(h.device().rates == std::vector<int>({44100, 48000, 88200, 96000}), "the device's sample rates are listed");
  expect(h.inputLatency() == 128 + 24 && h.outputLatency() == 128 + 32, "the reported latencies are read");
  expect(h.plan().inputs == std::vector<int>({1, 2}) && h.plan().outputs == std::vector<int>({2, 3, 4, 5}), "channel plan");
  expect(sim->created() == 6, "buffers for 2 inputs + 4 outputs");
  expect(h.device().inNames.size() == 4 && h.device().outNames[0] == "Analog Out 1", "channel names");
  expect(h.usesOutputReady(), "outputReady is used when the driver supports it");
  expect(h.start(&err), "streaming starts");
  for (int i = 0; i < 3; ++i) sim->step();
  expect(probe.calls == 3 && probe.lastFrames == 128 && probe.lastIn == 2 && probe.lastOut == 4, "each buffer reaches the render function");
  // Third step used half 0 at position 256: device output 2 (main L) = 0.5 * device input 1.
  const auto* o = static_cast<const int32_t*>(sim->buffer(2, 0));
  bool exact = o != nullptr;
  for (int i = 0; exact && i < 128; ++i) {
    const double want = 0.5 * simInput(256 + i, 1) * 2147483648.0;
    if (std::fabs(static_cast<double>(o[i]) - want) > 300.0) exact = false;
  }
  expect(exact, "input -> render -> output keeps the samples (int32 device format)");
  expect(sim->outputReadyCalls() >= 4, "outputReady after each buffer");
  expect(h.poll() == Host::None, "no events while all is well");

  expect(sim->message(abi::kResetRequest) == 1 && (h.poll() & Host::Reset), "a reset request reaches the UI thread");
  sim->setLatencyExtra(100, 200);
  expect(sim->message(abi::kLatenciesChanged) == 1 && (h.poll() & Host::Latency) && h.inputLatency() == 228 && h.outputLatency() == 328,
         "a latency change is re-read");
  expect(sim->message(abi::kResyncRequest) == 1 && (h.poll() & Host::Resync), "a resync request");
  expect(sim->message(abi::kBufferSizeChange, 256) == 1 && (h.poll() & Host::Reset), "a buffer size change asks for a reopen");
  expect(sim->message(abi::kOverload) == 1 && (h.poll() & Host::Overload) && h.overloads() == 1, "overloads are counted");
  expect(sim->message(abi::kSupportsTimeInfo) == 0, "time info is not asked for (plain bufferSwitch)");
  expect(sim->message(9999) == 0, "unknown selectors are not supported");
  sim->changeRate(96000);
  expect(!(h.poll() & Host::RateChanged), "a rate change to the same rate is ignored");
  sim->changeRate(44100);
  expect(h.poll() & Host::RateChanged, "a real rate change asks for a reopen");
  expect(h.controlPanel() && sim->panelCalls() == 1, "the control panel call reaches the driver");
  const long long before = h.callbacks();
  h.close();
  expect(!h.isOpen() && before == 3, "close");
}

void testHostCases() {
  std::string err;
  Probe probe;
  {  // a missing device (unplugged): open fails with a message, nothing crashes
    SimSpec s;
    s.threaded = false;
    s.present = false;
    Host h;
    expect(!h.openDriver(std::make_unique<SimDriver>(s), "gone", Config{}, nullptr, probeRender, &probe, &err) && !err.empty() && !h.isOpen(),
           "an unplugged device fails to open with a message");
  }
  {  // a rate the device lacks: the current rate stays; int24 driver; no outputReady
    SimSpec s;
    s.threaded = false;
    s.inputs = 0, s.outputs = 2;
    s.type = abi::kInt24LSB;
    s.supportsOutputReady = false;
    Config c;
    c.rate = 22050;
    Host h;
    auto d = std::make_unique<SimDriver>(s);
    SimDriver* sim = d.get();
    expect(h.openDriver(std::move(d), "out-only", c, nullptr, probeRender, &probe, &err) && h.sampleRate() == 48000, "an unsupported rate keeps the device's");
    expect(!h.usesOutputReady() && h.plan().inputs.empty(), "output-only device, no outputReady");
    h.start(&err);
    sim->step();
    const auto* o = static_cast<const unsigned char*>(sim->buffer(0, 0));
    expect(o && o[0] == 0x00 && o[1] == 0x00 && o[2] == 0x20, "int24 output written (0.25 = 0x200000)");
  }
  {  // block size invariance: buffers of 64 and 256 give the same output stream
    std::vector<int32_t> streams[2];
    const int sizes[2] = {64, 256};
    for (int t = 0; t < 2; ++t) {
      SimSpec s;
      s.threaded = false;
      Config c;
      c.buffer = sizes[t];
      Host h;
      auto d = std::make_unique<SimDriver>(s);
      SimDriver* sim = d.get();
      h.openDriver(std::move(d), "bs", c, nullptr, probeRender, &probe, &err);
      h.start(&err);
      for (int k = 0; k < 1024 / sizes[t]; ++k) {
        const int half = k & 1;
        sim->step();
        const auto* o = static_cast<const int32_t*>(sim->buffer(2, half));  // first output (after 2 inputs)
        streams[t].insert(streams[t].end(), o, o + sizes[t]);
      }
    }
    expect(streams[0].size() == 1024 && streams[0] == streams[1], "the output does not depend on the buffer size");
  }
  {  // a stalled driver (no callbacks after start) is reported once
    SimSpec s;
    s.threaded = false;
    Host h;
    h.openDriver(std::make_unique<SimDriver>(s), "stall", Config{}, nullptr, probeRender, &probe, &err);
    h.start(&err);
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    const int e1 = h.poll(), e2 = h.poll();
    expect((e1 & Host::Stalled) && !(e2 & Host::Stalled), "a driver that stops calling back is reported once");
  }
  {  // threaded driver: real-time callbacks, then switching to another driver at runtime
    Host h;
    SimSpec s;
    s.buffers = {32, 2048, 64, -1};
    Probe p1;
    expect(h.openDriver(std::make_unique<SimDriver>(s), "rt", Config{}, nullptr, probeRender, &p1, &err) && h.start(&err), "threaded driver starts");
    const auto t0 = std::chrono::steady_clock::now();
    while (h.callbacks() < 20 && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5)) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    expect(h.callbacks() >= 20, "the driver's thread calls back in real time");
    SimSpec s2 = simSpecFor("Gridline Simulated Stereo");
    Probe p2;
    Config c2;
    c2.buffer = 100;
    expect(h.openDriver(std::make_unique<SimDriver>(s2), "stereo", c2, nullptr, probeRender, &p2, &err) && h.start(&err), "switching drivers while running");
    expect(h.bufferFrames() == 128 && h.device().numOut == 2, "the second driver's limits apply (step 64 from 64)");
    const int calls1 = p1.calls;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    expect(p1.calls == calls1, "the first driver's thread is gone");
    h.close();
  }
  {  // only one driver at a time
    SimSpec s;
    s.threaded = false;
    Host a, b;
    a.openDriver(std::make_unique<SimDriver>(s), "a", Config{}, nullptr, probeRender, &probe, &err);
    expect(!b.openDriver(std::make_unique<SimDriver>(s), "b", Config{}, nullptr, probeRender, &probe, &err) && a.isOpen(), "a second host cannot open while one is open");
  }
}

void testDriverList() {
#ifndef _WIN32
  expect(listInstalledDrivers().empty(), "no installed ASIO drivers outside Windows");
#endif
#ifdef _WIN32
  _putenv_s("GRIDLINE_FAKE_ASIO", "1");
#else
  setenv("GRIDLINE_FAKE_ASIO", "1", 1);
#endif
  const std::vector<DriverEntry> v = listDrivers();
  int sims = 0;
  for (const auto& e : v) sims += e.simulated ? 1 : 0;
  expect(sims == 2 && backendAvailable(), "GRIDLINE_FAKE_ASIO lists the simulated drivers");
  Host h;
  Probe p;
  std::string err;
  Config c;
  c.driver = "Gridline Simulated Stereo";
  expect(h.open(c, nullptr, probeRender, &p, &err) && h.device().numIn == 2 && h.bufferFrames() == 128, "a simulated driver opens by name");
  h.close();
  c.driver = "Not Installed";
  expect(!h.open(c, nullptr, probeRender, &p, &err) && err.find("not installed") != std::string::npos, "a missing driver name fails with a message");
}

}  // namespace

int main() {
  testFormats();
  testBuffers();
  testChannels();
  testHost();
  testHostCases();
  testDriverList();
  return finish();
}
