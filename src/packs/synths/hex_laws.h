// Gridline — "synths" pack, J-33 group "hex": value laws of Hex Synth (the
// reference instrument's laws, from its parameter handlers), shared by the
// runtime, the editor's readouts and the FL decoder. x = stored 0..1 position.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace gl::packs::synths::hex::law {

inline int truncInt(double v) { return static_cast<int>(v); }  // toward zero, like the reference
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Shared time law (envelopes): seconds.
inline double time(double x) { return std::max(50.0 * x * x * x * x, 0.00101); }
inline double releaseTime(double x) { return std::max(0.35 * time(x) + 0.02, 0.01); }  // master release
inline double gainDb(double x) { return 60.0 * x - 30.0; }
inline double gain(double x) { return std::pow(10.0, gainDb(x) / 20.0); }
inline double drive(double x) { return 20.0 * x * x; }
inline int transpose(double x) { return clampi(truncInt(-24.0 + 48.0 * x), -24, 24); }  // semitones (also env → pitch)
inline double velExponent(double x) { return 2.0 * x + 1.0; }
inline double glideTime(double x) { return std::max(x * x * x, 0.0001); }
inline double lfo2Pitch(double x) { return x * x; }  // semitones at full LFO
inline double lfoHz(double x) { return 20.0 * x * x * x + 0.01; }
inline double fltEnvAmount(double x) {
  const double s = 2.0 * x - 1.0;
  return std::fabs(s) * s;
}
inline double fltLfo1(double x) { return x * x * x; }
inline int coarse(double x) { return clampi(truncInt(-99.0 + 198.0 * x), -99, 99); }  // semitones
inline double fine(double x) { return clampi(truncInt(-99.0 + 198.0 * x), -99, 99) / 100.0; }  // semitones
inline double shiftHz(double x) {
  const double s = 2.0 * x - 1.0;
  return 4000.0 * s * s * s * s * s;
}
inline double squared(double x) { return x * x; }
// FM amount: 1 % steps, squared.
inline double fm(double x) {
  const double q = std::floor(x * 100.0 + 1e-9) / 100.0;  // truncation of a non-negative value
  return q * q;
}
// LFO → oscillator amount, -0.99..1.00 (two rounding forms in the reference).
inline double lfoOsc(double x, int osc) {
  if (osc == 0 || osc == 4) return clampi(truncInt(-99.0 + 199.0 * x), -99, 100) / 100.0;
  const int i = truncInt(199.0 * x);
  return (i < 0 ? 0 : (i >= 200 ? 100 : i - 99)) / 100.0;
}
inline double modAmount(double x) { return clampi(truncInt(-99.0 + 199.0 * x), -99, 100) / 100.0; }
inline int seqSteps(double x) { return clampi(truncInt(1.0 + 63.0 * x), 1, 64); }
inline int seqStep(double x) { return clampi(truncInt(-26.0 + 50.0 * x), -26, 24); }  // -26 rest, -25 tie
inline double eqDb(double x) {
  const double s = 2.0 * x - 1.0;
  return 18.0 * s * std::fabs(s);
}
inline int bendRange(double x) { return clampi(truncInt(12.0 * x), 0, 12); }
inline int unisonCopies(int raw) { return std::min(std::max(raw, 0) + 1, 8); }
// Tempo-synced LFO / delay: beats per cycle of the codes 1..9 (1/16 … 16/4).
inline double syncDivisor(int code) {
  static const double d[10] = {0, 15, 30, 45, 60, 120, 180, 240, 480, 960};
  return d[clampi(code, 0, 9)];
}

// Display texts helpers (editor).
inline double fltCutoffHz(double x) { return std::pow(2.0, 5.0 + 9.0 * std::clamp(x, 0.0, 1.0)); }

}  // namespace gl::packs::synths::hex::law
