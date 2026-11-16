// Gridline - K-309 (with K-308): the plugins' update notice offers the right installer. The release's
// installer names come from tools/release/suite-installer.py (--print-names); for every system the
// notice of a Gridline Suite plugin must open the Suite installer of that system, and a Suite Mini
// plugin's the Mini one, among the full list of app and plugin downloads.
#include <cstdio>
#include <string>
#include <vector>
#include "Check.h"
#include "core/Update.h"

using namespace gl;

namespace {
void expect(bool ok, const std::string& name) { expect(ok, name.c_str()); }

std::vector<std::string> installerNames(const std::string& version) {
  std::vector<std::string> out;
  const std::string cmd = std::string("\"") + GRIDLINE_PYTHON + "\" -I \"" + GRIDLINE_SUITE_INSTALLER + "\" --print-names " + version;
  FILE* p = popen(cmd.c_str(), "r");
  if (!p) return out;
  char line[512];
  while (std::fgets(line, sizeof line, p)) {
    std::string s(line);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    if (!s.empty()) out.push_back(s);
  }
  pclose(p);
  return out;
}
}  // namespace

int main() {
  const std::string v = "0.9.0";
  const std::vector<std::string> names = installerNames(v);
  expect(names.size() == 14, "seven installers per suite: macOS, Windows, two Linux processors, three BSD systems (" + std::to_string(names.size()) + ")");
  update::Release r;
  r.tag = "v" + v;
  r.version = update::parseVersion(v);
  for (const std::string& n : names) r.assets.push_back({n, "https://example.invalid/" + n, 1});
  // The rest of the release, which must never be picked for a plugin's notice.
  for (const char* other : {"Gridline-0.9.0-macos-universal.dmg", "Gridline-0.9.0-windows-x64-setup.exe", "Gridline-0.9.0-windows-arm64-setup.exe",
                            "Gridline-0.9.0-linux-x86_64.AppImage", "Gridline-0.9.0-freebsd-x86_64.tar.gz", "Gridline-Suite-0.9.0-macos-all.zip",
                            "Gridline-Suite-Plasma-0.9.0-macos.zip", "Gridline-Suite-Mini-Trim-0.9.0-linux.zip", "Gridline-0.9.0-SHA256SUMS"})
    r.assets.push_back({other, std::string("https://example.invalid/") + other, 1});
  struct Case {
    update::Target t;
    std::string suite, mini;
  };
  const std::vector<Case> cases = {
      {{"macos", "aarch64"}, "Gridline-Suite-0.9.0-macos-universal.pkg", "Gridline-Suite-Mini-0.9.0-macos-universal.pkg"},
      {{"macos", "x86_64"}, "Gridline-Suite-0.9.0-macos-universal.pkg", "Gridline-Suite-Mini-0.9.0-macos-universal.pkg"},
      {{"windows", "x86_64"}, "Gridline-Suite-0.9.0-windows-setup.exe", "Gridline-Suite-Mini-0.9.0-windows-setup.exe"},
      {{"windows", "aarch64"}, "Gridline-Suite-0.9.0-windows-setup.exe", "Gridline-Suite-Mini-0.9.0-windows-setup.exe"},
      {{"linux", "x86_64", "deb"}, "Gridline-Suite-0.9.0-linux-x86_64-setup.run", "Gridline-Suite-Mini-0.9.0-linux-x86_64-setup.run"},
      {{"linux", "aarch64"}, "Gridline-Suite-0.9.0-linux-aarch64-setup.run", "Gridline-Suite-Mini-0.9.0-linux-aarch64-setup.run"},
      {{"freebsd", "x86_64"}, "Gridline-Suite-0.9.0-freebsd-x86_64-setup.run", "Gridline-Suite-Mini-0.9.0-freebsd-x86_64-setup.run"},
      {{"openbsd", "x86_64"}, "Gridline-Suite-0.9.0-openbsd-x86_64-setup.run", "Gridline-Suite-Mini-0.9.0-openbsd-x86_64-setup.run"},
      {{"netbsd", "x86_64"}, "Gridline-Suite-0.9.0-netbsd-x86_64-setup.run", "Gridline-Suite-Mini-0.9.0-netbsd-x86_64-setup.run"},
  };
  for (const Case& c : cases) {
    const update::Asset* s = update::pickSuiteAsset(r, c.t, false);
    const update::Asset* m = update::pickSuiteAsset(r, c.t, true);
    const std::string label = c.t.os + " " + c.t.arch + ": ";
    expect(s && s->name == c.suite, label + "a Suite plugin offers " + c.suite + " (got " + (s ? s->name : "none") + ")");
    expect(m && m->name == c.mini, label + "a Mini plugin offers " + c.mini + " (got " + (m ? m->name : "none") + ")");
  }
  expect(update::pickSuiteAsset(r, {"linux", "riscv64"}, true) == nullptr, "no installer for a processor without one");
  return finish();
}
