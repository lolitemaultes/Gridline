// Gridline - automatic updates: how was this copy installed, and how may it be updated?
//
// detectInstall() reads only what a Probe answers (environment, the program's path, files and the
// package databases), so tests feed it fake systems. strategyFor() turns the answer into what the
// updater may do: replace itself (AppImage, the .tar.gz / .zip folder, the macOS app), run the
// Windows setup, install a system package after asking for the password (.deb, .rpm), or leave it
// to a package manager and name the command. Pure logic. See docs/updates.md.
#pragma once
#include <functional>
#include <string>
#include "Update.h"

namespace gl::update {

enum class InstallKind {
  Unknown,
  DevBuild,          // a build folder (CMakeCache.txt next to the program): never replaced
  AppImage,          // $APPIMAGE
  Deb,               // our .deb (dpkg owns the program)
  Rpm,               // our .rpm
  DistroApt,         // a .deb from an apt repository that lists gridline
  Copr,              // the COPR repository
  Aur,               // pacman owns it (AUR gridline / gridline-bin)
  FreeBsdPkg,        // the FreeBSD port / package
  TarballFolder,     // an unpacked .tar.gz run from its own folder (bin/gridline-app)
  TarballPrefix,     // a .tar.gz installed by its install.sh into a prefix (~/.local)
  Flatpak,
  Snap,
  MacApp,            // Gridline.app in a folder this user can write to
  MacAppReadOnly,    // ... in a folder this user cannot write to
  MacTranslocated,   // macOS runs it from a randomised read-only copy (App Translocation)
  MacDiskImage,      // run straight from the mounted .dmg
  Homebrew,          // the Homebrew cask
  WindowsInstaller,  // the Inno Setup installation (also what winget installs)
  WindowsPortable,   // the .zip, unpacked anywhere
  Scoop,
  Chocolatey,
};
const char* kindId(InstallKind k);  // "appimage", "deb", ... (settings, logs, --update-kind)
InstallKind kindFromId(const std::string& id);  // Unknown for anything else
const char* kindName(InstallKind k);  // "AppImage", "Debian package (.deb)", ... (English; the UI translates)

// What detectInstall may look at. Every member has a default that says "nothing there".
struct Probe {
  std::string os;  // "linux", "macos", "windows", "freebsd", ... (Target::os)
  std::string exe;  // the running program, absolute, symlinks resolved
  std::function<const char*(const char*)> env = [](const char*) -> const char* { return nullptr; };
  std::function<bool(const std::string&)> exists = [](const std::string&) { return false; };
  std::function<std::string(const std::string&)> read = [](const std::string&) { return std::string(); };
  // The names in a folder (no paths), "" entries never.
  std::function<std::vector<std::string>(const std::string&)> list = [](const std::string&) { return std::vector<std::string>(); };
  std::function<bool(const std::string&)> writable = [](const std::string&) { return false; };
  // The rpm database / FreeBSD pkg own `exe` (the real probe runs rpm -qf / pkg which).
  std::function<bool(const std::string&)> rpmOwns = [](const std::string&) { return false; };
  std::function<bool(const std::string&)> pkgOwns = [](const std::string&) { return false; };
  std::function<bool(const std::string&)> readOnlyVolume = [](const std::string&) { return false; };
};
Probe systemProbe();  // the real computer

struct Install {
  InstallKind kind = InstallKind::Unknown;
  std::string target;  // what an update replaces: the AppImage file, the .app bundle, the folder, the prefix
  std::string detail;  // why (English, for the log and Settings' tooltip)
};
Install detectInstall(const Probe& p);
// GRIDLINE_DEV_INSTALL_KIND=<id> and GRIDLINE_DEV_INSTALL_TARGET=<path> replace the answer (tests,
// screenshots; never set in a normal run).
Install detectCurrentInstall();

enum class Method {
  None,              // nothing to do here (dev build, unknown)
  ReplaceFile,       // AppImage: the new file renamed over the old one
  ReplaceFolder,     // .tar.gz / .zip folder, macOS .app: the new folder swapped with the old one
  RunInstallScript,  // .tar.gz installed into a prefix: the new install.sh
  RunWindowsSetup,   // the Inno Setup installer, silently, after Gridline quits
  SystemPackage,     // .deb / .rpm with the system's password prompt
  PackageManager,    // tell the command; never install
  Store,             // Flatpak, Snap: the store updates it
  MoveFirst,         // macOS: move Gridline to a writable Applications folder first
};
struct Strategy {
  Method method = Method::None;
  const Asset* asset = nullptr;  // the release file to download (nullptr: none fits)
  std::string command;           // PackageManager / Store: the command to run ("brew upgrade --cask gridline")
  bool needsAdmin = false;       // the system asks for a password or permission
  bool canRollBack = false;      // the old version is kept until the new one has started
  bool automatic() const {       // the splash may do it by itself (an admin prompt still asks)
    return asset && (method == Method::ReplaceFile || method == Method::ReplaceFolder || method == Method::RunInstallScript ||
                     method == Method::RunWindowsSetup || method == Method::SystemPackage);
  }
};
// `t` is the computer (currentTarget()); `r` may be nullptr (then only method / command).
Strategy strategyFor(const Install& in, const Release* r, const Target& t);
// The release file of kind `in` for `t`: .AppImage, .deb, .rpm, the .tar.gz, the portable .zip,
// the .dmg, the setup .exe. Its name must carry the release's own version.
const Asset* assetFor(const Install& in, const Release& r, const Target& t);

}  // namespace gl::update
