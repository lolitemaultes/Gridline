// Gridline - J-141: the recent projects list (File > Open recent, the browser's Recent
// files, the welcome launcher). `recent.txt` in the config folder holds the paths, most
// recent first; `recent-times.tsv` beside it holds when each was last opened or saved
// (Unix seconds), so the launcher can say "2 h ago". J-148: `favourite-projects.txt` holds
// the projects marked as favourites (a heart on their tile); a favourite is never pushed
// off the list by newer projects. Pure file handling, no UI.
#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace gl::recents {

inline constexpr size_t kMaxRecent = 24;

struct List {
  std::vector<std::string> paths;          // most recent first, no duplicates
  std::map<std::string, int64_t> opened;   // path -> last opened / saved (Unix seconds)
  std::set<std::string> favourites;        // J-148: listed paths marked as favourites

  // Moves `path` to the front (adding it), stamps it with `now` and keeps at most `cap`
  // paths that are not favourites (the oldest go first; favourites always stay).
  void touch(const std::string& path, int64_t now, size_t cap = kMaxRecent);
  // Drops `path` (and its favourite mark); false when it was not listed.
  bool remove(const std::string& path);
  // J-148: favourite marks. Only a listed path can be a favourite; false when nothing changed.
  bool setFavourite(const std::string& path, bool on);
  bool isFavourite(const std::string& path) const { return favourites.count(path) != 0; }
  // Drops every path that is not a favourite (the list's "Clear"); returns how many went.
  size_t clearKeepingFavourites();
  // Drops every path for which `exists` is false; returns how many went.
  template <class Exists>
  size_t pruneMissing(Exists exists) {
    size_t n = 0;
    for (size_t i = 0; i < paths.size();)
      if (!exists(paths[i])) {
        opened.erase(paths[i]);
        favourites.erase(paths[i]);
        paths.erase(paths.begin() + static_cast<std::ptrdiff_t>(i));
        ++n;
      } else {
        ++i;
      }
    return n;
  }
  void clear() { paths.clear(), opened.clear(), favourites.clear(); }
  // Last opened time, else 0.
  int64_t openedAt(const std::string& path) const;
};

// Reads / writes the files in `dir` (missing files: an empty list). load() drops blank
// lines and duplicates and keeps at most kMaxRecent paths besides the favourites.
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
