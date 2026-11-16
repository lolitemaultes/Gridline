// Gridline - K-101: Hardware Insert. A mixer effect that sends the track to a pair of the audio
// device's outputs (an outboard compressor, a pedal, a tape machine), takes it back from a pair
// of the device's inputs and reports the round trip as its latency, so plugin delay
// compensation lines it up with the rest of the mix. The round trip is measured with a ping:
// one click goes out of the send, and the samples until it comes back are the latency.
// With the send set to None it is an audio return only: the sound of an external instrument
// that a MIDI Out channel plays, with that instrument's latency compensated the same way.
//
// Offline (export) and on a device without the outputs or inputs it names, the insert passes
// its input on unchanged (what comes back from outboard gear cannot be rendered offline).
#pragma once
#include <atomic>
#include <vector>
#include "Effects.h"

namespace gl {

class HwInsertFx final : public Effect {
 public:
  HwInsertFx();
  void process(const PluginState& state, const ProcessContext& ctx, float* L, float* R, int n) override;
  void reset() override;
  int latencyFor(const PluginState& state, double sampleRate) const override;
  bool directOutput(int& pair, const float*& L, const float*& R) const override;
  // bands: [0] the last ping's result in samples (-1: none yet, -2: no echo), [1] 1 while a ping
  // runs, [2] 1 when the device has the send and return the insert names.
  int meters(float* out, int max) const override;

  // UI thread: start a ping (the next blocks send the click and listen for it).
  void requestPing() { pingRequest_.fetch_add(1, std::memory_order_relaxed); }
  // UI thread: the last ping's result (samples; -1 none, -2 no echo within a second) and a
  // counter that moves each time a ping ends.
  int pingResult() const { return pingResult_.load(std::memory_order_relaxed); }
  int pingSerial() const { return pingSerial_.load(std::memory_order_relaxed); }
  bool connected() const { return connected_.load(std::memory_order_relaxed); }

  static constexpr int kMaxLatency = 96000;  // samples the dry path can wait (1 s at 96 kHz)

 private:
  std::vector<float> sendL_, sendR_;
  std::vector<float> dryL_, dryR_;  // the dry path's delay line (kDelay frames, ring)
  size_t dryPos_ = 0;
  int sendPair_ = -1;
  mutable bool fresh_ = false;  // the send holds this block's frames (process ran since the engine took them)
  // Ping (audio thread; the request counter and the result are read across threads).
  std::atomic<int> pingRequest_{0};
  int pingSeen_ = 0;
  int64_t pingLeft_ = 0;     // frames still listened for (0: no ping running)
  int64_t pingElapsed_ = 0;  // frames since the click went out
  std::atomic<int> pingResult_{-1};
  std::atomic<int> pingSerial_{0};
  std::atomic<bool> connected_{false};
};

}  // namespace gl
