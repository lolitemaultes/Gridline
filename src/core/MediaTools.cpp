// Gridline - J-192: yt-dlp and ffmpeg for Import from URL (see MediaTools.h).
#include "MediaTools.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include "Platform.h"
#include "UrlImport.h"
#include "Zip.h"
#if defined(__APPLE__)
#include <sys/xattr.h>
#endif

namespace fs = std::filesystem;

namespace gl::mediatools {

namespace {

std::string envStr(const char* name) {
  const char* v = std::getenv(name);
  return v ? std::string(v) : std::string();
}

bool runnable(const fs::path& p) {
  std::error_code ec;
  if (!fs::is_regular_file(p, ec)) return false;
#if defined(_WIN32)
  return true;
#else
  const fs::perms pr = fs::status(p, ec).permissions();
  return !ec && (pr & (fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec)) != fs::perms::none;
#endif
}

void makeRunnable(const fs::path& p) {
  std::error_code ec;
  fs::permissions(p, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec,
                  fs::perm_options::replace, ec);
#if defined(__APPLE__)
  removexattr(p.u8string().c_str(), "com.apple.quarantine", 0);  // fetched by Gridline itself, checked against its checksum
#endif
}

bool isBsd(const update::Target& t) { return t.os == "freebsd" || t.os == "openbsd" || t.os == "netbsd"; }

const update::Asset* assetNamed(const update::Release& r, const std::string& name) {
  for (const auto& a : r.assets)
    if (a.name == name) return &a;
  return nullptr;
}

// Downloads `a` into `dest`.part, checks it against `sums`, and leaves it there (true) or removes it.
bool fetchChecked(const Net& net, const update::Asset& a, const std::string& want, const fs::path& part, const Progress& progress,
                  const std::atomic<bool>* cancel, std::string& err) {
  std::error_code ec;
  fs::remove(part, ec);
  std::string e;
  if (!net.download(a.url, part.u8string(), progress, cancel, &e)) {
    fs::remove(part, ec);
    err = cancel && cancel->load() ? "cancelled" : e.empty() ? "the download of " + a.name + " failed" : e;
    return false;
  }
  if (update::sha256File(part.u8string()) != want) {
    fs::remove(part, ec);
    err = a.name + " does not match its published SHA-256 checksum";
    return false;
  }
  return true;
}

std::string httpError(const update::HttpResult& r, const std::string& what) {
  if (r.offline || r.status == 0) return "could not reach GitHub (" + what + ")";
  if (r.status == 403 || r.status == 429) return "GitHub is limiting requests right now (" + what + ")";
  return "could not read " + what + " (HTTP " + std::to_string(r.status) + ")";
}

bool latestRelease(const Net& net, const std::string& url, update::Release& out, std::string& err) {
  const update::HttpResult r = net.fetch(url);
  if (r.status != 200) {
    err = httpError(r, "the newest yt-dlp release");
    return false;
  }
  std::vector<update::Release> rel;
  std::string e;
  if (!update::parseReleases(r.body, rel, &e) || rel.empty()) {
    err = "the release information could not be read";
    return false;
  }
  out = rel.front();
  return true;
}

// Puts `fresh` in place of `target`: the old file goes to `previous` first and comes back on failure.
bool swapIn(const fs::path& fresh, const fs::path& target, const fs::path& previous, std::string& err) {
  std::error_code ec;
  const bool had = fs::exists(target, ec);
  if (had) {
    fs::remove(previous, ec);
    fs::rename(target, previous, ec);
    if (ec) {
      err = "could not move the old copy aside (" + ec.message() + ")";
      return false;
    }
  }
  fs::rename(fresh, target, ec);
  if (ec) {
    err = "could not put the new copy in place (" + ec.message() + ")";
    if (had) fs::rename(previous, target, ec);
    return false;
  }
  return true;
}

void writeText(const fs::path& p, const std::string& text) {
  std::ofstream f(p, std::ios::binary);
  f << text << "\n";
}

}  // namespace

const char* fromName(From f) {
  switch (f) {
    case From::None: return "none";
    case From::Override: return "override";
    case From::Installed: return "installed";
    case From::Bundled: return "bundled";
    case From::System: return "system";
  }
  return "?";
}

std::vector<std::string> Tool::argv() const {
  std::vector<std::string> a = prefix;
  a.push_back(path);
  return a;
}

std::string exeName(const std::string& base) {
#if defined(_WIN32)
  return base + ".exe";
#else
  return base;
#endif
}

std::string bundledDir() {
  const std::string exe = platform::executablePath();
  if (exe.empty()) return "";
  const fs::path dir = fs::u8path(exe).parent_path();
  std::error_code ec;
  for (const fs::path& c : {dir.parent_path() / "Resources" / "tools", dir / "tools", dir.parent_path() / "share" / "gridline" / "tools"})
    if (fs::is_directory(c, ec)) return c.u8string();
  return "";
}

std::string installDir() { return (fs::u8path(platform::dataRoot()) / "gridline" / "tools").u8string(); }

std::string findOnSystem(const std::string& name) {
  std::vector<fs::path> dirs;
#if defined(_WIN32)
  const char sep = ';';
#else
  const char sep = ':';
#endif
  const std::string path = envStr("PATH");
  std::stringstream ss(path);
  for (std::string d; std::getline(ss, d, sep);)
    if (!d.empty()) dirs.push_back(fs::u8path(d));
#if defined(_WIN32)
  if (const std::string la = envStr("LOCALAPPDATA"); !la.empty()) dirs.push_back(fs::u8path(la) / "Microsoft" / "WinGet" / "Links");
  if (const std::string up = envStr("USERPROFILE"); !up.empty()) dirs.push_back(fs::u8path(up) / "scoop" / "shims");
  if (const std::string pd = envStr("ProgramData"); !pd.empty()) dirs.push_back(fs::u8path(pd) / "chocolatey" / "bin");
#else
  for (const char* d : {"/opt/homebrew/bin", "/usr/local/bin", "/usr/bin", "/usr/pkg/bin", "/opt/local/bin", "/snap/bin"}) dirs.push_back(d);
#endif
  for (const fs::path& d : dirs) {
    if (runnable(d / exeName(name))) return (d / exeName(name)).u8string();
  }
  return "";
}

Tool locateYtdlp(const Search& s) {
  Tool t;
  if (const std::string o = envStr("GRIDLINE_YTDLP"); !o.empty()) {
    if (runnable(fs::u8path(o))) t.from = From::Override, t.path = o;
    return t;  // a set override never falls back (tests, a deliberate choice)
  }
  if (!s.installed.empty()) {
    const fs::path p = fs::u8path(s.installed) / exeName("yt-dlp");
    if (runnable(p)) return Tool{From::Installed, p.u8string(), {}};
    const fs::path pyz = fs::u8path(s.installed) / "yt-dlp.pyz";  // BSD: the zipapp and the system's python3
    std::error_code ec;
    if (fs::is_regular_file(pyz, ec)) {
      const std::string py = s.system ? findOnSystem("python3") : std::string();
      if (!py.empty()) return Tool{From::Installed, pyz.u8string(), {py}};
    }
  }
  if (!s.bundled.empty()) {
    const fs::path p = fs::u8path(s.bundled) / exeName("yt-dlp");
    if (runnable(p)) return Tool{From::Bundled, p.u8string(), {}};
  }
  if (s.system) {
    const std::string p = findOnSystem("yt-dlp");
    if (!p.empty()) return Tool{From::System, p, {}};
  }
  return t;
}

Tool locateFfmpeg(const Search& s) {
  Tool t;
  if (const std::string o = envStr("GRIDLINE_FFMPEG"); !o.empty()) {
    if (runnable(fs::u8path(o))) t.from = From::Override, t.path = o;
    return t;
  }
  if (!s.bundled.empty()) {
    const fs::path p = fs::u8path(s.bundled) / exeName("ffmpeg");
    if (runnable(p)) return Tool{From::Bundled, p.u8string(), {}};
  }
  if (!s.installed.empty()) {
    const fs::path p = fs::u8path(s.installed) / exeName("ffmpeg");
    if (runnable(p)) return Tool{From::Installed, p.u8string(), {}};
  }
  if (s.system) {
    const std::string p = findOnSystem("ffmpeg");
    if (!p.empty()) return Tool{From::System, p, {}};
  }
  return t;
}

std::string ytdlpAsset(const update::Target& t) {
  const bool arm = t.arch == "aarch64";
  if (t.os == "macos") return "yt-dlp_macos";  // universal (arm64 + x86_64)
  if (t.os == "windows") return arm ? "yt-dlp_arm64.exe" : "yt-dlp.exe";
  if (t.os == "linux") return arm ? "yt-dlp_linux_aarch64" : "yt-dlp_linux";
  if (isBsd(t)) return "yt-dlp";  // the zipapp (python3)
  return "";
}

std::string mediaToolsAsset(const std::string& version, const update::Target& t) {
  std::string arch;
  if (t.os == "macos") arch = "universal";
  else if (t.os == "windows") arch = t.arch == "aarch64" ? "arm64" : "x64";
  else if (t.os == "linux") arch = t.arch;
  else return "";
  return "Gridline-" + version + "-media-tools-" + t.os + "-" + arch + ".zip";
}

std::string runVersion(const std::vector<std::string>& argv, double seconds) {
  if (argv.empty()) return "";
  std::vector<std::string> a = argv;
  a.push_back(argv.back().find("ffmpeg") != std::string::npos ? "-version" : "--version");
  std::error_code ec;
  const fs::path tmp = fs::temp_directory_path(ec);
  const urlimport::RunResult r =
      urlimport::run(a, (tmp / ("gridline-ver-" + std::to_string(platform::processId()))).u8string(), nullptr, seconds, 0, "", nullptr);
  if (!r.ok) return "";
  const std::string first = r.out.substr(0, r.out.find_first_of("\r\n"));
  return first;
}

std::string recordedVersion(const std::string& dir) {
  std::ifstream f(fs::u8path(dir) / "yt-dlp.version", std::ios::binary);
  std::string v;
  std::getline(f, v);
  while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
  return v;
}

InstallResult installYtdlp(const Net& net, const std::string& dir, const Progress& progress, const std::atomic<bool>* cancel,
                           const std::string& assetName) {
  InstallResult out;
  const update::Target target = update::currentTarget();
  const std::string name = assetName.empty() ? ytdlpAsset(target) : assetName;
  if (name.empty()) {
    out.error = "yt-dlp has no download for this system; " + systemPackagesHint();
    return out;
  }
  const bool zipapp = name == "yt-dlp";
  std::string py;
  if (zipapp) {
    py = findOnSystem("python3");
    if (py.empty()) {
      out.error = "python3 is needed to run yt-dlp here; " + systemPackagesHint();
      return out;
    }
  }
  update::Release rel;
  if (!latestRelease(net, kYtdlpLatest, rel, out.error)) return out;
  const update::Asset* a = assetNamed(rel, name);
  const update::Asset* sums = assetNamed(rel, "SHA2-256SUMS");
  if (!a || !sums) {
    out.error = "the newest yt-dlp release has no " + std::string(!a ? name : "SHA2-256SUMS");
    return out;
  }
  const update::HttpResult sr = net.fetch(sums->url);
  if (sr.status != 200) {
    out.error = httpError(sr, "yt-dlp's checksums");
    return out;
  }
  const std::string want = update::checksumFor(sr.body, name);
  if (want.empty()) {
    out.error = name + " is not listed in yt-dlp's SHA2-256SUMS";
    return out;
  }
  std::error_code ec;
  const fs::path d = fs::u8path(dir);
  fs::create_directories(d, ec);
  const fs::path target_ = d / (zipapp ? std::string("yt-dlp.pyz") : exeName("yt-dlp"));
  // The new copy keeps its extension (Windows runs .exe files only) until it has run once.
  const fs::path part = d / (zipapp ? std::string("yt-dlp.new.pyz") : exeName("yt-dlp.new"));
  if (!fetchChecked(net, *a, want, part, progress, cancel, out.error)) return out;
  makeRunnable(part);
  // It must run before it replaces anything.
  std::vector<std::string> argv;
  if (zipapp) argv.push_back(py);
  argv.push_back(part.u8string());
  const std::string v = runVersion(argv);
  if (v.empty()) {
    fs::remove(part, ec);
    out.error = "the downloaded yt-dlp does not run on this computer";
    return out;
  }
  if (!swapIn(part, target_, fs::u8path(target_.u8string() + ".previous"), out.error)) {
    fs::remove(part, ec);
    return out;
  }
  writeText(d / "yt-dlp.version", rel.tag.empty() ? v : rel.tag);
  out.ok = true;
  out.path = target_.u8string();
  out.version = rel.tag.empty() ? v : rel.tag;
  return out;
}

InstallResult installFfmpeg(const Net& net, const std::string& dir, const Progress& progress, const std::atomic<bool>* cancel,
                            const update::Target& target) {
  InstallResult out;
  if (mediaToolsAsset("0", target).empty()) {
    out.error = "Gridline has no ffmpeg download for this system; " + systemPackagesHint();
    return out;
  }
  const update::HttpResult r = net.fetch(update::kListUrl);
  if (r.status != 200) {
    out.error = httpError(r, "Gridline's releases");
    return out;
  }
  std::vector<update::Release> rels;
  std::string e;
  if (!update::parseReleases(r.body, rels, &e)) {
    out.error = "Gridline's release list could not be read";
    return out;
  }
  // The newest release (betas too) that has the archive for this computer.
  const update::Release* pick = nullptr;
  const update::Asset* asset = nullptr;
  for (const auto& rel : rels) {
    if (rel.draft || !rel.version.valid) continue;
    const update::Asset* a = assetNamed(rel, mediaToolsAsset(update::toString(rel.version), target));
    if (!a || !update::checksumAsset(rel)) continue;
    if (!pick || update::compare(rel.version, pick->version) > 0) pick = &rel, asset = a;
  }
  if (!pick) {
    out.error = "no Gridline release has ffmpeg for this computer yet; " + systemPackagesHint();
    return out;
  }
  const update::Asset* sums = update::checksumAsset(*pick);
  const update::HttpResult sr = net.fetch(sums->url);
  if (sr.status != 200) {
    out.error = httpError(sr, "Gridline's checksums");
    return out;
  }
  const std::string want = update::checksumFor(sr.body, asset->name);
  if (want.empty()) {
    out.error = asset->name + " is not listed in " + sums->name;
    return out;
  }
  std::error_code ec;
  const fs::path d = fs::u8path(dir);
  fs::create_directories(d, ec);
  const fs::path part = d / (asset->name + ".part");
  if (!fetchChecked(net, *asset, want, part, progress, cancel, out.error)) return out;
  const fs::path unpack = d / ".media-tools-new";
  fs::remove_all(unpack, ec);
  std::string ze;
  if (!extractZip(part.u8string(), unpack.u8string(), nullptr, &ze)) {
    fs::remove(part, ec);
    fs::remove_all(unpack, ec);
    out.error = "the ffmpeg archive could not be unpacked: " + ze;
    return out;
  }
  fs::remove(part, ec);
  // The archive holds ffmpeg[.exe], ffprobe[.exe] and LICENSES/ (at the top or in one folder).
  fs::path root = unpack;
  if (!fs::exists(root / exeName("ffmpeg"), ec))
    for (fs::directory_iterator it(unpack, ec), end; !ec && it != end; it.increment(ec))
      if (it->is_directory() && fs::exists(it->path() / exeName("ffmpeg"))) root = it->path();
  const fs::path fresh = root / exeName("ffmpeg");
  if (!fs::is_regular_file(fresh, ec)) {
    fs::remove_all(unpack, ec);
    out.error = "the archive has no ffmpeg";
    return out;
  }
  makeRunnable(fresh);
  const std::string v = runVersion({fresh.u8string()});
  if (v.empty()) {
    fs::remove_all(unpack, ec);
    out.error = "the downloaded ffmpeg does not run on this computer";
    return out;
  }
  if (!swapIn(fresh, d / exeName("ffmpeg"), d / (exeName("ffmpeg") + ".previous"), out.error)) {
    fs::remove_all(unpack, ec);
    return out;
  }
  if (fs::is_regular_file(root / exeName("ffprobe"), ec)) {
    makeRunnable(root / exeName("ffprobe"));
    fs::remove(d / exeName("ffprobe"), ec);
    fs::rename(root / exeName("ffprobe"), d / exeName("ffprobe"), ec);
  }
  if (fs::is_directory(root / "LICENSES", ec)) {
    fs::remove_all(d / "LICENSES-ffmpeg", ec);
    fs::rename(root / "LICENSES", d / "LICENSES-ffmpeg", ec);
  }
  fs::remove_all(unpack, ec);
  out.ok = true;
  out.path = (d / exeName("ffmpeg")).u8string();
  out.version = v;
  return out;
}

bool updateDue(bool on, int64_t lastCheck, int64_t now) {
  if (!on) return false;
  return lastCheck <= 0 || now < lastCheck || now - lastCheck >= 24 * 3600;
}

UpdateResult updateYtdlp(const Net& net, const std::string& dir, const std::string& currentVersion, const std::atomic<bool>* cancel,
                         const std::string& asset) {
  UpdateResult u;
  update::Release rel;
  if (!latestRelease(net, kYtdlpLatest, rel, u.error)) return u;
  u.checked = true;
  u.version = currentVersion;
  if (!rel.tag.empty() && rel.tag == currentVersion) return u;  // current
  const InstallResult r = installYtdlp(net, dir, nullptr, cancel, asset);
  if (!r.ok) {
    u.error = r.error;
    return u;
  }
  u.updated = true;
  u.version = r.version;
  return u;
}

Net fakeNet(const std::string& toolPath, int stepMs) {
  const update::Target t = update::currentTarget();
  // The archive: the tool as ffmpeg (and ffprobe), with a licence folder.
  std::string zipBytes;
  {
    std::error_code ec;
    const fs::path zp = fs::temp_directory_path(ec) / ("gridline-fake-media-tools-" + std::to_string(platform::processId()) + ".zip");
    ZipWriter z;
    if (z.open(zp.u8string()) && z.addFile(exeName("ffmpeg"), toolPath) && z.addFile(exeName("ffprobe"), toolPath) &&
        z.add("LICENSES/README.txt", std::vector<uint8_t>{'f', 'a', 'k', 'e', '\n'}) && z.close()) {
      std::ifstream f(zp, std::ios::binary);
      std::stringstream ss;
      ss << f.rdbuf();
      zipBytes = ss.str();
    }
    fs::remove(zp, ec);
  }
  std::string toolBytes;
  {
    std::ifstream f(fs::u8path(toolPath), std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    toolBytes = ss.str();
  }
  const std::string yName = ytdlpAsset(t).empty() ? std::string("yt-dlp") : ytdlpAsset(t);
  update::Release y;
  y.tag = "2099.01.01";
  y.name = "yt-dlp 2099.01.01";
  y.assets = {{yName, "fake://yt-dlp/" + yName, static_cast<int64_t>(toolBytes.size())}, {"SHA2-256SUMS", "fake://yt-dlp/SHA2-256SUMS", 100}};
  const std::string ySums = update::sha256Hex(toolBytes) + "  " + yName + "\n";
  update::Release g;
  g.tag = "v9.9.9";
  g.name = "Gridline 9.9.9";
  const std::string gName = mediaToolsAsset("9.9.9", t);
  g.assets = {{gName.empty() ? std::string("none.zip") : gName, "fake://gridline/" + gName, static_cast<int64_t>(zipBytes.size())},
              {"Gridline-9.9.9-SHA256SUMS", "fake://gridline/SHA256SUMS", 100}};
  const std::string gSums = update::sha256Hex(zipBytes) + "  " + gName + "\n";
  const std::string yJson = update::releaseJson(y), gJson = "[" + update::releaseJson(g) + "]";
  Net n;
  n.fetch = [=](const std::string& url) {
    update::HttpResult r;
    r.status = 200;
    if (url == kYtdlpLatest) r.body = yJson;
    else if (url == update::kListUrl) r.body = gJson;
    else if (url == "fake://yt-dlp/SHA2-256SUMS") r.body = ySums;
    else if (url == "fake://gridline/SHA256SUMS") r.body = gSums;
    else r.status = 404;
    return r;
  };
  n.download = [=](const std::string& url, const std::string& path, const std::function<void(int64_t, int64_t)>& progress,
                   const std::atomic<bool>* cancel, std::string* err) {
    const std::string& bytes = url.rfind("fake://gridline/", 0) == 0 ? zipBytes : toolBytes;
    std::ofstream f(fs::u8path(path), std::ios::binary);
    const size_t step = bytes.size() / 10 + 1;
    for (size_t at = 0; at < bytes.size(); at += step) {
      if (cancel && cancel->load()) {
        if (err) *err = "cancelled";
        return false;
      }
      f.write(bytes.data() + at, static_cast<std::streamsize>(std::min(step, bytes.size() - at)));
      if (progress) progress(static_cast<int64_t>(std::min(at + step, bytes.size())), static_cast<int64_t>(bytes.size()));
      if (stepMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(stepMs));
    }
    return static_cast<bool>(f);
  };
  return n;
}

std::string systemPackagesHint() {
  const update::Target t = update::currentTarget();
  if (t.os == "freebsd") return "install them with: pkg install yt-dlp ffmpeg";
  if (t.os == "openbsd") return "install them with: pkg_add yt-dlp ffmpeg";
  if (t.os == "netbsd") return "install them with: pkgin install yt-dlp ffmpeg";
  if (t.os == "macos") return "or install them with: brew install yt-dlp ffmpeg";
  if (t.os == "windows") return "or install them with: winget install yt-dlp.yt-dlp Gyan.FFmpeg";
  return "or install them with your package manager (yt-dlp, ffmpeg)";
}

}  // namespace gl::mediatools
