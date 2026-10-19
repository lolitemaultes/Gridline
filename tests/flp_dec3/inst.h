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
  // J-22: → Plucker (gl.plucker, parameter index = tag; full coverage in
  // tests/packs/test_strings.cpp).
  {
    Run r;
    decode(r, "Plucked!", Bytes().i32(128).i32(32).i32(1).i32(0).i32(1).b);
    expect(r.ok && r.m.glType == "gl.plucker" && r.m.exact, "plucked: decodes to the Plucker (exact)");
    expect(near(param(r, 0), 128.0, 0.01) && near(param(r, 1), 32.0), "plucked: decay and colour");
    expect(near(param(r, 2), 1) && near(param(r, 3), 0) && near(param(r, 4), 1), "plucked: switches");
    expect(r.m.rawNorm && near(r.m.rawNorm(1, 64), 0.5), "plucked: automation range");
  }

  // ------------------------------------------------- Transistor Bass
  // J-22: → Step Bass (gl.stepbass); the container is walked to its end
  // (tests/packs/test_strings.cpp). Damaged states are rejected.
  {
    Run bad;
    decode(bad, "Transistor Bass", Bytes().zeros(1000).b);
    expect(!bad.ok, "transistor: state without a bank rejected");
  }

  // ------------------------------------------------- Fruity Slicer 2
  {
    Run r;
    decode(r, "Fruity Slicer 2", Bytes().u32(1).u32(1).zeros(40).str("slc1", false).zeros(60).b);
    expect(r.ok && r.m.glType == "gl.slicer" && r.m.exact, "slicer 2: empty state → empty slicer");
  }
}

}  // namespace dec3test
