// Gridline - J-192 tests: Import from URL without any network. The tools are a fake program
// (tests/fake_media_tool.cpp, its path is argv[1]) that acts as yt-dlp and ffmpeg; the tool set-up
// downloads from a fake GitHub (mediatools::fakeNet).
//   links, clipboard text, hosts; the info JSON (single, playlist, live, broken); progress lines
//   (yt-dlp's template, ffmpeg's -progress); errors and their explanations; times; command lines
//   (the link is one argument after "--", lossless float WAV, trim, metadata); file names and
//   the source file; the worker: fetch, playlist, import with progress into a WAV the engine
//   decodes, trim, tool errors, sign-in, no audio, conversion failure, cancel (the tool is killed
//   at once), a stalled download, a missing tool; Process output redirect and tree kill;
//   tool search order; install (yt-dlp, ffmpeg from the media-tools archive), checksum mismatch,
//   update (current / newer), the daily schedule.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include "Check.h"
#include "Hermetic.h"
#include "core/MediaTools.h"
#include "core/Platform.h"
#include "core/Update.h"
#include "core/UrlImport.h"
#include "engine/Sample.h"

namespace fs = std::filesystem;
using namespace gl;
namespace ul = gl::urlimport;
namespace mt = gl::mediatools;

namespace {

std::string fakeTool;

std::shared_ptr<ul::Job> waitJob(std::shared_ptr<ul::Job> j, double seconds = 30) {
  const auto t0 = std::chrono::steady_clock::now();
  while (!j->done && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < seconds)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  return j;
}

void scenario(const char* s) { platform::setEnv("GL_FAKE_SCENARIO", s); }

ul::Tools tools() { return ul::Tools{{fakeTool}, fakeTool}; }

void testLinks() {
  expect(ul::validUrl("https://www.youtube.com/watch?v=abc"), "https link");
  expect(ul::validUrl("http://example.com"), "http link");
  std::string why;
  expect(!ul::validUrl("ftp://example.com/a", &why) && !why.empty(), "ftp refused with a reason");
  expect(!ul::validUrl("https://exa mple.com"), "spaces refused");
  expect(!ul::validUrl("https://"), "no host refused");
  expect(!ul::validUrl("file:///etc/passwd"), "file links refused");
  expect(!ul::validUrl("-o/tmp/x"), "an option-like text refused");
  expect(!ul::validUrl(""), "empty refused");
  expect(!ul::validUrl("https://a.com/" + std::string(5000, 'x')), "very long refused");
  expect(ul::linkInText("  <https://vimeo.com/12345>\n") == "https://vimeo.com/12345", "clipboard: brackets and spaces trimmed");
  expect(ul::linkInText("\"https://soundcloud.com/a/b\"") == "https://soundcloud.com/a/b", "clipboard: quotes trimmed");
  expect(ul::linkInText("hello world").empty(), "clipboard: plain text is no link");
  expect(ul::linkInText("see https://a.com and more").empty(), "clipboard: a sentence is no link");
  expect(ul::hostOf("https://www.YouTube.com:443/watch?v=1") == "youtube.com", "host without www and port");
  expect(ul::hostOf("https://user@m.facebook.com/x") == "m.facebook.com", "host without user");
}

void testInfo() {
  ul::Info in;
  std::string err;
  expect(ul::parseInfo("{\"id\":\"x\",\"title\":\"T\",\"channel\":\"C\",\"extractor_key\":\"Vimeo\",\"duration\":61.5,"
                       "\"webpage_url\":\"https://vimeo.com/1\"}",
                       in, &err),
         "single item parsed");
  expect(!in.playlist && in.title == "T" && in.uploader == "C" && in.site == "Vimeo" && std::fabs(in.duration - 61.5) < 1e-9 &&
             in.url == "https://vimeo.com/1",
         "single item fields (channel as uploader)");
  expect(ul::parseInfo("WARNING: x\n{\"id\":\"x\",\"title\":\"T\"}", in), "stray line before the JSON is skipped");
  expect(!ul::parseInfo("not json", in, &err) && !err.empty(), "broken JSON fails with a reason");
  expect(ul::parseInfo("{\"_type\":\"playlist\",\"title\":\"P\",\"entries\":[{\"url\":\"https://a.com/1\",\"title\":\"one\",\"duration\":3},"
                       "{\"url\":\"not a link\",\"webpage_url\":\"https://a.com/2\",\"id\":\"two\"},{\"title\":\"no link\"}]}",
                       in),
         "playlist parsed");
  expect(in.playlist && in.entries.size() == 2 && in.entries[0].title == "one" && in.entries[1].url == "https://a.com/2" &&
             in.entries[1].title == "two",
         "playlist entries: links, id as the title, unusable ones left out");
  expect(!ul::parseInfo("{\"_type\":\"playlist\",\"title\":\"P\",\"entries\":[]}", in, &err), "empty playlist fails");
  expect(ul::parseInfo("{\"title\":\"L\",\"live_status\":\"is_live\"}", in) && in.live, "live stream recognised");
}

void testProgressAndErrors() {
  ul::DownloadProgress p;
  expect(ul::parseDownloadLine("[download] GLPROG 512 1024 NA NA", p) && p.got == 512 && p.total == 1024 && std::fabs(p.fraction - 0.5) < 1e-9,
         "download progress: bytes");
  expect(ul::parseDownloadLine("GLPROG 100 NA 3 12", p) && p.total == -1 && std::fabs(p.fraction - 0.25) < 1e-9, "download progress: fragments");
  expect(ul::parseDownloadLine("GLPROG NA NA NA NA", p) && p.fraction < 0, "download progress: unknown");
  expect(!ul::parseDownloadLine("[download] 50.0% of 3MiB", p), "other lines are not progress");
  double s = 0;
  expect(ul::parseFfmpegTime("out_time_us=1500000", s) && std::fabs(s - 1.5) < 1e-9, "ffmpeg out_time_us");
  expect(ul::parseFfmpegTime("out_time=00:01:02.500000", s) && std::fabs(s - 62.5) < 1e-6, "ffmpeg out_time");
  expect(!ul::parseFfmpegTime("out_time_us=N/A", s), "ffmpeg N/A ignored");
  expect(ul::toolError("WARNING: a\nERROR: [youtube] abc123: Video unavailable\n") == "Video unavailable", "tool error without prefixes");
  expect(ul::toolError("ERROR: Unsupported URL: https://x.com") == "Unsupported URL: https://x.com", "tool error kept");
  expect(ul::toolError("some\nlast line\n") == "last line", "no ERROR line: the last line");
  expect(ul::errorLead("This video is DRM protected").find("DRM") != std::string::npos, "lead: DRM");
  expect(ul::errorLead("Sign in to confirm your age").find("signed-in") != std::string::npos, "lead: sign in");
  expect(ul::errorLead("Private video").find("private") != std::string::npos, "lead: private");
  expect(ul::errorLead("Unsupported URL: x").find("not a media page") != std::string::npos, "lead: unsupported");
  expect(ul::errorLead("Unable to download webpage: <urlopen error [Errno 8] nodename nor servname provided>").find("reached") !=
             std::string::npos,
         "lead: offline");
  expect(ul::errorLead("something odd").empty(), "lead: none for unknown errors");
  expect(std::fabs(ul::parseTime("83") - 83) < 1e-9 && std::fabs(ul::parseTime("1:23") - 83) < 1e-9 &&
             std::fabs(ul::parseTime("1:02:03.5") - 3723.5) < 1e-9,
         "times parsed");
  expect(ul::parseTime("1:75") < 0 && ul::parseTime("abc") < 0 && ul::parseTime("") < 0 && ul::parseTime("1:2:3:4") < 0, "bad times refused");
  expect(ul::formatTime(83) == "1:23" && ul::formatTime(3723) == "1:02:03", "times formatted");
}

void testArgsAndFiles(const fs::path& dir) {
  const auto info = ul::infoArgs({"py", "yt.pyz"}, "https://a.com/x");
  expect(info.size() > 3 && info[0] == "py" && info[1] == "yt.pyz" && info[info.size() - 2] == "--" && info.back() == "https://a.com/x",
         "info: prefix, the link last after --");
  expect(std::find(info.begin(), info.end(), "--ignore-config") != info.end(), "info: the user's yt-dlp config is ignored");
  const auto dl = ul::downloadArgs({"yt"}, "/x/ffmpeg", "https://a.com/x", "/w");
  expect(std::find(dl.begin(), dl.end(), "bestaudio/best") != dl.end(), "download: best audio");
  expect(std::find(dl.begin(), dl.end(), "--no-playlist") != dl.end(), "download: one item");
  expect(std::find(dl.begin(), dl.end(), "/x/ffmpeg") != dl.end(), "download: our ffmpeg");
  expect(dl.back() == "https://a.com/x" && dl[dl.size() - 2] == "--", "download: the link last after --");
  const auto cv = ul::convertArgs("ff", "in.webm", "out.wav", ul::Trim{10, 20}, ul::Tags{"T", "A", "https://u"}, "");
  auto at = [&](const std::string& k) { return std::find(cv.begin(), cv.end(), k); };
  expect(at("pcm_f32le") != cv.end(), "convert: 32-bit float PCM");
  expect(at("-ar") == cv.end() && at("-ac") == cv.end(), "convert: source rate and channels kept");
  expect(at("-ss") != cv.end() && *(at("-ss") + 1) == "10.000" && at("-t") != cv.end() && *(at("-t") + 1) == "10.000", "convert: trim");
  expect(at("title=T") != cv.end() && at("artist=A") != cv.end() && at("comment=https://u") != cv.end(), "convert: metadata");
  expect(cv.back() == "out.wav", "convert: output last");
  expect(ul::safeFileName("a/b\\c:d*e?\"f<g>h|i") == "a b c d e f g h i", "file name: reserved characters");
  expect(ul::safeFileName("  ..hidden.  ") == "hidden", "file name: dots and spaces trimmed");
  expect(ul::safeFileName("CON") == "CON_" && ul::safeFileName("aux.txt") == "aux.txt_", "file name: Windows reserved names");
  expect(ul::safeFileName("").empty() == false && ul::safeFileName("///") == "Imported audio", "file name: nothing left");
  std::string longName;
  for (int i = 0; i < 100; ++i) longName += "\xC3\xA9";  // 200 bytes of e-acute
  const std::string cut = ul::safeFileName(longName);
  expect(cut.size() <= 120 && cut.size() % 2 == 0, "file name: cut on a UTF-8 boundary");
  fs::create_directories(dir);
  const std::string p1 = ul::uniquePath(dir.u8string(), "Song", ".wav");
  std::ofstream(fs::u8path(p1)) << "x";
  const std::string p2 = ul::uniquePath(dir.u8string(), "Song", ".wav");
  expect(fs::u8path(p1).filename() == "Song.wav" && fs::u8path(p2).filename() == "Song (2).wav", "unique names");
  ul::Source s{"Title", "Up", "Vimeo", "https://vimeo.com/1", "", 12.5};
  expect(ul::writeSource(p1, s), "source written");
  ul::Source r;
  expect(ul::readSource(p1, r) && r.title == "Title" && r.uploader == "Up" && r.site == "Vimeo" && r.url == s.url &&
             std::fabs(r.duration - 12.5) < 1e-9 && !r.imported.empty(),
         "source read back (with the import time)");
  expect(!ul::readSource((dir / "none.wav").u8string(), r), "no source file");
}

void testProcess(const fs::path& dir) {
  // Output redirect: stdout and stderr land in the files, lines are seen while the program runs.
  scenario("ok");
  std::vector<std::string> lines;
  ul::RunResult r = ul::run({fakeTool, "--ignore-config", "-J", "--", "https://a.com"}, (dir / "r1").u8string(), nullptr, 20, 0, "",
                            [&](const std::string& l) { lines.push_back(l); });
  expect(r.started && r.ok && r.code == 0 && r.out.find("Fake Song") != std::string::npos, "run: output captured");
  expect(!lines.empty() && !fs::exists(dir / "r1.out") && !fs::exists(dir / "r1.err"), "run: lines seen, files removed");
  scenario("error");
  r = ul::run({fakeTool, "--ignore-config", "-J", "--", "https://a.com"}, (dir / "r2").u8string(), nullptr, 20, 0, "", nullptr);
  expect(r.started && !r.ok && r.code == 1 && r.err.find("Unsupported URL") != std::string::npos, "run: error output and code");
  r = ul::run({(dir / "missing-tool").u8string(), "--version"}, (dir / "r3").u8string(), nullptr, 5, 0, "", nullptr);
  expect(!r.started && !r.ok, "run: a missing program does not start");
  // Cancel kills at once.
  scenario("slow");
  std::atomic<bool> cancel{false};
  std::thread t([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    cancel = true;
  });
  const auto t0 = std::chrono::steady_clock::now();
  r = ul::run({fakeTool, "--ignore-config", "-J", "--", "https://a.com"}, (dir / "r4").u8string(), &cancel, 60, 0, "", nullptr);
  t.join();
  const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  expect(r.cancelled && !r.ok && took < 5, "run: cancel stops the program at once");
  r = ul::run({fakeTool, "--ignore-config", "-J", "--", "https://a.com"}, (dir / "r5").u8string(), nullptr, 0.5, 0, "", nullptr);
  expect(r.timedOut && !r.ok, "run: overall time limit");
}

void testWorker(const fs::path& dir) {
  scenario("ok");
  auto j = waitJob(ul::startFetch(tools(), "https://example.com/watch?v=abc123", (dir / "f1").u8string()));
  expect(j->done && j->get() == ul::Phase::Done, "fetch: done");
  expect(j->info.title == "Fake Song: Take/One" && j->info.uploader == "Test Uploader" && j->info.site == "Youtube" &&
             std::fabs(j->info.duration - 2.0) < 1e-9 && j->info.url == "https://example.com/watch?v=abc123",
         "fetch: title, uploader, site, length, link");
  scenario("playlist");
  j = waitJob(ul::startFetch(tools(), "https://example.com/playlist?list=PL1", (dir / "f2").u8string()));
  expect(j->get() == ul::Phase::Done && j->info.playlist && j->info.entries.size() == 3 && j->info.entries[1].uploader == "Other Channel",
         "fetch: playlist with its items");
  scenario("error");
  j = waitJob(ul::startFetch(tools(), "https://example.com/nothing", (dir / "f3").u8string()));
  expect(j->get() == ul::Phase::Failed && j->error.find("Unsupported URL") != std::string::npos && !j->lead.empty(),
         "fetch: the tool's error and an explanation");
  scenario("drm");
  j = waitJob(ul::startFetch(tools(), "https://example.com/drm", (dir / "f4").u8string()));
  expect(j->get() == ul::Phase::Failed && j->lead.find("DRM") != std::string::npos, "fetch: DRM explained");
  scenario("live");
  j = waitJob(ul::startFetch(tools(), "https://example.com/live", (dir / "f5").u8string()));
  expect(j->get() == ul::Phase::Failed && j->lead.find("Live") != std::string::npos, "fetch: live streams refused");
  j = waitJob(ul::startFetch(ul::Tools{{(dir / "no-such-tool").u8string()}, fakeTool}, "https://a.com/x", (dir / "f6").u8string()));
  expect(j->get() == ul::Phase::Failed && !j->error.empty(), "fetch: a missing tool fails clearly");

  // Import: progress, a WAV the engine reads, metadata kept for the source file.
  scenario("ok");
  ul::Request rq;
  rq.item = ul::Item{"abc123", "Fake Song", "Test Uploader", "https://example.com/watch?v=abc123", 2.0};
  rq.site = "Youtube";
  auto job = ul::startImport(tools(), {rq}, (dir / "i1").u8string());
  double maxFrac = -1;
  bool sawDownloading = false, sawConverting = false;
  const auto t0 = std::chrono::steady_clock::now();
  while (!job->done && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 30) {
    maxFrac = std::max(maxFrac, job->fraction.load());
    sawDownloading = sawDownloading || job->get() == ul::Phase::Downloading;
    sawConverting = sawConverting || job->get() == ul::Phase::Converting;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  expect(job->get() == ul::Phase::Done && job->results.size() == 1, "import: done, one result");
  expect(sawDownloading && maxFrac > 0.0, "import: download progress seen");
  (void)sawConverting;
  const std::string wav = job->results.empty() ? "" : job->results[0].wav;
  SamplePtr smp = wav.empty() ? nullptr : loadSample(wav);
  expect(smp && std::fabs(smp->seconds() - 2.0) < 0.01 && std::fabs(smp->sampleRate - 44100) < 1, "import: a 2 s float WAV the engine decodes");
  expect(!wav.empty() && !fs::exists(fs::u8path(wav).parent_path() / "media.webm"), "import: the downloaded stream is removed");
  // Trim: the converter gets -ss / -t.
  rq.trim = ul::Trim{0.5, 1.5};
  job = waitJob(ul::startImport(tools(), {rq}, (dir / "i2").u8string()));
  smp = job->results.empty() ? nullptr : loadSample(job->results[0].wav);
  expect(job->get() == ul::Phase::Done && smp && std::fabs(smp->seconds() - 1.0) < 0.01, "import: trimmed to the range");
  rq.trim = {};
  // Playlist items in turn.
  ul::Request rq2 = rq;
  rq2.item.title = "Second";
  job = waitJob(ul::startImport(tools(), {rq, rq2}, (dir / "i3").u8string()));
  expect(job->get() == ul::Phase::Done && job->results.size() == 2 && job->items == 2 && job->results[1].request.item.title == "Second",
         "import: several items in turn");
  scenario("signin");
  job = waitJob(ul::startImport(tools(), {rq}, (dir / "i4").u8string()));
  expect(job->get() == ul::Phase::Failed && job->lead.find("signed-in") != std::string::npos, "import: sign-in needed is explained");
  scenario("noaudio");
  job = waitJob(ul::startImport(tools(), {rq}, (dir / "i5").u8string()));
  expect(job->get() == ul::Phase::Failed && job->error.find("without an audio file") != std::string::npos, "import: no file downloaded");
  scenario("convertfail");
  job = waitJob(ul::startImport(tools(), {rq}, (dir / "i6").u8string()));
  expect(job->get() == ul::Phase::Failed && job->error.find("matches no streams") != std::string::npos, "import: conversion error");
  scenario("slow");
  job = ul::startImport(tools(), {rq}, (dir / "i7").u8string());
  std::this_thread::sleep_for(std::chrono::milliseconds(600));
  const auto c0 = std::chrono::steady_clock::now();
  job->cancel = true;
  waitJob(job, 10);
  expect(job->get() == ul::Phase::Cancelled && std::chrono::duration<double>(std::chrono::steady_clock::now() - c0).count() < 5,
         "import: cancel stops the download at once");
  scenario("stall");
  ul::Limits lim;
  lim.stallSeconds = 1.5;
  job = waitJob(ul::startImport(tools(), {rq}, (dir / "i8").u8string(), lim), 20);
  expect(job->get() == ul::Phase::Failed && job->error.find("stopped making progress") != std::string::npos, "import: a stalled download is given up");
  scenario("ok");
}

void testTools(const fs::path& dir) {
  // Search order.
  const fs::path inst = dir / "installed", bund = dir / "bundled";
  fs::create_directories(inst);
  fs::create_directories(bund);
  auto place = [&](const fs::path& d, const std::string& base) {
    const fs::path p = d / mt::exeName(base);
    fs::copy_file(fakeTool, p, fs::copy_options::overwrite_existing);
    fs::permissions(p, fs::perms::owner_all, fs::perm_options::add);
    return p;
  };
  mt::Tool t = mt::locateYtdlp(mt::Search{inst.u8string(), bund.u8string(), false});
  expect(!t.found() && t.from == mt::From::None, "search: nothing found");
  place(bund, "yt-dlp");
  place(bund, "ffmpeg");
  t = mt::locateYtdlp(mt::Search{inst.u8string(), bund.u8string(), false});
  expect(t.from == mt::From::Bundled, "search: yt-dlp inside the app");
  place(inst, "yt-dlp");
  place(inst, "ffmpeg");
  t = mt::locateYtdlp(mt::Search{inst.u8string(), bund.u8string(), false});
  expect(t.from == mt::From::Installed, "search: an updated yt-dlp comes before the app's");
  t = mt::locateFfmpeg(mt::Search{inst.u8string(), bund.u8string(), false});
  expect(t.from == mt::From::Bundled, "search: the app's ffmpeg comes first");
  platform::setEnv("GRIDLINE_YTDLP", (dir / "nope").u8string().c_str());
  t = mt::locateYtdlp(mt::Search{inst.u8string(), bund.u8string(), false});
  expect(!t.found(), "search: a wrong override does not fall back");
  platform::setEnv("GRIDLINE_YTDLP", fakeTool.c_str());
  t = mt::locateYtdlp(mt::Search{inst.u8string(), bund.u8string(), false});
  expect(t.from == mt::From::Override && t.path == fakeTool, "search: override");
  platform::unsetEnv("GRIDLINE_YTDLP");
  const fs::path zipDir = dir / "zipapp";
  fs::create_directories(zipDir);
  std::ofstream(zipDir / "yt-dlp.pyz") << "zip";
  const std::string py = mt::findOnSystem("python3");
  t = mt::locateYtdlp(mt::Search{zipDir.u8string(), "", true});
  expect(py.empty() || (t.from == mt::From::Installed && t.prefix.size() == 1 && t.prefix[0] == py && t.argv().size() == 2),
         "search: the zipapp runs with python3");

  // Assets per system.
  expect(mt::ytdlpAsset({"macos", "aarch64"}) == "yt-dlp_macos" && mt::ytdlpAsset({"windows", "x86_64"}) == "yt-dlp.exe" &&
             mt::ytdlpAsset({"windows", "aarch64"}) == "yt-dlp_arm64.exe" && mt::ytdlpAsset({"linux", "x86_64"}) == "yt-dlp_linux" &&
             mt::ytdlpAsset({"linux", "aarch64"}) == "yt-dlp_linux_aarch64" && mt::ytdlpAsset({"freebsd", "x86_64"}) == "yt-dlp",
         "yt-dlp assets per system");
  expect(mt::mediaToolsAsset("0.8.0", {"macos", "x86_64"}) == "Gridline-0.8.0-media-tools-macos-universal.zip" &&
             mt::mediaToolsAsset("0.8.0", {"windows", "aarch64"}) == "Gridline-0.8.0-media-tools-windows-arm64.zip" &&
             mt::mediaToolsAsset("0.8.0", {"linux", "aarch64"}) == "Gridline-0.8.0-media-tools-linux-aarch64.zip" &&
             mt::mediaToolsAsset("0.8.0", {"openbsd", "x86_64"}).empty(),
         "media-tools archives per system");

  // Install from the fake GitHub.
  const update::Target here = update::currentTarget();
  if (mt::ytdlpAsset(here).empty() || mt::ytdlpAsset(here) == "yt-dlp" || mt::mediaToolsAsset("1", here).empty()) {
    expect(true, "install: (no downloads for this system; skipped)");
    return;
  }
  const fs::path target = dir / "tools";
  const mt::Net n = mt::fakeNet(fakeTool);
  int64_t lastGot = 0;
  mt::InstallResult r = mt::installYtdlp(n, target.u8string(), [&](int64_t g, int64_t) { lastGot = g; }, nullptr);
  expect(r.ok && r.version == "2099.01.01" && fs::exists(target / mt::exeName("yt-dlp")) && lastGot > 0, "install: yt-dlp, with progress");
  expect(mt::recordedVersion(target.u8string()) == "2099.01.01", "install: version recorded");
  t = mt::locateYtdlp(mt::Search{target.u8string(), "", false});
  expect(t.from == mt::From::Installed && !mt::runVersion(t.argv(), 20).empty(), "install: the installed yt-dlp runs");
  r = mt::installFfmpeg(n, target.u8string(), nullptr, nullptr, here);
  expect(r.ok && fs::exists(target / mt::exeName("ffmpeg")) && fs::exists(target / mt::exeName("ffprobe")) &&
             fs::exists(target / "LICENSES-ffmpeg" / "README.txt"),
         "install: ffmpeg, ffprobe and licences from the archive");
  expect(!fs::exists(target / ".media-tools-new"), "install: nothing left over");
  // A checksum mismatch never replaces the working copy.
  mt::Net bad = n;
  bad.fetch = [n](const std::string& url) {
    update::HttpResult h = n.fetch(url);
    if (url.find("SUMS") != std::string::npos) h.body = std::string(64, '0') + h.body.substr(64);
    return h;
  };
  const std::string before = update::sha256File((target / mt::exeName("yt-dlp")).u8string());
  r = mt::installYtdlp(bad, target.u8string(), nullptr, nullptr);
  expect(!r.ok && r.error.find("checksum") != std::string::npos, "install: checksum mismatch refused");
  expect(update::sha256File((target / mt::exeName("yt-dlp")).u8string()) == before, "install: the working copy is kept");
  // Offline.
  mt::Net off = n;
  off.fetch = [](const std::string&) {
    update::HttpResult h;
    h.offline = true;
    return h;
  };
  r = mt::installYtdlp(off, (dir / "tools-off").u8string(), nullptr, nullptr);
  expect(!r.ok && r.error.find("could not reach") != std::string::npos, "install: offline explained");
  // Update: current -> nothing; older -> installed, previous kept.
  mt::UpdateResult u = mt::updateYtdlp(n, target.u8string(), "2099.01.01", nullptr);
  expect(u.checked && !u.updated, "update: already current");
  u = mt::updateYtdlp(n, target.u8string(), "2020.01.01", nullptr);
  expect(u.checked && u.updated && u.version == "2099.01.01" && fs::exists(target / (mt::exeName("yt-dlp") + ".previous")),
         "update: newer one installed, previous kept");
  expect(mt::updateDue(true, 0, 1000) && !mt::updateDue(false, 0, 1000) && !mt::updateDue(true, 1000, 1000 + 3600) &&
             mt::updateDue(true, 1000, 1000 + 24 * 3600) && mt::updateDue(true, 5000, 1000),
         "update: once a day, off switch, clock set back");
}

}  // namespace

int main(int argc, char** argv) {
  const std::string root = test::hermeticTestEnv("urlimport");
  if (argc < 2) {
    std::printf("usage: gridline_tests_urlimport <fake media tool>\n");
    return 2;
  }
  fakeTool = fs::absolute(argv[1]).u8string();
  platform::setEnv("XDG_DATA_HOME", (fs::u8path(root) / "data").u8string().c_str());
  platform::unsetEnv("GRIDLINE_YTDLP");
  platform::unsetEnv("GRIDLINE_FFMPEG");
  const fs::path dir = fs::u8path(root) / "j192";
  fs::create_directories(dir);
  testLinks();
  testInfo();
  testProgressAndErrors();
  testArgsAndFiles(dir / "files");
  testProcess(dir);
  testWorker(dir / "work");
  testTools(dir);
  return finish();
}
