#include "Platform.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <clocale>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#include <process.h>
#include <psapi.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#if defined(__FreeBSD__) || defined(__DragonFly__) || defined(__NetBSD__) || defined(__OpenBSD__)
#include <sys/sysctl.h>
#include <sys/types.h>
#endif

namespace fs = std::filesystem;

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

int processId() {
#if defined(_WIN32)
  return static_cast<int>(GetCurrentProcessId());
#else
  return static_cast<int>(::getpid());
#endif
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
  if (MoveFileExW(widen(from).c_str(), widen(to).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
  const DWORD code = GetLastError();
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
    status_ = o.status_;
    o.started_ = false;
    o.pid_ = -1;
    o.handle_ = 0;
  }
  return *this;
}

Process::~Process() {
#if defined(_WIN32)
  if (handle_) CloseHandle(reinterpret_cast<HANDLE>(handle_));
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
  HANDLE hin = stdHandle(STD_INPUT_HANDLE, kInheritStdin), hout = stdHandle(STD_OUTPUT_HANDLE, kInheritStdout),
         herr = stdHandle(STD_ERROR_HANDLE, kInheritStderr);
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
  BOOL ok = CreateProcessW(wexe.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                           CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | (attrOk ? EXTENDED_STARTUPINFO_PRESENT : 0), nullptr,
                           nullptr, &si.StartupInfo, &pi);
  if (attrOk) DeleteProcThreadAttributeList(attrs);
  CloseHandle(hin);
  CloseHandle(hout);
  CloseHandle(herr);
  CloseHandle(nul);
  if (!ok) return false;
  CloseHandle(pi.hThread);
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
  if (started_ && handle_) TerminateProcess(reinterpret_cast<HANDLE>(handle_), 0xC000013Au);
}

#else

bool Process::start(const std::string& exe, const std::vector<std::string>& args, unsigned flags,
                    const std::vector<std::intptr_t>& inherit) {
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  if (!(flags & kInheritStdin)) posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  if (!(flags & kInheritStdout)) posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
  if (!(flags & kInheritStderr)) posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
  for (size_t i = 0; i < inherit.size(); ++i) posix_spawn_file_actions_adddup2(&fa, static_cast<int>(inherit[i]), 3 + static_cast<int>(i));
  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
#if defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
  // macOS: only 0-2 and the listed descriptors reach the child.
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
  for (int f = 0; f < 3; ++f) posix_spawn_file_actions_addinherit_np(&fa, f);
#endif
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
  if (started_ && pid_ > 0) ::kill(static_cast<pid_t>(pid_), SIGKILL);
}

#endif

}  // namespace gl::platform
