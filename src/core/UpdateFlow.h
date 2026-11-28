// Gridline - automatic updates: the work the splash screen shows.
//
// One Flow per start, on its own thread: check (a few seconds at most, with the cached ETag), then
// for an install that can update itself: check the disk and the place, fetch SHA256SUMS (and its
// minisign signature when this build has a release key), download with resume and retries, check
// size, checksum and version, prepare the new copy next to the old one (unpack, the disk image's
// app, its signature and its minimum system), test-run it, swap it in (UpdateApply.h), start the
// watcher and say "restart". Every failure ends in "start this version normally", logged, with a
// backoff so it does not ask again at every start.
//
// The splash asks snapshot() every frame; "Start now, update later" (background()) lets Gridline
// start while the download goes on, and the next start installs the verified copy at once.
// Every outside effect (network, programs, the clock, file operations) is injectable: the tests
// drive it against a local server and shims. See docs/updates.md.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "UpdateApply.h"
#include "UpdateInstall.h"
#include "UpdateVerify.h"

namespace gl::update::flow {

// Runs a program: args[0] is the program. Exit code, -1 when it could not start, -2 when it ran
// past `seconds` (killed). `output`: its stdout and stderr.
using Runner = std::function<int(const std::vector<std::string>& args, double seconds, std::string* output)>;
Runner systemRunner();
// Continues `path` from `url` (http::downloadContinue).
using Resumer = std::function<bool(const std::string& url, const std::string& path, const std::function<void(int64_t, int64_t)>& progress,
                                   const std::atomic<bool>* cancel, int stallSeconds, std::string* err)>;

struct Env {
  std::string configDir;
  std::string settingsPath;
  std::string currentVersion;
  bool beta = false;
  Install install;
  Target target;
  CondFetcher fetch;
  Resumer download;
  Runner run;
  std::vector<MinisignKey> keys;  // releaseKeys(); empty: SHA-256 only (see docs/updates.md)
  std::function<int64_t()> now;
  apply::FileOps ops;
  int pid = 0;
  std::vector<std::string> relaunchArgs;  // this start's arguments (without the program)
  std::string bundleId;    // macOS: the running app's CFBundleIdentifier
  std::string osVersion;   // macOS: "14.6"
  int checkTimeout = 4;    // seconds for the check
  int stallSeconds = 20;   // a download with no data this long: the splash lets Gridline start
  int watchSeconds = 60;   // the watcher's wait for the new version to finish starting
  bool startWatcher = true;  // tests start it themselves
  std::function<void(double seconds)> sleep;  // between retries (tests: no wait)
};
Env systemEnv(const std::string& configDir, const std::string& currentVersion, bool beta, const std::vector<std::string>& args);

enum class Stage { Checking, Downloading, Verifying, Installing, NeedsPermission /* the system asks: UAC or the password */, Restarting, Finished };
enum class Outcome {
  None,          // still working
  UpToDate,
  NotHere,       // this install does not update itself, or nothing fits (the popup may offer it)
  Skipped,       // a reason not to now (backoff, lock, the user said Skip)
  Offline,       // network trouble: quiet
  Failed,        // logged, noticed once
  Background,    // "Start now, update later": finishing in the background
  Staged,        // background download done: installs at the next start
  PermissionLater,  // the password prompt was refused or put off
  Relaunch,      // the new version is in place: quit now, the watcher starts it
};
const char* outcomeId(Outcome o);

struct Snapshot {
  Stage stage = Stage::Checking;
  Outcome outcome = Outcome::None;
  std::string version;  // the version being installed
  int64_t got = 0, total = 0;
  std::string message;  // English detail (log, Details)
  std::string command;  // NotHere: the package manager's command
  bool needsAdmin = false;
  bool wentBackground = false;  // the download stalled: the splash should let Gridline start (stays set)
};

class Flow {
 public:
  explicit Flow(Env env);
  ~Flow();  // cancels and waits for the thread
  void start();
  Snapshot snapshot() const;
  void background();                  // "Start now, update later"
  void cancel();                      // "Skip" / quitting: stops, keeps the part for later
  bool finished() const;
  void wait();  // tests
 private:
  void run();
  void work();
  void set(Stage s);
  void finish(Outcome o, const std::string& message);
  bool backgroundMode() const { return background_.load(); }
  Env env_;
  std::thread thread_;
  mutable std::mutex mu_;
  Snapshot snap_;
  std::atomic<bool> cancel_{false}, background_{false}, done_{false};
  std::atomic<int64_t> got_{0}, total_{0};
};

// ---- the pieces, for tests ----
// "Gridline <v>" in a program's --version output.
bool versionOutputMatches(const std::string& output, const std::string& version);
// macOS "13.0" <= "14.6".
bool osAtLeast(const std::string& have, const std::string& need);
// pkexec / apt / dnf: what went wrong, from the exit code and output.
enum class SystemInstallError { None, Cancelled, NotAuthorized, Locked, Dependencies, NoTool, Failed };
SystemInstallError classifySystemInstall(int exitCode, const std::string& output);
const char* describe(SystemInstallError e);  // English
// The command that installs a downloaded package with the system's password prompt:
// pkexec apt-get install -y <file> / pkexec dnf install -y <file> / zypper / rpm -U. Empty when
// pkexec or every package tool is missing. `have(tool)`: whether /usr/bin/<tool> (or the like) exists.
std::vector<std::string> systemInstallCommand(InstallKind k, const std::string& file, const std::function<std::string(const std::string&)>& which);
// The command line that starts `in` after the update (program + this start's arguments).
std::vector<std::string> relaunchCommand(const Install& in, const std::string& exe, const std::vector<std::string>& args);

}  // namespace gl::update::flow
