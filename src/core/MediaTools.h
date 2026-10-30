// Gridline - J-192: the two programs Import from URL runs (yt-dlp, ffmpeg): where they are,
// installing them on first use and keeping yt-dlp current.
//
// Where they come from, first match wins:
//   yt-dlp   GRIDLINE_YTDLP > the copy Gridline installed or updated (data folder) > the copy
//            inside the app > the system's (PATH, Homebrew, ...)
//   ffmpeg   GRIDLINE_FFMPEG > the copy inside the app > the copy Gridline installed > the system's
// (yt-dlp's updated copy comes before the one inside the app: sites change often and the app's
// own folder is read-only and signed; ffmpeg is not updated.)
//
// The copy inside the app: release packages carry both in a "tools" folder
// (macOS Gridline.app/Contents/Resources/tools, Windows <install folder>/tools, Linux
// share/gridline/tools), made by tools/release/fetch-media-tools.py from pinned, checked sources.
// The automatic install (builds without that folder): yt-dlp from yt-dlp's GitHub releases,
// checked against that release's SHA2-256SUMS; ffmpeg from the newest Gridline release's
// "media-tools" archive for this computer, checked against Gridline's SHA256SUMS. Both into
// <data folder>/gridline/tools. Nothing is replaced until the new file is checked and runs.
// BSD systems (no official binaries): the system's programs, else yt-dlp's Python zipapp run by
// the system's python3; ffmpeg from the system's packages.
//
// The network is injected (update::Fetcher / update::Downloader), so the tests run offline.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "Update.h"

namespace gl::mediatools {

enum class From { None, Override, Installed, Bundled, System };
const char* fromName(From f);  // "override", "installed", "bundled", "system", "none"

struct Tool {
  From from = From::None;
  std::string path;                 // the program ("" = not found)
  std::vector<std::string> prefix;  // run as prefix + [path]: {"/usr/bin/python3"} for the zipapp
  std::vector<std::string> argv() const;  // prefix + path
  bool found() const { return !path.empty(); }
};

// Folders. bundledDir: the "tools" folder of the running app ("" when there is none);
// installDir: <data folder>/gridline/tools.
std::string bundledDir();
std::string installDir();
std::string exeName(const std::string& base);  // "yt-dlp" / "yt-dlp.exe"
// The first executable `name` on PATH or in the usual places (/opt/homebrew/bin, /usr/local/bin,
// /usr/bin, /usr/pkg/bin, %LOCALAPPDATA%\Microsoft\WinGet\Links, scoop, chocolatey).
std::string findOnSystem(const std::string& name);

// `installed` / `bundled` default to installDir() / bundledDir(); `system` false skips the search.
struct Search {
  std::string installed, bundled;
  bool system = true;
};
Tool locateYtdlp(const Search& s);
Tool locateFfmpeg(const Search& s);
inline Tool locateYtdlp() { return locateYtdlp(Search{installDir(), bundledDir(), true}); }
inline Tool locateFfmpeg() { return locateFfmpeg(Search{installDir(), bundledDir(), true}); }

// ---- downloads for this computer ----
// yt-dlp's release asset: yt-dlp_macos (universal), yt-dlp.exe / yt-dlp_arm64.exe,
// yt-dlp_linux / yt-dlp_linux_aarch64; "" elsewhere (BSD: the zipapp "yt-dlp" needs python3).
std::string ytdlpAsset(const update::Target& t);
// Gridline's archive with ffmpeg and ffprobe: Gridline-<version>-media-tools-<os>-<arch>.zip
// (macOS: "universal"; Windows: x64 / arm64), "" for systems without one.
std::string mediaToolsAsset(const std::string& version, const update::Target& t);
inline constexpr const char* kYtdlpLatest = "https://api.github.com/repos/yt-dlp/yt-dlp/releases/latest";

using Progress = std::function<void(int64_t got, int64_t total)>;
struct Net {
  update::Fetcher fetch;
  update::Downloader download;
};
// Runs `<tool> --version` (up to `seconds`); its first line, "" when it does not run.
std::string runVersion(const std::vector<std::string>& argv, double seconds = 60);

struct InstallResult {
  bool ok = false;
  std::string path, version, error;  // error: English, one line
};
// The newest yt-dlp for this computer into `dir` (yt-dlp[.exe], yt-dlp.version). `asset` ""
// = ytdlpAsset(currentTarget()). An existing copy is kept as yt-dlp.previous until the new one
// is checked and runs; then it is replaced in one rename.
InstallResult installYtdlp(const Net& net, const std::string& dir, const Progress& progress, const std::atomic<bool>* cancel,
                           const std::string& asset = "");
// ffmpeg (and ffprobe) from the newest Gridline release that has a media-tools archive for
// this computer (`asset` "" = mediaToolsAsset(<that release>, currentTarget())).
InstallResult installFfmpeg(const Net& net, const std::string& dir, const Progress& progress, const std::atomic<bool>* cancel,
                            const update::Target& target);
// Daily yt-dlp update: due when on and the last check is a day old (or the clock went back).
bool updateDue(bool on, int64_t lastCheck, int64_t now);
struct UpdateResult {
  bool checked = false, updated = false;
  std::string version, error;
};
// Asks for the newest yt-dlp release; when its tag differs from `currentVersion`, installs it into
// `dir` (installYtdlp). Never touches the copy inside the app.
UpdateResult updateYtdlp(const Net& net, const std::string& dir, const std::string& currentVersion, const std::atomic<bool>* cancel,
                         const std::string& asset = "");
// The version a folder's yt-dlp.version file names ("" when none).
std::string recordedVersion(const std::string& dir);
// Tests and screenshots (no network): answers as GitHub does for yt-dlp's newest release and
// Gridline's release list; every download is `toolPath` (a program that acts as yt-dlp and as
// ffmpeg; the media-tools archive holds it as ffmpeg), written in 10 steps of `stepMs`.
Net fakeNet(const std::string& toolPath, int stepMs = 0);

// One line for systems Gridline cannot install the programs on ("pkg install yt-dlp ffmpeg").
std::string systemPackagesHint();

}  // namespace gl::mediatools
