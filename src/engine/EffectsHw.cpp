// Gridline - K-101: Hardware Insert (see EffectsHw.h).
#include "EffectsHw.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include "../core/PluginParams.h"
#include "Program.h"

namespace gl {

namespace {

constexpr size_t kRing = 131072;  // > kMaxLatency, a power of two
static_assert(kRing > static_cast<size_t>(HwInsertFx::kMaxLatency) && (kRing & (kRing - 1)) == 0, "ring");
constexpr float kPingLevel = 0.5f;     // the click (-6 dBFS, one sample)
constexpr float kPingThreshold = 0.05f;  // what counts as its echo (-26 dBFS)

float param(const PluginState& s, int i, float dflt) {
  return i >= 0 && i < static_cast<int>(s.params.size()) ? s.params[static_cast<size_t>(i)] : dflt;
}
int iparam(const PluginState& s, int i, int dflt) { return static_cast<int>(std::lround(param(s, i, static_cast<float>(dflt)))); }
float db(float v) { return std::pow(10.f, v / 20.f); }

}  // namespace

HwInsertFx::HwInsertFx()
    : sendL_(static_cast<size_t>(kEngineMaxBlock), 0.f), sendR_(static_cast<size_t>(kEngineMaxBlock), 0.f),
      dryL_(kRing, 0.f), dryR_(kRing, 0.f) {}

void HwInsertFx::reset() {
  std::fill(dryL_.begin(), dryL_.end(), 0.f);
  std::fill(dryR_.begin(), dryR_.end(), 0.f);
  std::fill(sendL_.begin(), sendL_.end(), 0.f);
  std::fill(sendR_.begin(), sendR_.end(), 0.f);
  pingLeft_ = 0;
}

int HwInsertFx::latencyFor(const PluginState& state, double sampleRate) const {
  (void)sampleRate;
  return std::clamp(iparam(state, pp::HwLatency, 0) + iparam(state, pp::HwOffset, 0), 0, kMaxLatency);
}

bool HwInsertFx::directOutput(int& pair, const float*& L, const float*& R) const {
  if (sendPair_ < 0 || !fresh_) return false;  // (a bypassed or skipped insert sends nothing)
  fresh_ = false;
  pair = sendPair_;
  L = sendL_.data();
  R = sendR_.data();
  return true;
}

int HwInsertFx::meters(float* out, int max) const {
  const float v[3] = {static_cast<float>(pingResult_.load(std::memory_order_relaxed)), pingLeft_ > 0 ? 1.f : 0.f,
                      connected_.load(std::memory_order_relaxed) ? 1.f : 0.f};
  const int k = std::min(max, 3);
  for (int i = 0; i < k; ++i) out[i] = v[i];
  return k;
}

void HwInsertFx::process(const PluginState& state, const ProcessContext& ctx, float* L, float* R, int n) {
  using namespace pp;
  n = std::min(n, kEngineMaxBlock);
  const int send = iparam(state, HwSend, 1) - 1;  // -1: none (an audio return only)
  const int ret = iparam(state, HwReturn, 0);     // input pair: channels 2 ret, 2 ret + 1
  const bool mono = param(state, HwReturnMono, 0) >= 0.5f;
  const int firstIn = 2 * ret;
  const bool haveSend = send >= 0 && send < ctx.directOutPairs;
  const bool haveReturn = ctx.deviceIn && firstIn < ctx.deviceInCount;
  const bool ok = !ctx.offline && haveReturn && (send < 0 || haveSend);
  connected_.store(ok, std::memory_order_relaxed);
  sendPair_ = ok && haveSend ? send : -1;
  fresh_ = sendPair_ >= 0;
  if (!ok) {  // nothing to send to or listen on: pass the input on (and nothing is delayed)
    pingLeft_ = 0;
    return;
  }
  const float sg = db(param(state, HwSendGain, 0)), rg = db(param(state, HwReturnGain, 0));
  const float mix = std::clamp(param(state, HwMix, 1), 0.f, 1.f);
  const int lat = latencyFor(state, ctx.sampleRate);
  const float* inL = ctx.deviceIn[firstIn];
  const float* inR = !mono && firstIn + 1 < ctx.deviceInCount ? ctx.deviceIn[firstIn + 1] : inL;
  // A ping asked for: the send carries one click (and nothing else) until its echo is back.
  const int req = pingRequest_.load(std::memory_order_relaxed);
  bool click = false;
  if (req != pingSeen_) {
    pingSeen_ = req;
    if (sendPair_ >= 0) {
      pingLeft_ = static_cast<int64_t>(ctx.sampleRate);  // listen for a second
      pingElapsed_ = 0;
      click = true;
    }
  }
  for (int i = 0; i < n; ++i) {
    const size_t u = static_cast<size_t>(i);
    // The send (before the dry path's delay: what leaves now comes back `lat` later).
    if (pingLeft_ > 0) {
      sendL_[u] = sendR_[u] = click && i == 0 ? kPingLevel : 0.f;
    } else {
      sendL_[u] = L[u] * sg;
      sendR_[u] = R[u] * sg;
    }
    // Listen for the echo (from the frame after the click).
    if (pingLeft_ > 0) {
      if (!(click && i == 0) && (std::fabs(inL[u]) > kPingThreshold || std::fabs(inR[u]) > kPingThreshold)) {
        pingResult_.store(static_cast<int>(std::min<int64_t>(pingElapsed_, kMaxLatency)), std::memory_order_relaxed);
        pingSerial_.fetch_add(1, std::memory_order_relaxed);
        pingLeft_ = 0;
      } else if (--pingLeft_ == 0) {
        pingResult_.store(-2, std::memory_order_relaxed);
        pingSerial_.fetch_add(1, std::memory_order_relaxed);
      }
      ++pingElapsed_;
    }
    // The dry path waits as long as the round trip, so dry and wet stay together.
    dryL_[dryPos_] = L[u];
    dryR_[dryPos_] = R[u];
    const size_t at = (dryPos_ + kRing - static_cast<size_t>(lat)) & (kRing - 1);
    dryPos_ = (dryPos_ + 1) & (kRing - 1);
    const float dl = dryL_[at], dr = dryR_[at];
    L[u] = dl * (1.f - mix) + inL[u] * rg * mix;
    R[u] = dr * (1.f - mix) + inR[u] * rg * mix;
  }
}

std::unique_ptr<Effect> createEffectHw(const std::string& type) {
  if (type == "gl.hwinsert") return std::make_unique<HwInsertFx>();
  return nullptr;
}

}  // namespace gl
