// Gridline - J-186: which regions of a slicing instrument are sounding, for its
// editor (Kerf lights the region, its flag and its pad, and draws the read position).
//
// One slot per voice of the instrument (fixed, kSlots). Written on the audio
// thread only (after each rendered block, and when a voice starts), read by
// the UI: relaxed atomics, no locks, no allocation. A slot holds the region
// it plays (-1: silent), the source frame it reads (-1: not shown), its level
// and whether its note is still held. Each start also bumps the slot's start
// counter after storing the region it started (release), so a region that
// starts and ends between two UI frames still flashes.
#pragma once
#include <atomic>
#include <cstdint>
#include <climits>

namespace gl {

class RegionActivity {
 public:
  static constexpr int kSlots = 32;

  // ---- audio thread
  void started(int slot, int region) {
    if (slot < 0 || slot >= kSlots) return;
    Slot& s = s_[slot];
    s.startedRegion.store(region, std::memory_order_relaxed);
    s.starts.fetch_add(1, std::memory_order_release);
  }
  void set(int slot, int region, int64_t frame, float level, bool held) {
    if (slot < 0 || slot >= kSlots) return;
    Slot& s = s_[slot];
    s.region.store(region, std::memory_order_relaxed);
    s.frame.store(frame < 0 || frame > INT32_MAX ? -1 : static_cast<int32_t>(frame), std::memory_order_relaxed);
    s.level.store(level, std::memory_order_relaxed);
    s.held.store(held, std::memory_order_relaxed);
  }
  void off(int slot) { set(slot, -1, -1, 0.0f, false); }

  // ---- any thread
  int region(int slot) const { return ok(slot) ? s_[slot].region.load(std::memory_order_relaxed) : -1; }
  int64_t frame(int slot) const { return ok(slot) ? s_[slot].frame.load(std::memory_order_relaxed) : -1; }
  float level(int slot) const { return ok(slot) ? s_[slot].level.load(std::memory_order_relaxed) : 0.0f; }
  bool held(int slot) const { return ok(slot) && s_[slot].held.load(std::memory_order_relaxed); }
  uint32_t starts(int slot) const { return ok(slot) ? s_[slot].starts.load(std::memory_order_acquire) : 0u; }
  int startedRegion(int slot) const { return ok(slot) ? s_[slot].startedRegion.load(std::memory_order_relaxed) : -1; }

 private:
  static bool ok(int slot) { return slot >= 0 && slot < kSlots; }
  struct Slot {
    std::atomic<int32_t> region{-1};
    std::atomic<int32_t> frame{-1};  // (32 bits: lock-free everywhere; 13 hours at 44.1 kHz)
    std::atomic<float> level{0};
    std::atomic<bool> held{false};
    std::atomic<uint32_t> starts{0};
    std::atomic<int32_t> startedRegion{-1};
  };
  Slot s_[kSlots];
};

}  // namespace gl
