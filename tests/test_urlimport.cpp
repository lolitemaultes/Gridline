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
#if defined(__APPLE__)
#include <sys/xattr.h>
#endif

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

// Error output captured from real yt-dlp runs (2026.10.26, November 2026) against the sites named:
// each maps to the right explanation, and only the passing ones are retried.
void testCapturedErrors() {
  struct Sample {
    const char* name;
    const char* stderrText;
    const char* leadHas;
    bool retry;
  };
  const Sample samples[] = {
      {"youtube 403", "WARNING: Unknown codec iamf.001.001.Opus\nERROR: unable to download video data: HTTP Error 403: Forbidden\n", "HTTP 403",
       true},
      {"vimeo login",
       "ERROR: [vimeo] 56015672: The web client only works when logged-in. Use --cookies, --cookies-from-browser, --username and "
       "--password, --netrc-cmd, or --netrc (vimeo) to provide account credentials. See  https://github.com/yt-dlp/yt-dlp/wiki/FAQ#how-do-"
       "i-pass-cookies-to-yt-dlp  for how to manually pass cookies\n",
       "signed-in", false},
      {"bandcamp 404", "ERROR: [Bandcamp:album] youtube-dl-test-album: Unable to download webpage: HTTP Error 404: Not Found (caused by "
                       "<HTTPError 404: Not Found>)\n",
       "not found on the site", false},
      {"archive.org empty item", "ERROR: [archive.org] mikho: opening play-av tag not found\n", "signed-in users", false},
      {"offline", "ERROR: [generic] Unable to download webpage: <urlopen error [Errno 8] nodename nor servname provided, or not known>\n",
       "could not be reached", false},
      {"rate limit", "ERROR: [youtube] abc: HTTP Error 429: Too Many Requests\n", "limiting requests", false},
      {"server error", "ERROR: unable to download video data: HTTP Error 503: Service Unavailable\n", "error of its own", true},
      {"twitter 500",
       "ERROR: [twitter] 665052190608723968: Unable to download webpage: HTTP Error 500: Domain Not Found (caused by <HTTPError 500: "
       "Domain Not Found>)\n",
       "error of its own", true},
      {"broken connection", "ERROR: [download] Got error: Remote end closed connection without response\n", "", true},
      {"tiktok blocked", "ERROR: [TikTok] 6748451240264420610: Your IP address is blocked from accessing this post\n", "blocks downloads",
       false},
      {"reddit login",
       "ERROR: [Reddit] 6rrwyj: Account authentication is required. Use --cookies-from-browser or --cookies for the authentication.\n",
       "signed-in", false},
      {"youtube removed", "ERROR: [youtube] zzzzzzzzzz0: This video is unavailable\n", "not available", false},
      {"drm", "ERROR: [DRM] The requested site is known to use DRM protection. It will NOT be supported.\n", "DRM", false},
  };
  for (const auto& s : samples) {
    const std::string e = ul::toolError(s.stderrText);
    const std::string lead = ul::errorLead(e);
    if (*s.leadHas) expect(lead.find(s.leadHas) != std::string::npos, (std::string("captured error explained: ") + s.name + " -> " + lead).c_str());
    expect(ul::retryableError(e) == s.retry, (std::string("captured error retried only when it may pass: ") + s.name).c_str());
  }
  // A title that is a file name (sound libraries) does not end up as "name.wav.wav".
  expect(ul::safeFileName("morning bell.wav") == "morning bell" && ul::safeFileName("Kick 01.FLAC") == "Kick 01" &&
             ul::safeFileName("v1.2 final") == "v1.2 final",
         "file name: an audio extension in the title is dropped");
  // Titles some sites give as HTML (a Wikimedia Commons file page) become plain text.
  expect(ul::plainText("<div class=\"fn\">\nAire, Second Music, The Fairy Queen, Z.629.\n</div>") ==
             "Aire, Second Music, The Fairy Queen, Z.629.",
         "title: HTML tags removed");
  expect(ul::plainText("Tom &amp; Jerry &lt;live&gt;") == "Tom & Jerry <live>", "title: entities decoded");
  expect(ul::plainText("I <3 synths, a < b") == "I <3 synths, a < b", "title: a lone < is kept");
  ul::Info in;
  expect(ul::parseInfo("{\"id\":\"x\",\"title\":\"<div class=\\\"fn\\\">\\nAire\\n</div>\"}", in) && in.title == "Aire",
         "info: an HTML title is read as plain text");
}

void testArgsAndFiles(const fs::path& dir) {
  const auto info = ul::infoArgs({"py", "yt.pyz"}, "https://a.com/x");
  expect(info.size() > 3 && info[0] == "py" && info[1] == "yt.pyz" && info[info.size() - 2] == "--" && info.back() == "https://a.com/x",
         "info: prefix, the link last after --");
  expect(std::find(info.begin(), info.end(), "--ignore-config") != info.end(), "info: the user's yt-dlp config is ignored");
  const auto dl = ul::downloadArgs({"yt"}, "/x/ffmpeg", "https://a.com/x", "/w");
  expect(std::find(dl.begin(), dl.end(), "bestaudio[audio_channels<=?2]/bestaudio/best") != dl.end(),
         "download: best audio, a stereo or mono stream before a surround one");
  expect(std::find(dl.begin(), dl.end(), "--write-info-json") != dl.end() &&
             std::find_if(dl.begin(), dl.end(), [](const std::string& x) { return x.rfind("infojson:", 0) == 0; }) != dl.end(),
         "download: the item's own info is written (titles of playlist items)");
  expect(std::find(dl.begin(), dl.end(), "--no-playlist") != dl.end(), "download: one item");
  expect(std::find(dl.begin(), dl.end(), "/x/ffmpeg") != dl.end(), "download: our ffmpeg");
  expect(dl.back() == "https://a.com/x" && dl[dl.size() - 2] == "--", "download: the link last after --");
  const auto cv = ul::convertArgs("ff", "in.webm", "out.wav", ul::Trim{10, 20}, ul::Tags{"T", "A", "https://u"}, "");
  auto at = [&](const std::string& k) { return std::find(cv.begin(), cv.end(), k); };
  expect(at("pcm_f32le") != cv.end(), "convert: 32-bit float PCM");
  expect(at("-ar") == cv.end() && at("-ac") == cv.end(), "convert: source rate kept");
  expect(at("-af") != cv.end() && *(at("-af") + 1) == "aformat=channel_layouts=mono|stereo",
         "convert: mono and stereo kept, surround mixed down to stereo");
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
  // A refused stream link (HTTP 403) is tried again with a fresh read of the page, and works.
  scenario("flaky403");
  job = waitJob(ul::startImport(tools(), {rq}, (dir / "i3b").u8string()));
  expect(job->get() == ul::Phase::Done && job->results.size() == 1 && job->attempt == 2, "import: a 403 is retried and the retry works");
  scenario("always403");
  ul::Limits fast;
  fast.retryPauseSeconds = 0.05;
  job = waitJob(ul::startImport(tools(), {rq}, (dir / "i3c").u8string(), fast));
  expect(job->get() == ul::Phase::Failed && job->attempt == fast.downloadTries && job->lead.find("403") != std::string::npos &&
             job->log.find("HTTP Error 403") != std::string::npos && job->log.find("--ignore-config") != std::string::npos,
         "import: a 403 every time stops after the tries, explained, with the command and its output in the details");
  // A playlist item known only by its number gets its title from the download's own info.
  scenario("untitled");
  auto fj = waitJob(ul::startFetch(tools(), "https://example.com/sets/s1", (dir / "f7").u8string()));
  expect(fj->get() == ul::Phase::Done && fj->info.entries.size() == 1 && fj->info.entries[0].title == "291144360",
         "fetch: a set whose items have only numbers");
  if (!fj->info.entries.empty()) {
    ul::Request rn;
    rn.item = fj->info.entries[0];
    job = waitJob(ul::startImport(tools(), {rn}, (dir / "i3d").u8string()));
    expect(job->get() == ul::Phase::Done && !job->results.empty() && job->results[0].request.item.title == "Named Track" &&
               job->results[0].request.item.uploader == "Set Owner" && std::fabs(job->results[0].request.item.duration - 2.0) < 1e-9,
           "import: the item is named by its own title, not its number");
  }
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
  // GitHub limiting requests: not checked, and the error says so (what the network test skips on).
  for (int status : {403, 429}) {
    mt::Net limited = n;
    limited.fetch = [status](const std::string&) {
      update::HttpResult h;
      h.status = status;
      return h;
    };
    u = mt::updateYtdlp(limited, target.u8string(), "2020.01.01", nullptr);
    expect(!u.checked && !u.updated && u.error.find("limiting") != std::string::npos && mt::setupLead(u.error).find("hour") != std::string::npos,
           ("update: GitHub limiting requests is explained (" + std::to_string(status) + "): " + u.error).c_str());
  }
  expect(mt::updateDue(true, 0, 1000) && !mt::updateDue(false, 0, 1000) && !mt::updateDue(true, 1000, 1000 + 3600) &&
             mt::updateDue(true, 1000, 1000 + 24 * 3600) && mt::updateDue(true, 5000, 1000),
         "update: once a day, off switch, clock set back");
}

// Import from URL that did not work: a video link opened from a playlist, a program the system
// stops (macOS quarantine), unclear failures, the JavaScript runtime for YouTube, set-up hints.
void testFixes(const fs::path& dir) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  // A video link from inside a playlist means that video (the playlist's own link stays a playlist).
  const auto info = ul::infoArgs({"yt"}, "https://www.youtube.com/watch?v=a1&list=PL1");
  const auto dash = std::find(info.begin(), info.end(), "--");
  expect(std::find(info.begin(), dash, "--no-playlist") != dash, "info: a video link inside a playlist reads that video");

  // Every failure says what to try.
  for (const char* e : {"This video is DRM protected", "Sign in to confirm your age", "Private video", "Unsupported URL: x",
                        "Unable to download webpage: <urlopen error [Errno 8] nodename nor servname provided>", "Video unavailable",
                        "Requested format is not available", "Sign in to confirm you're not a bot",
                        "Unable to extract initial player response; please report this issue", "HTTP Error 403: Forbidden",
                        "This live event will begin in 5 minutes"}) {
    const std::string l = ul::errorLead(e);
    expect(!l.empty(), (std::string("lead: known failure explained: ") + e).c_str());
  }
  expect(ul::errorLead("Sign in to confirm you're not a bot").find("bot") != std::string::npos, "lead: the bot check is named");
  expect(ul::errorLead("Unable to extract initial player response; please report this issue").find("Update yt-dlp") != std::string::npos,
         "lead: a changed site says to update yt-dlp");
  expect(ul::errorLead("Requested format is not available").find("update yt-dlp") != std::string::npos,
         "lead: no audio stream suggests an update");
  expect(!ul::fallbackLead().empty() && ul::fallbackLead().find("Settings") != std::string::npos, "lead: a fallback with what to try");

  ul::RunResult r;
  ul::RunFailure f = ul::explainRun(r, "The downloader (yt-dlp)");
  expect(f.error.find("could not be started") != std::string::npos && f.lead.find("Reinstall") != std::string::npos,
         "explain: a program that does not start says to reinstall");
  r.started = true;
  r.crashed = true;
  f = ul::explainRun(r, "The downloader (yt-dlp)");
  expect(f.error.find("stopped by the system") != std::string::npos && f.lead.find("Reinstall") != std::string::npos,
         "explain: a program ended by the system says so and what to do");
  r.crashed = false;
  r.code = 1;
  r.err = "ERROR: something new happened\n";
  f = ul::explainRun(r, "The downloader (yt-dlp)");
  expect(f.error == "something new happened" && f.lead == ul::fallbackLead(), "explain: an unknown error still says what to try");
  r.timedOut = true;
  f = ul::explainRun(r, "The downloader (yt-dlp)");
  expect(f.error.find("stopped making progress") != std::string::npos && f.lead.find("internet") != std::string::npos,
         "explain: a stall names the connection");

  // A program ended by a signal before it wrote anything (a blocked program on macOS).
#if !defined(_WIN32)
  scenario("killed");
  auto job = waitJob(ul::startFetch(tools(), "https://example.com/watch?v=abc123", (dir / "killed").u8string()));
  {
    std::lock_guard<std::mutex> lk(job->mu);
    expect(job->get() == ul::Phase::Failed && job->error.find("stopped by the system") != std::string::npos && !job->lead.empty(),
           "fetch: a program the system stops is explained");
  }
#endif
  scenario("bot");
  auto bot = waitJob(ul::startFetch(tools(), "https://example.com/watch?v=abc123", (dir / "bot").u8string()));
  {
    std::lock_guard<std::mutex> lk(bot->mu);
    expect(bot->get() == ul::Phase::Failed && bot->lead.find("bot") != std::string::npos, "fetch: the bot check is explained");
  }
  scenario("outdated");
  auto old = waitJob(ul::startFetch(tools(), "https://example.com/watch?v=abc123", (dir / "outdated").u8string()));
  {
    std::lock_guard<std::mutex> lk(old->mu);
    expect(old->get() == ul::Phase::Failed && old->lead.find("Update yt-dlp") != std::string::npos, "fetch: an outdated downloader says to update");
  }
  scenario("ok");

  // Set-up failures say what to try.
  expect(mt::setupLead("could not reach GitHub (the newest yt-dlp release)").find("internet") != std::string::npos, "setup: offline");
  expect(mt::setupLead("GitHub is limiting requests right now (x)").find("hour") != std::string::npos, "setup: rate limit");
  expect(mt::setupLead("yt-dlp_macos does not match its published SHA-256 checksum").find("checksum") != std::string::npos,
         "setup: checksum");
  expect(mt::setupLead("something else").find("Reinstall") != std::string::npos, "setup: anything else names Reinstall");

  // The JavaScript runtime for YouTube is found where the app's PATH does not reach.
  {
    const fs::path bin = dir / "js bin";
    fs::create_directories(bin, ec);
    fs::copy_file(fakeTool, bin / mt::exeName("deno"), fs::copy_options::overwrite_existing, ec);
    const char* oldPath = std::getenv("PATH");
    const std::string keep = oldPath ? oldPath : "";
    platform::setEnv("PATH", bin.u8string().c_str());
    const auto js = mt::jsRuntimeArgs();
    platform::setEnv("PATH", keep.c_str());
    expect(js.size() == 2 && js[0] == "--js-runtimes" && js[1] == "deno:" + (bin / mt::exeName("deno")).u8string(),
           "js runtime: deno handed to yt-dlp with its path");
  }
  // ...also where the runtimes' own installers put them in the home folder (not on an app's PATH):
  // deno's installer (~/.deno/bin), and node from nvm (the newest version).
  {
    const fs::path home = dir / "js home", empty = dir / "js empty";
    fs::create_directories(home / ".deno" / "bin", ec);
    fs::create_directories(empty, ec);
    fs::copy_file(fakeTool, home / ".deno" / "bin" / mt::exeName("deno"), fs::copy_options::overwrite_existing, ec);
    for (const char* v : {"v9.11.2", "v22.3.0", "v20.18.1"}) {
      fs::create_directories(home / ".nvm" / "versions" / "node" / v / "bin", ec);
      fs::copy_file(fakeTool, home / ".nvm" / "versions" / "node" / v / "bin" / mt::exeName("node"), fs::copy_options::overwrite_existing, ec);
    }
    const char* oldPath = std::getenv("PATH");
    const char* oldHome = std::getenv("HOME");
    const std::string keepPath = oldPath ? oldPath : "", keepHome = oldHome ? oldHome : "";
    platform::setEnv("PATH", empty.u8string().c_str());
    platform::setEnv("HOME", home.u8string().c_str());
    const auto js = mt::jsRuntimeArgs();
    fs::remove(home / ".deno" / "bin" / mt::exeName("deno"), ec);
    const auto js2 = mt::jsRuntimeArgs();
    const std::string systemDeno = mt::findOnSystem("deno");  // (this computer's own deno, in /usr/local/bin and the like)
    platform::setEnv("PATH", keepPath.c_str());
    platform::setEnv("HOME", keepHome.c_str());
    expect(js.size() == 2 && js[1] == "deno:" + (home / ".deno" / "bin" / mt::exeName("deno")).u8string(),
           "js runtime: deno from its installer's folder in the home folder");
    const std::string nvmNode = "node:" + (home / ".nvm" / "versions" / "node" / "v22.3.0" / "bin" / mt::exeName("node")).u8string();
    expect(js2.size() == 2 && (systemDeno.empty() ? js2[1] == nvmNode : js2[1] == "deno:" + systemDeno),
           "js runtime: node from nvm, the newest version (deno first where the system has one)");
  }

  // Two version checks at once (set-up and update) each read their own output.
  {
    std::string v1, v2;
    std::thread t1([&] { v1 = mt::runVersion({fakeTool}); });
    std::thread t2([&] { v2 = mt::runVersion({fakeTool}); });
    t1.join();
    t2.join();
    expect(v1 == "2099.01.01" && v2 == "2099.01.01", "version checks at the same time both read the version");
  }

  // macOS: the copies inside a downloaded app carry the quarantine flag; the system stops them.
#if defined(__APPLE__)
  {
    const fs::path app = dir / "Gridline.app tools";
    const fs::path inst = dir / "installed tools";
    fs::create_directories(app, ec);
    for (const char* n : {"yt-dlp", "ffmpeg", "ffprobe"}) {
      fs::copy_file(fakeTool, app / n, fs::copy_options::overwrite_existing, ec);
      fs::permissions(app / n, fs::perms::owner_all, fs::perm_options::replace, ec);
    }
    {
      std::ofstream(app / "yt-dlp.version") << "2026.10.26\n";
    }
    const char flag[] = "0083;6553a1b0;Browser;";
    for (const char* n : {"yt-dlp", "ffmpeg", "ffprobe"}) setxattr((app / n).u8string().c_str(), "com.apple.quarantine", flag, sizeof flag - 1, 0, 0);
    expect(mt::quarantined((app / "yt-dlp").u8string()), "quarantine: the flag is seen");
    // The folder can be changed: the flag is removed in place (ffprobe with ffmpeg).
    mt::Tool ff = mt::unblock(mt::Tool{mt::From::Bundled, (app / "ffmpeg").u8string(), {}}, inst.u8string());
    expect(ff.from == mt::From::Bundled && !mt::quarantined((app / "ffmpeg").u8string()) && !mt::quarantined((app / "ffprobe").u8string()),
           "quarantine: removed from ffmpeg and ffprobe inside the app");
    expect(!mt::runVersion(ff.argv()).empty(), "quarantine: the cleared ffmpeg runs");
    // The app's files cannot be changed (a read-only disk image): a copy without the flag is used.
    fs::permissions(app / "yt-dlp", fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace, ec);
    if (!mt::clearQuarantine((app / "yt-dlp").u8string())) {
      std::string err;
      mt::Tool yt = mt::unblock(mt::Tool{mt::From::Bundled, (app / "yt-dlp").u8string(), {}}, inst.u8string(), &err);
      expect(yt.from == mt::From::Installed && yt.path == (inst / "yt-dlp").u8string() && !mt::quarantined(yt.path) && err.empty(),
             "quarantine: a read-only copy is copied into Gridline's tools folder without the flag");
      expect(mt::recordedVersion(inst.u8string()) == "2026.10.26", "quarantine: the copy keeps its version");
      expect(mt::runVersion(yt.argv()) == "2099.01.01", "quarantine: the copy runs");
      // Next time the same copy is reused.
      const auto t0 = fs::last_write_time(inst / "yt-dlp", ec);
      yt = mt::unblock(mt::Tool{mt::From::Bundled, (app / "yt-dlp").u8string(), {}}, inst.u8string());
      expect(yt.from == mt::From::Installed && fs::last_write_time(inst / "yt-dlp", ec) == t0, "quarantine: the copy is reused");
    } else {
      expect(true, "quarantine: read-only case not reproducible here (flag removable)");
    }
    fs::permissions(app / "yt-dlp", fs::perms::owner_all, fs::perm_options::replace, ec);
    // A tool without the flag, or the system's, is left alone.
    mt::Tool sys{mt::From::System, "/usr/bin/true", {}};
    expect(mt::unblock(sys, inst.u8string()).path == "/usr/bin/true", "quarantine: the system's tools are left alone");
  }
#else
  expect(!mt::quarantined(fakeTool) && mt::clearQuarantine(fakeTool), "quarantine: nothing to do on this system");
#endif
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
  testCapturedErrors();
  testArgsAndFiles(dir / "files");
  testProcess(dir);
  testWorker(dir / "work");
  testTools(dir);
  testFixes(dir / "fixes");
  return finish();
}
