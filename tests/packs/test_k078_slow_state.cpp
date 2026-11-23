// Gridline - 0.7.8: a plugin slow to give its state holds the interface once, never again after
// each edit (SlowCallGate, used for the state Gridline reads after a plugin's edits): thirty
// frames that would each read a 250 ms state hold none of them over 100 ms after the first.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include "../../src/core/SlowCallGate.h"
#include "../Check.h"

using namespace gl;

int main() {
  SlowCallGate gate;
  int reads = 0;
  auto slowState = [&] {
    ++reads;
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  };
  auto fastState = [&] { ++reads; };
  double worstAfter = 0;
  for (int f = 0; f < 30; ++f) {
    const auto t = std::chrono::steady_clock::now();
    gate.run(1, slowState);  // the slow plugin, edited every frame
    gate.run(2, fastState);  // a quick one beside it
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
    if (f > 0) worstAfter = std::max(worstAfter, ms);
  }
  char b[160];
  std::snprintf(b, sizeof b, "a slow state is read once, then no frame waits for it (worst %.1f ms, %d reads)", worstAfter, reads);
  expect(worstAfter < SlowCallGate::kSlowMs && reads == 31, b);
  expect(gate.slow(1) && !gate.slow(2) && gate.allowed(2), "the quick plugin is still read after each edit");
  gate.reset(1);
  expect(gate.allowed(1), "a reloaded plugin gets another chance");
  return finish();
}
