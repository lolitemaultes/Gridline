// Gridline — "platter" pack: the controller hub. Every short MIDI message of
// every input goes into one broadcast ring (written by the device driver
// threads, a spin flag orders several inputs); every deck reads the ring from
// its own position on its own thread, so two decks on one controller (left and
// right) both see everything, whichever channel is selected. Readers check a
// sequence number per slot and skip slots that were overwritten.
#include <atomic>
#include <chrono>
#include "platter.h"

namespace gl::packs::platter::hub {

namespace {

struct Slot {
  std::atomic<uint64_t> seq{~0ull};  // index stored in this slot (~0 = being written)
  std::atomic<uint32_t> device{0};
  std::atomic<uint32_t> msg{0};
  std::atomic<int64_t> ns{0};  // arrival time (steady clock)
};

struct Ring {
  Slot slots[kSize];
  std::atomic<uint64_t> head{0};
  std::atomic_flag writer = ATOMIC_FLAG_INIT;
  std::atomic<bool> tapped{false};
  std::atomic<uint32_t> claims[kClaims] = {};
};

Ring& ring() {
  static Ring r;
  return r;
}

}  // namespace

uint32_t deviceId(const std::string& name) {
  if (name.empty()) return 0;
  uint32_t h = 2166136261u;
  for (unsigned char c : name) h = (h ^ c) * 16777619u;
  return h ? h : 1;
}

namespace {
std::atomic<int64_t (*)()> gClock{nullptr};
}  // namespace

int64_t nowNs() {
  if (int64_t (*fn)() = gClock.load(std::memory_order_relaxed)) return fn();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void setClock(int64_t (*fn)()) { gClock.store(fn, std::memory_order_relaxed); }

void push(uint32_t device, uint8_t status, uint8_t d1, uint8_t d2) {
  const int64_t t = nowNs();
  Ring& r = ring();
  while (r.writer.test_and_set(std::memory_order_acquire)) {
  }
  const uint64_t h = r.head.load(std::memory_order_relaxed);
  Slot& s = r.slots[h % kSize];
  s.seq.store(~0ull, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  s.device.store(device, std::memory_order_relaxed);
  s.msg.store(static_cast<uint32_t>(status) | (static_cast<uint32_t>(d1) << 8) | (static_cast<uint32_t>(d2) << 16), std::memory_order_relaxed);
  s.ns.store(t, std::memory_order_relaxed);
  s.seq.store(h, std::memory_order_release);
  r.head.store(h + 1, std::memory_order_release);
  r.writer.clear(std::memory_order_release);
}

void setTapped(bool on) { ring().tapped.store(on, std::memory_order_relaxed); }
bool tapped() { return ring().tapped.load(std::memory_order_relaxed); }
uint64_t head() { return ring().head.load(std::memory_order_acquire); }

void setClaimed(const uint32_t* ids, int count) {
  Ring& r = ring();
  for (int i = 0; i < kClaims; ++i) {
    const uint32_t v = i < count ? ids[i] : 0;
    if (r.claims[i].load(std::memory_order_relaxed) != v) r.claims[i].store(v, std::memory_order_relaxed);
  }
}

bool claimed(uint32_t id) {
  if (id == 0) return false;
  const Ring& r = ring();
  for (const auto& c : r.claims)
    if (c.load(std::memory_order_relaxed) == id) return true;
  return false;
}

bool read(uint64_t index, uint32_t& device, uint8_t& status, uint8_t& d1, uint8_t& d2, int64_t* ns) {
  const Slot& s = ring().slots[index % kSize];
  if (s.seq.load(std::memory_order_acquire) != index) return false;
  device = s.device.load(std::memory_order_relaxed);
  const uint32_t m = s.msg.load(std::memory_order_relaxed);
  const int64_t t = s.ns.load(std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_acquire);
  if (s.seq.load(std::memory_order_relaxed) != index) return false;  // overwritten while reading
  status = static_cast<uint8_t>(m & 0xFF);
  d1 = static_cast<uint8_t>((m >> 8) & 0xFF);
  d2 = static_cast<uint8_t>((m >> 16) & 0xFF);
  if (ns) *ns = t;
  return true;
}

}  // namespace gl::packs::platter::hub
