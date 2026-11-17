// Gridline - K-327: item walks never reach the secret store, and the config folder follows HOME.
//   1. secrets::NoAccess (the command palette's and the menu tree's walks): get / set / remove /
//      migrate fail at once and change nothing; nests; other threads are not affected.
//   2. The collaboration network settings are built without reading the TURN password; it is read
//      only when a session starts (once per run), never inside a walk, and the welcome screen's
//      probe uses only a password already read.
//   3. The config root follows HOME when neither GRIDLINE_CONFIG_HOME nor XDG_CONFIG_HOME is set
//      (Windows: APPDATA first, as before), and GRIDLINE_CONFIG_HOME still wins.
#include <atomic>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include "Check.h"
#include "Hermetic.h"
#include "collab/Controller.h"
#include "core/Platform.h"
#include "core/Secrets.h"
#include "core/Settings.h"

using namespace gl;
namespace fs = std::filesystem;

namespace {

void testNoAccess() {
  const fs::path base = fs::path(test::hermeticTestEnv("k327"));
  secrets::Store st;
  st.backend = secrets::Backend::File;
  st.dir = (base / "secrets-a").string();
  expect(st.set("mcp-token", "token-0123456789abcdef"), "store: set outside a walk");
  expect(!secrets::accessBlocked(), "no walk: access allowed");
  const std::string settingsPath = (base / "walk-settings.json").string();
  {
    secrets::NoAccess walk;
    expect(secrets::accessBlocked(), "walk: access blocked");
    const int before = secrets::blockedAttempts();
    std::string v;
    expect(!st.get("mcp-token", v) && v.empty(), "walk: get reads nothing");
    expect(!st.set("other", "x"), "walk: set writes nothing");
    expect(!st.remove("mcp-token"), "walk: remove removes nothing");
    {
      secrets::NoAccess nested;
      expect(secrets::accessBlocked(), "walk: nested");
    }
    expect(secrets::accessBlocked(), "walk: still blocked after a nested one ends");
    Settings s;
    s.set("mcp.token", std::string("legacy-token-0123456789"));
    int generated = 0;
    bool stored = true;
    const std::string t = secrets::migrate(s, "mcp.token", settingsPath, st, "mcp-token", [&] {
      ++generated;
      return std::string("generated-xxxxxxxx");
    }, &stored);
    expect(t.empty() && !stored && generated == 0, "walk: migrate reads, makes and stores nothing");
    expect(s.getString("mcp.token") == "legacy-token-0123456789" && !fs::exists(settingsPath), "walk: migrate leaves the settings alone");
    expect(secrets::blockedAttempts() >= before + 4, "walk: the attempts are counted");
    std::atomic<bool> otherThread{false};
    std::thread th([&] {
      std::string v2;
      otherThread = !secrets::accessBlocked() && st.get("mcp-token", v2) && v2 == "token-0123456789abcdef";
    });
    th.join();
    expect(otherThread.load(), "walk: another thread still reads its secrets");
  }
  expect(!secrets::accessBlocked(), "walk over: access allowed again");
  std::string v;
  expect(st.get("mcp-token", v) && v == "token-0123456789abcdef", "walk over: the secret is still there");
  expect(!st.get("other", v), "walk over: nothing was written during the walk");
}

void testTurnPassword() {
  collab::forgetTurnPassword();
  secrets::Store store;  // the hermetic config's secret files
  expect(store.backend == secrets::Backend::File, "turn: tests keep secrets in files");
  expect(store.set("collab-turn-password", "pw-1"), "turn: password stored");
  Settings s;
  s.set("collab.turnHost", std::string("turn.example.org"));
  s.set("collab.turnUser", std::string("me"));
  const collab::NetSettings n = collab::netSettingsFrom(s);
  expect(n.turnHost == "turn.example.org" && n.turnUser == "me" && n.turnPass.empty(), "turn: building the settings reads no secret");
  {
    secrets::NoAccess walk;
    collab::NetSettings m = n;
    expect(!collab::fillTurnPassword(m, true) && m.turnPass.empty(), "turn: never read inside a walk");
  }
  {
    collab::NetSettings m = n;
    expect(!collab::fillTurnPassword(m, false) && m.turnPass.empty(), "turn: the probe does not read the store");
  }
  {
    collab::NetSettings m = n;
    expect(collab::fillTurnPassword(m, true) && m.turnPass == "pw-1", "turn: a session start reads it");
  }
  expect(store.set("collab-turn-password", "pw-2"), "turn: changed outside Gridline");
  {
    collab::NetSettings m = n;
    expect(collab::fillTurnPassword(m, false) && m.turnPass == "pw-1", "turn: read once per run, then known (the probe gets it)");
  }
  collab::rememberTurnPassword("pw-3");
  {
    collab::NetSettings m = n;
    expect(collab::fillTurnPassword(m, true) && m.turnPass == "pw-3", "turn: a password typed on the page is used");
  }
  {
    Settings none;
    collab::NetSettings m = collab::netSettingsFrom(none);
    expect(collab::fillTurnPassword(m, false) && m.turnPass.empty(), "turn: no TURN server, nothing needed");
  }
  collab::forgetTurnPassword();
}

// A fixed environment for configRootFor.
struct Env {
  std::map<std::string, std::string> v;
  std::string operator()(const char* n) const {
    const auto it = v.find(n);
    return it == v.end() ? std::string() : it->second;
  }
};
fs::path norm(const std::string& p) { return fs::u8path(p).lexically_normal(); }

void testConfigRootFor() {
  Env e;
  e.v["HOME"] = "/scratch/hm";
  expect(norm(platform::configRootFor(e, false)) == norm("/scratch/hm/.config"), "config: macOS / Linux follow HOME");
  e.v["XDG_CONFIG_HOME"] = "/xdg";
  expect(platform::configRootFor(e, false) == "/xdg", "config: XDG_CONFIG_HOME before HOME");
  e.v["GRIDLINE_CONFIG_HOME"] = "/isolated";
  expect(platform::configRootFor(e, false) == "/isolated", "config: GRIDLINE_CONFIG_HOME first");
  expect(platform::configRootFor(e, false, "/data") == "/isolated", "config: GRIDLINE_CONFIG_HOME before the data folder");
  e.v.erase("GRIDLINE_CONFIG_HOME");
  e.v.erase("XDG_CONFIG_HOME");
  expect(norm(platform::configRootFor(e, false, "/data")) == norm("/data/config"), "config: the data folder before HOME");
  e.v["HOME"] = "/other/hm";
  expect(norm(platform::configRootFor(e, false)) == norm("/other/hm/.config"), "config: another HOME, another folder");
  Env w;  // Windows: APPDATA, then the home folder
  w.v["APPDATA"] = "C:/Users/u/AppData/Roaming";
  w.v["HOME"] = "C:/scratch";
  expect(platform::configRootFor(w, true) == "C:/Users/u/AppData/Roaming", "config: Windows uses APPDATA");
  w.v.erase("APPDATA");
  expect(norm(platform::configRootFor(w, true)) == norm("C:/scratch/.config"), "config: Windows without APPDATA follows HOME");
  w.v.erase("HOME");
  w.v["USERPROFILE"] = "C:/Users/u";
  expect(norm(platform::configRootFor(w, true)) == norm("C:/Users/u/.config"), "config: Windows falls back to USERPROFILE");
  w.v["GRIDLINE_CONFIG_HOME"] = "C:/isolated";
  expect(platform::configRootFor(w, true) == "C:/isolated", "config: Windows, GRIDLINE_CONFIG_HOME first");
}

// The process's own folders, with HOME at a scratch folder and neither GRIDLINE_CONFIG_HOME nor
// XDG_CONFIG_HOME set (before anything reads HOME, which is read once per process). Nothing is written.
void testProcessFollowsHome(const fs::path& scratchHome) {
  platform::setEnv("HOME", scratchHome.u8string().c_str());
  platform::unsetEnv("GRIDLINE_CONFIG_HOME");
  platform::unsetEnv("XDG_CONFIG_HOME");
  const fs::path want = (scratchHome / ".config").lexically_normal();
  expect(norm(platform::configRoot()) == want, "config: this process's config root follows HOME");
  expect(norm(Settings::defaultPath()) == (want / "gridline" / "settings.json").lexically_normal(), "config: settings.json under HOME");
  const std::string isolated = (scratchHome / "isolated").u8string();
  platform::setEnv("GRIDLINE_CONFIG_HOME", isolated.c_str());
  expect(norm(platform::configRoot()) == norm(isolated), "config: GRIDLINE_CONFIG_HOME still wins over HOME");
#if defined(__APPLE__)
  platform::unsetEnv("GRIDLINE_CONFIG_HOME");
  platform::unsetEnv("GRIDLINE_SECRET_STORE");
  expect(secrets::defaultBackend() == secrets::Backend::File, "secrets: a scratch HOME keeps the Keychain out too");
#endif
}

}  // namespace

int main() {
  testConfigRootFor();
  {
    std::error_code ec;
    const fs::path home = fs::temp_directory_path(ec) / ("gridline-k327-home-" + std::to_string(platform::processId()));
    fs::create_directories(home, ec);
    testProcessFollowsHome(home);
    fs::remove_all(home, ec);
  }
  test::hermeticTestEnv("k327");  // sets GRIDLINE_CONFIG_HOME, XDG_CONFIG_HOME and the file secret store again
  testNoAccess();
  testTurnPassword();
  return finish();
}
