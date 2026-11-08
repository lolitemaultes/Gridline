// Gridline: crash reports (handler, watchdog, recoverable failures, the copyable code).
//
// What happens when something goes wrong:
//  - A fatal signal or unhandled exception: a report is written with signal-safe code only, a
//    forked child tries to save a fresh recovery copy (POSIX), and the process ends. A small
//    separate process (the sentinel) then shows the pop-up; if it could not run, the next start does.
//  - A frozen interface: the watchdog thread writes the same report and the sentinel shows a
//    pop-up beside the frozen window. Nothing is killed unless the user asks; the report is
//    dropped when the interface comes back.
//  - A caught exception: reportRecoverable writes a report and the app shows the pop-up and goes on.
//
// Nothing is ever uploaded. Reports hold file names of modules only, never folders; text from
// exceptions is scrubbed of paths and the user name.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gl::crash {

// ---- breadcrumbs: any thread, the audio thread too (no allocation, no lock, no I/O) ----
void crumb(const char* what) noexcept;
void crumb(const char* what, const char* detail) noexcept;
// A plugin library was just loaded: the crash path learns its name at once (no-op until installed).
void modulesChanged() noexcept;
// Inside a catch block of code linked into everything, plugin bundles too (no-op until installed):
// the same as reportCurrentException.
void caught(const char* where) noexcept;

// ---- installing ----
struct Config {
  std::string configDir;      // reports go in <configDir>/crash-reports, copies in <configDir>/recovery
  std::string selfPath;       // this program; "" asks the system
  bool sentinel = true;       // start the separate process that shows the pop-up
  bool watchdog = true;
  bool devBacktrace = false;  // GRIDLINE_DEV_BACKTRACE owns SIGUSR2 and std::terminate
  int frozenAfterSeconds = 30;
};
void install(const Config& c);
bool installed();
// The run ended as it should: the sentinel shows nothing. Call when the main loop has ended.
void markCleanExit();
// Text of OpenGL's vendor, renderer and version, once a context exists.
void setGpu(const char* vendor, const char* renderer, const char* version);

// ---- watchdog ----
// UI thread, once per frame. Also fine from a worker doing the UI thread's work while it waits.
void uiHeartbeat() noexcept;
// From here on a silent UI thread counts as frozen (startup work is not watched).
void armWatchdog() noexcept;
void noteAudioThread() noexcept;  // audio callback: a relaxed store of the thread id
// Long legitimate work on the UI thread: not frozen while one is alive, up to `maxSeconds`.
class BusyScope {
 public:
  explicit BusyScope(const char* what, int maxSeconds = 600) noexcept;
  ~BusyScope();
  BusyScope(const BusyScope&) = delete;
  BusyScope& operator=(const BusyScope&) = delete;

 private:
  int64_t previous_ = 0;
};
// K-132: how many BusyScopes have started in this process, and whether one is alive now (tests).
uint64_t busyScopesStarted() noexcept;
bool busyNow() noexcept;

// ---- recoverable failures (never the audio thread) ----
struct Recoverable {
  std::string where;       // what Gridline was doing, plain words ("Opening a project")
  std::string reportPath;
  std::string code;
  std::string id;
};
// Writes a report for a caught exception and queues it for the pop-up, unless the same failure
// was reported within the last minute. Returns the report's id.
std::string reportRecoverable(const char* where, const char* typeName, const std::string& what);
// Inside a catch block: the current exception's type and text.
std::string reportCurrentException(const char* where);
// The queue the pop-up drains (UI thread).
bool takeRecoverable(Recoverable& out);

// K-47: the graphics driver went and Gridline could not get it back. Nothing is wrong with the memory,
// so a report and a fresh recovery copy are written here and now, and the app ends: the pop-up process
// shows the report with the copy, in a process of its own. UI thread. (Not installed: it just ends.)
[[noreturn]] void endAfterGraphicsLoss(const char* why);

// ---- recovery copies ----
// A fresher copy saved from the crash path, in a forked child: `save(path)` runs on a copy of the
// process's memory. Its failure costs nothing; the last periodic copy stays.
using RescueFn = bool (*)(const char* path);
void setRescue(RescueFn fn) noexcept;
void setUnsavedWork(bool unsaved) noexcept;
std::string recoveryDir(const std::string& configDir);
std::string sessionId();  // this run's name: <unix time>-<pid>

// ---- reports on disk (normal code: UI thread, sentinel, dialog) ----
struct PendingReport {
  std::string path;
  std::string kind;        // crash, freeze, exception, ended, graphics
  std::string id;
  std::string text;        // the whole code
  std::string sessionId;   // whose recovery copy belongs to it ("" none)
  std::string recoveryFile;
  std::string originalProject;   // where that project lived ("" never saved)
  int64_t recoverySaved = 0;     // unix time of that copy (0 none)
  bool recoveryRescued = false;  // saved at the moment of the crash
  int64_t crashTime = 0;
};
std::string reportsDir(const std::string& configDir);
// Sessions that died without ending (no process left) get what they lack, a report included.
// Returns the reports not yet seen, oldest first.
std::vector<PendingReport> collectPending(const std::string& configDir);
PendingReport readReport(const std::string& configDir, const std::string& path);
void markSeen(const std::string& path);
// Deletes the copy of a session (it was recovered, or the user does not want it).
void discardRecovery(const std::string& configDir, const std::string& sessionId);
// Trims old reports and copies (kept 30 days, newest 30 reports).
void sweep(const std::string& configDir);

// ---- parts with their own tests ----
// Removes folders and the user name from free text. No allocation, always terminates `out`.
void scrubText(const char* in, char* out, size_t cap, const char* homeDir, const char* userName) noexcept;
// The code between its marker lines from pasted text (quote marks and all); "" when none.
std::string extractCode(const std::string& pasted);

// The freeze decision, free of clocks and threads. Update about twice a second.
class FreezeDetector {
 public:
  enum class Action { None, Frozen, Recovered };
  explicit FreezeDetector(int64_t frozenAfterMs, int64_t maxTickGapMs = 5000) : after_(frozenAfterMs), maxGap_(maxTickGapMs) {}
  // busyUntilMs: legitimate work is running until then (0 none). snoozeUntilMs: the user chose to wait.
  Action update(int64_t nowMs, uint64_t heartbeat, bool armed, int64_t busyUntilMs, int64_t snoozeUntilMs) {
    Action out = Action::None;
    // A long gap between two looks: the machine slept, or the process was stopped, or the system
    // is starved. Whatever it was, it says nothing about the UI thread: start counting again.
    const bool gap = hasTick_ && nowMs - lastTick_ > maxGap_;
    const bool beat = hasTick_ && heartbeat != heartbeat_;
    if (!armed || !hasTick_ || gap || beat || nowMs < busyUntilMs || nowMs < snoozeUntilMs) {
      if (frozen_ && beat) {
        frozen_ = false;
        out = Action::Recovered;
      }
      stallStart_ = nowMs;
    }
    heartbeat_ = heartbeat;
    lastTick_ = nowMs;
    hasTick_ = true;
    if (!frozen_ && armed && nowMs - stallStart_ >= after_) {
      frozen_ = true;
      out = Action::Frozen;
    }
    return out;
  }
  bool frozen() const { return frozen_; }
  // "Keep waiting": the pop-up closed; counts from now.
  void rearm(int64_t nowMs) {
    frozen_ = false;
    stallStart_ = nowMs;
  }

 private:
  int64_t after_, maxGap_;
  int64_t lastTick_ = 0, stallStart_ = 0;
  uint64_t heartbeat_ = 0;
  bool hasTick_ = false, frozen_ = false;
};

// ---- the separate process ----
// gridline-app --crash-sentinel <session file> <link>: returns the exit code.
int sentinelMain(int argc, char** argv);

// ---- the session as the pop-up process sees it ----
struct SessionView;
SessionView* openSession(const std::string& sessionFile);
void closeSession(SessionView* s);
std::string sessionFileOf(const std::string& configDir, const std::string& sessionId);
uint64_t sessionHeartbeat(const SessionView* s);
bool sessionFrozen(const SessionView* s);
int64_t sessionPid(const SessionView* s);
void sessionSnooze(SessionView* s);
// Asks the frozen process to save a fresh copy, waits up to `waitMs`, and ends it.
void sessionForceEnd(SessionView* s, int waitMs);
bool processAlive(int64_t pid);
// Starts this program again, on its own, with these arguments. False when it could not.
bool startSelf(const std::vector<std::string>& args);
// The program's path, kept for restarts (the pop-up process asks here too).
std::string selfPath();
int64_t ownPid();

// ---- forced failures for tests ----
// Only with GRIDLINE_CRASH_TEST_FORCE set and a --hidden run (main.cpp decides); never by accident.
// segv, bus, ill, fpe, abort, terminate, overflow, heap, thread, audio, exception, freeze.
void forceForTest(const std::string& kind);
bool audioCrashRequested() noexcept;
[[noreturn]] void crashNowForTest();

}  // namespace gl::crash
