// Gridline - K-142 (0.7.8): the shape memo of the plugin editors' drawing.
//
// A plugin editor redraws every control each frame, and most of a knob's cost is turning its
// circles, arcs and strokes into triangles. When a control is drawn again exactly as before (same
// place, size, value, colours and state) and the draw list would turn it into the same triangles
// (same anti-aliasing flags, fringe, white pixel and line texture), the memo copies the triangles
// it kept from the last time instead of building them again. What reaches the screen is the very
// same vertex and index data, so nothing looks different; only the work is saved.
//
//   if (!memo::replay(dl, key)) {   // not kept: draw, and keep the result for the next frame
//     memo::Record rec(dl, key);
//     ... draw ...
//   }
//
// The key holds every input of the drawing (the caller adds them); the memo adds the draw list's
// state. A drawing is kept only once the same key was seen in two frames running (a control being
// dragged or a scrolled editor costs no more than before), and dropped when unused for a while.
// UI thread only.
#pragma once
#include <cstddef>
#include <cstdint>
#include "../../third_party/imgui/imgui.h"

namespace gl::skin::memo {

// Inputs of one drawing, packed as 32-bit words.
struct Key {
  static constexpr int kMax = 40;
  uint32_t w[kMax] = {};
  int n = 0;
  void add(uint32_t v) {
    if (n < kMax) w[n++] = v;
  }
  void add(int v) { add(static_cast<uint32_t>(v)); }
  void add(bool v) { add(static_cast<uint32_t>(v ? 1 : 0)); }
  void add(float v);
  void add(ImVec2 v) { add(v.x), add(v.y); }
  bool full() const { return n >= kMax; }
};

// A small number standing for a block of drawing inputs too big for a key (a whole palette, a
// style's colours): equal bytes get the same number, different bytes a different one (compared
// in full, never by a hash alone). Remembers the last few blocks.
uint32_t contextId(const void* bytes, size_t size);

// Copies the kept triangles into dl and returns true when this key's drawing is kept and the
// draw list is in the same state; false: the caller draws (inside a Record).
bool replay(ImDrawList* dl, const Key& key);

// Keeps what is drawn into dl between its construction and destruction under the key (when the
// key was seen in the frame before; otherwise it only notes the key).
class Record {
 public:
  Record(ImDrawList* dl, const Key& key);
  ~Record();
  Record(const Record&) = delete;
  Record& operator=(const Record&) = delete;

 private:
  ImDrawList* dl_;
  Key key_;
  int cmd0_, vtx0_, idx0_;
  unsigned cur0_;
  bool keep_;
};

// Dev and tests. Off: every drawing is built as before (the memo keeps nothing). Verify: a kept
// drawing is built again anyway and compared with what was kept (stats().mismatches counts the
// ones that differ: must stay 0).
void setEnabled(bool on);
bool enabled();
void setVerify(bool on);
struct Stats {
  uint64_t hits = 0, misses = 0, kept = 0, verified = 0, mismatches = 0;
  size_t entries = 0;
};
Stats stats();
void resetStats();
// Drops every kept drawing.
void clear();

}  // namespace gl::skin::memo
