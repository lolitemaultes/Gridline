// Gridline: K-61, slowing down low monophonic sounds (a bass line) in a clip's offline render.
//
// A spectral stretch keeps a bass's partials but not how they line up in time: the waveform's peaks
// spread and its attacks soften. A sound that repeats one period after another does not need that:
// slowing it down can repeat whole periods. This stretch overlaps pieces of the source itself (60 ms
// windows, 15 ms apart), each placed where it lines up with the piece before (waveform-similarity
// overlap-add, a method from the published literature), so the waveform stays the source's. It lasts
// only as long as the sound stays periodic, so lowTonal() decides from the audio, per clip. Measured
// on a real bass and on synthetic ones against the spectral stretch at the same setting: about 4 dB
// less noise between the partials, about 2 dB more of the waveform's crest factor kept, attacks with
// less ahead of them.
//
// Only for slowing down (the time line of the engine, see inRange()): speeding up has to skip pieces
// of the source, and the spectral stretch keeps a bass's level better there (measured: twice the
// level error otherwise). Everything else plays through the spectral stretch as before.
#pragma once
#include <cstdint>
#include <functional>

namespace gl::stretch {

// Input frames per engine frame this stretch is used at.
inline bool lowTonalInRange(double inPerEngineFrame) { return inPerEngineFrame >= 0.25 && inPerEngineFrame <= 0.9; }

// Is `n` frames of `nch` channels (at `sr`) one low periodic sound: almost all of its energy low,
// periodic with a fundamental of 30 to 300 Hz, and (stereo) close to mono?
bool lowTonal(const float* const* in, int nch, int64_t n, double sr);
// What it decides from, each 0..1: the channels' difference against their sum, the share of the
// energy below about 1 kHz, the share of the energy in periodic frames (tests, diagnostics).
struct LowTonalMeasure {
  double side = 1, low = 0, periodic = 0;
};
LowTonalMeasure measureLowTonal(const float* const* in, int nch, int64_t n, double sr);

// Renders frames [0, len) whose frame q plays input position x(q), as the spectral stream does (a
// continuous stretch that begins on the input leading into x(0); silence past the input's end).
// `out` must be silent; every frame below the value given to `progress` is final.
void lowTonalStream(const float* const* in, int nch, int64_t n, float* const* out, int64_t len, double sr,
                    const std::function<double(int64_t)>& x, const std::function<void(int64_t)>* progress);

}  // namespace gl::stretch
