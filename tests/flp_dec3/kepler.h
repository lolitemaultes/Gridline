// Gridline — third-wave decoder tests: Kepler Exo → Analog (included by test_flp_dec3.cpp).
#pragma once
#include <map>
#include <utility>
#include "../../src/packs/synths/synths.h"
#include "../flp_gen2/Gen2Test.h"

namespace dec3test {

using namespace gen2test;

namespace keptest {

// A Kepler Exo state: header, 151 tag floats (defaults 0 unless given), then
// the 8-row modulation matrix (source, destination tag; -1 = empty).
inline Bytes keplerExo(const std::map<int, float>& tags, const std::vector<std::pair<int, int>>& matrix = {}) {
  Bytes b;
  b.u32(1).u32(1).u32(0).u32(0).u32(0).u32(0).u32(0).u32(9).str("Kepler-2", true);
  b.u32(2).u32(151);
  for (int t = 0; t < 151; ++t) {
    auto it = tags.find(t);
    b.f32(it == tags.end() ? 0.0f : it->second);
  }
  b.u32(1).u32(8);
  for (size_t r = 0; r < 8; ++r) {
    if (r < matrix.size()) b.i32(matrix[r].first).i32(matrix[r].second);
    else b.i32(-1).i32(-1);
  }
  b.u32(2).u32(8).u32(0).zeros(8);
  return b;
}

}  // namespace keptest

inline void testKepler() {
  using namespace keptest;
  namespace sy = gl::packs::synths;
  expect(registered("Kepler Exo"), "kepler exo: registered");
  {
    // DCO1 saw + pulse, DCO2 saw +12 st hard-synced, 24 dB filter, env 2 VCA,
    // LFO1 → cutoff, matrix env 1 → resonance, arpeggiator, reverb.
    std::map<int, float> t = {{7, 1}, {8, 1}, {12, 1}, {13, 0.625f}, {15, 1}, {27, 1}, {32, 1}, {33, 0.5f},
                              {37, 0.5f}, {38, 0.4f}, {40, 1}, {44, 0.6f}, {43, 1}, {45, 0.3f}, {47, 1},
                              {65, 0}, {66, 0.5f}, {67, 0.25f}, {68, 0.5f}, {72, 0.5f}, {74, 0.5f}, {75, 1},
                              {83, 1}, {85, 1}, {86, 0.5f}, {88, 0.5f}, {89, 0.25f}, {90, 0.5f},
                              {92, 0.75f}, {121, 1}, {133, 0.3f}, {4, 0.5f}, {24, 0.5f}};
    Run r;
    decode(r, "Kepler Exo", keplerExo(t, {{2, 38}}).b);
    expect(r.ok && r.m.glType == "gl.analog" && !r.m.exact, "kepler exo: decodes approximately to Analog");
    expect(near(param(r, sy::anOsc(0, sy::OscWave)), sy::WaveSaw) && near(param(r, sy::anOsc(2, sy::OscOn)), 1) &&
               near(param(r, sy::anOsc(2, sy::OscWave)), sy::WavePulse),
           "kepler exo: DCO1 saw + pulse on oscillators 1 and 3");
    expect(near(param(r, sy::anOsc(1, sy::OscCoarse)), 12) && near(param(r, sy::anOsc(1, sy::OscMod)), sy::ModSync) &&
               near(param(r, sy::anOsc(1, sy::OscLevel)), 0.4),
           "kepler exo: DCO2 transpose, hard sync, level");
    expect(near(param(r, sy::AnFltMode), sy::FltLp24) && near(param(r, sy::AnCutoff), 20.0 * std::sqrt(1000.0), 0.5) &&
               near(param(r, sy::AnReso), 0.4) && near(param(r, sy::AnFltEnv), -0.6),
           "kepler exo: filter slope, cutoff, resonance, inverted env amount");
    expect(near(param(r, sy::AnAmpS), 0.25) && near(param(r, sy::AnAmpA), 0.001, 1e-4), "kepler exo: VCA follows env 2");
    expect(near(param(r, sy::anLfo(0, sy::LfoDest)), sy::AdCutoff) && near(param(r, sy::anLfo(0, sy::LfoAmt)), 0.3),
           "kepler exo: LFO1 → cutoff");
    expect(near(param(r, sy::anMx(0, 0)), sy::AsFilterEnv) && near(param(r, sy::anMx(0, 1)), sy::AdResonance) &&
               near(param(r, sy::anMx(0, 2)), 0.5),
           "kepler exo: matrix row env 1 → resonance");
    expect(near(param(r, sy::AnPlayMode), sy::PlayMono) && near(param(r, sy::AnTranspose), 12) && near(param(r, sy::AnPan), 0),
           "kepler exo: mono, octave up, centred pan");
    expect(near(param(r, sy::AnArpOn), 1) && near(param(r, sy::AnArpDiv), sy::Div1_16) && near(param(r, sy::AnArpOct), 2) &&
               near(param(r, sy::AnArpMode), sy::ArpUpDown),
           "kepler exo: arpeggiator");
    expect(near(param(r, sy::AnFxDrive + sy::FxRevOn), 1) && near(param(r, sy::AnFxDrive + sy::FxRevMix), 0.3), "kepler exo: reverb");
    // Automation: tag 37 → cutoff (log), recorded value of tag 38 in 0..6000.
    expect(r.m.param && r.m.param(37) == sy::AnCutoff && r.m.value && near(r.m.value(37, 1.0), 20000, 1), "kepler exo: cutoff automation");
    expect(r.m.rawNorm && near(r.m.rawNorm(38, 3000), 0.5) && std::isnan(r.m.rawNorm(500, 1)), "kepler exo: recorded values → normalized");
  }
  {
    Run r;
    decode(r, "Kepler Exo", std::vector<uint8_t>{1, 2, 3});
    expect(r.ok && r.m.glType == "gl.analog", "kepler exo: short state keeps defaults");
  }
}

}  // namespace dec3test
