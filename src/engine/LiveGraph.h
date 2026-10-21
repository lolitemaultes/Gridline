// Gridline — J-68: change-driven caches for editor graphics.
//
// Plugin editors draw curves that are pure functions of the plugin's
// parameters (EQ / filter responses, transfer curves, envelope shapes, decay
// views) and sometimes of audio data. Immediate-mode UIs run every frame, so
// each such graphic keeps its computed samples in a Memo keyed by a hash of
// exactly the inputs it depends on: the samples are recomputed only when a
// parameter, the sample rate, the graph size or the audio data counter moves.
// UI thread only (no locks needed); the audio thread never touches these.
#pragma once
#include <cstdint>
#include <cstring>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gl::livegraph {

// FNV-1a over the inputs of one graphic.
struct Hasher {
  uint64_t h = 1469598103934665603ull;
  Hasher& bytes(const void* p, size_t n) {
    const auto* b = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    return *this;
  }
  Hasher& f(float v) { return bytes(&v, sizeof v); }
  Hasher& d(double v) { return bytes(&v, sizeof v); }
  Hasher& i(int64_t v) { return bytes(&v, sizeof v); }
  Hasher& u(uint64_t v) { return bytes(&v, sizeof v); }
  Hasher& floats(const std::vector<float>& v) { return v.empty() ? *this : bytes(v.data(), v.size() * sizeof(float)); }
  Hasher& str(std::string_view s) { return bytes(s.data(), s.size()); }
  uint64_t value() const { return h; }
};

// One cached graphic: `compute` runs only when the input hash changes.
template <class T>
struct Memo {
  uint64_t key = 0;
  bool valid = false;
  uint32_t computes = 0;  // how often the samples were (re)computed (tests, profiling)
  T value{};
  template <class F>
  const T& get(uint64_t k, F&& compute) {
    if (!valid || k != key) {
      compute(value);
      key = k, valid = true, ++computes;
    }
    return value;
  }
};

// Sampled curve: x positions are implied by the caller's axis (uniform in the
// graph's own coordinates); y holds the function values.
using Curve = std::vector<float>;

// Per-editor store of memos: one per (plugin instance key, graphic id).
template <class T>
Memo<T>& memo(uint64_t instanceKey, uint32_t graphic) {
  static std::unordered_map<uint64_t, Memo<T>> store;
  return store[instanceKey * 0x9E3779B97F4A7C15ull ^ graphic];
}

}  // namespace gl::livegraph
