// Gridline — Trio Synth "Reference" oscillators (J-65): the oscillator
// section of the reference three-oscillator instrument, rebuilt from its
// measured behaviour (sample by sample against the instrument run locally).
//   Pitch: 32-bit phase accumulators; increment = round(2^(cents/1200) · base)
//   with base = 2^32 · 523.2511 Hz / sample rate (the note's key + the
//   oscillator's coarse / fine, in cents, relative to C5 — which sounds at
//   523.25 Hz like the reference).
//   Levels: osc 1 (1 − m2)(1 − m3), osc 2 m2 (1 − m3), osc 3 m3 (with osc 3 as
//   AM: 1 − m2, m2, m3), invert = negative level; per-oscillator pan
//   cos / sin((p + 1) π / 4); stereo phase = right-side phase offset; stereo
//   detune = right-side pitch offset (cents). AM multiplies the sum so far by
//   (1 − osc 3 · level).
//   Classic oscillators: 16384-sample one-cycle tables read without
//   interpolation (phase >> 18). High-quality oscillators: quarter-octave mip
//   tables (32768 samples per cycle, harmonics up to 1024 / 2^(level / 4)),
//   chosen per block from the pitch, read with linear interpolation; sine
//   computed directly; noise from a running generator; below 22 Hz the plain
//   shapes are computed.
//   Note end: a 10 ms raised-cosine fade (the reference's release declick).
// Tables are built once (UI thread: packs::prepareAux) and shared.
#pragma once
#include <cstdint>
#include <memory>
#include <vector>
#include "../../core/PluginInfo.h"
#include "../../engine/Generators.h"

namespace gl::packs::synths::trioref {

// Reference shapes: 0 sine, 1 triangle, 2 square, 3 saw, 4 rounded saw, 5 noise, 6 pulse 25 % (Gridline), 7 custom.
enum Shape { RSine, RTri, RSquare, RSaw, RRound, RNoise, RPulse, RCustom };

struct Tables final : AuxData {
  static constexpr int kClassic = 16384, kMipLen = 32768, kLevels = 44;
  std::vector<float> classic[7];          // kClassic + 2 each
  std::vector<float> mip[4][kLevels];     // triangle, square, saw, rounded saw; kMipLen + 2 each (hq only)
  bool hq = false;
};
// Shared tables (built on first call; `hq` adds the mip tables). UI thread.
std::shared_ptr<const Tables> tables(bool hq);

struct Setup {
  int shape[3] = {};
  int cents[3] = {};
  float level[3] = {}, L[3] = {}, R[3] = {};
  uint32_t phaseR[3] = {};
  int detune[3] = {};
  int amOsc = -1;      // oscillator index that amplitude-modulates (2) or −1
  bool hq = false, stereo = false;
  bool custom = false;  // a custom-wave oscillator (not modelled: callers keep their own engine)
  float base = 0;      // increment for 523.25 Hz
  double sr = 44100;
};
// From a Trio Synth state (Gridline parameters, as the import writes them).
Setup setup(const PluginState& s, double sampleRate);

struct Voice {
  uint32_t ph[2][3] = {};
};
void start(Voice& v, const Setup& s);
// Renders n frames into the interleaved buffer `buf` (2n floats, overwritten);
// `cents` = the note's pitch relative to C5 in cents; `noiseSeed` = the
// instrument's running noise generator (shared by its voices).
void render(Voice& v, const Setup& s, const Tables& t, float cents, float* buf, int n, uint32_t& noiseSeed);
// Release declick gain for frame k after the note end (0 from 10 ms on).
float declick(int k, double sampleRate);
inline int declickLength(double sampleRate) { return static_cast<int>(sampleRate * 0.01); }

}  // namespace gl::packs::synths::trioref
