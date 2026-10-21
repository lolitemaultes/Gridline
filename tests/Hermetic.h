// Gridline tests — a private scratch area per test run.
//
// Several builds (worktrees, parallel ctest) run the same tests at once on
// one machine. Fixed paths under the system temp folder (or the user's
// config folder) are shared between those runs, and one run deleting or
// rewriting another's files makes tests fail at random. Call
// hermeticTestEnv() first thing in main(): it creates a unique directory,
// points TMPDIR (temp_directory_path, child processes) and XDG_CONFIG_HOME
// (settings, plugin cache, plugin manager prefs) at it, keeps secrets in
// files there instead of the Keychain, and removes it when the process exits
// normally.
#pragma once
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>
#include "core/Platform.h"

namespace gl::test {

inline const std::string& hermeticTestEnv(const char* tag = "test") {
  static const std::string dir = [tag] {
    std::error_code ec;
    std::string base = std::filesystem::temp_directory_path(ec).string();
    if (base.empty()) base = "/tmp";
    // A new directory nobody else uses (create_directory fails when it exists).
    std::mt19937_64 rng(static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
                        static_cast<uint64_t>(gl::platform::processId()));
    std::string d;
    for (int attempt = 0; attempt < 100 && d.empty(); ++attempt) {
      const std::string cand = base + "/gridline-" + tag + "-" + std::to_string(gl::platform::processId()) + "-" + std::to_string(rng() % 1000000);
      if (std::filesystem::create_directory(cand, ec)) d = cand;
    }
    if (d.empty()) d = base + "/gridline-" + tag + "-" + std::to_string(gl::platform::processId());
    std::filesystem::create_directories(d + "/tmp", ec);
    std::filesystem::create_directories(d + "/config", ec);
    gl::platform::setEnv("TMPDIR", (d + "/tmp").c_str());
    gl::platform::setEnv("TMP", (d + "/tmp").c_str());   // Windows: GetTempPath reads TMP, then TEMP
    gl::platform::setEnv("TEMP", (d + "/tmp").c_str());
    gl::platform::setEnv("XDG_CONFIG_HOME", (d + "/config").c_str());
    gl::platform::setEnv("GRIDLINE_SECRET_STORE", "file");  // J-61: never the user's Keychain / Credential Manager
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
