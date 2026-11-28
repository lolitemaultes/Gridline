// Gridline - automatic updates: the splash screen's work (see UpdateFlow.h).
#include "UpdateFlow.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include "Json.h"
#include "Platform.h"
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

namespace gl::update::flow {

namespace fs = std::filesystem;
namespace ap = gl::update::apply;

namespace {

fs::path P(const std::string& s) { return fs::u8path(s); }

std::string readText(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return {};
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool writeText(const fs::path& p, const std::string& t) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  const fs::path tmp = p.u8string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out << t;
    if (!out) return false;
  }
  fs::rename(tmp, p, ec);
  return !ec;
}

int64_t sizeOf(const fs::path& p) {
  std::error_code ec;
  const auto n = fs::file_size(p, ec);
  return ec ? -1 : static_cast<int64_t>(n);
}

// A program on PATH (or in the usual system folders); "" when there is none.
std::string which(const std::string& name) {
  if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) return name;
#if defined(_WIN32)
  const char sep = ';';
  const char* exts[] = {".exe", ".cmd", ".bat", ""};
#else
  const char sep = ':';
  const char* exts[] = {""};
#endif
  std::string dirs = platform::getEnv("PATH");
#if !defined(_WIN32)
  dirs += ":/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin";
#endif
  std::stringstream ss(dirs);
  for (std::string d; std::getline(ss, d, sep);) {
    if (d.empty()) continue;
    for (const char* e : exts) {
      const fs::path f = P(d) / (name + e);
      std::error_code ec;
      if (fs::is_regular_file(f, ec)) return f.u8string();
    }
  }
  return {};
}

bool selfUpdating(Method m) {
  return m == Method::ReplaceFile || m == Method::ReplaceFolder || m == Method::RunInstallScript || m == Method::RunWindowsSetup ||
         m == Method::SystemPackage;
}

bool contains(const std::string& hay, const char* needle) {
  std::string h = hay;
  for (char& c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return h.find(needle) != std::string::npos;
}

}  // namespace

const char* outcomeId(Outcome o) {
  switch (o) {
    case Outcome::None: return "none";
    case Outcome::UpToDate: return "up-to-date";
    case Outcome::NotHere: return "not-here";
    case Outcome::Skipped: return "skipped";
    case Outcome::Offline: return "offline";
    case Outcome::Failed: return "failed";
    case Outcome::Background: return "background";
    case Outcome::Staged: return "staged";
    case Outcome::PermissionLater: return "permission-later";
    case Outcome::Relaunch: return "relaunch";
  }
  return "none";
}

// ------------------------------------------------------------------ small pieces

bool versionOutputMatches(const std::string& output, const std::string& version) {
  const std::string want = "Gridline " + version;
  size_t at = output.find(want);
  while (at != std::string::npos) {
    const size_t end = at + want.size();
    if (end == output.size() || output[end] == '\n' || output[end] == '\r' || output[end] == ' ') return true;
    at = output.find(want, at + 1);
  }
  return false;
}

bool osAtLeast(const std::string& have, const std::string& need) {
  auto parts = [](const std::string& s) {
    std::vector<int> v;
    std::stringstream ss(s);
    for (std::string x; std::getline(ss, x, '.');) v.push_back(std::atoi(x.c_str()));
    while (v.size() < 3) v.push_back(0);
    return v;
  };
  if (need.empty()) return true;
  if (have.empty()) return true;  // unknown: the test run decides
  const std::vector<int> a = parts(have), b = parts(need);
  for (size_t i = 0; i < 3; ++i)
    if (a[i] != b[i]) return a[i] > b[i];
  return true;
}

SystemInstallError classifySystemInstall(int exitCode, const std::string& output) {
  if (exitCode == 0) return SystemInstallError::None;
  if (exitCode == -1) return SystemInstallError::NoTool;
  // pkexec: 126 the password dialog was dismissed, 127 not authorised (or failed to authenticate).
  if (exitCode == 126) return SystemInstallError::Cancelled;
  if (exitCode == 127) return contains(output, "not authorized") || contains(output, "authentication") ? SystemInstallError::NotAuthorized
                                                                                                       : SystemInstallError::NoTool;
  if (contains(output, "could not get lock") || contains(output, "unable to acquire the dpkg frontend lock") ||
      contains(output, "waiting for process with pid") || contains(output, "is locked") || contains(output, "lock file"))
    return SystemInstallError::Locked;
  if (contains(output, "unmet dependencies") || contains(output, "depends:") || contains(output, "nothing provides") ||
      contains(output, "conflicting requests") || contains(output, "failed dependencies") || contains(output, "problem: "))
    return SystemInstallError::Dependencies;
  return SystemInstallError::Failed;
}

const char* describe(SystemInstallError e) {
  switch (e) {
    case SystemInstallError::None: return "installed";
    case SystemInstallError::Cancelled: return "the password prompt was closed";
    case SystemInstallError::NotAuthorized: return "the system did not allow the installation";
    case SystemInstallError::Locked: return "another program is installing software right now";
    case SystemInstallError::Dependencies: return "the package needs other software that could not be installed";
    case SystemInstallError::NoTool: return "no way to ask for the password here (pkexec is missing)";
    case SystemInstallError::Failed: return "the package manager reported an error";
  }
  return "?";
}

std::vector<std::string> systemInstallCommand(InstallKind k, const std::string& file, const std::function<std::string(const std::string&)>& which) {
  const std::string pkexec = which("pkexec");
  if (pkexec.empty()) return {};
  if (k == InstallKind::Deb) {
    // apt resolves the new version's dependencies; dpkg alone would leave them broken.
    if (const std::string apt = which("apt-get"); !apt.empty()) return {pkexec, apt, "install", "-y", "--no-install-recommends", file};
    if (const std::string dpkg = which("dpkg"); !dpkg.empty()) return {pkexec, dpkg, "-i", file};
    return {};
  }
  if (k == InstallKind::Rpm) {
    if (const std::string dnf = which("dnf"); !dnf.empty()) return {pkexec, dnf, "install", "-y", file};
    if (const std::string zyp = which("zypper"); !zyp.empty())
      return {pkexec, zyp, "--non-interactive", "install", "--allow-unsigned-rpm", file};
    if (const std::string rpm = which("rpm"); !rpm.empty()) return {pkexec, rpm, "-U", file};
  }
  return {};
}

std::vector<std::string> relaunchCommand(const Install& in, const std::string& exe, const std::vector<std::string>& args) {
  std::vector<std::string> c;
  switch (in.kind) {
    case InstallKind::AppImage: c.push_back(in.target); break;
    case InstallKind::TarballFolder: c.push_back((P(in.target) / "bin" / "gridline-app").u8string()); break;
    case InstallKind::MacApp: c.push_back((P(in.target) / "Contents" / "MacOS" / "gridline-app").u8string()); break;
    case InstallKind::WindowsPortable: c.push_back((P(in.target) / "gridline-app.exe").u8string()); break;
    default: c.push_back(exe); break;
  }
  c.insert(c.end(), args.begin(), args.end());
  return c;
}

// ------------------------------------------------------------------ the real computer

Runner systemRunner() {
  return [](const std::vector<std::string>& args, double seconds, std::string* output) -> int {
    if (args.empty()) return -1;
    std::error_code ec;
    static std::atomic<int> n{0};
    fs::path tmp = fs::temp_directory_path(ec);
    if (ec) tmp = ".";
    const fs::path out = tmp / ("gridline-update-run-" + std::to_string(platform::processId()) + "-" + std::to_string(n++) + ".txt");
    platform::Process p;
    p.redirectOutput(out.u8string(), out.u8string());
    std::string exe = args[0];
    exe = which(exe);
    if (exe.empty() || !p.start(exe, args)) {
      fs::remove(out, ec);
      return -1;
    }
    int code;
    if (!p.wait(seconds)) {
      p.kill();
      p.wait(5);
      code = -2;
    } else {
      code = p.status().crashed ? 128 : p.status().code;
    }
    if (output) *output = readText(out);
    fs::remove(out, ec);
    return code;
  };
}

Env systemEnv(const std::string& configDir, const std::string& currentVersion, bool beta, const std::vector<std::string>& args) {
  Env e;
  e.configDir = configDir;
  e.settingsPath = (P(configDir) / "settings.json").u8string();
  e.currentVersion = currentVersion;
  e.beta = beta;
  e.install = detectCurrentInstall();
  e.target = currentTarget();
  e.run = systemRunner();
  e.keys = releaseKeys();
  e.now = [] { return static_cast<int64_t>(std::time(nullptr)); };
  e.pid = platform::processId();
  e.relaunchArgs = args;
  e.sleep = [](double s) { std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(s * 1000))); };
#if defined(__APPLE__)
  char v[64] = {};
  size_t n = sizeof v;
  if (sysctlbyname("kern.osproductversion", v, &n, nullptr, 0) == 0) e.osVersion = v;
  if (e.install.kind == InstallKind::MacApp) {
    std::string out;
    if (e.run({"/usr/libexec/PlistBuddy", "-c", "Print :CFBundleIdentifier", (P(e.install.target) / "Contents" / "Info.plist").u8string()}, 10,
              &out) == 0) {
      while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
      e.bundleId = out;
    }
  }
#endif
  // Test keys only ever go with the tests' own local server.
  if (const char* k = std::getenv("GRIDLINE_UPDATE_TEST_KEY"); k && *k) {
    const char* srv = std::getenv("GRIDLINE_UPDATE_TEST_SERVER");
    MinisignKey key;
    if (srv && std::strncmp(srv, "http://127.0.0.1:", 17) == 0 && parseMinisignKey(k, key)) e.keys = {key};
  }
  return e;
}

// ------------------------------------------------------------------ Flow

Flow::Flow(Env env) : env_(std::move(env)) {
  if (!env_.now) env_.now = [] { return static_cast<int64_t>(std::time(nullptr)); };
  if (!env_.sleep) env_.sleep = [](double) {};
}

Flow::~Flow() {
  cancel_ = true;
  if (thread_.joinable()) thread_.join();
}

void Flow::start() {
  thread_ = std::thread([this] { run(); });
}

Snapshot Flow::snapshot() const {
  std::lock_guard<std::mutex> lk(mu_);
  Snapshot s = snap_;
  s.got = got_.load();
  s.total = total_.load();
  return s;
}

void Flow::background() { background_ = true; }
void Flow::cancel() { cancel_ = true; }
bool Flow::finished() const { return done_.load(); }
void Flow::wait() {
  if (thread_.joinable()) thread_.join();
}

void Flow::set(Stage s) {
  std::lock_guard<std::mutex> lk(mu_);
  snap_.stage = s;
}

void Flow::finish(Outcome o, const std::string& message) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    snap_.stage = Stage::Finished;
    snap_.outcome = o;
    snap_.message = message;
  }
  ap::log(ap::updatesDir(env_.configDir), std::string("splash: ") + outcomeId(o) + (message.empty() ? "" : ": " + message), env_.now());
}

void Flow::run() {
  const std::string dir = ap::updatesDir(env_.configDir);
  if (!ap::lock(dir, env_.pid, env_.now(), nullptr)) {
    finish(Outcome::Skipped, "another Gridline is updating");
    done_ = true;
    return;
  }
  try {
    work();
  } catch (const std::exception& e) {
    finish(Outcome::Failed, std::string("unexpected error: ") + e.what());
  } catch (...) {
    finish(Outcome::Failed, "unexpected error");
  }
  ap::unlock(dir);
  done_ = true;
}

namespace {

struct Cache {
  std::string url, etag, body;
  int64_t checkedAt = 0;
};
Cache readCache(const std::string& dir) {
  Cache c;
  Json j;
  if (jsonParse(readText(P(dir) / "cache.json"), j) && j.is(Json::Type::Object)) {
    c.url = j.text("url");
    c.etag = j.text("etag");
    c.body = j.text("body");
    c.checkedAt = jsonInt<int64_t>(j.num("checkedAt"));
  }
  return c;
}
void writeCache(const std::string& dir, const Cache& c) {
  Json j = Json::makeObject();
  j.object["url"] = Json::makeString(c.url);
  j.object["etag"] = Json::makeString(c.etag);
  j.object["body"] = Json::makeString(c.body);
  j.object["checkedAt"] = Json::makeNumber(static_cast<double>(c.checkedAt));
  writeText(P(dir) / "cache.json", jsonStringify(j));
}

}  // namespace

void Flow::work() {
  const std::string dir = ap::updatesDir(env_.configDir);
  const int64_t now = env_.now();
  ap::cleanup(dir, env_.install.target, now);
  Strategy strat = strategyFor(env_.install, nullptr, env_.target);
  if (!selfUpdating(strat.method)) {
    {
      std::lock_guard<std::mutex> lk(mu_);
      snap_.command = strat.command;
    }
    finish(Outcome::NotHere, std::string("this copy (") + kindName(env_.install.kind) + ") is not updated by Gridline");
    return;
  }
  // A verified download from an earlier start (Start now, update later).
  ap::Journal staged;
  const bool haveStaged = ap::readJournal(dir, staged) && staged.phase == ap::Phase::Staged && !staged.staged.empty() &&
                          compare(parseVersion(staged.to), parseVersion(env_.currentVersion)) > 0;

  // ---- check
  set(Stage::Checking);
  const std::string url = releasesUrl(env_.beta);
  Cache cache = readCache(dir);
  if (cache.url != url) cache = Cache{};
  HttpResult res;
  if (!cache.body.empty() && now >= cache.checkedAt && now - cache.checkedAt < 15 * 60) {
    res.status = 200;  // asked a moment ago: the same answer
    res.body = cache.body;
  } else {
    res = env_.fetch(url, cache.body.empty() ? std::string() : cache.etag, env_.checkTimeout);
    if (res.status == 304 && !cache.body.empty()) {
      res.status = 200;
      res.body = cache.body;
      cache.checkedAt = now;
      writeCache(dir, cache);
    } else if (res.status == 200) {
      cache.url = url;
      cache.etag = res.etag;
      cache.body = res.body;
      cache.checkedAt = now;
      writeCache(dir, cache);
    }
  }
  if (cancel_) return finish(Outcome::Skipped, "cancelled");
  CheckResult cr = evaluate(res, env_.currentVersion, env_.beta);
  Release rel;
  if (cr.status == Status::Available) {
    rel = cr.release;
  } else if (haveStaged && (cr.status == Status::Offline || cr.status == Status::Failed || cr.status == Status::RateLimited)) {
    // Offline, but the new version is already here and checked.
    rel.version = parseVersion(staged.to);
    rel.tag = "v" + staged.to;
  } else {
    if (cr.status == Status::Offline || cr.status == Status::RateLimited || cr.status == Status::Failed) {
      ap::recordFailure(dir, "", ap::FailKind::Network, update::describe(cr), now);
      return finish(Outcome::Offline, update::describe(cr));
    }
    if (haveStaged) ap::clearJournal(dir);  // the staged version is no longer the newest (or a downgrade)
    return finish(Outcome::UpToDate, update::describe(cr));
  }
  const std::string ver = toString(rel.version);
  {
    std::lock_guard<std::mutex> lk(mu_);
    snap_.version = ver;
  }
  std::string why;
  if (!ap::mayTry(dir, ver, now, &why)) return finish(Outcome::Skipped, why);

  std::string file;     // the verified download
  std::string sha;
  Asset stagedAsset;
  const bool useStaged = haveStaged && staged.to == ver;
  if (haveStaged && !useStaged) ap::clearJournal(dir);
  strat = strategyFor(env_.install, rel.assets.empty() ? nullptr : &rel, env_.target);
  const bool replaces = strat.method == Method::ReplaceFile || strat.method == Method::ReplaceFolder || strat.method == Method::RunInstallScript;
  const std::string downloads = replaces ? ap::stagingDirFor(env_.install.target) : (P(dir) / "downloads").u8string();
  if (useStaged) {
    set(Stage::Verifying);
    if (sha256File(staged.staged) != staged.sha256) {
      std::error_code ec;
      fs::remove(P(staged.staged), ec);
      ap::clearJournal(dir);
      ap::recordFailure(dir, ver, ap::FailKind::Download, "the downloaded update was changed on disk", now);
      return finish(Outcome::Failed, "the downloaded update was changed on disk");
    }
    file = staged.staged;
    sha = staged.sha256;
    if (rel.body.empty()) rel.body = staged.notes;
    stagedAsset.name = staged.asset;
    if (!strat.asset) strat.asset = &stagedAsset;  // only the name is needed from here on
  } else {
    if (!strat.asset) {
      ap::log(dir, "Gridline " + ver + " has no " + kindName(env_.install.kind) + " download for " + env_.target.os + " " + env_.target.arch, now);
      return finish(Outcome::NotHere, "the release has no download for this kind of install");
    }
    const Asset asset = *strat.asset;
    const ap::Preflight pf = ap::preflight(env_.install, downloads, asset.size);
    if (!pf.ok) {
      ap::recordFailure(dir, ver, ap::FailKind::Install, pf.reason, now);
      return finish(Outcome::Failed, pf.reason);
    }
    // ---- the checksum list, and its signature
    const std::string sumsName = "Gridline-" + ver + "-SHA256SUMS";
    const Asset* sums = nullptr;
    const Asset* sig = nullptr;
    for (const Asset& a : rel.assets) {
      if (a.name == sumsName) sums = &a;
      if (a.name == sumsName + ".minisig") sig = &a;
    }
    if (!sums) {
      ap::recordFailure(dir, ver, ap::FailKind::Download, "the release has no " + sumsName, now);
      return finish(Outcome::Failed, "the release has no " + sumsName);
    }
    HttpResult sr = env_.fetch(sums->url, std::string(), 15);
    if (sr.status != 200) {
      const std::string m = "could not read " + sumsName + (sr.status ? " (HTTP " + std::to_string(sr.status) + ")" : "");
      ap::recordFailure(dir, ver, ap::FailKind::Network, m, now);
      return finish(Outcome::Offline, m);
    }
    if (!env_.keys.empty()) {
      if (!sig) {
        ap::recordFailure(dir, ver, ap::FailKind::Download, "the release is not signed", now);
        return finish(Outcome::Failed, "the release is not signed");
      }
      HttpResult sg = env_.fetch(sig->url, std::string(), 15);
      if (sg.status != 200) {
        const std::string m = "could not read the release signature";
        ap::recordFailure(dir, ver, ap::FailKind::Network, m, now);
        return finish(Outcome::Offline, m);
      }
      const SigResult v = verifyMinisign(sg.body, sr.body, env_.keys, sumsName);
      if (v != SigResult::Ok) {
        const std::string m = std::string("signature check failed: ") + update::describe(v);
        ap::recordFailure(dir, ver, ap::FailKind::Download, m, now);
        return finish(Outcome::Failed, m);
      }
    }
    sha = checksumFor(sr.body, asset.name);
    if (sha.empty()) {
      ap::recordFailure(dir, ver, ap::FailKind::Download, asset.name + " is not listed in " + sumsName, now);
      return finish(Outcome::Failed, asset.name + " is not listed in " + sumsName);
    }
    // ---- download, resumable
    set(Stage::Downloading);
    std::error_code ec;
    fs::create_directories(P(downloads), ec);
    const fs::path dest = P(downloads) / asset.name;
    const fs::path part = dest.u8string() + ".part", meta = dest.u8string() + ".part.json";
    total_ = asset.size;
    if (fs::is_regular_file(dest, ec) && sha256File(dest.u8string()) == sha) {
      file = dest.u8string();  // downloaded before
    } else {
      fs::remove(dest, ec);
      if (readText(meta) != sha) fs::remove(part, ec);  // a part of something else
      writeText(meta, sha);
      std::string err;
      bool ok = false;
      static const double waits[] = {1, 3, 9};
      for (int attempt = 0; attempt < 4 && !ok; ++attempt) {
        if (attempt > 0) {
          if (cancel_) break;
          env_.sleep(waits[attempt - 1]);
        }
        if (cancel_) break;
        err.clear();
        const int stall = backgroundMode() ? std::max(60, env_.stallSeconds) : env_.stallSeconds;
        ok = env_.download(
            asset.url, part.u8string(),
            [this, &asset](int64_t got, int64_t total) {
              got_ = got;
              total_ = total > 0 ? total : asset.size;
            },
            &cancel_, stall, &err);
        const int64_t have = sizeOf(part);
        got_ = std::max<int64_t>(0, have);
        if (ok && asset.size > 0 && have != asset.size) {
          if (have > asset.size) {  // not the file we asked for (a captive portal's page, a wrong file)
            fs::remove(part, ec);
            err = "the download was " + std::to_string(have) + " bytes instead of " + std::to_string(asset.size);
          } else {
            err = "the download ended early";
          }
          ok = false;
        }
        if (!ok && err == "stalled" && !backgroundMode()) {
          background_ = true;  // the splash lets Gridline start; this goes on behind it
          std::lock_guard<std::mutex> lk(mu_);
          snap_.outcome = Outcome::Background;
          snap_.wentBackground = true;
        }
        if (!ok && cancel_) break;
        if (!ok) ap::log(dir, "download of " + asset.name + " attempt " + std::to_string(attempt + 1) + ": " + err, env_.now());
      }
      if (cancel_ && !ok) return finish(Outcome::Skipped, "cancelled (the part is kept for the next start)");
      if (!ok) {
        const bool network = err == "stalled" || err == "no connection" || err == "the connection broke off" || err == "the download ended early";
        ap::recordFailure(dir, ver, network ? ap::FailKind::Network : ap::FailKind::Download, err, env_.now());
        return finish(network ? Outcome::Offline : Outcome::Failed, err);
      }
      set(Stage::Verifying);
      if (sha256File(part.u8string()) != sha) {
        fs::remove(part, ec);
        fs::remove(meta, ec);
        const std::string m = "the downloaded file does not match its SHA-256 checksum";
        ap::recordFailure(dir, ver, ap::FailKind::Download, m, env_.now());
        return finish(Outcome::Failed, m);
      }
      fs::rename(part, dest, ec);
      fs::remove(meta, ec);
      if (ec) {
        ap::recordFailure(dir, ver, ap::FailKind::Install, "could not keep the download: " + ec.message(), env_.now());
        return finish(Outcome::Failed, "could not keep the download");
      }
      file = dest.u8string();
    }
    if (backgroundMode()) {
      ap::Journal j;
      j.phase = ap::Phase::Staged;
      j.from = env_.currentVersion;
      j.to = ver;
      j.kind = kindId(env_.install.kind);
      j.staged = file;
      j.asset = asset.name;
      j.sha256 = sha;
      j.notes = notesExcerpt(rel.body);
      j.time = env_.now();
      ap::writeJournal(dir, j);
      return finish(Outcome::Staged, "Gridline " + ver + " is ready; it is installed at the next start");
    }
  }
  if (cancel_) return finish(Outcome::Skipped, "cancelled");

  // ---- put it in place
  ap::Journal j;
  j.from = env_.currentVersion;
  j.to = ver;
  j.kind = kindId(env_.install.kind);
  j.target = env_.install.target;
  j.notes = notesExcerpt(rel.body);
  j.time = env_.now();
  auto fail = [&](const std::string& m) {
    ap::recordFailure(dir, ver, ap::FailKind::Install, m, env_.now());
    finish(Outcome::Failed, m);
  };
  auto testRun = [&](const std::string& program) -> bool {
    std::string out;
    const int code = env_.run({program, "--version"}, 30, &out);
    if (code == 0 && versionOutputMatches(out, ver)) return true;
    std::string m = "the new version does not start on this computer";
    if (code == -1) m += " (it could not be run here)";
    else if (code == -2) m += " (it did not answer)";
    else m += " (exit " + std::to_string(code) + (out.empty() ? std::string() : ": " + out.substr(0, 200)) + ")";
    fail(m);
    return false;
  };
  const std::string stage = ap::stagingDirFor(env_.install.target);
  std::vector<std::string> relaunch;
  set(Stage::Installing);
  std::error_code ec;
  switch (env_.install.kind) {
    case InstallKind::AppImage: {
      fs::permissions(P(file), fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add, ec);
      if (!testRun(file)) return;
      j.mode = "file";
      j.staged = file;
      break;
    }
    case InstallKind::TarballFolder:
    case InstallKind::WindowsPortable: {
      const fs::path into = P(stage) / ("new-" + ver);
      fs::remove_all(into, ec);
      fs::create_directories(into, ec);
      std::string out;
      if (env_.run({"tar", "-xf", file, "-C", into.u8string()}, 300, &out) != 0) return fail("could not unpack " + P(file).filename().u8string());
      // One top folder, "Gridline-<v>-<system>-<arch>".
      fs::path top;
      int n = 0;
      for (fs::directory_iterator it(into, ec), end; !ec && it != end; it.increment(ec)) top = it->path(), ++n;
      if (n != 1 || !fs::is_directory(top, ec)) return fail("the download is not laid out as expected");
      const fs::path program = env_.install.kind == InstallKind::WindowsPortable ? top / "gridline.exe" : top / "bin" / "gridline-app";
      if (!testRun(program.u8string())) return;
      j.mode = "folder";
      j.staged = top.u8string();
      break;
    }
    case InstallKind::MacApp: {
      const fs::path mnt = P(stage) / "mount", into = P(stage) / ("new-" + ver);
      fs::remove_all(into, ec);
      fs::create_directories(mnt, ec);
      fs::create_directories(into, ec);
      std::string out;
      if (env_.run({"hdiutil", "attach", "-nobrowse", "-readonly", "-noautoopen", "-noverify", "-mountpoint", mnt.u8string(), file}, 120, &out) != 0)
        return fail("could not open the disk image: " + out.substr(0, 200));
      const int copied = env_.run({"ditto", (mnt / "Gridline.app").u8string(), (into / "Gridline.app").u8string()}, 600, &out);
      std::string detachOut;
      env_.run({"hdiutil", "detach", mnt.u8string(), "-force"}, 60, &detachOut);
      fs::remove(mnt, ec);
      if (copied != 0) return fail("could not copy the new app (disk full?)");
      const fs::path app = into / "Gridline.app";
      if (env_.run({"codesign", "--verify", "--deep", "--strict", app.u8string()}, 120, &out) != 0)
        return fail("the new app's code signature is not valid");
      const std::string plist = (app / "Contents" / "Info.plist").u8string();
      std::string id;
      env_.run({"/usr/libexec/PlistBuddy", "-c", "Print :CFBundleIdentifier", plist}, 10, &id);
      while (!id.empty() && (id.back() == '\n' || id.back() == '\r' || id.back() == ' ')) id.pop_back();
      if (!env_.bundleId.empty() && id != env_.bundleId) return fail("the new app is not Gridline (" + id + ")");
      std::string minOs;
      if (env_.run({"/usr/libexec/PlistBuddy", "-c", "Print :LSMinimumSystemVersion", plist}, 10, &minOs) == 0) {
        while (!minOs.empty() && (minOs.back() == '\n' || minOs.back() == '\r' || minOs.back() == ' ')) minOs.pop_back();
        if (!osAtLeast(env_.osVersion, minOs)) return fail("Gridline " + ver + " needs macOS " + minOs + " or later (this Mac has " + env_.osVersion + ")");
      }
      // Checked above: the copy may run without the "downloaded from the internet" prompt.
      env_.run({"xattr", "-dr", "com.apple.quarantine", app.u8string()}, 60, &out);
      if (!testRun((app / "Contents" / "MacOS" / "gridline-app").u8string())) return;
      j.mode = "folder";
      j.staged = app.u8string();
      break;
    }
    case InstallKind::WindowsInstaller: {
      // Windows asks for permission when the setup starts: the splash says so first.
      {
        std::lock_guard<std::mutex> lk(mu_);
        snap_.needsAdmin = true;
      }
      set(Stage::NeedsPermission);
      // No question from Gridline: the system's own prompt (UAC / the password) is the only one.
      if (cancel_) return finish(Outcome::Skipped, "cancelled (the download is kept)");
      j.mode = "none";
      j.staged = file;
      break;
    }
    case InstallKind::Deb:
    case InstallKind::Rpm: {
      const std::vector<std::string> cmd =
          systemInstallCommand(env_.install.kind, file, [](const std::string& t) { return which(t); });
      if (cmd.empty()) {  // backs off like any failure: no new download at every start
        ap::recordFailure(dir, ver, ap::FailKind::Install, std::string(describe(SystemInstallError::NoTool)), env_.now());
        finish(Outcome::PermissionLater, std::string(describe(SystemInstallError::NoTool)) + "; the package is ready in " + file);
        return;
      }
      {
        std::lock_guard<std::mutex> lk(mu_);
        snap_.needsAdmin = true;
      }
      set(Stage::NeedsPermission);
      // No question from Gridline: the system's own prompt (UAC / the password) is the only one.
      if (cancel_) return finish(Outcome::Skipped, "cancelled (the download is kept)");
      std::string out;
      const int code = env_.run(cmd, 1800, &out);  // the stage stays NeedsPermission while the system asks and installs
      const SystemInstallError e = classifySystemInstall(code, out);
      if (e == SystemInstallError::Cancelled) {
        ap::recordFailure(dir, ver, ap::FailKind::Cancelled, describe(e), env_.now());
        return finish(Outcome::PermissionLater, describe(e));
      }
      if (e != SystemInstallError::None) {
        ap::recordFailure(dir, ver, e == SystemInstallError::Locked ? ap::FailKind::Network : ap::FailKind::Install,
                          std::string(describe(e)) + (out.empty() ? "" : ": " + out.substr(out.size() > 400 ? out.size() - 400 : 0)), env_.now());
        return finish(Outcome::Failed, describe(e));
      }
      j.mode = "none";
      break;
    }
    default: return finish(Outcome::NotHere, "this kind of install is not updated by Gridline");
  }
  if (cancel_ && j.mode != "none") return finish(Outcome::Skipped, "cancelled");
  ap::keepSettings(dir, j, env_.settingsPath);
  if (j.mode == "file" || j.mode == "folder") {
    std::string err;
    if (!ap::swapIn(dir, j, env_.ops, &err)) return fail(err);
  } else {
    j.phase = ap::Phase::Swapped;  // installed by the system / the setup: nothing to swap back
    ap::writeJournal(dir, j);
  }
  ap::log(dir, "Gridline " + ver + " is in place (" + j.kind + "); restarting", env_.now());
  set(Stage::Restarting);
  relaunch = relaunchCommand(env_.install, platform::executablePath(), env_.relaunchArgs);
  if (env_.startWatcher) {
    std::string err;
    if (!ap::startWatcher(dir, env_.pid, j, env_.watchSeconds, relaunch, &err)) ap::log(dir, "watcher: " + err, env_.now());
  }
  finish(Outcome::Relaunch, "Gridline " + ver + " is installed");
}

}  // namespace gl::update::flow
