// Gridline — debug log (see Log.h).
#include "Log.h"
#include <chrono>
#include <cstdio>
#include <deque>
#include <fstream>
#include <mutex>

namespace gl::logging {

namespace {
struct State {
  std::mutex mu;
  std::deque<Entry> entries;
  uint64_t serial = 0;
  Level max = Level::Info;
  std::string file;
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};
State& st() {
  static State s;
  return s;
}
}  // namespace

const char* levelName(Level l) {
  switch (l) {
    case Level::Error: return "error";
    case Level::Warning: return "warning";
    case Level::Info: return "info";
    default: return "verbose";
  }
}

std::string format(const Entry& e) {
  char head[64];
  std::snprintf(head, sizeof head, "%9.3f  %-7s  ", e.time, levelName(e.level));
  return head + e.source + "  " + e.text;
}

void add(Level level, const std::string& source, const std::string& text) {
  State& s = st();
  std::lock_guard<std::mutex> lk(s.mu);
  if (static_cast<int>(level) > static_cast<int>(s.max)) return;
  Entry e;
  e.time = std::chrono::duration<double>(std::chrono::steady_clock::now() - s.start).count();
  e.level = level;
  e.source = source;
  e.text = text.size() > 4000 ? text.substr(0, 4000) : text;
  if (!s.file.empty()) {
    std::ofstream os(s.file, std::ios::app);
    os << format(e) << "\n";
  }
  s.entries.push_back(std::move(e));
  while (s.entries.size() > kMaxEntries) s.entries.pop_front();
  ++s.serial;
}

std::vector<Entry> entries() {
  State& s = st();
  std::lock_guard<std::mutex> lk(s.mu);
  return std::vector<Entry>(s.entries.begin(), s.entries.end());
}
uint64_t serial() {
  State& s = st();
  std::lock_guard<std::mutex> lk(s.mu);
  return s.serial;
}
void clear() {
  State& s = st();
  std::lock_guard<std::mutex> lk(s.mu);
  s.entries.clear();
  ++s.serial;
}
void setMaxLevel(Level l) {
  State& s = st();
  std::lock_guard<std::mutex> lk(s.mu);
  s.max = l;
}
Level maxLevel() {
  State& s = st();
  std::lock_guard<std::mutex> lk(s.mu);
  return s.max;
}
void setFile(const std::string& path) {
  State& s = st();
  std::lock_guard<std::mutex> lk(s.mu);
  s.file = path;
}
std::string file() {
  State& s = st();
  std::lock_guard<std::mutex> lk(s.mu);
  return s.file;
}

}  // namespace gl::logging
