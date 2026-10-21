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
#include <cstdint>
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
// Environment (UTF-8). setEnv with overwrite=false keeps an existing value.
bool setEnv(const char* name, const char* value, bool overwrite = true);
bool unsetEnv(const char* name);
// Absolute path of the running executable, empty when unknown.
std::string executablePath();

// Locale-independent numbers ('.' as the decimal separator whatever LC_NUMERIC says; hosted
// plugins may change the process locale). parseDouble behaves like strtod.
double parseDouble(const char* s, char** end = nullptr);
// printf-style formatting of one double ("%.17g", "%.3f", ...) with '.' as the separator.
std::string formatDouble(const char* fmt, double v);

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
  ExitStatus status_;
};

}  // namespace gl::platform
