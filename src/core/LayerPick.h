// Gridline — K-169: which children of a layer play a note (pure, allocation-free; the engine's
// live notes and the note compiler share it).
#pragma once
#include <cstdint>
#include "Song.h"

namespace gl::layer {

// What the cycle and no-repeat modes remember between notes.
struct PickState {
  uint32_t count = 0;  // notes picked so far (cycle)
  int last = -1;       // child index picked last (random without repeats)
};

// `w[i]` > 0: child i can play the note. Returns the children that play it as a bit mask
// (child i = bit i, at most 32 children). `rnd` is the note's random number.
inline uint32_t pick(int mode, const float* w, int n, uint32_t rnd, PickState& st) {
  uint32_t all = 0;
  int count = 0;
  for (int i = 0; i < n && i < 32; ++i)
    if (w[i] > 0.0f) all |= 1u << i, ++count;
  if (count == 0 || mode == kLayerAll) return all;
  auto nth = [&](int k, int skip) {  // the k-th eligible child, not counting `skip`
    for (int i = 0; i < n && i < 32; ++i) {
      if (!(all & (1u << i)) || i == skip) continue;
      if (k-- == 0) return i;
    }
    return -1;
  };
  int chosen = -1;
  if (mode == kLayerCycle) {
    chosen = nth(static_cast<int>(st.count % static_cast<uint32_t>(count)), -1);
    ++st.count;
  } else if (mode == kLayerRandomNoRepeat && count > 1 && st.last >= 0 && (all & (1u << st.last))) {
    chosen = nth(static_cast<int>(rnd % static_cast<uint32_t>(count - 1)), st.last);
  } else {  // random (and the first no-repeat pick)
    chosen = nth(static_cast<int>(rnd % static_cast<uint32_t>(count)), -1);
  }
  st.last = chosen;
  return chosen >= 0 ? 1u << chosen : 0u;
}

}  // namespace gl::layer
