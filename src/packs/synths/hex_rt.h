// Gridline — "synths" pack, J-33 group "hex": runtime building blocks of Hex
// Synth shared by its runtime files (wave spectra, band-limited tables, the
// filter coefficient table, the effect slots). Internal to the pack.
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "../../engine/Generators.h"
#include "hex.h"

namespace gl::packs::synths::hex {

// One wave: 128 harmonic amplitudes and phases (cycles).
struct Spectrum {
  float amp[kHarmonics] = {};
  float phase[kHarmonics] = {};
};

// Spectrum of wave selector `w` (1..32); custom (0) comes from the state data.
// Waves 6..32 are the reference instrument's own spectra, read in place from
// its installed plugin file (never shipped with Gridline); without it,
// formula approximations of the same families are used. `exact` (optional)
// tells which of the two was used.
Spectrum builtinSpectrum(int w, bool* exact = nullptr);
// True when the reference instrument's spectra were found on this computer.
bool referenceSpectraAvailable();

// 128 band-limited levels of a 256-sample cycle: level b holds harmonics
// 1..128 − b. Stored with one guard sample before and two after each cycle
// for 4-point cubic reads: t[b][0] = y[255], t[b][1 + n] = y[n], t[b][257..258] = y[0..1].
struct WaveTable {
  float t[kHarmonics][259];
};
using TablePtr = std::shared_ptr<const WaveTable>;
// Built (and cached by content) on the UI thread.
TablePtr waveTable(const Spectrum& s);

// 4-point cubic (Catmull-Rom) value of segment n (0..255) at fraction f.
inline float cubicAt(const float* t, int n, float f) {
  const float p = t[n], y0 = t[n + 1], y1 = t[n + 2], y2 = t[n + 3];
  const float c1 = (y1 - p) * 0.5f;
  const float c3 = (y2 - p) * 0.5f + 1.5f * (y0 - y1);
  const float c2 = p + c1 - y0 - c3;
  return ((c3 * f + c2) * f + c1) * f + y0;
}

// Filter coefficients of cutoff index i (0..65535): stage gain g / 2, 1 − g and
// the resonance feedback scale.
struct FilterCoef {
  float halfG, oneMinusG, feedback;
};
const FilterCoef* filterTable();  // 65536 entries, built once

// Prepared data of a gl.hexsynth instance: the eight slot tables.
struct HexAux final : AuxData {
  TablePtr slot[kSlots];
  int wave[kSlots] = {};
};
std::shared_ptr<const HexAux> buildHexAux(const std::string& data, const std::vector<float>& params);

}  // namespace gl::packs::synths::hex
