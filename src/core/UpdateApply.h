// Gridline - automatic updates: putting a verified new version in place, safely.
//
// Never a half-replaced install: the new version is staged next to the old one (same disk), checked,
// and swapped in with one atomic step where the system has one (rename over a file; the two folders
// exchanged with renamex_np RENAME_SWAP on macOS, renameat2 RENAME_EXCHANGE on Linux), every step
// written to a journal first. The old version is kept until the new one has started. recover()
// runs at every start and makes the install consistent again after a crash, a kill or a power cut
// in the middle; a watcher started just before Gridline quits starts the new version and puts the
// old one back when the new one fails to start.
//
// Everything lives in <config>/updates/ (journal.json, failures.json, update.log, lock/, downloads/)
// and in a hidden ".gridline-update" folder next to what is replaced. The updater never touches
// projects, and settings.json only to put back the copy it made when an update is rolled back.
// Pure file logic with injectable file operations (tests break them on purpose). docs/updates.md.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "UpdateInstall.h"

namespace gl::update::apply {

std::string updatesDir(const std::string& configDir);  // <config>/updates (created)
// The hidden staging folder next to `target` ("<parent>/.gridline-update").
std::string stagingDirFor(const std::string& target);

// ---- the log: <updates>/update.log, one line per event, kept under 256 KB ----
void log(const std::string& dir, const std::string& what, int64_t now);
std::vector<std::string> logTail(const std::string& dir, size_t lines);

// ---- one update at a time: <updates>/lock (a folder, made atomically, with the owner's pid) ----
// Taken over when its process is gone or it is older than 30 minutes.
bool lock(const std::string& dir, int pid, int64_t now, const std::function<bool(int)>& alive);
void unlock(const std::string& dir);
bool processAlive(int pid);

// ---- failures and backoff: <updates>/failures.json ----
enum class FailKind {
  Network,   // offline, DNS, TLS, timeouts, 5xx, rate limits: quiet, tried again at the next start
  Download,  // the file arrived wrong (size, checksum, signature): backoff, notice once
  Install,   // could not put it in place (disk, rights, test run, package manager): backoff, notice once
  Started,   // the new version did not start: rolled back; never again by itself
  Cancelled  // the user said no (password prompt, Skip): no notice, tried again tomorrow
};
const char* failKindId(FailKind k);
struct Failure {
  std::string version;   // the version that failed ("" = none)
  std::string kind;      // failKindId
  std::string reason;    // English, one line
  int count = 0;         // failures in a row for this version
  int64_t nextTry = 0;   // not automatically before this time
  bool blocked = false;  // never automatically again (Started); "Try again" clears it
  bool noticeShown = false;
};
bool readFailure(const std::string& dir, Failure& out);
void writeFailure(const std::string& dir, const Failure& f);
// Records a failure of `version` at `now` (backoff: 1 h, 4 h, 24 h, then only by hand; a network
// failure 10 minutes; Cancelled 24 h; Started blocks the version). Logged too.
Failure recordFailure(const std::string& dir, const std::string& version, FailKind kind, const std::string& reason, int64_t now);
void clearFailure(const std::string& dir);
// The reason in plain words for the interface ("the download was damaged"): no checksums, tools or
// file names. The technical reason stays in update.log and in the Details tooltip. English, lower case.
std::string plainReason(const std::string& technical);
// May `version` be tried automatically now?
bool mayTry(const std::string& dir, const std::string& version, int64_t now, std::string* why = nullptr);

// ---- the journal: <updates>/journal.json ----
enum class Phase { None, Staged, Swapping, Swapped, Trial, Confirmed, RolledBack };
const char* phaseId(Phase p);
struct Journal {
  Phase phase = Phase::None;
  std::string from, to;  // versions
  std::string kind;      // kindId
  std::string mode;      // "file", "folder", "none" (system package, Windows setup)
  std::string target;    // what is replaced
  std::string staged;    // the new copy before the swap
  std::string backup;    // the old copy after the swap
  uint64_t stagedId = 0;  // the staged copy's file id (inode), to tell it apart after a crash
  int trialStarts = 0;   // starts of the new version that have not finished starting
  int64_t time = 0;
  std::string notes;     // release notes (plain text) for "Updated to ..."
  std::string settingsCopy;  // settings.json as it was before the swap
  bool noticeShown = false;  // "Updated to ..." / "... was put back" shown
  std::string reason;        // RolledBack: why
  std::string asset;         // Staged: the release file's name
  std::string sha256;        // Staged: its checksum (checked again before it is used)
};
bool readJournal(const std::string& dir, Journal& out);
bool writeJournal(const std::string& dir, const Journal& j);  // temp file + rename
void clearJournal(const std::string& dir);

// ---- file operations (tests replace them to fail, or to "die", at any step) ----
struct FileOps {
  // `step` names the operation about to run ("swap:exchange", ...); returning false fails it as if
  // the system had refused, throwing ends the swap as a crash would (nothing after it runs).
  std::function<bool(const std::string& step)> before = [](const std::string&) { return true; };
  bool atomicExchange = true;  // use the system's exchange call where there is one
};
uint64_t fileId(const std::string& path);  // inode (0: none / Windows)
bool exchangePaths(const std::string& a, const std::string& b);  // atomic, false when not supported

// ---- checks before anything is downloaded ----
struct Preflight {
  bool ok = true;
  std::string reason;  // English
};
// Room for `bytes` next to `target` (and in `downloadDir`), a writable place, not a read-only or
// noexec mount (for kinds that run what they put there).
Preflight preflight(const Install& in, const std::string& downloadDir, int64_t bytes);
int64_t freeBytes(const std::string& path);  // -1: unknown
bool noexecMount(const std::string& path);

// ---- the swap ----
// `staged` is the verified new file / folder on the same disk as `target`. Writes the journal
// (Swapping, then Swapped), keeps the old one as `<staging>/previous-<from>`. False with `err`
// when nothing changed (the old version stays as it was).
bool swapIn(const std::string& dir, Journal& j, const FileOps& ops, std::string* err);
// Puts the kept old version back (the new one is removed). Journal: RolledBack.
bool rollBack(const std::string& dir, Journal& j, const FileOps& ops, std::string* err);
// settings.json before the swap: copied to <updates>/settings-before-<to>.json.
void keepSettings(const std::string& dir, Journal& j, const std::string& settingsPath);
// After a rollback: puts that copy back when the new version changed settings.json (the changed one
// is kept as settings-from-<to>.json).
void restoreSettings(const std::string& dir, const Journal& j, const std::string& settingsPath);

// ---- every start, before the settings are read ----
struct Recovery {
  enum class What {
    Nothing,
    FinishedSwap,   // a swap that was cut short had finished: the new version runs
    UndidSwap,      // ... had not finished: the old version is whole again
    Trial,          // this is the new version's first start(s): confirm() once it runs
    RolledBack,     // the new version failed to start; the old one is back (tell the user once)
    RollBackSelf,   // this (new) version failed to start before: put the old back and start it
    Confirmed,      // the update finished earlier; "Updated to ..." not shown yet
  } what = What::Nothing;
  Journal journal;
  std::string message;  // English, for the log
};
Recovery recover(const std::string& dir, const std::string& currentVersion, const std::string& settingsPath, const FileOps& ops,
                 int64_t now);
// The new version runs: the watcher stops watching, the kept old version is deleted.
void confirm(const std::string& dir, const std::string& currentVersion, int64_t now);
// The "Updated to ..." / "... was put back" notice was shown.
void noticeShown(const std::string& dir);
// Leftovers: .part files and staging folders of finished or abandoned updates (older than a day),
// exit / confirm markers. Never the folder of an update in progress.
void cleanup(const std::string& dir, const std::string& target, int64_t now);

// ---- the watcher (POSIX shell; a .cmd on Windows) ----
// Waits until process `pid` has ended, runs `command`, and when it ends with an error within
// `seconds` without confirm-<version> appearing, rolls back (`j.mode` "file" / "folder") and runs
// `command` again (the old version, which says so). Written to <updates>/watcher.sh.
std::string watcherScript();
std::string watcherCmdScript();  // Windows: runs the setup with elevation, then Gridline
std::vector<std::string> watcherArgs(const std::string& dir, int pid, const Journal& j, int seconds, const std::vector<std::string>& command);
bool startWatcher(const std::string& dir, int pid, const Journal& j, int seconds, const std::vector<std::string>& command, std::string* err);

}  // namespace gl::update::apply
