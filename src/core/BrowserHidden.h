// Gridline — folders hidden from the browser (GitHub #58). Pure logic (no UI): the folders the
// user hid, saved per user in <config>/browser-hidden.txt (never in a project), and whether a path
// is one of them or inside one. The browser (app/Browser.cpp) leaves hidden folders and their
// contents out of every tab, place and search unless Show hidden is on, when they show dimmed.
#pragma once
#include <string>
#include <vector>

namespace gl::browse {

class HiddenFolders {
 public:
  // The folders, normalised (no trailing separator), sorted, each once.
  const std::vector<std::string>& folders() const { return folders_; }
  bool empty() const { return folders_.empty(); }
  // `dir` itself was hidden.
  bool isHidden(const std::string& dir) const;
  // `path` is a hidden folder or lies anywhere inside one.
  bool covers(const std::string& path) const;
  // The hidden folder that covers `path` nearest to it ("" when none).
  std::string coveringFolder(const std::string& path) const;
  // Returns true when the list changed. Showing a folder never shows a hidden folder around it.
  bool hide(const std::string& dir);
  bool show(const std::string& dir);
  // Removes every hidden folder at or inside `dir` (a folder removed from the browser, say).
  bool showWithin(const std::string& dir);
  // A folder renamed or moved: hidden folders at or inside `from` move with it.
  bool moved(const std::string& from, const std::string& to);

  // The saved form: "gridline-browser-hidden 1", then one folder per line. Unknown or empty lines
  // are skipped.
  std::string toText() const;
  static HiddenFolders fromText(const std::string& text);

  static std::string normalise(const std::string& path);

 private:
  std::vector<std::string> folders_;
};

// Keeps the paths no hidden folder covers (search results, starred files), unless `showHidden`.
std::vector<std::string> withoutHidden(const HiddenFolders& h, const std::vector<std::string>& paths, bool showHidden);

}  // namespace gl::browse
