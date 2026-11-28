// Gridline - automatic updates: putting the new version in place and taking it back, in sandbox
// folders only. The swap is broken on purpose at every step (refused, or "killed" there, and once
// by really killing a process in the middle), and recover() must always leave one whole version.
// The watcher script runs with stand-in programs that start fine, crash or quit early. Also the
// lock between two Gridlines, the backoff after failures, cleanup and the checks before a download.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "Check.h"
#include "core/Platform.h"
#include "core/UpdateApply.h"
#if !defined(_WIN32)
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace gl::update;
using namespace gl::update::apply;
namespace fs = std::filesystem;

namespace {

fs::path gRoot;

std::string readFile(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
void writeFile(const fs::path& p, const std::string& s) {
  fs::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << s;
}

struct Sandbox {
  fs::path root, config, updates, apps;
  explicit Sandbox(const std::string& name) {
    root = gRoot / name;
    std::error_code ec;
    fs::remove_all(root, ec);
    config = root / "config";
    apps = root / "Applications";
    fs::create_directories(config);
    fs::create_directories(apps);
    updates = fs::u8path(updatesDir(config.u8string()));
    writeFile(config / "settings.json", "{\"ui.scale\": 1.0}");
    writeFile(root / "Projects" / "song.glp", "my song");
  }
  // A "file" install (AppImage) at Applications/Gridline.AppImage with a staged new one.
  Journal fileSetup() {
    writeFile(apps / "Gridline.AppImage", "old 0.8.0");
    const fs::path st = fs::u8path(stagingDirFor((apps / "Gridline.AppImage").u8string()));
    writeFile(st / "Gridline-0.9.0-linux-x86_64.AppImage", "new 0.9.0");
    Journal j;
    j.from = "0.8.0";
    j.to = "0.9.0";
    j.kind = "appimage";
    j.mode = "file";
    j.target = (apps / "Gridline.AppImage").u8string();
    j.staged = (st / "Gridline-0.9.0-linux-x86_64.AppImage").u8string();
    return j;
  }
  // A "folder" install (the macOS app, the .tar.gz folder) with a staged new one.
  Journal folderSetup() {
    writeFile(apps / "Gridline.app" / "Contents" / "version", "old 0.8.0");
    writeFile(apps / "Gridline.app" / "Contents" / "only-old", "x");
    const fs::path st = fs::u8path(stagingDirFor((apps / "Gridline.app").u8string()));
    writeFile(st / "new-0.9.0" / "Gridline.app" / "Contents" / "version", "new 0.9.0");
    Journal j;
    j.from = "0.8.0";
    j.to = "0.9.0";
    j.kind = "mac-app";
    j.mode = "folder";
    j.target = (apps / "Gridline.app").u8string();
    j.staged = (st / "new-0.9.0" / "Gridline.app").u8string();
    return j;
  }
  std::string installed(const Journal& j) const {
    return j.mode == "file" ? readFile(j.target) : readFile(fs::u8path(j.target) / "Contents" / "version");
  }
  bool userFilesUntouched() const { return readFile(root / "Projects" / "song.glp") == "my song"; }
};

struct Killed : std::runtime_error {
  Killed() : std::runtime_error("killed") {}
};

const char* kFileSteps[] = {"journal:swapping", "swap:link", "swap:rename", "journal:swapped"};
const char* kFolderSteps[] = {"journal:swapping", "swap:exchange", "swap:keep-old", "journal:swapped"};
const char* kFolderStepsNoExchange[] = {"journal:swapping", "swap:move-old", "swap:move-new", "journal:swapped"};

int64_t T0 = 1764300000;

}  // namespace

void plainWords() {
  const char* banned[] = {"SHA", "pkexec", "ETag", "minisign", "RENAME", "checksum", "SUMS"};
  const char* reasons[] = {"the downloaded file does not match its SHA-256 checksum", "the release has no Gridline-0.9.0-SHA256SUMS",
                           "the signature of SHA256SUMS does not match (minisign)", "no way to ask for the password here (pkexec is missing)",
                           "the new app's code signature is not valid", "Gridline 0.9.0 needs macOS 99.0 or later (this Mac has 14.6)",
                           "can't write to /x (no permission, or a read-only disk)", "not enough free disk space (needs 80 MB, 3 MB free)",
                           "renameat2 RENAME_EXCHANGE failed", "an ETag answer nobody expected"};
  bool clean = true;
  for (const char* r : reasons) {
    const std::string p = plainReason(r);
    for (const char* b : banned) clean = clean && p.find(b) == std::string::npos;
  }
  expect(clean, "plain reasons: no checksums, tools or file names reach the interface");
  expect(plainReason("the downloaded file does not match its SHA-256 checksum") == "the download was damaged", "plain reasons: a damaged download says so");
  expect(plainReason("Gridline 0.9.0 needs macOS 99.0 or later (this Mac has 14.6)").find("macOS 99.0 or later") != std::string::npos,
         "plain reasons: the macOS version a release needs is kept");
}

int main(int argc, char** argv) {
  gRoot = fs::temp_directory_path() / ("gl-update-apply-" + std::to_string(gl::platform::processId()));
  // Helper mode for the real-kill test: does a swap that sleeps in the middle (the parent kills it).
  if (argc == 4 && std::string(argv[1]) == "--swap-and-sleep") {
    Journal j;
    if (!readJournal(argv[2], j)) return 2;
    FileOps ops;
    ops.atomicExchange = false;
    const std::string at = argv[3];
    ops.before = [at](const std::string& step) {
      if (step == at) {
        std::printf("ready\n");
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::seconds(30));
      }
      return true;
    };
    std::string err;
    swapIn(argv[2], j, ops, &err);
    return 0;
  }
  plainWords();

  // ---- the lock
  {
    Sandbox s("lock");
    const std::string d = s.updates.u8string();
    auto alive = [](int pid) { return pid == 111; };
    expect(lock(d, 111, T0, alive), "lock: the first Gridline takes it");
    expect(!lock(d, 222, T0 + 5, alive), "lock: a second Gridline does not, while the first runs");
    expect(lock(d, 111, T0 + 6, alive), "lock: the owner may take it again");
    expect(lock(d, 222, T0 + 7, [](int) { return false; }), "lock: taken over when its owner is gone");
    unlock(d);
    expect(lock(d, 333, T0, alive) && lock(d, 444, T0 + 31 * 60, [](int) { return true; }), "lock: taken over after 30 minutes");
    unlock(d);
    expect(processAlive(gl::platform::processId()) && !processAlive(-5), "processAlive: this process yes, nonsense no");
  }

  // ---- failures and backoff
  {
    Sandbox s("backoff");
    const std::string d = s.updates.u8string();
    expect(mayTry(d, "0.9.0", T0), "backoff: nothing recorded, try");
    recordFailure(d, "0.9.0", FailKind::Download, "checksum", T0);
    expect(!mayTry(d, "0.9.0", T0 + 60) && mayTry(d, "0.9.0", T0 + 3601), "backoff: 1 hour after the first failure");
    recordFailure(d, "0.9.0", FailKind::Download, "checksum", T0 + 3601);
    expect(!mayTry(d, "0.9.0", T0 + 3601 + 3 * 3600) && mayTry(d, "0.9.0", T0 + 3601 + 4 * 3600 + 1), "backoff: 4 hours after the second");
    recordFailure(d, "0.9.0", FailKind::Download, "checksum", T0 + 20 * 3600);
    expect(!mayTry(d, "0.9.0", T0 + 40 * 3600) && mayTry(d, "0.9.0", T0 + 45 * 3600), "backoff: a day after the third");
    recordFailure(d, "0.9.0", FailKind::Download, "checksum", T0 + 45 * 3600);
    expect(!mayTry(d, "0.9.0", T0 + 400 * 3600), "backoff: after the fourth, only by hand");
    expect(mayTry(d, "0.9.1", T0 + 45 * 3600), "backoff: a newer version is tried again");
    Failure f;
    expect(readFailure(d, f) && f.count == 4 && f.blocked && !f.noticeShown, "backoff: the record (count, blocked, notice pending)");
    clearFailure(d);
    expect(mayTry(d, "0.9.0", T0 + 45 * 3600), "Try again: clears it");
    recordFailure(d, "0.9.0", FailKind::Network, "offline", T0);
    expect(!mayTry(d, "0.9.0", T0 + 60) && mayTry(d, "0.9.0", T0 + 601), "network trouble: 10 minutes, never counted");
    expect(readFailure(d, f) && f.count == 0, "network trouble does not count against the version");
    recordFailure(d, "0.9.0", FailKind::Started, "crashed", T0);
    expect(!mayTry(d, "0.9.0", T0 + 999999), "a version that failed to start is never tried again by itself");
    clearFailure(d);
    recordFailure(d, "0.9.0", FailKind::Cancelled, "Not now", T0);
    expect(!mayTry(d, "0.9.0", T0 + 23 * 3600) && mayTry(d, "0.9.0", T0 + 24 * 3600 + 1), "Not now: asked again tomorrow");
    expect(!logTail(d, 50).empty(), "every failure is logged");
  }

  // ---- journal
  {
    Sandbox s("journal");
    Journal j = s.fileSetup();
    j.phase = Phase::Trial;
    j.trialStarts = 2;
    j.notes = "notes\nwith lines";
    j.stagedId = 123456789012ull;
    j.sha256 = "ab";
    expect(writeJournal(s.updates.u8string(), j), "journal written");
    Journal k;
    expect(readJournal(s.updates.u8string(), k) && k.phase == Phase::Trial && k.trialStarts == 2 && k.notes == j.notes && k.stagedId == j.stagedId &&
               k.target == j.target && k.sha256 == "ab",
           "journal read back");
    writeFile(s.updates / "journal.json", "{ damaged");
    expect(!readJournal(s.updates.u8string(), k), "a damaged journal is not used");
  }

  // ---- swap a file, roll it back
  {
    Sandbox s("swapfile");
    Journal j = s.fileSetup();
    std::string err;
    expect(swapIn(s.updates.u8string(), j, FileOps{}, &err) && s.installed(j) == "new 0.9.0" && readFile(j.backup) == "old 0.8.0" && j.phase == Phase::Swapped,
           "file swap: the new one in place, the old one kept");
    expect(rollBack(s.updates.u8string(), j, FileOps{}, &err) && s.installed(j) == "old 0.8.0" && j.phase == Phase::RolledBack, "file rollback");
    expect(s.userFilesUntouched(), "file swap: projects untouched");
  }
  // ---- swap a folder (atomic exchange, and the two-rename fallback), roll it back
  for (bool exchange : {true, false}) {
    Sandbox s(exchange ? "swapdir" : "swapdir2");
    Journal j = s.folderSetup();
    FileOps ops;
    ops.atomicExchange = exchange;
    std::string err;
    const std::string tag = exchange ? "folder swap (exchange): " : "folder swap (two renames): ";
    expect(swapIn(s.updates.u8string(), j, ops, &err) && s.installed(j) == "new 0.9.0" && readFile(fs::u8path(j.backup) / "Contents" / "version") == "old 0.8.0",
           (tag + "new in place, old kept").c_str());
    expect(!fs::exists(fs::u8path(j.target) / "Contents" / "only-old"), (tag + "nothing of the old one mixed in").c_str());
    expect(rollBack(s.updates.u8string(), j, ops, &err) && s.installed(j) == "old 0.8.0" && fs::exists(fs::u8path(j.target) / "Contents" / "only-old"),
           (tag + "rollback puts the whole old one back").c_str());
  }
#if defined(__APPLE__) || defined(__linux__)
  {
    Sandbox s("exchange");
    writeFile(s.root / "a" / "f", "A");
    writeFile(s.root / "b" / "f", "B");
    expect(exchangePaths((s.root / "a").u8string(), (s.root / "b").u8string()) && readFile(s.root / "a" / "f") == "B" && readFile(s.root / "b" / "f") == "A",
           "the system's atomic exchange works here");
  }
#endif

  // ---- refused at every step: nothing changes
  auto refuseAll = [&](const char* const* steps, size_t n, bool folder, bool exchange, const char* what) {
    bool ok = true;
    for (size_t i = 0; i < n; ++i) {
      Sandbox s(std::string("refuse-") + what + std::to_string(i));
      Journal j = folder ? s.folderSetup() : s.fileSetup();
      FileOps ops;
      ops.atomicExchange = exchange;
      const std::string step = steps[i];
      ops.before = [step](const std::string& at) { return at != step; };
      std::string err;
      const bool swapped = swapIn(s.updates.u8string(), j, ops, &err);
      const std::string v = s.installed(j);
      // Refusing the last journal write still leaves a finished swap (the new one); every earlier
      // refusal leaves the old one.
      const bool good = step == "journal:swapped" || step == "swap:keep-old" ? (swapped && v == "new 0.9.0") : (!swapped && v == "old 0.8.0");
      if (!good) std::printf("# refused at %s: swapped=%d installed=%s err=%s\n", step.c_str(), swapped, v.c_str(), err.c_str());
      ok = ok && good;
    }
    return ok;
  };
  expect(refuseAll(kFileSteps, 4, false, true, "file"), "file swap refused at each step: the old version stays whole");
  expect(refuseAll(kFolderSteps, 4, true, true, "dir"), "folder swap (exchange) refused at each step: one whole version");
  expect(refuseAll(kFolderStepsNoExchange, 4, true, false, "dir2"), "folder swap (two renames) refused at each step: the old version stays whole");

  // ---- "killed" at every step, then the next start's recover(): one whole version, consistent journal
  auto killAll = [&](const char* const* steps, size_t n, bool folder, bool exchange, const char* what) {
    bool ok = true;
    for (size_t i = 0; i < n; ++i) {
      for (const char* runs : {"0.8.0", "0.9.0"}) {  // the next start may be the old or the new program
        Sandbox s(std::string("kill-") + what + std::to_string(i) + runs);
        Journal j = folder ? s.folderSetup() : s.fileSetup();
        FileOps ops;
        ops.atomicExchange = exchange;
        const std::string step = steps[i];
        ops.before = [step](const std::string& at) {
          if (at == step) throw Killed();
          return true;
        };
        std::string err;
        try {
          swapIn(s.updates.u8string(), j, ops, &err);
        } catch (const Killed&) {
        }
        const Recovery r = recover(s.updates.u8string(), runs, (s.config / "settings.json").u8string(), FileOps{}, T0);
        const std::string v = s.installed(j);
        Journal after;
        const bool hasJournal = readJournal(s.updates.u8string(), after);
        const bool whole = v == "old 0.8.0" || v == "new 0.9.0";
        const bool consistent = !hasJournal || after.phase != Phase::Swapping;
        // When the new one is in place, the old one must still be kept for a rollback.
        const bool keptOld = v != "new 0.9.0" || (hasJournal && (readFile(after.backup) == "old 0.8.0" ||
                                                                  readFile(fs::u8path(after.backup) / "Contents" / "version") == "old 0.8.0"));
        if (!(whole && consistent && keptOld))
          std::printf("# killed at %s (next start %s): installed=%s phase=%s what=%d\n", step.c_str(), runs, v.c_str(),
                      hasJournal ? phaseId(after.phase) : "-", static_cast<int>(r.what));
        ok = ok && whole && consistent && keptOld && s.userFilesUntouched();
      }
    }
    return ok;
  };
  expect(killAll(kFileSteps, 4, false, true, "file"), "file swap killed at each step: the next start finds one whole version");
  expect(killAll(kFolderSteps, 4, true, true, "dir"), "folder swap (exchange) killed at each step: one whole version, the old one kept");
  expect(killAll(kFolderStepsNoExchange, 4, true, false, "dir2"), "folder swap (two renames) killed at each step: one whole version");

#if !defined(_WIN32)
  // ---- a real process killed (SIGKILL) between moving the old folder aside and moving the new one in
  {
    Sandbox s("realkill");
    Journal j = s.folderSetup();
    writeJournal(s.updates.u8string(), j);
    gl::platform::Process p;
    const fs::path out = s.root / "helper.txt";
    p.redirectOutput(out.u8string(), std::string());
    const bool started = p.start(argv[0], {argv[0], "--swap-and-sleep", s.updates.u8string(), "swap:move-new"});
    for (int i = 0; i < 100 && readFile(out).find("ready") == std::string::npos; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const bool midway = !fs::exists(j.target);  // the old one is aside, the new one not in yet
    ::kill(p.pid(), SIGKILL);
    p.wait(5);
    const Recovery r = recover(s.updates.u8string(), "0.8.0", (s.config / "settings.json").u8string(), FileOps{}, T0);
    expect(started && midway && r.what == Recovery::What::UndidSwap && s.installed(j) == "old 0.8.0",
           "a process really killed in the middle of the swap: the next start puts the old version back whole");
  }
#endif

  // ---- the new version's first starts, rollback by itself, confirm
  {
    Sandbox s("trial");
    Journal j = s.fileSetup();
    std::string err;
    keepSettings(s.updates.u8string(), j, (s.config / "settings.json").u8string());
    swapIn(s.updates.u8string(), j, FileOps{}, &err);
    const std::string sp = (s.config / "settings.json").u8string();
    Recovery r = recover(s.updates.u8string(), "0.9.0", sp, FileOps{}, T0);
    expect(r.what == Recovery::What::Trial && r.journal.trialStarts == 1, "first start of the new version: a trial");
    writeFile(s.config / "settings.json", "{\"migrated by 0.9.0\": true}");  // the new version changed the settings
    r = recover(s.updates.u8string(), "0.9.0", sp, FileOps{}, T0 + 10);
    expect(r.what == Recovery::What::Trial && r.journal.trialStarts == 2, "second unfinished start: still a trial");
    r = recover(s.updates.u8string(), "0.9.0", sp, FileOps{}, T0 + 20);
    expect(r.what == Recovery::What::RollBackSelf && s.installed(j) == "old 0.8.0", "third unfinished start: the old version is put back");
    expect(readFile(s.config / "settings.json") == "{\"ui.scale\": 1.0}" && readFile(s.updates / "settings-from-0.9.0.json").find("migrated") != std::string::npos,
           "rollback: the settings as they were, the new version's copy kept aside");
    expect(!mayTry(s.updates.u8string(), "0.9.0", T0 + 999999), "rollback: that version is not installed again by itself");
    r = recover(s.updates.u8string(), "0.8.0", sp, FileOps{}, T0 + 30);
    expect(r.what == Recovery::What::RolledBack, "the old version says once that the new one was put back");
    noticeShown(s.updates.u8string());
    r = recover(s.updates.u8string(), "0.8.0", sp, FileOps{}, T0 + 40);
    expect(r.what == Recovery::What::Nothing, "... and only once");
    expect(s.userFilesUntouched(), "trial and rollback: projects untouched");
  }
  {
    Sandbox s("confirm");
    Journal j = s.fileSetup();
    std::string err;
    swapIn(s.updates.u8string(), j, FileOps{}, &err);
    const std::string sp = (s.config / "settings.json").u8string();
    recover(s.updates.u8string(), "0.9.0", sp, FileOps{}, T0);
    confirm(s.updates.u8string(), "0.9.0", T0 + 5);
    Journal k;
    expect(readJournal(s.updates.u8string(), k) && k.phase == Phase::Confirmed && !fs::exists(j.backup) && fs::exists(s.updates / "confirm-0.9.0"),
           "confirm: done, the old copy deleted, the watcher told");
    Recovery r = recover(s.updates.u8string(), "0.9.0", sp, FileOps{}, T0 + 10);
    expect(r.what == Recovery::What::Confirmed, "the next start shows \"Updated to\" once");
    noticeShown(s.updates.u8string());
    expect(recover(s.updates.u8string(), "0.9.0", sp, FileOps{}, T0 + 20).what == Recovery::What::Nothing, "... and then nothing");
    confirm(s.updates.u8string(), "0.9.0", T0 + 30);  // a second confirm is harmless
    expect(s.installed(j) == "new 0.9.0", "a second confirm changes nothing");
  }

  // ---- cleanup
  {
    Sandbox s("cleanup");
    const std::string target = (s.apps / "Gridline.AppImage").u8string();
    writeFile(target, "old");
    const fs::path st = fs::u8path(stagingDirFor(target));
    writeFile(st / "previous-0.7.0", "leftover");
    writeFile(st / "new-0.8.5" / "x", "leftover");
    writeFile(st / "Gridline-0.9.0-linux-x86_64.AppImage.part", "resume me");
    writeFile(st / "Gridline-0.6.0-linux-x86_64.AppImage.part", "too old");
    fs::last_write_time(st / "Gridline-0.6.0-linux-x86_64.AppImage.part", fs::file_time_type::clock::now() - std::chrono::hours(24 * 20));
    writeFile(s.updates / "confirm-0.7.0", "x");
    writeFile(s.updates / "exit-0.7.0", "1");
    cleanup(s.updates.u8string(), target, T0);
    expect(!fs::exists(st / "previous-0.7.0") && !fs::exists(st / "new-0.8.5") && !fs::exists(s.updates / "confirm-0.7.0") && !fs::exists(s.updates / "exit-0.7.0"),
           "cleanup: leftovers of earlier updates removed");
    expect(fs::exists(st / "Gridline-0.9.0-linux-x86_64.AppImage.part") && !fs::exists(st / "Gridline-0.6.0-linux-x86_64.AppImage.part"),
           "cleanup: a recent part kept for resuming, an old one removed");
    expect(readFile(target) == "old", "cleanup: the install itself untouched");
    Journal j = s.fileSetup();
    std::string err;
    swapIn(s.updates.u8string(), j, FileOps{}, &err);
    cleanup(s.updates.u8string(), j.target, T0);
    expect(fs::exists(j.backup), "cleanup: never the old copy of an update still being tried");
  }

  // ---- checks before a download
  {
    Sandbox s("preflight");
    Install in;
    in.kind = InstallKind::AppImage;
    in.target = (s.apps / "Gridline.AppImage").u8string();
    writeFile(in.target, "old");
    expect(preflight(in, s.updates.u8string(), 1000).ok, "preflight: room and rights: fine");
    const Preflight big = preflight(in, s.updates.u8string(), int64_t(1) << 60);
    expect(!big.ok && big.reason.find("disk space") != std::string::npos, "preflight: not enough disk space, said so");
    Install none;
    none.kind = InstallKind::AppImage;
    expect(!preflight(none, s.updates.u8string(), 10).ok, "preflight: no known place, no update");
#if !defined(_WIN32)
    if (::geteuid() != 0) {
      fs::permissions(s.apps, fs::perms::owner_read | fs::perms::owner_exec);
      const Preflight ro = preflight(in, s.updates.u8string(), 1000);
      fs::permissions(s.apps, fs::perms::owner_all);
      expect(!ro.ok && ro.reason.find("can't write") != std::string::npos, "preflight: a folder this user can't write to, said so");
    }
#endif
    expect(!fs::exists(fs::u8path(stagingDirFor(in.target))), "preflight leaves no folder behind");
  }

#if !defined(_WIN32)
  // ---- the watcher, with stand-in programs
  auto script = [](const fs::path& p, const std::string& body) {
    writeFile(p, "#!/bin/sh\n" + body + "\n");
    fs::permissions(p, fs::perms::owner_all);
  };
  auto runWatcher = [&](Sandbox& s, Journal& j, const std::string& newApp, int pid, int seconds) {
    writeFile(s.updates / "watcher.sh", watcherScript());
    // The swap as Gridline does it, then the watcher with the "new" program.
    std::string err;
    swapIn(s.updates.u8string(), j, FileOps{}, &err);
    script(s.root / "launch.sh", newApp);
    gl::platform::Process w;
    const bool ok = w.start("/bin/sh", watcherArgs(s.updates.u8string(), pid, j, seconds, {"/bin/sh", (s.root / "launch.sh").u8string()}));
    w.wait(seconds + 30);
    return ok;
  };
  {
    Sandbox s("watch-ok");
    Journal j = s.fileSetup();
    const std::string confirmLine = "touch '" + (s.updates / "confirm-0.9.0").u8string() + "'; sleep 1";
    runWatcher(s, j, confirmLine, 999999, 10);
    expect(s.installed(j) == "new 0.9.0" && !fs::exists(s.updates / "rolledback-0.9.0"), "watcher: the new version starts: it stays");
  }
  {
    Sandbox s("watch-crash");
    Journal j = s.fileSetup();
    // The "program" crashes the first time (the new one) and records the second start (the old one).
    const std::string body = "if [ -f '" + (s.root / "first").u8string() + "' ]; then touch '" + (s.root / "old-started").u8string() +
                             "'; exit 0; fi; touch '" + (s.root / "first").u8string() + "'; exit 3";
    runWatcher(s, j, body, 999999, 10);
    for (int i = 0; i < 50 && !fs::exists(s.root / "old-started"); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    expect(s.installed(j) == "old 0.8.0" && readFile(s.updates / "rolledback-0.9.0").find('3') != std::string::npos,
           "watcher: the new version crashes while starting: the old one is put back");
    expect(fs::exists(s.root / "old-started"), "watcher: ... and started");
    const Recovery r = recover(s.updates.u8string(), "0.8.0", (s.config / "settings.json").u8string(), FileOps{}, T0);
    expect(r.what == Recovery::What::RolledBack && !mayTry(s.updates.u8string(), "0.9.0", T0 + 999999),
           "watcher rollback: reported once by the old version, not retried by itself");
    expect(logTail(s.updates.u8string(), 20).size() >= 2, "watcher: what it did is in the log");
  }
  {
    Sandbox s("watch-folder-crash");
    Journal j = s.folderSetup();
    runWatcher(s, j, "if [ -f '" + (s.root / "first").u8string() + "' ]; then exit 0; fi; touch '" + (s.root / "first").u8string() + "'; kill -9 $$", 999999, 10);
    for (int i = 0; i < 50 && s.installed(j) != "old 0.8.0"; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    expect(s.installed(j) == "old 0.8.0" && fs::exists(fs::u8path(j.target) / "Contents" / "only-old"), "watcher: an app folder killed while starting is swapped back whole");
  }
  {
    Sandbox s("watch-quit");
    Journal j = s.fileSetup();
    runWatcher(s, j, "exit 0", 999999, 5);
    expect(s.installed(j) == "new 0.9.0", "watcher: the new version quits normally before it finished starting: kept (no rollback for a clean quit)");
  }
  {
    Sandbox s("watch-waits");
    Journal j = s.fileSetup();
    // The "old Gridline", still quitting: an orphan (like the real one, whose parent is not this
    // test), so it is gone, not a zombie of ours, when it ends.
    const fs::path pidFile = s.root / "old.pid";
    gl::platform::Process launcher;
    launcher.start("/bin/sh", {"sh", "-c", "sleep 2 & echo $! > '" + pidFile.u8string() + "'"});
    launcher.wait(5);
    const int oldPid = std::atoi(readFile(pidFile).c_str());
    const auto t0 = std::chrono::steady_clock::now();
    runWatcher(s, j, "touch '" + (s.root / "started").u8string() + "'; touch '" + (s.updates / "confirm-0.9.0").u8string() + "'", oldPid, 10);
    const double waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    expect(waited >= 1.5 && fs::exists(s.root / "started"), "watcher: starts the new version only after the old one has quit");
  }
#endif
  std::error_code ec;
  fs::remove_all(gRoot, ec);
  return finish();
}
