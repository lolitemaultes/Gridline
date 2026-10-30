// Gridline - J-192: Import from URL, pure logic and the worker (see UrlImport.h).
#include "UrlImport.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include "Json.h"
#include "Platform.h"

namespace fs = std::filesystem;

namespace gl::urlimport {

namespace {

std::string lower(std::string s) {
  for (auto& c : s)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return s;
}

std::string trim(const std::string& s) {
  const size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  const size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

bool contains(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

int64_t toInt(const std::string& s) {
  if (s.empty() || s == "NA" || s == "None") return -1;
  char* end = nullptr;
  const double v = platform::parseDouble(s.c_str(), &end);
  if (end == s.c_str() || !std::isfinite(v) || v < 0) return -1;
  return static_cast<int64_t>(v);
}

std::string isoNow() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char b[32];
  std::strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return b;
}

}  // namespace

// ---------------------------------------------------------------- links ----
bool validUrl(const std::string& url, std::string* why) {
  auto fail = [&](const char* w) {
    if (why) *why = w;
    return false;
  };
  if (url.empty()) return fail("Enter a link");
  if (url.size() > 4096) return fail("The link is too long");
  for (unsigned char c : url)
    if (c <= 0x20 || c == 0x7F) return fail("A link has no spaces");
  const std::string l = lower(url.substr(0, 8));
  size_t p = 0;
  if (l.rfind("https://", 0) == 0) p = 8;
  else if (l.rfind("http://", 0) == 0) p = 7;
  else return fail("Links start with https:// or http://");
  const size_t end = url.find_first_of("/?#", p);
  std::string host = url.substr(p, end == std::string::npos ? std::string::npos : end - p);
  const size_t at = host.rfind('@');
  if (at != std::string::npos) host = host.substr(at + 1);
  if (host.empty() || host[0] == ':' || host[0] == '.') return fail("The link has no site name");
  return true;
}

std::string linkInText(const std::string& text) {
  std::string t = trim(text);
  while (!t.empty() && (t.front() == '"' || t.front() == '\'' || t.front() == '<')) t.erase(t.begin());
  while (!t.empty() && (t.back() == '"' || t.back() == '\'' || t.back() == '>')) t.pop_back();
  return validUrl(t) ? t : std::string();
}

std::string hostOf(const std::string& url) {
  const size_t s = url.find("://");
  if (s == std::string::npos) return "";
  const size_t p = s + 3;
  const size_t end = url.find_first_of("/?#", p);
  std::string h = lower(url.substr(p, end == std::string::npos ? std::string::npos : end - p));
  const size_t at = h.rfind('@');
  if (at != std::string::npos) h = h.substr(at + 1);
  const size_t colon = h.find(':');
  if (colon != std::string::npos) h = h.substr(0, colon);
  if (h.rfind("www.", 0) == 0) h = h.substr(4);
  return h;
}

// ----------------------------------------------------------------- info ----
namespace {
std::string uploaderOf(const Json& j) {
  for (const char* k : {"uploader", "channel", "artist", "creator", "uploader_id"}) {
    const std::string v = j.text(k);
    if (!v.empty()) return v;
  }
  return "";
}
}  // namespace

bool parseInfo(const std::string& json, Info& out, std::string* err) {
  out = Info{};
  Json j;
  // yt-dlp prints one JSON document; anything before it (stray lines) is skipped.
  const size_t brace = json.find('{');
  if (brace == std::string::npos || !jsonParse(json.substr(brace), j) || !j.is(Json::Type::Object)) {
    if (err) *err = "The downloader's answer could not be read";
    return false;
  }
  out.title = j.text("title");
  out.uploader = uploaderOf(j);
  out.site = j.text("extractor_key", j.text("extractor"));
  out.url = j.text("webpage_url", j.text("original_url"));
  out.duration = std::max(0.0, j.num("duration"));
  const Json* live = j.find("is_live");
  out.live = (live && live->is(Json::Type::Bool) && live->boolean) || j.text("live_status") == "is_live";
  if (j.text("_type") == "playlist") {
    out.playlist = true;
    if (const Json* e = j.find("entries"); e && e->is(Json::Type::Array)) {
      for (const Json& x : e->array) {
        if (!x.is(Json::Type::Object)) continue;
        Item it;
        it.id = x.text("id");
        it.title = x.text("title");
        it.uploader = uploaderOf(x);
        it.url = x.text("url", x.text("webpage_url"));
        if (it.url.empty() || !validUrl(it.url)) it.url = x.text("webpage_url", x.text("original_url"));
        it.duration = std::max(0.0, x.num("duration"));
        if (it.title.empty()) it.title = it.id;
        if (!it.url.empty() && validUrl(it.url)) out.entries.push_back(it);
      }
    }
    if (out.entries.empty()) {
      if (err) *err = "The playlist is empty or its items cannot be read";
      return false;
    }
  }
  if (out.title.empty()) out.title = out.playlist ? std::string("Playlist") : j.text("id", "Imported audio");
  return true;
}

// ------------------------------------------------------------- progress ----
bool parseDownloadLine(const std::string& line, DownloadProgress& out) {
  const size_t at = line.find("GLPROG ");
  if (at == std::string::npos) return false;
  std::istringstream ss(line.substr(at + 7));
  std::string got, total, fi, fc;
  ss >> got >> total >> fi >> fc;
  out = DownloadProgress{};
  out.got = toInt(got);
  out.total = toInt(total);
  const int64_t i = toInt(fi), n = toInt(fc);
  if (out.got >= 0 && out.total > 0) out.fraction = std::clamp(static_cast<double>(out.got) / static_cast<double>(out.total), 0.0, 1.0);
  else if (i >= 0 && n > 0) out.fraction = std::clamp(static_cast<double>(i) / static_cast<double>(n), 0.0, 1.0);
  return true;
}

bool parseFfmpegTime(const std::string& line, double& seconds) {
  const std::string l = trim(line);
  auto value = [&](const char* key) -> std::string {
    const size_t n = std::char_traits<char>::length(key);
    return l.compare(0, n, key) == 0 ? l.substr(n) : std::string();
  };
  std::string v = value("out_time_us=");
  if (!v.empty()) {
    const int64_t us = toInt(v);
    if (us < 0) return false;
    seconds = static_cast<double>(us) / 1e6;
    return true;
  }
  v = value("out_time_ms=");  // ffmpeg prints microseconds here too (a long-standing quirk)
  if (!v.empty()) {
    const int64_t us = toInt(v);
    if (us < 0) return false;
    seconds = static_cast<double>(us) / 1e6;
    return true;
  }
  v = value("out_time=");
  if (!v.empty()) {
    const double t = parseTime(v);
    if (t < 0) return false;
    seconds = t;
    return true;
  }
  return false;
}

std::string toolError(const std::string& errText) {
  std::istringstream ss(errText);
  std::string last, lastAny;
  for (std::string line; std::getline(ss, line);) {
    line = trim(line);
    if (line.empty()) continue;
    lastAny = line;
    if (line.rfind("ERROR:", 0) == 0) last = trim(line.substr(6));
  }
  if (last.empty()) return lastAny;
  if (!last.empty() && last[0] == '[') {  // "[youtube] abc123: Video unavailable"
    const size_t close = last.find("] ");
    if (close != std::string::npos) {
      last = last.substr(close + 2);
      const size_t colon = last.find(": ");
      if (colon != std::string::npos && colon < 64 && last.substr(0, colon).find(' ') == std::string::npos) last = last.substr(colon + 2);
    }
  }
  return last;
}

std::string errorLead(const std::string& e) {
  const std::string l = lower(e);
  if (contains(l, "drm")) return "This media is protected (DRM) and cannot be imported.";
  if (contains(l, "sign in") || contains(l, "log in") || contains(l, "login") || contains(l, "cookies") || contains(l, "authenticat") ||
      contains(l, "members-only") || contains(l, "account"))
    return "This media needs a signed-in account. Gridline does not sign in to sites.";
  if (contains(l, "private")) return "This media is private.";
  if (contains(l, "unsupported url")) return "This link is not a media page the downloader knows.";
  if (contains(l, "your country") || contains(l, "geo restrict") || contains(l, "geo-restrict") || contains(l, "region"))
    return "This media is not available in your region.";
  if (contains(l, "requested format is not available") || contains(l, "no video formats") || contains(l, "no formats"))
    return "No audio stream was found on this page.";
  if (contains(l, "unable to download webpage") || contains(l, "getaddrinfo") || contains(l, "name or service not known") ||
      contains(l, "nodename nor servname") || contains(l, "timed out") || contains(l, "network is unreachable") ||
      contains(l, "connection refused") || contains(l, "connection reset") || contains(l, "failed to resolve"))
    return "The site could not be reached. Check the internet connection and try again.";
  if (contains(l, "unavailable") || contains(l, "removed") || contains(l, "404") || contains(l, "not found") || contains(l, "does not exist"))
    return "This media is not available (removed, or the link is not right).";
  if (contains(l, "live")) return "Live streams cannot be imported.";
  return "";
}

// ---------------------------------------------------------------- times ----
double parseTime(const std::string& text) {
  const std::string t = trim(text);
  if (t.empty()) return -1;
  double total = 0;
  int parts = 0;
  size_t p = 0;
  while (p <= t.size()) {
    const size_t c = t.find(':', p);
    const std::string part = t.substr(p, c == std::string::npos ? std::string::npos : c - p);
    if (part.empty() || part.find_first_not_of("0123456789.") != std::string::npos) return -1;
    char* end = nullptr;
    const double v = platform::parseDouble(part.c_str(), &end);
    if (end != part.c_str() + part.size() || !std::isfinite(v)) return -1;
    if (parts > 0 && v >= 60) return -1;
    total = total * 60 + v;
    if (++parts > 3) return -1;
    if (c == std::string::npos) break;
    p = c + 1;
  }
  return total;
}

std::string formatTime(double seconds) {
  if (!(seconds >= 0) || !std::isfinite(seconds)) return "?";
  const long s = std::lround(seconds);
  char b[32];
  if (s >= 3600) std::snprintf(b, sizeof b, "%ld:%02ld:%02ld", s / 3600, (s / 60) % 60, s % 60);
  else std::snprintf(b, sizeof b, "%ld:%02ld", s / 60, s % 60);
  return b;
}

// -------------------------------------------------------- command lines ----
std::vector<std::string> infoArgs(const std::vector<std::string>& ytdlp, const std::string& url) {
  std::vector<std::string> a = ytdlp;
  for (const char* x : {"--ignore-config", "--no-warnings", "--socket-timeout", "30", "--flat-playlist", "-J", "--"}) a.push_back(x);
  a.push_back(url);
  return a;
}

std::vector<std::string> downloadArgs(const std::vector<std::string>& ytdlp, const std::string& ffmpeg, const std::string& url,
                                      const std::string& workDir) {
  std::vector<std::string> a = ytdlp;
  for (const char* x : {"--ignore-config", "--no-playlist", "-f", "bestaudio/best", "--newline", "--progress", "--progress-template",
                        "download:GLPROG %(progress.downloaded_bytes)s %(progress.total_bytes,progress.total_bytes_estimate)s "
                        "%(progress.fragment_index)s %(progress.fragment_count)s",
                        "--no-mtime", "--socket-timeout", "30", "--retries", "3", "--fragment-retries", "3"})
    a.push_back(x);
  if (!ffmpeg.empty()) {
    a.push_back("--ffmpeg-location");
    a.push_back(ffmpeg);
  }
  a.push_back("-o");
  a.push_back((fs::u8path(workDir) / "media.%(ext)s").u8string());
  a.push_back("--");
  a.push_back(url);
  return a;
}

std::vector<std::string> convertArgs(const std::string& ffmpeg, const std::string& in, const std::string& out, const Trim& trim,
                                     const Tags& tags, const std::string& progressFile) {
  std::vector<std::string> a = {ffmpeg, "-nostdin", "-hide_banner", "-nostats", "-y", "-i", in};
  if (trim.start > 0) {
    a.push_back("-ss");
    a.push_back(platform::formatDouble("%.3f", trim.start));
  }
  if (trim.end > 0) {
    a.push_back("-t");
    a.push_back(platform::formatDouble("%.3f", std::max(0.001, trim.end - std::max(0.0, trim.start))));
  }
  for (const char* x : {"-map", "0:a:0", "-vn", "-sn", "-dn", "-map_metadata", "-1", "-c:a", "pcm_f32le", "-rf64", "auto"}) a.push_back(x);
  auto meta = [&](const char* key, const std::string& v) {
    if (v.empty()) return;
    a.push_back("-metadata");
    a.push_back(std::string(key) + "=" + v);
  };
  meta("title", tags.title);
  meta("artist", tags.artist);
  meta("comment", tags.url);
  a.push_back("-progress");
  a.push_back(progressFile.empty() ? std::string("pipe:1") : progressFile);
  a.push_back(out);
  return a;
}

// ---------------------------------------------------------------- files ----
std::string safeFileName(const std::string& title) {
  std::string s;
  for (unsigned char c : title) {
    if (c < 0x20 || c == 0x7F || std::string("<>:\"/\\|?*").find(static_cast<char>(c)) != std::string::npos) c = ' ';
    if (c == ' ' && (s.empty() || s.back() == ' ')) continue;
    s.push_back(static_cast<char>(c));
  }
  if (s.size() > 120) {
    size_t n = 120;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;  // not inside a UTF-8 sequence
    s.resize(n);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
  while (!s.empty() && (s.front() == ' ' || s.front() == '.' || s.front() == '-')) s.erase(s.begin());
  if (s.empty()) return "Imported audio";
  // Names Windows reserves (also with an extension).
  std::string base = lower(s.substr(0, s.find('.')));
  static const char* reserved[] = {"con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9",
                                   "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
  for (const char* r : reserved)
    if (base == r) return s + "_";
  return s;
}

std::string uniquePath(const std::string& dir, const std::string& stem, const std::string& ext) {
  const fs::path d = fs::u8path(dir);
  std::error_code ec;
  fs::path p = d / fs::u8path(stem + ext);
  for (int n = 2; fs::exists(p, ec) || fs::exists(fs::u8path(sourcePath(p.u8string())), ec); ++n)
    p = d / fs::u8path(stem + " (" + std::to_string(n) + ")" + ext);
  return p.u8string();
}

std::string sourcePath(const std::string& audioPath) { return audioPath + ".source.json"; }

bool writeSource(const std::string& audioPath, const Source& s) {
  Json j = Json::makeObject();
  j.object["title"] = Json::makeString(s.title);
  j.object["uploader"] = Json::makeString(s.uploader);
  j.object["site"] = Json::makeString(s.site);
  j.object["url"] = Json::makeString(s.url);
  j.object["duration"] = Json::makeNumber(s.duration);
  j.object["imported"] = Json::makeString(s.imported.empty() ? isoNow() : s.imported);
  j.object["format"] = Json::makeString("gridline-import-source-1");
  std::ofstream f(fs::u8path(sourcePath(audioPath)), std::ios::binary);
  f << jsonStringify(j) << "\n";
  return static_cast<bool>(f);
}

bool readSource(const std::string& audioPath, Source& out) {
  std::ifstream f(fs::u8path(sourcePath(audioPath)), std::ios::binary);
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  Json j;
  if (!jsonParse(ss.str(), j) || !j.is(Json::Type::Object)) return false;
  out.title = j.text("title");
  out.uploader = j.text("uploader");
  out.site = j.text("site");
  out.url = j.text("url");
  out.imported = j.text("imported");
  out.duration = j.num("duration");
  return true;
}

const char* phaseName(Phase p) {
  switch (p) {
    case Phase::Idle: return "idle";
    case Phase::Info: return "info";
    case Phase::Downloading: return "downloading";
    case Phase::Converting: return "converting";
    case Phase::Done: return "done";
    case Phase::Failed: return "failed";
    case Phase::Cancelled: return "cancelled";
  }
  return "?";
}

// ---------------------------------------------------------------- run ----
namespace {

// New bytes of a file a child is writing (from `offset` on).
std::string readFrom(const fs::path& p, uint64_t& offset) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return "";
  f.seekg(0, std::ios::end);
  const std::streamoff size = f.tellg();
  if (size <= 0 || static_cast<uint64_t>(size) <= offset) return "";
  f.seekg(static_cast<std::streamoff>(offset));
  std::string s(static_cast<size_t>(static_cast<uint64_t>(size) - offset), '\0');
  f.read(&s[0], static_cast<std::streamsize>(s.size()));
  s.resize(static_cast<size_t>(f.gcount()));
  offset += s.size();
  return s;
}

uint64_t treeSize(const std::string& dir) {
  if (dir.empty()) return 0;
  uint64_t n = 0;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(fs::u8path(dir), fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
       it.increment(ec)) {
    std::error_code e2;
    if (it->is_regular_file(e2)) n += it->file_size(e2);
  }
  return n;
}

// Splits text into lines at '\n' or '\r' (progress bars rewrite their line with '\r').
void feed(std::string& pending, const std::string& add, const std::function<void(const std::string&)>& onLine) {
  pending += add;
  size_t start = 0;
  for (size_t i = 0; i < pending.size(); ++i) {
    if (pending[i] != '\n' && pending[i] != '\r') continue;
    if (i > start && onLine) onLine(pending.substr(start, i - start));
    start = i + 1;
  }
  pending.erase(0, start);
}

}  // namespace

RunResult run(const std::vector<std::string>& argv, const std::string& scratchBase, const std::atomic<bool>* cancel, double seconds,
              double stallSeconds, const std::string& watchDir, const std::function<void(const std::string&)>& onLine) {
  RunResult r;
  if (argv.empty()) return r;
  const fs::path outP = fs::u8path(scratchBase + ".out"), errP = fs::u8path(scratchBase + ".err");
  std::error_code ec;
  fs::create_directories(outP.parent_path(), ec);
  platform::Process p;
  p.redirectOutput(outP.u8string(), errP.u8string());
  if (!p.start(argv[0], argv, platform::Process::kTree)) {
    fs::remove(outP, ec);
    fs::remove(errP, ec);
    return r;
  }
  r.started = true;
  using clock = std::chrono::steady_clock;
  const auto t0 = clock::now();
  auto lastLife = t0, lastScan = t0;
  uint64_t offOut = 0, offErr = 0, lastSize = treeSize(watchDir);
  std::string pendOut, pendErr;
  auto pump = [&]() {
    const std::string o = readFrom(outP, offOut), e = readFrom(errP, offErr);
    if (!o.empty() || !e.empty()) lastLife = clock::now();
    r.out += o;
    r.err += e;
    feed(pendOut, o, onLine);
    feed(pendErr, e, onLine);
  };
  for (;;) {
    const bool exited = p.wait(0.1);
    pump();
    if (exited) break;
    const auto now = clock::now();
    if (!watchDir.empty() && std::chrono::duration<double>(now - lastScan).count() >= 1.0) {
      lastScan = now;
      const uint64_t sz = treeSize(watchDir);
      if (sz != lastSize) lastSize = sz, lastLife = now;
    }
    const double el = std::chrono::duration<double>(now - t0).count();
    const double quiet = std::chrono::duration<double>(now - lastLife).count();
    const bool stop = cancel && cancel->load();
    if (stop || (seconds > 0 && el > seconds) || (stallSeconds > 0 && quiet > stallSeconds)) {
      r.cancelled = stop;
      r.timedOut = !stop;
      p.kill();
      p.wait(10);
      pump();
      break;
    }
  }
  if (!pendOut.empty() && onLine) onLine(pendOut);
  if (!pendErr.empty() && onLine) onLine(pendErr);
  r.code = p.status().crashed ? -1 : p.status().code;
  r.ok = !r.cancelled && !r.timedOut && p.status().exited && !p.status().crashed && p.status().code == 0;
  fs::remove(outP, ec);
  fs::remove(errP, ec);
  return r;
}

// --------------------------------------------------------------- worker ----
namespace {

void fail(Job& j, const RunResult& r, const std::string& fallback) {
  std::lock_guard<std::mutex> lk(j.mu);
  if (r.cancelled) {
    j.phase = static_cast<int>(Phase::Cancelled);
    return;
  }
  if (!r.started) {
    j.error = fallback;
  } else if (r.timedOut) {
    j.error = "The download stopped making progress and was given up";
    j.lead = "The site did not answer in time. Try again later.";
  } else {
    j.error = toolError(r.err);
    if (j.error.empty()) j.error = fallback + " (" + (r.code < 0 ? std::string("stopped") : "exit code " + std::to_string(r.code)) + ")";
    j.lead = errorLead(j.error);
  }
  j.phase = static_cast<int>(Phase::Failed);
}

std::string findMedia(const fs::path& dir) {
  std::string best;
  uintmax_t bestSize = 0;
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string n = it->path().filename().u8string();
    if (n.rfind("media.", 0) != 0) continue;
    const std::string l = lower(n);
    if (l.size() > 5 && (l.compare(l.size() - 5, 5, ".part") == 0 || l.compare(l.size() - 5, 5, ".ytdl") == 0)) continue;
    if (l.find(".temp.") != std::string::npos || l.find(".part-") != std::string::npos) continue;
    std::error_code e2;
    const uintmax_t sz = it->file_size(e2);
    if (!e2 && sz > bestSize) best = it->path().u8string(), bestSize = sz;
  }
  return best;
}

std::string mbText(int64_t b) {
  char t[32];
  if (b < 1024 * 1024) std::snprintf(t, sizeof t, "%.0f KB", static_cast<double>(b) / 1024.0);
  else std::snprintf(t, sizeof t, "%.1f MB", static_cast<double>(b) / (1024.0 * 1024.0));
  return t;
}

}  // namespace

std::shared_ptr<Job> startFetch(const Tools& t, const std::string& url, const std::string& workDir, const Limits& lim) {
  auto job = std::make_shared<Job>();
  job->phase = static_cast<int>(Phase::Info);
  job->items = 1;
  std::thread([job, t, url, workDir, lim] {
    std::error_code ec;
    fs::create_directories(fs::u8path(workDir), ec);
    const RunResult r = run(infoArgs(t.ytdlp, url), (fs::u8path(workDir) / "info").u8string(), &job->cancel, lim.infoSeconds, 0, "", nullptr);
    if (!r.ok) {
      fail(*job, r, "The downloader could not be started");
    } else {
      Info info;
      std::string err;
      std::lock_guard<std::mutex> lk(job->mu);
      if (!parseInfo(r.out, info, &err)) {
        job->error = err;
        job->phase = static_cast<int>(Phase::Failed);
      } else if (info.live) {
        job->error = "This is a live stream";
        job->lead = "Live streams cannot be imported.";
        job->phase = static_cast<int>(Phase::Failed);
      } else {
        if (info.url.empty()) info.url = url;
        job->info = std::move(info);
        job->phase = static_cast<int>(Phase::Done);
      }
    }
    job->done = true;
  }).detach();
  return job;
}

std::shared_ptr<Job> startImport(const Tools& t, const std::vector<Request>& reqs, const std::string& workDir, const Limits& lim) {
  auto job = std::make_shared<Job>();
  job->phase = static_cast<int>(Phase::Downloading);
  job->items = static_cast<int>(reqs.size());
  std::thread([job, t, reqs, workDir, lim] {
    for (size_t i = 0; i < reqs.size(); ++i) {
      const Request& rq = reqs[i];
      job->item = static_cast<int>(i);
      job->fraction = -1;
      job->phase = static_cast<int>(Phase::Downloading);
      const fs::path dir = fs::u8path(workDir) / std::to_string(i + 1);
      std::error_code ec;
      fs::remove_all(dir, ec);
      fs::create_directories(dir, ec);
      // 1. the best audio as it is
      RunResult r = run(downloadArgs(t.ytdlp, t.ffmpeg, rq.item.url, dir.u8string()), (fs::u8path(workDir) / "dl").u8string(), &job->cancel,
                        0, lim.stallSeconds, dir.u8string(), [&](const std::string& line) {
                          DownloadProgress p;
                          if (!parseDownloadLine(line, p)) return;
                          if (p.fraction >= 0) job->fraction = p.fraction;
                          if (p.got >= 0) {
                            std::lock_guard<std::mutex> lk(job->mu);
                            job->detail = p.total > 0 ? mbText(p.got) + " / " + mbText(p.total) : mbText(p.got);
                          }
                        });
      const std::string media = r.ok ? findMedia(dir) : std::string();
      if (!r.ok || media.empty()) {
        if (r.ok) {
          std::lock_guard<std::mutex> lk(job->mu);
          job->error = "The downloader finished without an audio file";
          job->phase = static_cast<int>(Phase::Failed);
        } else {
          fail(*job, r, "The downloader could not be started");
        }
        job->done = true;
        return;
      }
      // 2. decoded once into a float WAV (the decoder's output exactly)
      job->phase = static_cast<int>(Phase::Converting);
      job->fraction = -1;
      {
        std::lock_guard<std::mutex> lk(job->mu);
        job->detail.clear();
      }
      double total = rq.trim.end > 0 ? rq.trim.end - std::max(0.0, rq.trim.start) : rq.item.duration - std::max(0.0, rq.trim.start);
      const std::string wav = (dir / "audio.wav").u8string();
      double seconds = 0;
      Tags tags{rq.item.title, rq.item.uploader, rq.item.url};
      r = run(convertArgs(t.ffmpeg, media, wav, rq.trim, tags, ""), (fs::u8path(workDir) / "cv").u8string(), &job->cancel, 0, lim.stallSeconds,
              dir.u8string(), [&](const std::string& line) {
                double s = 0;
                const size_t d = line.find("Duration: ");
                if (d != std::string::npos && total <= 0) {
                  const double v = parseTime(line.substr(d + 10, line.find(',', d) == std::string::npos ? std::string::npos : line.find(',', d) - d - 10));
                  if (v > 0) total = v - std::max(0.0, rq.trim.start);
                }
                if (parseFfmpegTime(line, s)) {
                  seconds = s;
                  if (total > 0) job->fraction = std::clamp(s / total, 0.0, 1.0);
                }
              });
      std::error_code e3;
      if (!r.ok || !fs::exists(fs::u8path(wav), e3) || fs::file_size(fs::u8path(wav), e3) <= 44) {
        if (r.ok) {
          std::lock_guard<std::mutex> lk(job->mu);
          job->error = "The converter wrote no audio";
          job->lead = "The file has no audio the converter can read.";
          job->phase = static_cast<int>(Phase::Failed);
        } else {
          fail(*job, r, "The converter could not be started");
        }
        job->done = true;
        return;
      }
      fs::remove(fs::u8path(media), e3);  // the WAV holds it all now
      std::lock_guard<std::mutex> lk(job->mu);
      job->results.push_back(Result{wav, rq, seconds});
    }
    job->fraction = 1;
    job->phase = static_cast<int>(Phase::Done);
    job->done = true;
  }).detach();
  return job;
}

}  // namespace gl::urlimport
