// Note on / off events one of the in-tree test instruments received, in arrival order, for the
// host tests that check what an instrument hears (shared by the probes of every format).
#pragma once
#include <cstdint>

struct GridlineNoteLog {
  struct Ev {
    bool on;
    int key;
    int id;      // the format's note id, -1 when it has none
    int64_t at;  // sample: frames the instrument has run + the event's offset in the block
  };
  static constexpr int kMax = 256;
  Ev ev[kMax];
  int count = 0;
  int64_t frames = 0;  // frames the instrument has run
  void add(bool on, int key, int id, int offset) {
    if (count < kMax) ev[count++] = {on, key, id, frames + offset};
  }
};
