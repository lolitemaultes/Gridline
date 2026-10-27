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
  size_t others = 0;
  for (const auto& p : paths) others += favourites.count(p) ? 0 : 1;
  for (size_t i = paths.size(); others > cap && i-- > 0;) {
    if (favourites.count(paths[i])) continue;
    opened.erase(paths[i]);
    paths.erase(paths.begin() + static_cast<std::ptrdiff_t>(i));
    --others;
  }
}

bool List::remove(const std::string& path) {
  const auto it = std::find(paths.begin(), paths.end(), path);
  if (it == paths.end()) return false;
  paths.erase(it);
  opened.erase(path);
  favourites.erase(path);
  return true;
}

bool List::setFavourite(const std::string& path, bool on) {
  if (!on) return favourites.erase(path) != 0;
  if (std::find(paths.begin(), paths.end(), path) == paths.end()) return false;
  return favourites.insert(path).second;
}

size_t List::clearKeepingFavourites() {
  const size_t before = paths.size();
  for (size_t i = paths.size(); i-- > 0;)
    if (!favourites.count(paths[i])) {
      opened.erase(paths[i]);
      paths.erase(paths.begin() + static_cast<std::ptrdiff_t>(i));
    }
  return before - paths.size();
}

int64_t List::openedAt(const std::string& path) const {
  const auto it = opened.find(path);
  return it == opened.end() ? 0 : it->second;
}

List load(const std::string& dir) {
  List l;
  std::set<std::string> seen, fav;
  std::ifstream ff(fs::u8path(dir) / "favourite-projects.txt");
  for (std::string line; std::getline(ff, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty()) fav.insert(line);
  }
  std::ifstream rf(fs::u8path(dir) / "recent.txt");
  size_t others = 0;
  for (std::string line; std::getline(rf, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || seen.count(line)) continue;
    const bool f = fav.count(line) != 0;
    if (!f && others >= kMaxRecent) continue;
    seen.insert(line);
    l.paths.push_back(line);
    if (f) l.favourites.insert(line);
    else ++others;
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
  std::ofstream ff(fs::u8path(dir) / "favourite-projects.txt");
  for (const auto& r : l.paths)
    if (l.isFavourite(r)) ff << r << "\n";
  return static_cast<bool>(wf) && static_cast<bool>(tf) && static_cast<bool>(ff);
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
