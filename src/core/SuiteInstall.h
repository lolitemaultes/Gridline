// Gridline — installing the Gridline Suite (J-131) into this user's plug-in
// folders, so other music programs find it.
//
// Every Gridline download carries the suite:
//   macOS     Gridline.app/Contents/Resources/Suite/<name>.{vst3,clap,component}
//   Windows   <program folder>\plugins\{VST3,CLAP}  (the installer also puts them
//             in Common Files\VST3 and Common Files\CLAP)
//   Linux/BSD <prefix>/lib/vst3, <prefix>/lib/clap (packages: /usr/lib/...);
//             AppImage: usr/share/gridline/suite inside the image
// Where a package already put the suite into a folder hosts scan (/usr/lib/vst3,
// Common Files, /Library/Audio/Plug-Ins), nothing more is needed. Otherwise the
// app copies it, with the user's consent, into the per-user folders:
//   macOS     ~/Library/Audio/Plug-Ins/{VST3,CLAP,Components}
//   Windows   %LOCALAPPDATA%\Programs\Common\{VST3,CLAP}
//   Linux/BSD ~/.vst3, ~/.clap
// and keeps it at the app's version (an updated app updates the suite). Each
// bundle carries gridline-suite.txt ("Gridline Suite <version> <build>") so
// installed copies can be compared with the shipped ones.
#pragma once
#include <string>
#include <vector>

namespace gl::suite {

enum class InstallState {
  NotShipped,       // this copy of Gridline carries no suite (e.g. a build tree)
  NotInstalled,     // shipped, not in this user's plug-in folders
  Outdated,         // installed copies differ from the shipped version
  Installed,        // installed in the per-user folders, same version
  SystemInstalled,  // in folders hosts scan, put there by a package / installer
};
const char* stateName(InstallState s);

struct BundleCopy {
  std::string format;  // "VST3", "CLAP", "AU"
  std::string source;  // shipped bundle (file or folder)
  std::string dest;    // per-user destination
  std::vector<std::string> extras;  // CLAP on Windows / Linux: "<name>-assets" folder beside the file
};

struct InstallStatus {
  std::string suite;  // "gridline"
  std::string name;   // "Gridline Suite"
  InstallState state = InstallState::NotShipped;
  std::string shippedVersion, installedVersion;
  std::vector<BundleCopy> bundles;
};

// Status of a suite for the running program (bundles found relative to the
// executable, or under `programDir` when given).
InstallStatus installStatus(const std::string& suiteId, const std::string& programDir = "");
// Copies (or updates) the shipped bundles into the per-user folders.
bool install(const InstallStatus& st, std::string* err = nullptr);
// Removes the per-user copies (system-wide copies belong to their package).
bool remove(const InstallStatus& st, std::string* err = nullptr);

// What the app does at startup with the suites it carries. Once this user has
// any suite copy in their plug-in folders, every suite this Gridline carries is
// brought to its version (an updated Gridline replaces the old copies and adds
// a suite that is new in it), whatever the install answer. Without copies:
// "yes" installs, "no" does nothing, anything else asks.
enum class StartupAction { None, Copy, Ask };
StartupAction startupAction(const std::vector<InstallStatus>& suites, const std::string& pref);
// The per-user plug-in folders of this system (VST3, CLAP, AU on macOS).
std::vector<std::pair<std::string, std::string>> userPluginFolders();
// The version line written into a suite bundle at build time.
std::string versionLine(const std::string& suiteName);

}  // namespace gl::suite
