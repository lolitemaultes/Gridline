#include "NetBlock.h"
#include <cerrno>
#include <cstdlib>
#include <cstring>

#if defined(__APPLE__)
#include <sandbox.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cstddef>
#include <cstdint>
#include <vector>
#endif

namespace gl::host {

namespace {
bool gRequested = false;
int gGuard = kNetOpen;
std::string gWhy;

bool forcedFailure() {
  const char* e = std::getenv("GRIDLINE_DEV_NET_BLOCK_FAIL");
  return e && *e && *e != '0';
}

#if defined(__linux__)
bool writeText(const char* path, const std::string& s) {
  const int fd = ::open(path, O_WRONLY | O_CLOEXEC);
  if (fd < 0) return false;
  const bool ok = ::write(fd, s.data(), s.size()) == static_cast<ssize_t>(s.size());
  ::close(fd);
  return ok;
}

// A user namespace (no rights needed where the system allows them) holding a network namespace of
// its own: only a loopback device that is down, no route anywhere. The same user and group inside
// it, so files keep their owner.
bool enterNetworkNamespace(std::string& why) {
  const uid_t uid = ::getuid();
  const gid_t gid = ::getgid();
  if (::unshare(CLONE_NEWUSER | CLONE_NEWNET) != 0) {
    why = std::string("no private network space: ") + std::strerror(errno);
    return false;
  }
  writeText("/proc/self/setgroups", "deny");
  writeText("/proc/self/uid_map", std::to_string(uid) + " " + std::to_string(uid) + " 1\n");
  writeText("/proc/self/gid_map", std::to_string(gid) + " " + std::to_string(gid) + " 1\n");
  return true;
}

sock_filter op(uint16_t code, uint32_t k) {
  sock_filter f{};
  f.code = code;
  f.k = k;
  return f;
}
sock_filter jump(uint16_t code, uint32_t k, uint8_t yes, uint8_t no) {
  sock_filter f = op(code, k);
  f.jt = yes;
  f.jf = no;
  return f;
}

// The filter: socket() for AF_INET / AF_INET6 and io_uring (which opens sockets without socket())
// fail with EPERM; on x86-64 also the x32 calls and 32-bit programs' socket calls (socketcall
// refused whole, socket() by family). Everything else is allowed.
bool installSocketFilter(std::string& why) {
#if defined(__x86_64__)
  constexpr uint32_t kArch = AUDIT_ARCH_X86_64;
  constexpr uint32_t kForeign = AUDIT_ARCH_I386;
  constexpr uint32_t kForeignSocket = 359, kForeignSocketcall = 102;
  constexpr uint32_t kX32 = 0x40000000u;  // x32 calls carry this bit
#elif defined(__aarch64__)
  constexpr uint32_t kArch = AUDIT_ARCH_AARCH64;
  constexpr uint32_t kForeign = AUDIT_ARCH_ARM;
  constexpr uint32_t kForeignSocket = 281, kForeignSocketcall = 0xFFFFFFFFu;  // 32-bit ARM has no socketcall
  constexpr uint32_t kX32 = 0xFFFFFFFFu;                                         // (no such calls)
#else
  why = "no system call filter for this processor";
  return false;
#endif
#if defined(__x86_64__) || defined(__aarch64__)
  constexpr uint32_t kIoUringSetup = 425;  // the same number on every architecture
  constexpr uint32_t kDeny = SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA);
  constexpr uint32_t kNr = offsetof(seccomp_data, nr), kArchAt = offsetof(seccomp_data, arch);
  constexpr uint32_t kArg0 = offsetof(seccomp_data, args);  // low half of the first argument (little endian)
  const uint16_t ld = BPF_LD | BPF_W | BPF_ABS, jeq = BPF_JMP | BPF_JEQ | BPF_K, jge = BPF_JMP | BPF_JGE | BPF_K, ret = BPF_RET | BPF_K;
  // Jumps count the instructions skipped: 13 refuses, 14 allows.
  std::vector<sock_filter> f = {
      /* 0 */ op(ld, kArchAt),
      /* 1 */ jump(jeq, kArch, 4, 0),                // native: 6
      /* 2 */ jump(jeq, kForeign, 0, 11),            // neither: allow
      /* 3 */ op(ld, kNr),
      /* 4 */ jump(jeq, kForeignSocketcall, 8, 0),   // refuse
      /* 5 */ jump(jeq, kForeignSocket, 4, 8),       // the family (10), else allow
      /* 6 */ op(ld, kNr),
      /* 7 */ jump(jeq, static_cast<uint32_t>(__NR_socket), 2, 0),  // the family (10)
      /* 8 */ jump(jeq, kIoUringSetup, 4, 0),                       // refuse
      /* 9 */ jump(jge, kX32, 3, 4),                                // x86-64: x32 calls refused; else allow
      /* 10 */ op(ld, kArg0),
      /* 11 */ jump(jeq, AF_INET, 1, 0),
      /* 12 */ jump(jeq, AF_INET6, 0, 1),
      /* 13 */ op(ret, kDeny),
      /* 14 */ op(ret, SECCOMP_RET_ALLOW),
  };
  sock_fprog prog{};
  prog.len = static_cast<unsigned short>(f.size());
  prog.filter = f.data();
  if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
    why = std::string("no system call filter: ") + std::strerror(errno);
    return false;
  }
#if defined(SECCOMP_SET_MODE_FILTER) && defined(SECCOMP_FILTER_FLAG_TSYNC) && defined(__NR_seccomp)
  // Every thread of the process (there should be one), else this thread and the ones it starts.
  if (::syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_TSYNC, &prog) == 0) return true;
#endif
  if (::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog, 0, 0) != 0) {
    why = std::string("no system call filter: ") + std::strerror(errno);
    return false;
  }
  return true;
#endif
}
#endif
}  // namespace

bool networkBlockAvailable() {
#if defined(__APPLE__) || defined(__linux__)
  return true;
#else
  return false;
#endif
}

int applyNetworkBlock(std::string* why) {
  std::string reason;
  int guard = kNetOpen;
  if (forcedFailure()) {
    reason = "blocking was made to fail for a test";
  } else {
#if defined(__APPLE__)
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
    // Public, documented and still in the system; marked deprecated since 10.8 in favour of the App
    // Store's App Sandbox, which an ordinary signed program cannot apply to one child process.
    char* err = nullptr;
    if (sandbox_init(kSBXProfileNoInternet, SANDBOX_NAMED, &err) == 0) {
      guard = kNetSeatbelt;
    } else {
      reason = std::string("the system sandbox refused: ") + (err ? err : "unknown error");
    }
    if (err) sandbox_free_error(err);
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#elif defined(__linux__)
    std::string nsWhy, filterWhy;
    if (enterNetworkNamespace(nsWhy)) guard = kNetNamespace;
    else if (installSocketFilter(filterWhy)) guard = kNetSeccomp;
    else reason = nsWhy + "; " + filterWhy;
#else
    reason = "this system offers no way to block one program's internet access without administrator rights";
#endif
  }
  if (why) *why = reason;
  return guard;
}

void noteNetworkBlock(bool requested, int guard, const std::string& why) {
  gRequested = requested;
  gGuard = guard;
  gWhy = why;
}
bool networkBlockRequested() { return gRequested; }
int networkGuard() { return gGuard; }
std::string networkBlockError() { return gWhy; }

}  // namespace gl::host
