// Gridline - J-89: update check tests. No network: every request goes to a fake
// fetcher (the real one is never called here).
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "Check.h"
#include "GridlineVersion.h"
#include "core/Platform.h"
#include "core/Settings.h"
#include "core/Update.h"
#include "host/net/Http.h"

using namespace gl;
using namespace gl::update;
namespace fs = std::filesystem;

namespace {

int cmp(const char* a, const char* b) { return compare(parseVersion(a), parseVersion(b)); }

// A /releases/latest answer shaped like GitHub's (extra fields kept, as captured).
const char* kLatestSample = R"JSON({
  "url": "https://api.github.com/repos/lolitemaultes/Gridline/releases/180000001",
  "assets_url": "https://api.github.com/repos/lolitemaultes/Gridline/releases/180000001/assets",
  "html_url": "https://github.com/lolitemaultes/Gridline/releases/tag/v0.6.1",
  "id": 180000001,
  "author": {"login": "lolitemaultes", "id": 1, "type": "User"},
  "tag_name": "v0.6.1",
  "target_commitish": "main",
  "name": "Gridline 0.6.1",
  "draft": false,
  "prerelease": false,
  "created_at": "2026-11-02T09:41:12Z",
  "published_at": "2026-11-02T10:00:00Z",
  "assets": [
    {"id": 1, "name": "Gridline-0.6.1-macos-universal.dmg", "content_type": "application/x-apple-diskimage", "state": "uploaded",
     "size": 61234567, "download_count": 0, "browser_download_url": "https://github.com/lolitemaultes/Gridline/releases/download/v0.6.1/Gridline-0.6.1-macos-universal.dmg"},
    {"id": 2, "name": "Gridline-0.6.1-windows-x64-setup.exe", "size": 40111222,
     "browser_download_url": "https://github.com/lolitemaultes/Gridline/releases/download/v0.6.1/Gridline-0.6.1-windows-x64-setup.exe"},
    {"id": 3, "name": "Gridline-0.6.1-windows-arm64-setup.exe", "size": 39111222,
     "browser_download_url": "https://github.com/lolitemaultes/Gridline/releases/download/v0.6.1/Gridline-0.6.1-windows-arm64-setup.exe"},
    {"id": 4, "name": "Gridline-0.6.1-windows-arm64.zip", "size": 39000000,
     "browser_download_url": "https://github.com/lolitemaultes/Gridline/releases/download/v0.6.1/Gridline-0.6.1-windows-arm64.zip"},
    {"id": 5, "name": "Gridline-0.6.1-linux-x86_64.AppImage", "size": 70000000,
     "browser_download_url": "https://github.com/lolitemaultes/Gridline/releases/download/v0.6.1/Gridline-0.6.1-linux-x86_64.AppImage"},
    {"id": 6, "name": "Gridline-0.6.1-linux-aarch64.AppImage", "size": 69000000,
     "browser_download_url": "https://github.com/lolitemaultes/Gridline/releases/download/v0.6.1/Gridline-0.6.1-linux-aarch64.AppImage"},
    {"id": 7, "name": "Gridline-0.6.1-linux-x86_64.tar.gz", "size": 50000000,
     "browser_download_url": "https://github.com/lolitemaultes/Gridline/releases/download/v0.6.1/Gridline-0.6.1-linux-x86_64.tar.gz"},
    {"id": 8, "name": "Gridline-0.6.1-linux-x86_64.deb", "size": 50000000,
     "browser_download_url": "https://github.com/lolitemaultes/Gridline/releases/download/v0.6.1/Gridline-0.6.1-linux-x86_64.deb"},
    {"id": 9, "name": "Gridline-0.6.1-freebsd-x86_64.tar.gz", "size": 48000000,
     "browser_download_url": "https://github.com/lolitemaultes/Gridline/releases/download/v0.6.1/Gridline-0.6.1-freebsd-x86_64.tar.gz"},
    {"id": 10, "name": "Gridline-0.6.1-openbsd-x86_64.tar.gz", "size": 48000000,
     "browser_download_url": "https://github.com/lolitemaultes/Gridline/releases/download/v0.6.1/Gridline-0.6.1-openbsd-x86_64.tar.gz"},
    {"id": 11, "name": "Gridline-0.6.1-netbsd-x86_64.tar.gz", "size": 48000000,
     "browser_download_url": "https://github.com/lolitemaultes/Gridline/releases/download/v0.6.1/Gridline-0.6.1-netbsd-x86_64.tar.gz"},
    {"id": 12, "name": "Gridline-0.6.1-SHA256SUMS", "size": 1100,
     "browser_download_url": "https://github.com/lolitemaultes/Gridline/releases/download/v0.6.1/Gridline-0.6.1-SHA256SUMS"}
  ],
  "tarball_url": "https://api.github.com/repos/lolitemaultes/Gridline/tarball/v0.6.1",
  "zipball_url": "https://api.github.com/repos/lolitemaultes/Gridline/zipball/v0.6.1",
  "body": "![Gridline](https://github.com/lolitemaultes/Gridline/raw/main/docs/images/hero.png)\r\n\r\n## Highlights\r\n\r\n- **Update check**: Gridline tells you when a new version is out.\r\n- Faster `render` of long songs\r\n\r\n<img width=\"600\" src=\"https://example.invalid/x.png\">\r\n\r\nSee the [changelog](https://github.com/lolitemaultes/Gridline/blob/main/CHANGELOG.md) &amp; docs."
})JSON";

void versions() {
  expect(parseVersion("v0.6.0").valid && parseVersion("0.6.0").major == 0 && parseVersion("0.6.0").minor == 6, "parse v0.6.0");
  expect(parseVersion("V1.2").valid && parseVersion("V1.2").patch == 0, "parse two-part version");
  expect(parseVersion("0.7.0-beta.2").pre == "beta.2", "prerelease identifiers");
  expect(parseVersion("1.0.0+build.5").valid && parseVersion("1.0.0+build.5").pre.empty(), "build metadata ignored");
  expect(!parseVersion("").valid && !parseVersion("latest").valid && !parseVersion("v1.2.x").valid && !parseVersion("1..2").valid &&
             !parseVersion("1.2.3-").valid && !parseVersion("1.2.3.4").valid,
         "invalid versions rejected");
  expect(cmp("0.6.0", "0.10.0") < 0, "0.6.0 < 0.10.0 (numeric, not text)");
  expect(cmp("0.10.0", "0.9.9") > 0, "0.10.0 > 0.9.9");
  expect(cmp("v0.6.1", "0.6.0") > 0, "0.6.1 > 0.6.0");
  expect(cmp("0.6.0", "v0.6.0") == 0, "v prefix does not matter");
  expect(cmp("1.0", "1.0.0") == 0, "1.0 == 1.0.0");
  expect(cmp("0.7.0-beta.1", "0.7.0") < 0, "prerelease < its release");
  expect(cmp("0.7.0-beta.1", "0.6.9") > 0, "prerelease > older release");
  expect(cmp("0.7.0-beta.2", "0.7.0-beta.10") < 0, "beta.2 < beta.10");
  const char* order[] = {"1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta", "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0"};
  bool sorted = true;
  for (size_t i = 0; i + 1 < sizeof order / sizeof order[0]; ++i) sorted = sorted && cmp(order[i], order[i + 1]) < 0 && cmp(order[i + 1], order[i]) > 0;
  expect(sorted, "semver 2.0 precedence example");
  expect(toString(parseVersion("v0.7.0-rc.1")) == "0.7.0-rc.1", "toString");
  expect(parseVersion(GRIDLINE_VERSION).valid, "the compiled version parses");
}

void parsing() {
  std::vector<Release> rs;
  std::string err;
  expect(parseReleases(kLatestSample, rs, &err) && rs.size() == 1, "captured /releases/latest sample parses");
  const Release& r = rs[0];
  expect(r.tag == "v0.6.1" && r.version.valid && toString(r.version) == "0.6.1", "tag and version");
  expect(r.htmlUrl == "https://github.com/lolitemaultes/Gridline/releases/tag/v0.6.1" && r.name == "Gridline 0.6.1", "page and title");
  expect(r.assets.size() == 12 && r.assets[0].size == 61234567, "assets with sizes");
  expect(!r.draft && !r.prerelease, "flags");
  auto pick = [&](const char* os, const char* arch) {
    const Asset* a = pickAsset(r, Target{os, arch});
    return a ? a->name : std::string("-");
  };
  expect(pick("macos", "aarch64") == "Gridline-0.6.1-macos-universal.dmg" && pick("macos", "x86_64") == "Gridline-0.6.1-macos-universal.dmg",
         "macOS: the universal disk image");
  expect(pick("windows", "x86_64") == "Gridline-0.6.1-windows-x64-setup.exe", "Windows x64: setup");
  expect(pick("windows", "aarch64") == "Gridline-0.6.1-windows-arm64-setup.exe", "Windows ARM64: setup (not the zip)");
  expect(pick("linux", "x86_64") == "Gridline-0.6.1-linux-x86_64.AppImage", "Linux x86_64: AppImage");
  expect(pick("linux", "aarch64") == "Gridline-0.6.1-linux-aarch64.AppImage", "Linux aarch64: AppImage");
  expect(pick("freebsd", "x86_64") == "Gridline-0.6.1-freebsd-x86_64.tar.gz" && pick("openbsd", "x86_64") == "Gridline-0.6.1-openbsd-x86_64.tar.gz" &&
             pick("netbsd", "x86_64") == "Gridline-0.6.1-netbsd-x86_64.tar.gz",
         "BSD: the tarball of the system");
  expect(pick("freebsd", "aarch64") == "-" && pick("haiku", "x86_64") == "-" && pick("linux", "unknown") == "-", "no asset: release page only");
  expect(checksumAsset(r) && checksumAsset(r)->name == "Gridline-0.6.1-SHA256SUMS", "SHA256SUMS asset");
  const Target here = currentTarget();
  expect(!here.os.empty() && here.os != "unknown", "current system is known");

  // Notes: plain text, no images.
  const std::string n = notesExcerpt(r.body);
  expect(n.find("Highlights") == 0, "heading text first, image line gone");
  expect(n.find("![") == std::string::npos && n.find("<img") == std::string::npos && n.find("hero.png") == std::string::npos, "images stripped");
  expect(n.find("\xE2\x80\xA2 Update check: Gridline tells you") != std::string::npos, "bullets, emphasis removed");
  expect(n.find("See the changelog & docs.") != std::string::npos, "links as text, entities decoded");
  expect(n.find('`') == std::string::npos && n.find('\r') == std::string::npos && n.find("##") == std::string::npos, "no markers");
  expect(n.find("\n\n\n") == std::string::npos, "blank lines collapsed");
  std::string longBody;
  for (int i = 0; i < 200; ++i) longBody += "- item number " + std::to_string(i) + " of a long list\n";
  const std::string cut = notesExcerpt(longBody, 300);
  expect(cut.size() <= 300 + 4 && cut.size() > 150 && cut.substr(cut.size() - 3) == "\xE2\x80\xA6", "long notes cut with an ellipsis");
  expect(notesExcerpt("#\n####\n").empty(), "marker-only headings");

  // A /releases list: drafts and prereleases.
  Release beta = fakeRelease("0.7.0-beta.1"), draft = fakeRelease("0.8.0"), stable = fakeRelease("0.6.2"), junk = fakeRelease("0.9.0");
  draft.draft = true;
  junk.tag = "nightly";
  junk.version = parseVersion(junk.tag);
  Release flagged = fakeRelease("0.6.5");
  flagged.prerelease = true;  // flagged in GitHub, plain tag
  const std::string list = "[" + releaseJson(beta) + "," + releaseJson(draft) + "," + releaseJson(stable) + "," + releaseJson(junk) + "," +
                           releaseJson(flagged) + "]";
  expect(parseReleases(list, rs) && rs.size() == 5, "release list parses");
  expect(pickRelease(rs, false) && toString(pickRelease(rs, false)->version) == "0.6.2", "stable channel: newest release, no draft / prerelease");
  expect(pickRelease(rs, true) && toString(pickRelease(rs, true)->version) == "0.7.0-beta.1", "beta channel includes prereleases, never drafts");
  expect(!parseReleases("{\"message\": \"Not Found\"}", rs, &err) && err.find("Not Found") != std::string::npos, "API error object");
  expect(!parseReleases("<html>", rs, &err), "not JSON");
  expect(parseReleases("[]", rs) && rs.empty() && !pickRelease(rs, true), "empty list");
}

void checksums() {
  expect(sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256 empty");
  expect(sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256 abc");
  expect(sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
             "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
         "sha256 two blocks");
  expect(sha256Hex(std::string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "sha256 million a");
  {
    Sha256 s;  // fed in odd pieces
    const std::string m(1000, 'q');
    for (size_t i = 0; i < m.size(); i += 7) s.add(m.data() + i, std::min<size_t>(7, m.size() - i));
    expect(s.hex() == sha256Hex(m), "sha256 incremental");
  }
  const std::string sums =
      "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855  Gridline-0.6.1-linux-x86_64.AppImage\r\n"
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad *Gridline-0.6.1-macos-universal.dmg\n"
      "garbage line\n"
      "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1  dist/Gridline-0.6.1-windows-x64-setup.exe\n";
  expect(checksumFor(sums, "Gridline-0.6.1-linux-x86_64.AppImage") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
         "sums: text mode, upper case");
  expect(checksumFor(sums, "Gridline-0.6.1-macos-universal.dmg") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
         "sums: binary mode marker");
  expect(checksumFor(sums, "Gridline-0.6.1-windows-x64-setup.exe").size() == 64, "sums: path prefix");
  expect(checksumFor(sums, "Gridline-0.6.1-linux-x86_64.AppImag").empty() && checksumFor("", "x").empty(), "sums: not listed");
}

struct CountingFetcher {
  std::vector<std::string> urls;
  HttpResult answer;
  Fetcher fn() {
    return [this](const std::string& url) {
      urls.push_back(url);
      return answer;
    };
  }
};

void checking() {
  const Release r = fakeRelease("0.6.1");
  CheckResult c = check(fakeFetcher(r), "0.6.0", false);
  expect(c.status == Status::Available && toString(c.release.version) == "0.6.1", "0.6.1 offered to 0.6.0");
  expect(describe(c) == "Gridline 0.6.1 is available (you have 0.6.0): https://github.com/lolitemaultes/Gridline/releases/tag/v0.6.1",
         "available text");
  c = check(fakeFetcher(r), "0.6.1", false);
  expect(c.status == Status::UpToDate && describe(c) == "You're up to date (0.6.1)", "same version: up to date");
  expect(check(fakeFetcher(r), "0.10.0", false).status == Status::UpToDate, "0.10.0 is newer than 0.6.1");
  expect(check(fakeFetcher(fakeRelease("0.10.0")), "0.6.0", false).status == Status::Available, "0.10.0 offered to 0.6.0");

  CountingFetcher f;
  f.answer.status = 200;
  f.answer.body = kLatestSample;
  c = check(f.fn(), "0.6.0", false);
  expect(c.status == Status::Available && f.urls.size() == 1 && f.urls[0] == kLatestUrl, "one request to releases/latest");
  f.urls.clear();
  f.answer.body = "[" + releaseJson(fakeRelease("0.7.0-beta.1")) + "]";
  c = check(f.fn(), "0.6.0", true);
  expect(c.status == Status::Available && f.urls.size() == 1 && f.urls[0] == kListUrl, "beta: one request to the release list");
  expect(check(f.fn(), "0.6.0", false).status == Status::NoRelease, "only a prerelease, stable channel: nothing");

  f.answer = {};
  f.answer.status = 404;
  f.answer.body = "{\"message\":\"Not Found\"}";
  c = check(f.fn(), "0.6.0", false);
  expect(c.status == Status::NoRelease && describe(c).find("up to date (0.6.0)") != std::string::npos, "no release yet (404)");
  f.answer.status = 403;
  expect(check(f.fn(), "0.6.0", false).status == Status::RateLimited, "403: rate limited");
  f.answer.status = 429;
  expect(check(f.fn(), "0.6.0", false).status == Status::RateLimited, "429: rate limited");
  f.answer.status = 500;
  expect(check(f.fn(), "0.6.0", false).status == Status::Failed, "500: failed");
  f.answer = {};
  f.answer.offline = true;
  f.answer.error = "no connection";
  expect(check(f.fn(), "0.6.0", false).status == Status::Offline, "offline");
  f.answer = {};
  f.answer.status = 200;
  f.answer.body = "{not json";
  expect(check(f.fn(), "0.6.0", false).status == Status::Failed, "bad JSON: failed");
  Fetcher thrower = [](const std::string&) -> HttpResult { throw 1; };
  expect(check(thrower, "0.6.0", false).status == Status::Failed, "a throwing fetcher is contained");
  expect(check(fakeFetcher(r), "dev", false).status == Status::Failed, "unparsable running version");
  expect(!http::detail::httpsOnly("http://api.github.com/") && http::detail::httpsOnly(kLatestUrl), "HTTPS only");
  expect(userAgent() == std::string("Gridline/") + GRIDLINE_VERSION, "user agent");
}

// J-125: the compiled version (GRIDLINE_VERSION = numbers + "-" + label) and the update
// decisions around the real 0.7.0-beta.1 prerelease.
void builtVersion() {
  const std::string pre = GRIDLINE_VERSION_PRE, numeric = GRIDLINE_VERSION_NUMERIC;
  expect(std::string(GRIDLINE_VERSION) == (pre.empty() ? numeric : numeric + "-" + pre), "full version = numbers [+ '-' label]");
  const Version v = parseVersion(GRIDLINE_VERSION), n = parseVersion(numeric);
  expect(v.valid && v.major == GRIDLINE_VERSION_MAJOR && v.minor == GRIDLINE_VERSION_MINOR && v.patch == GRIDLINE_VERSION_PATCH,
         "the version's numbers are project(VERSION)");
  expect(v.pre == pre && n.valid && n.pre.empty() && toString(v) == GRIDLINE_VERSION, "label split off, round trip");
  expect(std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.patch) == numeric, "numbers only: x.y.z");
  expect(pre.empty() ? compare(v, n) == 0 : compare(v, n) < 0, "a prerelease sorts before its release");
  const Version b = parseVersion("0.7.0-beta.1");
  expect(b.valid && b.major == 0 && b.minor == 7 && b.patch == 0 && b.pre == "beta.1" && toString(b) == "0.7.0-beta.1", "0.7.0-beta.1 parts");
}

// GitHub, as the update check sees it: /releases/latest answers the newest release that is not a
// draft or prerelease (a prerelease is published with --latest=false), /releases all of them.
Fetcher github(const std::vector<Release>& rs) {
  return [rs](const std::string& url) {
    HttpResult h;
    h.status = 200;
    if (url == kListUrl) {
      h.body = "[";
      for (size_t i = 0; i < rs.size(); ++i) h.body += (i ? "," : "") + releaseJson(rs[i]);
      h.body += "]";
    } else if (url == kLatestUrl) {
      const Release* latest = nullptr;
      for (const Release& r : rs)
        if (!r.draft && !r.prerelease && (!latest || compare(r.version, latest->version) > 0)) latest = &r;
      if (latest) h.body = releaseJson(*latest);
      else h.status = 404;
    } else {
      h.status = 404;
    }
    return h;
  };
}

std::string offered(const std::vector<Release>& rs, const char* current, bool beta) {
  const CheckResult c = check(github(rs), current, beta);
  if (c.status == Status::Available) return toString(c.release.version);
  return c.status == Status::UpToDate || c.status == Status::NoRelease ? "-" : "error";
}

void prereleases() {
  const Release v061 = fakeRelease("0.6.1"), b1 = fakeRelease("0.7.0-beta.1"), b2 = fakeRelease("0.7.0-beta.2"), v070 = fakeRelease("0.7.0");
  expect(b1.prerelease && b1.tag == "v0.7.0-beta.1" && !v061.prerelease, "the beta is a GitHub pre-release tagged v0.7.0-beta.1");

  // The beta is out; 0.6.1 is the newest release.
  const std::vector<Release> now = {b1, v061};
  expect(offered(now, "0.6.1", false) == "-", "0.6.1, beta off: 0.7.0-beta.1 is not offered");
  expect(offered(now, "0.6.1", true) == "0.7.0-beta.1", "0.6.1, beta on: 0.7.0-beta.1 is offered");
  expect(offered(now, "0.6.0", false) == "0.6.1" && offered(now, "0.5.1", false) == "0.6.1", "older stable users, beta off: 0.6.1, not the beta");
  expect(offered(now, "0.7.0-beta.1", false) == "-" && offered(now, "0.7.0-beta.1", true) == "-", "the beta itself is up to date");
  expect(offered({v061}, "0.7.0-beta.1", false) == "-" && offered({v061}, "0.7.0-beta.1", true) == "-",
         "a beta user is never offered an older stable (0.6.1)");

  // A second beta.
  const std::vector<Release> second = {b2, b1, v061};
  expect(offered(second, "0.7.0-beta.1", true) == "0.7.0-beta.2", "0.7.0-beta.1, beta on: 0.7.0-beta.2 is offered");
  expect(offered(second, "0.7.0-beta.1", false) == "-", "0.7.0-beta.1, beta off: no beta, no older stable");
  expect(offered(second, "0.6.1", true) == "0.7.0-beta.2", "0.6.1, beta on: the newest beta");

  // 0.7.0 final.
  const std::vector<Release> released = {v070, b2, b1, v061};
  expect(offered(released, "0.7.0-beta.1", false) == "0.7.0", "0.7.0-beta.1, beta off: 0.7.0 is offered");
  expect(offered(released, "0.7.0-beta.1", true) == "0.7.0", "0.7.0-beta.1, beta on: 0.7.0 is offered");
  expect(offered(released, "0.7.0-beta.2", false) == "0.7.0", "0.7.0-beta.2: 0.7.0 is offered");
  expect(offered(released, "0.6.1", false) == "0.7.0" && offered(released, "0.6.1", true) == "0.7.0", "0.6.1: 0.7.0 on both channels");
  expect(offered(released, "0.7.0", true) == "-" && offered(released, "0.7.0", false) == "-", "0.7.0 is not offered its betas");

  // Even if a prerelease were marked latest by mistake, the stable channel skips it.
  Fetcher wrongLatest = [&](const std::string& url) {
    HttpResult h;
    h.status = 200;
    h.body = url == kLatestUrl ? releaseJson(b1) : "[]";
    return h;
  };
  expect(check(wrongLatest, "0.6.1", false).status == Status::NoRelease, "a prerelease answered as latest is not offered on the stable channel");
  Release unflagged = b1;
  unflagged.prerelease = false;  // flag missing, the tag still says beta
  expect(offered({unflagged, v061}, "0.6.1", true) == "0.7.0-beta.1", "beta on: a beta tag is offered without the flag");
  const std::vector<Release> mixed = {unflagged, v061};
  const Release* stablePick = pickRelease(mixed, false);
  expect(stablePick && toString(stablePick->version) == "0.6.1", "beta off: a beta tag is a prerelease even without the flag");

  // The beta's downloads (the release workflow's file names) are found for every system.
  Release real = b1;
  real.assets.clear();
  for (const char* n : {"Gridline-0.7.0-beta.1-macos-universal.dmg", "Gridline-0.7.0-beta.1-windows-x64-setup.exe",
                        "Gridline-0.7.0-beta.1-windows-x64.zip", "Gridline-0.7.0-beta.1-windows-arm64-setup.exe",
                        "Gridline-0.7.0-beta.1-windows-arm64.zip", "Gridline-0.7.0-beta.1-linux-x86_64.AppImage",
                        "Gridline-0.7.0-beta.1-linux-aarch64.AppImage", "Gridline-0.7.0-beta.1-linux-x86_64.tar.gz",
                        "Gridline-0.7.0-beta.1-linux-amd64.deb", "Gridline-0.7.0-beta.1-linux-x86_64.rpm",
                        "Gridline-0.7.0-beta.1-freebsd-x86_64.tar.gz", "Gridline-0.7.0-beta.1-openbsd-x86_64.tar.gz",
                        "Gridline-0.7.0-beta.1-netbsd-x86_64.tar.gz", "Gridline-0.7.0-beta.1-SHA256SUMS"}) {
    Asset a;
    a.name = n;
    a.url = std::string("https://github.com/lolitemaultes/Gridline/releases/download/v0.7.0-beta.1/") + n;
    real.assets.push_back(a);
  }
  auto pick = [&](const char* os, const char* arch) {
    const Asset* a = pickAsset(real, Target{os, arch});
    return a ? a->name : std::string("-");
  };
  expect(pick("macos", "aarch64") == "Gridline-0.7.0-beta.1-macos-universal.dmg", "beta: macOS disk image");
  expect(pick("windows", "x86_64") == "Gridline-0.7.0-beta.1-windows-x64-setup.exe" &&
             pick("windows", "aarch64") == "Gridline-0.7.0-beta.1-windows-arm64-setup.exe",
         "beta: Windows setups");
  expect(pick("linux", "x86_64") == "Gridline-0.7.0-beta.1-linux-x86_64.AppImage" &&
             pick("linux", "aarch64") == "Gridline-0.7.0-beta.1-linux-aarch64.AppImage",
         "beta: Linux AppImages");
  expect(pick("freebsd", "x86_64") == "Gridline-0.7.0-beta.1-freebsd-x86_64.tar.gz" &&
             pick("netbsd", "x86_64") == "Gridline-0.7.0-beta.1-netbsd-x86_64.tar.gz",
         "beta: BSD tarballs");
  expect(checksumAsset(real) && checksumAsset(real)->name == "Gridline-0.7.0-beta.1-SHA256SUMS", "beta: SHA256SUMS");
}

void schedule() {
  const int64_t day = 24 * 3600, t0 = 1800000000;
  Prefs p;
  expect(dueAtStartup(p, t0, false), "never checked: due");
  p.lastCheck = t0;
  expect(!dueAtStartup(p, t0 + 3600, false), "1 h later: not due");
  expect(!dueAtStartup(p, t0 + day - 1, false), "just under 24 h: not due");
  expect(dueAtStartup(p, t0 + day, false), "24 h later: due");
  expect(dueAtStartup(p, t0 - 100, false), "clock set back: due");
  expect(!dueAtStartup(p, t0 + 2 * day, true), "disabled: never due");
  p.autoCheck = false;
  expect(!dueAtStartup(p, t0 + 2 * day, false), "automatic checks off: never due");

  Prefs q;
  const Release r061 = fakeRelease("0.6.1"), r062 = fakeRelease("0.6.2");
  expect(shouldOffer(q, r061, t0), "offered by default");
  skipVersion(q, r061);
  expect(q.skipVersion == "0.6.1" && !shouldOffer(q, r061, t0) && !shouldOffer(q, r061, t0 + 30 * day), "skipped version stays skipped");
  expect(shouldOffer(q, r062, t0), "a newer version is offered again");
  remindLater(q, t0);
  expect(!shouldOffer(q, r062, t0 + 3600), "remind later: not within the day");
  expect(shouldOffer(q, r062, t0 + day), "remind later: offered after a day");

  Settings s;
  Prefs d = loadPrefs(s);
  expect(d.autoCheck && !d.beta && d.lastCheck == 0 && d.skipVersion.empty(), "defaults: automatic on, beta off, never checked");
  q.beta = true;
  q.lastCheck = t0;
  savePrefs(s, q);
  d = loadPrefs(s);
  expect(d.beta && d.lastCheck == t0 && d.skipVersion == "0.6.1" && d.remindAfter == q.remindAfter, "prefs saved in settings");

  platform::setEnv("GRIDLINE_NO_UPDATE_CHECK", "1");
  expect(disabledByEnvironment(), "GRIDLINE_NO_UPDATE_CHECK disables");
}

void installer() {
  const fs::path dir = fs::temp_directory_path() / ("gridline-update-test-" + std::to_string(platform::processId()));
  std::error_code ec;
  fs::remove_all(dir, ec);
  const Release r = fakeRelease("0.6.1");
  const Asset* a = pickAsset(r, Target{"linux", "x86_64"});
  expect(a != nullptr, "fake release has a Linux asset");
  if (!a) return;
  int64_t last = 0, total = 0;
  InstallerResult res = downloadInstaller(fakeFetcher(r), fakeDownloader(), r, *a, dir.string(), [&](int64_t got, int64_t all) {
    last = got;
    total = all;
  }, nullptr);
  expect(res.ok && fs::exists(res.path) && fs::path(res.path).filename() == a->name, "installer downloaded and verified");
  expect(last == total && total == a->size, "progress reaches the size");
  expect(sha256File(res.path) == checksumFor(fakeSums(r), a->name), "digest matches SHA256SUMS");
  expect(!fs::exists(res.path + ".part"), "no partial file left");
#if !defined(_WIN32)
  expect((fs::status(res.path).permissions() & fs::perms::owner_exec) != fs::perms::none, "AppImage is executable");
#endif
  // Same file again: nothing fetched, the intact copy is reused.
  int calls = 0;
  Downloader counting = [&calls](const std::string&, const std::string&, const std::function<void(int64_t, int64_t)>&, const std::atomic<bool>*,
                                 std::string*) {
    ++calls;
    return false;
  };
  InstallerResult again = downloadInstaller(fakeFetcher(r), counting, r, *a, dir.string(), nullptr, nullptr);
  expect(again.ok && again.path == res.path && calls == 0, "an intact earlier download is reused");

  // A tampered download is refused and removed.
  const Asset* mac = pickAsset(r, Target{"macos", "aarch64"});
  Downloader tampered = [](const std::string&, const std::string& path, const std::function<void(int64_t, int64_t)>&, const std::atomic<bool>*,
                           std::string*) {
    std::ofstream(path, std::ios::binary) << "not the installer";
    return true;
  };
  InstallerResult bad = downloadInstaller(fakeFetcher(r), tampered, r, *mac, dir.string(), nullptr, nullptr);
  expect(!bad.ok && bad.error.find("SHA-256") != std::string::npos, "checksum mismatch refused");
  expect(!fs::exists(dir / mac->name) && !fs::exists(dir / (mac->name + ".part")), "mismatching file removed");

  // No SHA256SUMS in the release: not downloaded at all.
  Release nosums = r;
  nosums.assets.pop_back();
  InstallerResult ns = downloadInstaller(fakeFetcher(nosums), fakeDownloader(), nosums, *pickAsset(nosums, Target{"windows", "x86_64"}),
                                         dir.string(), nullptr, nullptr);
  expect(!ns.ok && ns.error.find("SHA256SUMS") != std::string::npos, "release without SHA256SUMS: no download");

  // Cancelled.
  std::atomic<bool> cancel{true};
  InstallerResult cn =
      downloadInstaller(fakeFetcher(r), fakeDownloader(), r, *pickAsset(r, Target{"netbsd", "x86_64"}), dir.string(), nullptr, &cancel);
  expect(!cn.ok && cn.error == "cancelled", "cancelled download");
  // An existing different file of that name is kept; the new one gets " (1)".
  const Asset* bsd = pickAsset(r, Target{"freebsd", "x86_64"});
  std::ofstream(dir / bsd->name) << "older file";
  InstallerResult side = downloadInstaller(fakeFetcher(r), fakeDownloader(), r, *bsd, dir.string(), nullptr, nullptr);
  expect(side.ok && fs::path(side.path).filename() == "Gridline-0.6.1-freebsd-x86_64 (1).tar.gz", "existing file kept, numbered name");
  fs::remove_all(dir, ec);
}

}  // namespace

int main() {
  versions();
  parsing();
  checksums();
  checking();
  builtVersion();
  prereleases();
  schedule();
  installer();
  return finish();
}
