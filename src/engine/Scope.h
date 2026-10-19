// Gridline — lock-free audio tap for displays (spectrum, oscilloscope).
// The audio thread writes; the UI copies the most recent samples. Reads may
// see a sample mid-update, which is harmless for drawing.
#pragma once
#include <array>
#include <atomic>
#include <cstddef>

namespace gl {

class ScopeTap {
 public:
  static constexpr size_t kSize = 8192;  // power of two
  void write(const float* L, const float* R, int n) {
    size_t w = pos_.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i) buf_[(w + static_cast<size_t>(i)) & (kSize - 1)] = (L[i] + R[i]) * 0.5f;
    pos_.store(w + static_cast<size_t>(n), std::memory_order_release);
  }
  // Copies the newest n samples (oldest first). Returns false if nothing written yet.
  bool read(float* out, size_t n) const {
    size_t w = pos_.load(std::memory_order_acquire);
    if (w == 0) return false;
    if (n > kSize) n = kSize;
    for (size_t i = 0; i < n; ++i) out[i] = buf_[(w - n + i) & (kSize - 1)];
    return true;
  }
  size_t written() const { return pos_.load(std::memory_order_acquire); }

 private:
  std::array<float, kSize> buf_{};
  std::atomic<size_t> pos_{0};
};

}  // namespace gl
