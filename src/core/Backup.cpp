#include "Backup.h"
#include <algorithm>
#include <ctime>
#include <filesystem>
#include "Project.h"

namespace gl {

namespace fs = std::filesystem;

namespace {
std::string stemOf(const std::string& projectPath) {
  if (projectPath.empty()) return "Untitled";
  std::string name = fs::path(projectPath).filename().string();
  for (const char* ext : {".gridline.json", ".json", ".glproj"}) {
    std::string e(ext);
    if (name.size() > e.size() && name.compare(name.size() - e.size(), e.size(), e) == 0)
      return name.substr(0, name.size() - e.size());
  }
  return fs::path(projectPath).stem().string();
}

const char* kBackupTag = " (backup ";
}  // namespace

int64_t BackupPolicy::intervalSeconds() const {
  switch (frequency) {
    case BackupFrequency::Never: return -1;
    case BackupFrequency::Rarely: return 15 * 60;
    case BackupFrequency::Occasionally: return 10 * 60;
    case BackupFrequency::Regularly:
    case BackupFrequency::Frequently: return 5 * 60;
    case BackupFrequency::VeryFrequently: return 60;
  }
  return -1;
}

std::string BackupManager::folderFor(const std::string& projectPath) const {
  if (policy_.inProjectFolder && !projectPath.empty())
    return (fs::path(projectPath).parent_path() / "Backup").string();
  return policy_.globalFolder;
}

bool BackupManager::due(int64_t nowSec) const {
  int64_t iv = policy_.intervalSeconds();
  if (iv < 0) return false;
  return lastBackup_ < 0 || nowSec - lastBackup_ >= iv;
}

std::vector<std::string> BackupManager::list(const std::string& projectPath) const {
  std::vector<std::string> out;
  std::error_code ec;
  fs::path dir = folderFor(projectPath);
  if (dir.empty() || !fs::is_directory(dir, ec)) return out;
  std::string prefix = stemOf(projectPath) + kBackupTag;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    std::string n = e.path().filename().string();
    if (e.is_regular_file() && n.compare(0, prefix.size(), prefix) == 0) out.push_back(e.path().string());
  }
  std::sort(out.begin(), out.end());  // timestamped names sort chronologically
  return out;
}

std::string BackupManager::write(const Song& song, const std::string& projectPath, int64_t nowSec,
                                 std::string* err) {
  fs::path dir = folderFor(projectPath);
  if (dir.empty()) {
    if (err) *err = "no backup folder configured";
    return "";
  }
  std::error_code ec;
  fs::create_directories(dir, ec);
  std::time_t t = static_cast<std::time_t>(nowSec);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char stamp[32];
  std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H-%M-%S", &tm);
  fs::path file = dir / (stemOf(projectPath) + kBackupTag + stamp + ").gridline.json");
  if (!saveProject(file.string(), song, err)) return "";
  lastBackup_ = nowSec;
  auto all = list(projectPath);
  for (size_t i = 0; i + static_cast<size_t>(std::max(policy_.maxCount, 1)) < all.size(); ++i)
    fs::remove(all[i], ec);
  return file.string();
}

}  // namespace gl
