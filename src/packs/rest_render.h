// Gridline - K-142 (0.7.8): resting that keeps an instrument where it would have been.
//
// While an instrument rests the engine leaves its blocks out and calls Generator::rested(n). An
// instrument whose idle render still moves something on (a free-running oscillator or LFO, a
// sample-and-hold clock, a random generator, the delay heads and feedback of its own effects) runs
// that same render here into a scratch buffer that nobody hears, so the next note plays exactly as
// if it had never rested. None of its voices sounds while it rests, so what runs is its own idle
// cost, the same as before it rested.
#pragma once

#include <algorithm>
#include "../engine/Program.h"

namespace gl::packs {

template <class Render>
inline void restRender(int n, Render&& render) {
  float l[kEngineMaxBlock], r[kEngineMaxBlock];
  for (int pos = 0; pos < n;) {
    const int m = std::min(n - pos, kEngineMaxBlock);
    std::fill(l, l + m, 0.f);
    std::fill(r, r + m, 0.f);
    render(l, r, m);
    pos += m;
  }
}

}  // namespace gl::packs
