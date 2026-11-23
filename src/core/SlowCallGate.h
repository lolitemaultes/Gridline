// Gridline - a gate for calls the interface thread makes on its own (not because the person asked):
// a call that once took longer than a frame's worth for a key is not made for that key again until
// reset (for example the plugin state Gridline reads after edits; a save still reads it).
#pragma once
#include <chrono>
#include <cstdint>
#include <map>

namespace gl {

class SlowCallGate {
 public:
  static constexpr double kSlowMs = 100;
  bool allowed(uint64_t key) const { return !slow_.count(key); }
  bool slow(uint64_t key) const { return slow_.count(key) != 0; }
  // Runs `call` when allowed and times it; returns false when the gate skipped it. `took` gets
  // the time when it was slow (to tell the person once).
  template <class Fn>
  bool run(uint64_t key, Fn&& call, double* tookMs = nullptr) {
    if (!allowed(key)) return false;
    const auto t0 = std::chrono::steady_clock::now();
    call();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (ms > kSlowMs) slow_[key] = ms;
    if (tookMs) *tookMs = ms;
    return true;
  }
  void reset(uint64_t key) { slow_.erase(key); }
  void clear() { slow_.clear(); }

 private:
  std::map<uint64_t, double> slow_;
};

}  // namespace gl
