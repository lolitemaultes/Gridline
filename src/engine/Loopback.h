// Gridline - K-123 (0.7.7): the loopback latency check. A cable from an output back into an input;
// Gridline plays a short noise burst (AudioEngine::startLatencyProbe) and listens for it. The lag
// where the input matches the burst best is the round trip, in samples of the engine's own clock:
// what a recording has to move a take earlier by (before plugin delay compensation).
#pragma once
#include <string>
#include <vector>

namespace gl::loopback {

// The burst: deterministic noise with short raised-cosine edges (sharp to correlate, gentle on
// speakers left on), about 43 ms long at any rate.
std::vector<float> probeSignal(double sampleRate);
// Frames to listen after the burst starts (the longest round trip measured: a little over 1 s).
size_t listenFrames(double sampleRate);

struct Lag {
  bool found = false;
  int frames = 0;          // the round trip
  bool inverted = false;   // the input comes back upside down
  float match = 0;         // normalised correlation at the lag, 0..1
  float level = 0;         // peak of what came back (linear)
};
// The lag of `sent` inside `got` (got[i + lag] ~ sent[i]). Found when the match is at least 0.5.
Lag findLag(const std::vector<float>& sent, const std::vector<float>& got);

// Several measurements in a row: the result when at least two agree within 2 frames (the median of
// the agreeing ones), else none.
struct Summary {
  bool ok = false;
  int frames = 0;
  int agree = 0, total = 0;
};
Summary summarise(const std::vector<Lag>& runs);

// The setup a measurement belongs to (output device, input device, rate, buffer): a measured
// round trip only applies while the same setup plays.
std::string setupKey(const std::string& output, const std::string& input, double sampleRate, int buffer);

// The samples a recording is moved earlier by: the device round trip (measured when it applies,
// else the automatic estimate), plus the plugin delay compensation, less the manual offset.
int recordLatency(int automaticRoundTrip, int measuredRoundTrip, int pluginDelay, int manualOffset);

}  // namespace gl::loopback
