// Gridline: breadcrumbs, the only crash code core and engine call directly. Small and free of
// dependencies, because the plugin bundles link it too; there it fills a ring nobody reads.
// No allocation, no lock, no I/O: fine on the audio thread.
#include <atomic>
#include <chrono>
#include <cstring>
#include "CrashInternal.h"

namespace gl::crash {
namespace detail {
namespace {

CrumbRing gLocalRing;                    // until the session exists (zero initialised)
std::atomic<CrumbRing*> gRing{nullptr};  // null: gLocalRing
std::atomic<int64_t> gStartMs{0};
std::atomic<void (*)()> gModulesHook{nullptr};
std::atomic<void (*)(const char*)> gCaughtHook{nullptr};

int64_t steadyMs() noexcept {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void packText(uint64_t (&words)[7], const char* a, const char* b) noexcept {
  char t[sizeof words] = {};
  size_t n = 0;
  for (; a && *a && n + 1 < sizeof t; ++a) t[n++] = *a;
  if (b && *b && n + 2 < sizeof t) {
    t[n++] = ' ';
    for (; *b && n + 1 < sizeof t; ++b) t[n++] = *b;
  }
  std::memcpy(words, t, sizeof t);
}

void addCrumb(const char* a, const char* b) noexcept {
  uint64_t words[7];
  packText(words, a, b);
  CrumbRing& r = crumbRing();
  const int64_t ms = steadyMs() - gStartMs.load(std::memory_order_relaxed);
  // The same crumb again (a drag makes hundreds): the newest slot counts it.
  const uint64_t last = r.next.load(std::memory_order_relaxed);
  if (last > 0) {
    CrumbSlot& s = r.slots[(last - 1) % kCrumbs];
    bool same = s.seq.load(std::memory_order_acquire) == last;
    for (int i = 0; same && i < 7; ++i) same = s.text[i].load(std::memory_order_relaxed) == words[i];
    if (same) {
      s.repeat.fetch_add(1, std::memory_order_relaxed);
      s.ms.store(static_cast<uint64_t>(ms), std::memory_order_relaxed);
      return;
    }
  }
  const uint64_t n = r.next.fetch_add(1, std::memory_order_relaxed);
  CrumbSlot& s = r.slots[n % kCrumbs];
  s.seq.store(0, std::memory_order_release);
  s.ms.store(static_cast<uint64_t>(ms), std::memory_order_relaxed);
  s.repeat.store(0, std::memory_order_relaxed);
  for (int i = 0; i < 7; ++i) s.text[i].store(words[i], std::memory_order_relaxed);
  s.seq.store(n + 1, std::memory_order_release);
}

}  // namespace

CrumbRing& crumbRing() noexcept {
  CrumbRing* r = gRing.load(std::memory_order_relaxed);
  return r ? *r : gLocalRing;
}
void setCrumbRing(CrumbRing* r) noexcept { gRing.store(r, std::memory_order_relaxed); }
void setCrumbClockStart(int64_t ms) noexcept { gStartMs.store(ms, std::memory_order_relaxed); }
void setHooks(void (*modules)(), void (*caughtFn)(const char*)) noexcept {
  gModulesHook.store(modules);
  gCaughtHook.store(caughtFn);
}

}  // namespace detail

void crumb(const char* what) noexcept { detail::addCrumb(what, nullptr); }
void crumb(const char* what, const char* d) noexcept { detail::addCrumb(what, d); }

void modulesChanged() noexcept {
  if (auto f = detail::gModulesHook.load()) f();
}

void caught(const char* where) noexcept {
  auto f = detail::gCaughtHook.load();
  if (!f) return;
  try {
    f(where);
  } catch (...) {  // out of memory while reporting: the failure was handled by the caller anyway
  }
}

}  // namespace gl::crash
