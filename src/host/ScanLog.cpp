#include "ScanLog.h"
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include "GridlineVersion.h"

namespace fs = std::filesystem;

namespace gl::host {

const char* scanOutcomeName(ScanReport::Outcome o) {
  switch (o) {
    case ScanReport::Outcome::Listed: return "listed";
    case ScanReport::Outcome::Failed: return "failed";
    case ScanReport::Outcome::NotForThisComputer: return "not for this computer";
    case ScanReport::Outcome::TimedOut: return "timed out";
    case ScanReport::Outcome::Crashed: return "crashed";
    case ScanReport::Outcome::TooMuchMemory: return "too much memory";
    case ScanReport::Outcome::Cancelled: return "cancelled";
  }
  return "failed";
}

std::string scanLogTime() {
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &now);
#else
  localtime_r(&now, &tm);
#endif
  char b[32];
  std::strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &tm);
  return b;
}

std::string scanLogHeader(const std::string& time, int entries, int jobs) {
#if defined(_WIN32)
  const char* system = "Windows";
#elif defined(__APPLE__)
  const char* system = "macOS";
#elif defined(__linux__)
  const char* system = "Linux";
#else
  const char* system = "BSD";
#endif
#if defined(_M_ARM64) || defined(__aarch64__)
  const char* cpu = "arm64";
#elif defined(_M_IX86) || defined(__i386__)
  const char* cpu = "x86";
#else
  const char* cpu = "x64";
#endif
  char b[256];
  std::snprintf(b, sizeof b, "== %s  scan of %d plugin files, %d at a time  (Gridline %s, %s %s)\n", time.c_str(), entries, jobs, GRIDLINE_VERSION,
                system, cpu);
  return b;
}

std::string formatScanLogEntry(const ScanLogEntry& e) {
  const ScanReport& r = e.report;
  std::string s = e.time + "  " + e.format + "  " + e.entry + "\n";
  s += "  result: " + std::string(scanOutcomeName(r.outcome)) + (e.attempt > 0 ? " (retried alone)" : "") + ": " + e.error + "\n";
  char b[200];
  std::snprintf(b, sizeof b, "  scanner: %s after %.1f s, %d plugins described, peak memory %.0f MB\n", r.exit.empty() ? "did not start" : r.exit.c_str(),
                r.seconds, r.progress, static_cast<double>(r.peakMemory) / (1024.0 * 1024.0));
  s += b;
  if (!r.note.empty()) s += "  note: " + r.note + "\n";
  if (e.retried) s += "  it is tried once more, alone, after the other plugins\n";
  if (!r.childLog.empty()) {
    s += "  scanner steps:\n";
    size_t a = 0;
    while (a < r.childLog.size()) {
      size_t nl = r.childLog.find('\n', a);
      if (nl == std::string::npos) nl = r.childLog.size();
      std::string line = r.childLog.substr(a, nl - a);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (!line.empty()) s += "    " + line + "\n";
      a = nl + 1;
    }
  }
  return s;
}

bool appendScanLog(const std::string& file, const std::string& text, std::uintmax_t maxBytes) {
  if (file.empty()) return false;
  const fs::path p = fs::u8path(file);
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  const std::uintmax_t size = fs::file_size(p, ec);
  if (!ec && size > 0 && size + text.size() > maxBytes) {  // starts again rather than grow past the limit
    const fs::path old = fs::u8path(file + ".old");
    fs::remove(old, ec);
    fs::rename(p, old, ec);
  }
  std::ofstream f(p, std::ios::binary | std::ios::app);
  f << text;
  return static_cast<bool>(f);
}

}  // namespace gl::host
