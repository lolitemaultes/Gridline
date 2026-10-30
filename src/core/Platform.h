// Gridline: operating-system services used across the code base (folders, process id,
// environment, executable path, locale-independent numbers, child processes).
//
// Folders per system (the Gridline folder is created below these):
//   config  $GRIDLINE_CONFIG_HOME, else $XDG_CONFIG_HOME, else
//           Windows %APPDATA%, macOS and Linux / BSD ~/.config
//           (macOS has always used ~/.config/gridline; it stays there so existing settings,
//           presets and the plugin cache keep working)
//   cache   $XDG_CACHE_HOME, else Windows %LOCALAPPDATA%, else ~/.cache
//   data    $XDG_DATA_HOME, else Windows %APPDATA%, else ~/.local/share
#pragma once
// printf-style format checking where the compiler has it.
#if defined(__GNUC__) || defined(__clang__)
#define GL_PRINTF_FORMAT(fmtIndex, firstArg) __attribute__((format(printf, fmtIndex, firstArg)))
#else
#define GL_PRINTF_FORMAT(fmtIndex, firstArg)
#endif
// Exported from a shared library / plugin binary.
#if defined(_WIN32)
#define GL_EXPORT __declspec(dllexport)
#else
#define GL_EXPORT __attribute__((visibility("default")))
#endif
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gl::platform {

// The user's home folder ($HOME; Windows: %USERPROFILE%), nullptr when unknown. The pointer stays
// valid for the life of the process (drop-in for std::getenv("HOME")).
const char* homeEnv();
std::string homeDir();  // homeEnv() or "."
std::string configRoot();
std::string cacheRoot();
std::string dataRoot();

int processId();
// Child processes that load plugins (scanner, sandbox, plugin check): a crash ends the process
// at once instead of opening a system error dialog (Windows) that would hang it until its timeout.
void quietCrashDialogs();
// Environment (UTF-8). setEnv with overwrite=false keeps an existing value.
bool setEnv(const char* name, const char* value, bool overwrite = true);
bool unsetEnv(const char* name);
// Absolute path of the running executable, empty when unknown.
std::string executablePath();

// Writes a file only the user can read (POSIX 0600; Windows: files in the user's profile are
// private to the user). False on failure.
bool writePrivateFile(const std::string& path, const std::string& data);
// The user may write to this existing file or folder (access(W_OK); Windows: not read-only).
// False with errno set (EACCES, EROFS, ...) otherwise.
bool writable(const std::string& path);
// Moves `from` over `to`, replacing it (atomic on POSIX and NTFS). False on failure, `err` set.
bool replaceFile(const std::string& from, const std::string& to, std::string* err = nullptr);
// A file time as seconds since 1970 (std::filesystem's clock has a different epoch per
// standard library: 1970 in libc++, 2174 in libstdc++, 1601 in MSVC's).
int64_t fileTimeSeconds(std::filesystem::file_time_type t);
// Processor time of this process (user + system, seconds) and its peak resident memory (MB).
double cpuSeconds();
double peakRssMb();

// Locale-independent numbers ('.' as the decimal separator whatever LC_NUMERIC says; hosted
// plugins may change the process locale). parseDouble behaves like strtod.
double parseDouble(const char* s, char** end = nullptr);
// printf-style formatting of one double ("%.17g", "%.3f", ...) with '.' as the separator.
std::string formatDouble(const char* fmt, double v);

// Shell commands (tests, tools): a word quoted for the system's shell (sh: '...';
// Windows cmd: "..."), and std::system that copes with cmd's quote stripping.
std::string shellQuote(const std::string& word);
int runShell(const std::string& command);
// The null device for shell redirections ("/dev/null", "NUL").
const char* nullDevice();

// ---- child processes ----
struct ExitStatus {
  bool exited = false;   // false: still running / unknown
  bool crashed = false;  // ended by a signal (POSIX) or an exception code (Windows)
  int code = 0;          // exit code, or the signal / exception code when crashed
  std::string describe() const;
};

class Process {
 public:
  enum Flags : unsigned {
    kQuiet = 0,            // stdin / stdout / stderr go to the null device
    kInheritStderr = 1,    // stderr shared with the parent
    kInheritStdout = 2,
    kInheritStdin = 4,
    // J-192: the child and everything it starts form one group (POSIX: its own process group;
    // Windows: a job object); kill() ends the whole tree (a downloader and the converter it runs).
    kTree = 8,
  };
  Process() = default;
  Process(const Process&) = delete;
  Process& operator=(const Process&) = delete;
  Process(Process&& o) noexcept;
  Process& operator=(Process&& o) noexcept;
  ~Process();  // does not kill the child; call kill() / wait() first

  // args[0] is the program name passed to the child. `inherit` lists extra descriptors /
  // handles the child receives (POSIX: duplicated to 3, 4, ...; Windows: inheritable handles).
  bool start(const std::string& exe, const std::vector<std::string>& args, unsigned flags = kQuiet,
             const std::vector<std::intptr_t>& inherit = {});
  // J-192: the next start() sends the child's stdout / stderr to these files (created or
  // truncated; "" = as the flags say), which the parent may read while the child writes them.
  void redirectOutput(const std::string& outFile, const std::string& errFile) {
    outFile_ = outFile;
    errFile_ = errFile;
  }
  bool running() const { return started_ && !status_.exited; }
  // Waits up to `seconds` (< 0: forever); true when the child has exited.
  bool wait(double seconds = -1);
  void kill();
  const ExitStatus& status() const { return status_; }
  int pid() const { return pid_; }

 private:
  bool started_ = false;
  int pid_ = -1;
  std::intptr_t handle_ = 0;  // Windows process handle
  std::intptr_t job_ = 0;     // Windows job object (kTree)
  bool group_ = false;        // POSIX: the child leads its own process group (kTree)
  std::string outFile_, errFile_;
  ExitStatus status_;
};

}  // namespace gl::platform
