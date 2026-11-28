// Gridline — K-432 (GL-8006CE18): Linux Wayland / XWayland X11 error safety and
// static/global std::thread teardown safety tests.
//
// Tests BOTH interlocking root causes behind GL-8006CE18:
//   1. X11 error policy (host/ui/X11ErrorPolicy.h) + native::installX11SafetyHandlers():
//      - Guarded Gridline displays suppress expected race errors silently (0 logs).
//      - Nested & concurrent ErrorGuard scopes across multiple threads/displays are
//        lock-free, race-free, and never restore a fatal handler.
//      - Third-party plugin display errors NEVER return fatal / NEVER exit(), log full
//        details for the first N errors, then throttle to periodic summary lines.
//      - Handler classification replaces nullptr and _XDefaultError / _XDefaultIOError
//        with Gridline's permanent handler while preserving custom non-fatal handlers.
//      - On Linux, live libX11 integration verifies installX11SafetyHandlers() installs
//        and re-ensures the non-fatal error handler without clobbering custom handlers.
//   2. Static/global std::thread teardown safety (core/ThreadJoin.h + hardened classes):
//      - safeJoin() handles default threads, active workers, and self-thread detach
//        without throwing resource_deadlock_would_occur -> std::terminate().
//      - Hardened worker classes (SerialWorker, browse::Index, single::Server, OscServer,
//        mcp::JobRunner, mcp::HttpServer, WorkerPool, stems::Pool) destroy cleanly while
//        running without explicit stop() / shutdown(), including real exit() teardown in
//        a forked child process on POSIX.
#include "Check.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#endif
#if defined(__linux__)
#include <dlfcn.h>
#endif

#include "../src/core/BrowserIndex.h"
#include "../src/core/Mcp.h"
#include "../src/core/McpTransport.h"
#include "../src/core/OscServer.h"
#include "../src/core/SerialWorker.h"
#include "../src/core/SingleInstance.h"
#include "../src/core/ThreadJoin.h"
#include "../src/engine/WorkerPool.h"
#include "../src/engine/stems/StemOps.h"
#include "../src/host/ui/NativeView.h"
#include "../src/host/ui/X11ErrorPolicy.h"

namespace xp = gl::host::native::x11policy;
namespace fs = std::filesystem;

namespace {

int dummyExternalHandler(void*, void*) { return 0; }
int dummyOurHandler(void*, void*) { return 0; }
int dummyDefaultHandler(void*, void*) { return 0; }

void testErrorPolicyGuardedDisplay() {
  xp::PolicyState st;
  int ownDpyStorage = 1;
  int otherDpyStorage = 2;
  void* ownDpy = &ownDpyStorage;
  void* otherDpy = &otherDpyStorage;

  expect(!st.isGuarded(ownDpy), "unguarded display initially false");
  expect(!st.isGuarded(nullptr), "null display never guarded");
  expect(st.enterGuard(nullptr) == -1, "enterGuard(nullptr) returns -1");

  // Single guard suppresses own display silently (0 logs) without affecting plugin display.
  const int s1 = st.enterGuard(ownDpy);
  expect(s1 >= 0, "enterGuard returns valid slot");
  expect(st.isGuarded(ownDpy), "own display is guarded");
  expect(!st.isGuarded(otherDpy), "plugin display is not guarded");

  xp::ErrorEvent evOwn{ownDpy, 0x1234, 42, 3 /* BadWindow */, 2 /* ChangeWindowAttributes */, 0};
  const xp::Evaluation rOwn = st.handleError(evOwn);
  expect(rOwn.decision == xp::ErrorDecision::SuppressGuarded, "guarded display error suppressed silently");
  expect(rOwn.returnCode == 0, "guarded display error returns 0 (non-fatal)");
  expect(st.guardedSuppressed() == 1, "guardedSuppressed incremented");
  expect(st.totalPluginErrors() == 0, "plugin error counter untouched by guarded error");
  expect(st.loggedCount() == 0, "0 logs emitted for guarded display");

  char buf[256] = {};
  expect(xp::formatErrorMessage(evOwn, "BadWindow", rOwn, buf, sizeof buf) == 0, "formatErrorMessage emits nothing for guarded error");

  // Nested guard on same display: leaving inner keeps display guarded until outer leaves.
  const int s2 = st.enterGuard(ownDpy);
  expect(s2 >= 0 && s2 != s1, "nested guard claims independent slot");
  st.leaveGuard(ownDpy, s2);
  expect(st.isGuarded(ownDpy), "display stays guarded after inner guard leaves");
  st.leaveGuard(ownDpy, s1);
  expect(!st.isGuarded(ownDpy), "display unguarded once outer guard leaves");

  // Overflow beyond kMaxGuardSlots still guards and unguards cleanly.
  std::vector<int> slots;
  for (size_t i = 0; i < xp::kMaxGuardSlots + 5; ++i) slots.push_back(st.enterGuard(ownDpy));
  expect(st.isGuarded(ownDpy), "overflow guards keep display guarded");
  for (int sl : slots) st.leaveGuard(ownDpy, sl);
  expect(!st.isGuarded(ownDpy), "overflow guards release cleanly");
}

void testErrorPolicyThrottlingAndFormatting() {
  xp::PolicyState st;
  int plugDpyStorage = 99;
  void* plugDpy = &plugDpyStorage;

  // First kInitialLogLimit (8) errors -> LogDetail with returnCode == 0.
  for (uint64_t i = 1; i <= xp::kInitialLogLimit; ++i) {
    xp::ErrorEvent ev{plugDpy, 0xABCD00 + i, 100 + i, 9 /* BadDrawable */, 62 /* CopyArea */, 1};
    const xp::Evaluation r = st.handleError(ev);
    expect(r.decision == xp::ErrorDecision::LogDetail, "initial plugin error logs detail");
    expect(r.returnCode == 0, "plugin error always returns 0 (non-fatal)");
    expect(r.totalPluginErrors == i, "total plugin error count matches");
    char line[384] = {};
    const size_t len = xp::formatErrorMessage(ev, "BadDrawable", r, line, sizeof line);
    expect(len > 0 && std::strstr(line, "BadDrawable") != nullptr && std::strstr(line, "code=9") != nullptr &&
               std::strstr(line, "req=62.1") != nullptr,
           "detail log line includes error text, error_code, request_code.minor_code");
  }

  // Errors 9..107 -> SuppressThrottled, 0 logs, returnCode == 0.
  bool allThrottled = true;
  bool allNonFatal = true;
  for (uint64_t i = xp::kInitialLogLimit + 1; i < xp::kInitialLogLimit + xp::kSummaryInterval; ++i) {
    xp::ErrorEvent ev{plugDpy, 0x1000, i, 8 /* BadMatch */, 42, 0};
    const xp::Evaluation r = st.handleError(ev);
    if (r.decision != xp::ErrorDecision::SuppressThrottled) allThrottled = false;
    if (r.returnCode != 0) allNonFatal = false;
    char line[64] = {};
    if (xp::formatErrorMessage(ev, "BadMatch", r, line, sizeof line) != 0) allThrottled = false;
  }
  expect(allThrottled, "burst errors between limit and summary interval are throttled silently");
  expect(allNonFatal, "throttled burst errors always return 0");

  // Error #108 (8 + 100) -> LogSummary with 99 suppressed since last log.
  {
    xp::ErrorEvent ev{plugDpy, 0xBEEF, 500, 2 /* BadValue */, 12, 3};
    const xp::Evaluation r = st.handleError(ev);
    expect(r.decision == xp::ErrorDecision::LogSummary, "summary emitted at initialLimit + summaryInterval");
    expect(r.totalPluginErrors == xp::kInitialLogLimit + xp::kSummaryInterval, "summary total count exact");
    expect(r.suppressedSinceLastLog == xp::kSummaryInterval - 1, "summary suppressed count exact");
    expect(r.returnCode == 0, "summary error returns 0");
    char line[384] = {};
    const size_t len = xp::formatErrorMessage(ev, "BadValue", r, line, sizeof line);
    expect(len > 0 && std::strstr(line, "108 X11 errors total") != nullptr && std::strstr(line, "99 suppressed") != nullptr &&
               std::strstr(line, "BadValue") != nullptr,
           "summary line formats total, suppressed count, and latest error");
  }

  // Burst up to 1,000 total errors -> verify exact counters.
  for (uint64_t i = xp::kInitialLogLimit + xp::kSummaryInterval + 1; i <= 1000; ++i) {
    xp::ErrorEvent ev{plugDpy, 0x1, i, 3, 2, 0};
    const xp::Evaluation r = st.handleError(ev);
    if (r.returnCode != 0) allNonFatal = false;
  }
  expect(allNonFatal, "all 1000 burst plugin errors returned 0 (never fatal)");
  expect(st.totalPluginErrors() == 1000, "1000 total plugin errors counted");
  // Logged: first 8 + summaries at 108, 208, ..., 908 (9 summaries) = 17 logs total.
  expect(st.loggedCount() == 17, "exact throttled log count (17 logs for 1000 errors)");
  expect(st.throttledSuppressed() == 983, "exact throttled suppressed count (983 of 1000)");
}

void testHandlerClassification() {
  const uintptr_t ours = xp::functionAddress(&dummyOurHandler);
  const uintptr_t defFatal = xp::functionAddress(&dummyDefaultHandler);
  const uintptr_t extCustom = xp::functionAddress(&dummyExternalHandler);

  expect(ours != 0 && defFatal != 0 && extCustom != 0, "functionAddress produces non-zero addresses");

  expect(xp::isDefaultErrorSymbol("_XDefaultError"), "_XDefaultError recognized as default fatal");
  expect(xp::isDefaultErrorSymbol("XDefaultError"), "XDefaultError recognized as default fatal");
  expect(!xp::isDefaultErrorSymbol("X11_XErrorHandler"), "SDL X11_XErrorHandler not misclassified as fatal default");
  expect(!xp::isDefaultErrorSymbol(nullptr), "nullptr symbol name safely false");

  expect(xp::isDefaultIOErrorSymbol("_XDefaultIOError"), "_XDefaultIOError recognized as default fatal I/O");
  expect(xp::isDefaultIOErrorSymbol("XDefaultIOError"), "XDefaultIOError recognized as default fatal I/O");
  expect(!xp::isDefaultIOErrorSymbol("customIOHandler"), "custom I/O handler not misclassified");

  const xp::HandlerKind kNull = xp::classifyHandler(0, ours, defFatal, false);
  expect(kNull == xp::HandlerKind::Null && xp::shouldReplaceHandler(kNull), "nullptr handler classified Null -> replace");

  const xp::HandlerKind kDefAddr = xp::classifyHandler(defFatal, ours, defFatal, false);
  expect(kDefAddr == xp::HandlerKind::DefaultFatal && xp::shouldReplaceHandler(kDefAddr), "known _XDefaultError address -> replace");

  const xp::HandlerKind kDefSym = xp::classifyHandler(extCustom, ours, 0, true);
  expect(kDefSym == xp::HandlerKind::DefaultFatal && xp::shouldReplaceHandler(kDefSym), "symbol match _XDefaultError -> replace");

  const xp::HandlerKind kOurs = xp::classifyHandler(ours, ours, defFatal, false);
  expect(kOurs == xp::HandlerKind::Ours && !xp::shouldReplaceHandler(kOurs), "own handler classified Ours -> keep");

  const xp::HandlerKind kExt = xp::classifyHandler(extCustom, ours, defFatal, false);
  expect(kExt == xp::HandlerKind::ExternalCustom && !xp::shouldReplaceHandler(kExt), "external custom handler classified ExternalCustom -> preserve");
}

void testConcurrentErrorGuardStress() {
  xp::PolicyState st;
  int dpys[4] = {1, 2, 3, 4};
  int plugDpy = 5;
  std::atomic<bool> stop{false};
  std::atomic<bool> ok{true};

  std::vector<std::thread> workers;
  for (int t = 0; t < 6; ++t) {
    workers.emplace_back([&, t] {
      void* d = &dpys[t % 4];
      for (int i = 0; i < 400; ++i) {
        const int s = st.enterGuard(d);
        if (!st.isGuarded(d)) ok.store(false, std::memory_order_relaxed);
        const xp::Evaluation ev = st.handleError(xp::ErrorEvent{d, 1, static_cast<uint64_t>(i), 3, 2, 0});
        if (ev.decision != xp::ErrorDecision::SuppressGuarded || ev.returnCode != 0) ok.store(false, std::memory_order_relaxed);
        st.leaveGuard(d, s);
      }
    });
  }

  std::thread plugErrors([&] {
    while (!stop.load(std::memory_order_acquire)) {
      const xp::Evaluation ev = st.handleError(xp::ErrorEvent{&plugDpy, 7, 1, 8, 2, 0});
      if (ev.decision == xp::ErrorDecision::SuppressGuarded || ev.returnCode != 0) ok.store(false, std::memory_order_relaxed);
    }
  });

  for (auto& w : workers) w.join();
  stop.store(true, std::memory_order_release);
  plugErrors.join();

  expect(ok.load(), "concurrent nested ErrorGuards and plugin errors are race-free");
  expect(!st.isGuarded(&dpys[0]) && !st.isGuarded(&dpys[1]) && !st.isGuarded(&dpys[2]) && !st.isGuarded(&dpys[3]),
         "all display guard slots released cleanly after stress test");
  expect(st.guardedSuppressed() == 6 * 400, "exact guarded error count under concurrency");
}

void testSafeJoinHelper() {
  // 1. Default-constructed thread is a safe no-op.
  std::thread empty;
  gl::threads::safeJoin(empty);
  expect(!empty.joinable(), "safeJoin on default std::thread is a no-op");

  // 2. Normal join of active worker.
  std::atomic<bool> ran{false};
  std::thread t([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ran.store(true);
  });
  gl::threads::safeJoin(t);
  expect(ran.load() && !t.joinable(), "safeJoin joins active worker cleanly");

  // 3. Self-thread detach safety: calling safeJoin from the thread itself must NOT throw / terminate!
  struct SelfBox {
    std::thread th;
    std::mutex mu;
    std::condition_variable cv;
    bool ready = false;
    std::atomic<bool> detachedSelf{false};
  };
  auto box = std::make_shared<SelfBox>();
  {
    std::lock_guard<std::mutex> lk(box->mu);
    box->th = std::thread([box] {
      {
        std::unique_lock<std::mutex> lk2(box->mu);
        box->cv.wait(lk2, [&] { return box->ready; });
      }
      gl::threads::safeJoin(box->th);  // Would throw resource_deadlock_would_occur with plain join()!
      box->detachedSelf.store(true, std::memory_order_release);
    });
    box->ready = true;
  }
  box->cv.notify_all();
  for (int i = 0; i < 200 && !box->detachedSelf.load(std::memory_order_acquire); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  expect(box->detachedSelf.load(std::memory_order_acquire), "safeJoin detaches instead of terminating when called on self thread");
}

// Static workers on an unexpected exit(): a busy worker must not hold the exit forever,
// a finished one is joined.
void testJoinWithin() {
  {
    std::atomic<bool> running{true};
    std::thread t([&running] { running.store(false, std::memory_order_release); });
    gl::threads::joinWithin(t, running, 2000);
    expect(!t.joinable(), "joinWithin joins a worker that finishes");
  }
  {
    static std::atomic<bool> stuckRunning{true};
    static std::atomic<bool> release{false};
    std::thread t([] {
      while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
      stuckRunning.store(false);
    });
    const auto t0 = std::chrono::steady_clock::now();
    gl::threads::joinWithin(t, stuckRunning, 100);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    expect(!t.joinable(), "joinWithin detaches a worker that stays busy");
    expect(ms >= 90 && ms < 1500, "joinWithin waits about its limit, then gives up");
    release.store(true);
  }
}

void testHardenedClassDestructors() {
  // 1. SerialWorker destroyed immediately with work queued (no explicit drain).
  {
    std::atomic<int> count{0};
    {
      gl::SerialWorker w;
      w.post([&] { count.fetch_add(1); });
      w.post([&] { count.fetch_add(1); });
    }
    expect(count.load() == 2, "SerialWorker drains and destroys cleanly without terminate");
  }

  // 2. SerialWorker destroyed FROM INSIDE its own job (self-thread destructor!).
  {
    std::atomic<bool> done{false};
    auto t0 = std::chrono::steady_clock::now();
    auto wp = std::make_shared<gl::SerialWorker>();
    std::weak_ptr<gl::SerialWorker> weak = wp;
    wp->post([holder = wp, &done]() mutable {
      // Wait until the outer scope drops its shared_ptr so this thread runs ~SerialWorker().
      for (int i = 0; i < 200 && holder.use_count() > 1; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
      holder.reset();  // Runs ~SerialWorker() on the worker thread itself!
      done.store(true, std::memory_order_release);
    });
    wp.reset();
    for (int i = 0; i < 300 && !done.load(std::memory_order_acquire); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    expect(done.load(std::memory_order_acquire) && weak.expired(), "SerialWorker self-thread destruction succeeds without terminate");
    expect(ms < 1200.0, "SerialWorker self-thread destruction avoids 1.5s drain self-deadlock");
  }

  // 3. browse::Index destroyed immediately after start() without explicit stop().
  {
    const fs::path tmpDir = fs::temp_directory_path() / "gridline_k432_idx_test";
    std::error_code ec;
    fs::create_directories(tmpDir, ec);
    {
      gl::browse::Index ix;
      ix.setRoots({{tmpDir.u8string(), false}});
      ix.start();
    }  // ~Index() must stop & join cleanly
    fs::remove_all(tmpDir, ec);
    expect(true, "browse::Index destroyed while active without stop()");
  }

  // 4. single::Server destroyed while serving without explicit stop() (POSIX socket).
#if !defined(_WIN32)
  {
    const std::string sock = (fs::temp_directory_path() / ("gl_k432_" + std::to_string(::getpid()) + ".sock")).u8string();
    std::error_code ec;
    fs::remove(fs::u8path(sock), ec);
    fs::remove(fs::u8path(sock + ".lock"), ec);
    {
      gl::single::Server srv;
      if (srv.claim(sock) == gl::single::Server::Claim::Taken) srv.start([] {});
    }  // ~Server() must stop & join cleanly
    fs::remove(fs::u8path(sock), ec);
    fs::remove(fs::u8path(sock + ".lock"), ec);
    expect(true, "single::Server destroyed while serving without stop()");
  }
#endif

  // 5. OscServer destroyed while running without explicit stop().
  {
    gl::OscServer osc;
    gl::OscServerConfig cfg;
    cfg.port = 0;  // ephemeral port
    cfg.tcp = false;
    (void)osc.start(cfg);
  }
  expect(true, "OscServer destroyed while running without stop()");

  // 6. mcp::JobRunner destroyed while job is running without explicit cancelAll().
  {
    std::atomic<bool> sawCancel{false};
    {
      gl::mcp::JobRunner runner;
      auto job = std::make_shared<gl::mcp::Job>();
      job->id = runner.nextId();
      job->tool = "test";
      runner.start(job, [&](gl::mcp::Job& j) {
        while (!j.cancel.load()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        sawCancel.store(true);
        return gl::mcp::ToolResult{};
      });
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }  // ~JobRunner() cancels and joins cleanly
    expect(sawCancel.load(), "mcp::JobRunner destroyed while running cancels & joins cleanly");
  }

  // 7. mcp::HttpServer destroyed while watch thread is active without explicit stop().
  {
    gl::mcp::HttpServer srv;
    if (srv.start(0)) srv.watch([] {});
  }
  expect(true, "mcp::HttpServer destroyed while watch thread active without stop()");

  // 8. WorkerPool and stems::Pool destroyed immediately after spawning threads.
  {
    gl::WorkerPool wp;
    wp.ensureThreads(2);
  }
  {
    gl::stems::Pool sp(2);
  }
  expect(true, "WorkerPool and stems::Pool destroy cleanly without terminate");
}

#if !defined(_WIN32)
void testExitStaticTeardownInChild() {
  // Simulates what happens if a third-party library ever calls exit(0) while static
  // thread-owning Gridline objects are alive: __run_exit_handlers must run all static
  // destructors cleanly WITHOUT invoking std::terminate() / SIGABRT!
  const pid_t pid = ::fork();
  if (pid == 0) {
    std::set_terminate([] { _exit(99); });
    static gl::SerialWorker w;
    w.post([] { std::this_thread::sleep_for(std::chrono::milliseconds(5)); });
    static gl::WorkerPool pool;
    pool.ensureThreads(2);
    static gl::mcp::JobRunner jobs;
    auto j = std::make_shared<gl::mcp::Job>();
    j->id = jobs.nextId();
    jobs.start(j, [](gl::mcp::Job& job) {
      while (!job.cancel.load()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
      return gl::mcp::ToolResult{};
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::exit(0);  // Runs __run_exit_handlers -> static destructors!
  }
  expect(pid > 0, "forked child for exit() static-teardown verification");
  if (pid > 0) {
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
    }
    const bool cleanZero = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    expect(cleanZero, "std::exit() with live static thread-owning objects exits 0 (no std::terminate / SIGABRT)");
  }
}
#endif

void testInstallX11SafetyHandlersPlatform() {
  // Callable cleanly on every platform (no-op on macOS / Windows; active on Linux / BSD).
  gl::host::native::installX11SafetyHandlers();
  gl::host::native::installX11SafetyHandlers();  // idempotent
  expect(true, "native::installX11SafetyHandlers() is safe and idempotent on this platform");

#if defined(__linux__)
  void* h = dlopen("libX11.so.6", RTLD_LAZY | RTLD_NOLOAD);
  if (!h) h = dlopen("libX11.so.6", RTLD_LAZY);
  if (h) {
    using XErrHandler = int (*)(void*, void*);
    auto setErr = reinterpret_cast<XErrHandler (*)(XErrHandler)>(dlsym(h, "XSetErrorHandler"));
    const uintptr_t defErr = reinterpret_cast<uintptr_t>(dlsym(h, "_XDefaultError"));
    if (setErr) {
      // 1. After installX11SafetyHandlers(), active handler is non-null and NOT _XDefaultError.
      XErrHandler active = setErr(nullptr);  // Temporarily resets to _XDefaultError while returning previous.
      const uintptr_t activeAddr = xp::functionAddress(active);
      expect(activeAddr != 0 && activeAddr != defErr, "Linux libX11: installX11SafetyHandlers installed non-fatal handler");

      // 2. Calling installX11SafetyHandlers() after a plugin reset XSetErrorHandler(nullptr)
      //    must immediately replace _XDefaultError with Gridline's permanent non-fatal handler.
      gl::host::native::installX11SafetyHandlers();
      XErrHandler restored = setErr(&dummyExternalHandler);
      expect(xp::functionAddress(restored) == activeAddr, "Linux libX11: re-ensures non-fatal handler after reset to nullptr");

      // 3. When a custom non-fatal handler is installed, installX11SafetyHandlers() preserves it.
      gl::host::native::installX11SafetyHandlers();
      XErrHandler preserved = setErr(restored);
      expect(xp::functionAddress(preserved) == xp::functionAddress(&dummyExternalHandler),
             "Linux libX11: preserves custom non-fatal external handler");
    }
  }
#endif
}

}  // namespace

int main() {
  testErrorPolicyGuardedDisplay();
  testErrorPolicyThrottlingAndFormatting();
  testHandlerClassification();
  testConcurrentErrorGuardStress();
  testSafeJoinHelper();
  testJoinWithin();
  testHardenedClassDestructors();
#if !defined(_WIN32)
  testExitStaticTeardownInChild();
#endif
  testInstallX11SafetyHandlersPlatform();
  return finish();
}
