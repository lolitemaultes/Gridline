// Gridline: harmonic / percussive separation for the hybrid stretch (J-145).
//
// Median filtering of the short-time magnitude spectrum (Fitzgerald, DAFx
// 2010): a median across time keeps what is steady (tones: harmonic), a median
// across frequency keeps what is broadband and short (hits: percussive). Soft
// (Wiener) masks of the two estimates split the sound so that the harmonic and
// percussive parts add back to the input exactly.
//
// The split lets each part go through the engine that suits it (Driedger,
// Muller & Ewert 2014, "Improving time-scale modification of music signals
// using harmonic-percussive separation"): the harmonic part through one
// continuous engine that is never restarted (bass notes and pads flow through
// drum hits without a dip), the percussive part through a time-domain engine
// restarted at each hit (hits copied, never smeared or doubled).
//
// One mask for all channels (from their mix), so the stereo image stays put.
// Offline / UI thread (allocates).
#pragma once
#include <cstdint>
#include <memory>
#include <vector>

namespace gl::pitch {

struct HpssSettings {
  double windowSec = 0.046;     // analysis window (a power of two at the rate, 2048 frames at 44.1 kHz)
  double timeSec = 0.2;         // median length across time (harmonic estimate)
  double freqHz = 360.0;        // median length across frequency (percussive estimate)
  // Weight of the percussive estimate against the harmonic one. Below 1, what is
  // ambiguous stays harmonic (it is never restarted): the dense low harmonics of
  // a bass blur the median across frequency, and at 1 a third of a bass line
  // would be taken for hits. 0.35 keeps hits whole (within 1 dB above 2 kHz).
  double percussiveBias = 0.35;
  // Onsets (input frames, ascending), optional: what is new at a hit is
  // percussive. From the first frame that reaches a hit, the harmonic part
  // may not grow over what it held just before it (+2 dB) until the analysis
  // window has passed the hit, then it takes up the rest over rampSec. The
  // attack of a hit (a kick's body, a new note) then goes through the engine
  // restarted at the hit, not through the continuous one, which would smear
  // it ahead of the hit; what sounded before the hit stays harmonic.
  const std::vector<int64_t>* onsets = nullptr;
  double rampSec = 0.06;
};

// h[c][0, n) = the harmonic part of x[c][0, n); the percussive part is x - h.
void harmonicPart(const float* const* x, int nch, int64_t n, double sampleRate, float* const* h, const HpssSettings& s = {});

}  // namespace gl::pitch
