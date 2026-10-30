// Gridline - J-192: File > Import > Import from URL. A media link (video and audio sites:
// anything yt-dlp supports) becomes a lossless audio file in the project and a clip.
//
// Gridline does not talk to the sites itself: it runs yt-dlp (public domain, Unlicense) to read
// the link and fetch the best audio stream as it is, then ffmpeg (LGPL build) to decode that
// stream into a 32-bit float WAV at its own sample rate and channel count, the decoder's output
// exactly: no second lossy step, overs above 0 dBFS kept. Both run as child processes with
// argument arrays (never a shell; the link is one argument after "--").
// Where the tools come from: MediaTools.h. The worker: UrlImportJob below. The popup:
// app/UrlImportUi.h. Behaviour and privacy: docs/import-url.md.
//
// This file: pure logic (unit tested in tests/test_urlimport.cpp without any network).
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace gl::urlimport {

// ---- links ----
// An http(s) link with a host, no spaces or control characters, at most 4096 bytes.
bool validUrl(const std::string& url, std::string* why = nullptr);
// The link in clipboard text (surrounding spaces / quotes / angle brackets trimmed), or "" when
// the text is not one link.
std::string linkInText(const std::string& text);
// "youtube.com" from "https://www.youtube.com/watch?v=..." (no "www.", lower case).
std::string hostOf(const std::string& url);

// ---- what the link holds (yt-dlp -J --flat-playlist) ----
struct Item {
  std::string id, title, uploader, url;
  double duration = 0;  // seconds, 0 = unknown
};
struct Info {
  bool playlist = false;
  std::string title, uploader, site, url;  // site: yt-dlp's extractor ("Youtube", "Vimeo", ...)
  double duration = 0;
  std::vector<Item> entries;  // playlist items (link, title, duration as far as known)
  bool live = false;          // a live stream: refused (it never ends)
};
bool parseInfo(const std::string& json, Info& out, std::string* err = nullptr);

// ---- progress ----
// The download line Gridline asks yt-dlp to print (downloadArgs' --progress-template):
// "GLPROG <downloaded bytes> <total or estimate> <fragment index> <fragment count>", "NA" when
// unknown. True for such a line; `fraction` in 0..1 or < 0 when unknown.
struct DownloadProgress {
  int64_t got = -1, total = -1;
  double fraction = -1;
};
bool parseDownloadLine(const std::string& line, DownloadProgress& out);
// ffmpeg -progress lines: "out_time_us=12345678" / "out_time_ms=..." / "out_time=00:00:12.34".
// True with the decoded position in seconds.
bool parseFfmpegTime(const std::string& line, double& seconds);
// "ERROR: ..." lines of the tool's error output, the last one, without the "ERROR: " and the
// "[extractor] id: " prefix; else the last non-empty line; "" when there is none.
std::string toolError(const std::string& errText);
// A one-line explanation for well-known failures (sign-in, DRM, private, unsupported, offline,
// region, removed), "" otherwise. English; the UI translates it.
std::string errorLead(const std::string& toolErrorText);

// ---- times ("83", "1:23", "1:02:03.5") ----
double parseTime(const std::string& text);  // seconds, < 0 when not a time
std::string formatTime(double seconds);     // "1:23" / "1:02:03"

// ---- command lines ----
struct Trim {
  double start = -1, end = -1;  // seconds; < 0 = from the start / to the end
  bool any() const { return start > 0 || end > 0; }
};
std::vector<std::string> infoArgs(const std::vector<std::string>& ytdlp, const std::string& url);
// Fetches the best audio (or, when the site has no audio-only stream, the best single file) as it
// is into `workDir`/media.<ext>.
std::vector<std::string> downloadArgs(const std::vector<std::string>& ytdlp, const std::string& ffmpeg, const std::string& url,
                                      const std::string& workDir);
struct Tags {
  std::string title, artist, url;
};
std::vector<std::string> convertArgs(const std::string& ffmpeg, const std::string& in, const std::string& out, const Trim& trim,
                                     const Tags& tags, const std::string& progressFile);

// ---- files ----
// A file name from a title: no path separators, reserved characters or names, trimmed, at most
// 120 bytes (cut on a UTF-8 boundary); "Imported audio" when nothing is left.
std::string safeFileName(const std::string& title);
// `dir`/`stem``ext`, or "`stem` (2)`ext`", "(3)", ... when taken.
std::string uniquePath(const std::string& dir, const std::string& stem, const std::string& ext);
// The source of an imported file, kept beside it in "<file>.source.json".
struct Source {
  std::string title, uploader, site, url, imported;  // imported: ISO 8601 date and time
  double duration = 0;
};
std::string sourcePath(const std::string& audioPath);
bool writeSource(const std::string& audioPath, const Source& s);
bool readSource(const std::string& audioPath, Source& out);

// ---- the worker ----
struct Tools {
  std::vector<std::string> ytdlp;  // program and leading arguments ({"python3", "yt-dlp"} for the zipapp)
  std::string ffmpeg;
};
struct Limits {
  double infoSeconds = 90;    // reading the link
  double stallSeconds = 120;  // download / conversion without any sign of life
};
enum class Phase { Idle, Info, Downloading, Converting, Done, Failed, Cancelled };
const char* phaseName(Phase p);  // "info", "downloading", ... (logs, dev output)

struct Request {
  Item item;  // url, title, uploader, duration
  std::string site;
  Trim trim;
};
struct Result {
  std::string wav;  // the converted file (in the work folder)
  Request request;
  double seconds = 0;
};
// Shared between the UI thread and the worker.
struct Job {
  std::atomic<int> phase{static_cast<int>(Phase::Idle)};
  std::atomic<double> fraction{-1};  // this item's progress, < 0 unknown
  std::atomic<int> item{0}, items{0};
  std::atomic<bool> cancel{false}, done{false};
  std::mutex mu;  // guards the fields below
  std::string error, lead;  // Failed: the tool's error and a short explanation
  std::string detail;       // e.g. "3.1 MB of 5.0 MB"
  Info info;                // fetch: what the link holds
  std::vector<Result> results;
  Phase get() const { return static_cast<Phase>(phase.load()); }
};
// Reads the link (Phase Info -> Done / Failed / Cancelled).
std::shared_ptr<Job> startFetch(const Tools& t, const std::string& url, const std::string& workDir, const Limits& lim = {});
// Downloads and converts each request in turn into `workDir`/<n>/ (Downloading / Converting ->
// Done with results / Failed / Cancelled). A cancel kills the running tool and everything it started.
std::shared_ptr<Job> startImport(const Tools& t, const std::vector<Request>& reqs, const std::string& workDir, const Limits& lim = {});

// Runs a program to its end with stdout / stderr in files (read while it runs). `onLine` sees each
// complete line of both. Gives up after `seconds` (> 0), or when nothing was written to either
// file nor to `watchDir` (any size change below it) for `stallSeconds` (> 0), or on `cancel`.
struct RunResult {
  bool started = false, ok = false, timedOut = false, cancelled = false;
  int code = -1;
  std::string out, err;  // everything written
};
RunResult run(const std::vector<std::string>& argv, const std::string& scratchBase, const std::atomic<bool>* cancel, double seconds,
              double stallSeconds, const std::string& watchDir, const std::function<void(const std::string&)>& onLine);

}  // namespace gl::urlimport
