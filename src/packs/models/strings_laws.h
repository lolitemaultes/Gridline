// Gridline — "models" pack, J-22 group "strings": value laws of the Twin
// String / Plucker / Step Bass knobs (0..1 positions → engine units). Shared
// by the runtimes and the editors' read-outs. Evidence notes per law.
#pragma once
#include <algorithm>
#include <cmath>

namespace gl::packs::models::strs::law {

inline double clamp01(double x) { return std::clamp(x, 0.0, 1.0); }
inline double expMap(double x, double lo, double hi) { return lo * std::pow(hi / lo, clamp01(x)); }

// ---- Twin String
// Envelope times (amp, high-cut, free envelope): exponential, 1 ms .. 8 s
// (attack) / 4 ms .. 16 s (decay, release). Presets put long pads near 0.7..1.
inline double tsAttack(double x) { return x <= 0.0 ? 0.0005 : expMap(x, 0.001, 8.0); }
inline double tsDecay(double x) { return expMap(x, 0.004, 16.0); }
inline double tsRelease(double x) { return expMap(x, 0.004, 16.0); }
// Master level: −24 dB at 0 (presets park it at 0 and still sound) .. +6 dB.
inline double tsLevelGain(double x) { return std::pow(10.0, (-24.0 + 30.0 * clamp01(x)) / 20.0); }
// String ring time (T60, s); 1.0 = practically undamped (bowed / pad presets
// sit at 0.97..1 and rely on the amp envelope).
inline double tsStringT60(double x) { return x >= 0.999 ? 120.0 : expMap(x, 0.02, 40.0); }
// High / low cut cutoff (Hz) from a 0..1 position (log 20 Hz .. 20 kHz).
inline double tsCutHz(double x) { return expMap(x, 20.0, 20000.0); }
// Exciter filters track the note: high cut from the fundamental up ten
// octaves; low cut from six octaves below up to the fundamental (presets park
// it at 1.0 on bright patches without losing the note).
inline double tsHiCutHz(double x, double f0) { return f0 * std::pow(2.0, 10.0 * clamp01(x)); }
inline double tsLoCutHz(double x, double f0) { return f0 * std::pow(2.0, 6.0 * (clamp01(x) - 1.0)); }
// Filter resonance → SVF resonance 0..0.97.
inline double tsReso(double x) { return 0.97 * clamp01(x); }
// Exciter noise rate: noise sample-and-hold frequency, 200 Hz .. full rate.
inline double tsNoiseHz(double x, double sr) { return std::min(sr, expMap(x, 200.0, 48000.0)); }
// Free LFO rate (Hz).
inline double tsLfoHz(double x) { return expMap(x, 0.05, 25.0); }
// Resonator band frequency (Hz).
inline double tsResHz(double x) { return expMap(x, 40.0, 8000.0); }
// Comb delay as a fraction of the note period: 0..1 → ¼ .. 4 periods (log).
inline double tsCombPeriods(double x) { return expMap(x, 0.25, 4.0); }
// String B frequency ratio (raw 0..6).
inline double tsRatio(int k) {
  static const double r[7] = {0.25, 1.0 / 3.0, 0.5, 1.0, 2.0, 3.0, 4.0};
  return r[std::clamp(k, 0, 6)];
}
// Fine tune ±1 semitone. Octave transpose: the reference stores a free knob
// position 0..1 that its engine snaps to 7 octave steps (−3..+3);
// Gridline stores the step (0..6, 3 = none).
inline double tsFineSemis(double x) { return (clamp01(x) - 0.5) * 2.0; }
// Octave wheel: the knob position truncated to 7 steps (0 = −3 oct … 6 = +3 oct).
inline int tsOctaveStep(double pos) { return std::clamp(static_cast<int>(clamp01(pos) * 6.0), 0, 6); }
inline double tsTransposeSemis(int step) { return 12.0 * (std::clamp(step, 0, 6) - 3); }
// Delay time (s) when free; sync steps in beats.
inline double tsDelaySec(double x) { return expMap(x, 0.01, 2.0); }
inline double tsDelayBeats(int k) {
  static const double b[10] = {0, 0.25, 0.5, 0.75, 1.0, 2.0, 3.0, 4.0, 8.0, 16.0};
  return b[std::clamp(k, 0, 9)];
}
// Reverb decay (s).
inline double tsReverbSec(double x) { return expMap(x, 0.3, 12.0); }

// ---- Plucker
// Decay 0..256 → T60 (s) at A4 (normalized) / at the note.
inline double pkT60(double v) { return expMap(v / 256.0, 0.08, 12.0); }

// ---- Step Bass (patch floats 0..1)
inline double sbCutoffHz(double x) { return expMap(x, 40.0, 9000.0); }
inline double sbDecaySec(double x) { return expMap(x, 0.04, 3.0); }
inline double sbHighPassHz(double x) { return expMap(x, 5.0, 600.0); }
inline double sbLfoHz(double x) { return expMap(x, 0.05, 12.0); }

}  // namespace gl::packs::models::strs::law
