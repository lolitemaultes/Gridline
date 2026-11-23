// Gridline — folders hidden from the browser (GitHub #58). See BrowserHidden.h.
#include "BrowserHidden.h"
#include <algorithm>
#include <filesystem>
#include <sstream>

namespace gl::browse {

namespace {

constexpr const char* kHeader = "gridline-browser-hidden 1";

bool isSep(char c) { return c == '/' || c == '\\'; }

// `path` is `dir` or inside it (on whole path parts: "/a/bc" is not inside "/a/b").
bool within(const std::string& path, const std::string& dir) {
  if (dir.empty() || path.size() < dir.size() || path.compare(0, dir.size(), dir) != 0) return false;
  return path.size() == dir.size() || isSep(path[dir.size()]) || isSep(dir.back());
}

}  // namespace

std::string HiddenFolders::normalise(const std::string& path) {
  if (path.empty()) return path;
  std::string s = std::filesystem::path(path).lexically_normal().generic_string();
  while (s.size() > 1 && s.back() == '/' && !(s.size() == 3 && s[1] == ':')) s.pop_back();  // keep "/" and "C:/"
  return s;
}

bool HiddenFolders::isHidden(const std::string& dir) const {
  const std::string d = normalise(dir);
  return std::binary_search(folders_.begin(), folders_.end(), d);
}

bool HiddenFolders::covers(const std::string& path) const { return !coveringFolder(path).empty(); }

std::string HiddenFolders::coveringFolder(const std::string& path) const {
  if (folders_.empty() || path.empty()) return {};
  const std::string p = normalise(path);
  std::string best;
  for (const std::string& f : folders_)
    if (within(p, f) && f.size() > best.size()) best = f;
  return best;
}

bool HiddenFolders::hide(const std::string& dir) {
  const std::string d = normalise(dir);
  if (d.empty()) return false;
  const auto it = std::lower_bound(folders_.begin(), folders_.end(), d);
  if (it != folders_.end() && *it == d) return false;
  folders_.insert(it, d);
  return true;
}

bool HiddenFolders::show(const std::string& dir) {
  const std::string d = normalise(dir);
  const auto it = std::lower_bound(folders_.begin(), folders_.end(), d);
  if (it == folders_.end() || *it != d) return false;
  folders_.erase(it);
  return true;
}

bool HiddenFolders::showWithin(const std::string& dir) {
  const std::string d = normalise(dir);
  const size_t n = folders_.size();
  folders_.erase(std::remove_if(folders_.begin(), folders_.end(), [&d](const std::string& f) { return within(f, d); }), folders_.end());
  return folders_.size() != n;
}

bool HiddenFolders::moved(const std::string& from, const std::string& to) {
  const std::string f = normalise(from), t = normalise(to);
  if (f.empty() || t.empty() || f == t) return false;
  bool any = false;
  for (std::string& h : folders_)
    if (within(h, f)) h = t + h.substr(f.size()), any = true;
  if (!any) return false;
  std::sort(folders_.begin(), folders_.end());
  folders_.erase(std::unique(folders_.begin(), folders_.end()), folders_.end());
  return true;
}

std::string HiddenFolders::toText() const {
  std::string out = std::string(kHeader) + "\n";
  for (const std::string& f : folders_) out += f + "\n";
  return out;
}

HiddenFolders HiddenFolders::fromText(const std::string& text) {
  HiddenFolders h;
  std::istringstream in(text);
  std::string line;
  bool first = true;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (first) {
      first = false;
      if (line.rfind("gridline-browser-hidden", 0) == 0) continue;
    }
    if (line.empty() || line.find('\t') != std::string::npos) continue;
    h.hide(line);
  }
  return h;
}

std::vector<std::string> withoutHidden(const HiddenFolders& h, const std::vector<std::string>& paths, bool showHidden) {
  if (showHidden || h.empty()) return paths;
  std::vector<std::string> out;
  out.reserve(paths.size());
  for (const std::string& p : paths)
    if (!h.covers(p)) out.push_back(p);
  return out;
}

}  // namespace gl::browse
