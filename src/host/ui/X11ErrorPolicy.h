// Gridline — K-432 (GL-8006CE18): pure policy for Linux/BSD X11 error handling.
//
// Zero X11 header dependency so both macOS and Linux builds compile and unit-test
// the exact handler-classification, guarded-display suppression, and rate-limited
// logging logic used by NativeViewX11.cpp.
//
// Design invariants:
//   1. Never treat an X11 protocol error as fatal (never exit(), never chain to
//      _XDefaultError or nullptr).
//   2. Nested/concurrent ErrorGuard scopes on Gridline's own Display connections
//      suppress expected race errors silently (0 logs) using lock-free atomic
//      depth counters, without ever swapping XSetErrorHandler back and forth.
//   3. Errors on third-party plugin or other X11 connections are logged in full
//      for the first kInitialLogLimit occurrences, then rate-limited to one
//      summary line every kSummaryInterval errors so a spamming plugin cannot
//      flood stderr/logs.
//   4. Handler verification replaces nullptr and libX11's fatal _XDefaultError
//      (or _XDefaultIOError) with Gridline's permanent handler while preserving
//      any custom non-fatal handler installed by SDL2 or a plugin.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace gl::host::native::x11policy {

// Portable copy of the relevant XErrorEvent fields (matches Xlib's ABI).
struct ErrorEvent {
  void* display = nullptr;
  uint64_t resourceId = 0;
  uint64_t serial = 0;
  uint8_t errorCode = 0;
  uint8_t requestCode = 0;
  uint8_t minorCode = 0;
};

enum class ErrorDecision {
  SuppressGuarded,    // Expected error on a guarded Gridline display: 0 logs, return 0.
  LogDetail,          // First kInitialLogLimit plugin/other errors: full detail log, return 0.
  LogSummary,         // Periodic summary after kInitialLogLimit: throttled summary log, return 0.
  SuppressThrottled,  // Suppressed burst error between summaries: 0 logs, return 0.
};

enum class HandlerKind {
  Null,            // nullptr -> Xlib resets to fatal _XDefaultError / _XDefaultIOError.
  DefaultFatal,    // libX11's built-in _XDefaultError / _XDefaultIOError (calls exit(1)).
  Ours,            // Gridline's own permanent non-fatal handler is already installed.
  ExternalCustom,  // A third-party or SDL2 custom handler (preserve on re-check).
};

struct Evaluation {
  ErrorDecision decision = ErrorDecision::SuppressGuarded;
  uint64_t totalPluginErrors = 0;
  uint64_t suppressedSinceLastLog = 0;
  int returnCode = 0;  // Always 0: X11 protocol errors are never fatal.
};

inline constexpr uint64_t kInitialLogLimit = 8;
inline constexpr uint64_t kSummaryInterval = 100;
inline constexpr size_t kMaxGuardSlots = 32;

// Returns true when `symName` names libX11's built-in fatal protocol error handler.
inline bool isDefaultErrorSymbol(const char* symName) noexcept {
  if (!symName || !*symName) return false;
  return std::strcmp(symName, "_XDefaultError") == 0 || std::strcmp(symName, "XDefaultError") == 0;
}

// Returns true when `symName` names libX11's built-in fatal I/O error handler.
inline bool isDefaultIOErrorSymbol(const char* symName) noexcept {
  if (!symName || !*symName) return false;
  return std::strcmp(symName, "_XDefaultIOError") == 0 || std::strcmp(symName, "XDefaultIOError") == 0;
}

// Classifies the handler pointer returned by XSetErrorHandler / XSetIOErrorHandler.
// `isDefaultSym` should be computed via isDefaultErrorSymbol / isDefaultIOErrorSymbol
// when dladdr provides a symbol name.
inline HandlerKind classifyHandler(uintptr_t previous, uintptr_t ours, uintptr_t knownDefaultFatal, bool isDefaultSym) noexcept {
  if (previous == 0) return HandlerKind::Null;
  if (previous == ours) return HandlerKind::Ours;
  if ((knownDefaultFatal != 0 && previous == knownDefaultFatal) || isDefaultSym) return HandlerKind::DefaultFatal;
  return HandlerKind::ExternalCustom;
}

// True when `kind` is unsafe (null or Xlib's fatal default) and must be replaced
// with Gridline's permanent handler rather than restored.
inline bool shouldReplaceHandler(HandlerKind kind) noexcept {
  return kind == HandlerKind::Null || kind == HandlerKind::DefaultFatal;
}

// Lock-free, thread-safe state shared by Gridline's permanent X11 error handler
// and ErrorGuard scopes. Safe to invoke from any thread, including while Xlib
// holds its internal Display mutex.
class PolicyState {
 public:
  PolicyState() = default;
  PolicyState(const PolicyState&) = delete;
  PolicyState& operator=(const PolicyState&) = delete;

  // Enters a guarded scope on `display` (nestable, concurrent across threads and displays).
  // Each guard claims its own slot via a single lock-free CAS, eliminating ABA races.
  // Returns the slot index (>= 0) on success, or -1 if `display` is null.
  int enterGuard(void* display) noexcept {
    if (!display) return -1;
    for (size_t i = 0; i < kMaxGuardSlots; ++i) {
      void* expected = nullptr;
      if (slots_[i].compare_exchange_strong(expected, display, std::memory_order_acq_rel, std::memory_order_relaxed)) {
        return static_cast<int>(i);
      }
    }
    // Fallback counter if more than kMaxGuardSlots guards are active simultaneously.
    overflowDepth_.fetch_add(1, std::memory_order_acq_rel);
    return static_cast<int>(kMaxGuardSlots);
  }

  // Leaves a guarded scope started by enterGuard().
  void leaveGuard(void* display, int slot) noexcept {
    if (!display || slot < 0) return;
    if (static_cast<size_t>(slot) >= kMaxGuardSlots) {
      overflowDepth_.fetch_sub(1, std::memory_order_acq_rel);
      return;
    }
    slots_[static_cast<size_t>(slot)].store(nullptr, std::memory_order_release);
  }

  bool isGuarded(void* display) const noexcept {
    if (!display) return false;
    if (overflowDepth_.load(std::memory_order_acquire) > 0) return true;
    for (size_t i = 0; i < kMaxGuardSlots; ++i) {
      if (slots_[i].load(std::memory_order_acquire) == display) return true;
    }
    return false;
  }

  // Evaluates an incoming X11 protocol error and updates counters atomically.
  Evaluation handleError(const ErrorEvent& ev, uint64_t initialLimit = kInitialLogLimit, uint64_t summaryInterval = kSummaryInterval) noexcept {
    Evaluation out;
    out.returnCode = 0;  // Never fatal.
    if (isGuarded(ev.display)) {
      guardedSuppressed_.fetch_add(1, std::memory_order_relaxed);
      out.decision = ErrorDecision::SuppressGuarded;
      return out;
    }
    const uint64_t n = pluginErrors_.fetch_add(1, std::memory_order_acq_rel) + 1;
    out.totalPluginErrors = n;
    if (n <= initialLimit) {
      logged_.fetch_add(1, std::memory_order_relaxed);
      out.decision = ErrorDecision::LogDetail;
      return out;
    }
    const uint64_t past = n - initialLimit;
    if (summaryInterval > 0 && (past % summaryInterval) == 0) {
      logged_.fetch_add(1, std::memory_order_relaxed);
      out.decision = ErrorDecision::LogSummary;
      out.suppressedSinceLastLog = summaryInterval - 1;
      return out;
    }
    throttledSuppressed_.fetch_add(1, std::memory_order_relaxed);
    out.decision = ErrorDecision::SuppressThrottled;
    return out;
  }

  uint64_t guardedSuppressed() const noexcept { return guardedSuppressed_.load(std::memory_order_relaxed); }
  uint64_t totalPluginErrors() const noexcept { return pluginErrors_.load(std::memory_order_relaxed); }
  uint64_t loggedCount() const noexcept { return logged_.load(std::memory_order_relaxed); }
  uint64_t throttledSuppressed() const noexcept { return throttledSuppressed_.load(std::memory_order_relaxed); }

  void resetForTest() noexcept {
    for (size_t i = 0; i < kMaxGuardSlots; ++i) slots_[i].store(nullptr, std::memory_order_relaxed);
    overflowDepth_.store(0, std::memory_order_relaxed);
    guardedSuppressed_.store(0, std::memory_order_relaxed);
    pluginErrors_.store(0, std::memory_order_relaxed);
    logged_.store(0, std::memory_order_relaxed);
    throttledSuppressed_.store(0, std::memory_order_relaxed);
  }

 private:
  std::atomic<void*> slots_[kMaxGuardSlots]{};
  std::atomic<int> overflowDepth_{0};
  std::atomic<uint64_t> guardedSuppressed_{0};
  std::atomic<uint64_t> pluginErrors_{0};
  std::atomic<uint64_t> logged_{0};
  std::atomic<uint64_t> throttledSuppressed_{0};
};

// Formats a single-line diagnostic for stderr without heap allocation.
// Returns the number of characters written (excluding NUL), or 0 for suppressed decisions.
inline size_t formatErrorMessage(const ErrorEvent& ev, const char* errorText, const Evaluation& eval, char* buf, size_t cap) noexcept {
  if (!buf || cap == 0) return 0;
  buf[0] = '\0';
  const char* text = (errorText && *errorText) ? errorText : "X11 protocol error";
  int n = 0;
  if (eval.decision == ErrorDecision::LogDetail) {
    n = std::snprintf(buf, cap, "[x11] ignored non-fatal error #%llu: %s (code=%u, req=%u.%u, res=0x%llx, serial=%llu)\n",
                      static_cast<unsigned long long>(eval.totalPluginErrors), text, static_cast<unsigned>(ev.errorCode),
                      static_cast<unsigned>(ev.requestCode), static_cast<unsigned>(ev.minorCode), static_cast<unsigned long long>(ev.resourceId),
                      static_cast<unsigned long long>(ev.serial));
  } else if (eval.decision == ErrorDecision::LogSummary) {
    n = std::snprintf(buf, cap, "[x11] ignored %llu X11 errors total (%llu suppressed since last log); latest: %s (code=%u, req=%u.%u, res=0x%llx)\n",
                      static_cast<unsigned long long>(eval.totalPluginErrors), static_cast<unsigned long long>(eval.suppressedSinceLastLog), text,
                      static_cast<unsigned>(ev.errorCode), static_cast<unsigned>(ev.requestCode), static_cast<unsigned>(ev.minorCode),
                      static_cast<unsigned long long>(ev.resourceId));
  } else {
    return 0;
  }
  if (n <= 0) return 0;
  return static_cast<size_t>(n) >= cap ? cap - 1 : static_cast<size_t>(n);
}

// Helper for safe conversion of a function pointer to uintptr_t without -Wpedantic warnings.
template <class Fn>
inline uintptr_t functionAddress(Fn fn) noexcept {
  static_assert(sizeof(Fn) <= sizeof(uintptr_t));
  uintptr_t addr = 0;
  std::memcpy(&addr, &fn, sizeof(Fn));
  return addr;
}

}  // namespace gl::host::native::x11policy
