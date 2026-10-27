#include "Recents.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace gl::recents {

namespace fs = std::filesystem;

void List::touch(const std::string& path, int64_t now, size_t cap) {
  if (path.empty()) return;
  paths.erase(std::remove(paths.begin(), paths.end(), path), paths.end());
  paths.insert(paths.begin(), path);
  opened[path] = now;
  while (paths.size() > cap) {
    opened.erase(paths.back());
    paths.pop_back();
  }
}

bool List::remove(const std::string& path) {
  const auto it = std::find(paths.begin(), paths.end(), path);
  if (it == paths.end()) return false;
  paths.erase(it);
  opened.erase(path);
  return true;
}

int64_t List::openedAt(const std::string& path) const {
  const auto it = opened.find(path);
  return it == opened.end() ? 0 : it->second;
}

List load(const std::string& dir) {
  List l;
  std::set<std::string> seen;
  std::ifstream rf(fs::u8path(dir) / "recent.txt");
  for (std::string line; std::getline(rf, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || !seen.insert(line).second) continue;
    l.paths.push_back(line);
    if (l.paths.size() >= kMaxRecent) break;
  }
  std::ifstream tf(fs::u8path(dir) / "recent-times.tsv");
  for (std::string line; std::getline(tf, line);) {
    const size_t tab = line.find('\t');
    if (tab == std::string::npos) continue;
    const std::string path = line.substr(tab + 1);
    if (!seen.count(path)) continue;
    try {
      l.opened[path] = std::stoll(line.substr(0, tab));
    } catch (...) {
    }
  }
  return l;
}

bool save(const std::string& dir, const List& l) {
  std::error_code ec;
  fs::create_directories(fs::u8path(dir), ec);
  std::ofstream wf(fs::u8path(dir) / "recent.txt");
  for (const auto& r : l.paths) wf << r << "\n";
  std::ofstream tf(fs::u8path(dir) / "recent-times.tsv");
  for (const auto& r : l.paths)
    if (const int64_t t = l.openedAt(r); t > 0) tf << t << "\t" << r << "\n";
  return static_cast<bool>(wf) && static_cast<bool>(tf);
}

bool exists(const std::string& path) {
  std::error_code ec;
  return !path.empty() && fs::exists(fs::u8path(path), ec);
}

Age age(int64_t then, int64_t now) {
  if (then <= 0) return {};
  const int64_t d = std::max<int64_t>(0, now - then);
  if (d < 60) return {AgeUnit::JustNow, 0};
  if (d < 3600) return {AgeUnit::Minutes, static_cast<int>(d / 60)};
  if (d < 86400) return {AgeUnit::Hours, static_cast<int>(d / 3600)};
  if (d < 2 * 86400) return {AgeUnit::Yesterday, 1};
  if (d < 7 * 86400) return {AgeUnit::Days, static_cast<int>(d / 86400)};
  return {AgeUnit::Date, 0};
}

}  // namespace gl::recents
