// Gridline - automatic updates: the whole splash-screen flow against a local stand-in for the
// release host (127.0.0.1 only, tests/update_fake_server.h) through the real network code, with
// faults on demand: offline, slow, errors, captive-portal pages, broken JSON, dropped and stalled
// downloads, cut-off and corrupted files, missing files, signatures, downgrades, a failing test
// run, a read-only folder, a stalled download moving behind Gridline, closing it, two Gridlines at once, a system
// package with a stand-in password prompt (pkexec) and, on macOS, a real disk image. Nothing is
// installed outside the sandbox folders; never GitHub. Skipped on Windows (no POSIX server).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>
#include "Check.h"
#include "core/Platform.h"
#include "core/Update.h"
#include "core/UpdateFlow.h"
#include "host/net/Http.h"
#include "update_fake_server.h"
#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace gl::update;
using namespace gl::update::flow;
namespace fs = std::filesystem;
namespace ap = gl::update::apply;

#if defined(_WIN32)
int main() {
  std::printf("# the flow tests need the POSIX test server: skipped on Windows\n");
  expect(true, "skipped on Windows");
  return finish();
}
#else

namespace {

fs::path gRoot;
const int64_t T0 = 1764300000;

std::string readFile(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
void writeFile(const fs::path& p, const std::string& s) {
  fs::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << s;
}

// A program that answers --version like Gridline <v> (and is big enough to be cut and resumed).
std::string fakeProgram(const std::string& v, int exitCode = 0) {
  std::string s = "#!/bin/sh\necho \"Gridline " + v + "\"\nexit " + std::to_string(exitCode) + "\n";
  while (s.size() < 300 * 1024) s += "# padding so the download takes more than one piece ........................................\n";
  return s;
}

struct Rig {
  testsrv::Server srv;
  fs::path root, config, apps;
  std::string version = "0.9.0";
  std::string appImageBody;
  Release rel;
  Env env;
  explicit Rig(const std::string& name, const std::string& current = "0.8.0") {
    root = gRoot / name;
    std::error_code ec;
    fs::remove_all(root, ec);
    config = root / "config";
    apps = root / "Applications";
    fs::create_directories(config);
    fs::create_directories(apps);
    writeFile(config / "settings.json", "{\"ui.scale\": 1.0}");
    writeFile(root / "Projects" / "song.glp", "my song");
    writeFile(apps / "Gridline.AppImage", fakeProgram(current));
    fs::permissions(apps / "Gridline.AppImage", fs::perms::owner_all);
    gl::platform::setEnv("GRIDLINE_UPDATE_TEST_SERVER", srv.base().c_str());
    env.configDir = config.u8string();
    env.settingsPath = (config / "settings.json").u8string();
    env.currentVersion = current;
    env.install.kind = InstallKind::AppImage;
    env.install.target = (apps / "Gridline.AppImage").u8string();
    env.target = Target("linux", "x86_64");
    env.fetch = gl::http::condFetcher();
    env.download = [](const std::string& url, const std::string& path, const std::function<void(int64_t, int64_t)>& progress,
                      const std::atomic<bool>* cancel, int stall, std::string* err) {
      return gl::http::downloadContinue(url, path, progress, cancel, stall, err);
    };
    env.run = systemRunner();
    env.now = [] { return T0; };
    env.pid = gl::platform::processId();
    env.startWatcher = false;
    env.stallSeconds = 3;
    env.checkTimeout = 3;
    env.sleep = [](double) {};
    appImageBody = fakeProgram(version);
    publish();
  }
  std::string assetName() const { return "Gridline-" + version + "-linux-x86_64.AppImage"; }
  std::string sumsName() const { return "Gridline-" + version + "-SHA256SUMS"; }
  // The release JSON, the AppImage and SHA256SUMS on the server.
  void publish(bool withSums = true, bool withAsset = true, bool prerelease = false) {
    rel = Release{};
    rel.tag = "v" + version;
    rel.name = "Gridline " + version;
    rel.version = parseVersion(version);
    rel.prerelease = prerelease;
    rel.htmlUrl = "https://example.invalid/release";
    rel.body = "## What's new\n- Faster.\n";
    if (withAsset) rel.assets.push_back(Asset{assetName(), srv.base() + "/dl/" + assetName(), static_cast<int64_t>(appImageBody.size())});
    if (withSums) rel.assets.push_back(Asset{sumsName(), srv.base() + "/dl/" + sumsName(), 0});
    testsrv::Route api;
    api.body = releaseJson(rel);
    api.contentType = "application/json";
    api.etag = "\"etag-" + version + "\"";
    srv.set("/releases/latest", api);
    testsrv::Route list = api;
    list.body = "[" + releaseJson(rel) + "]";
    srv.set("/releases?per_page=20", list);
    testsrv::Route a;
    a.body = appImageBody;
    srv.set("/dl/" + assetName(), a);
    testsrv::Route s;
    s.body = sha256Hex(appImageBody) + "  " + assetName() + "\n";
    s.contentType = "text/plain";
    srv.set("/dl/" + sumsName(), s);
  }
  void addSignature(const std::string& sig) {
    rel.assets.push_back(Asset{sumsName() + ".minisig", srv.base() + "/dl/" + sumsName() + ".minisig", 0});
    testsrv::Route api = srv.get("/releases/latest");
    api.body = releaseJson(rel);
    srv.set("/releases/latest", api);
    testsrv::Route s;
    s.body = sig;
    srv.set("/dl/" + sumsName() + ".minisig", s);
  }
  void setAsset(const testsrv::Route& r) { srv.set("/dl/" + assetName(), r); }
  testsrv::Route assetRoute() {
    testsrv::Route a;
    a.body = appImageBody;
    return a;
  }
  Snapshot run() {
    Flow f(env);
    f.start();
    f.wait();
    return f.snapshot();
  }
  std::string installed() const { return readFile(apps / "Gridline.AppImage"); }
  bool newInstalled() const { return installed().find("Gridline " + version + "\"") != std::string::npos; }
  bool oldInstalled() const { return installed().find("Gridline 0.8.0\"") != std::string::npos; }
  bool untouched() const { return readFile(root / "Projects" / "song.glp") == "my song" && oldInstalled(); }
  std::string updates() const { return ap::updatesDir(config.u8string()); }
};

bool mayTryNoCount(Rig& g) {
  ap::Failure f;
  return !ap::readFailure(g.updates(), f) || f.count == 0;
}
std::string readSums(Rig& g) { return g.srv.get("/dl/" + g.sumsName()).body; }

std::string outcome(const Snapshot& s) { return std::string(outcomeId(s.outcome)) + " (" + s.message + ")"; }
void note(const char* what, const Snapshot& s) { std::printf("# %s: %s\n", what, outcome(s).c_str()); }

}  // namespace

int main() {
  gRoot = fs::temp_directory_path() / ("gl-update-flow-" + std::to_string(gl::platform::processId()));
  gl::platform::setEnv("GRIDLINE_NO_UPDATE_CHECK", "1");  // nothing else in this process may ask GitHub
  gl::platform::unsetEnv("GRIDLINE_UPDATE_TEST_KEY");

  // ---- the happy path
  {
    Rig g("happy");
    Snapshot s = g.run();
    note("happy", s);
    expect(s.outcome == Outcome::Relaunch && g.newInstalled(), "AppImage: downloaded, checked, test-run and swapped in; restart");
    ap::Journal j;
    expect(ap::readJournal(g.updates(), j) && j.phase == ap::Phase::Swapped && readFile(j.backup).find("Gridline 0.8.0") != std::string::npos,
           "the old AppImage is kept until the new one has started");
    expect(fs::exists(fs::u8path(g.updates()) / "settings-before-0.9.0.json") && readFile(g.config / "settings.json") == "{\"ui.scale\": 1.0}",
           "settings copied before, never changed");
    expect(readFile(g.root / "Projects" / "song.glp") == "my song", "projects untouched");
    bool ua = true;
    for (const testsrv::Seen& seen : g.srv.seen()) ua = ua && seen.userAgent.rfind("Gridline/", 0) == 0 && seen.userAgent.find('@') == std::string::npos;
    expect(ua, "every request: only the generic Gridline/<version> User-Agent");
    expect(fs::perms::none != (fs::status(g.apps / "Gridline.AppImage").permissions() & fs::perms::owner_exec), "the new AppImage can run");
  }
  // ---- nothing to do
  {
    Rig g("uptodate", "0.9.0");
    Snapshot s = g.run();
    expect(s.outcome == Outcome::UpToDate && g.installed().find("Gridline 0.9.0") != std::string::npos, "up to date: nothing happens");
  }
  {
    Rig g("downgrade", "1.0.0");
    Snapshot s = g.run();
    expect(s.outcome == Outcome::UpToDate && g.srv.count("/dl/" + g.assetName()) == 0, "an older release is never installed (downgrade)");
  }
  {
    Rig g("prerelease");
    g.version = "0.9.0-beta.1";
    g.appImageBody = fakeProgram(g.version);
    g.publish(true, true, true);
    // /releases/latest never answers a prerelease on GitHub; here it does, and must still be refused.
    Snapshot s = g.run();
    expect(s.outcome == Outcome::UpToDate && g.oldInstalled(), "a beta is not installed on the Stable channel");
    g.env.beta = true;
    s = g.run();
    note("beta channel", s);
    expect(s.outcome == Outcome::Relaunch && g.newInstalled(), "... and is on the Beta channel");
  }
  // ---- network and server trouble: quiet, quick, this version starts
  {
    Rig g("offline");
    g.srv.stop();
    const auto t0 = std::chrono::steady_clock::now();
    Snapshot s = g.run();
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    note("offline", s);
    expect(s.outcome == Outcome::Offline && took < 4.0 && g.untouched(), "offline (nothing listening): quiet, within seconds");
    expect(mayTryNoCount(g), "offline is not held against the version");
  }
  {
    Rig g("slowcheck");
    testsrv::Route api = g.srv.get("/releases/latest");
    api.delayMs = 15000;
    g.srv.set("/releases/latest", api);
    g.env.checkTimeout = 2;
    const auto t0 = std::chrono::steady_clock::now();
    Snapshot s = g.run();
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    note("slow check", s);
    expect(s.outcome == Outcome::Offline && took < 9.0 && g.untouched(), "a server that does not answer: the check gives up after its time");
    g.srv.stop();
  }
  for (int code : {500, 502, 503, 403, 429}) {
    Rig g("status" + std::to_string(code));
    testsrv::Route api = g.srv.get("/releases/latest");
    api.status = code;
    api.body = "{\"message\": \"busy\"}";
    g.srv.set("/releases/latest", api);
    Snapshot s = g.run();
    expect(s.outcome == Outcome::Offline && g.untouched(), ("HTTP " + std::to_string(code) + " from the server: quiet, tried later").c_str());
  }
  {
    Rig g("captive");
    testsrv::Route api;
    api.body = "<!doctype html><html><body>Sign in to the cafe network</body></html>";
    api.contentType = "text/html";
    g.srv.set("/releases/latest", api);
    Snapshot s = g.run();
    expect(s.outcome == Outcome::Offline && g.untouched(), "a captive portal's sign-in page instead of the release: quiet");
  }
  {
    Rig g("badjson");
    testsrv::Route api;
    api.body = "{\"tag_name\": \"v0.9.0\", \"assets\": [";
    g.srv.set("/releases/latest", api);
    Snapshot s = g.run();
    expect(s.outcome == Outcome::Offline && g.untouched(), "malformed release JSON: quiet");
  }
  // ---- the cached answer (rate limits)
  {
    Rig g("etag");
    g.env.currentVersion = "0.9.0";
    g.run();
    g.env.now = [] { return T0 + 20 * 60; };
    g.srv.forget();
    g.run();
    bool conditional = false;
    for (const testsrv::Seen& seen : g.srv.seen())
      if (seen.path == "/releases/latest") conditional = seen.ifNoneMatch == "\"etag-0.9.0\"";
    expect(conditional, "the next check sends the cached ETag (a 304 costs no rate limit)");
    g.env.currentVersion = "0.8.0";
    g.env.now = [] { return T0 + 40 * 60; };
    Snapshot s = g.run();
    expect(s.outcome == Outcome::Relaunch, "a 304 answer uses the cached release");
    g.srv.forget();
    g.env.now = [] { return T0 + 41 * 60; };
    g.env.currentVersion = "0.9.0";
    g.run();
    expect(g.srv.count("/releases/latest") == 0, "a check a minute ago: not asked again");
  }
  // ---- the download
  {
    Rig g("drop");
    testsrv::Route a = g.assetRoute();
    a.dropAfter = 100 * 1024;
    a.dropTimes = 2;
    g.setAsset(a);
    Snapshot s = g.run();
    note("dropped twice", s);
    bool ranged = false;
    for (const testsrv::Seen& seen : g.srv.seen()) ranged = ranged || (seen.path == "/dl/" + g.assetName() && !seen.range.empty());
    expect(s.outcome == Outcome::Relaunch && g.newInstalled() && ranged, "the connection drops mid-download: resumed with a range request");
  }
  {
    Rig g("noresume");
    testsrv::Route a = g.assetRoute();
    a.dropAfter = 100 * 1024;
    a.dropTimes = 1;
    a.ignoreRange = true;
    g.setAsset(a);
    Snapshot s = g.run();
    note("no ranges", s);
    expect(s.outcome == Outcome::Relaunch && g.newInstalled(), "a server that ignores ranges: the file comes whole again");
  }
  {
    Rig g("truncated");
    testsrv::Route a = g.assetRoute();
    a.dropAfter = 50 * 1024;
    a.dropTimes = 100;
    g.setAsset(a);
    Snapshot s = g.run();
    note("always cut", s);
    expect(s.outcome == Outcome::Offline && g.untouched(), "a file that is always cut off: not installed, quiet");
    const fs::path part = fs::u8path(ap::stagingDirFor(g.env.install.target)) / (g.assetName() + ".part");
    expect(fs::exists(part), "... the part is kept");
    g.setAsset(g.assetRoute());
    g.env.now = [] { return T0 + 3600; };
    s = g.run();
    expect(s.outcome == Outcome::Relaunch && g.newInstalled(), "the next start resumes it and installs");
  }
  {
    Rig g("stall");
    testsrv::Route a = g.assetRoute();
    a.stallAfter = 60 * 1024;
    a.stallMs = 6000;
    a.stallTimes = 1;
    g.setAsset(a);
    Flow f(g.env);
    f.start();
    bool sawBackground = false;
    for (int i = 0; i < 400 && !f.finished(); ++i) {
      if (f.snapshot().wentBackground) sawBackground = true;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    f.wait();
    Snapshot s = f.snapshot();
    sawBackground = sawBackground || s.wentBackground;
    note("stalled", s);
    expect(sawBackground, "a stalled download: the splash is told to let Gridline start");
    expect(s.outcome == Outcome::Staged && g.oldInstalled(), "... the download finishes behind it and waits for the next start");
    g.srv.stop();  // and the next start is offline:
    s = g.run();
    note("staged, offline", s);
    expect(s.outcome == Outcome::Relaunch && g.newInstalled(), "the next start installs the checked download at once, even offline");
  }
  {
    Rig g("corrupt");
    testsrv::Route a = g.assetRoute();
    a.corruptAt = 200 * 1024;
    g.setAsset(a);
    Snapshot s = g.run();
    note("corrupt", s);
    const fs::path stage = fs::u8path(ap::stagingDirFor(g.env.install.target));
    expect(s.outcome == Outcome::Failed && g.untouched() && !fs::exists(stage / (g.assetName() + ".part")) && !fs::exists(stage / g.assetName()),
           "a file that does not match its checksum: deleted, never installed");
    expect(!ap::mayTry(g.updates(), "0.9.0", T0 + 60), "... and not tried again at once");
    ap::Failure fl;
    expect(ap::readFailure(g.updates(), fl) && fl.kind == "download" && !fl.noticeShown, "... the notice is due (once)");
  }
  {
    Rig g("bigger");
    testsrv::Route a = g.assetRoute();
    a.body = g.appImageBody + std::string(5000, 'x');
    g.setAsset(a);
    Snapshot s = g.run();
    expect(s.outcome == Outcome::Failed && g.untouched(), "a file bigger than the release says: refused");
  }
  {
    Rig g("lying");
    testsrv::Route a = g.assetRoute();
    a.lieLength = static_cast<long>(g.appImageBody.size());
    a.body = g.appImageBody.substr(0, 120 * 1024);  // claims the whole size, sends part of it
    a.dropAfter = 120 * 1024;
    a.dropTimes = 100;
    g.setAsset(a);
    Snapshot s = g.run();
    expect(s.outcome != Outcome::Relaunch && g.untouched(), "a truncated file (fewer bytes than its length): not installed");
  }
  {
    Rig g("noasset");
    g.publish(true, false);
    Snapshot s = g.run();
    expect(s.outcome == Outcome::NotHere && g.untouched(), "a release without this install's file: left to the popup");
  }
  {
    Rig g("nosums");
    g.publish(false, true);
    Snapshot s = g.run();
    expect(s.outcome == Outcome::Failed && g.untouched(), "a release without SHA256SUMS: refused");
  }
  {
    Rig g("unlisted");
    testsrv::Route sums;
    sums.body = "0000000000000000000000000000000000000000000000000000000000000000  Gridline-0.9.0-windows-x64-setup.exe\n";
    g.srv.set("/dl/" + g.sumsName(), sums);
    Snapshot s = g.run();
    expect(s.outcome == Outcome::Failed && g.untouched(), "a file not in SHA256SUMS: refused");
  }
  // ---- signatures (a build with a release key)
  {
    const std::array<uint8_t, 32> seed = {7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7};
    const std::array<uint8_t, 8> id = {9, 8, 7, 6, 5, 4, 3, 2};
    MinisignKey key;
    key.keyId = id;
    key.pub = ed25519PublicKey(seed);
    {
      Rig g("signed");
      g.env.keys = {key};
      g.addSignature(signMinisign(seed, id, readSums(g), "timestamp:1\tfile:" + g.sumsName() + "\thashed", true));
      Snapshot s = g.run();
      note("signed", s);
      expect(s.outcome == Outcome::Relaunch && g.newInstalled(), "signed release, good signature: installed");
    }
    {
      Rig g("unsigned");
      g.env.keys = {key};
      Snapshot s = g.run();
      expect(s.outcome == Outcome::Failed && g.untouched(), "a build with a key refuses an unsigned release");
    }
    {
      Rig g("badsig");
      g.env.keys = {key};
      g.addSignature(signMinisign(seed, id, readSums(g) + "tampered", "timestamp:1\tfile:" + g.sumsName(), true));
      Snapshot s = g.run();
      expect(s.outcome == Outcome::Failed && g.untouched(), "a signature that does not match SHA256SUMS: refused");
    }
    {
      Rig g("replayed");
      g.env.keys = {key};
      g.addSignature(signMinisign(seed, id, readSums(g), "timestamp:1\tfile:Gridline-0.7.0-SHA256SUMS", true));
      Snapshot s = g.run();
      expect(s.outcome == Outcome::Failed && g.untouched(), "an old release's signature under a new tag: refused");
    }
    {
      Rig g("tampered-both");
      g.env.keys = {key};
      // An attacker changes the AppImage and SHA256SUMS together; the signature is the old one.
      const std::string goodSums = readSums(g);
      g.addSignature(signMinisign(seed, id, goodSums, "timestamp:1\tfile:" + g.sumsName(), false));
      g.appImageBody = fakeProgram("0.9.0") + "# evil";
      testsrv::Route a = g.assetRoute();
      g.setAsset(a);
      testsrv::Route sums;
      sums.body = sha256Hex(g.appImageBody) + "  " + g.assetName() + "\n";
      g.srv.set("/dl/" + g.sumsName(), sums);
      Snapshot s = g.run();
      expect(s.outcome == Outcome::Failed && g.untouched(), "a changed file with a matching changed SHA256SUMS: the signature stops it");
    }
  }
  // ---- the new version must run here
  {
    Rig g("testrun");
    g.appImageBody = fakeProgram("0.9.0", 1);
    g.publish();
    Snapshot s = g.run();
    note("test run fails", s);
    expect(s.outcome == Outcome::Failed && g.untouched() && s.message.find("does not start") != std::string::npos,
           "a new version that does not run on this computer (too old a system, wrong processor): refused before the swap");
  }
  {
    Rig g("wrongversion");
    g.appImageBody = fakeProgram("0.8.5");
    g.publish();
    Snapshot s = g.run();
    expect(s.outcome == Outcome::Failed && g.untouched(), "a file that says it is another version: refused");
  }
  // ---- disk and rights
  if (::geteuid() != 0) {
    Rig g("readonly");
    fs::permissions(g.apps, fs::perms::owner_read | fs::perms::owner_exec);
    Snapshot s = g.run();
    fs::permissions(g.apps, fs::perms::owner_all);
    note("read-only", s);
    expect(s.outcome == Outcome::Failed && g.untouched() && g.srv.count("/dl/" + g.assetName()) == 0,
           "a folder this user can't write to: said before anything is downloaded");
  }
  {
    Rig g("diskfull");
    g.rel.assets[0].size = int64_t(1) << 60;  // claims an impossible size
    testsrv::Route api = g.srv.get("/releases/latest");
    api.body = releaseJson(g.rel);
    g.srv.set("/releases/latest", api);
    Snapshot s = g.run();
    expect(s.outcome == Outcome::Failed && s.message.find("disk space") != std::string::npos && g.untouched(), "not enough disk space: said, nothing downloaded");
  }
  // ---- the user and other Gridlines
  {
    Rig g("cancel");
    testsrv::Route a = g.assetRoute();
    a.chunkDelayMs = 200;
    g.setAsset(a);
    Flow f(g.env);
    f.start();
    for (int i = 0; i < 200 && f.snapshot().got < 32 * 1024; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    f.cancel();
    f.wait();
    Snapshot s = f.snapshot();
    const fs::path part = fs::u8path(ap::stagingDirFor(g.env.install.target)) / (g.assetName() + ".part");
    expect(s.outcome == Outcome::Skipped && g.untouched() && fs::exists(part), "Gridline closed mid-download: stops, keeps the part");
    g.setAsset(g.assetRoute());
    s = g.run();
    expect(s.outcome == Outcome::Relaunch && g.newInstalled(), "the next start resumes and installs");
  }
  {
    Rig g("twogridlines");
    ap::lock(g.updates(), 1, T0, [](int) { return true; });  // another live Gridline is updating
    Snapshot s = g.run();
    expect(s.outcome == Outcome::Skipped && g.untouched(), "two Gridlines at once: only one updates");
    ap::unlock(g.updates());
  }
  {
    Rig g("backoff");
    ap::recordFailure(g.updates(), "0.9.0", ap::FailKind::Install, "earlier", T0 - 60);
    Snapshot s = g.run();
    expect(s.outcome == Outcome::Skipped && g.srv.count("/dl/" + g.assetName()) == 0, "after a failure: not at the very next start (backoff)");
  }
  {
    Rig g("nothere");
    g.env.install.kind = InstallKind::Homebrew;
    Snapshot s = g.run();
    expect(s.outcome == Outcome::NotHere && s.command == "brew upgrade --cask gridline" && g.srv.seen().empty(),
           "a package manager install: no request at all, the command named");
  }
  // ---- a system package with a stand-in password prompt
  {
    for (const char* mode : {"nopkexec", "cancel", "locked", "deps", "ok"}) {  // no Gridline button: the system prompt comes at once
      Rig g(std::string("deb-") + mode);
      g.version = "0.9.0";
      g.env.install.kind = InstallKind::Deb;
      g.env.install.target.clear();
      // The .deb is any file; the stand-in pkexec records its arguments and answers like pkexec.
      g.appImageBody = "a debian package";
      const std::string debName = "Gridline-0.9.0-linux-amd64.deb";
      g.rel = Release{};
      g.rel.tag = "v0.9.0";
      g.rel.version = parseVersion("0.9.0");
      g.rel.assets = {Asset{debName, g.srv.base() + "/dl/" + debName, static_cast<int64_t>(g.appImageBody.size())},
                      Asset{g.sumsName(), g.srv.base() + "/dl/" + g.sumsName(), 0}};
      testsrv::Route api;
      api.body = releaseJson(g.rel);
      g.srv.set("/releases/latest", api);
      testsrv::Route deb;
      deb.body = g.appImageBody;
      g.srv.set("/dl/" + debName, deb);
      testsrv::Route sums;
      sums.body = sha256Hex(g.appImageBody) + "  " + debName + "\n";
      g.srv.set("/dl/" + g.sumsName(), sums);
      const fs::path bin = g.root / "bin";
      const std::string answer = std::string(mode) == "cancel" ? "exit 126"
                                 : std::string(mode) == "locked" ? "echo 'E: Could not get lock /var/lib/dpkg/lock-frontend' >&2; exit 100"
                                 : std::string(mode) == "deps"   ? "echo 'The following packages have unmet dependencies:' >&2; exit 100"
                                                                 : "exit 0";
      // The stand-in prompt takes a moment, as a real one does (the splash shows why it is up).
      if (std::string(mode) != "nopkexec")
        writeFile(bin / "pkexec", "#!/bin/sh\nsleep 0.4\necho \"$@\" > '" + (g.root / "pkexec-args").u8string() + "'\n" + answer + "\n");
      else
        writeFile(bin / "pkexec-not-here", "");
      writeFile(bin / "apt-get", "#!/bin/sh\nexit 0\n");
      if (fs::exists(bin / "pkexec")) fs::permissions(bin / "pkexec", fs::perms::owner_all);
      fs::permissions(bin / "apt-get", fs::perms::owner_all);
      const std::string oldPath = gl::platform::getEnv("PATH");
      gl::platform::setEnv("PATH", (bin.u8string() + ":/usr/bin:/bin").c_str());
      Flow f(g.env);
      f.start();
      bool sawPrompt = false;  // the splash's "your system will ask for your password" state
      for (int i = 0; i < 400 && !f.finished(); ++i) {
        sawPrompt = sawPrompt || f.snapshot().stage == Stage::NeedsPermission;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      f.wait();
      gl::platform::setEnv("PATH", oldPath.c_str());
      Snapshot s = f.snapshot();
      const std::string args = readFile(g.root / "pkexec-args");
      const std::string m = mode;
      bool good = m == "nopkexec" || (sawPrompt && s.needsAdmin);
      if (m == "nopkexec") good = good && s.outcome == Outcome::PermissionLater && args.empty();
      if (m == "cancel")
        good = good && s.outcome == Outcome::PermissionLater && args.find("install -y") != std::string::npos && !ap::mayTry(g.updates(), "0.9.0", T0 + 3600);
      if (m == "locked") good = good && s.outcome == Outcome::Failed && s.message.find("another program") != std::string::npos;
      if (m == "deps") good = good && s.outcome == Outcome::Failed && s.message.find("other software") != std::string::npos;
      if (m == "ok") good = good && s.outcome == Outcome::Relaunch && args.find(debName) != std::string::npos;
      note(mode, s);
      expect(good, (std::string(".deb, password prompt: ") + mode).c_str());
    }
  }
  // ---- a .tar.gz folder (real tar)
  {
    Rig g("tarball");
    const fs::path inst = g.root / "Gridline-0.8.0-linux-x86_64";
    writeFile(inst / "bin" / "gridline-app", fakeProgram("0.8.0"));
    fs::permissions(inst / "bin" / "gridline-app", fs::perms::owner_all);
    writeFile(inst / "install.sh", "#!/bin/sh\n");
    writeFile(inst / "share" / "gridline" / "old-only", "x");
    const fs::path src = g.root / "src" / "Gridline-0.9.0-linux-x86_64";
    writeFile(src / "bin" / "gridline-app", fakeProgram("0.9.0"));
    fs::permissions(src / "bin" / "gridline-app", fs::perms::owner_all);
    writeFile(src / "share" / "gridline" / "new-only", "x");
    const fs::path tgz = g.root / "Gridline-0.9.0-linux-x86_64.tar.gz";
    std::string out;
    systemRunner()({"tar", "-czf", tgz.u8string(), "-C", (g.root / "src").u8string(), "Gridline-0.9.0-linux-x86_64"}, 60, &out);
    const std::string body = readFile(tgz);
    g.env.install.kind = InstallKind::TarballFolder;
    g.env.install.target = inst.u8string();
    const std::string name = "Gridline-0.9.0-linux-x86_64.tar.gz";
    g.rel = Release{};
    g.rel.tag = "v0.9.0";
    g.rel.version = parseVersion("0.9.0");
    g.rel.assets = {Asset{name, g.srv.base() + "/dl/" + name, static_cast<int64_t>(body.size())},
                    Asset{g.sumsName(), g.srv.base() + "/dl/" + g.sumsName(), 0}};
    testsrv::Route api;
    api.body = releaseJson(g.rel);
    g.srv.set("/releases/latest", api);
    testsrv::Route t;
    t.body = body;
    g.srv.set("/dl/" + name, t);
    testsrv::Route sums;
    sums.body = sha256Hex(body) + "  " + name + "\n";
    g.srv.set("/dl/" + g.sumsName(), sums);
    Snapshot s = g.run();
    note("tarball", s);
    expect(s.outcome == Outcome::Relaunch && readFile(inst / "bin" / "gridline-app").find("Gridline 0.9.0") != std::string::npos &&
               fs::exists(inst / "share" / "gridline" / "new-only") && !fs::exists(inst / "share" / "gridline" / "old-only"),
           ".tar.gz folder: unpacked next to it, test-run and swapped as a whole");
  }
#if defined(__APPLE__)
  // ---- macOS: a real disk image with a stand-in app (ad hoc signed), swapped into a sandbox Applications
  auto makeApp = [](const fs::path& app, const std::string& v, const std::string& id, const std::string& minOs) {
    writeFile(app / "Contents" / "MacOS" / "gridline-app", fakeProgram(v));
    fs::permissions(app / "Contents" / "MacOS" / "gridline-app", fs::perms::owner_all);
    writeFile(app / "Contents" / "Info.plist",
              "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
              "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\"><dict>"
              "<key>CFBundleIdentifier</key><string>" + id + "</string><key>CFBundleExecutable</key><string>gridline-app</string>"
              "<key>CFBundleShortVersionString</key><string>" + v + "</string><key>LSMinimumSystemVersion</key><string>" + minOs +
              "</string></dict></plist>\n");
    std::string out;
    return systemRunner()({"codesign", "--force", "--sign", "-", app.u8string()}, 60, &out) == 0;
  };
  for (const char* mode : {"ok", "newer-os", "other-app", "tampered"}) {
    Rig g(std::string("mac-") + mode);
    const std::string m = mode;
    const fs::path inst = g.apps / "Gridline.app";
    makeApp(inst, "0.8.0", "com.lolitemaultes.Gridline", "11.0");
    const fs::path src = g.root / "dmgsrc";
    const bool signedOk = makeApp(src / "Gridline.app", "0.9.0", m == "other-app" ? "com.example.Other" : "com.lolitemaultes.Gridline",
                                  m == "newer-os" ? "99.0" : "11.0");
    if (m == "tampered") writeFile(src / "Gridline.app" / "Contents" / "MacOS" / "gridline-app", fakeProgram("0.9.0") + "# changed after signing");
    const fs::path dmg = g.root / "Gridline-0.9.0-macos-universal.dmg";
    std::string out;
    const int made = systemRunner()({"hdiutil", "create", "-quiet", "-fs", "HFS+", "-srcfolder", src.u8string(), "-volname", "Gridline", "-format", "UDZO",
                                     dmg.u8string()},
                                    120, &out);
    if (made != 0 || !signedOk) {
      std::printf("# hdiutil/codesign not usable here (%d): %s\n", made, out.c_str());
      expect(true, (std::string("macOS disk image (skipped): ") + mode).c_str());
      continue;
    }
    const std::string body = readFile(dmg);
    const std::string name = "Gridline-0.9.0-macos-universal.dmg";
    g.env.install.kind = InstallKind::MacApp;
    g.env.install.target = inst.u8string();
    g.env.target = Target("macos", "aarch64");
    g.env.bundleId = "com.lolitemaultes.Gridline";
    g.env.osVersion = "14.6";
    g.rel = Release{};
    g.rel.tag = "v0.9.0";
    g.rel.version = parseVersion("0.9.0");
    g.rel.assets = {Asset{name, g.srv.base() + "/dl/" + name, static_cast<int64_t>(body.size())},
                    Asset{g.sumsName(), g.srv.base() + "/dl/" + g.sumsName(), 0}};
    testsrv::Route api;
    api.body = releaseJson(g.rel);
    g.srv.set("/releases/latest", api);
    testsrv::Route d;
    d.body = body;
    g.srv.set("/dl/" + name, d);
    testsrv::Route sums;
    sums.body = sha256Hex(body) + "  " + name + "\n";
    g.srv.set("/dl/" + g.sumsName(), sums);
    Snapshot s = g.run();
    note(mode, s);
    const std::string now = readFile(inst / "Contents" / "MacOS" / "gridline-app");
    if (m == "ok")
      expect(s.outcome == Outcome::Relaunch && now.find("Gridline 0.9.0") != std::string::npos, "macOS: the app from the disk image, checked and swapped in");
    if (m == "newer-os")
      expect(s.outcome == Outcome::Failed && s.message.find("needs macOS 99.0") != std::string::npos && now.find("Gridline 0.8.0") != std::string::npos,
             "macOS: a version that needs a newer macOS is refused with the reason");
    if (m == "other-app")
      expect(s.outcome == Outcome::Failed && now.find("Gridline 0.8.0") != std::string::npos, "macOS: an app that is not Gridline is refused");
    if (m == "tampered")
      expect(s.outcome == Outcome::Failed && s.message.find("signature") != std::string::npos && now.find("Gridline 0.8.0") != std::string::npos,
             "macOS: an app whose code signature is broken is refused");
    std::error_code ec;
    expect(!fs::exists(fs::u8path(ap::stagingDirFor(inst.u8string())) / "mount" / "Gridline.app", ec), (std::string("macOS: the disk image is detached: ") + mode).c_str());
  }
#endif
  std::error_code ec;
  fs::remove_all(gRoot, ec);
  return finish();
}
#endif
