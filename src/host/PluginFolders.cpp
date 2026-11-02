#include "PluginFolders.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <set>

namespace fs = std::filesystem;

namespace gl::host {

namespace {
std::string normal(const std::string& p) {
  std::string s = fs::path(p).lexically_normal().generic_string();
  while (s.size() > 1 && s.back() == '/') s.pop_back();
#if defined(_WIN32)
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
#endif
  return s;
}
bool isDir(const std::string& p) {
  std::error_code ec;
  return !p.empty() && fs::is_directory(p, ec);
}
// The folder's real location when it exists (links resolved), else its normal spelling.
std::string real(const std::string& p) {
  std::error_code ec;
  fs::path c = fs::canonical(p, ec);
  return ec ? normal(p) : normal(c.string());
}
}  // namespace

const char* folderEnvVar(const std::string& formatId) {
  if (formatId == "vst2") return "VST_PATH";
  if (formatId == "vst3") return "VST3_PATH";
  if (formatId == "clap") return "CLAP_PATH";
  if (formatId == "lv2") return "LV2_PATH";
  return "";
}

bool folderSearchApplies(const std::string& formatId) { return formatId != "au"; }

std::string folderKey(const std::string& formatIdOrStar, const std::string& path) { return formatIdOrStar + "|" + path; }

bool sameFolder(const std::string& a, const std::string& b) {
  if (normal(a) == normal(b)) return true;
  std::error_code ec;
  return isDir(a) && isDir(b) && fs::equivalent(a, b, ec) && !ec;
}

bool folderInside(const std::string& inner, const std::string& outer) {
  const std::string i = real(inner), o = real(outer);
  if (i.size() <= o.size() || i.compare(0, o.size(), o) != 0) return false;
  return o.back() == '/' || i[o.size()] == '/';
}

std::vector<FolderRow> folderRows(const Format& f, const PluginPrefs& p) {
  std::vector<FolderRow> rows;
  const std::string id = f.id;
  std::vector<std::string> env;
  if (const char* var = folderEnvVar(id); *var) addEnvPaths(env, var);
  const bool defaultsOn = !p.noDefaultFolders.count(id) || !folderSearchApplies(id);
  for (const auto& d : f.defaultPaths ? f.defaultPaths() : std::vector<std::string>{}) {
    // Some formats' default lists already hold their environment variable's folders: listed under env.
    if (std::find(env.begin(), env.end(), d) != env.end()) continue;
    rows.push_back(FolderRow{id, d, FolderSource::Default, defaultsOn, isDir(d), false, {}});
  }
  if (folderSearchApplies(id)) {
    for (const auto& e : env) rows.push_back(FolderRow{id, e, FolderSource::Env, true, isDir(e), false, {}});
    if (auto it = p.formatFolders.find(id); it != p.formatFolders.end())
      for (const auto& u : it->second) rows.push_back(FolderRow{id, u, FolderSource::User, !p.disabledFolders.count(folderKey(id, u)), isDir(u), false, {}});
    for (const auto& u : p.extraFolders) rows.push_back(FolderRow{id, u, FolderSource::Every, !p.disabledFolders.count(folderKey("*", u)), isDir(u), false, {}});
  }
  for (size_t i = 0; i < rows.size(); ++i) {
    FolderRow& r = rows[i];
    if (!r.enabled || !r.exists) continue;
    for (size_t j = 0; j < i && !r.duplicate; ++j)
      if (rows[j].searched() && sameFolder(r.path, rows[j].path)) r.duplicate = true;
  }
  for (auto& r : rows) {
    if (!r.searched()) continue;
    for (const auto& o : rows)
      if (&o != &r && o.searched() && folderInside(r.path, o.path)) {
        r.inside = o.path;
        break;
      }
  }
  return rows;
}

std::vector<std::string> searchFolders(const Format& f, const PluginPrefs& p) {
  std::vector<std::string> out;
  for (const auto& r : folderRows(f, p))
    if (r.searched()) out.push_back(r.path);
  return out;
}

std::vector<std::string> searchFolders(const std::string& formatId) {
  const Format* f = findFormat(formatId);
  return f ? searchFolders(*f, pluginPrefs()) : std::vector<std::string>{};
}

std::vector<std::string> discoverFormat(const Format& f, const PluginPrefs& p) {
  std::vector<std::string> found = f.discover(searchFolders(f, p));
  if (!folderSearchApplies(f.id)) return found;  // AU: registry keys, not paths
  std::vector<std::string> out;
  std::set<std::string> seen;
  for (auto& e : found)
    if (seen.insert(real(e)).second) out.push_back(std::move(e));
  return out;
}

}  // namespace gl::host
