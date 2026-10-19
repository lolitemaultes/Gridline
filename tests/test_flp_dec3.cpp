// Gridline — FLP import, third-wave decoders (preset coverage) and preset-form
// fixes of earlier decoders, on synthetic plugin states built here (no FL
// Studio content). One header per decoder group.
#include "flp_dec3/fixes.h"
#include "flp_dec3/fx.h"
#include "flp_dec3/inst.h"
#include "flp_dec3/kepler.h"
#include "flp_dec3/vfx.h"

int main() {
  using namespace dec3test;
  testFixes();
  testFx();
  testInst();
  testKepler();
  testVfx();
  return finish();
}
