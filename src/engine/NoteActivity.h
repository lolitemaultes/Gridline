// Gridline - per-channel note activity for the interface (J-77): which keys
// of a channel are sounding (piano roll / keyboard previews light them) and a
// per-key trigger counter + velocity (drum pads flash on every hit, even a
// note that starts and ends between two frames).
//
// Written on the audio thread only, read by the UI: fixed-size, no locks, no
// allocation. The held-key set is published as a 128-bit bitset (two
// relaxed atomics); the trigger counter of a key is bumped after its
// velocity is stored (release), so a reader that sees the new count
// (acquire) also sees the hit's velocity.
#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>

namespace gl {

class NoteActivity {
 public:
  static constexpr int kKeys = 128;
  static constexpr int kSlots = 256;  // notes tracked at once per channel (more just do not light)

  // ---- audio thread
  void on(uint32_t id, float pitch, float velocity) {
    off(id);  // a retrigger of the same note id replaces it
    const int k = keyOf(pitch);
    if (k < 0) return;
    vel_[k].store(static_cast<uint8_t>(std::lround(std::fmin(1.f, std::fmax(0.f, velocity)) * 127.f)), std::memory_order_relaxed);
    hits_[k].fetch_add(1, std::memory_order_release);
    if (count_ >= kSlots) return;
    slots_[count_++] = Slot{id, static_cast<uint8_t>(k)};
    if (held_[k]++ == 0) publish(k, true);
  }
  void off(uint32_t id) {
    for (int i = 0; i < count_; ++i)
      if (slots_[i].id == id) {
        const int k = slots_[i].key;
        slots_[i] = slots_[--count_];
        if (held_[k] > 0 && --held_[k] == 0) publish(k, false);
        return;
      }
  }
  // A held note glides to another key (legato / portamento).
  void retarget(uint32_t id, float pitch) {
    const int k = keyOf(pitch);
    for (int i = 0; i < count_; ++i)
      if (slots_[i].id == id) {
        const int old = slots_[i].key;
        if (k < 0 || k == old) return;
        if (held_[old] > 0 && --held_[old] == 0) publish(old, false);
        slots_[i].key = static_cast<uint8_t>(k);
        if (held_[k]++ == 0) publish(k, true);
        return;
      }
  }
  void clear() {
    count_ = 0;
    for (int k = 0; k < kKeys; ++k) held_[k] = 0;
    bits_[0].store(0, std::memory_order_relaxed);
    bits_[1].store(0, std::memory_order_relaxed);
  }

  // ---- any thread
  bool held(int key) const {
    if (key < 0 || key >= kKeys) return false;
    return (bits_[key >> 6].load(std::memory_order_relaxed) >> (key & 63)) & 1u;
  }
  uint64_t bits(int word) const { return bits_[word & 1].load(std::memory_order_relaxed); }
  uint32_t hits(int key) const { return key < 0 || key >= kKeys ? 0u : hits_[key].load(std::memory_order_acquire); }
  float velocity(int key) const { return key < 0 || key >= kKeys ? 0.f : static_cast<float>(vel_[key].load(std::memory_order_relaxed)) / 127.f; }

 private:
  struct Slot {
    uint32_t id = 0;
    uint8_t key = 0;
  };
  static int keyOf(float pitch) {
    const long k = std::lround(pitch);
    return k < 0 || k >= kKeys ? -1 : static_cast<int>(k);
  }
  void publish(int k, bool down) {
    const uint64_t m = uint64_t{1} << (k & 63);
    std::atomic<uint64_t>& w = bits_[k >> 6];
    // Only the audio thread writes: load + store is enough (no RMW contention).
    const uint64_t v = w.load(std::memory_order_relaxed);
    w.store(down ? (v | m) : (v & ~m), std::memory_order_relaxed);
  }

  std::atomic<uint64_t> bits_[2] = {};
  std::atomic<uint32_t> hits_[kKeys] = {};
  std::atomic<uint8_t> vel_[kKeys] = {};
  Slot slots_[kSlots];
  int count_ = 0;
  uint16_t held_[kKeys] = {};
};

}  // namespace gl
