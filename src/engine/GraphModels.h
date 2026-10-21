// Gridline — J-68: the models behind live editor graphics.
//
// Each function turns a plugin state into what its editor graphic shows,
// computed from the same laws (and, where possible, the same functions) the
// engine runs, so the pictures are truthful: decay views from the reverb
// networks' loop gains, transfer curves from the dynamics / distortion laws,
// filter responses from the biquads actually designed. Pure functions of the
// state (UI thread; tests compare them with the engines' measured output).
#pragma once
#include <array>
#include <cstdint>
#include "../core/PluginInfo.h"

namespace gl::graphmodel {

// ---------------------------------------------------------------- reverb (gl.reverb, all three networks)
struct ReverbView {
  int model = 0;             // 0 hall, 1 plate, 2 full
  double preSec = 0;         // pre-delay
  double decayKnob = 1.5;    // the decay control in seconds (read-out)
  double rt60 = 1.5;         // mid-band T60 (s) the network actually produces
  double rt60Low = 1.5;      // T60 below the bass crossover (bass multiplier)
  double rt60High = 1.5;     // T60 at kHighHz (damping in the loop)
  double crossHz = 500;      // bass crossover
  double dampHz = 0;         // damping cutoff (0 = off)
  double size = 0.5;         // room size 0..1
  double diffusion = 1;      // 0..1
  double modDepth = 0;       // 0..1 (display only)
  double dry = 1, er = 0.3, wet = 0.25;  // linear output levels
  double spread = 1;         // Q-5: stereo spread of the tail (0 mono, 1 as recorded, 2 widened)
  double color = 0;          // Q-5: hall colour tilt -1 (dark) .. 1 (bright)
  int freeze = 0;            // hall: 0 off, 1 freeze, 2 sustain
  int erCount = 0;           // early reflections (time after the input, gain incl. the ER level)
  std::array<double, 16> erSec{}, erGain{};
  static constexpr double kHighHz = 6000;
};
ReverbView reverbView(const PluginState& st, double sampleRate, double tempo);
// Relative tail amplitude (0..1) at `sec` after the pre-delay for a T60.
double decayAmp(double sec, double rt60);

// ---------------------------------------------------------------- stereo delay reference (gl.delay, DlyModel 3)
// The loop distortion's static transfer (limit: the envelope limiter's steady
// state for a constant input; saturate: the quadratic knee with make-up).
double delay3Transfer(int distType, double thrN, double hard, double sym, double x);

// ---------------------------------------------------------------- filter (gl.filter, modern model)
// Response (dB) of the state-variable / shelf filter as the engine runs it:
// TPT SVF taps combined as complex values (blend levels, notch = low + high),
// the steep second stage, shelves / peak as biquads, then the dry / wet mix.
double filterResponseDb(const PluginState& st, double sampleRate, double hz);

// ---------------------------------------------------------------- low lifter (gl.lowlift)
// The linear part of the band split at hz: Linkwitz-Riley low / high bands
// with their levels, the low cut and (legacy) the boost shelf, mixed with the
// dry signal as the engine mixes them. The reference engine's low band is
// level-normalised before shaping: its curve is the nominal (full-scale) gain.
struct LowLiftResponse {
  double low = 0, high = 0, total = 0;  // linear magnitudes
};
LowLiftResponse lowLiftResponse(const PluginState& st, double sampleRate, double hz);

// ---------------------------------------------------------------- limiter compressor (gl.limiter)
// Engine law (EffectsDynK2.cpp): gain change in dB for a detector RMS level in dBFS.
bool limiterCompOn(const PluginState& st);
double limiterCompGainDb(const PluginState& st, double levelDb);

// ---------------------------------------------------------------- multiband compressor (gl.multiband)
// Band filter magnitudes (linear) at count frequencies, from the engine's designs.
void multibandBandResponses(const PluginState& st, double sampleRate, const double* hz, int count, double* low, double* mid, double* high);

}  // namespace gl::graphmodel
