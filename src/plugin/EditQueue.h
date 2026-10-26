// Gridline — parameter edits made on the main thread (the editor, presets),
// waiting to be told to a host that takes them from its processing call
// (CLAP: output events of process / flush). One producer (main thread), one
// consumer at a time (process or flush); no locks, no allocation.
#pragma once
#include <atomic>
#include <cstdint>

namespace gl::plugin {

class EditQueue {
 public:
  enum Kind : uint32_t { Value = 0, Begin = 1, End = 2 };
  // False when full (the caller then asks the host to read every value again).
  bool push(Kind k, int index) {
    const uint32_t h = head_.load(std::memory_order_relaxed);
    if (h - tail_.load(std::memory_order_acquire) >= kSize) return false;
    items_[h % kSize] = (static_cast<uint32_t>(k) << 30) | (static_cast<uint32_t>(index) & 0x3FFFFFFFu);
    head_.store(h + 1, std::memory_order_release);
    return true;
  }
  bool pop(Kind& k, int& index) {
    const uint32_t t = tail_.load(std::memory_order_relaxed);
    if (t == head_.load(std::memory_order_acquire)) return false;
    const uint32_t v = items_[t % kSize];
    tail_.store(t + 1, std::memory_order_release);
    k = static_cast<Kind>(v >> 30);
    index = static_cast<int>(v & 0x3FFFFFFFu);
    return true;
  }

 private:
  static constexpr uint32_t kSize = 8192;
  uint32_t items_[kSize] = {};
  std::atomic<uint32_t> head_{0}, tail_{0};
};

}  // namespace gl::plugin
