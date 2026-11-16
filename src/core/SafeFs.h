// Gridline - K-307: file system questions about strings that came from outside (a project, a
// plugin's saved state or parameter text, a drop, the command line). Such a string need not be a
// path at all: a hosted plugin's state can hold a list of names on lines, or text over the
// system's name limits, and the throwing std::filesystem calls end the thread that asks
// ("status: File name too long" in an export worker). These never throw: a string that cannot
// name a file is "not there".
#pragma once
#include <filesystem>
#include <string>
#include <system_error>

namespace gl::safefs {

inline constexpr size_t kMaxPath = 4096;  // PATH_MAX on Linux (longer than macOS and Windows allow)
inline constexpr size_t kMaxName = 255;   // one name in a path (NAME_MAX)

// Could `s` name a file? Not empty, no control characters (a newline: text, not a path), no
// longer than kMaxPath and no name in it (between separators) longer than kMaxName. On Windows
// both slashes separate names; elsewhere only '/', so a backslash run counts as one name.
inline bool plausiblePath(const std::string& s) noexcept {
  if (s.empty() || s.size() > kMaxPath) return false;
  size_t run = 0;
  for (unsigned char c : s) {
    if (c < 0x20 || c == 0x7F) return false;
#ifdef _WIN32
    const bool sep = c == '/' || c == '\\';
#else
    const bool sep = c == '/';
#endif
    run = sep ? 0 : run + 1;
    if (run > kMaxName) return false;
  }
  return true;
}

inline std::filesystem::path toPath(const std::string& s) noexcept {
  try {
    return std::filesystem::u8path(s);
  } catch (...) {  // (invalid UTF-8 on Windows)
    return {};
  }
}

inline bool exists(const std::string& s) noexcept {
  if (!plausiblePath(s)) return false;
  std::error_code ec;
  return std::filesystem::exists(toPath(s), ec) && !ec;
}
inline bool isFile(const std::string& s) noexcept {
  if (!plausiblePath(s)) return false;
  std::error_code ec;
  return std::filesystem::is_regular_file(toPath(s), ec) && !ec;
}
inline bool isDir(const std::string& s) noexcept {
  if (!plausiblePath(s)) return false;
  std::error_code ec;
  return std::filesystem::is_directory(toPath(s), ec) && !ec;
}
// The same for a path built here (no name check), without the exception.
inline bool existsAt(const std::filesystem::path& p) noexcept {
  std::error_code ec;
  return std::filesystem::exists(p, ec) && !ec;
}
inline bool isFileAt(const std::filesystem::path& p) noexcept {
  std::error_code ec;
  return std::filesystem::is_regular_file(p, ec) && !ec;
}
inline bool isDirAt(const std::filesystem::path& p) noexcept {
  std::error_code ec;
  return std::filesystem::is_directory(p, ec) && !ec;
}

}  // namespace gl::safefs
