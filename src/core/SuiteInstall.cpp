// Gridline — installing the Gridline Suite into the per-user plug-in folders (J-131).
#include "SuiteInstall.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include "GridlineVersion.h"
#include "Platform.h"
#include "PluginSuite.h"
#if defined(__APPLE__)
#include <sys/xattr.h>
#endif

namespace fs = std::filesystem;

namespace gl::suite {

namespace {

constexpr const char* kMarker = "gridline-suite.txt";

std::string readLine(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::string line;
  if (f) std::getline(f, line);
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) line.pop_back();
  return line;
}

bool present(const fs::path& p) {
  std::error_code ec;
  return fs::exists(p, ec);
}

// The version file of an installed or shipped bundle (CLAP files keep it in their assets folder).
fs::path markerOf(const fs::path& bundle) {
  std::error_code ec;
  if (fs::is_directory(bundle, ec)) return bundle / "Contents" / "Resources" / kMarker;
  return bundle.parent_path() / (bundle.stem().string() + "-assets") / kMarker;
}

#if defined(_WIN32)
fs::path envPath(const char* name) {
  const char* v = std::getenv(name);
  return v && *v ? fs::u8path(v) : fs::path();
}
#endif

struct Folders {
  fs::path vst3, clap, au;  // per-user
  std::vector<fs::path> systemVst3, systemClap, systemAu;
};

Folders folders() {
  Folders f;
  const fs::path home = fs::u8path(platform::homeDir());
#if defined(__APPLE__)
  f.vst3 = home / "Library" / "Audio" / "Plug-Ins" / "VST3";
  f.clap = home / "Library" / "Audio" / "Plug-Ins" / "CLAP";
  f.au = home / "Library" / "Audio" / "Plug-Ins" / "Components";
  f.systemVst3 = {"/Library/Audio/Plug-Ins/VST3"};
  f.systemClap = {"/Library/Audio/Plug-Ins/CLAP"};
  f.systemAu = {"/Library/Audio/Plug-Ins/Components"};
#elif defined(_WIN32)
  const fs::path local = envPath("LOCALAPPDATA");
  f.vst3 = local / "Programs" / "Common" / "VST3";
  f.clap = local / "Programs" / "Common" / "CLAP";
  if (const fs::path common = envPath("CommonProgramW6432"); !common.empty()) {
    f.systemVst3.push_back(common / "VST3");
    f.systemClap.push_back(common / "CLAP");
  }
  if (const fs::path common = envPath("CommonProgramFiles"); !common.empty()) {
    f.systemVst3.push_back(common / "VST3");
    f.systemClap.push_back(common / "CLAP");
  }
#else
  f.vst3 = home / ".vst3";
  f.clap = home / ".clap";
  f.systemVst3 = {"/usr/lib/vst3", "/usr/local/lib/vst3", "/usr/lib64/vst3"};
  f.systemClap = {"/usr/lib/clap", "/usr/local/lib/clap", "/usr/lib64/clap"};
#endif
  return f;
}

bool sameDir(const fs::path& a, const fs::path& b) {
  std::error_code ec;
  return fs::equivalent(a, b, ec);
}

void clearQuarantine(const fs::path& root) {
#if defined(__APPLE__)
  // A copy of a downloaded app's bundle would carry the download's quarantine
  // flag: hosts would refuse to load it. The user asked for this install.
  removexattr(root.c_str(), "com.apple.quarantine", XATTR_NOFOLLOW);
  std::error_code ec;
  if (fs::is_directory(root, ec))
    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
      removexattr(it->path().c_str(), "com.apple.quarantine", XATTR_NOFOLLOW);
#else
  (void)root;
#endif
}

// Replaces `to` with a copy of `from` (a file or a folder): copied next to it first, then swapped.
bool replaceWithCopy(const fs::path& from, const fs::path& to, std::string* err) {
  std::error_code ec;
  fs::create_directories(to.parent_path(), ec);
  const fs::path tmp = to.parent_path() / (".gridline-install-" + to.filename().string());
  fs::remove_all(tmp, ec);
  ec.clear();
  if (fs::is_directory(from, ec))
    fs::copy(from, tmp, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
  else
    fs::copy_file(from, tmp, fs::copy_options::overwrite_existing, ec);
  if (ec) {
    if (err) *err = "copy to " + tmp.string() + ": " + ec.message();
    fs::remove_all(tmp, ec);
    return false;
  }
  clearQuarantine(tmp);
  fs::remove_all(to, ec);
  ec.clear();
  fs::rename(tmp, to, ec);
  if (ec) {
    if (err) *err = "rename to " + to.string() + ": " + ec.message();
    fs::remove_all(tmp, ec);
    return false;
  }
  return true;
}

}  // namespace

const char* stateName(InstallState s) {
  switch (s) {
    case InstallState::NotShipped: return "not shipped with this copy";
    case InstallState::NotInstalled: return "not installed";
    case InstallState::Outdated: return "installed, older version";
    case InstallState::Installed: return "installed";
    case InstallState::SystemInstalled: return "installed for all users";
  }
  return "";
}

std::string versionLine(const std::string& suiteName) { return suiteName + " " + GRIDLINE_VERSION + " " + GRIDLINE_BUILD_ID; }

std::vector<std::pair<std::string, std::string>> userPluginFolders() {
  const Folders f = folders();
  std::vector<std::pair<std::string, std::string>> out = {{"VST3", f.vst3.string()}, {"CLAP", f.clap.string()}};
  if (!f.au.empty()) out.emplace_back("AU", f.au.string());
  return out;
}

InstallStatus installStatus(const std::string& suiteId, const std::string& programDir) {
  InstallStatus st;
  st.suite = suiteId;
  const Suite* s = findSuite(suiteId);
  if (!s) return st;
  st.name = s->name;
  const fs::path exe = programDir.empty() ? fs::u8path(platform::executablePath()).parent_path() : fs::u8path(programDir);
  const Folders f = folders();
  const std::string n = s->name;
  struct Cand {
    const char* format;
    fs::path source;
    fs::path destDir;
    const std::vector<fs::path>* system;
  };
  std::vector<Cand> cands;
#if defined(__APPLE__)
  const fs::path res = exe.parent_path() / "Resources" / "Suite";
  cands = {{"VST3", res / (n + ".vst3"), f.vst3, &f.systemVst3},
           {"CLAP", res / (n + ".clap"), f.clap, &f.systemClap},
           {"AU", res / (n + ".component"), f.au, &f.systemAu}};
#elif defined(_WIN32)
  cands = {{"VST3", exe / "plugins" / "VST3" / (n + ".vst3"), f.vst3, &f.systemVst3},
           {"CLAP", exe / "plugins" / "CLAP" / (n + ".clap"), f.clap, &f.systemClap}};
  // The installer puts the suite straight into Common Files (not beside the program).
  for (Cand& c : cands)
    if (!present(c.source) && !c.system->empty()) c.source = c.system->front() / c.source.filename();
#else
  const fs::path prefix = exe.parent_path();
  fs::path vst3 = prefix / "lib" / "vst3" / (n + ".vst3"), clap = prefix / "lib" / "clap" / (n + ".clap");
  if (!present(vst3)) {  // AppImage
    vst3 = prefix / "share" / "gridline" / "suite" / (n + ".vst3");
    clap = prefix / "share" / "gridline" / "suite" / (n + ".clap");
  }
  cands = {{"VST3", vst3, f.vst3, &f.systemVst3}, {"CLAP", clap, f.clap, &f.systemClap}};
#endif
  bool systemAll = true, userAll = true, userAny = false, upToDate = true;
  for (const Cand& c : cands) {
    if (!present(c.source)) continue;
    BundleCopy b;
    b.format = c.format;
    b.source = c.source.string();
    b.dest = (c.destDir / c.source.filename()).string();
    std::error_code ec;
    if (!fs::is_directory(c.source, ec)) {
      const fs::path assets = c.source.parent_path() / (c.source.stem().string() + "-assets");
      if (present(assets)) b.extras.push_back(assets.string());
    }
    const std::string shipped = readLine(markerOf(c.source));
    if (st.shippedVersion.empty()) st.shippedVersion = shipped;
    // Already in a folder hosts scan (a package or the Windows installer put it there)?
    bool inSystem = false;
    for (const fs::path& sys : *c.system) {
      if (sameDir(c.source.parent_path(), sys)) inSystem = true;
      const fs::path there = sys / c.source.filename();
      if (present(there) && readLine(markerOf(there)) == shipped) inSystem = true;
    }
    systemAll = systemAll && inSystem;
    const fs::path dest = fs::u8path(b.dest);
    if (present(dest)) {
      userAny = true;
      const std::string inst = readLine(markerOf(dest));
      if (st.installedVersion.empty()) st.installedVersion = inst;
      if (inst != shipped) upToDate = false;
    } else {
      userAll = false;
    }
    st.bundles.push_back(std::move(b));
  }
  if (st.bundles.empty())
    st.state = InstallState::NotShipped;
  else if (userAll && upToDate)
    st.state = InstallState::Installed;
  else if (userAny)
    st.state = InstallState::Outdated;
  else if (systemAll)
    st.state = InstallState::SystemInstalled;
  else
    st.state = InstallState::NotInstalled;
  return st;
}

bool install(const InstallStatus& st, std::string* err) {
  if (st.bundles.empty()) {
    if (err) *err = "this copy of Gridline carries no Gridline Suite";
    return false;
  }
  for (const BundleCopy& b : st.bundles) {
    if (!replaceWithCopy(fs::u8path(b.source), fs::u8path(b.dest), err)) return false;
    for (const std::string& x : b.extras) {
      const fs::path from = fs::u8path(x);
      if (!replaceWithCopy(from, fs::u8path(b.dest).parent_path() / from.filename(), err)) return false;
    }
  }
  return true;
}

StartupAction startupAction(const std::vector<InstallStatus>& suites, const std::string& pref) {
  bool needs = false, copies = false;
  for (const InstallStatus& st : suites) {
    needs = needs || st.state == InstallState::NotInstalled || st.state == InstallState::Outdated;
    copies = copies || st.state == InstallState::Installed || st.state == InstallState::Outdated;
  }
  if (!needs) return StartupAction::None;
  if (copies || pref == "yes") return StartupAction::Copy;
  return pref == "no" ? StartupAction::None : StartupAction::Ask;
}

bool remove(const InstallStatus& st, std::string* err) {
  bool ok = true;
  for (const BundleCopy& b : st.bundles) {
    std::error_code ec;
    const fs::path dest = fs::u8path(b.dest);
    fs::remove_all(dest, ec);
    if (ec) {
      ok = false;
      if (err) *err = dest.string() + ": " + ec.message();
    }
    for (const std::string& x : b.extras) fs::remove_all(dest.parent_path() / fs::u8path(x).filename(), ec);
  }
  return ok;
}

}  // namespace gl::suite
