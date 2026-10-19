// Gridline — non-interleaved float audio buffer.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace gl {

struct AudioBuffer {
  size_t channels = 0;
  size_t frames = 0;
  std::vector<float> data;  // channels * frames, plane-major: ch * frames + i

  AudioBuffer() = default;
  AudioBuffer(size_t ch, size_t n) { resize(ch, n); }

  void resize(size_t ch, size_t n) {
    channels = ch;
    frames = n;
    data.assign(ch * n, 0.0f);
  }
  void clear() { std::fill(data.begin(), data.end(), 0.0f); }

  float* plane(size_t ch) { return data.data() + ch * frames; }
  const float* plane(size_t ch) const { return data.data() + ch * frames; }

  float at(size_t ch, size_t i) const { return data[ch * frames + i]; }
  float& at(size_t ch, size_t i) { return data[ch * frames + i]; }

  float peak() const {
    float p = 0.0f;
    for (float v : data) p = std::max(p, std::fabs(v));
    return p;
  }
};

}  // namespace gl
