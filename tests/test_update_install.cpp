// Gridline - automatic updates: how this copy was installed (every kind, on fake systems), what the
// updater may do for each, the release file it picks, and the small rules around installing
// (version answers, minimum system, package manager errors, commands). No network, no installs.
#include <map>
#include <set>
#include <string>
#include <vector>
#include "Check.h"
#include "core/Update.h"
#include "core/UpdateFlow.h"
#include "core/UpdateInstall.h"

using namespace gl::update;
using namespace gl::update::flow;

namespace {

// A made-up computer: files (path -> contents), folders (path -> names), variables.
struct Fake {
  std::string os = "linux", exe;
  std::map<std::string, std::string> env, files;
  std::map<std::string, std::vector<std::string>> dirs;
  std::set<std::string> readOnly, rpm, pkg, readOnlyVolumes;
  Probe probe() const {
    Probe p;
    p.os = os;
    p.exe = exe;
    p.env = [this](const char* n) -> const char* {
      auto it = env.find(n);
      return it == env.end() ? nullptr : it->second.c_str();
    };
    p.exists = [this](const std::string& f) { return files.count(f) || dirs.count(f); };
    p.read = [this](const std::string& f) {
      auto it = files.find(f);
      return it == files.end() ? std::string() : it->second;
    };
    p.list = [this](const std::string& d) {
      auto it = dirs.find(d);
      return it == dirs.end() ? std::vector<std::string>() : it->second;
    };
    p.writable = [this](const std::string& f) { return !readOnly.count(f); };
    p.rpmOwns = [this](const std::string& f) { return rpm.count(f) > 0; };
    p.pkgOwns = [this](const std::string& f) { return pkg.count(f) > 0; };
    p.readOnlyVolume = [this](const std::string& f) { return readOnlyVolumes.count(f) > 0; };
    return p;
  }
};

InstallKind kindOf(const Fake& f) { return detectInstall(f.probe()).kind; }

Release release(const std::string& v) { return fakeRelease(v); }

}  // namespace

int main() {
  // ---- detection
  {
    Fake f;
    f.exe = "/app/bin/gridline-app";
    f.files["/.flatpak-info"] = "[Application]";
    expect(kindOf(f) == InstallKind::Flatpak, "Flatpak: /.flatpak-info");
    f.files.clear();
    f.env["FLATPAK_ID"] = "com.lolitemaultes.Gridline";
    expect(kindOf(f) == InstallKind::Flatpak, "Flatpak: FLATPAK_ID");
  }
  {
    Fake f;
    f.exe = "/snap/gridline/12/usr/bin/gridline-app";
    f.env["SNAP"] = "/snap/gridline/12";
    f.env["SNAP_NAME"] = "gridline";
    expect(kindOf(f) == InstallKind::Snap, "Snap: $SNAP and $SNAP_NAME");
  }
  {
    Fake f;
    f.exe = "/tmp/.mount_GridlnXyz/usr/bin/gridline-app";
    f.env["APPIMAGE"] = "/home/u/Applications/Gridline.AppImage";
    f.files["/home/u/Applications/Gridline.AppImage"] = "x";
    const Install in = detectInstall(f.probe());
    expect(in.kind == InstallKind::AppImage && in.target == "/home/u/Applications/Gridline.AppImage", "AppImage: $APPIMAGE is the target");
    f.files.clear();
    expect(kindOf(f) != InstallKind::AppImage, "AppImage: a $APPIMAGE that names no file is not trusted");
  }
  {
    Fake f;
    f.exe = "/home/me/gridline/build-k448/gridline-app";
    f.files["/home/me/gridline/build-k448/CMakeCache.txt"] = "";
    expect(kindOf(f) == InstallKind::DevBuild, "a build folder is a development build");
    f.os = "macos";
    f.exe = "/Users/me/gridline/build/Gridline.app/Contents/MacOS/gridline-app";
    f.files["/Users/me/gridline/build/CMakeCache.txt"] = "";
    expect(kindOf(f) == InstallKind::DevBuild, "a development build of the macOS app too");
  }
  {
    Fake f;
    f.exe = "/usr/bin/gridline-app";
    f.files["/var/lib/dpkg/info/gridline.list"] = "/.\n/usr\n/usr/bin\n/usr/bin/gridline-app\n/usr/bin/gridline\n";
    expect(kindOf(f) == InstallKind::Deb, ".deb: dpkg lists the program");
    f.dirs["/etc/apt/sources.list.d"] = {"ubuntu.sources", "gridline-ppa.list"};
    expect(kindOf(f) == InstallKind::DistroApt, ".deb from an apt repository: the package manager updates it");
    f.dirs.clear();
    f.files["/var/lib/dpkg/info/gridline.list"] = "/usr/bin/gridline\n";
    expect(kindOf(f) != InstallKind::Deb, ".deb: a list without this program is something else");
  }
  {
    Fake f;
    f.exe = "/usr/bin/gridline-app";
    f.rpm.insert("/usr/bin/gridline-app");
    expect(kindOf(f) == InstallKind::Rpm, ".rpm: the rpm database owns the program");
    f.dirs["/etc/yum.repos.d"] = {"fedora.repo", "_copr:copr.fedorainfracloud.org:lolitemaultes:gridline.repo"};
    expect(kindOf(f) == InstallKind::Copr, "COPR: its repository file");
  }
  {
    Fake f;
    f.exe = "/usr/bin/gridline-app";
    f.dirs["/var/lib/pacman/local"] = {"glibc-2.40-1", "gridline-bin-0.7.8-1"};
    f.files["/var/lib/pacman/local/gridline-bin-0.7.8-1/files"] = "%FILES%\nusr/\nusr/bin/\nusr/bin/gridline-app\n";
    expect(kindOf(f) == InstallKind::Aur, "AUR: pacman's file list");
  }
  {
    Fake f;
    f.os = "freebsd";
    f.exe = "/usr/local/bin/gridline-app";
    f.pkg.insert("/usr/local/bin/gridline-app");
    expect(kindOf(f) == InstallKind::FreeBsdPkg, "FreeBSD: pkg owns the program");
  }
  {
    Fake f;
    f.exe = "/home/u/Gridline-0.7.8-linux-x86_64/bin/gridline-app";
    f.files["/home/u/Gridline-0.7.8-linux-x86_64/install.sh"] = "#!/bin/sh";
    f.dirs["/home/u/Gridline-0.7.8-linux-x86_64/share/gridline"] = {"assets"};
    const Install in = detectInstall(f.probe());
    expect(in.kind == InstallKind::TarballFolder && in.target == "/home/u/Gridline-0.7.8-linux-x86_64", ".tar.gz run from its folder");
    Fake g;
    g.exe = "/home/u/.local/bin/gridline-app";
    g.files["/home/u/.local/share/gridline/install-manifest.txt"] = "/home/u/.local/bin/gridline-app\n/home/u/.local/bin/gridline\n";
    expect(kindOf(g) == InstallKind::TarballPrefix, ".tar.gz installed by install.sh");
    {
      Install in;
      in.kind = InstallKind::TarballPrefix;
      in.target = "/home/u/.local";
      const Strategy st = strategyFor(in, nullptr, Target("linux", "x86_64"));
      expect(!st.canRollBack && st.method == Method::None && st.command.find("install.sh") != std::string::npos,
             ".tar.gz in a prefix: not automatic (no way back), the steps are named");
    }
    Fake h;
    h.exe = "/opt/somewhere/gridline-app";
    expect(kindOf(h) == InstallKind::Unknown, "nothing recognisable: unknown");
  }
  {
    Fake f;
    f.os = "macos";
    f.env["HOME"] = "/Users/u";
    f.exe = "/Applications/Gridline.app/Contents/MacOS/gridline-app";
    const Install in = detectInstall(f.probe());
    expect(in.kind == InstallKind::MacApp && in.target == "/Applications/Gridline.app", "macOS: the app in Applications");
    f.readOnly.insert("/Applications");
    expect(kindOf(f) == InstallKind::MacAppReadOnly, "macOS: Applications not writable for this user");
    f.readOnly.clear();
    f.dirs["/opt/homebrew/Caskroom/gridline"] = {"0.7.8"};
    expect(kindOf(f) == InstallKind::Homebrew, "macOS: Homebrew's cask");
    f.dirs.clear();
    f.exe = "/private/var/folders/xy/T/AppTranslocation/ABCD-1234/d/Gridline.app/Contents/MacOS/gridline-app";
    expect(kindOf(f) == InstallKind::MacTranslocated, "macOS: App Translocation");
    f.exe = "/Volumes/Gridline 0.7.8/Gridline.app/Contents/MacOS/gridline-app";
    f.readOnlyVolumes.insert("/Volumes/Gridline 0.7.8/Gridline.app");
    expect(kindOf(f) == InstallKind::MacDiskImage, "macOS: run from the disk image");
    f.exe = "/Users/u/bin/gridline-app";
    expect(kindOf(f) == InstallKind::Unknown, "macOS: a bare program outside a bundle is not replaced");
  }
  {
    Fake f;
    f.os = "windows";
    f.exe = "C:\\Program Files\\Gridline\\gridline-app.exe";
    f.files["C:\\Program Files\\Gridline\\unins000.exe"] = "";
    expect(kindOf(f) == InstallKind::WindowsInstaller, "Windows: the setup's uninstaller next to the program");
    f.files.clear();
    expect(kindOf(f) == InstallKind::WindowsPortable, "Windows: no uninstaller, the portable .zip");
    f.exe = "C:\\Users\\u\\scoop\\apps\\gridline\\0.7.8\\gridline-app.exe";
    expect(kindOf(f) == InstallKind::Scoop, "Windows: Scoop");
    f.exe = "C:\\ProgramData\\chocolatey\\lib\\gridline\\tools\\gridline-app.exe";
    f.env["ChocolateyInstall"] = "C:\\ProgramData\\chocolatey";
    f.dirs["C:\\ProgramData\\chocolatey\\lib\\gridline"] = {"tools"};
    expect(kindOf(f) == InstallKind::Chocolatey, "Windows: Chocolatey");
  }
  {
    for (int i = 0; i <= static_cast<int>(InstallKind::Chocolatey); ++i) {
      const InstallKind k = static_cast<InstallKind>(i);
      if (kindFromId(kindId(k)) != k) expect(false, "kind ids round trip");
    }
    expect(kindFromId("nonsense") == InstallKind::Unknown, "an unknown id is Unknown");
  }

  // ---- strategies
  const Release r = release("0.9.0");
  const Target linux64("linux", "x86_64"), mac("macos", "aarch64"), win("windows", "x86_64"), winArm("windows", "aarch64");
  auto strat = [&](InstallKind k, const std::string& target, const Target& t) {
    Install in;
    in.kind = k;
    in.target = target;
    return strategyFor(in, &r, t);
  };
  {
    Strategy s = strat(InstallKind::AppImage, "/home/u/Gridline.AppImage", linux64);
    expect(s.method == Method::ReplaceFile && s.canRollBack && !s.needsAdmin && s.asset && s.asset->name == "Gridline-0.9.0-linux-x86_64.AppImage",
           "AppImage: replaces itself with the new AppImage");
    s = strat(InstallKind::AppImage, "", linux64);
    expect(!s.automatic(), "AppImage without a known file: not by itself");
    s = strat(InstallKind::Deb, "", Target("linux", "aarch64"));
    expect(s.method == Method::SystemPackage && s.needsAdmin && s.asset && s.asset->name == "Gridline-0.9.0-linux-arm64.deb",
           ".deb: the new .deb, with the password prompt");
    s = strat(InstallKind::Rpm, "", linux64);
    expect(s.method == Method::SystemPackage && s.asset && s.asset->name == "Gridline-0.9.0-linux-x86_64.rpm", ".rpm: the new .rpm");
    s = strat(InstallKind::TarballFolder, "/home/u/Gridline-0.8.0-linux-x86_64", linux64);
    expect(s.method == Method::ReplaceFolder && s.canRollBack && s.asset && s.asset->name == "Gridline-0.9.0-linux-x86_64.tar.gz",
           ".tar.gz folder: the new folder swapped in");
    s = strat(InstallKind::MacApp, "/Applications/Gridline.app", mac);
    expect(s.method == Method::ReplaceFolder && s.asset && s.asset->name == "Gridline-0.9.0-macos-universal.dmg", "macOS: the app from the new disk image");
    s = strat(InstallKind::WindowsInstaller, "C:\\Program Files\\Gridline", winArm);
    expect(s.method == Method::RunWindowsSetup && s.needsAdmin && s.asset && s.asset->name == "Gridline-0.9.0-windows-arm64-setup.exe",
           "Windows: the setup for this processor");
    expect(strat(InstallKind::WindowsPortable, "D:\\Gridline", win).method == Method::ReplaceFolder, "Windows portable: the folder swapped");
  }
  {
    const struct {
      InstallKind k;
      Method m;
      const char* cmd;
    } pm[] = {{InstallKind::Flatpak, Method::Store, "flatpak update com.lolitemaultes.Gridline"},
              {InstallKind::Snap, Method::Store, "snap refresh gridline"},
              {InstallKind::Homebrew, Method::PackageManager, "brew upgrade --cask gridline"},
              {InstallKind::Scoop, Method::PackageManager, "scoop update gridline"},
              {InstallKind::Chocolatey, Method::PackageManager, "choco upgrade gridline"},
              {InstallKind::Aur, Method::PackageManager, "yay -Syu gridline"},
              {InstallKind::Copr, Method::PackageManager, "sudo dnf upgrade gridline"},
              {InstallKind::FreeBsdPkg, Method::PackageManager, "sudo pkg upgrade gridline"}};
    bool all = true;
    for (const auto& c : pm) {
      const Strategy s = strat(c.k, "/x", linux64);
      all = all && s.method == c.m && s.command == c.cmd && !s.automatic() && !s.asset;
    }
    expect(all, "package managers and stores: never by itself, the command named");
    expect(strat(InstallKind::DevBuild, "/x", linux64).method == Method::None && strat(InstallKind::Unknown, "/x", linux64).method == Method::None,
           "development builds and unknown installs: nothing");
    expect(strat(InstallKind::MacTranslocated, "/x", mac).method == Method::MoveFirst && strat(InstallKind::MacAppReadOnly, "/x", mac).method == Method::MoveFirst,
           "macOS translocated / read-only: move to Applications first");
  }
  {
    // The file must carry the release's own version (an old file under a new tag is refused).
    Release odd = r;
    for (Asset& a : odd.assets)
      if (a.name == "Gridline-0.9.0-linux-x86_64.AppImage") a.name = "Gridline-0.8.0-linux-x86_64.AppImage";
    Install in;
    in.kind = InstallKind::AppImage;
    in.target = "/x";
    expect(assetFor(in, odd, linux64) == nullptr, "an asset named for another version is never used");
    Release none = r;
    none.assets.clear();
    expect(!strategyFor(in, &none, linux64).automatic(), "a release without the file for this install: not by itself");
  }

  // ---- install helpers
  expect(versionOutputMatches("Gridline 0.9.0\n", "0.9.0") && !versionOutputMatches("Gridline 0.9.0-beta.1\n", "0.9.0") &&
             !versionOutputMatches("Gridline 0.8.0\n", "0.9.0") && versionOutputMatches("[graphics]\nGridline 0.9.0", "0.9.0"),
         "the test run must answer exactly the new version");
  expect(osAtLeast("14.6", "11.0") && osAtLeast("13.0", "13") && !osAtLeast("12.7.4", "13.0") && osAtLeast("15.0.1", "15.0"),
         "minimum macOS compared by parts");
  expect(classifySystemInstall(0, "") == SystemInstallError::None, "pkexec 0: installed");
  expect(classifySystemInstall(126, "") == SystemInstallError::Cancelled, "pkexec 126: the password dialog was closed");
  expect(classifySystemInstall(127, "Error executing command as another user: Not authorized") == SystemInstallError::NotAuthorized,
         "pkexec 127: not authorised");
  expect(classifySystemInstall(100, "E: Could not get lock /var/lib/dpkg/lock-frontend. It is held by process 1234 (apt)") == SystemInstallError::Locked,
         "apt: dpkg locked by another program");
  expect(classifySystemInstall(100, "The following packages have unmet dependencies:\n gridline : Depends: libsdl2-2.0-0") ==
             SystemInstallError::Dependencies,
         "apt: dependency errors");
  expect(classifySystemInstall(1, "Error: \n Problem: conflicting requests\n  - nothing provides libjack.so.0") == SystemInstallError::Dependencies,
         "dnf: dependency errors");
  expect(classifySystemInstall(-1, "") == SystemInstallError::NoTool, "no program: no tool");
  {
    auto which = [](std::set<std::string> have) {
      return [have](const std::string& t) { return have.count(t) ? "/usr/bin/" + t : std::string(); };
    };
    auto c = systemInstallCommand(InstallKind::Deb, "/tmp/g.deb", which({"pkexec", "apt-get", "dpkg"}));
    expect(c == std::vector<std::string>{"/usr/bin/pkexec", "/usr/bin/apt-get", "install", "-y", "--no-install-recommends", "/tmp/g.deb"},
           ".deb: pkexec apt-get install (dependencies resolved)");
    c = systemInstallCommand(InstallKind::Rpm, "/tmp/g.rpm", which({"pkexec", "dnf", "rpm"}));
    expect(c.size() == 5 && c[1] == "/usr/bin/dnf", ".rpm: pkexec dnf install");
    c = systemInstallCommand(InstallKind::Rpm, "/tmp/g.rpm", which({"pkexec", "zypper"}));
    expect(c.size() == 6 && c[1] == "/usr/bin/zypper" && c[4] == "--allow-unsigned-rpm", ".rpm on openSUSE: zypper");
    expect(systemInstallCommand(InstallKind::Deb, "/tmp/g.deb", which({"apt-get"})).empty(), "no pkexec: no command (download ready instead)");
  }
  {
    Install in;
    in.kind = InstallKind::MacApp;
    in.target = "/Applications/Gridline.app";
    auto c = relaunchCommand(in, "/x", {"song.glp"});
    expect(c == std::vector<std::string>{"/Applications/Gridline.app/Contents/MacOS/gridline-app", "song.glp"}, "relaunch: the swapped app, same arguments");
    in.kind = InstallKind::AppImage;
    in.target = "/home/u/Gridline.AppImage";
    expect(relaunchCommand(in, "/tmp/.mount/usr/bin/gridline-app", {})[0] == "/home/u/Gridline.AppImage", "relaunch: the AppImage file, not its old mount");
    in.kind = InstallKind::Deb;
    expect(relaunchCommand(in, "/usr/bin/gridline-app", {})[0] == "/usr/bin/gridline-app", "relaunch: a system package in place");
  }
  return finish();
}
