// Gridline - K-265: the data folder setting (see DataFolder.h).
#include "DataFolder.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace gl::datafolder {

std::string pointerFile(const std::string& defaultConfigRoot) {
  return (fs::u8path(defaultConfigRoot) / "gridline" / "data-folder.txt").u8string();
}

namespace {
bool isDir(const fs::path& p) {
  std::error_code ec;
  return fs::is_directory(p, ec);
}
std::string trim(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
  return s.substr(i);
}
}  // namespace

Info resolve(const std::string& defaultConfigRoot, const std::string& exePath, bool envOverride) {
  Info in;
  if (envOverride) {
    in.envOverride = true;
    return in;
  }
  if (!exePath.empty()) {  // portable: beside the program, or beside the macOS app bundle
    fs::path dir = fs::u8path(exePath).parent_path();
    std::vector<fs::path> look = {dir / kPortableName};
    for (fs::path p = dir; !p.empty() && p != p.parent_path(); p = p.parent_path())
      if (p.extension() == ".app") {
        look.push_back(p.parent_path() / kPortableName);
        break;
      }
    for (const fs::path& p : look)
      if (isDir(p)) {
        in.folder = in.wanted = p.u8string();
        in.portable = true;
        return in;
      }
  }
  std::ifstream f(fs::u8path(pointerFile(defaultConfigRoot)));
  if (!f) return in;
  std::string line;
  std::getline(f, line);
  line = trim(line);
  if (line.empty()) return in;
  in.wanted = line;
  if (fs::u8path(line).is_absolute() && isDir(fs::u8path(line))) in.folder = line;
  else in.missing = true;
  return in;
}

bool writePointer(const std::string& defaultConfigRoot, const std::string& folder, std::string* err) {
  const fs::path p = fs::u8path(pointerFile(defaultConfigRoot));
  std::error_code ec;
  if (folder.empty()) {
    fs::remove(p, ec);
    if (ec && err) *err = ec.message();
    return !ec;
  }
  if (!fs::u8path(folder).is_absolute()) {
    if (err) *err = "the data folder must be a full path";
    return false;
  }
  fs::create_directories(p.parent_path(), ec);
  std::ofstream o(p, std::ios::binary | std::ios::trunc);
  o << folder << "\n";
  o.close();
  if (!o) {
    if (err) *err = "cannot write " + p.u8string();
    return false;
  }
  return true;
}

bool copyInto(const std::string& configRoot, const std::string& cacheRoot, const std::string& dataRoot, const std::string& folder,
              const std::function<bool(double, const std::string&)>& progress, std::string* err) {
  struct Job {
    fs::path from, to;
  };
  std::vector<std::pair<fs::path, fs::path>> files;  // source file, target file
  const Job roots[] = {{fs::u8path(configRoot) / "gridline", fs::u8path(folder) / "config" / "gridline"},
                       {fs::u8path(cacheRoot) / "gridline", fs::u8path(folder) / "cache" / "gridline"},
                       {fs::u8path(dataRoot) / "gridline", fs::u8path(folder) / "data" / "gridline"}};
  std::error_code ec;
  const fs::path target = fs::weakly_canonical(fs::u8path(folder), ec);
  for (const Job& j : roots) {
    if (!isDir(j.from)) continue;
    // never copy a folder into itself (a data folder inside the default folders)
    const fs::path src = fs::weakly_canonical(j.from, ec);
    const std::string s = src.u8string(), t = target.u8string();
    if (!t.empty() && t.compare(0, s.size(), s) == 0 && (t.size() == s.size() || t[s.size()] == '/' || t[s.size()] == '\\')) {
      if (err) *err = "the new folder is inside the folder it would copy";
      return false;
    }
    for (fs::recursive_directory_iterator it(j.from, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
      if (!it->is_regular_file(ec)) continue;
      if (it->path().filename() == "data-folder.txt") continue;
      files.push_back({it->path(), j.to / fs::relative(it->path(), j.from, ec)});
    }
  }
  for (const Job& j : roots) fs::create_directories(j.to, ec);
  for (size_t i = 0; i < files.size(); ++i) {
    if (progress && !progress(static_cast<double>(i) / static_cast<double>(std::max<size_t>(1, files.size())), files[i].first.filename().u8string())) {
      if (err) *err = "cancelled";
      return false;
    }
    fs::create_directories(files[i].second.parent_path(), ec);
    fs::copy_file(files[i].first, files[i].second, fs::copy_options::overwrite_existing, ec);
    if (ec) {
      if (err) *err = "cannot copy " + files[i].first.u8string() + ": " + ec.message();
      return false;
    }
  }
  if (progress) progress(1.0, "");
  return true;
}

}  // namespace gl::datafolder
