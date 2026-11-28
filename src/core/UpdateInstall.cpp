// Gridline - automatic updates: install kind and strategy (see UpdateInstall.h).
#include "UpdateInstall.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include "Platform.h"
#if !defined(_WIN32)
#include <sys/statvfs.h>
#include <unistd.h>
#endif

namespace gl::update {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool endsWith(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Folder part of a path, for '/' and '\\' alike ("" at the top).
std::string parentOf(const std::string& p) {
  const size_t s = p.find_last_of(p.find('\\') != std::string::npos && p.find('/') == std::string::npos ? "\\" : "/");
  if (s == std::string::npos || s == 0) return s == 0 ? std::string("/") : std::string();
  return p.substr(0, s);
}

std::string join(const std::string& a, const std::string& b, bool windows) {
  if (a.empty()) return b;
  const char sep = windows ? '\\' : '/';
  return a.back() == sep ? a + b : a + sep + b;
}

// "a file list (one path per line) mentions `path`", with or without the leading '/'.
bool listMentions(const std::string& list, const std::string& path) {
  if (path.empty() || list.empty()) return false;
  const std::string rel = path[0] == '/' ? path.substr(1) : path;
  size_t a = 0;
  while (a < list.size()) {
    size_t b = list.find('\n', a);
    if (b == std::string::npos) b = list.size();
    std::string l = list.substr(a, b - a);
    if (!l.empty() && l.back() == '\r') l.pop_back();
    if (l == path || l == rel) return true;
    a = b + 1;
  }
  return false;
}

}  // namespace

const char* kindId(InstallKind k) {
  switch (k) {
    case InstallKind::Unknown: return "unknown";
    case InstallKind::DevBuild: return "dev";
    case InstallKind::AppImage: return "appimage";
    case InstallKind::Deb: return "deb";
    case InstallKind::Rpm: return "rpm";
    case InstallKind::DistroApt: return "apt-repo";
    case InstallKind::Copr: return "copr";
    case InstallKind::Aur: return "aur";
    case InstallKind::FreeBsdPkg: return "freebsd-pkg";
    case InstallKind::TarballFolder: return "tarball";
    case InstallKind::TarballPrefix: return "tarball-prefix";
    case InstallKind::Flatpak: return "flatpak";
    case InstallKind::Snap: return "snap";
    case InstallKind::MacApp: return "mac-app";
    case InstallKind::MacAppReadOnly: return "mac-app-readonly";
    case InstallKind::MacTranslocated: return "mac-translocated";
    case InstallKind::MacDiskImage: return "mac-dmg";
    case InstallKind::Homebrew: return "homebrew";
    case InstallKind::WindowsInstaller: return "windows-setup";
    case InstallKind::WindowsPortable: return "windows-portable";
    case InstallKind::Scoop: return "scoop";
    case InstallKind::Chocolatey: return "chocolatey";
  }
  return "unknown";
}

InstallKind kindFromId(const std::string& id) {
  for (int i = 0; i <= static_cast<int>(InstallKind::Chocolatey); ++i)
    if (id == kindId(static_cast<InstallKind>(i))) return static_cast<InstallKind>(i);
  return InstallKind::Unknown;
}

const char* kindName(InstallKind k) {
  switch (k) {
    case InstallKind::Unknown: return "Unknown";
    case InstallKind::DevBuild: return "Development build";
    case InstallKind::AppImage: return "AppImage";
    case InstallKind::Deb: return "Debian package (.deb)";
    case InstallKind::Rpm: return "RPM package (.rpm)";
    case InstallKind::DistroApt: return "Package from an apt repository";
    case InstallKind::Copr: return "COPR repository";
    case InstallKind::Aur: return "Arch User Repository";
    case InstallKind::FreeBsdPkg: return "FreeBSD package";
    case InstallKind::TarballFolder: return "Unpacked .tar.gz folder";
    case InstallKind::TarballPrefix: return ".tar.gz installed with install.sh";
    case InstallKind::Flatpak: return "Flatpak";
    case InstallKind::Snap: return "Snap";
    case InstallKind::MacApp: return "Application folder";
    case InstallKind::MacAppReadOnly: return "Application in a read-only folder";
    case InstallKind::MacTranslocated: return "Application opened from Downloads (translocated)";
    case InstallKind::MacDiskImage: return "Application on the disk image";
    case InstallKind::Homebrew: return "Homebrew";
    case InstallKind::WindowsInstaller: return "Windows setup";
    case InstallKind::WindowsPortable: return "Portable .zip folder";
    case InstallKind::Scoop: return "Scoop";
    case InstallKind::Chocolatey: return "Chocolatey";
  }
  return "Unknown";
}

Install detectInstall(const Probe& p) {
  Install in;
  const bool win = p.os == "windows";
  auto env = [&](const char* n) -> std::string {
    const char* v = p.env ? p.env(n) : nullptr;
    return v ? std::string(v) : std::string();
  };
  const std::string exe = p.exe;
  const std::string exeDir = parentOf(exe);
  auto set = [&](InstallKind k, std::string target, std::string why) {
    in.kind = k;
    in.target = std::move(target);
    in.detail = std::move(why);
    return in;
  };
  // Sandboxed stores first: inside them the other signs mean nothing.
  if (!win && (p.exists("/.flatpak-info") || !env("FLATPAK_ID").empty())) return set(InstallKind::Flatpak, {}, "/.flatpak-info");
  if (!win && !env("SNAP").empty() && !env("SNAP_NAME").empty()) return set(InstallKind::Snap, {}, "$SNAP");
  if (p.os == "linux") {
    const std::string img = env("APPIMAGE");
    if (!img.empty() && p.exists(img)) return set(InstallKind::AppImage, img, "$APPIMAGE");
  }
  // A build folder: the developer's own copy is never replaced.
  {
    std::string d = exeDir;
    for (int up = 0; up < 5 && !d.empty(); ++up) {  // Gridline.app/Contents/MacOS is three deeper
      if (p.exists(join(d, "CMakeCache.txt", win))) return set(InstallKind::DevBuild, {}, join(d, "CMakeCache.txt", win));
      const std::string next = parentOf(d);
      if (next == d) break;
      d = next;
    }
  }
  if (p.os == "macos") {
    const size_t at = exe.find(".app/Contents/MacOS/");
    if (at == std::string::npos) return set(InstallKind::Unknown, {}, "not inside an application bundle");
    const std::string bundle = exe.substr(0, at + 4);
    if (bundle.find("/AppTranslocation/") != std::string::npos) return set(InstallKind::MacTranslocated, bundle, "App Translocation");
    if (bundle.rfind("/Volumes/", 0) == 0 && p.readOnlyVolume(bundle)) return set(InstallKind::MacDiskImage, bundle, "read-only volume");
    for (const char* room : {"/opt/homebrew/Caskroom/gridline", "/usr/local/Caskroom/gridline"})
      if (p.exists(room) && (bundle == "/Applications/Gridline.app" || bundle == env("HOME") + "/Applications/Gridline.app"))
        return set(InstallKind::Homebrew, bundle, room);
    const std::string folder = parentOf(bundle);
    if (!p.writable(folder) || !p.writable(bundle)) return set(InstallKind::MacAppReadOnly, bundle, folder + " is not writable");
    return set(InstallKind::MacApp, bundle, bundle);
  }
  if (win) {
    const std::string l = lower(exe);
    if (l.find("\\scoop\\apps\\gridline\\") != std::string::npos) return set(InstallKind::Scoop, exeDir, "scoop");
    const std::string choco = env("ChocolateyInstall");
    if (!choco.empty() && p.exists(join(choco, "lib\\gridline", true))) return set(InstallKind::Chocolatey, exeDir, "chocolatey");
    if (p.exists(join(exeDir, "unins000.exe", true))) return set(InstallKind::WindowsInstaller, exeDir, "unins000.exe");
    return set(InstallKind::WindowsPortable, exeDir, "no uninstaller next to the program");
  }
  // Linux and the BSDs: the package databases.
  if (p.os == "linux") {
    for (const std::string& n : p.list("/var/lib/pacman/local")) {
      if (n.rfind("gridline-", 0) != 0) continue;
      if (listMentions(p.read("/var/lib/pacman/local/" + n + "/files"), exe)) return set(InstallKind::Aur, {}, "pacman: " + n);
    }
    for (const char* list : {"/var/lib/dpkg/info/gridline.list", "/var/lib/dpkg/info/gridline:amd64.list", "/var/lib/dpkg/info/gridline:arm64.list"}) {
      if (!listMentions(p.read(list), exe)) continue;
      // An apt repository that offers gridline (a distribution's own, or a third party's).
      for (const std::string& n : p.list("/etc/apt/sources.list.d"))
        if (lower(n).find("gridline") != std::string::npos) return set(InstallKind::DistroApt, {}, "/etc/apt/sources.list.d/" + n);
      for (const std::string& n : p.list("/var/lib/apt/lists"))
        if (lower(n).find("gridline") != std::string::npos) return set(InstallKind::DistroApt, {}, "/var/lib/apt/lists/" + n);
      return set(InstallKind::Deb, {}, list);
    }
    if ((exe.rfind("/usr/", 0) == 0 || exe.rfind("/opt/", 0) == 0) && p.rpmOwns(exe)) {
      for (const std::string& n : p.list("/etc/yum.repos.d")) {
        const std::string l = lower(n);
        if (l.find("copr") != std::string::npos && l.find("gridline") != std::string::npos) return set(InstallKind::Copr, {}, "/etc/yum.repos.d/" + n);
      }
      return set(InstallKind::Rpm, {}, "rpm -qf");
    }
  }
  if (p.os == "freebsd" && exe.rfind("/usr/local/", 0) == 0 && p.pkgOwns(exe)) return set(InstallKind::FreeBsdPkg, {}, "pkg which");
  // The .tar.gz: run from its unpacked folder, or installed by its install.sh into a prefix.
  if (endsWith(exeDir, "/bin")) {
    const std::string root = parentOf(exeDir);
    if (p.exists(join(root, "share/gridline/install-manifest.txt", false)) &&
        listMentions(p.read(join(root, "share/gridline/install-manifest.txt", false)), exe))
      return set(InstallKind::TarballPrefix, root, "install-manifest.txt");
    if (p.exists(join(root, "install.sh", false)) && p.exists(join(root, "share/gridline", false)))
      return set(InstallKind::TarballFolder, root, "install.sh next to bin/");
  }
  return set(InstallKind::Unknown, {}, "no sign of how it was installed");
}

Probe systemProbe() {
  Probe p;
  p.os = currentTarget().os;
  std::error_code ec;
  fs::path e = fs::u8path(platform::executablePath());
  const fs::path real = fs::weakly_canonical(e, ec);
  p.exe = (ec ? e : real).u8string();
  p.env = [](const char* n) -> const char* { return std::getenv(n); };
  p.exists = [](const std::string& f) {
    std::error_code e2;
    return fs::exists(fs::u8path(f), e2);
  };
  p.read = [](const std::string& f) {
    std::ifstream in(fs::u8path(f), std::ios::binary);
    if (!in) return std::string();
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return s.size() > (8u << 20) ? std::string() : s;
  };
  p.list = [](const std::string& dir) {
    std::vector<std::string> out;
    std::error_code e2;
    for (fs::directory_iterator it(fs::u8path(dir), e2), end; !e2 && it != end; it.increment(e2)) out.push_back(it->path().filename().u8string());
    return out;
  };
  p.writable = [](const std::string& f) {
#if defined(_WIN32)
    (void)f;
    return true;
#else
    return ::access(f.c_str(), W_OK) == 0;
#endif
  };
  auto owns = [](const std::string& tool, std::vector<std::string> args) {
    platform::Process pr;
    if (!pr.start(tool, args)) return false;
    if (!pr.wait(5)) {
      pr.kill();
      pr.wait(2);
      return false;
    }
    return !pr.status().crashed && pr.status().code == 0;
  };
  p.rpmOwns = [owns](const std::string& f) {
    std::error_code e2;
    if (!fs::exists("/usr/bin/rpm", e2)) return false;
    if (!fs::is_directory("/var/lib/rpm", e2) && !fs::is_directory("/usr/lib/sysimage/rpm", e2)) return false;
    return owns("/usr/bin/rpm", {"rpm", "-qf", "--quiet", f});
  };
  p.pkgOwns = [owns](const std::string& f) {
    std::error_code e2;
    if (!fs::exists("/usr/sbin/pkg", e2)) return false;
    return owns("/usr/sbin/pkg", {"pkg", "which", "-q", f});
  };
  p.readOnlyVolume = [](const std::string& f) {
#if defined(_WIN32)
    (void)f;
    return false;
#else
    struct statvfs sv{};
    return ::statvfs(f.c_str(), &sv) == 0 && (sv.f_flag & ST_RDONLY) != 0;
#endif
  };
  return p;
}

Install detectCurrentInstall() {
  if (const char* k = std::getenv("GRIDLINE_DEV_INSTALL_KIND"); k && *k) {
    Install in;
    in.kind = kindFromId(k);
    if (const char* t = std::getenv("GRIDLINE_DEV_INSTALL_TARGET")) in.target = t;
    in.detail = "GRIDLINE_DEV_INSTALL_KIND";
    return in;
  }
  return detectInstall(systemProbe());
}

namespace {
// "Gridline-<exact version><suffix>"; the version in the name must be the release's own.
const Asset* exactAsset(const Release& r, const std::string& suffix) {
  if (!r.version.valid) return nullptr;
  const std::string want = lower("gridline-" + toString(r.version) + suffix);
  for (const Asset& a : r.assets)
    if (lower(a.name) == want) return &a;
  return nullptr;
}
std::string debArch(const Target& t) { return t.arch == "aarch64" ? "arm64" : "amd64"; }
std::string winArch(const Target& t) { return t.arch == "aarch64" ? "arm64" : "x64"; }
}  // namespace

const Asset* assetFor(const Install& in, const Release& r, const Target& t) {
  switch (in.kind) {
    case InstallKind::AppImage: return exactAsset(r, "-linux-" + t.arch + ".AppImage");
    case InstallKind::Deb: return exactAsset(r, "-linux-" + debArch(t) + ".deb");
    case InstallKind::Rpm: return exactAsset(r, "-linux-" + t.arch + ".rpm");
    case InstallKind::TarballFolder:
    case InstallKind::TarballPrefix: return exactAsset(r, "-" + t.os + "-" + t.arch + ".tar.gz");
    case InstallKind::MacApp: return exactAsset(r, "-macos-universal.dmg");
    case InstallKind::WindowsInstaller: return exactAsset(r, "-windows-" + winArch(t) + "-setup.exe");
    case InstallKind::WindowsPortable: return exactAsset(r, "-windows-" + winArch(t) + ".zip");
    default: return nullptr;
  }
}

Strategy strategyFor(const Install& in, const Release* r, const Target& t) {
  Strategy s;
  auto pm = [&](const char* cmd) {
    s.method = Method::PackageManager;
    s.command = cmd;
    return s;
  };
  switch (in.kind) {
    case InstallKind::AppImage: s.method = Method::ReplaceFile, s.canRollBack = true; break;
    case InstallKind::TarballFolder:
    case InstallKind::WindowsPortable:
    case InstallKind::MacApp: s.method = Method::ReplaceFolder, s.canRollBack = true; break;
    case InstallKind::TarballPrefix:  // spread over a prefix: no whole-folder swap, so not by itself (no rollback)
      s.method = Method::None, s.command = "tar -xzf Gridline-<version>-linux-<arch>.tar.gz && sh Gridline-<version>-linux-<arch>/install.sh";
      break;
    case InstallKind::WindowsInstaller: s.method = Method::RunWindowsSetup, s.needsAdmin = true; break;
    case InstallKind::Deb:
    case InstallKind::Rpm: s.method = Method::SystemPackage, s.needsAdmin = true; break;
    case InstallKind::Flatpak:
      s.method = Method::Store;
      s.command = "flatpak update com.lolitemaultes.Gridline";
      return s;
    case InstallKind::Snap:
      s.method = Method::Store;
      s.command = "snap refresh gridline";
      return s;
    case InstallKind::Homebrew: return pm("brew upgrade --cask gridline");
    case InstallKind::Scoop: return pm("scoop update gridline");
    case InstallKind::Chocolatey: return pm("choco upgrade gridline");
    case InstallKind::Aur: return pm("yay -Syu gridline");
    case InstallKind::Copr: return pm("sudo dnf upgrade gridline");
    case InstallKind::DistroApt: return pm("sudo apt update && sudo apt upgrade gridline");
    case InstallKind::FreeBsdPkg: return pm("sudo pkg upgrade gridline");
    case InstallKind::MacAppReadOnly:
    case InstallKind::MacTranslocated:
    case InstallKind::MacDiskImage: s.method = Method::MoveFirst; return s;
    case InstallKind::Unknown:
    case InstallKind::DevBuild: return s;
  }
  // A user-level swap needs the place it replaces.
  if ((s.method == Method::ReplaceFile || s.method == Method::ReplaceFolder || s.method == Method::RunInstallScript) && in.target.empty()) {
    s.method = Method::None;
    return s;
  }
  if (r) s.asset = assetFor(in, *r, t);
  return s;
}

}  // namespace gl::update
