#include "Platform.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <clocale>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>
#if defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#if defined(_WIN32)
#include <windows.h>
#include <process.h>
#include <psapi.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif
#if defined(__APPLE__)
#include <libproc.h>
#include <mach/mach.h>
#include <sys/sysctl.h>
#include <pthread.h>
#include <mach-o/dyld.h>
#endif
#if defined(__FreeBSD__) || defined(__DragonFly__) || defined(__NetBSD__) || defined(__OpenBSD__)
#include <sys/types.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#endif
#if defined(__FreeBSD__)
#include <sys/user.h>
#endif
#if defined(__linux__)
#include <fstream>
#include <sstream>
#endif

#if defined(__linux__) && !defined(__GLIBC__)
#include <pthread.h>
#endif

namespace fs = std::filesystem;

#if defined(__linux__) && !defined(__GLIBC__)
namespace {
// musl gives new threads 128 KB of stack (glibc: 8 MB, from the stack limit). Audio rendering,
// plugin and decoder threads need more: every thread of the process gets 8 MB.
[[maybe_unused]] const bool kMuslThreadStacks = [] {
  pthread_attr_t a;
  if (pthread_attr_init(&a) != 0) return false;
  pthread_attr_setstacksize(&a, 8u << 20);
  const bool ok = pthread_setattr_default_np(&a) == 0;
  pthread_attr_destroy(&a);
  return ok;
}();
}  // namespace
#endif

namespace gl::platform {

#if defined(_WIN32)
namespace {
std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}
std::string narrow(const wchar_t* w) {
  if (!w || !*w) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}
std::string envUtf8(const wchar_t* name) {
  DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
  if (n == 0) return {};
  std::wstring w(n, L'\0');
  n = GetEnvironmentVariableW(name, w.data(), n);
  w.resize(n);
  return narrow(w.c_str());
}
}  // namespace
#endif

namespace {
std::string env(const char* name) {
#if defined(_WIN32)
  return envUtf8(widen(name).c_str());
#else
  const char* v = std::getenv(name);
  return v ? std::string(v) : std::string();
#endif
}
}  // namespace

const char* homeEnv() {
  static const std::string home = [] {
    std::string h = env("HOME");
#if defined(_WIN32)
    if (h.empty()) h = env("USERPROFILE");
    if (h.empty()) {
      std::string d = env("HOMEDRIVE"), p = env("HOMEPATH");
      if (!d.empty() && !p.empty()) h = d + p;
    }
    for (auto& c : h)
      if (c == '\\') c = '/';
#endif
    return h;
  }();
  return home.empty() ? nullptr : home.c_str();
}

std::string homeDir() {
  const char* h = homeEnv();
  return h ? std::string(h) : std::string(".");
}

std::string configRoot() {
  if (std::string g = env("GRIDLINE_CONFIG_HOME"); !g.empty()) return g;
  if (std::string x = env("XDG_CONFIG_HOME"); !x.empty()) return x;
#if defined(_WIN32)
  if (std::string a = env("APPDATA"); !a.empty()) return a;
#endif
  return (fs::path(homeDir()) / ".config").string();
}

std::string cacheRoot() {
  if (std::string x = env("XDG_CACHE_HOME"); !x.empty()) return x;
#if defined(_WIN32)
  if (std::string a = env("LOCALAPPDATA"); !a.empty()) return a;
#endif
  return (fs::path(homeDir()) / ".cache").string();
}

std::string dataRoot() {
  if (std::string x = env("XDG_DATA_HOME"); !x.empty()) return x;
#if defined(_WIN32)
  if (std::string a = env("APPDATA"); !a.empty()) return a;
#endif
  return (fs::path(homeDir()) / ".local" / "share").string();
}

void quietCrashDialogs() {
#if defined(_WIN32)
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  // K-73: no "stopped working" window either when a plugin in a scanner child fails fast (stack
  // cookie, invalid parameter): Windows Error Reporting shows it whatever the error mode, and the
  // scan would wait for the timeout. WerSetFlags(WER_FAULT_REPORTING_NO_UI), looked up by name.
  if (HMODULE k32 = GetModuleHandleW(L"kernel32.dll")) {
    if (FARPROC p = GetProcAddress(k32, "WerSetFlags")) {
      using WerSetFlagsFn = HRESULT(WINAPI*)(DWORD);
      WerSetFlagsFn setFlags = nullptr;
      static_assert(sizeof setFlags == sizeof p, "function pointer size");
      std::memcpy(&setFlags, &p, sizeof setFlags);
      setFlags(0x00000020);  // WER_FAULT_REPORTING_NO_UI
    }
  }
#endif
}

int processId() {
#if defined(_WIN32)
  return static_cast<int>(GetCurrentProcessId());
#else
  return static_cast<int>(::getpid());
#endif
}

void lowerThisThreadPriority() {
#if defined(__APPLE__)
  pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#elif defined(_WIN32)
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#elif defined(__linux__)
  setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 10);  // (per thread on Linux)
#endif
}

void* zeroedPages(std::size_t bytes) {
#if defined(_WIN32)
  return VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
  void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  return p == MAP_FAILED ? nullptr : p;
#endif
}

void releasePages(void* p, std::size_t bytes) {
  if (!p) return;
#if defined(_WIN32)
  (void)bytes;
  VirtualFree(p, 0, MEM_RELEASE);
#else
  ::munmap(p, bytes);
#endif
}

FileLock& FileLock::operator=(FileLock&& o) noexcept {
  if (this != &o) {
    unlock();
    handle_ = o.handle_;
    o.handle_ = kNone;
  }
  return *this;
}

bool FileLock::tryLock(const std::string& path, Mode mode) {
  unlock();
#if defined(_WIN32)
  // (not inherited by child processes; the lock is on one byte, per handle)
  HANDLE h = CreateFileW(widen(path).c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  OVERLAPPED at{};
  const DWORD flags = LOCKFILE_FAIL_IMMEDIATELY | (mode == Mode::Exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0);
  if (!LockFileEx(h, flags, 0, 1, 0, &at)) {
    CloseHandle(h);
    return false;
  }
  handle_ = reinterpret_cast<std::intptr_t>(h);
  return true;
#else
  // flock, not fcntl: it belongs to this open file, so two holders in one process still exclude
  // each other and closing an unrelated descriptor of the same file does not drop it. O_CLOEXEC: a
  // child process that outlives a crash does not keep it.
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (fd < 0) return false;
  if (::flock(fd, (mode == Mode::Exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB) != 0) {
    ::close(fd);
    return false;
  }
  handle_ = fd;
  return true;
#endif
}

void FileLock::unlock() {
  if (handle_ == kNone) return;
#if defined(_WIN32)
  CloseHandle(reinterpret_cast<HANDLE>(handle_));
#else
  ::close(static_cast<int>(handle_));
#endif
  handle_ = kNone;
}

bool setEnv(const char* name, const char* value, bool overwrite) {
#if defined(_WIN32)
  if (!overwrite && !env(name).empty()) return true;
  const std::wstring n = widen(name), v = widen(value ? value : "");
  // Both the Win32 block (child processes) and the C runtime's copy (getenv).
  SetEnvironmentVariableW(n.c_str(), v.c_str());
  return _wputenv_s(n.c_str(), v.c_str()) == 0;
#else
  return ::setenv(name, value ? value : "", overwrite ? 1 : 0) == 0;
#endif
}

bool unsetEnv(const char* name) {
#if defined(_WIN32)
  const std::wstring n = widen(name);
  SetEnvironmentVariableW(n.c_str(), nullptr);
  return _wputenv_s(n.c_str(), L"") == 0;
#else
  return ::unsetenv(name) == 0;
#endif
}

std::string executablePath() {
#if defined(_WIN32)
  std::wstring buf(MAX_PATH, L'\0');
  for (;;) {
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0) return {};
    if (n < buf.size()) {
      buf.resize(n);
      return narrow(buf.c_str());
    }
    buf.resize(buf.size() * 2);
  }
#elif defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string buf(size, '\0');
  if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
  buf.resize(std::strlen(buf.c_str()));
  std::error_code ec;
  auto p = fs::weakly_canonical(buf, ec);
  return ec ? buf : p.string();
#elif defined(__FreeBSD__) || defined(__DragonFly__)
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
  char buf[4096];
  size_t len = sizeof buf;
  if (sysctl(mib, 4, buf, &len, nullptr, 0) == 0) return std::string(buf);
  return {};
#elif defined(__NetBSD__)
  int mib[4] = {CTL_KERN, KERN_PROC_ARGS, -1, KERN_PROC_PATHNAME};
  char buf[4096];
  size_t len = sizeof buf;
  if (sysctl(mib, 4, buf, &len, nullptr, 0) == 0) return std::string(buf);
  std::error_code ec;
  auto p = fs::read_symlink("/proc/curproc/exe", ec);
  return ec ? std::string() : p.string();
#elif defined(__OpenBSD__)
  // OpenBSD has no API for the executable's path: argv[0] of this process, made absolute
  // (relative to the starting folder) or looked up in PATH.
  int mib[4] = {CTL_KERN, KERN_PROC_ARGS, static_cast<int>(::getpid()), KERN_PROC_ARGV};
  size_t len = 0;
  if (sysctl(mib, 4, nullptr, &len, nullptr, 0) != 0 || len == 0) return {};
  std::vector<char> buf(len);
  if (sysctl(mib, 4, buf.data(), &len, nullptr, 0) != 0) return {};
  char** argv = reinterpret_cast<char**>(buf.data());
  if (!argv[0]) return {};
  const std::string a0 = argv[0];
  std::error_code ec;
  if (a0.find('/') != std::string::npos) {
    auto p = fs::weakly_canonical(fs::absolute(a0, ec), ec);
    return ec ? std::string() : p.string();
  }
  const std::string path = env("PATH");
  size_t pos = 0;
  while (pos <= path.size()) {
    const size_t c = path.find(':', pos);
    const std::string dir = path.substr(pos, c == std::string::npos ? std::string::npos : c - pos);
    const fs::path cand = fs::path(dir.empty() ? "." : dir) / a0;
    if (::access(cand.c_str(), X_OK) == 0) return fs::weakly_canonical(cand, ec).string();
    if (c == std::string::npos) break;
    pos = c + 1;
  }
  return {};
#else
  std::error_code ec;
  auto p = fs::read_symlink("/proc/self/exe", ec);
  return ec ? std::string() : p.string();
#endif
}

bool writePrivateFile(const std::string& path, const std::string& data) {
#if defined(_WIN32)
  FILE* f = _wfopen(widen(path).c_str(), L"wb");
  if (!f) return false;
  const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
  return std::fclose(f) == 0 && ok;
#else
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) return false;
  ::fchmod(fd, 0600);
  size_t done = 0;
  while (done < data.size()) {
    const auto n = ::write(fd, data.data() + done, data.size() - done);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    done += static_cast<size_t>(n);
  }
  return ::close(fd) == 0 && done == data.size();
#endif
}

int64_t fileTimeSeconds(std::filesystem::file_time_type t) {
  using namespace std::chrono;
  // The two epochs are a whole number of seconds apart, so the offset is read from the two clocks
  // and rounded to whole seconds: the time between the two now() reads (nanoseconds, or more when
  // the thread is preempted) never reaches the result. Adding that read gap to `t` itself, as this
  // did before, moved a file stamped near a second boundary into the next or previous second now and
  // then, and the plugin scan took such a plugin as changed and scanned it again.
  using FileClock = std::filesystem::file_time_type::clock;
  const double fileNow = duration<double>(FileClock::now().time_since_epoch()).count();
  const double sysNow = duration<double>(system_clock::now().time_since_epoch()).count();
  const auto offset = static_cast<int64_t>(std::llround(sysNow - fileNow));
  return static_cast<int64_t>(floor<seconds>(t.time_since_epoch()).count()) + offset;
}

bool writable(const std::string& path) {
#if defined(_WIN32)
  const DWORD a = GetFileAttributesW(widen(path).c_str());
  if (a == INVALID_FILE_ATTRIBUTES) {
    errno = ENOENT;
    return false;
  }
  if ((a & FILE_ATTRIBUTE_READONLY) && !(a & FILE_ATTRIBUTE_DIRECTORY)) {
    errno = EACCES;
    return false;
  }
  return true;
#else
  return ::access(path.c_str(), W_OK) == 0;
#endif
}

bool replaceFile(const std::string& from, const std::string& to, std::string* err) {
#if defined(_WIN32)
  // Another writer replacing the same file, a virus scanner or the search indexer may hold it
  // for a moment: retry briefly before giving up.
  DWORD code = 0;
  for (int attempt = 0; attempt < 100; ++attempt) {
    if (MoveFileExW(widen(from).c_str(), widen(to).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    code = GetLastError();
    if (code != ERROR_ACCESS_DENIED && code != ERROR_SHARING_VIOLATION && code != ERROR_LOCK_VIOLATION) break;
    Sleep(5);
  }
  errno = code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION ? EACCES
          : code == ERROR_DISK_FULL || code == ERROR_HANDLE_DISK_FULL    ? ENOSPC
          : code == ERROR_PATH_NOT_FOUND || code == ERROR_FILE_NOT_FOUND ? ENOENT
                                                                         : EIO;
  if (err) *err = "cannot replace " + to + " (error " + std::to_string(static_cast<unsigned long>(code)) + ")";
  return false;
#else
  if (std::rename(from.c_str(), to.c_str()) == 0) return true;
  if (err) *err = "cannot replace " + to + ": " + std::strerror(errno);
  return false;
#endif
}

double cpuSeconds() {
#if defined(_WIN32)
  FILETIME c, e, k, u;
  if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
  auto sec = [](const FILETIME& t) {
    return static_cast<double>((static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime) * 1e-7;
  };
  return sec(k) + sec(u);
#else
  rusage ru{};
  getrusage(RUSAGE_SELF, &ru);
  return static_cast<double>(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) +
         static_cast<double>(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) * 1e-6;
#endif
}

double peakRssMb() {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS pmc{};
  pmc.cb = sizeof pmc;
  if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) return 0;
  return static_cast<double>(pmc.PeakWorkingSetSize) / (1024.0 * 1024.0);
#else
  rusage ru{};
  getrusage(RUSAGE_SELF, &ru);
#if defined(__APPLE__)
  return static_cast<double>(ru.ru_maxrss) / (1024.0 * 1024.0);  // bytes
#else
  return static_cast<double>(ru.ru_maxrss) / 1024.0;  // kilobytes
#endif
#endif
}

std::uint64_t physicalMemoryBytes() {
#if defined(_WIN32)
  MEMORYSTATUSEX ms{};
  ms.dwLength = sizeof ms;
  return GlobalMemoryStatusEx(&ms) ? static_cast<std::uint64_t>(ms.ullTotalPhys) : 0;
#else
  const long pages = ::sysconf(_SC_PHYS_PAGES), size = ::sysconf(_SC_PAGESIZE);
  return pages > 0 && size > 0 ? static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(size) : 0;
#endif
}

std::uint64_t availableMemoryBytes() {
#if defined(_WIN32)
  MEMORYSTATUSEX ms{};
  ms.dwLength = sizeof ms;
  return GlobalMemoryStatusEx(&ms) ? static_cast<std::uint64_t>(ms.ullAvailPhys) : 0;
#elif defined(__linux__)
  std::ifstream f("/proc/meminfo");
  std::string key;
  std::uint64_t kb = 0;
  std::string unit;
  while (f >> key >> kb) {
    std::getline(f, unit);
    if (key == "MemAvailable:") return kb * 1024u;
  }
  return 0;
#elif defined(__APPLE__)
  vm_statistics64_data_t vs{};
  mach_msg_type_number_t n = HOST_VM_INFO64_COUNT;
  if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vs), &n) != KERN_SUCCESS) return 0;
  const std::uint64_t page = static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
  return (static_cast<std::uint64_t>(vs.free_count) + vs.inactive_count + vs.purgeable_count) * page;
#elif defined(__FreeBSD__)
  unsigned freePages = 0, inactivePages = 0;
  size_t len = sizeof freePages;
  if (sysctlbyname("vm.stats.vm.v_free_count", &freePages, &len, nullptr, 0) != 0) return 0;
  len = sizeof inactivePages;
  if (sysctlbyname("vm.stats.vm.v_inactive_count", &inactivePages, &len, nullptr, 0) != 0) inactivePages = 0;
  return (static_cast<std::uint64_t>(freePages) + inactivePages) * static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
#else
  return 0;
#endif
}

// ---------------------------------------------------------------- numbers

double parseDouble(const char* s, char** end) {
  const char dp = *std::localeconv()->decimal_point;
  if (dp == '.' || dp == '\0') return std::strtod(s, end);
  // Copy the number's characters, using the locale's separator in place of '.'.
  char buf[128];
  size_t n = 0;
  const char* p = s;
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
  const size_t lead = static_cast<size_t>(p - s);
  while (n + 1 < sizeof buf && p[n] &&
         (std::strchr("0123456789+-eExXpPaAbBcCdDfFiInNtTyY", p[n]) != nullptr || p[n] == '.')) {
    buf[n] = p[n] == '.' ? dp : p[n];
    ++n;
  }
  buf[n] = '\0';
  char* e = nullptr;
  double v = std::strtod(buf, &e);
  if (end) *end = const_cast<char*>(e == buf ? s : s + lead + static_cast<size_t>(e - buf));
  return v;
}

std::string formatDouble(const char* fmt, double v) {
  char buf[64];
  int n = std::snprintf(buf, sizeof buf, fmt, v);
  if (n < 0) return {};
  std::string s(buf, static_cast<size_t>(std::min<int>(n, static_cast<int>(sizeof buf) - 1)));
  const char dp = *std::localeconv()->decimal_point;
  if (dp != '.' && dp != '\0')
    for (auto& c : s)
      if (c == dp) c = '.';
  return s;
}

std::string shellQuote(const std::string& word) {
#if defined(_WIN32)
  std::string q = "\"";
  for (char c : word) q += c == '"' ? std::string("\\\"") : std::string(1, c);
  return q + "\"";
#else
  std::string q = "'";
  for (char c : word) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
  return q + "'";
#endif
}

int runShell(const std::string& command) {
#if defined(_WIN32)
  // cmd /c drops the first and the last quote of a line holding several: wrap it once more.
  return std::system(("\"" + command + "\"").c_str());
#else
  return std::system(command.c_str());
#endif
}

const char* nullDevice() {
#if defined(_WIN32)
  return "NUL";
#else
  return "/dev/null";
#endif
}

// ---------------------------------------------------------------- processes

std::string ExitStatus::describe() const {
  if (!exited) return "still running";
#if defined(_WIN32)
  if (crashed) {
    char b[32];
    std::snprintf(b, sizeof b, "0x%08X", static_cast<unsigned>(code));
    return std::string("crashed (exception ") + b + ")";
  }
#else
  if (crashed) return "crashed (signal " + std::to_string(code) + ")";
#endif
  return "exited (code " + std::to_string(code) + ")";
}

Process::Process(Process&& o) noexcept { *this = std::move(o); }

Process& Process::operator=(Process&& o) noexcept {
  if (this != &o) {
    started_ = o.started_;
    pid_ = o.pid_;
    handle_ = o.handle_;
    job_ = o.job_;
    group_ = o.group_;
    memoryLimit_ = o.memoryLimit_;
    outFile_ = std::move(o.outFile_);
    errFile_ = std::move(o.errFile_);
    status_ = o.status_;
    o.started_ = false;
    o.pid_ = -1;
    o.handle_ = 0;
    o.job_ = 0;
  }
  return *this;
}

Process::~Process() {
#if defined(_WIN32)
  if (handle_) CloseHandle(reinterpret_cast<HANDLE>(handle_));
  if (job_) CloseHandle(reinterpret_cast<HANDLE>(job_));
#endif
}

#if defined(_WIN32)
namespace {
// Command-line quoting for CommandLineToArgvW / the C runtime's parser.
void appendQuoted(std::wstring& cmd, const std::wstring& a) {
  if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
    cmd += a;
    return;
  }
  cmd += L'"';
  for (size_t i = 0;; ++i) {
    size_t bs = 0;
    while (i < a.size() && a[i] == L'\\') ++i, ++bs;
    if (i == a.size()) {
      cmd.append(bs * 2, L'\\');
      break;
    }
    if (a[i] == L'"') {
      cmd.append(bs * 2 + 1, L'\\');
      cmd += L'"';
    } else {
      cmd.append(bs, L'\\');
      cmd += a[i];
    }
  }
  cmd += L'"';
}
}  // namespace

bool Process::start(const std::string& exe, const std::vector<std::string>& args, unsigned flags,
                    const std::vector<std::intptr_t>& inherit) {
  std::wstring cmd;
  for (size_t i = 0; i < args.size(); ++i) {
    if (i) cmd += L' ';
    appendQuoted(cmd, widen(args[i]));
  }
  SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
  auto stdHandle = [&](DWORD which, unsigned flag) -> HANDLE {
    if (flags & flag) {
      HANDLE h = GetStdHandle(which);
      if (h && h != INVALID_HANDLE_VALUE) {
        HANDLE dup = nullptr;
        if (DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &dup, 0, TRUE, DUPLICATE_SAME_ACCESS)) return dup;
      }
    }
    HANDLE dup = nullptr;
    DuplicateHandle(GetCurrentProcess(), nul, GetCurrentProcess(), &dup, 0, TRUE, DUPLICATE_SAME_ACCESS);
    return dup;
  };
  // J-192: stdout / stderr into files the parent can read while the child writes.
  auto toFile = [&](const std::string& path, HANDLE fallback) -> HANDLE {
    if (path.empty()) return fallback;
    HANDLE f = CreateFileW(widen(path).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return fallback;
    CloseHandle(fallback);
    return f;
  };
  HANDLE hin = stdHandle(STD_INPUT_HANDLE, kInheritStdin), hout = toFile(outFile_, stdHandle(STD_OUTPUT_HANDLE, kInheritStdout)),
         herr = toFile(errFile_, stdHandle(STD_ERROR_HANDLE, kInheritStderr));
  std::vector<HANDLE> list = {hin, hout, herr};
  for (auto h : inherit) {
    SetHandleInformation(reinterpret_cast<HANDLE>(h), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    list.push_back(reinterpret_cast<HANDLE>(h));
  }
  // Only the listed handles reach the child.
  SIZE_T attrSize = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
  std::vector<char> attrBuf(attrSize);
  auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
  bool attrOk = InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize) &&
                UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list.data(), list.size() * sizeof(HANDLE), nullptr, nullptr);
  STARTUPINFOEXW si{};
  si.StartupInfo.cb = sizeof si;
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  si.StartupInfo.hStdInput = hin;
  si.StartupInfo.hStdOutput = hout;
  si.StartupInfo.hStdError = herr;
  si.lpAttributeList = attrOk ? attrs : nullptr;
  PROCESS_INFORMATION pi{};
  std::wstring wexe = widen(exe);
  for (auto& c : wexe)
    if (c == L'/') c = L'\\';
  // J-192: kTree starts the child suspended inside a job object, so everything it starts is in the job too.
  HANDLE job = nullptr;
  if (flags & kTree) {
    job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
      li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      if (memoryLimit_ > 0) {  // K-154: the whole tree's committed memory
        li.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_JOB_MEMORY;
        li.JobMemoryLimit = static_cast<SIZE_T>(memoryLimit_);
      }
      SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
  }
  BOOL ok = CreateProcessW(wexe.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                           CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | (attrOk ? EXTENDED_STARTUPINFO_PRESENT : 0) |
                               (job ? CREATE_SUSPENDED : 0),
                           nullptr, nullptr, &si.StartupInfo, &pi);
  if (attrOk) DeleteProcThreadAttributeList(attrs);
  CloseHandle(hin);
  CloseHandle(hout);
  CloseHandle(herr);
  CloseHandle(nul);
  if (!ok) {
    if (job) CloseHandle(job);
    return false;
  }
  if (job) {
    if (!AssignProcessToJobObject(job, pi.hProcess)) {
      CloseHandle(job);
      job = nullptr;
    }
    ResumeThread(pi.hThread);
  }
  CloseHandle(pi.hThread);
  if (job_) CloseHandle(reinterpret_cast<HANDLE>(job_));
  job_ = reinterpret_cast<std::intptr_t>(job);
  handle_ = reinterpret_cast<std::intptr_t>(pi.hProcess);
  pid_ = static_cast<int>(pi.dwProcessId);
  started_ = true;
  status_ = {};
  return true;
}

bool Process::wait(double seconds) {
  if (!started_) return true;
  if (status_.exited) return true;
  DWORD ms = seconds < 0 ? INFINITE : static_cast<DWORD>(seconds * 1000.0);
  if (WaitForSingleObject(reinterpret_cast<HANDLE>(handle_), ms) != WAIT_OBJECT_0) return false;
  DWORD code = 0;
  GetExitCodeProcess(reinterpret_cast<HANDLE>(handle_), &code);
  status_.exited = true;
  // NTSTATUS error codes (0xC...) are crashes: access violation, stack overflow, abort (0xC0000409).
  status_.crashed = (code & 0xF0000000u) == 0xC0000000u;
  status_.code = static_cast<int>(code);
  return true;
}

void Process::kill() {
  // Reads only what start() wrote: safe while another thread waits for the child.
  if (started_ && job_) TerminateJobObject(reinterpret_cast<HANDLE>(job_), 0xC000013Au);  // J-192: the whole tree
  if (started_ && handle_) TerminateProcess(reinterpret_cast<HANDLE>(handle_), 0xC000013Au);
}

void Process::killTree() {
  if (started_ && job_) TerminateJobObject(reinterpret_cast<HANDLE>(job_), 0xC000013Au);
}

std::uint64_t Process::treeMemoryBytes() const {
  if (!started_ || status_.exited) return 0;
  auto privateBytes = [](HANDLE h) -> std::uint64_t {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof pmc;
    if (!GetProcessMemoryInfo(h, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc)) return 0;
    return static_cast<std::uint64_t>(std::max(pmc.WorkingSetSize, pmc.PrivateUsage));
  };
  if (!job_) return handle_ ? privateBytes(reinterpret_cast<HANDLE>(handle_)) : 0;
  // The job's process list (a fixed buffer: a scan tree has a handful of processes).
  struct {
    JOBOBJECT_BASIC_PROCESS_ID_LIST head;
    ULONG_PTR more[255];
  } list{};
  list.head.NumberOfAssignedProcesses = 256;
  if (!QueryInformationJobObject(reinterpret_cast<HANDLE>(job_), JobObjectBasicProcessIdList, &list, sizeof list, nullptr)) return 0;
  std::uint64_t total = 0;
  for (DWORD i = 0; i < list.head.NumberOfProcessIdsInList && i < 256; ++i) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(list.head.ProcessIdList[i]));
    if (!h) continue;
    total += privateBytes(h);
    CloseHandle(h);
  }
  return total;
}

bool Process::treeMemoryLimitHit() const {
  if (!started_ || !job_ || memoryLimit_ == 0) return false;
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
  if (!QueryInformationJobObject(reinterpret_cast<HANDLE>(job_), JobObjectExtendedLimitInformation, &li, sizeof li, nullptr)) return false;
  // An allocation that would pass the limit fails, so the peak stays just under it.
  return static_cast<std::uint64_t>(li.PeakJobMemoryUsed) + (64u << 20) >= memoryLimit_;
}

#else

bool Process::start(const std::string& exe, const std::vector<std::string>& args, unsigned flags,
                    const std::vector<std::intptr_t>& inherit) {
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  if (!(flags & kInheritStdin)) posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  // J-192: stdout / stderr into files (redirectOutput) the parent reads while the child writes.
  if (!outFile_.empty()) posix_spawn_file_actions_addopen(&fa, 1, outFile_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  else if (!(flags & kInheritStdout)) posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
  if (!errFile_.empty()) posix_spawn_file_actions_addopen(&fa, 2, errFile_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  else if (!(flags & kInheritStderr)) posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
  for (size_t i = 0; i < inherit.size(); ++i) posix_spawn_file_actions_adddup2(&fa, static_cast<int>(inherit[i]), 3 + static_cast<int>(i));
  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  short spawnFlags = 0;
  if (flags & kTree) {  // J-192: its own process group; kill() signals the group
    posix_spawnattr_setpgroup(&attr, 0);
    spawnFlags |= POSIX_SPAWN_SETPGROUP;
  }
#if defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
  // macOS: only 0-2 and the listed descriptors reach the child.
  spawnFlags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
  for (int f = 0; f < 3; ++f) posix_spawn_file_actions_addinherit_np(&fa, f);
#endif
  posix_spawnattr_setflags(&attr, spawnFlags);
  std::vector<std::string> copy = args;
  std::vector<char*> argv;
  for (auto& a : copy) argv.push_back(a.data());
  argv.push_back(nullptr);
  pid_t pid = -1;
  int rc = ::posix_spawn(&pid, exe.c_str(), &fa, &attr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  posix_spawnattr_destroy(&attr);
  if (rc != 0) return false;
  pid_ = static_cast<int>(pid);
  group_ = (flags & kTree) != 0;
  started_ = true;
  status_ = {};
  return true;
}

bool Process::wait(double seconds) {
  if (!started_) return true;
  if (status_.exited) return true;
  const auto t0 = std::chrono::steady_clock::now();
  for (;;) {
    int st = 0;
    pid_t r = ::waitpid(static_cast<pid_t>(pid_), &st, seconds < 0 ? 0 : WNOHANG);
    if (r < 0 && errno == EINTR) continue;
    if (r < 0) {  // not our child any more
      status_.exited = true;
      status_.code = -1;
      return true;
    }
    if (r > 0) {
      status_.exited = true;
      if (WIFSIGNALED(st)) {
        status_.crashed = true;
        status_.code = WTERMSIG(st);
      } else {
        status_.code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
      }
      return true;
    }
    if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() >= seconds) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

void Process::kill() {
  // Reads only what start() wrote: safe while another thread waits for the child. Callers make
  // sure the child has not been reaped yet (its id could be reused).
  if (started_ && pid_ > 0 && group_) ::kill(-static_cast<pid_t>(pid_), SIGKILL);  // J-192: the whole group
  if (started_ && pid_ > 0) ::kill(static_cast<pid_t>(pid_), SIGKILL);
}

void Process::killTree() {
  // A process group's id is never handed to a new process while the group has members, so
  // signalling it after the leader was reaped reaches only what is left of this tree.
  if (started_ && pid_ > 0 && group_) ::kill(-static_cast<pid_t>(pid_), SIGKILL);
}

namespace {
// K-154: resident memory of the processes in group `pgid` (or of `pid` alone when pgid <= 0).
std::uint64_t groupMemory(pid_t pid, pid_t pgid) {
  const std::uint64_t page = static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
  (void)page;
#if defined(__linux__)
  // /proc/<n>/stat: "pid (comm) state ppid pgrp ...", rss (pages) is field 24.
  auto read = [&](const std::string& dir, pid_t& group) -> std::uint64_t {
    std::ifstream f(dir + "/stat");
    std::string s;
    if (!std::getline(f, s)) return 0;
    const size_t close = s.rfind(')');
    if (close == std::string::npos) return 0;
    std::istringstream in(s.substr(close + 2));
    std::string field;
    std::uint64_t rss = 0;
    for (int i = 3; in >> field; ++i) {
      if (i == 5) group = static_cast<pid_t>(std::atol(field.c_str()));
      if (i == 24) {
        rss = std::strtoull(field.c_str(), nullptr, 10);
        break;
      }
    }
    return rss * page;
  };
  if (pgid <= 0) {
    pid_t g = 0;
    return read("/proc/" + std::to_string(pid), g);
  }
  std::uint64_t total = 0;
  std::error_code ec;
  for (fs::directory_iterator it("/proc", ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = it->path().filename().string();
    if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos) continue;
    pid_t g = 0;
    const std::uint64_t r = read(it->path().string(), g);
    if (g == pgid) total += r;
  }
  return total;
#elif defined(__APPLE__)
  auto footprint = [](pid_t p) -> std::uint64_t {
    rusage_info_v2 ri{};
    if (proc_pid_rusage(p, RUSAGE_INFO_V2, reinterpret_cast<rusage_info_t*>(&ri)) != 0) return 0;
    return ri.ri_phys_footprint;
  };
  if (pgid <= 0) return footprint(pid);
  pid_t pids[512] = {};
  const int n = std::min(512, proc_listpgrppids(pgid, pids, static_cast<int>(sizeof pids)));  // (a count of processes)
  std::uint64_t total = 0;
  for (int i = 0; i < n; ++i)
    if (pids[i] > 0) total += footprint(pids[i]);
  return total;
#elif defined(__FreeBSD__)
  int mib[4] = {CTL_KERN, KERN_PROC, pgid > 0 ? KERN_PROC_PGRP : KERN_PROC_PID, pgid > 0 ? static_cast<int>(pgid) : static_cast<int>(pid)};
  size_t len = 0;
  if (sysctl(mib, 4, nullptr, &len, nullptr, 0) != 0 || len == 0) return 0;
  len += 16 * sizeof(kinfo_proc);
  std::vector<kinfo_proc> procs(len / sizeof(kinfo_proc) + 1);
  len = procs.size() * sizeof(kinfo_proc);
  if (sysctl(mib, 4, procs.data(), &len, nullptr, 0) != 0) return 0;
  std::uint64_t total = 0;
  for (size_t i = 0; i < len / sizeof(kinfo_proc); ++i) total += static_cast<std::uint64_t>(procs[i].ki_rssize) * page;
  return total;
#elif defined(__OpenBSD__)
  int mib[6] = {CTL_KERN, KERN_PROC, pgid > 0 ? KERN_PROC_PGRP : KERN_PROC_PID, pgid > 0 ? static_cast<int>(pgid) : static_cast<int>(pid),
                static_cast<int>(sizeof(kinfo_proc)), 0};
  size_t len = 0;
  if (sysctl(mib, 6, nullptr, &len, nullptr, 0) != 0 || len == 0) return 0;
  len += 16 * sizeof(kinfo_proc);
  std::vector<kinfo_proc> procs(len / sizeof(kinfo_proc) + 1);
  len = procs.size() * sizeof(kinfo_proc);
  mib[5] = static_cast<int>(procs.size());
  if (sysctl(mib, 6, procs.data(), &len, nullptr, 0) != 0) return 0;
  std::uint64_t total = 0;
  for (size_t i = 0; i < len / sizeof(kinfo_proc); ++i) total += static_cast<std::uint64_t>(procs[i].p_vm_rssize) * page;
  return total;
#elif defined(__NetBSD__)
  int mib[6] = {CTL_KERN, KERN_PROC2, pgid > 0 ? KERN_PROC_PGRP : KERN_PROC_PID, pgid > 0 ? static_cast<int>(pgid) : static_cast<int>(pid),
                static_cast<int>(sizeof(kinfo_proc2)), 0};
  size_t len = 0;
  if (sysctl(mib, 6, nullptr, &len, nullptr, 0) != 0 || len == 0) return 0;
  len += 16 * sizeof(kinfo_proc2);
  std::vector<kinfo_proc2> procs(len / sizeof(kinfo_proc2) + 1);
  len = procs.size() * sizeof(kinfo_proc2);
  mib[5] = static_cast<int>(procs.size());
  if (sysctl(mib, 6, procs.data(), &len, nullptr, 0) != 0) return 0;
  std::uint64_t total = 0;
  for (size_t i = 0; i < len / sizeof(kinfo_proc2); ++i) total += static_cast<std::uint64_t>(procs[i].p_vm_rssize) * page;
  return total;
#else
  (void)pid;
  (void)pgid;
  return 0;
#endif
}
}  // namespace

std::uint64_t Process::treeMemoryBytes() const {
  if (!started_ || status_.exited || pid_ <= 0) return 0;
  return groupMemory(static_cast<pid_t>(pid_), group_ ? static_cast<pid_t>(pid_) : 0);
}

bool Process::treeMemoryLimitHit() const { return false; }

#endif

namespace {
#if !defined(_WIN32)
// K-154: an ended process its parent has not reaped yet (a zombie). A container's first process
// (`tail -f /dev/null`, `sleep infinity`) never reaps, so a killed orphan stays one for good. No
// allocation: plain reads.
bool zombie(pid_t pid) {
#if defined(__linux__)
  char path[32];
  std::snprintf(path, sizeof path, "/proc/%d/stat", static_cast<int>(pid));
  const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  char buf[512];
  const ssize_t n = ::read(fd, buf, sizeof buf - 1);
  ::close(fd);
  if (n <= 0) return false;
  buf[n] = '\0';
  // "pid (comm) state ...": the state follows the last ')' (a name may hold one).
  const char* close = std::strrchr(buf, ')');
  if (!close || close[1] != ' ') return false;
  return close[2] == 'Z' || close[2] == 'X' || close[2] == 'x';
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#if defined(SZOMB)
  constexpr int kZombie = SZOMB;
#else
  constexpr int kZombie = 5;  // the value on every BSD and macOS
#endif
#if defined(__APPLE__)
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, static_cast<int>(pid)};
  kinfo_proc kp{};
  size_t len = sizeof kp;
  if (sysctl(mib, 4, &kp, &len, nullptr, 0) != 0 || len == 0) return false;
  return kp.kp_proc.p_stat == kZombie;
#elif defined(__FreeBSD__)
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, static_cast<int>(pid)};
  kinfo_proc kp{};
  size_t len = sizeof kp;
  if (sysctl(mib, 4, &kp, &len, nullptr, 0) != 0 || len == 0) return false;
  return kp.ki_stat == kZombie;
#elif defined(__OpenBSD__)
  int mib[6] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, static_cast<int>(pid), static_cast<int>(sizeof(kinfo_proc)), 1};
  kinfo_proc kp{};
  size_t len = sizeof kp;
  if (sysctl(mib, 6, &kp, &len, nullptr, 0) != 0 || len == 0) return false;
  return kp.p_stat == kZombie || kp.p_stat == kZombie + 1;  // SZOMB, SDEAD
#else
  int mib[6] = {CTL_KERN, KERN_PROC2, KERN_PROC_PID, static_cast<int>(pid), static_cast<int>(sizeof(kinfo_proc2)), 1};
  kinfo_proc2 kp{};
  size_t len = sizeof kp;
  if (sysctl(mib, 6, &kp, &len, nullptr, 0) != 0 || len == 0) return false;
  return kp.p_stat == kZombie || kp.p_stat == kZombie + 1;  // SZOMB, SDEAD
#endif
#else
  (void)pid;
  return false;
#endif
}
#endif
}  // namespace

bool processRunning(std::int64_t pid) {
  if (pid <= 0) return false;
#if defined(_WIN32)
  HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
  if (!h) return GetLastError() == ERROR_ACCESS_DENIED;  // another user's: running
  const bool running = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
  CloseHandle(h);
  return running;
#else
  if (::kill(static_cast<pid_t>(pid), 0) != 0 && errno != EPERM) return false;  // EPERM: another user's
  return !zombie(static_cast<pid_t>(pid));
#endif
}

}  // namespace gl::platform
