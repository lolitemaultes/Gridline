// Gridline — debug log (Settings → Debug): a bounded, thread-safe list of
// messages from the app, scripts and devices, optionally mirrored to a file.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace gl::logging {

enum class Level : int { Error = 0, Warning, Info, Verbose };
const char* levelName(Level l);

struct Entry {
  double time = 0;  // seconds since the log started
  Level level = Level::Info;
  std::string source;  // "app", "script", "midi", …
  std::string text;
};

inline constexpr size_t kMaxEntries = 4000;

// Messages above `maxLevel()` are dropped.
void add(Level level, const std::string& source, const std::string& text);
inline void error(const std::string& src, const std::string& t) { add(Level::Error, src, t); }
inline void warning(const std::string& src, const std::string& t) { add(Level::Warning, src, t); }
inline void info(const std::string& src, const std::string& t) { add(Level::Info, src, t); }
inline void verbose(const std::string& src, const std::string& t) { add(Level::Verbose, src, t); }

std::vector<Entry> entries();
uint64_t serial();  // increases with every added entry
void clear();
void setMaxLevel(Level l);
Level maxLevel();
// Appends every new entry to `path` ("" = no file).
void setFile(const std::string& path);
std::string file();
std::string format(const Entry& e);  // "  12.345  info   script  text"

}  // namespace gl::logging
