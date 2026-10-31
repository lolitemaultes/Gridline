// Gridline - J-89: update check against the project's GitHub releases.
//
// Pure logic, no network and no UI (unit tested in tests/test_update.cpp with
// a fake fetcher): semantic versions, the GitHub release JSON, which release
// and which download fit this computer, the SHA256SUMS check, release notes
// as plain text, and the schedule (once a day, "Skip this version", "Remind
// me later"). The network requests live in host/net/Http.h; the popup and the
// settings in app/UpdateUi.cpp. Privacy and behaviour: docs/updates.md.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace gl {
class Settings;
struct Json;
}

namespace gl::update {

// ---- versions ("v0.6.1", "0.10.0", "0.7.0-beta.2") ----
struct Version {
  int major = 0, minor = 0, patch = 0;
  std::string pre;  // prerelease identifiers after '-' ("beta.2"), empty for a release
  bool valid = false;
};
Version parseVersion(const std::string& text);  // a leading 'v' / 'V' is allowed; "+build" metadata is ignored
// Semantic version precedence: < 0, 0, > 0. A prerelease sorts before its release;
// prerelease identifiers compare numerically when both are numbers, else as text.
int compare(const Version& a, const Version& b);
std::string toString(const Version& v);  // "0.6.1" / "0.7.0-beta.2"

// ---- releases (GitHub REST API: /repos/<owner>/<repo>/releases[/latest]) ----
inline constexpr const char* kRepo = "lolitemaultes/Gridline";
inline constexpr const char* kLatestUrl = "https://api.github.com/repos/lolitemaultes/Gridline/releases/latest";
inline constexpr const char* kListUrl = "https://api.github.com/repos/lolitemaultes/Gridline/releases?per_page=20";
inline constexpr const char* kReleasesPage = "https://github.com/lolitemaultes/Gridline/releases";

struct Asset {
  std::string name;
  std::string url;  // browser_download_url
  int64_t size = 0;
};
struct Release {
  std::string tag;      // tag_name ("v0.6.1")
  std::string name;     // title ("Gridline 0.6.1")
  std::string htmlUrl;  // release page
  std::string body;     // release notes (Markdown)
  std::string published;  // published_at (ISO 8601)
  bool draft = false;
  bool prerelease = false;
  std::vector<Asset> assets;
  Version version;  // parsed from tag
};
bool parseRelease(const Json& j, Release& out);  // false when it is not a release object
// A /releases/latest object or a /releases array. False (with `err`) on invalid JSON.
bool parseReleases(const std::string& json, std::vector<Release>& out, std::string* err = nullptr);
// The newest release that may be offered: not a draft, a valid version tag, and not a
// prerelease (by flag or tag) unless `beta`. nullptr when there is none.
const Release* pickRelease(const std::vector<Release>& releases, bool beta);

// ---- downloads for this computer ----
struct Target {
  Target() = default;
  Target(std::string o, std::string a, std::string p = {}) : os(std::move(o)), arch(std::move(a)), package(std::move(p)) {}
  std::string os;    // "macos", "windows", "linux", "freebsd", "openbsd", "netbsd"
  std::string arch;  // "x86_64", "aarch64"
  std::string package;  // Linux: the distribution's package format, "deb" or "rpm"; "" = none of ours
};
Target currentTarget();  // on Linux, `package` comes from /etc/os-release
// The package format of a Linux distribution from its os-release text (ID and ID_LIKE):
// "deb" for Debian, Ubuntu and their derivatives, "rpm" for Fedora, RHEL and openSUSE
// families, "" for the rest (Arch, Gentoo, ...).
std::string linuxPackage(const std::string& osRelease);
// The installer / package of `r` for `t` (nullptr: none, offer the release page):
//   macOS          Gridline-<v>-macos-universal.dmg
//   Windows        Gridline-<v>-windows-<x64|arm64>-setup.exe
//   Linux deb      Gridline-<v>-linux-<amd64|arm64>.deb
//   Linux rpm      Gridline-<v>-linux-<x86_64|aarch64>.rpm
//   other Linux    Gridline-<v>-linux-<x86_64|aarch64>.AppImage (also when the release has no package)
//   FreeBSD etc.   Gridline-<v>-<system>-<arch>.tar.gz
const Asset* pickAsset(const Release& r, const Target& t);
const Asset* checksumAsset(const Release& r);  // "...SHA256SUMS"
// The hex digest listed for `fileName` in a sha256sum file ("<hex>  <name>" or "<hex> *<name>"),
// lower case; empty when it is not listed.
std::string checksumFor(const std::string& sums, const std::string& fileName);

// SHA-256 (FIPS 180-4), lower-case hex.
class Sha256 {
 public:
  Sha256();
  void add(const void* data, size_t n);
  std::string hex();  // finishes; call once
 private:
  void block(const unsigned char* p);
  uint32_t h_[8];
  unsigned char buf_[64];
  size_t used_ = 0;
  uint64_t total_ = 0;
};
std::string sha256Hex(const std::string& data);
std::string sha256File(const std::string& path);  // empty when the file cannot be read

// Release notes for the popup: Markdown as plain text (images and HTML tags removed,
// links as their text, no heading / emphasis / code markers, list items as bullets),
// cut at a line end near `maxChars` with an ellipsis.
std::string notesExcerpt(const std::string& markdown, size_t maxChars = 1500);

// ---- the check ----
struct HttpResult {
  int status = 0;     // HTTP status; 0 = no response
  std::string body;
  std::string error;  // transport error text
  bool offline = false;  // could not reach the server (no network, DNS, timeout)
};
using Fetcher = std::function<HttpResult(const std::string& url)>;

enum class Status { UpToDate, Available, NoRelease, Offline, RateLimited, Failed, Disabled };
struct CheckResult {
  Status status = Status::Failed;
  Release release;      // Available: the offered release
  std::string current;  // the running version
  std::string detail;   // Failed: what went wrong (English, for logs and the CLI)
};
// One request: /releases/latest, or /releases when `beta`. Never throws.
CheckResult check(const Fetcher& fetch, const std::string& currentVersion, bool beta);
// One line for the command line ("Gridline 0.6.1 is available (you have 0.6.0): <url>").
std::string describe(const CheckResult& r);
// Request headers sent with every request (no identifiers): User-Agent and Accept.
std::string userAgent();  // "Gridline/<version>"

// ---- schedule ----
inline constexpr int64_t kCheckInterval = 24 * 3600;  // automatic checks: at most once a day
inline constexpr int64_t kRemindDelay = 24 * 3600;    // "Remind me later"
struct Prefs {
  bool autoCheck = true;    // update.auto
  bool beta = false;        // update.beta
  int64_t lastCheck = 0;    // update.lastCheck (seconds since 1970; 0 = never)
  std::string skipVersion;  // update.skipVersion ("0.6.1")
  int64_t remindAfter = 0;  // update.remindAfter
};
Prefs loadPrefs(const Settings& s);
void savePrefs(Settings& s, const Prefs& p);
// GRIDLINE_NO_UPDATE_CHECK (any value) or CI (automated builds) is set.
bool disabledByEnvironment();
// An automatic check is due at start-up: automatic checks on, not disabled, and the last
// check (or a clock set back) was at least kCheckInterval ago.
bool dueAtStartup(const Prefs& p, int64_t now, bool disabled);
// A release found by an automatic check is shown: not the skipped version and not within
// a "Remind me later" delay.
bool shouldOffer(const Prefs& p, const Release& r, int64_t now);
void remindLater(Prefs& p, int64_t now);
void skipVersion(Prefs& p, const Release& r);

// ---- downloading the installer ----
// Writes `url` to `path`; progress(received, total) from the calling thread; `cancel` polled.
using Downloader = std::function<bool(const std::string& url, const std::string& path,
                                      const std::function<void(int64_t, int64_t)>& progress, const std::atomic<bool>* cancel,
                                      std::string* err)>;
struct InstallerResult {
  bool ok = false;
  std::string path;   // the verified file
  std::string error;  // English
};
// Fetches the release's SHA256SUMS, downloads `asset` into `folder` (as <name>.part, renamed
// once the digest matches; an existing file of that name gets " (1)", " (2)", ...).
InstallerResult downloadInstaller(const Fetcher& fetch, const Downloader& download, const Release& r, const Asset& asset,
                                  const std::string& folder, const std::function<void(int64_t, int64_t)>& progress,
                                  const std::atomic<bool>* cancel);

// A made-up release for --fake-update and the tests: tag v<version>, notes with an image,
// a list and links, one asset per platform and a SHA256SUMS whose digests are those of
// fakeAssetBytes(name). Its URLs start with "fake://" (never fetched).
Release fakeRelease(const std::string& version);
std::string fakeAssetBytes(const std::string& name);
std::string fakeSums(const Release& r);
Fetcher fakeFetcher(const Release& r);       // answers the release JSON and its SHA256SUMS
Downloader fakeDownloader();                 // writes fakeAssetBytes(name) in a few steps
std::string releaseJson(const Release& r);   // GitHub-shaped JSON (tests, fakes)

}  // namespace gl::update
