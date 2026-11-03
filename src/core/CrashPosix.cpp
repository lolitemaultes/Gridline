// Gridline: crash reports on macOS, Linux and the BSDs. The handlers, the frame walk and the module
// table. Everything from onFatal down runs in signal context: raw system calls and our own
// formatting only, no allocation, no locks, no stdio.
#include "CrashInternal.h"
#if !defined(_WIN32)
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#if defined(__APPLE__)
#include <sys/ucontext.h>
#elif !defined(__OpenBSD__)  // (there ucontext_t comes with <signal.h>)
#include <ucontext.h>
#endif
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <string>
#include <thread>
#include <typeinfo>
#include <vector>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <CoreFoundation/CoreFoundation.h>
#include <sys/sysctl.h>
#else
#include <link.h>
#endif

extern char** environ;

namespace gl::crash::detail {

namespace {

const int kFatal[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP, SIGSYS};
const int kSampleSignal = SIGUSR2;

pthread_t gUiThread;
std::atomic<pthread_t> gAudioThread{};
std::atomic<bool> gAudioSeen{false};
int gProbe[2] = {-1, -1};     // a pipe: write() tells whether memory can be read, without a fault
int gReservedFd = -1;         // given back when the process ran out of descriptors
int gSentinelLink = -1;
char gStack[1024 * 1024];     // the spare stack (untouched until a fault; serialising the rescue copy runs on it too)
std::atomic<int> gInFatal{0};
std::atomic<pthread_t> gFatalThread{};

ReportInfo gSample;           // the watchdog's look at the UI thread
std::atomic<int> gSampleState{0};
ReportInfo gFatalInfo;        // static: a report must fit on any stack
char gThreadText[64];

// ---- safe reads ----
bool safeRead(uintptr_t addr, void* out, size_t n) noexcept {
  const ssize_t w = ::write(gProbe[1], reinterpret_cast<const void*>(addr), n);
  if (w != static_cast<ssize_t>(n)) {
    if (w > 0) {
      char t[64];
      (void)!::read(gProbe[0], t, sizeof t);
    }
    return false;
  }
  return ::read(gProbe[0], out, n) == static_cast<ssize_t>(n);
}

uintptr_t stripPointer(uintptr_t v) noexcept {
#if defined(__aarch64__)
  return v & 0x0000FFFFFFFFFFFFull;  // pointer authentication bits
#else
  return v;
#endif
}

// ---- registers of the faulting thread ----
struct Regs {
  uintptr_t pc = 0, fp = 0, sp = 0, lr = 0;
};

Regs regsOf(void* ctx) noexcept {
  Regs r;
  auto* uc = static_cast<ucontext_t*>(ctx);
  (void)uc;
#if defined(__APPLE__) && defined(__x86_64__)
  r.pc = uc->uc_mcontext->__ss.__rip;
  r.fp = uc->uc_mcontext->__ss.__rbp;
  r.sp = uc->uc_mcontext->__ss.__rsp;
#elif defined(__APPLE__) && defined(__aarch64__)
  r.pc = static_cast<uintptr_t>(__darwin_arm_thread_state64_get_pc(uc->uc_mcontext->__ss));
  r.fp = static_cast<uintptr_t>(__darwin_arm_thread_state64_get_fp(uc->uc_mcontext->__ss));
  r.sp = static_cast<uintptr_t>(__darwin_arm_thread_state64_get_sp(uc->uc_mcontext->__ss));
  r.lr = static_cast<uintptr_t>(__darwin_arm_thread_state64_get_lr(uc->uc_mcontext->__ss));
#elif defined(__linux__) && defined(__x86_64__)
  r.pc = static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]);
  r.fp = static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RBP]);
  r.sp = static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RSP]);
#elif defined(__linux__) && defined(__aarch64__)
  r.pc = static_cast<uintptr_t>(uc->uc_mcontext.pc);
  r.fp = static_cast<uintptr_t>(uc->uc_mcontext.regs[29]);
  r.lr = static_cast<uintptr_t>(uc->uc_mcontext.regs[30]);
  r.sp = static_cast<uintptr_t>(uc->uc_mcontext.sp);
#elif defined(__FreeBSD__) && defined(__x86_64__)
  r.pc = static_cast<uintptr_t>(uc->uc_mcontext.mc_rip);
  r.fp = static_cast<uintptr_t>(uc->uc_mcontext.mc_rbp);
  r.sp = static_cast<uintptr_t>(uc->uc_mcontext.mc_rsp);
#elif defined(__FreeBSD__) && defined(__aarch64__)
  r.pc = static_cast<uintptr_t>(uc->uc_mcontext.mc_gpregs.gp_elr);
  r.fp = static_cast<uintptr_t>(uc->uc_mcontext.mc_gpregs.gp_x[29]);
  r.lr = static_cast<uintptr_t>(uc->uc_mcontext.mc_gpregs.gp_lr);
  r.sp = static_cast<uintptr_t>(uc->uc_mcontext.mc_gpregs.gp_sp);
#elif defined(__NetBSD__) && defined(__x86_64__)
  r.pc = static_cast<uintptr_t>(uc->uc_mcontext.__gregs[_REG_RIP]);
  r.fp = static_cast<uintptr_t>(uc->uc_mcontext.__gregs[_REG_RBP]);
  r.sp = static_cast<uintptr_t>(uc->uc_mcontext.__gregs[_REG_RSP]);
#elif defined(__OpenBSD__) && defined(__x86_64__)
  r.pc = static_cast<uintptr_t>(uc->sc_rip);
  r.fp = static_cast<uintptr_t>(uc->sc_rbp);
  r.sp = static_cast<uintptr_t>(uc->sc_rsp);
#else
  r.fp = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
  r.sp = r.fp;
#endif
  return r;
}

// ---- walking: the frame chain, then a scan of the stack where the chain is short ----
void walk(const Regs& r, ReportInfo& info) noexcept {
  int n = 0;
  auto add = [&](uintptr_t pc, bool scanned) {
    if (n < kMaxFrames) {
      info.pcs[n] = pc;
      info.scanned[n] = scanned;
      ++n;
    }
  };
  if (r.pc) add(stripPointer(r.pc), false);
  if (r.lr && findModule(stripPointer(r.lr))) add(stripPointer(r.lr), false);
  uintptr_t fp = r.fp;
  bool complete = false;  // the chain ran to its proper end
  while (n < kMaxFrames && fp && (fp & (sizeof(uintptr_t) - 1)) == 0) {
    uintptr_t pair[2];
    if (!safeRead(fp, pair, sizeof pair)) break;
    const uintptr_t next = pair[0], ret = stripPointer(pair[1]);
    if (ret == 0) {
      complete = true;
      break;
    }
    if (!(n >= 2 && info.pcs[n - 1] == ret)) add(ret, false);
    if (next == 0) complete = true;
    if (next <= fp || next - fp > 8u * 1024 * 1024) break;
    fp = next;
  }
  if (!complete && n < 16 && r.sp) {
    // No frame pointers in that code (system libraries on Linux): return addresses on the stack.
    uintptr_t words[512];
    uintptr_t at = r.sp & ~(sizeof(uintptr_t) - 1);
    for (int page = 0; page < 16 && n < 32; ++page, at += sizeof words) {
      if (!safeRead(at, words, sizeof words)) break;
      for (size_t i = 0; i < sizeof words / sizeof *words && n < 32; ++i) {
        const uintptr_t v = stripPointer(words[i]);
        const Module* m = findModule(v);
        if (!m || v - m->base < 4096) continue;  // (values that merely look like the image's own header)
        bool dup = false;
        for (int k = 0; k < n && !dup; ++k) dup = info.pcs[k] == v;
        if (!dup) add(v, true);
      }
    }
  }
  info.frames = n;
}

const char* signalName(int sig) noexcept {
  switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL";
    case SIGFPE: return "SIGFPE";
    case SIGABRT: return "SIGABRT";
    case SIGTRAP: return "SIGTRAP";
    case SIGSYS: return "SIGSYS";
    default: return "signal";
  }
}

void describeThread(char* out, size_t cap) noexcept {
  const char* role = plat::threadRole();
  char name[32] = "";
#if defined(__APPLE__) || defined(__linux__)
  pthread_getname_np(pthread_self(), name, sizeof name);
#endif
  size_t n = 0;
  for (; *role && n + 1 < cap; ++role) out[n++] = *role;
  if (name[0] && n + 4 + std::strlen(name) < cap) {
    out[n++] = ' ';
    out[n++] = '(';
    for (const char* p = name; *p; ++p) out[n++] = *p;
    out[n++] = ')';
  }
  out[n] = 0;
}

// ---- a fresh recovery copy, in a child that is a copy of the memory ----
void childFault(int) { _exit(97); }

void rescueInChild() noexcept {
  for (int s : kFatal) ::signal(s, childFault);
  ::signal(SIGALRM, SIG_DFL);
  sigset_t none;
  sigemptyset(&none);
  sigprocmask(SIG_SETMASK, &none, nullptr);
  alarm(8);
  char tmp[1200], final[1200], mark[1200];
  rescuePaths(tmp, sizeof tmp, final, sizeof final, mark, sizeof mark);
  RescueFn fn = rescueFn();
  if (fn && fn(tmp) && ::rename(tmp, final) == 0) {
    const int fd = ::open(mark, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd >= 0) {
      char num[24];
      int k = 0;
      char rev[24];
      uint64_t v = static_cast<uint64_t>(plat::unixTime());
      do {
        rev[k++] = static_cast<char>('0' + v % 10);
        v /= 10;
      } while (v);
      int m = 0;
      while (k) num[m++] = rev[--k];
      (void)!::write(fd, num, static_cast<size_t>(m));
      ::close(fd);
    }
    _exit(0);
  }
  ::unlink(tmp);
  _exit(98);
}

void rescueFork(bool fromSignal) noexcept {
  SessionBlock* s = session();
  if (!s || !rescueFn() || !s->unsaved.load(std::memory_order_relaxed)) return;
  if (fromSignal) {
    // fork runs the system's fork handlers, which can wait on a lock this crash holds. The report
    // is on disk by now; a stuck fork only costs the fresher copy.
    alarm(12);
  }
  const pid_t pid = ::fork();
  if (pid == 0) rescueInChild();
  if (pid > 0) {
    int st = 0;
    while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {
    }
  }
}

// ---- the fatal handler ----
void onFatal(int sig, siginfo_t* si, void* ctx) {
  const pthread_t self = pthread_self();
  int expected = 0;
  if (!gInFatal.compare_exchange_strong(expected, 1)) {
    // A fault while reporting, on this thread: what was written stands. Another thread: wait.
    if (pthread_equal(gFatalThread.load(), self)) _exit(128 + sig);
    for (int i = 0; i < 300; ++i) {
      struct timespec t = {0, 100000000};
      nanosleep(&t, nullptr);
    }
    _exit(128 + sig);
  }
  gFatalThread.store(self);
  struct sigaction dfl;
  std::memset(&dfl, 0, sizeof dfl);
  dfl.sa_handler = SIG_DFL;
  sigaction(SIGALRM, &dfl, nullptr);
  alarm(20);  // never hang here: the sentinel takes over when this process is gone

  const Regs regs = regsOf(ctx);
  ReportInfo& info = gFatalInfo;
  info.kind = "crash";
  info.cause = signalName(sig);
  info.number = sig;
  info.code = si ? si->si_code : 0;
  if (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE) {
    info.address = si ? reinterpret_cast<uint64_t>(si->si_addr) : 0;
    info.hasAddress = true;
    const uint64_t a = info.address, sp = regs.sp;
    if ((sig == SIGSEGV || sig == SIGBUS) && sp && (a > sp ? a - sp : sp - a) < 256 * 1024) info.note = "the address is at the stack pointer: stack overflow";
  }
  const char* term = terminateText();
  if (sig == SIGABRT && term[0]) {
    info.typeName = "uncaught exception";
    info.message = term;
  } else if (sig == SIGABRT) {
    info.note = "abort: a failed check, or memory damage found by the allocator";
  }
  describeThread(gThreadText, sizeof gThreadText);
  info.thread = gThreadText;
  walk(regs, info);

  // Any stale report from a freeze makes way.
  SessionBlock* s = session();
  char name[128];
  if (s && s->frozen.load() && s->reportReady.load()) {
    char old[1200];
    joinPath(old, sizeof old, reportDirC(), s->reportName);
    ::unlink(old);
  }
  const bool ok = writeReportToDisk(info, name, sizeof name);
  if (s && ok) {
    copyText(s->reportName, sizeof s->reportName, name);
    s->reportReady.store(1);
    s->frozen.store(0);
  }
  static const char msg[] = "Gridline crashed; a crash report was written. It has no personal paths; see the pop-up.\n";
  (void)!::write(2, msg, sizeof msg - 1);
  rescueFork(true);
  _exit(128 + sig);  // not raise: the system's own crash dialog stays away, the sentinel takes over
}

void onSample(int, siginfo_t*, void* ctx) {
  if (gSampleState.load() != 1) return;
  const Regs r = regsOf(ctx);
  walk(r, gSample);
  gSampleState.store(2);
}

void onTerminate() {
  char text[440];
  size_t n = 0;
  auto put = [&](const char* t) {
    for (; t && *t && n + 1 < sizeof text; ++t) text[n++] = *t;
  };
  if (std::exception_ptr e = std::current_exception()) {
    try {
      std::rethrow_exception(e);
    } catch (const std::exception& x) {
      put(typeid(x).name());
      put(": ");
      put(x.what());
    } catch (...) {
      put("an exception of an unknown type");
    }
  } else {
    put("std::terminate called with no exception");
  }
  text[n] = 0;
  setTerminateText(text);
  std::abort();
}

// ---- modules ----
#if defined(__APPLE__)
void refreshApple() {
  Module* t = beginModules();
  int n = 0;
  const uint32_t count = _dyld_image_count();
  for (uint32_t i = 0; i < count && n < kMaxModules; ++i) {
    const auto* h = reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(i));
    if (!h || h->magic != MH_MAGIC_64) continue;
    Module& m = t[n];
    const char* name = _dyld_get_image_name(i);
    m.base = reinterpret_cast<uintptr_t>(h);
    m.execStart = m.execEnd = 0;
    m.idLen = 0;
    copyText(m.name, sizeof m.name, baseName(name ? name : "?"));
    const intptr_t slide = _dyld_get_image_vmaddr_slide(i);
    auto cmd = reinterpret_cast<uintptr_t>(h + 1);
    for (uint32_t c = 0; c < h->ncmds; ++c) {
      const auto* lc = reinterpret_cast<const load_command*>(cmd);
      if (lc->cmd == LC_SEGMENT_64) {
        const auto* seg = reinterpret_cast<const segment_command_64*>(lc);
        if (std::strncmp(seg->segname, "__TEXT", 16) == 0) {
          m.execStart = static_cast<uintptr_t>(seg->vmaddr) + static_cast<uintptr_t>(slide);
          m.execEnd = m.execStart + static_cast<uintptr_t>(seg->vmsize);
        }
      } else if (lc->cmd == LC_UUID) {
        const auto* u = reinterpret_cast<const uuid_command*>(lc);
        std::memcpy(m.idBytes, u->uuid, 16);
        m.idLen = 16;
      }
      cmd += lc->cmdsize;
    }
    if (m.execEnd) ++n;
  }
  endModules(n);
}
#else
struct IterState {
  Module* table;
  int n;
};
int onImage(struct dl_phdr_info* info, size_t, void* data) {
  auto* st = static_cast<IterState*>(data);
  if (st->n >= kMaxModules) return 1;
  Module& m = st->table[st->n];
  m.base = static_cast<uintptr_t>(info->dlpi_addr);
  m.execStart = m.execEnd = 0;
  m.idLen = 0;
  const char* name = info->dlpi_name;
  copyText(m.name, sizeof m.name, name && *name ? baseName(name) : gProgramName);
  for (int i = 0; i < info->dlpi_phnum; ++i) {
    const auto& ph = info->dlpi_phdr[i];
    const uintptr_t start = m.base + static_cast<uintptr_t>(ph.p_vaddr);
    if (ph.p_type == PT_LOAD && (ph.p_flags & PF_X)) {
      if (!m.execStart || start < m.execStart) m.execStart = start;
      if (start + ph.p_memsz > m.execEnd) m.execEnd = start + static_cast<uintptr_t>(ph.p_memsz);
    } else if (ph.p_type == PT_NOTE) {
      // Notes: name size, descriptor size, type, then both, each padded to four bytes.
      const char* p = reinterpret_cast<const char*>(start);
      const char* end = p + ph.p_memsz;
      while (p + 12 <= end) {
        uint32_t nsz, dsz, type;
        std::memcpy(&nsz, p, 4);
        std::memcpy(&dsz, p + 4, 4);
        std::memcpy(&type, p + 8, 4);
        const char* nm = p + 12;
        const char* desc = nm + ((nsz + 3u) & ~3u);
        if (type == 3 && nsz == 4 && std::memcmp(nm, "GNU", 4) == 0 && desc + dsz <= end) {  // NT_GNU_BUILD_ID
          m.idLen = dsz < sizeof m.idBytes ? dsz : static_cast<uint32_t>(sizeof m.idBytes);
          std::memcpy(m.idBytes, desc, m.idLen);
          break;
        }
        p = desc + ((dsz + 3u) & ~3u);
      }
    }
  }
  if (m.execEnd) ++st->n;
  return 0;
}
#endif

}  // namespace

namespace plat {

const char* threadRole() noexcept {
  const pthread_t self = pthread_self();
  if (pthread_equal(self, gUiThread)) return "ui";
  if (gAudioSeen.load(std::memory_order_relaxed) && pthread_equal(self, gAudioThread.load(std::memory_order_relaxed))) return "audio";
  return "worker";
}

void noteAudioThread() noexcept {
  gAudioThread.store(pthread_self(), std::memory_order_relaxed);
  gAudioSeen.store(true, std::memory_order_relaxed);
}

void refreshModules() {
#if defined(__APPLE__)
  refreshApple();
#else
  IterState st{beginModules(), 0};
  dl_iterate_phdr(onImage, &st);
  endModules(st.n);
#endif
}

void installHandlers(bool devBacktrace) {
  gUiThread = pthread_self();
  if (::pipe(gProbe) == 0) {
    for (int fd : gProbe) {
      ::fcntl(fd, F_SETFD, FD_CLOEXEC);
      ::fcntl(fd, F_SETFL, O_NONBLOCK);
    }
  }
  gReservedFd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
  // A first call to pthread_getname_np and friends, while nothing is wrong.
  char probe[32];
  describeThread(probe, sizeof probe);

  stack_t st;
  std::memset(&st, 0, sizeof st);
  st.ss_sp = gStack;
  st.ss_size = sizeof gStack;
  sigaltstack(&st, nullptr);  // the UI thread; other threads report on their own stack

  struct sigaction sa;
  std::memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = onFatal;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
  sigemptyset(&sa.sa_mask);
  for (int s : kFatal) sigaction(s, &sa, nullptr);
  if (!devBacktrace) {
    struct sigaction samp;
    std::memset(&samp, 0, sizeof samp);
    samp.sa_sigaction = onSample;
    samp.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
    sigfillset(&samp.sa_mask);
    sigaction(kSampleSignal, &samp, nullptr);
    std::set_terminate(onTerminate);
  }
#if defined(__APPLE__)
  // A plugin's own dialog runs a nested event loop on the UI thread, and Gridline's frames wait for
  // it. Events still being served means nothing is frozen: every turn of the loop counts as a sign
  // of life, whichever loop it is. What is really stuck never comes back to any loop.
  CFRunLoopObserverRef loop = CFRunLoopObserverCreate(
      nullptr, kCFRunLoopBeforeWaiting | kCFRunLoopAfterWaiting, true, 0, [](CFRunLoopObserverRef, CFRunLoopActivity, void*) { uiHeartbeat(); }, nullptr);
  if (loop) CFRunLoopAddObserver(CFRunLoopGetMain(), loop, kCFRunLoopCommonModes);
#endif
}

void sampleUiStack(ReportInfo& out) {
  gSample = ReportInfo{};
  gSampleState.store(1);
  if (pthread_kill(gUiThread, kSampleSignal) == 0) {
    for (int i = 0; i < 40 && gSampleState.load() != 2; ++i) sleepMs(25);
  }
  if (gSampleState.exchange(0) == 2) {
    std::memcpy(out.pcs, gSample.pcs, sizeof out.pcs);
    std::memcpy(out.scanned, gSample.scanned, sizeof out.scanned);
    out.frames = gSample.frames;
  } else {
    out.note = "the thread did not answer the look at its stack";
  }
}

void currentStack(ReportInfo& out) {
  Regs r;
  r.fp = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
  r.sp = r.fp;
  walk(r, out);
}

void currentThreadName(char* out, size_t cap) { describeThread(out, cap); }

void* mapFile(const std::string& path, size_t size, bool create) {
  const int fd = ::open(path.c_str(), create ? (O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC) : (O_RDWR | O_CLOEXEC), 0600);
  if (fd < 0) return nullptr;
  if (create) {
    if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
      ::close(fd);
      return nullptr;
    }
  } else {
    struct stat st;
    if (::fstat(fd, &st) != 0 || static_cast<size_t>(st.st_size) < size) {
      ::close(fd);
      return nullptr;
    }
  }
  void* mem = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  ::close(fd);
  return mem == MAP_FAILED ? nullptr : mem;
}

void unmapFile(void* mem, size_t size) {
  if (mem) ::munmap(mem, size);
}

bool writeFileAtomic(const char* tmp, const char* final, const ReportInfo& info) noexcept {
  int fd = ::open(tmp, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
  if (fd < 0 && errno == EMFILE && gReservedFd >= 0) {  // out of descriptors
    ::close(gReservedFd);
    gReservedFd = -1;
    fd = ::open(tmp, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
  }
  if (fd < 0) return false;
  struct Out {
    int fd;
    bool ok;
  } out{fd, true};
  {
    Writer w(
        [](void* ctx, const char* d, size_t n) {
          auto* o = static_cast<Out*>(ctx);
          while (n > 0) {
            const ssize_t k = ::write(o->fd, d, n);
            if (k < 0 && errno == EINTR) continue;
            if (k <= 0) {
              o->ok = false;
              return;
            }
            d += k;
            n -= static_cast<size_t>(k);
          }
        },
        &out);
    writeReport(info, w);
  }
  ::fsync(fd);
  ::close(fd);
  return ::rename(tmp, final) == 0 && out.ok;
}

int64_t unixTime() noexcept {
  struct timespec t;
  clock_gettime(CLOCK_REALTIME, &t);
  return static_cast<int64_t>(t.tv_sec);
}

void spawnSentinel(const std::string& self, const std::string& sessionFile, const std::string& testDir) {
  int link[2];
  if (::pipe(link) != 0) return;
  ::fcntl(link[1], F_SETFD, FD_CLOEXEC);  // only this process holds the end the sentinel watches
  const std::string fd = std::to_string(link[0]);
  std::vector<std::string> args = {self, "--crash-sentinel", sessionFile, fd};
  if (!testDir.empty()) args.push_back(testDir);
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  argv.push_back(nullptr);
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  // Nothing may hold on to this process's output: scripts wait for its end.
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
  pid_t pid = 0;
  const int rc = posix_spawn(&pid, self.c_str(), &fa, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  ::close(link[0]);
  if (rc != 0) {
    ::close(link[1]);
    return;
  }
  gSentinelLink = link[1];
}

void closeSentinelLink() {
  if (gSentinelLink >= 0) ::close(gSentinelLink);
  gSentinelLink = -1;
}

bool waitParent(const char* link, int pollMs) {
  const int fd = std::atoi(link);
  struct pollfd p;
  p.fd = fd;
  p.events = POLLIN;
  p.revents = 0;
  const int rc = ::poll(&p, 1, pollMs);
  if (rc <= 0) return rc < 0 && errno != EINTR;
  char b[16];
  const ssize_t n = ::read(fd, b, sizeof b);
  return n <= 0;
}

uint32_t parentExitCode(const char*) { return 0; }

void detachSentinel() {
  ::setsid();
  ::signal(SIGINT, SIG_IGN);
  ::signal(SIGHUP, SIG_IGN);
  ::signal(SIGQUIT, SIG_IGN);
  ::signal(SIGPIPE, SIG_IGN);
}

void endProcess(int64_t pid) {
  if (pid > 0) ::kill(static_cast<pid_t>(pid), SIGKILL);
}

bool alive(int64_t pid) {
  if (pid <= 0) return false;
  return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
}

bool spawnDetached(const std::string& exe, const std::vector<std::string>& argsIn) {
  std::vector<std::string> args;
  args.push_back(exe);
  args.insert(args.end(), argsIn.begin(), argsIn.end());
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  argv.push_back(nullptr);
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
  pid_t pid = 0;
  const int rc = posix_spawn(&pid, exe.c_str(), &fa, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  return rc == 0;
}

std::string selfPath() {
  // An AppImage's own mount goes when its first process ends: restart from the image.
  if (const char* img = std::getenv("APPIMAGE"); img && *img) return img;
#if defined(__APPLE__)
  char buf[4096];
  uint32_t size = sizeof buf;
  if (_NSGetExecutablePath(buf, &size) == 0) return buf;
  return {};
#elif defined(__linux__)
  char buf[4096];
  const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n > 0) return std::string(buf, static_cast<size_t>(n));
  return {};
#else
  char buf[4096];
  const ssize_t n = ::readlink("/proc/curproc/file", buf, sizeof buf - 1);
  if (n > 0) return std::string(buf, static_cast<size_t>(n));
  return {};
#endif
}

std::string osDescription() {
  struct utsname u;
  std::string s;
  if (::uname(&u) == 0) s = std::string(u.sysname) + " " + u.release;  // never the machine's name
#if defined(__APPLE__)
  char v[64];
  size_t n = sizeof v;
  if (sysctlbyname("kern.osproductversion", v, &n, nullptr, 0) == 0) s = "macOS " + std::string(v) + " (" + s + ")";
#elif defined(__linux__)
  std::ifstream f("/etc/os-release");
  std::string line;
  while (std::getline(f, line)) {
    if (line.rfind("PRETTY_NAME=", 0) == 0) {
      std::string name = line.substr(12);
      if (name.size() >= 2 && name.front() == '"') name = name.substr(1, name.size() - 2);
      s = name + " (" + s + ")";
      break;
    }
  }
#endif
  return s;
}

const char* archName() noexcept {
#if defined(__x86_64__)
  return "x86_64";
#elif defined(__aarch64__)
  return "arm64";
#else
  return "other";
#endif
}

int64_t currentPid() noexcept { return static_cast<int64_t>(::getpid()); }

void rescueNow() { rescueFork(false); }

void sleepMs(int ms) {
  struct timespec t = {ms / 1000, static_cast<long>(ms % 1000) * 1000000L}, left;
  while (nanosleep(&t, &left) != 0 && errno == EINTR) t = left;  // (the watchdog's look at a stack interrupts it)
}

namespace {
volatile int gZero = 0;
volatile int gSink = 0;
int deep(int depth) {
  volatile char pad[4096];
  pad[0] = static_cast<char>(depth);
  if (gZero == 0) gSink = deep(depth + 1) + pad[0];
  return gSink;
}
void segv() { *reinterpret_cast<volatile int*>(static_cast<uintptr_t>(gZero + 8)) = 1; }
}  // namespace

void forceCrash(const std::string& kind) {
  if (kind == "segv") segv();
  if (kind == "bus") {
    // A page of a file with no bytes behind it.
    char path[1200];
    joinPath(path, sizeof path, recoveryDirC(), ".bus-test");
    const int fd = ::open(path, O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0600);
    ::unlink(path);
    void* m = ::mmap(nullptr, 4096, PROT_READ, MAP_SHARED, fd, 0);
    gSink = *static_cast<volatile int*>(m);
  }
  if (kind == "ill") __builtin_trap();
  if (kind == "fpe") ::raise(SIGFPE);
  if (kind == "abort") std::abort();
  if (kind == "terminate") std::thread([] { throw std::runtime_error("forced uncaught failure in /Users/someone/Secret Songs/x.flp: test"); }).join();
  if (kind == "overflow") gSink = deep(0);
  if (kind == "heap") {
    char* volatile p = static_cast<char*>(std::malloc(64));
    std::free(p);
    std::free(p);
    std::abort();  // the allocator did not notice: the abort stands for it
  }
  if (kind == "thread") std::thread([] { segv(); }).join();
  if (kind == "freeze") {
    for (;;) sleepMs(1000);
  }
  if (kind.rfind("stall:", 0) == 0) sleepMs(std::atoi(kind.c_str() + 6) * 1000);
}

}  // namespace plat

}  // namespace gl::crash::detail
#endif  // !_WIN32
