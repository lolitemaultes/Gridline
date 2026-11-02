// K-14: the application icon everywhere. The icon built into the program decodes; the names that
// tie a running window to its launcher agree across the desktop entry, AppStream metadata, MIME
// type, macOS bundle and the window class the program sets; every icon file the packages install
// exists at its size; the Windows .ico holds every size the shell asks for.
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

#include "Check.h"
#include "app/AppIconImage.h"

using namespace gl;

namespace {
std::string readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}
std::string desktopKey(const std::string& text, const std::string& key) {
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line))
    if (line.rfind(key + "=", 0) == 0) return line.substr(key.size() + 1);
  return "";
}
bool has(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }
std::uint32_t be32(const std::string& s, std::size_t at) {
  return (static_cast<std::uint32_t>(static_cast<unsigned char>(s[at])) << 24) | (static_cast<std::uint32_t>(static_cast<unsigned char>(s[at + 1])) << 16) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(s[at + 2])) << 8) | static_cast<std::uint32_t>(static_cast<unsigned char>(s[at + 3]));
}
int pngSide(const std::string& png) {  // width of a square PNG, 0 when not a PNG
  if (png.size() < 24 || png.compare(1, 3, "PNG") != 0 || png.compare(12, 4, "IHDR") != 0) return 0;
  const std::uint32_t w = be32(png, 16), h = be32(png, 20);
  return w == h ? static_cast<int>(w) : -1;
}
}  // namespace

int main() {
  // ---- the embedded icon ----
  int n = 0;
  appicon::embeddedPngs(&n);
  expect(n >= 2, "two embedded icon sizes");
  const appicon::Image big = appicon::decode(256);
  expect(big.w == 256 && big.h == 256 && big.rgba.size() == 256u * 256u * 4u, "256 px icon decodes");
  if (big.w == 256) {
    const auto alpha = [&](int x, int y) { return big.rgba[(static_cast<std::size_t>(y) * 256 + static_cast<std::size_t>(x)) * 4 + 3]; };
    expect(alpha(0, 0) == 0 && alpha(255, 0) == 0, "corners transparent");
    expect(alpha(128, 128) == 255 && alpha(60, 128) == 255, "tile opaque");
  }
  expect(appicon::decode(512).w == 512, "512 px icon (macOS Dock outside a bundle)");
  expect(appicon::decode(16).w == 256, "a small request gets the smallest embedded size above it");
  expect(appicon::decode(4096).w == 512, "a large request gets the largest");

  // ---- Linux / BSD: desktop entry, window class, AppStream, MIME ----
  const std::string desktop = readFile(std::string("assets/linux/") + appicon::kDesktopId + ".desktop");
  expect(!desktop.empty(), "the desktop entry is named after the window class (gridline.desktop)");
  expect(desktopKey(desktop, "Icon") == appicon::kIconName, "desktop Icon= is the icon name");
  expect(desktopKey(desktop, "StartupWMClass") == appicon::kDesktopId, "desktop StartupWMClass= is the window class");
  expect(desktopKey(desktop, "Exec").rfind("gridline-app", 0) == 0, "desktop Exec= starts gridline-app");
  const std::string meta = readFile(std::string("assets/linux/") + appicon::kAppStreamId + ".metainfo.xml");
  expect(has(meta, std::string("<id>") + appicon::kAppStreamId + "</id>"), "AppStream id (the Flatpak app id)");
  expect(has(meta, std::string("<replaces>\n    <id>") + appicon::kAppId + "</id>"), "AppStream replaces the old id");
  expect(has(meta, std::string("<launchable type=\"desktop-id\">") + appicon::kDesktopId + ".desktop</launchable>"), "AppStream launchable");
  expect(has(meta, std::string("<icon type=\"stock\">") + appicon::kIconName + "</icon>"), "AppStream stock icon");
  expect(has(readFile("assets/linux/gridline.xml"), std::string("<icon name=\"") + appicon::kIconName + "\"/>"), "MIME type icon");
  const std::string cmake = readFile("CMakeLists.txt");
  for (int sz : {16, 24, 32, 48, 64, 96, 128, 256, 512}) {
    const std::string f = "assets/icons/gridline-app-" + std::to_string(sz) + ".png";
    expect(pngSide(readFile(f)) == sz, ("hicolor " + std::to_string(sz) + " px icon").c_str());
  }
  expect(has(cmake, "foreach(sz 16 24 32 48 64 96 128 256 512)"), "CMake installs every hicolor size");
  const std::string svg = readFile("assets/icons/gridline-app.svg");
  expect(has(svg, "<svg") && has(svg, "viewBox=\"0 0 1024 1024\"") && has(svg, "</svg>"), "scalable icon");
  expect(has(cmake, "assets/icons/gridline-app.svg DESTINATION share/icons/hicolor/scalable/apps RENAME gridline.svg"), "CMake installs the scalable icon");

  // ---- macOS: bundle icon and identifier ----
  expect(has(cmake, std::string("<key>CFBundleIdentifier</key><string>") + appicon::kAppId + "</string>"), "bundle identifier");
  expect(has(cmake, "<key>CFBundleIconFile</key><string>Gridline</string>"), "bundle icon file");
  const std::string icns = readFile("assets/icons/Gridline.icns");
  std::set<std::string> types;
  for (std::size_t i = 8; icns.size() >= 8 && i + 8 <= icns.size();) {
    types.insert(icns.substr(i, 4));
    const std::uint32_t len = be32(icns, i + 4);
    if (len < 8) break;
    i += len;
  }
  for (const char* t : {"ic04", "ic05", "ic07", "ic08", "ic09", "ic10", "ic11", "ic12", "ic13", "ic14"})
    expect(types.count(t) == 1, (std::string(".icns has ") + t).c_str());

  // ---- Windows: every size the shell asks for (100 to 250 % small and large icons) ----
  const std::string ico = readFile("assets/icons/Gridline.ico");
  std::set<int> sizes;
  if (ico.size() >= 6) {
    const int count = static_cast<unsigned char>(ico[4]) | (static_cast<unsigned char>(ico[5]) << 8);
    for (int i = 0; i < count && 6 + 16 * static_cast<std::size_t>(i) + 16 <= ico.size(); ++i) {
      const int w = static_cast<unsigned char>(ico[6 + 16 * static_cast<std::size_t>(i)]);
      sizes.insert(w == 0 ? 256 : w);
    }
  }
  for (int sz : {16, 20, 24, 32, 40, 48, 64, 256}) expect(sizes.count(sz) == 1, (".ico has " + std::to_string(sz) + " px").c_str());
  const std::string iss = readFile("tools/windows/gridline.iss");
  expect(has(iss, "DefaultIcon\"; ValueType: string; ValueName: \"\"; ValueData: \"{app}\\gridline-app.exe,0\""), "file association icon");
  return finish();
}
