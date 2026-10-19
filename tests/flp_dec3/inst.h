// Gridline — third-wave decoder tests: instruments (included by test_flp_dec3.cpp).
#pragma once
#include "../../src/core/PluginParams.h"
#include "../flp_gen2/Gen2Test.h"

namespace dec3test {

using namespace gen2test;

inline void testInst() {
  using namespace gl;
  using namespace gl::pp;

  // -------------------------------------------------------- Plucked!
  {
    Run r;
    decode(r, "Plucked!", Bytes().i32(128).i32(32).i32(1).i32(0).i32(1).b);
    expect(r.ok && r.m.glType == "gl.pluck", "plucked: decodes to the string model");
    expect(near(param(r, PlkDecay), 20.0, 0.01) && near(param(r, PlkBrightness), 0.25), "plucked: decay and colour");
    expect(near(param(r, PlkNormalize), 1) && near(param(r, PlkGate), 0) && param(r, PlkWidth) > 0.5, "plucked: switches");
    expect(r.m.rawNorm && near(r.m.rawNorm(1, 64), 0.5), "plucked: automation range");
  }

  // ------------------------------------------------- Transistor Bass
  {
    Bytes st;
    st.u32(1).u32(1).u32(8).u32(0).u32(2).u32(3).u32(2).u32(0).zeros(16);  // header
    for (int k = 0; k < 4; ++k) st.u32(1).u8(0).u32(static_cast<uint32_t>(k)).u32(static_cast<uint32_t>(k)).zeros(6);  // programs
    st.u32(4).str("Acid", false);
    float f[100] = {};
    for (float& x : f) x = 0.5f;
    f[1] = 1.0f, f[2] = 0.0f, f[4] = 0.8f, f[27] = 1.0f, f[29] = 0.25f, f[41] = 0.0f;
    for (float x : f) st.f32(x);
    st.zeros(64);
    Run r;
    decode(r, "Transistor Bass", st.b);
    expect(r.ok && r.m.glType == "gl.acid", "transistor: decodes to the acid bass");
    expect(near(param(r, AcWave), 1) && near(param(r, AcCutoff), 60, 0.5) && near(param(r, AcReso), 0.8), "transistor: wave, cutoff, resonance");
    expect(near(param(r, AcDrive), 0.25) && near(param(r, AcSeqOn), 0), "transistor: distortion drive, sequencer off");
    Run bad;
    decode(bad, "Transistor Bass", Bytes().zeros(1000).b);
    expect(!bad.ok, "transistor: state without a patch rejected");
  }

  // ------------------------------------------------- Fruity Slicer 2
  {
    Run r;
    decode(r, "Fruity Slicer 2", Bytes().u32(1).u32(1).zeros(40).str("slc1", false).zeros(60).b);
    expect(r.ok && r.m.glType == "gl.slicer" && r.m.exact, "slicer 2: empty state → empty slicer");
  }
}

}  // namespace dec3test
