// Gridline: crash reports, what the common part and the platform parts share.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "Crash.h"

namespace gl::crash::detail {

static_assert(std::atomic<uint64_t>::is_always_lock_free, "the crash path needs lock-free 64-bit atomics");

// ---- breadcrumbs: a fixed ring; readers (crash path, other process) tolerate a slot being written ----
struct CrumbSlot {
  std::atomic<uint64_t> seq;      // 0 while written; else the number of the crumb + 1
  std::atomic<uint64_t> ms;       // since the session began
  std::atomic<uint64_t> repeat;   // the same crumb again
  std::atomic<uint64_t> text[7];  // 56 bytes
};
constexpr int kCrumbs = 64;
struct CrumbRing {
  std::atomic<uint64_t> next;
  CrumbSlot slots[kCrumbs];
};

// CrashCrumb.cpp: small enough for the plugin bundles to carry.
CrumbRing& crumbRing() noexcept;
void setCrumbRing(CrumbRing* r) noexcept;
void setCrumbClockStart(int64_t steadyMs) noexcept;
void setHooks(void (*modules)(), void (*caught)(const char*)) noexcept;

// ---- the session: a fixed block in a mapped file, readable after the process died ----
constexpr uint32_t kSessionMagic = 0x31534C47;  // "GLS1"
struct SessionBlock {
  uint32_t magic;
  uint32_t size;
  int64_t pid;
  int64_t startUnix;
  std::atomic<uint64_t> heartbeat;
  std::atomic<uint32_t> armed;
  std::atomic<uint32_t> cleanExit;
  std::atomic<uint32_t> frozen;         // the watchdog wrote a freeze report
  std::atomic<uint32_t> snooze;         // the user chose to keep waiting
  std::atomic<uint32_t> rescueRequest;  // pop-up: "force restart", save a copy first
  std::atomic<uint32_t> rescueDone;
  std::atomic<uint32_t> reportReady;    // reportName is complete
  std::atomic<uint32_t> handled;        // the user already dealt with this session's end
  std::atomic<uint32_t> unsaved;        // the project has unsaved work
  std::atomic<uint32_t> exitCode;       // Windows: filled in by the sentinel
  char reportName[128];
  char sessionName[48];
  char version[32];
  char build[32];
  char os[160];
  char gpu[224];
  char arch[16];
  CrumbRing crumbs;
};

// ---- report text, built without allocating ----
class Writer {
 public:
  using Sink = void (*)(void* ctx, const char* data, size_t n);
  Writer(Sink sink, void* ctx) : sink_(sink), ctx_(ctx) {}
  ~Writer() { flush(); }
  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;
  Writer& str(const char* s) {
    for (; s && *s; ++s) ch(*s);
    return *this;
  }
  Writer& ch(char c) {
    if (n_ == sizeof buf_) flush();
    buf_[n_++] = c;
    return *this;
  }
  Writer& dec(int64_t v) {
    if (v < 0) {
      ch('-');
      return udec(static_cast<uint64_t>(0) - static_cast<uint64_t>(v));
    }
    return udec(static_cast<uint64_t>(v));
  }
  Writer& udec(uint64_t v) {
    char t[24];
    int k = 0;
    do {
      t[k++] = static_cast<char>('0' + v % 10);
      v /= 10;
    } while (v);
    while (k) ch(t[--k]);
    return *this;
  }
  Writer& hex(uint64_t v) {
    str("0x");
    char t[20];
    int k = 0;
    do {
      t[k++] = "0123456789abcdef"[v & 15];
      v >>= 4;
    } while (v);
    while (k) ch(t[--k]);
    return *this;
  }
  void flush() {
    if (n_) sink_(ctx_, buf_, n_);
    n_ = 0;
  }

 private:
  Sink sink_;
  void* ctx_;
  char buf_[512];
  size_t n_ = 0;
};

// ---- modules: two fixed tables, the crash path reads the finished one ----
constexpr int kMaxModules = 640;
struct Module {
  uintptr_t base;
  uintptr_t execStart, execEnd;
  char name[56];
  uint8_t idBytes[20];
  uint32_t idLen;
};
const Module* findModule(uintptr_t pc) noexcept;
Module* beginModules() noexcept;  // the table not in use; refreshing is one thread at a time
void endModules(int count) noexcept;
const char* baseName(const char* path) noexcept;
void copyText(char* out, size_t cap, const char* in) noexcept;
extern char gProgramName[56];  // the main program's module name

// ---- what a report says ----
constexpr int kMaxFrames = 64;
struct ReportInfo {
  const char* kind = "crash";      // crash, freeze, exception, ended, graphics
  const char* cause = nullptr;     // SIGSEGV, EXCEPTION_ACCESS_VIOLATION, ...
  int64_t code = 0;
  int64_t number = 0;
  uint64_t address = 0;
  bool hasAddress = false;
  const char* thread = nullptr;
  const char* where = nullptr;     // exception: what Gridline was doing
  const char* typeName = nullptr;  // exception: its type
  const char* message = nullptr;   // scrubbed
  const char* note = nullptr;
  uintptr_t pcs[kMaxFrames] = {};
  bool scanned[kMaxFrames] = {};   // found by scanning the stack, not by the frame chain
  int frames = 0;
};

// The common part (Crash.cpp); these run in signal context.
SessionBlock* session() noexcept;
const char* reportDirC() noexcept;
const char* recoveryDirC() noexcept;
const char* homeC() noexcept;
const char* userC() noexcept;
const char* terminateText() noexcept;
void setTerminateText(const char* text) noexcept;
void writeReport(const ReportInfo& info, Writer& w) noexcept;
// <kind>-YYYYMMDD-HHMMSS-<pid>.txt
void reportFileName(const char* kind, int64_t unixTime, int64_t pid, char* name, size_t cap) noexcept;
void joinPath(char* out, size_t cap, const char* dir, const char* name) noexcept;
// Writes the report under a new name in the report folder and tells the session.
bool writeReportToDisk(const ReportInfo& info, char* nameOut, size_t nameCap) noexcept;
// Paths of the fresh copy for this session.
void rescuePaths(char* tmp, size_t tmpCap, char* final, size_t finalCap, char* mark, size_t markCap) noexcept;
RescueFn rescueFn() noexcept;
bool audioCrashWanted() noexcept;

// ---- the platform parts (CrashPosix.cpp, CrashWin.cpp) ----
namespace plat {
void installHandlers(bool devBacktrace);
void refreshModules();
// Stack of the thread that runs the interface, asked from the watchdog thread.
void sampleUiStack(ReportInfo& out);
void currentStack(ReportInfo& out);
void currentThreadName(char* out, size_t cap);
const char* threadRole() noexcept;
void noteAudioThread() noexcept;
void* mapFile(const std::string& path, size_t size, bool create);
void unmapFile(void* mem, size_t size);
// New file, whole text or nothing visible under the final name (signal context).
bool writeFileAtomic(const char* tmp, const char* final, const ReportInfo& info) noexcept;
int64_t unixTime() noexcept;
void spawnSentinel(const std::string& self, const std::string& sessionFile, const std::string& hiddenArg);
void closeSentinelLink();
// Blocks at most pollMs. True when the parent ended.
bool waitParent(const char* link, int pollMs);
// Windows only knows the exit code here; 0 elsewhere.
uint32_t parentExitCode(const char* link);
void detachSentinel();
void endProcess(int64_t pid);
bool alive(int64_t pid);
bool spawnDetached(const std::string& exe, const std::vector<std::string>& args);
std::string selfPath();
std::string osDescription();
const char* archName() noexcept;
int64_t currentPid() noexcept;
// From the watchdog thread on request of the pop-up: a fresh copy in a child process.
void rescueNow();
void forceCrash(const std::string& kind);
void sleepMs(int ms);
}  // namespace plat

}  // namespace gl::crash::detail
