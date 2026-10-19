// Gridline — FLP import, second-wave instrument decoders, on synthetic plugin
// states built here (no FL Studio content). One header per decoder group.
#include "flp_gen2/controllers.h"
#include "flp_gen2/pads.h"
#include "flp_gen2/slicers.h"
#include "flp_gen2/synths_a.h"
#include "flp_gen2/synths_b.h"

int main() {
  using namespace gen2test;
  expect(gl::findPlugin("gl.slicer") != nullptr, "catalogue available");
  testSlicers();
  testPads();
  testSynthsA();
  testSynthsB();
  testControllers();
  return finish();
}
