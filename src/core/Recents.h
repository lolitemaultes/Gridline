// Gridline - J-141: the recent projects list (File > Open recent, the browser's Recent
// files, the welcome launcher). `recent.txt` in the config folder holds the paths, most
// recent first; `recent-times.tsv` beside it holds when each was last opened or saved
// (Unix seconds), so the launcher can say "2 h ago". Pure file handling, no UI.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace gl::recents {

inline constexpr size_t kMaxRecent = 24;

struct List {
  std::vector<std::string> paths;          // most recent first, no duplicates
  std::map<std::string, int64_t> opened;   // path -> last opened / saved (Unix seconds)

  // Moves `path` to the front (adding it), stamps it with `now` and keeps at most `cap` paths.
  void touch(const std::string& path, int64_t now, size_t cap = kMaxRecent);
  // Drops `path`; false when it was not listed.
  bool remove(const std::string& path);
  // Drops every path for which `exists` is false; returns how many went.
  template <class Exists>
  size_t pruneMissing(Exists exists) {
    size_t n = 0;
    for (size_t i = 0; i < paths.size();)
      if (!exists(paths[i])) {
        opened.erase(paths[i]);
        paths.erase(paths.begin() + static_cast<std::ptrdiff_t>(i));
        ++n;
      } else {
        ++i;
      }
    return n;
  }
  void clear() { paths.clear(), opened.clear(); }
  // Last opened time, else 0.
  int64_t openedAt(const std::string& path) const;
};

// Reads / writes the two files in `dir` (missing files: an empty list). load() drops blank
// lines and duplicates and keeps at most kMaxRecent paths.
List load(const std::string& dir);
bool save(const std::string& dir, const List& l);

// The file exists and is a regular file (or a folder: collaborative projects).
bool exists(const std::string& path);

// How long ago `then` was, for "5 min ago" style text: the unit and the count.
enum class AgeUnit { None, JustNow, Minutes, Hours, Yesterday, Days, Date };
struct Age {
  AgeUnit unit = AgeUnit::None;
  int count = 0;
};
Age age(int64_t then, int64_t now);

}  // namespace gl::recents
