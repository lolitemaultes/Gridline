// Gridline - automatic updates: staging, swap, rollback, recovery (see UpdateApply.h).
#include "UpdateApply.h"
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include "Json.h"
#include "Platform.h"
#if defined(_WIN32)
#include <windows.h>
#else
#include <signal.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <stdio.h>  // renamex_np
#endif
#if defined(__linux__)
#include <fcntl.h>
#include <sys/syscall.h>
#endif

namespace gl::update::apply {

namespace fs = std::filesystem;

namespace {

fs::path P(const std::string& s) { return fs::u8path(s); }

std::string readText(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return {};
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool writeAtomic(const fs::path& p, const std::string& text) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  const fs::path tmp = p.u8string() + ".tmp-" + std::to_string(platform::processId());
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << text;
    out.flush();
    if (!out) {
      out.close();
      fs::remove(tmp, ec);
      return false;
    }
  }
  fs::rename(tmp, p, ec);
  if (ec) {
    fs::remove(p, ec);
    fs::rename(tmp, p, ec);
  }
  if (ec) fs::remove(tmp, ec);
  return !ec;
}

std::string isoTime(int64_t t) {
  std::time_t tt = static_cast<std::time_t>(t);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &tt);
#else
  gmtime_r(&tt, &tm);
#endif
  char b[32];
  std::strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return b;
}

Json boolJson(bool v) {
  Json j;
  j.type = Json::Type::Bool;
  j.boolean = v;
  return j;
}
bool boolOf(const Json& j, const char* k) {
  const Json* v = j.find(k);
  return v && v->is(Json::Type::Bool) && v->boolean;
}

bool removeAll(const fs::path& p) {
  std::error_code ec;
  fs::remove_all(p, ec);
  return !ec;
}

bool present(const fs::path& p) {
  std::error_code ec;
  return fs::exists(fs::symlink_status(p, ec));
}

bool renamePath(const std::string& a, const std::string& b, std::string* err) {
  std::error_code ec;
  fs::rename(P(a), P(b), ec);
  if (ec && err) *err = "could not move " + a + " to " + b + ": " + ec.message();
  return !ec;
}

}  // namespace

std::string updatesDir(const std::string& configDir) {
  const fs::path d = P(configDir) / "updates";
  std::error_code ec;
  fs::create_directories(d, ec);
  return d.u8string();
}

std::string stagingDirFor(const std::string& target) {
  fs::path t = P(target);
  if (!t.has_filename()) t = t.parent_path();
  return (t.parent_path() / ".gridline-update").u8string();
}

// ------------------------------------------------------------------ log

void log(const std::string& dir, const std::string& what, int64_t now) {
  const fs::path f = P(dir) / "update.log";
  std::error_code ec;
  fs::create_directories(P(dir), ec);
  if (fs::file_size(f, ec) > 256 * 1024 && !ec) {  // keep the newer half
    std::string all = readText(f);
    size_t cut = all.find('\n', all.size() / 2);
    if (cut != std::string::npos) writeAtomic(f, all.substr(cut + 1));
  }
  std::ofstream out(f, std::ios::binary | std::ios::app);
  std::string line = what;
  for (char& c : line)
    if (c == '\n' || c == '\r') c = ' ';
  out << isoTime(now) << " " << line << "\n";
}

std::vector<std::string> logTail(const std::string& dir, size_t lines) {
  const std::string all = readText(P(dir) / "update.log");
  std::vector<std::string> out;
  size_t a = 0;
  while (a < all.size()) {
    size_t b = all.find('\n', a);
    if (b == std::string::npos) b = all.size();
    if (b > a) out.push_back(all.substr(a, b - a));
    a = b + 1;
  }
  if (out.size() > lines) out.erase(out.begin(), out.end() - static_cast<std::ptrdiff_t>(lines));
  return out;
}

// ------------------------------------------------------------------ lock

bool processAlive(int pid) {
  if (pid <= 0) return false;
#if defined(_WIN32)
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
  if (!h) return false;
  DWORD code = 0;
  const bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
  CloseHandle(h);
  return alive;
#else
  return ::kill(pid, 0) == 0 || errno == EPERM;
#endif
}

bool lock(const std::string& dir, int pid, int64_t now, const std::function<bool(int)>& alive) {
  const fs::path l = P(dir) / "lock";
  std::error_code ec;
  fs::create_directories(P(dir), ec);
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (fs::create_directory(l, ec) && !ec) {
      writeAtomic(l / "owner", std::to_string(pid) + " " + std::to_string(now));
      return true;
    }
    // Held: by a live process that took it recently, it stays theirs.
    const std::string owner = readText(l / "owner");
    int other = 0;
    long long since = 0;
    if (std::sscanf(owner.c_str(), "%d %lld", &other, &since) < 1) other = 0;
    const bool stale = other <= 0 || !(alive ? alive(other) : processAlive(other)) || now - since > 30 * 60 || since > now + 60;
    if (other == pid) return true;
    if (!stale) return false;
    removeAll(l);
  }
  return false;
}

void unlock(const std::string& dir) { removeAll(P(dir) / "lock"); }

// ------------------------------------------------------------------ failures

const char* failKindId(FailKind k) {
  switch (k) {
    case FailKind::Network: return "network";
    case FailKind::Download: return "download";
    case FailKind::Install: return "install";
    case FailKind::Started: return "started";
    case FailKind::Cancelled: return "cancelled";
  }
  return "?";
}

bool readFailure(const std::string& dir, Failure& out) {
  Json j;
  if (!jsonParse(readText(P(dir) / "failures.json"), j) || !j.is(Json::Type::Object)) return false;
  out.version = j.text("version");
  out.kind = j.text("kind");
  out.reason = j.text("reason");
  out.count = jsonInt<int>(j.num("count"));
  out.nextTry = jsonInt<int64_t>(j.num("nextTry"));
  out.blocked = boolOf(j, "blocked");
  out.noticeShown = boolOf(j, "noticeShown");
  return true;
}

void writeFailure(const std::string& dir, const Failure& f) {
  Json j = Json::makeObject();
  j.object["version"] = Json::makeString(f.version);
  j.object["kind"] = Json::makeString(f.kind);
  j.object["reason"] = Json::makeString(f.reason);
  j.object["count"] = Json::makeNumber(f.count);
  j.object["nextTry"] = Json::makeNumber(static_cast<double>(f.nextTry));
  j.object["blocked"] = boolJson(f.blocked);
  j.object["noticeShown"] = boolJson(f.noticeShown);
  writeAtomic(P(dir) / "failures.json", jsonStringify(j));
}

std::string plainReason(const std::string& t) {
  auto has = [&t](const char* w) { return t.find(w) != std::string::npos; };
  if (has("needs macOS")) {  // "Gridline X needs macOS 13.0 or later (this Mac has ...)": the version is the useful part
    const size_t a = t.find("needs macOS"), b = t.find(" or later");
    return "it needs a newer version of macOS" + (b != std::string::npos && b > a ? " (" + t.substr(a + 6, b - a - 6) + " or later)" : std::string());
  }
  if (has("checksum") || has("changed on disk") || has("bytes instead of") || has("ended early") || has("not listed"))
    return "the download was damaged";
  if (has("signature") || has("not Gridline") || has("signed")) return "the download could not be confirmed as a genuine Gridline release";
  if (has("SHA256SUMS") || has("has no download")) return "the release is missing a file Gridline needs to check it";
  if (has("disk space")) return "there isn't enough free disk space";
  if (has("noexec") || has("doesn't allow programs")) return "the disk Gridline is on doesn't allow programs to run";
  if (has("can't write") || has("permission") || has("read-only") || has("install location")) return "Gridline can't change the folder it is installed in";
  if (has("does not start on this computer") || has("says it is") || has("another version")) return "the new version doesn't run on this computer";
  if (has("did not start") || has("didn't start")) return "the new version didn't start";
  if (has("another program is installing")) return "another program was installing software at the time";
  if (has("other software")) return "it needs other software that couldn't be installed";
  if (has("password prompt was closed") || has("Not now") || has("skipped")) return "it was put off";
  if (has("pkexec") || has("no way to ask")) return "this system has no way to ask for your password";
  if (has("did not allow") || has("package manager") || has("setup")) return "the system's installer reported a problem";
  if (has("unpack") || has("disk image") || has("copy") || has("laid out")) return "the download couldn't be unpacked";
  if (has("connection") || has("stalled") || has("reach") || has("no connection")) return "the connection dropped";
  return "something went wrong";
}

Failure recordFailure(const std::string& dir, const std::string& version, FailKind kind, const std::string& reason, int64_t now) {
  Failure f;
  readFailure(dir, f);
  const std::string k = failKindId(kind);
  if (f.version != version) f = Failure{};
  f.version = version;
  // Network trouble does not count against the version: the next start simply asks again.
  if (kind == FailKind::Network) {
    f.nextTry = std::max(f.nextTry, now + 10 * 60);
    if (f.kind.empty()) f.kind = k;
    if (f.reason.empty() || f.kind == k) f.reason = reason;
    writeFailure(dir, f);
    log(dir, "check/download " + version + ": " + reason, now);
    return f;
  }
  const bool newKind = f.kind != k || f.reason != reason;
  f.kind = k;
  f.reason = reason;
  if (kind == FailKind::Cancelled) {
    f.nextTry = now + 24 * 3600;
  } else if (kind == FailKind::Started) {
    f.blocked = true;
    f.count += 1;
  } else {
    f.count += 1;
    static const int64_t waits[] = {3600, 4 * 3600, 24 * 3600};
    if (f.count > 3) f.blocked = true;
    else f.nextTry = now + waits[f.count - 1];
  }
  if (newKind) f.noticeShown = false;
  writeFailure(dir, f);
  log(dir, std::string("update to ") + version + " failed (" + k + "): " + reason, now);
  return f;
}

void clearFailure(const std::string& dir) {
  std::error_code ec;
  fs::remove(P(dir) / "failures.json", ec);
}

bool mayTry(const std::string& dir, const std::string& version, int64_t now, std::string* why) {
  Failure f;
  if (!readFailure(dir, f) || f.version != version) return true;
  if (f.blocked) {
    if (why) *why = "not again by itself after: " + f.reason;
    return false;
  }
  if (now < f.nextTry) {
    if (why) *why = "waiting after: " + f.reason;
    return false;
  }
  return true;
}

// ------------------------------------------------------------------ journal

const char* phaseId(Phase p) {
  switch (p) {
    case Phase::None: return "none";
    case Phase::Staged: return "staged";
    case Phase::Swapping: return "swapping";
    case Phase::Swapped: return "swapped";
    case Phase::Trial: return "trial";
    case Phase::Confirmed: return "confirmed";
    case Phase::RolledBack: return "rolledback";
  }
  return "none";
}

bool readJournal(const std::string& dir, Journal& out) {
  Json j;
  if (!jsonParse(readText(P(dir) / "journal.json"), j) || !j.is(Json::Type::Object)) return false;
  Journal r;
  const std::string ph = j.text("phase");
  for (int i = 0; i <= static_cast<int>(Phase::RolledBack); ++i)
    if (ph == phaseId(static_cast<Phase>(i))) r.phase = static_cast<Phase>(i);
  r.from = j.text("from");
  r.to = j.text("to");
  r.kind = j.text("kind");
  r.mode = j.text("mode");
  r.target = j.text("target");
  r.staged = j.text("staged");
  r.backup = j.text("backup");
  r.stagedId = static_cast<uint64_t>(std::strtoull(j.text("stagedId").c_str(), nullptr, 10));
  r.trialStarts = jsonInt<int>(j.num("trialStarts"));
  r.time = jsonInt<int64_t>(j.num("time"));
  r.notes = j.text("notes");
  r.settingsCopy = j.text("settingsCopy");
  r.noticeShown = boolOf(j, "noticeShown");
  r.reason = j.text("reason");
  r.asset = j.text("asset");
  r.sha256 = j.text("sha256");
  out = r;
  return true;
}

bool writeJournal(const std::string& dir, const Journal& r) {
  Json j = Json::makeObject();
  j.object["phase"] = Json::makeString(phaseId(r.phase));
  j.object["from"] = Json::makeString(r.from);
  j.object["to"] = Json::makeString(r.to);
  j.object["kind"] = Json::makeString(r.kind);
  j.object["mode"] = Json::makeString(r.mode);
  j.object["target"] = Json::makeString(r.target);
  j.object["staged"] = Json::makeString(r.staged);
  j.object["backup"] = Json::makeString(r.backup);
  j.object["stagedId"] = Json::makeString(std::to_string(r.stagedId));
  j.object["trialStarts"] = Json::makeNumber(r.trialStarts);
  j.object["time"] = Json::makeNumber(static_cast<double>(r.time));
  j.object["notes"] = Json::makeString(r.notes);
  j.object["settingsCopy"] = Json::makeString(r.settingsCopy);
  j.object["noticeShown"] = boolJson(r.noticeShown);
  j.object["reason"] = Json::makeString(r.reason);
  j.object["asset"] = Json::makeString(r.asset);
  j.object["sha256"] = Json::makeString(r.sha256);
  return writeAtomic(P(dir) / "journal.json", jsonStringify(j));
}

void clearJournal(const std::string& dir) {
  std::error_code ec;
  fs::remove(P(dir) / "journal.json", ec);
}

// ------------------------------------------------------------------ file operations

uint64_t fileId(const std::string& path) {
#if defined(_WIN32)
  (void)path;
  return 0;
#else
  struct stat st{};
  if (::lstat(path.c_str(), &st) != 0) return 0;
  return static_cast<uint64_t>(st.st_ino);
#endif
}

bool exchangePaths(const std::string& a, const std::string& b) {
#if defined(__APPLE__)
  return ::renamex_np(a.c_str(), b.c_str(), RENAME_SWAP) == 0;
#elif defined(__linux__) && defined(SYS_renameat2)
  return ::syscall(SYS_renameat2, AT_FDCWD, a.c_str(), AT_FDCWD, b.c_str(), 2u /* RENAME_EXCHANGE */) == 0;
#else
  (void)a, (void)b;
  return false;
#endif
}

// ------------------------------------------------------------------ preflight

int64_t freeBytes(const std::string& path) {
  std::error_code ec;
  fs::path p = P(path);
  while (!p.empty() && !fs::exists(p, ec)) p = p.parent_path();
  if (p.empty()) return -1;
  const fs::space_info s = fs::space(p, ec);
  return ec ? -1 : static_cast<int64_t>(s.available);
}

bool noexecMount(const std::string& path) {
#if defined(_WIN32)
  (void)path;
  return false;
#else
  struct statvfs sv{};
  if (::statvfs(path.c_str(), &sv) != 0) return false;
#if defined(ST_NOEXEC)
  return (sv.f_flag & ST_NOEXEC) != 0;
#else
  return false;
#endif
#endif
}

namespace {
bool canWriteIn(const fs::path& dir, std::string* why) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  const fs::path probe = dir / (".gridline-write-test-" + std::to_string(platform::processId()));
  {
    std::ofstream out(probe, std::ios::binary | std::ios::trunc);
    out << "x";
    out.flush();
    if (!out) {
      if (why) *why = dir.u8string();
      return false;
    }
  }
  fs::remove(probe, ec);
  return true;
}
}  // namespace

Preflight preflight(const Install& in, const std::string& downloadDir, int64_t bytes) {
  Preflight r;
  const bool replaces = in.kind == InstallKind::AppImage || in.kind == InstallKind::TarballFolder || in.kind == InstallKind::TarballPrefix ||
                        in.kind == InstallKind::MacApp || in.kind == InstallKind::WindowsPortable;
  // Archives are unpacked next to the old copy: the archive and the unpacked files.
  const bool archive = in.kind != InstallKind::AppImage;
  const int64_t need = bytes > 0 ? bytes * (archive && replaces ? 4 : 2) + 64ll * 1024 * 1024 : 256ll * 1024 * 1024;
  std::vector<fs::path> places;
  if (replaces) {
    if (in.target.empty()) {
      r.ok = false;
      r.reason = "the install location is not known";
      return r;
    }
    places.push_back(P(stagingDirFor(in.target)));
  } else {
    places.push_back(P(downloadDir));
  }
  // The staging folder is made only to try it; it goes on every way out (only when empty).
  struct Tidy {
    fs::path p;
    bool existed;
    ~Tidy() {
      std::error_code e;
      if (!existed) fs::remove(p, e);
    }
  } tidy{places.front(), fs::exists(places.front())};
  for (const fs::path& p : places) {
    std::string where;
    if (!canWriteIn(p, &where)) {
      r.ok = false;
      r.reason = "can't write to " + p.parent_path().u8string() + " (no permission, or a read-only disk)";
      return r;
    }
    const int64_t free = freeBytes(p.u8string());
    if (free >= 0 && free < need) {
      r.ok = false;
      char b[160];
      std::snprintf(b, sizeof b, "not enough free disk space (needs %.0f MB, %.0f MB free)", static_cast<double>(need) / 1e6,
                    static_cast<double>(free) / 1e6);
      r.reason = b;
      return r;
    }
    if (replaces && noexecMount(p.u8string())) {
      r.ok = false;
      r.reason = p.parent_path().u8string() + " is on a disk that doesn't allow programs to run (noexec)";
      return r;
    }
  }
  return r;
}

// ------------------------------------------------------------------ swap and rollback

bool swapIn(const std::string& dir, Journal& j, const FileOps& ops, std::string* err) {
  const std::string stage = stagingDirFor(j.target);
  if (j.backup.empty()) j.backup = (P(stage) / ("previous-" + j.from)).u8string();
  std::error_code ec;
  if (!present(P(j.staged))) {
    if (err) *err = "the new version is not there any more";
    return false;
  }
  if (!present(P(j.target))) {
    if (err) *err = j.target + " is missing";
    return false;
  }
  removeAll(P(j.backup));  // an older leftover
  j.stagedId = fileId(j.staged);
  j.phase = Phase::Swapping;
  if (!ops.before("journal:swapping") || !writeJournal(dir, j)) {
    if (err) *err = "could not write the update journal";
    return false;
  }
  if (j.mode == "file") {
    // The old file stays reachable as the backup (a hard link: no copy, no space), then the new one
    // is renamed over the old name in one step: the path is never missing.
    if (!ops.before("swap:link")) {
      if (err) *err = "could not keep the old version";
      j.phase = Phase::None;
      writeJournal(dir, j);
      return false;
    }
    fs::create_hard_link(P(j.target), P(j.backup), ec);
    if (ec) {
      ec.clear();
      fs::copy_file(P(j.target), P(j.backup), fs::copy_options::overwrite_existing, ec);
    }
    if (ec) {
      if (err) *err = "could not keep the old version: " + ec.message();
      removeAll(P(j.backup));
      j.phase = Phase::None;
      writeJournal(dir, j);
      return false;
    }
    if (!ops.before("swap:rename") || !renamePath(j.staged, j.target, err)) {
      if (err && err->empty()) *err = "could not put the new version in place";
      removeAll(P(j.backup));
      j.phase = Phase::None;
      writeJournal(dir, j);
      return false;
    }
  } else {
    bool exchanged = false;
    if (ops.atomicExchange) {
      if (!ops.before("swap:exchange")) {
        if (err) *err = "could not swap the folders";
        j.phase = Phase::None;
        writeJournal(dir, j);
        return false;
      }
      exchanged = exchangePaths(j.staged, j.target);
    }
    if (exchanged) {
      // `staged` now holds the old version.
      if (!ops.before("swap:keep-old") || !renamePath(j.staged, j.backup, err)) {
        // The new version is in place; the old one stays under the staged name (recover() finds it).
        j.backup = j.staged;
      }
    } else {
      if (!ops.before("swap:move-old") || !renamePath(j.target, j.backup, err)) {
        if (err && err->empty()) *err = "could not move the old version aside";
        j.phase = Phase::None;
        writeJournal(dir, j);
        return false;
      }
      if (!ops.before("swap:move-new") || !renamePath(j.staged, j.target, err)) {
        std::string e2;
        renamePath(j.backup, j.target, &e2);  // the old one back at once
        if (err && err->empty()) *err = "could not put the new version in place";
        j.phase = Phase::None;
        writeJournal(dir, j);
        return false;
      }
    }
  }
  j.phase = Phase::Swapped;
  j.trialStarts = 0;
  ops.before("journal:swapped");
  writeJournal(dir, j);
  return true;
}

bool rollBack(const std::string& dir, Journal& j, const FileOps& ops, std::string* err) {
  if (!present(P(j.backup))) {
    if (err) *err = "the previous version was not kept";
    return false;
  }
  if (j.mode == "file") {
    if (!ops.before("rollback:rename") || !renamePath(j.backup, j.target, err)) return false;
  } else {
    const std::string failed = (P(stagingDirFor(j.target)) / ("failed-" + j.to)).u8string();
    removeAll(P(failed));
    if (present(P(j.target))) {
      if (!ops.before("rollback:move-new") || !renamePath(j.target, failed, err)) return false;
    }
    if (!ops.before("rollback:move-old") || !renamePath(j.backup, j.target, err)) {
      std::string e2;
      renamePath(failed, j.target, &e2);
      return false;
    }
    removeAll(P(failed));
  }
  j.phase = Phase::RolledBack;
  writeJournal(dir, j);
  return true;
}

void keepSettings(const std::string& dir, Journal& j, const std::string& settingsPath) {
  std::error_code ec;
  if (!fs::is_regular_file(P(settingsPath), ec)) return;
  const fs::path copy = P(dir) / ("settings-before-" + j.to + ".json");
  fs::copy_file(P(settingsPath), copy, fs::copy_options::overwrite_existing, ec);
  if (!ec) j.settingsCopy = copy.u8string();
}

void restoreSettings(const std::string& dir, const Journal& j, const std::string& settingsPath) {
  if (j.settingsCopy.empty()) return;
  const std::string before = readText(P(j.settingsCopy));
  if (before.empty()) return;
  const std::string now = readText(P(settingsPath));
  if (now == before) return;
  std::error_code ec;
  if (!now.empty()) fs::copy_file(P(settingsPath), P(dir) / ("settings-from-" + j.to + ".json"), fs::copy_options::overwrite_existing, ec);
  writeAtomic(P(settingsPath), before);
}

// ------------------------------------------------------------------ recovery at start

Recovery recover(const std::string& dir, const std::string& currentVersion, const std::string& settingsPath, const FileOps& ops,
                 int64_t now) {
  Recovery r;
  Journal j;
  if (!readJournal(dir, j)) return r;
  r.journal = j;
  // The watcher rolled back (it leaves a marker; it cannot write JSON).
  const fs::path marker = P(dir) / ("rolledback-" + j.to);
  if (present(marker) && j.phase != Phase::RolledBack) {
    const std::string code = readText(marker);
    j.phase = Phase::RolledBack;
    j.reason = "Gridline " + j.to + " stopped while starting (exit " + std::string(code.empty() ? "?" : code.substr(0, code.find('\n'))) + ")";
    j.noticeShown = false;
    writeJournal(dir, j);
    std::error_code ec;
    fs::remove(marker, ec);
    restoreSettings(dir, j, settingsPath);
    recordFailure(dir, j.to, FailKind::Started, j.reason, now);
    log(dir, "rolled back to " + j.from + ": " + j.reason, now);
  }
  switch (j.phase) {
    case Phase::None:
    case Phase::Staged: return r;
    case Phase::Swapping: {
      // Cut short in the middle of the swap: which side of it are we on?
      const bool targetThere = present(P(j.target));
      const bool newInPlace = targetThere && j.stagedId != 0 && fileId(j.target) == j.stagedId;
      if (newInPlace) {
        if (j.mode != "file" && present(P(j.staged)) && !present(P(j.backup))) j.backup = j.staged;  // exchanged, old not renamed yet
        j.phase = Phase::Swapped;
        writeJournal(dir, j);
        r.what = Recovery::What::FinishedSwap;
        r.message = "an interrupted update to " + j.to + " had finished";
        log(dir, r.message, now);
        break;  // on to the trial below
      }
      if (!targetThere && present(P(j.backup))) {
        std::string e;
        renamePath(j.backup, j.target, &e);
      } else if (j.mode == "file") {
        removeAll(P(j.backup));  // the link of the old file (the old file is still in place)
      }
      removeAll(P(j.staged));
      j.phase = Phase::None;
      clearJournal(dir);
      r.journal = j;
      r.what = Recovery::What::UndidSwap;
      r.message = "an interrupted update to " + j.to + " was undone; " + j.from + " is whole";
      log(dir, r.message, now);
      return r;
    }
    case Phase::RolledBack:
      r.journal = j;
      r.what = j.noticeShown ? Recovery::What::Nothing : Recovery::What::RolledBack;
      return r;
    case Phase::Confirmed:
      r.journal = j;
      r.what = j.noticeShown || j.to != currentVersion ? Recovery::What::Nothing : Recovery::What::Confirmed;
      return r;
    case Phase::Swapped:
    case Phase::Trial: break;
  }
  // Swapped / Trial: the new version is in place.
  if (j.to != currentVersion) {
    // The old version runs although the new one is in place: the user started a copy from elsewhere,
    // or the watcher put the old one back by hand. Nothing to do here; the new one confirms itself.
    r.journal = j;
    return r;
  }
  j.trialStarts += 1;
  if (j.trialStarts > 2 && j.mode != "none") {
    // Two starts that never finished starting: the old version comes back.
    r.journal = j;
    r.what = Recovery::What::RollBackSelf;
    r.message = "Gridline " + j.to + " did not finish starting twice";
    std::string err;
    if (rollBack(dir, j, ops, &err)) {
      j.reason = r.message;
      writeJournal(dir, j);
      restoreSettings(dir, j, settingsPath);
      recordFailure(dir, j.to, FailKind::Started, r.message, now);
      r.journal = j;
    } else {
      r.what = Recovery::What::Trial;
      r.message += "; the old version could not be put back: " + err;
      log(dir, r.message, now);
      j.phase = Phase::Trial;
      writeJournal(dir, j);
    }
    return r;
  }
  j.phase = Phase::Trial;
  writeJournal(dir, j);
  r.journal = j;
  if (r.what == Recovery::What::Nothing) r.what = Recovery::What::Trial;
  return r;
}

void confirm(const std::string& dir, const std::string& currentVersion, int64_t now) {
  Journal j;
  if (!readJournal(dir, j) || j.to != currentVersion || (j.phase != Phase::Trial && j.phase != Phase::Swapped)) return;
  writeAtomic(P(dir) / ("confirm-" + j.to), std::to_string(now));
  j.phase = Phase::Confirmed;
  writeJournal(dir, j);
  if (!j.backup.empty() && j.backup != j.target) removeAll(P(j.backup));
  if (!j.target.empty()) {
    std::error_code ec;
    fs::remove(P(stagingDirFor(j.target)), ec);  // only when empty
  }
  clearFailure(dir);
  log(dir, "Gridline " + j.to + " started; the update is done", now);
}

void noticeShown(const std::string& dir) {
  Journal j;
  if (!readJournal(dir, j)) return;
  j.noticeShown = true;
  writeJournal(dir, j);
}

void cleanup(const std::string& dir, const std::string& target, int64_t now) {
  Journal j;
  const bool haveJournal = readJournal(dir, j);
  const bool busy = haveJournal && (j.phase == Phase::Swapping || j.phase == Phase::Swapped || j.phase == Phase::Trial);
  const std::string keep = haveJournal && j.phase == Phase::Staged ? j.staged : std::string();  // checked, waiting for the next start
  std::error_code ec;
  const auto old = [&](const fs::path& p, int64_t seconds) {
    const auto t = fs::last_write_time(p, ec);
    if (ec) return false;
    const auto age = std::chrono::duration_cast<std::chrono::seconds>(fs::file_time_type::clock::now() - t).count();
    return age > seconds;
  };
  (void)now;
  // Markers of earlier updates.
  for (fs::directory_iterator it(P(dir), ec), end; !ec && it != end; it.increment(ec)) {
    const std::string n = it->path().filename().u8string();
    const bool marker = n.rfind("confirm-", 0) == 0 || n.rfind("exit-", 0) == 0;
    if (marker && !(busy && (n == "confirm-" + j.to || n == "exit-" + j.to))) fs::remove(it->path(), ec), ec.clear();
    if (n.rfind("settings-before-", 0) == 0 && old(it->path(), 30 * 24 * 3600)) fs::remove(it->path(), ec), ec.clear();
    if (n.find(".tmp-") != std::string::npos && old(it->path(), 3600)) fs::remove(it->path(), ec), ec.clear();
  }
  // Downloads nobody resumed in two weeks.
  for (fs::directory_iterator it(P(dir) / "downloads", ec), end; !ec && it != end; it.increment(ec))
    if (old(it->path(), 14 * 24 * 3600) && !(it->path() == P(keep))) removeAll(it->path());
  ec.clear();
  // The staging folder next to the install: everything but the update in progress.
  if (!target.empty() && !busy) {
    const fs::path stage = P(stagingDirFor(target));
    for (fs::directory_iterator it(stage, ec), end; !ec && it != end; it.increment(ec)) {
      const std::string n = it->path().filename().u8string();
      const bool part = n.size() > 5 && n.compare(n.size() - 5, 5, ".part") == 0;
      const bool partMeta = n.size() > 10 && n.compare(n.size() - 10, 10, ".part.json") == 0;
      if ((part || partMeta) && !old(it->path(), 14 * 24 * 3600)) continue;  // a download to resume
      if (!keep.empty() && it->path() == P(keep)) continue;
      removeAll(it->path());
    }
    ec.clear();
    fs::remove(stage, ec);
  }
}

// ------------------------------------------------------------------ watcher

std::string watcherScript() {
  return R"SH(#!/bin/sh
# Gridline update watcher (written by Gridline, see docs/updates.md).
# watcher.sh <pid> <updates dir> <version> <file|folder|none> <target> <backup> <seconds> -- <command...>
pid=$1; dir=$2; ver=$3; mode=$4; target=$5; backup=$6; watch=$7
shift 7
[ "$1" = "--" ] && shift
say() { printf '%s watcher: %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$1" >> "$dir/update.log"; }
# The new version must not see the old AppImage's mount.
unset APPDIR APPIMAGE ARGV0 OWD
case "${LD_LIBRARY_PATH:-}" in *.mount_*) unset LD_LIBRARY_PATH ;; esac
n=0
while kill -0 "$pid" 2>/dev/null; do
  n=$((n + 1))
  if [ "$n" -gt 1200 ]; then say "Gridline $pid did not quit in 2 minutes; not starting $ver"; exit 1; fi
  sleep 0.1
done
rm -f "$dir/exit-$ver"
( "$@"; echo $? > "$dir/exit-$ver" ) &
t=0
limit=$((watch * 10))
while [ "$t" -lt "$limit" ]; do
  if [ -f "$dir/confirm-$ver" ]; then exit 0; fi
  if [ -f "$dir/exit-$ver" ]; then
    code=$(cat "$dir/exit-$ver")
    if [ -f "$dir/confirm-$ver" ] || [ "$code" = 0 ]; then exit 0; fi
    if [ "$mode" = none ] || [ ! -e "$backup" ]; then
      say "Gridline $ver ended with $code while starting; no previous version to go back to"
      exit 1
    fi
    say "Gridline $ver ended with $code while starting; putting the previous version back"
    if [ "$mode" = file ]; then
      mv -f "$backup" "$target" || { say "could not put $backup back"; exit 1; }
    else
      rm -rf "$target.failed-$ver"
      mv "$target" "$target.failed-$ver" && mv "$backup" "$target" || { say "could not put $backup back"; exit 1; }
      rm -rf "$target.failed-$ver"
    fi
    echo "$code" > "$dir/rolledback-$ver"
    "$@" &
    exit 0
  fi
  sleep 0.1
  t=$((t + 1))
done
exit 0
)SH";
}

std::string watcherCmdScript() {
  // %1 pid  %2 updates dir  %3 version  %4 setup.exe  %5 the Gridline program
  return "@echo off\r\n"
         "rem Gridline update watcher (written by Gridline, see docs/updates.md)\r\n"
         "setlocal\r\n"
         "set n=0\r\n"
         ":wait\r\n"
         "tasklist /FI \"PID eq %1\" 2>nul | find \"%1\" >nul\r\n"
         "if errorlevel 1 goto run\r\n"
         "set /a n+=1\r\n"
         "if %n% gtr 240 goto gaveup\r\n"
         "ping -n 2 127.0.0.1 >nul\r\n"
         "goto wait\r\n"
         ":run\r\n"
         "powershell -NoProfile -NonInteractive -Command \"try { $p = Start-Process -FilePath '%~4' -ArgumentList "
         "'/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART','/SP-','/CLOSEAPPLICATIONS','/LOG=\"%~2\\setup.log\"' -Verb RunAs -Wait "
         "-PassThru; exit $p.ExitCode } catch { exit 1223 }\"\r\n"
         "echo %errorlevel%> \"%~2\\setup-exit-%3\"\r\n"
         "start \"\" \"%~5\"\r\n"
         "exit /b 0\r\n"
         ":gaveup\r\n"
         "echo Gridline did not quit; the update to %3 was not installed>> \"%~2\\update.log\"\r\n"
         "exit /b 1\r\n";
}

std::vector<std::string> watcherArgs(const std::string& dir, int pid, const Journal& j, int seconds, const std::vector<std::string>& command) {
  std::vector<std::string> a = {"sh", (P(dir) / "watcher.sh").u8string(), std::to_string(pid), dir, j.to, j.mode.empty() ? "none" : j.mode,
                                j.target.empty() ? "-" : j.target, j.backup.empty() ? "-" : j.backup, std::to_string(seconds), "--"};
  a.insert(a.end(), command.begin(), command.end());
  return a;
}

bool startWatcher(const std::string& dir, int pid, const Journal& j, int seconds, const std::vector<std::string>& command, std::string* err) {
#if defined(_WIN32)
  (void)seconds;
  const fs::path script = P(dir) / "watcher.cmd";
  if (!writeAtomic(script, watcherCmdScript())) {
    if (err) *err = "could not write the watcher";
    return false;
  }
  const std::string setup = j.staged, program = command.empty() ? std::string() : command[0];
  std::string cmd = "cmd.exe";
  platform::Process p;
  const bool ok = p.start(cmd, {"cmd.exe", "/c", script.u8string(), std::to_string(pid), dir, j.to, setup, program});
  if (!ok && err) *err = "could not start the watcher";
  return ok;
#else
  const fs::path script = P(dir) / "watcher.sh";
  if (!writeAtomic(script, watcherScript())) {
    if (err) *err = "could not write the watcher";
    return false;
  }
  platform::Process p;
  // Its own process group: it outlives Gridline and is not ended with it.
  const bool ok = p.start("/bin/sh", watcherArgs(dir, pid, j, seconds, command), platform::Process::kTree);
  if (!ok && err) *err = "could not start the watcher";
  return ok;
#endif
}

}  // namespace gl::update::apply
