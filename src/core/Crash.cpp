// Gridline: crash reports, the part common to every system: breadcrumbs, the session block, the
// report text, the watchdog thread, the sentinel process and the reports on disk.
//
// Everything named "signal context" below runs after a fatal fault: only async-signal-safe calls,
// no allocation, no locks, no stdio. See CrashPosix.cpp and CrashWin.cpp for the platform parts.
#include "Crash.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <new>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <typeinfo>
#include "CrashInternal.h"
#include "GridlineVersion.h"
#include "Log.h"

namespace fs = std::filesystem;

namespace gl::crash {

namespace detail {

namespace {

std::atomic<SessionBlock*> gSession{nullptr};
std::atomic<int64_t> gBusyUntilMs{0};
std::atomic<bool> gInstalled{false};
std::atomic<RescueFn> gRescue{nullptr};
std::atomic<int> gAudioCrash{0};

char gReportDir[1024];
char gRecoveryDir[1024];
char gHome[512];
char gUser[128];
char gTerminate[480];
char gSessionId[48];

// The module tables: the crash path reads whichever is finished.
Module gModules[2][kMaxModules];
std::atomic<int> gModuleCount[2];
std::atomic<int> gModuleLive{0};

int64_t steadyMs() noexcept {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// FNV-1a; the id of a report comes from its kind, cause and stack.
struct Fnv {
  uint32_t h = 2166136261u;
  void add(const char* s) noexcept {
    for (; s && *s; ++s) h = (h ^ static_cast<uint8_t>(*s)) * 16777619u;
  }
  void add(uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i, v >>= 8) h = (h ^ static_cast<uint8_t>(v)) * 16777619u;
  }
};

void writeHexId(Writer& w, uint32_t v) noexcept {
  for (int i = 28; i >= 0; i -= 4) w.ch("0123456789ABCDEF"[(v >> i) & 15u]);
}

// Civil date of a unix time (days to y/m/d, Howard Hinnant's method): no localtime in signal context.
void utcParts(int64_t t, int& y, int& mo, int& d, int& h, int& mi, int& s) noexcept {
  const int64_t day = t >= 0 ? t / 86400 : (t - 86399) / 86400;
  const int64_t rem = t - day * 86400;
  h = static_cast<int>(rem / 3600);
  mi = static_cast<int>(rem % 3600 / 60);
  s = static_cast<int>(rem % 60);
  const int64_t z = day + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const int64_t doe = z - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  mo = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  y = static_cast<int>(yoe + era * 400 + (mo <= 2 ? 1 : 0));
}

void pad2(char*& p, int v) noexcept {
  *p++ = static_cast<char>('0' + (v / 10) % 10);
  *p++ = static_cast<char>('0' + v % 10);
}

void writeUtc(Writer& w, int64_t t) noexcept {
  int y, mo, d, h, mi, s;
  utcParts(t, y, mo, d, h, mi, s);
  char b[24];
  char* p = b;
  pad2(p, y / 100);
  pad2(p, y % 100);
  *p++ = '-';
  pad2(p, mo);
  *p++ = '-';
  pad2(p, d);
  *p++ = 'T';
  pad2(p, h);
  *p++ = ':';
  pad2(p, mi);
  *p++ = ':';
  pad2(p, s);
  *p++ = 'Z';
  *p = 0;
  w.str(b);
}

void writeCrumbs(const CrumbRing& r, Writer& w) noexcept {
  const uint64_t end = r.next.load(std::memory_order_acquire);
  const uint64_t begin = end > kCrumbs ? end - kCrumbs : 0;
  for (uint64_t n = begin; n < end; ++n) {
    const CrumbSlot& s = r.slots[n % kCrumbs];
    const uint64_t seq = s.seq.load(std::memory_order_acquire);
    if (seq != n + 1) continue;  // being written
    uint64_t words[7];
    for (int i = 0; i < 7; ++i) words[i] = s.text[i].load(std::memory_order_relaxed);
    char text[sizeof words + 1];
    std::memcpy(text, words, sizeof words);
    text[sizeof words] = 0;
    char clean[sizeof text];
    scrubText(text, clean, sizeof clean, gHome, gUser);
    const uint64_t ms = s.ms.load(std::memory_order_relaxed);
    w.str("  ").udec(ms / 1000).ch('.');
    const uint64_t frac = ms % 1000;
    w.ch(static_cast<char>('0' + frac / 100)).ch(static_cast<char>('0' + frac / 10 % 10)).ch(static_cast<char>('0' + frac % 10));
    w.str(" s ").str(clean);
    const uint64_t rep = s.repeat.load(std::memory_order_relaxed);
    if (rep) w.str(" (x").udec(rep + 1).ch(')');
    w.ch('\n');
  }
}

}  // namespace

SessionBlock* session() noexcept { return gSession.load(std::memory_order_relaxed); }
const char* reportDirC() noexcept { return gReportDir; }
const char* recoveryDirC() noexcept { return gRecoveryDir; }
const char* homeC() noexcept { return gHome; }
const char* userC() noexcept { return gUser; }
const char* terminateText() noexcept { return gTerminate; }
RescueFn rescueFn() noexcept { return gRescue.load(std::memory_order_relaxed); }
bool audioCrashWanted() noexcept { return gAudioCrash.load(std::memory_order_relaxed) != 0; }
char gProgramName[56] = "gridline-app";

void setTerminateText(const char* text) noexcept {
  scrubText(text, gTerminate, sizeof gTerminate, gHome, gUser);
}

void copyText(char* out, size_t cap, const char* in) noexcept {
  size_t n = 0;
  for (; in && *in && n + 1 < cap; ++in) out[n++] = *in;
  out[n] = 0;
}

void joinPath(char* out, size_t cap, const char* dir, const char* name) noexcept {
  size_t n = 0;
  for (; *dir && n + 2 < cap; ++dir) out[n++] = *dir;
#if defined(_WIN32)
  if (n + 1 < cap) out[n++] = '\\';
#else
  if (n + 1 < cap) out[n++] = '/';
#endif
  for (; *name && n + 1 < cap; ++name) out[n++] = *name;
  out[n] = 0;
}

const char* baseName(const char* path) noexcept {
  const char* b = path;
  for (const char* p = path; p && *p; ++p)
    if (*p == '/' || *p == '\\') b = p + 1;
  return b;
}

const Module* findModule(uintptr_t pc) noexcept {
  const int live = gModuleLive.load(std::memory_order_acquire);
  const int count = gModuleCount[live].load(std::memory_order_acquire);
  for (int i = 0; i < count; ++i) {
    const Module& m = gModules[live][i];
    if (pc >= m.execStart && pc < m.execEnd) return &m;
  }
  return nullptr;
}

Module* beginModules() noexcept { return gModules[1 - gModuleLive.load(std::memory_order_acquire)]; }

void endModules(int count) noexcept {
  const int next = 1 - gModuleLive.load(std::memory_order_acquire);
  gModuleCount[next].store(count, std::memory_order_release);
  gModuleLive.store(next, std::memory_order_release);
}

void reportFileName(const char* kind, int64_t t, int64_t pid, char* name, size_t cap) noexcept {
  int y, mo, d, h, mi, s;
  utcParts(t, y, mo, d, h, mi, s);
  char stamp[32];
  char* p = stamp;
  pad2(p, y / 100);
  pad2(p, y % 100);
  pad2(p, mo);
  pad2(p, d);
  *p++ = '-';
  pad2(p, h);
  pad2(p, mi);
  pad2(p, s);
  *p = 0;
  char num[24];
  int k = 0;
  uint64_t v = static_cast<uint64_t>(pid);
  char rev[24];
  do {
    rev[k++] = static_cast<char>('0' + v % 10);
    v /= 10;
  } while (v);
  int m = 0;
  while (k) num[m++] = rev[--k];
  num[m] = 0;
  size_t n = 0;
  auto put = [&](const char* t2) {
    for (; *t2 && n + 1 < cap; ++t2) name[n++] = *t2;
  };
  put(kind);
  put("-");
  put(stamp);
  put("-");
  put(num);
  put(".txt");
  name[n] = 0;
}

// The code, signal context. Its lines are "key: value"; tools/crash_symbolize.py reads them.
void writeReportFor(const SessionBlock* s, const CrumbRing& r, const ReportInfo& info, Writer& w) noexcept {
  Fnv fnv;
  fnv.add(info.kind);
  fnv.add(info.cause);
  fnv.add(info.where);
  for (int i = 0, used = 0; i < info.frames && used < 12; ++i) {
    if (info.scanned[i]) continue;
    const Module* m = findModule(info.pcs[i]);
    fnv.add(m ? m->name : "?");
    fnv.add(static_cast<uint64_t>(m ? info.pcs[i] - m->base : 0));
    ++used;
  }
  w.str("GRIDLINE CRASH CODE 1\nid: GL-");
  writeHexId(w, fnv.h);
  w.str("\nkind: ").str(info.kind).ch('\n');
  if (info.cause) {
    w.str("cause: ").str(info.cause);
    if (info.number) w.str(" (").dec(info.number).ch(')');
    if (info.code) w.str(" code ").dec(info.code);
    if (info.hasAddress) w.str(" address ").hex(info.address);
    w.ch('\n');
  }
  if (info.note) w.str("note: ").str(info.note).ch('\n');
  if (info.where) w.str("doing: ").str(info.where).ch('\n');
  if (info.typeName) w.str("type: ").str(info.typeName).ch('\n');
  if (info.message && *info.message) w.str("message: ").str(info.message).ch('\n');
  if (info.thread) w.str("thread: ").str(info.thread).ch('\n');
  const int64_t now = plat::unixTime();
  w.str("time: ");
  writeUtc(w, now);
  w.ch('\n');
  if (s) {
    w.str("uptime: ").dec(now - s->startUnix).str(" s\n");
    w.str("session: ").str(s->sessionName).ch('\n');
    w.str("version: ").str(s->version).ch('\n');
    w.str("build: ").str(s->build).ch('\n');
    w.str("arch: ").str(s->arch).ch('\n');
    w.str("os: ").str(s->os).ch('\n');
    w.str("gpu: ").str(s->gpu[0] ? s->gpu : "not known yet").ch('\n');
    w.str("unsaved work: ").str(s->unsaved.load(std::memory_order_relaxed) ? "yes" : "no").ch('\n');
  }
  w.str("stack:\n");
  for (int i = 0; i < info.frames; ++i) {
    const Module* m = findModule(info.pcs[i]);
    w.str("  ").dec(i).ch(' ');
    if (m) w.str(m->name).ch('+').hex(info.pcs[i] - m->base);
    else w.str("unknown ").hex(info.pcs[i]);
    if (info.scanned[i]) w.str(" ?");
    w.ch('\n');
  }
  if (info.frames == 0) w.str("  (not available)\n");
  w.str("modules:\n");
  for (int i = 0; i < info.frames; ++i) {
    const Module* m = findModule(info.pcs[i]);
    if (!m) continue;
    bool seen = false;
    for (int j = 0; j < i && !seen; ++j) seen = findModule(info.pcs[j]) == m;
    if (seen) continue;
    w.str("  ").str(m->name).str(" id ");
    for (uint32_t b = 0; b < m->idLen; ++b) w.ch("0123456789abcdef"[m->idBytes[b] >> 4]).ch("0123456789abcdef"[m->idBytes[b] & 15]);
    if (m->idLen == 0) w.ch('-');
    w.ch('\n');
  }
  w.str("recent actions:\n");
  writeCrumbs(r, w);
  w.str("END GRIDLINE CRASH CODE\n");
}

void writeReport(const ReportInfo& info, Writer& w) noexcept {
  SessionBlock* s = session();
  writeReportFor(s, s ? s->crumbs : crumbRing(), info, w);
}

bool writeReportToDisk(const ReportInfo& info, char* nameOut, size_t nameCap) noexcept {
  char name[128], tmp[1200], final[1200];
  reportFileName(info.kind, plat::unixTime(), plat::currentPid(), name, sizeof name);
  // Failures in the same second keep their own files (exceptions: the app goes on after them).
  static std::atomic<unsigned> sequence{0};
  if (std::strcmp(info.kind, "exception") == 0) {
    const unsigned k = sequence.fetch_add(1);
    const size_t len = std::strlen(name);
    if (k > 0 && len > 4 && len + 8 < sizeof name) {
      char* p = name + len - 4;
      *p++ = '-';
      *p++ = static_cast<char>('a' + (k / 676) % 26);
      *p++ = static_cast<char>('a' + (k / 26) % 26);
      *p++ = static_cast<char>('a' + k % 26);
      copyText(p, 5, ".txt");
    }
  }
  joinPath(final, sizeof final, gReportDir, name);
  joinPath(tmp, sizeof tmp, gReportDir, ".writing");
  // Two reports in the same second (an exception on top of an exception) share nothing but the name.
  const bool ok = plat::writeFileAtomic(tmp, final, info);
  if (nameOut) copyText(nameOut, nameCap, name);
  return ok;
}

void rescuePaths(char* tmp, size_t tmpCap, char* final, size_t finalCap, char* mark, size_t markCap) noexcept {
  char n[96];
  size_t k = 0;
  auto put = [&](const char* t) {
    for (; *t && k + 1 < sizeof n; ++t) n[k++] = *t;
    n[k] = 0;
  };
  put(gSessionId);
  put(".rescue.tmp.gridline.json");
  joinPath(tmp, tmpCap, gRecoveryDir, n);
  k = 0;
  put(gSessionId);
  put(".gridline.json");
  joinPath(final, finalCap, gRecoveryDir, n);
  k = 0;
  put(gSessionId);
  put(".rescued");
  joinPath(mark, markCap, gRecoveryDir, n);
}

}  // namespace detail

using namespace detail;

// ---- scrubbing: no allocation ----
namespace {

bool pathChar(char c) { return c != 0 && c != '"' && c != '\'' && c != ':' && c != ',' && c != ';' && c != ')' && c != '(' && c != '\n' && c != '\r' && c != '\t' && c != '<' && c != '>'; }

bool startsPath(const char* p, const char* begin) {
  const bool boundary = p == begin || p[-1] == ' ' || p[-1] == '"' || p[-1] == '\'' || p[-1] == '(' || p[-1] == '=' || p[-1] == ':' || p[-1] == '\t' || p[-1] == '\n';
  if (!boundary) return false;
  if (p[0] == '/' && p[1] && p[1] != ' ') return true;                                   // /home/me
  if (p[0] == '~' && (p[1] == '/' || p[1] == '\\')) return true;                         // ~/x
  if (p[0] == '\\' && p[1] == '\\') return true;                                         // \\server\share
  if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':' && (p[2] == '/' || p[2] == '\\')) return true;  // C:\x
  if (std::strncmp(p, "file://", 7) == 0) return true;
  return false;
}

// A word after a space still belongs to a path when it holds a folder separator or looks like a file name.
bool wordContinuesPath(const char* p) {
  bool sep = false, dot = false;
  for (; *p && *p != ' ' && pathChar(*p); ++p) {
    if (*p == '/' || *p == '\\') sep = true;
    if (*p == '.' && p[1] && p[1] != ' ') dot = true;
  }
  return sep || dot;
}

bool ieq(char a, char b) {
  if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
  if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
  return a == b;
}

size_t matchAt(const char* p, const char* word) {
  if (!word || !*word) return 0;
  size_t n = 0;
  for (; word[n]; ++n)
    if (!p[n] || !ieq(p[n], word[n])) return 0;
  return n;
}

}  // namespace

void scrubText(const char* in, char* out, size_t cap, const char* homeDir, const char* userName) noexcept {
  if (cap == 0) return;
  size_t n = 0;
  auto put = [&](const char* t) {
    for (; *t && n + 1 < cap; ++t) out[n++] = *t;
  };
  const char* begin = in;
  const bool usableUser = userName && std::strlen(userName) >= 3;
  for (const char* p = in; p && *p && n + 1 < cap;) {
    if (startsPath(p, begin)) {
      const char quote = p > begin && (p[-1] == '"' || p[-1] == '\'') ? p[-1] : 0;
      if (std::strncmp(p, "file://", 7) == 0) p += 7;
      else if (p[1] == ':') p += 2;  // C:
      while (*p) {
        if (quote ? *p == quote : !pathChar(*p)) break;
        if (!quote && *p == ' ' && !wordContinuesPath(p + 1)) break;
        ++p;
      }
      put("<path>");
      continue;
    }
    if (const size_t k = matchAt(p, homeDir)) {
      put("<path>");
      p += k;
      continue;
    }
    if (usableUser) {
      if (const size_t k = matchAt(p, userName)) {
        put("<user>");
        p += k;
        continue;
      }
    }
    if (static_cast<unsigned char>(*p) < 0x20 && *p != ' ') {
      out[n++] = ' ';  // one line per field
      ++p;
      continue;
    }
    out[n++] = *p++;
  }
  out[n] = 0;
}

// ---- heartbeat and friends ----
void uiHeartbeat() noexcept {
  if (SessionBlock* s = session()) s->heartbeat.fetch_add(1, std::memory_order_relaxed);
}
void armWatchdog() noexcept {
  if (SessionBlock* s = session()) s->armed.store(1, std::memory_order_relaxed);
}
void noteAudioThread() noexcept { plat::noteAudioThread(); }
void setRescue(RescueFn fn) noexcept { gRescue.store(fn, std::memory_order_relaxed); }
void setUnsavedWork(bool unsaved) noexcept {
  if (SessionBlock* s = session()) s->unsaved.store(unsaved ? 1 : 0, std::memory_order_relaxed);
}
bool installed() { return gInstalled.load(); }
std::string sessionId() { return gSessionId; }
bool audioCrashRequested() noexcept { return audioCrashWanted(); }

BusyScope::BusyScope(const char* what, int maxSeconds) noexcept {
  previous_ = gBusyUntilMs.load(std::memory_order_relaxed);
  gBusyUntilMs.store(steadyMs() + int64_t{maxSeconds} * 1000, std::memory_order_relaxed);
  if (what) crumb("busy", what);
}
BusyScope::~BusyScope() {
  gBusyUntilMs.store(previous_, std::memory_order_relaxed);
  uiHeartbeat();  // the time spent counts from now
}

std::string reportsDir(const std::string& configDir) { return (fs::u8path(configDir) / "crash-reports").u8string(); }
std::string recoveryDir(const std::string& configDir) { return (fs::u8path(configDir) / "recovery").u8string(); }
std::string sessionFileOf(const std::string& configDir, const std::string& sid) {
  return (fs::u8path(reportsDir(configDir)) / ("session-" + sid + ".bin")).u8string();
}

// ---- the watchdog thread ----
namespace {

std::atomic_flag gRefreshing = ATOMIC_FLAG_INIT;
void refreshModulesNow() {
  if (gRefreshing.test_and_set()) return;
  plat::refreshModules();
  gRefreshing.clear();
}

struct Watchdog {
  std::thread thread;
  std::mutex mu;
  std::condition_variable cv;
  bool stop = false;
};
Watchdog* gWatchdog = nullptr;
int gFrozenAfterMs = 30000;
std::string gFreezeReport;  // watchdog thread, until it was joined

std::string readFile(const std::string& path) {
  std::ifstream f(fs::u8path(path), std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

void watchdogLoop(Watchdog* w) {
  FreezeDetector det(gFrozenAfterMs);
  int64_t snoozeUntil = 0;
  std::string& freezeReport = gFreezeReport;
  unsigned tick = 0;
  SessionBlock* s = session();
  for (;;) {
    {
      std::unique_lock<std::mutex> lk(w->mu);
      if (w->cv.wait_for(lk, std::chrono::milliseconds(500), [w] { return w->stop; })) return;
    }
    if ((++tick & 3u) == 0) refreshModulesNow();  // plugins come and go; the crash path cannot ask
    if (s->rescueRequest.exchange(0)) {
      plat::rescueNow();
      s->rescueDone.store(1);
    }
    const int64_t now = steadyMs();
    if (s->snooze.exchange(0)) {
      snoozeUntil = now + 120000;  // two more minutes before asking again
      det.rearm(now);
      if (!freezeReport.empty()) {
        std::error_code ec;
        fs::remove(fs::u8path(freezeReport), ec);
        freezeReport.clear();
      }
      s->reportReady.store(0);
      s->frozen.store(0);
    }
    const auto act = det.update(now, s->heartbeat.load(std::memory_order_relaxed), s->armed.load(std::memory_order_relaxed) != 0,
                                gBusyUntilMs.load(std::memory_order_relaxed), snoozeUntil);
    if (act == FreezeDetector::Action::Frozen) {
      ReportInfo info;
      info.kind = "freeze";
      info.cause = "the interface stopped answering";
      info.thread = "ui";
      plat::sampleUiStack(info);
      char name[128];
      if (writeReportToDisk(info, name, sizeof name)) {
        copyText(s->reportName, sizeof s->reportName, name);
        s->reportReady.store(1);
        freezeReport = (fs::u8path(gReportDir) / name).u8string();
      }
      s->frozen.store(1);
      logging::warning("app", "The interface has not answered for a while; a freeze report was written");
    } else if (act == FreezeDetector::Action::Recovered) {
      // It came back: long work, not a hang. No report stays behind and the pop-up closes itself.
      if (!freezeReport.empty()) {
        std::error_code ec;
        fs::remove(fs::u8path(freezeReport), ec);
        freezeReport.clear();
      }
      s->reportReady.store(0);
      s->reportName[0] = 0;
      s->frozen.store(0);
      logging::warning("app", "The interface answers again after a long stall");
      crumb("ui came back after a stall");
    }
  }
}

void fillSession(SessionBlock* s, int64_t startUnix, const std::string& sid) {
  s->size = sizeof(SessionBlock);
  s->pid = plat::currentPid();
  s->startUnix = startUnix;
  copyText(s->sessionName, sizeof s->sessionName, sid.c_str());
  copyText(s->version, sizeof s->version, GRIDLINE_VERSION);
  copyText(s->build, sizeof s->build, GRIDLINE_BUILD_ID);
  copyText(s->os, sizeof s->os, plat::osDescription().c_str());
  copyText(s->arch, sizeof s->arch, plat::archName());
  s->magic = kSessionMagic;
}

}  // namespace

void install(const Config& c) {
  if (gInstalled.exchange(true)) return;
  setCrumbClockStart(steadyMs());
  std::error_code ec;
  const std::string reports = reportsDir(c.configDir), recovery = recoveryDir(c.configDir);
  fs::create_directories(fs::u8path(reports), ec);
  fs::create_directories(fs::u8path(recovery), ec);
  copyText(gReportDir, sizeof gReportDir, reports.c_str());
  copyText(gRecoveryDir, sizeof gRecoveryDir, recovery.c_str());
#if defined(_WIN32)
  if (const char* h = std::getenv("USERPROFILE")) copyText(gHome, sizeof gHome, h);
  if (const char* u = std::getenv("USERNAME")) copyText(gUser, sizeof gUser, u);
#else
  if (const char* h = std::getenv("HOME")) copyText(gHome, sizeof gHome, h);
  if (const char* u = std::getenv("USER")) copyText(gUser, sizeof gUser, u);
#endif
  const int64_t start = plat::unixTime();
  const std::string sid = std::to_string(start) + "-" + std::to_string(plat::currentPid());
  copyText(gSessionId, sizeof gSessionId, sid.c_str());
  gFrozenAfterMs = std::max(1, c.frozenAfterSeconds) * 1000;

  const std::string file = sessionFileOf(c.configDir, sid);
  auto* block = static_cast<SessionBlock*>(plat::mapFile(file, sizeof(SessionBlock), true));
  if (!block) {  // a read-only or full config folder: memory only, the crash path still works
    static SessionBlock fallback{};
    block = &fallback;
  }
  fillSession(block, start, sid);
  setCrumbRing(&block->crumbs);
  gSession.store(block, std::memory_order_release);

  refreshModulesNow();
  plat::installHandlers(c.devBacktrace);
  setHooks(refreshModulesNow, [](const char* where) { reportCurrentException(where); });
  std::atexit([] { markCleanExit(); });  // exit() is a decision, not a failure
  crumb("start");
  if (c.sentinel) {
    const std::string self = c.selfPath.empty() ? plat::selfPath() : c.selfPath;
    std::string test;
    if (const char* t = std::getenv("GRIDLINE_CRASH_TEST_SENTINEL")) test = t;
    if (!self.empty()) plat::spawnSentinel(self, file, test);
  }
  if (c.watchdog) {
    gWatchdog = new Watchdog();
    gWatchdog->thread = std::thread(watchdogLoop, gWatchdog);
  }
}

void markCleanExit() {
  SessionBlock* s = session();
  if (!s) return;
  if (s->cleanExit.exchange(1)) return;
  crumb("clean exit");
  if (Watchdog* w = gWatchdog) {
    gWatchdog = nullptr;
    {
      std::lock_guard<std::mutex> lk(w->mu);
      w->stop = true;
    }
    w->cv.notify_all();
    if (w->thread.joinable()) w->thread.join();
  }
  // It came back and ended properly before the watchdog looked again: no hang to report.
  if (!gFreezeReport.empty()) {
    std::error_code ec;
    fs::remove(fs::u8path(gFreezeReport), ec);
    gFreezeReport.clear();
    s->reportReady.store(0);
    s->frozen.store(0);
  }
}

void setGpu(const char* vendor, const char* renderer, const char* version) {
  SessionBlock* s = session();
  if (!s) return;
  std::string t = std::string(renderer ? renderer : "?") + " / " + (vendor ? vendor : "?") + " / " + (version ? version : "?");
  copyText(s->gpu, sizeof s->gpu, t.c_str());
}

void endAfterGraphicsLoss(const char* why) {
  if (installed()) {
    crumb("graphics lost for good", why);
    char clean[480];
    scrubText(why ? why : "", clean, sizeof clean, gHome, gUser);
    char thread[64];
    plat::currentThreadName(thread, sizeof thread);
    ReportInfo info;
    info.kind = "graphics";
    info.cause = "graphics driver reset";
    info.note = "a new OpenGL context could not be made";
    info.message = clean;
    info.thread = thread;
    plat::currentStack(info);
    SessionBlock* s = session();
    char name[128];
    if (writeReportToDisk(info, name, sizeof name) && s) {
      copyText(s->reportName, sizeof s->reportName, name);
      s->reportReady.store(1);
      s->frozen.store(0);
    }
    // This thread is healthy and the song is whole: the copy is a plain save.
    RescueFn fn = rescueFn();
    if (s && fn && s->unsaved.load(std::memory_order_relaxed)) {
      char tmp[1200], final[1200], mark[1200];
      rescuePaths(tmp, sizeof tmp, final, sizeof final, mark, sizeof mark);
      std::error_code ec;
      if (fn(tmp)) {
        fs::rename(fs::u8path(tmp), fs::u8path(final), ec);
        if (!ec) std::ofstream(fs::u8path(mark), std::ios::trunc) << plat::unixTime();
      } else {
        fs::remove(fs::u8path(tmp), ec);
      }
    }
  }
  std::fprintf(stderr, "Gridline lost its graphics driver and could not get it back; a report was written.\n");
  std::fflush(nullptr);
  std::_Exit(3);
}

// ---- recoverable failures ----
namespace {

std::mutex gRecMu;
std::deque<Recoverable> gRecQueue;
std::map<std::string, int64_t> gRecSeen;

std::string idOfText(const std::string& code) {
  const size_t p = code.find("id: ");
  if (p == std::string::npos) return {};
  const size_t e = code.find('\n', p);
  return code.substr(p + 4, e == std::string::npos ? std::string::npos : e - p - 4);
}

}  // namespace

std::string reportRecoverable(const char* where, const char* typeName, const std::string& what) {
  if (!installed()) {
    logging::error("app", std::string(where ? where : "") + ": " + what);
    return {};
  }
  char clean[480];
  scrubText(what.c_str(), clean, sizeof clean, gHome, gUser);
  char thread[64];
  plat::currentThreadName(thread, sizeof thread);
  ReportInfo info;
  info.kind = "exception";
  info.where = where;
  info.typeName = typeName;
  info.message = clean;
  info.thread = thread;
  plat::currentStack(info);
  crumb("exception in", where);
  char name[128];
  // Reports of different threads must not share the file being written.
  std::lock_guard<std::mutex> lk(gRecMu);
  if (!writeReportToDisk(info, name, sizeof name)) return {};
  const std::string path = (fs::u8path(gReportDir) / name).u8string();
  Recoverable r;
  r.where = where ? where : "";
  r.reportPath = path;
  r.code = readFile(path);
  r.id = idOfText(r.code);
  markSeen(path);  // the pop-up shows it now; the next start has nothing to add
  logging::error("app", std::string("Internal error while: ") + r.where + " (" + r.id + ")");
  const int64_t now = plat::unixTime();
  auto it = gRecSeen.find(r.id);
  if (it != gRecSeen.end() && now - it->second < 60) return r.id;  // no second pop-up for the same failure
  gRecSeen[r.id] = now;
  gRecQueue.push_back(std::move(r));
  return gRecQueue.back().id;
}

std::string reportCurrentException(const char* where) {
  try {
    throw;
  } catch (const std::exception& e) {
    return reportRecoverable(where, typeid(e).name(), e.what());
  } catch (...) {
    return reportRecoverable(where, "unknown", "an exception of an unknown type");
  }
}

bool takeRecoverable(Recoverable& out) {
  std::lock_guard<std::mutex> lk(gRecMu);
  if (gRecQueue.empty()) return false;
  out = std::move(gRecQueue.front());
  gRecQueue.pop_front();
  return true;
}

// ---- reports on disk ----
namespace {

std::string fieldOf(const std::string& text, const char* key) {
  const std::string k = std::string("\n") + key + ": ";
  const size_t p = text.find(k);
  if (p == std::string::npos) return {};
  const size_t b = p + k.size();
  const size_t e = text.find('\n', b);
  return text.substr(b, e == std::string::npos ? std::string::npos : e - b);
}

// Writes the report of a session that ended without one (normal code, another process).
std::string writeEndedReport(const SessionBlock* s, const std::string& configDir) {
  char name[128];
  reportFileName("ended", plat::unixTime(), s->pid, name, sizeof name);
  const std::string path = (fs::u8path(reportsDir(configDir)) / name).u8string();
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return {};
  ReportInfo info;
  info.kind = "ended";
  info.cause = "the process ended without a report";
  const uint32_t code = s->exitCode.load();
  info.code = code;
  info.note = "closed from outside, out of memory, or a failure the handler could not catch";
  Writer w([](void* ctx, const char* d, size_t n) { std::fwrite(d, 1, n, static_cast<std::FILE*>(ctx)); }, f);
  writeReportFor(s, s->crumbs, info, w);
  w.flush();
  std::fclose(f);
  return path;
}

void removeQuietly(const fs::path& p) {
  std::error_code ec;
  fs::remove(p, ec);
}

// A session whose process is gone: its report's path ("" when it ended well or nothing is left to show).
std::string finishSessionFile(const fs::path& file, const std::string& configDir) {
  auto* s = static_cast<SessionBlock*>(plat::mapFile(file.u8string(), sizeof(SessionBlock), false));
  std::string report;
  if (s && s->magic == kSessionMagic && s->size == sizeof(SessionBlock)) {
    if (s->pid == plat::currentPid() || plat::alive(s->pid)) {
      plat::unmapFile(s, sizeof(SessionBlock));
      return {};
    }
    if (s->handled.load()) {
      if (s->reportReady.load()) markSeen((fs::u8path(reportsDir(configDir)) / s->reportName).u8string());
    } else if (s->reportReady.load()) {
      report = (fs::u8path(reportsDir(configDir)) / s->reportName).u8string();
    } else if (!s->cleanExit.load()) {
      report = writeEndedReport(s, configDir);
    }
  }
  if (s) plat::unmapFile(s, sizeof(SessionBlock));
  removeQuietly(file);
  return report;
}

bool isOwnedByLiveProcess(const std::string& name) {
  // freeze-<stamp>-<pid>.txt of another running Gridline: still its own business
  const size_t dash = name.rfind('-'), dot = name.rfind('.');
  if (dash == std::string::npos || dot == std::string::npos || dot <= dash) return false;
  const int64_t pid = std::strtoll(name.substr(dash + 1, dot - dash - 1).c_str(), nullptr, 10);
  return name.rfind("freeze-", 0) == 0 && pid != plat::currentPid() && plat::alive(pid);
}

bool showingElsewhere(const fs::path& report) {
  fs::path mark = report;
  mark += ".showing";
  std::error_code ec;
  if (!fs::exists(mark, ec)) return false;
  const int64_t pid = std::strtoll(readFile(mark.u8string()).c_str(), nullptr, 10);
  if (pid > 0 && plat::alive(pid)) return true;
  removeQuietly(mark);
  return false;
}

}  // namespace

PendingReport readReport(const std::string& configDir, const std::string& path) {
  PendingReport r;
  r.path = path;
  r.text = readFile(path);
  r.kind = fieldOf(r.text, "kind");
  r.id = fieldOf(r.text, "id");
  if (r.text.find("id: ") == 0) r.id = idOfText(r.text);
  r.sessionId = fieldOf(r.text, "session");
  std::error_code ec;
  const auto t = fs::last_write_time(fs::u8path(path), ec);
  if (!ec) {
    const auto sys = std::chrono::time_point_cast<std::chrono::seconds>(t - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    r.crashTime = sys.time_since_epoch().count();
  }
  if (!r.sessionId.empty()) {
    const fs::path rec = fs::u8path(recoveryDir(configDir));
    const fs::path copy = rec / (r.sessionId + ".gridline.json");
    if (fs::exists(copy, ec)) {
      r.recoveryFile = copy.u8string();
      std::ifstream meta(rec / (r.sessionId + ".meta"));
      std::string line;
      while (std::getline(meta, line)) {
        if (line.rfind("original=", 0) == 0) r.originalProject = line.substr(9);
        if (line.rfind("saved=", 0) == 0) r.recoverySaved = std::strtoll(line.c_str() + 6, nullptr, 10);
      }
      if (fs::exists(rec / (r.sessionId + ".rescued"), ec)) {
        r.recoveryRescued = true;
        r.recoverySaved = std::strtoll(readFile((rec / (r.sessionId + ".rescued")).u8string()).c_str(), nullptr, 10);
      }
    }
  }
  return r;
}

std::vector<PendingReport> collectPending(const std::string& configDir) {
  std::vector<PendingReport> out;
  const fs::path dir = fs::u8path(reportsDir(configDir));
  std::error_code ec;
  std::vector<fs::path> sessions, reports;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string n = it->path().filename().u8string();
    if (n.rfind("session-", 0) == 0 && it->path().extension() == ".bin") sessions.push_back(it->path());
  }
  for (const fs::path& s : sessions) finishSessionFile(s, configDir);
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    if (it->path().extension() == ".txt") reports.push_back(it->path());
  std::sort(reports.begin(), reports.end(), [](const fs::path& a, const fs::path& b) {
    std::error_code e1, e2;
    return fs::last_write_time(a, e1) < fs::last_write_time(b, e2);
  });
  for (const fs::path& p : reports) {
    fs::path seen = p;
    seen += ".seen";
    std::error_code e;
    if (fs::exists(seen, e) || isOwnedByLiveProcess(p.filename().u8string()) || showingElsewhere(p)) continue;
    out.push_back(readReport(configDir, p.u8string()));
  }
  return out;
}

void markSeen(const std::string& path) {
  std::ofstream(fs::u8path(path + ".seen")) << "seen\n";
}

void discardRecovery(const std::string& configDir, const std::string& sid) {
  if (sid.empty()) return;
  const fs::path rec = fs::u8path(recoveryDir(configDir));
  for (const char* ext : {".gridline.json", ".meta", ".rescued", ".rescue.tmp.gridline.json"}) removeQuietly(rec / (sid + ext));
}

void sweep(const std::string& configDir) {
  const auto now = fs::file_time_type::clock::now();
  const auto limit = std::chrono::hours(24 * 30);
  std::error_code ec;
  std::vector<fs::path> reports;
  const fs::path dir = fs::u8path(reportsDir(configDir));
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    if (it->path().extension() == ".txt") reports.push_back(it->path());
  std::sort(reports.begin(), reports.end(), [](const fs::path& a, const fs::path& b) {
    std::error_code e1, e2;
    return fs::last_write_time(a, e1) > fs::last_write_time(b, e2);
  });
  for (size_t i = 0; i < reports.size(); ++i) {
    std::error_code e;
    const auto t = fs::last_write_time(reports[i], e);
    if (i >= 30 || (!e && now - t > limit)) {
      fs::path seen = reports[i];
      seen += ".seen";
      removeQuietly(seen);
      removeQuietly(reports[i]);
    }
  }
  const fs::path rec = fs::u8path(recoveryDir(configDir));
  for (fs::directory_iterator it(rec, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e;
    const auto t = fs::last_write_time(it->path(), e);
    if (!e && now - t > limit) removeQuietly(it->path());
  }
}

std::string extractCode(const std::string& pasted) {
  const size_t a = pasted.find("GRIDLINE CRASH CODE 1");
  if (a == std::string::npos) return {};
  const std::string endMark = "END GRIDLINE CRASH CODE";
  const size_t b = pasted.find(endMark, a);
  std::string body = pasted.substr(a, b == std::string::npos ? std::string::npos : b + endMark.size() - a);
  std::string out;
  std::istringstream in(body);
  std::string line;
  while (std::getline(in, line)) {
    size_t i = 0;
    while (i < line.size() && (line[i] == '>' || line[i] == '`')) ++i;  // a quoted or fenced paste
    if (i && i < line.size() && line[i] == ' ') ++i;
    out += line.substr(i) + "\n";
  }
  return out;
}

// ---- the session seen from the pop-up process ----
struct SessionView {
  SessionBlock* block = nullptr;
};

SessionView* openSession(const std::string& file) {
  auto* b = static_cast<SessionBlock*>(plat::mapFile(file, sizeof(SessionBlock), false));
  if (!b) return nullptr;
  if (b->magic != kSessionMagic || b->size != sizeof(SessionBlock)) {
    plat::unmapFile(b, sizeof(SessionBlock));
    return nullptr;
  }
  auto* v = new SessionView;
  v->block = b;
  return v;
}
void closeSession(SessionView* s) {
  if (!s) return;
  plat::unmapFile(s->block, sizeof(SessionBlock));
  delete s;
}
uint64_t sessionHeartbeat(const SessionView* s) { return s ? s->block->heartbeat.load() : 0; }
bool sessionFrozen(const SessionView* s) { return s && s->block->frozen.load() != 0; }
int64_t sessionPid(const SessionView* s) { return s ? s->block->pid : 0; }
void sessionSnooze(SessionView* s) {
  if (s) s->block->snooze.store(1);
}
void sessionForceEnd(SessionView* s, int waitMs) {
  if (!s) return;
  s->block->rescueRequest.store(1);
  for (int t = 0; t < waitMs && !s->block->rescueDone.load(); t += 50) plat::sleepMs(50);
  s->block->handled.store(1);
  plat::endProcess(s->block->pid);
}
bool processAlive(int64_t pid) { return plat::alive(pid); }
std::string selfPath() { return plat::selfPath(); }
int64_t ownPid() { return plat::currentPid(); }
bool startSelf(const std::vector<std::string>& args) { return plat::spawnDetached(plat::selfPath(), args); }

// ---- the sentinel ----
int sentinelMain(int argc, char** argv) {
  // --crash-sentinel <session file> <link> [test folder]
  if (argc < 4) return 2;
  plat::detachSentinel();
  const std::string file = argv[2];
  const char* link = argv[3];
  const std::string testDir = argc > 4 ? argv[4] : "";
  const fs::path filePath = fs::u8path(file);
  const std::string configDir = filePath.parent_path().parent_path().u8string();
  auto* s = static_cast<SessionBlock*>(plat::mapFile(file, sizeof(SessionBlock), false));
  if (!s || s->magic != kSessionMagic) {
    // No shared block: wait for the end anyway; the next start finds what is left.
    while (!plat::waitParent(link, 1000)) {
    }
    return 0;
  }
  auto showDialog = [&](const std::string& report) {
    std::vector<std::string> args = {"--crash-dialog", report, "--crash-session", file};
    if (!testDir.empty()) {
      args.insert(args.end(), {"--hidden", "--screenshot", (fs::u8path(testDir) / "dialog.ppm").u8string(), "--frames", "6"});
    }
    plat::spawnDetached(plat::selfPath(), args);
  };
  bool freezeShown = false;
  while (!plat::waitParent(link, 500)) {
    const bool frozen = s->frozen.load() != 0 && s->reportReady.load() != 0;
    if (frozen && !freezeShown) {
      freezeShown = true;
      showDialog((fs::u8path(reportsDir(configDir)) / s->reportName).u8string());
    } else if (!frozen) {
      freezeShown = false;
    }
  }
  s->exitCode.store(plat::parentExitCode(link));
  // The process may still be finishing its report through the fork that saves the copy: the link
  // closes only when both have gone, so what is here is final.
  std::string report;
  const bool handled = s->handled.load() != 0;
  if (s->reportReady.load()) report = (fs::u8path(reportsDir(configDir)) / s->reportName).u8string();
  else if (!s->cleanExit.load() && !handled) report = writeEndedReport(s, configDir);
  const bool dialogUp = freezeShown;  // the freeze pop-up sees the end and shows the report itself
  plat::unmapFile(s, sizeof(SessionBlock));
  if (!report.empty() && !handled && !dialogUp) showDialog(report);
  if (!dialogUp) removeQuietly(filePath);
  return 0;
}

// ---- tests ----
void forceForTest(const std::string& kind) {
  if (kind == "audio") {
    gAudioCrash.store(1);
    return;
  }
  if (kind == "exception") throw std::runtime_error("forced failure in /Users/someone/Secret Songs/my song.flp: test");
  plat::forceCrash(kind);
}

void crashNowForTest() {
  plat::forceCrash("segv");
  std::abort();
}

}  // namespace gl::crash
