// Gridline — third-wave decoder tests: Kepler Exo → DCO Synth since J-22 (included by test_flp_dec3.cpp).
#pragma once
#include <map>
#include <utility>
#include "../../src/packs/synths/dco.h"
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
  namespace dc = gl::packs::synths::dco;
  expect(registered("Kepler Exo"), "kepler exo: registered");
  {
    // J-22: Kepler Exo → DCO Synth (parameter index = control tag, exact).
    std::map<int, float> t = {{7, 1}, {8, 1}, {12, 1}, {13, 0.625f}, {15, 1}, {27, 1}, {32, 1}, {33, 0.5f},
                              {37, 0.5f}, {38, 0.4f}, {40, 1}, {44, 0.6f}, {43, 1}, {45, 0.3f}, {47, 1},
                              {65, 0}, {66, 0.5f}, {67, 0.25f}, {68, 0.5f}, {72, 0.5f}, {74, 0.5f}, {75, 1},
                              {83, 1}, {85, 1}, {86, 0.5f}, {88, 0.5f}, {89, 0.25f}, {90, 0.5f},
                              {92, 0.75f}, {121, 1}, {133, 0.3f}, {4, 0.5f}, {24, 0.5f}};
    Run r;
    decode(r, "Kepler Exo", keplerExo(t, {{2, 38}}).b);
    expect(r.ok && r.m.glType == "gl.dcosynth" && r.m.exact, "kepler exo: decodes exactly to DCO Synth");
    expect(param(r, dc::DcDco1Pulse) == 1 && param(r, dc::DcDco1Saw) == 1 && param(r, dc::DcDco2On) == 1 &&
               near(param(r, dc::DcDco2Semi), 0.625f) && param(r, dc::DcSync) == 1,
           "kepler exo: DCO switches, DCO 2 semitones, hard sync");
    expect(param(r, dc::DcSlope) == 3 && near(param(r, dc::DcCutoff), 0.5f) && near(param(r, dc::DcReso), 0.4f) &&
               param(r, dc::DcEnvPolarity) == 1 && near(param(r, dc::DcEnv1ToCut), 0.6f),
           "kepler exo: slope selector index, cutoff, resonance, polarity, env amount");
    expect(param(r, dc::DcVcaSrc) == 2 && near(param(r, dc::DcEnv2S), 0.25f), "kepler exo: VCA = env 2");
    expect(param(r, dc::DcMono) == 1 && param(r, dc::DcOctave) == 2 && param(r, dc::DcArpOn) == 1 && param(r, dc::DcArpRange) == 1 &&
               param(r, dc::DcArpMode) == 2,
           "kepler exo: mono, octave up, arpeggiator selectors");
    expect(param(r, dc::DcMxSrc1) == 3 && param(r, dc::DcMxDst1) == 39 && near(param(r, dc::DcMxAmt1), 0.75f),
           "kepler exo: matrix row env 1 → resonance");
    expect(param(r, dc::DcRevOn) == 1 && near(param(r, dc::DcRevWet), 0.3f) && param(r, dc::DcPanel) == dc::PanelTwo, "kepler exo: reverb, panel");
    expect(r.m.param && r.m.param(37) == dc::DcCutoff && r.m.value && near(r.m.value(37, 1.0), 1.0) && near(r.m.value(40, 1.0), 3),
           "kepler exo: automation identity (selectors → index)");
    expect(r.m.rawNorm && near(r.m.rawNorm(38, 3000), 0.5) && std::isnan(r.m.rawNorm(500, 1)), "kepler exo: recorded values → normalized");
  }
  {
    Run r;
    decode(r, "Kepler Exo", std::vector<uint8_t>{1, 2, 3});
    expect(!r.ok, "kepler exo: short state refused");
  }
}

}  // namespace dec3test
