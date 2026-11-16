// Gridline - K-308: the update check shared by the app and the plugins (core/UpdateShared.h) and
// the plugin installer picked from a release (update::pickSuiteAsset).
#include <filesystem>
#include <map>
#include <string>
#include "Check.h"
#include "Hermetic.h"
#include "core/Update.h"
#include "core/UpdateShared.h"

using namespace gl::update;
namespace sh = gl::update::shared;
namespace fs = std::filesystem;

namespace {
void expect(bool ok, const std::string& name) { expect(ok, name.c_str()); }
sh::Env envOf(const std::map<std::string, std::string>& m) {
  return [m](const char* k) -> const char* {
    auto it = m.find(k);
    return it == m.end() ? nullptr : it->second.c_str();
  };
}
Release releaseWith(const std::vector<std::string>& names, const std::string& tag = "v0.8.0") {
  Release r;
  r.tag = tag;
  r.version = parseVersion(tag);
  for (const auto& n : names) r.assets.push_back({n, "https://example.invalid/" + n, 1});
  return r;
}
}  // namespace

int main() {
  const std::string root = gl::test::hermeticTestEnv("k308_update_shared");

  // ---- where the state lives
  expect(sh::configDir(envOf({{"GRIDLINE_CONFIG_HOME", "/x/cfg"}, {"HOME", "/h"}}), false) == fs::u8path("/x/cfg/gridline").u8string(),
         "the config root override wins (as the app's settings)");
  expect(sh::configDir(envOf({{"XDG_CONFIG_HOME", "/x"}, {"HOME", "/h"}}), false) == fs::u8path("/x/gridline").u8string(), "XDG config folder");
  expect(sh::configDir(envOf({{"HOME", "/h"}}), false) == fs::u8path("/h/.config/gridline").u8string(), "~/.config/gridline");
  expect(sh::configDir(envOf({{"APPDATA", "C:/Users/me/AppData/Roaming"}}), true).find("gridline") != std::string::npos, "%APPDATA%/gridline");
  expect(sh::configDir(envOf({}), false).empty(), "no home: no folder (no check)");

  // ---- turned off
  for (const char* k : {"GRIDLINE_NO_UPDATE_CHECK", "CI", "GRIDLINE_PLUGIN_SHOT", "GRIDLINE_PLUGIN_WINDOW_SHOT", "GRIDLINE_PLUGIN_OPEN_HOSTED"})
    expect(sh::disabledByEnv(envOf({{k, "1"}})), std::string("off with ") + k);
  expect(!sh::disabledByEnv(envOf({{"CI", ""}})), "an empty variable does not count");

  // ---- the state file round trip
  const std::string dir = root + "/state";
  sh::State s;
  expect(!sh::read(dir, s), "no file yet");
  s.lastAttempt = 1000, s.lastSuccess = 1001, s.latest = "0.8.1", s.page = "https://example.invalid/p", s.suiteUrl = "u1",
  s.miniUrl = "u2", s.skipVersion = "0.8.0", s.remindAfter = 5000, s.pluginsAuto = false, s.beta = true;
  expect(sh::write(dir, s), "the state is written");
  sh::State back;
  expect(sh::read(dir, back) && back == s, "and read back the same");
  expect(!fs::exists(fs::u8path(dir) / "update-check.json.tmp"), "no temporary file is left");

  // ---- once a day
  const auto none = envOf({});
  sh::State d;
  expect(sh::decide(d, 100000, none) == sh::Decision::Fetch, "never checked: fetch");
  d.lastAttempt = 100000;
  expect(sh::decide(d, 100000 + 3600, none) == sh::Decision::Cached, "an hour later: the cached answer");
  expect(sh::decide(d, 100000 + sh::kDay + 1, none) == sh::Decision::Fetch, "a day later: fetch again");
  expect(sh::decide(d, 50000, none) == sh::Decision::Fetch, "a clock set back: fetch");
  d.pluginsAuto = false;
  expect(sh::decide(d, 900000, none) == sh::Decision::Off, "opted out: off");
  d.pluginsAuto = true;
  expect(sh::decide(d, 900000, envOf({{"GRIDLINE_NO_UPDATE_CHECK", "1"}})) == sh::Decision::Off, "the variable turns it off");

  // ---- one request between many processes
  const std::string ldir = root + "/lock";
  expect(sh::claim(ldir, 1000), "the first claim wins");
  expect(!sh::claim(ldir, 1010), "a second claim waits for the first");
  expect(!sh::claim(ldir, 1000 + sh::kStaleLock), "... still within five minutes");
  expect(sh::claim(ldir, 1000 + sh::kStaleLock + 1), "a stale lock is taken over");
  sh::release(ldir);
  expect(!fs::exists(fs::u8path(ldir) / "update-check.lock"), "release removes the lock");
  expect(sh::claim(ldir, 2000), "and the next claim wins");
  sh::release(ldir);
  int leftovers = 0;
  for (const auto& e : fs::directory_iterator(fs::u8path(ldir))) leftovers += e.path().filename().u8string().rfind("update-check.lock.", 0) == 0;
  expect(leftovers == 0, "no private lock files are left");

  // ---- the notice
  sh::State n;
  n.latest = "0.8.1";
  expect(sh::shouldShow(n, "0.8.0", 10), "a newer version shows");
  expect(!sh::shouldShow(n, "0.8.1", 10) && !sh::shouldShow(n, "0.9.0", 10), "the same or an older one does not");
  n.skipVersion = "0.8.1";
  expect(!sh::shouldShow(n, "0.8.0", 10), "a skipped version does not");
  n.skipVersion.clear(), n.remindAfter = 100;
  expect(!sh::shouldShow(n, "0.8.0", 50) && sh::shouldShow(n, "0.8.0", 150), "remind me later waits");
  n.latest.clear();
  expect(!sh::shouldShow(n, "0.8.0", 150), "nothing found: nothing shows");

  // ---- the plugin installers in a release
  const Release r = releaseWith({"Gridline-0.8.0-macos-universal.dmg", "Gridline-0.8.0-windows-x64-setup.exe", "Gridline-0.8.0-linux-x86_64.AppImage",
                                 "Gridline-Suite-0.8.0-macos-universal.pkg", "Gridline-Suite-Mini-0.8.0-macos-universal.pkg",
                                 "Gridline-Suite-0.8.0-windows-setup.exe", "Gridline-Suite-Mini-0.8.0-windows-setup.exe",
                                 "Gridline-Suite-0.8.0-linux-x86_64-setup.run", "Gridline-Suite-0.8.0-linux-aarch64-setup.run",
                                 "Gridline-Suite-Mini-0.8.0-freebsd-x86_64-setup.run", "Gridline-Suite-Plasma-0.8.0-macos.zip",
                                 "Gridline-Suite-0.8.0-macos-all.zip"});
  auto name = [](const Asset* a) { return a ? a->name : std::string("-"); };
  expect(name(pickSuiteAsset(r, Target{"macos", "aarch64"}, false)) == "Gridline-Suite-0.8.0-macos-universal.pkg", "macOS: the Suite pkg");
  expect(name(pickSuiteAsset(r, Target{"macos", "x86_64"}, true)) == "Gridline-Suite-Mini-0.8.0-macos-universal.pkg", "macOS: the Mini pkg");
  expect(name(pickSuiteAsset(r, Target{"windows", "aarch64"}, false)) == "Gridline-Suite-0.8.0-windows-setup.exe", "Windows: one setup");
  expect(name(pickSuiteAsset(r, Target{"windows", "x86_64"}, true)) == "Gridline-Suite-Mini-0.8.0-windows-setup.exe", "Windows: Mini setup");
  expect(name(pickSuiteAsset(r, Target{"linux", "aarch64", "deb"}, false)) == "Gridline-Suite-0.8.0-linux-aarch64-setup.run", "Linux arm: its .run");
  expect(pickSuiteAsset(r, Target{"linux", "x86_64"}, true) == nullptr, "no Mini .run for Linux in this release: the page");
  expect(name(pickSuiteAsset(r, Target{"freebsd", "x86_64"}, true)) == "Gridline-Suite-Mini-0.8.0-freebsd-x86_64-setup.run", "FreeBSD Mini");
  expect(pickSuiteAsset(r, Target{"freebsd", "x86_64"}, false) == nullptr, "the Suite never takes the Mini file");
  expect(name(pickAsset(r, Target{"macos", "aarch64"})) == "Gridline-0.8.0-macos-universal.dmg", "the app's own pick is unchanged");
  expect(name(pickAsset(r, Target{"windows", "x86_64"})) == "Gridline-0.8.0-windows-x64-setup.exe", "the app never takes a suite setup");

  // ---- one check, as a plugin makes it (fake fetcher: no network)
  {
    const std::string cdir = root + "/check";
    int calls = 0;
    const std::string json = R"({"tag_name":"v0.8.1","name":"Gridline 0.8.1","html_url":"https://example.invalid/rel","draft":false,"prerelease":false,
      "assets":[{"name":"Gridline-Suite-0.8.1-macos-universal.pkg","browser_download_url":"https://example.invalid/s.pkg","size":5},
                {"name":"Gridline-Suite-Mini-0.8.1-macos-universal.pkg","browser_download_url":"https://example.invalid/m.pkg","size":5}]})";
    Fetcher fake = [&](const std::string&) {
      ++calls;
      HttpResult h;
      h.status = 200;
      h.body = json;
      return h;
    };
    const Target mac{"macos", "aarch64"};
    sh::State r = sh::runCheck(cdir, 1e6, none, fake, "0.8.0", mac);
    expect(calls == 1 && r.latest == "0.8.1" && r.suiteUrl == "https://example.invalid/s.pkg" && r.miniUrl == "https://example.invalid/m.pkg",
           "a check records the newer version and this system's installers");
    expect(r.lastAttempt == 1e6 && r.lastSuccess == 1e6 && sh::shouldShow(r, "0.8.0", 1e6 + 1), "... and the notice shows");
    r = sh::runCheck(cdir, 1e6 + 3600, none, fake, "0.8.0", mac);
    expect(calls == 1 && r.latest == "0.8.1", "an hour later: the cached answer, no request");
    expect(!fs::exists(fs::u8path(cdir) / "update-check.lock"), "the claim is released");
    sh::claim(cdir, 1e6 + 2 * sh::kDay);
    r = sh::runCheck(cdir, 1e6 + 2 * sh::kDay + 1, none, fake, "0.8.0", mac);
    expect(calls == 1, "another process holds the claim: no second request");
    sh::release(cdir);
    r = sh::runCheck(cdir, 1e6 + 2 * sh::kDay + 2, envOf({{"CI", "1"}}), fake, "0.8.0", mac);
    expect(calls == 1, "turned off: no request");
    Fetcher offline = [&](const std::string&) {
      ++calls;
      HttpResult h;
      h.offline = true;
      return h;
    };
    r = sh::runCheck(cdir, 1e6 + 2 * sh::kDay + 3, none, offline, "0.8.0", mac);
    expect(calls == 2 && r.latest == "0.8.1" && r.lastAttempt == 1e6 + 2 * sh::kDay + 3 && r.lastSuccess == 1e6,
           "offline: the old answer stays, the attempt counts for the day");
    r = sh::runCheck(cdir, 1e6 + 2 * sh::kDay + 4, none, fake, "0.8.0", mac, true);
    expect(calls == 3, "Check now asks even within the day");
  }
  return finish();
}
