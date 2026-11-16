// Gridline - K-101: Hardware Insert. A simulated device loops the insert's send back to its
// return with a known delay: the ping measures that delay, the reported latency follows the
// setting, the dry path lines up with the returned sound, and the engine sends the insert's
// audio to its output pair (not the mix) and hands it the device input.
#include <cmath>
#include <deque>
#include <vector>
#include "Check.h"
#include "core/Document.h"
#include "core/PluginInfo.h"
#include "core/PluginParams.h"
#include "core/Params.h"
#include "core/Song.h"
#include "engine/AudioEngine.h"
#include "engine/EffectsHw.h"
#include "engine/Program.h"

using namespace gl;
using namespace gl::pp;

namespace {

constexpr double kSr = 48000.0;

PluginState hwState(std::initializer_list<std::pair<int, float>> set) {
  PluginState s = defaultPluginState("gl.hwinsert");
  for (const auto& [k, v] : set) s.params[static_cast<size_t>(k)] = v;
  return s;
}

// The insert on a device whose output pair 0 comes back on inputs 1-2 `delay` frames later.
struct Loop {
  HwInsertFx fx;
  int delay;
  std::deque<float> wire;  // what is in the cable (left only; right copies it)
  std::vector<float> inL, inR;
  const float* ins[2];
  explicit Loop(int d) : delay(d), wire(static_cast<size_t>(d), 0.f) {}
  // One block: the device input is what left `delay` frames ago.
  void block(const PluginState& s, float* L, float* R, int n, bool offline = false, int pairs = 1) {
    inL.assign(static_cast<size_t>(n), 0.f);
    for (int i = 0; i < n; ++i) {
      inL[static_cast<size_t>(i)] = wire.front();
      wire.pop_front();
      wire.push_back(0.f);  // filled after the block with what the insert sent
    }
    inR = inL;
    ins[0] = inL.data(), ins[1] = inR.data();
    ProcessContext ctx;
    ctx.sampleRate = kSr;
    ctx.offline = offline;
    ctx.deviceIn = ins;
    ctx.deviceInCount = 2;
    ctx.directOutPairs = pairs;
    fx.process(s, ctx, L, R, n);
    int pair = -1;
    const float* sl = nullptr;
    const float* sr = nullptr;
    if (fx.directOutput(pair, sl, sr) && pair == 0)
      for (int i = 0; i < n; ++i) wire[wire.size() - static_cast<size_t>(n - i)] = sl[i];
  }
};

void testPing() {
  for (int delay : {300, 1250, 4096}) {
    Loop lp(delay);
    const PluginState s = hwState({});
    std::vector<float> L(128), R(128);
    lp.fx.requestPing();
    for (int b = 0; b < 100; ++b) lp.block(s, L.data(), R.data(), 128);
    expect(lp.fx.pingResult() == delay && lp.fx.pingSerial() == 1, ("the ping measures the round trip: " + std::to_string(delay)).c_str());
  }
  // Nothing comes back: no echo within a second.
  Loop lp(300);
  const PluginState s = hwState({{HwReturn, 1.f}});  // listening on inputs 3-4, which the device lacks
  std::vector<float> L(128), R(128);
  lp.fx.requestPing();
  for (int b = 0; b < 400; ++b) lp.block(s, L.data(), R.data(), 128);
  expect(lp.fx.pingResult() == -1 && !lp.fx.connected(), "a return the device does not have: the insert is not connected, no ping");
  Loop deaf(300);
  deaf.fx.requestPing();
  const PluginState muted = hwState({});
  for (int b = 0; b < 400; ++b) {
    deaf.block(muted, L.data(), R.data(), 128);
    std::fill(deaf.wire.begin(), deaf.wire.end(), 0.f);  // the cable is unplugged
  }
  expect(deaf.fx.pingResult() == -2 && deaf.fx.pingSerial() == 1, "no echo within a second: the ping fails");
}

void testLatencyAndAlignment() {
  HwInsertFx fx;
  expect(fx.latencyFor(hwState({{HwLatency, 777.f}}), kSr) == 777, "the reported latency is the round trip setting");
  expect(fx.latencyFor(hwState({{HwLatency, 777.f}, {HwOffset, -100.f}}), kSr) == 677, "plus the offset");
  expect(fx.latencyFor(hwState({}), kSr) == 0, "a new insert reports none");
  // Half dry, half wet: an impulse comes out once, whole, `delay` later.
  const int delay = 500;
  Loop lp(delay);
  const PluginState s = hwState({{HwLatency, static_cast<float>(delay)}, {HwMix, 0.5f}});
  std::vector<float> outL;
  for (int b = 0; b < 20; ++b) {
    std::vector<float> L(128, 0.f), R(128, 0.f);
    if (b == 2) L[10] = R[10] = 1.f;
    lp.block(s, L.data(), R.data(), 128);
    outL.insert(outL.end(), L.begin(), L.end());
  }
  const size_t at = 2 * 128 + 10 + delay;
  double other = 0;
  for (size_t i = 0; i < outL.size(); ++i)
    if (i != at) other += std::fabs(outL[i]);
  expect(std::fabs(outL[at] - 1.f) < 1e-6 && other < 1e-6, "dry and returned sound line up");
  // Send and return levels.
  Loop g(200);
  const PluginState sg = hwState({{HwSendGain, -6.f}, {HwReturnGain, -6.f}});
  std::vector<float> L(128, 0.5f), R(128, 0.5f);
  for (int b = 0; b < 4; ++b) {
    std::fill(L.begin(), L.end(), 0.5f), std::fill(R.begin(), R.end(), 0.5f);
    g.block(sg, L.data(), R.data(), 128);
  }
  expect(std::fabs(L[100] - 0.5f * std::pow(10.f, -12.f / 20.f)) < 1e-4, "send and return gains");
}

void testOfflineAndReturnOnly() {
  Loop lp(100);
  std::vector<float> L(128, 0.25f), R(128, 0.25f);
  lp.block(hwState({}), L.data(), R.data(), 128, true);
  int pair = -1;
  const float* a = nullptr;
  const float* b = nullptr;
  expect(L[50] == 0.25f && !lp.fx.directOutput(pair, a, b), "offline: the input passes on, nothing is sent");
  Loop nop(100);
  std::fill(L.begin(), L.end(), 0.25f);
  nop.block(hwState({}), L.data(), R.data(), 128, false, 0);
  expect(L[50] == 0.25f && !nop.fx.connected(), "a device without the output pair: the input passes on");
  // Return only (an external instrument a MIDI Out channel plays): the device input comes out.
  Loop ret(100);
  ret.wire.assign(100, 0.3f);
  std::fill(L.begin(), L.end(), 0.f);
  ret.block(hwState({{HwSend, 0.f}}), L.data(), R.data(), 100);
  expect(std::fabs(L[50] - 0.3f) < 1e-6 && ret.fx.connected() && !ret.fx.directOutput(pair, a, b), "return only: the device input, nothing sent");
}

void testEngine() {
  Document d(makeEmptySong());
  if (d.song().mixer.size() < 2) d.addMixerTrack();
  const ObjectId tr = d.song().mixer[1].id;
  expect(d.setSlotPlugin(tr, 0, "gl.hwinsert"), "a Hardware Insert on insert 1");
  d.setParam(slotParam(tr, 0, HwLatency), 256, false);
  ProgramBuilder b;
  AudioEngine eng{kSr};
  eng.setProgram(b.build(d.song()));
  expect(eng.outputLatency() >= 256, "plugin delay compensation counts the insert's round trip");
  eng.setInputChannels(2);
  std::vector<float> mL(256), mR(256), tL(256), tR(256);
  float* tl[1] = {tL.data()};
  float* tr2[1] = {tR.data()};
  const int mix[1] = {-1};
  AudioEngine::OutputTaps taps;
  taps.count = 1, taps.mixer = mix, taps.L = tl, taps.R = tr2;
  auto rt = b.effectRuntime(tr, 0);
  auto* fx = rt ? dynamic_cast<HwInsertFx*>(rt->fx.get()) : nullptr;
  expect(fx != nullptr, "the insert runs in the mixer");
  if (!fx) return;
  fx->requestPing();
  std::vector<float> back(512, 0.f);  // device input: the pair's output a block later
  bool clicked = false;
  for (int k = 0; k < 400; ++k) {
    eng.pushInput(back.data(), 256);
    eng.process(mL.data(), mR.data(), 256, &taps);
    for (int i = 0; i < 256; ++i) {
      clicked = clicked || tL[static_cast<size_t>(i)] > 0.4f;
      back[static_cast<size_t>(2 * i)] = tL[static_cast<size_t>(i)];
      back[static_cast<size_t>(2 * i + 1)] = tR[static_cast<size_t>(i)];
    }
  }
  expect(clicked, "the ping's click reaches the output pair");
  expect(fx->pingResult() > 0 && fx->pingResult() < 48000, "the engine's ping comes back through the device input");
}

}  // namespace

int main() {
  testPing();
  testLatencyAndAlignment();
  testOfflineAndReturnOnly();
  testEngine();
  return finish();
}
