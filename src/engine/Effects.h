// Gridline — mixer insert effect runtime. One instance per occupied slot,
// used only by the audio thread (or the offline renderer).
#pragma once
#include <memory>
#include <string>
#include "../core/PluginInfo.h"
#include "Generators.h"  // ProcessContext

namespace gl {

// Extra audio connections of a mixer slot, set by the engine before every
// processAux() call (all null when unused). Buffers hold the block's frames.
struct SideIo {
  const float* scL = nullptr;  // sidechain input of the slot's mixer track
  const float* scR = nullptr;
  float* sendL = nullptr;      // destination for send effects (added to)
  float* sendR = nullptr;
  const float* recvL = nullptr;  // named audio bus for return effects
  const float* recvR = nullptr;
  float* multiL[kMaxSendOutputs] = {};  // PluginInfo::sendParams outputs (added to; null = unrouted)
  float* multiR[kMaxSendOutputs] = {};
};

class Effect {
 public:
  virtual ~Effect() = default;
  // In-place stereo processing of n frames.
  virtual void process(const PluginState& state, const ProcessContext& ctx, float* L, float* R, int n) = 0;
  // Same with the slot's prepared data (impulse responses etc., see packs::prepareAux).
  // The engine calls this one; effects that need aux data override it.
  virtual void processAux(const PluginState& state, const ChannelAux& aux, const ProcessContext& ctx, float* L, float* R,
                          int n) {
    (void)aux;
    process(state, ctx, L, R, n);
  }
  virtual void reset() = 0;
  // Gain reduction etc. for the UI (dB, <= 0); 0 when not applicable.
  virtual float meter() const { return 0.0f; }
  // Additional per-band meters (e.g. multiband gain reduction); returns count written.
  virtual int meters(float* out, int max) const {
    (void)out;
    (void)max;
    return 0;
  }
  // Detector input level in dB (dynamics editors draw it on the transfer curve).
  virtual float inputLevel() const { return -120.0f; }
  // Sidechain / send / return buffers for the next processAux() (default: ignored).
  virtual void setSideIo(const SideIo& io) { (void)io; }
  // Hosted plugins: see Generator::prepareHost (UI thread, every Program build).
  virtual void prepareHost(const PluginState& state, double sampleRate) {
    (void)state;
    (void)sampleRate;
  }
  // Processing latency in samples (for plugin delay compensation): the live
  // value reported by hosted plugins. Built-in effects report through
  // latencyFor() instead; this returns 0 for them.
  virtual int latencySamples() const { return 0; }
  // Latency (samples) this effect adds to its input when it runs with `state`
  // at `sampleRate` — the delay of a unit impulse through the wet path (and the
  // dry path, which latent effects align internally). Called on the UI thread
  // whenever a Program is built (plugin delay compensation), so it must be a
  // pure function of its arguments and must not touch audio-thread state.
  // Effects whose latency depends on their settings (linear-phase modes,
  // look-ahead, oversampling, FFT sizes) override it; the default forwards
  // latencySamples() so hosted plugins report their own value.
  virtual int latencyFor(const PluginState& state, double sampleRate) const {
    (void)state;
    (void)sampleRate;
    return latencySamples();
  }
  // UI thread, Program build: true when this instance must be replaced (see Generator::stale).
  virtual bool stale() const { return false; }
  // Controller effects: current control outputs (0..1), as listed in
  // PluginInfo::outputs. Returns how many were written. Read after processAux().
  virtual int controlOutputs(float* out, int max) const {
    (void)out;
    (void)max;
    return 0;
  }
};

std::unique_ptr<Effect> createEffect(const std::string& type);
// Tests: an extra factory consulted before the built-in ones (nullptr clears).
// Lets engine tests insert synthetic effects (e.g. a pure latent delay).
using EffectFactory = std::unique_ptr<Effect> (*)(const std::string& type);
void setTestEffectFactory(EffectFactory f);
// Latency of `type` with `state` at `sampleRate`, from a scratch instance
// (UI thread; 0 for unknown types). Editors and the mixer readout use it.
int effectLatency(const PluginState& state, double sampleRate);
// Waveshaper transfer function (shared with its editor).
double waveshaperCurve(const PluginState& s, double x);

}  // namespace gl
