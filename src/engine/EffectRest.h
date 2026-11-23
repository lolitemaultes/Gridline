// Gridline - K-142 (0.7.8): effects that rest and still sound as if they never had.
//
// While the engine leaves an effect out it calls Effect::rested(state, aux, ctx, n). An effect
// with nothing that moves by itself does nothing there. One with LFOs, clocks or a pitch engine
// moves them on: either by running its idle render unheard (exact, and costs what running did), or,
// once its whole signal state holds exact zeros, by stepping only its clocks the way n frames of
// silence would (the same operations in the same order, so the result is bit-identical). Silence in
// keeps a zero state zero, so from then on nothing else changes.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include "Effects.h"
#include "Program.h"  // kEngineMaxBlock

namespace gl {

// Runs `fx` on n frames of silence into a scratch buffer nobody hears.
inline void restRenderFx(Effect& fx, const PluginState& st, const ChannelAux& aux, const ProcessContext& ctx, int n) {
  float l[kEngineMaxBlock], r[kEngineMaxBlock];
  for (int pos = 0; pos < n;) {
    const int m = std::min(n - pos, kEngineMaxBlock);
    std::fill(l, l + m, 0.f);
    std::fill(r, r + m, 0.f);
    fx.processAux(st, aux, ctx, l, r, m);
    pos += m;
  }
}

// The two stages above. The effect calls woke() whenever it processes a block of its own (any
// sound may have come in), and run() from rested(): `render(n)` (its idle render) until `zero()`
// says its signal state is all zeros, then `clocks(n)`.
struct RestSettle {
  bool zero = false;
  void woke() { zero = false; }
  template <class Render, class Zero, class Clocks>
  void run(int n, Render&& render, Zero&& isZero, Clocks&& clocks) {
    if (zero) {
      clocks(n);
      return;
    }
    render(n);
    zero = isZero();
  }
};

// K-142 (0.7.8 close): the phase n frames of `ph += inc; ph -= floor(ph)` leave, exactly. For a phase
// in [0, 1) and |inc| < 1 the sum is in (-1, 2), where floor() is -1, 0 or 1: subtracting it is adding
// one, nothing, or taking one away, the same operations (and with inc >= 0 what `if (ph >= 1) ph -= 1`
// does); without the floor() call each frame costs one addition. A closed form would differ in the
// last bits, and then how the engine splits the rest into blocks (a sleeping graph takes whole device
// blocks) would change the sound after it.
inline double advancePhase(double ph, double inc, int n) {
  if (!(ph >= 0.0 && ph < 1.0 && std::fabs(inc) < 1.0)) {  // (never, from the effects' own clocks)
    for (int i = 0; i < n; ++i) {
      ph += inc;
      ph -= std::floor(ph);
    }
    return ph;
  }
  for (int i = 0; i < n; ++i) {
    ph += inc;
    if (ph >= 1.0) ph -= 1.0;
    else if (ph < 0.0) ph += 1.0;
  }
  return ph;
}

// Signal state below this (-160 dBFS) counts as settled for effects whose feedback or followers take
// minutes to reach exact zero: left frozen, it changes the sound after the rest by less than -120 dBFS.
constexpr double kRestQuiet = 1e-8;

template <class It>
inline bool allZeroRange(It b, It e) {
  for (; b != e; ++b)
    if (*b != 0) return false;
  return true;
}
template <class C>
inline bool allZeroOf(const C& c) {
  return allZeroRange(std::begin(c), std::end(c));
}
// A biquad's (dsp::Biquad) memory.
template <class B>
inline bool biquadZero(const B& b) {
  return b.x1 == 0 && b.x2 == 0 && b.y1 == 0 && b.y2 == 0;
}

}  // namespace gl
