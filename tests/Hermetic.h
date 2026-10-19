// Gridline tests — a private scratch area per test run.
//
// Several builds (worktrees, parallel ctest) run the same tests at once on
// one machine. Fixed paths under the system temp folder (or the user's
// config folder) are shared between those runs, and one run deleting or
// rewriting another's files makes tests fail at random. Call
// hermeticTestEnv() first thing in main(): it creates a unique directory,
// points TMPDIR (temp_directory_path, child processes) and XDG_CONFIG_HOME
// (settings, plugin cache, plugin manager prefs) at it, and removes it when
// the process exits normally.
#pragma once
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <unistd.h>

namespace gl::test {

inline const std::string& hermeticTestEnv(const char* tag = "test") {
  static const std::string dir = [tag] {
    std::error_code ec;
    std::string base = std::filesystem::temp_directory_path(ec).string();
    if (base.empty()) base = "/tmp";
    std::string tmpl = base + "/gridline-" + tag + "-XXXXXX";
    std::string d = mkdtemp(tmpl.data()) ? tmpl : base + "/gridline-" + tag + "-" + std::to_string(getpid());
    std::filesystem::create_directories(d + "/tmp", ec);
    std::filesystem::create_directories(d + "/config", ec);
    setenv("TMPDIR", (d + "/tmp").c_str(), 1);
    setenv("XDG_CONFIG_HOME", (d + "/config").c_str(), 1);
    static struct Cleanup {
      std::string path;
      ~Cleanup() {
        std::error_code e;
        std::filesystem::remove_all(path, e);
      }
    } cleanup{d};
    return d;
  }();
  return dir;
}

}  // namespace gl::test
