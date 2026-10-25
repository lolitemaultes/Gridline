// Gridline - J-111e collaboration: project files at the end of collaboration. See Lifecycle.h.
#include "Lifecycle.h"
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include "../core/I18n.h"
#include "../core/Platform.h"
#include "../core/Project.h"
#include "../core/ProjectInterop.h"

namespace fs = std::filesystem;

namespace gl::collab {

bool exportPlainProject(const Song& song, const std::string& srcFolder, const std::string& destFolder, const std::string& name,
                        bool removeHistory, std::string* outFile, std::string* err) {
  std::error_code ec;
  const fs::path src = fs::path(srcFolder).lexically_normal(), dst = fs::path(destFolder).lexically_normal();
  const bool inPlace = fs::exists(dst, ec) && fs::exists(src, ec) && fs::equivalent(src, dst, ec);
  fs::create_directories(dst, ec);
  if (ec) {
    if (err) *err = std::string(i18n::tr("Cannot create the folder")) + ": " + dst.string();
    return false;
  }
  Song copy = song;
  copy.idSlot = 0;
  copy.idStride = 1;
  const std::string srcAudio = (src / "Audio").generic_string() + "/";
  std::map<std::string, std::string> done;
  bool ok = true;
  forEachFileRef(copy, [&](std::string& p) {
    if (p.empty()) return;
    const std::string g = fs::path(p).lexically_normal().generic_string();
    if (g.rfind(srcAudio, 0) != 0) return;  // a file outside the project folder stays where it is
    const std::string file = g.substr(srcAudio.size());
    auto it = done.find(file);
    if (it == done.end()) {
      if (!inPlace) {
        std::error_code e2;
        fs::create_directories(dst / "Audio", e2);
        const fs::path to = dst / "Audio" / file;
        if (!fs::exists(to, e2)) fs::copy_file(fs::path(srcAudio) / file, to, fs::copy_options::overwrite_existing, e2);
        if (e2 && fs::exists(fs::path(srcAudio) / file)) ok = false;  // (a file that never arrived stays a missing reference)
      }
      it = done.emplace(file, "Audio/" + file).first;
    }
    p = it->second;
  });
  if (!ok) {
    if (err) *err = i18n::tr("Some audio files could not be copied.");
    return false;
  }
  std::string base = name.empty() ? std::string("Project") : name;
  for (char& c : base)
    if (c == '/' || c == '\\' || c == ':' || static_cast<unsigned char>(c) < 0x20) c = '-';
  const std::string file = (dst / (base + ".gridline.json")).string();
  if (!writeFileAtomic(file, projectToText(copy), err)) return false;
  if (removeHistory) {
    // the shared history, other members' records and names: gone with the collaboration
    fs::remove_all(dst / ".gridline-collab", ec);
    if (ec) {
      if (err) *err = std::string(i18n::tr("Could not remove the collaboration history")) + ": " + ec.message();
      return false;
    }
  }
  if (outFile) *outFile = file;
  return true;
}

namespace {

fs::path uniqueIn(const fs::path& dir, const std::string& name) {
  std::error_code ec;
  fs::path p = dir / name;
  for (int i = 2; fs::exists(p, ec) && i < 1000; ++i) p = dir / (name + " " + std::to_string(i));
  return p;
}

#if !defined(_WIN32) && !defined(__APPLE__)
std::string isoNow() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char b[32];
  std::strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%S", &tm);
  return b;
}
#endif

}  // namespace

bool moveToTrash(const std::string& path, std::string* err) {
  std::error_code ec;
  const fs::path p = fs::absolute(fs::path(path), ec).lexically_normal();
  if (!fs::exists(p, ec)) {
    if (err) *err = i18n::tr("The folder does not exist.");
    return false;
  }
  if (const char* t = std::getenv("GRIDLINE_TEST_TRASH_DIR"); t && *t) {
    const char* root = std::getenv("GRIDLINE_TEST_TRASH_ROOT");
    const std::string r = root ? fs::path(root).lexically_normal().generic_string() : std::string();
    if (r.empty() || p.generic_string().rfind(r, 0) != 0) {
      if (err) *err = "test trash: path outside GRIDLINE_TEST_TRASH_ROOT";
      return false;
    }
    fs::create_directories(t, ec);
    fs::rename(p, uniqueIn(t, p.filename().string()), ec);
    if (ec && err) *err = ec.message();
    return !ec;
  }
#if defined(_WIN32)
  if (err) *err = i18n::tr("Moving to the Recycle Bin is not available here. Delete the folder yourself if you want it gone.");
  return false;
#else
  const std::string home = platform::homeDir();
#if defined(__APPLE__)
  const fs::path trash = fs::path(home) / ".Trash";
  fs::create_directories(trash, ec);
  fs::rename(p, uniqueIn(trash, p.filename().string()), ec);
  if (ec) {
    if (err) *err = std::string(i18n::tr("Could not move the folder to the Trash")) + ": " + ec.message();
    return false;
  }
  return true;
#else
  // freedesktop.org trash: $XDG_DATA_HOME/Trash/{files,info}
  const char* xdg = std::getenv("XDG_DATA_HOME");
  const fs::path base = (xdg && *xdg ? fs::path(xdg) : fs::path(home) / ".local" / "share") / "Trash";
  fs::create_directories(base / "files", ec);
  fs::create_directories(base / "info", ec);
  const fs::path to = uniqueIn(base / "files", p.filename().string());
  {
    std::ofstream info(base / "info" / (to.filename().string() + ".trashinfo"));
    info << "[Trash Info]\nPath=" << p.string() << "\nDeletionDate=" << isoNow() << "\n";
  }
  fs::rename(p, to, ec);  // only within one disk: never a copy-then-delete
  if (ec) {
    fs::remove(base / "info" / (to.filename().string() + ".trashinfo"), ec);
    if (err) *err = i18n::tr("Could not move the folder to the trash (it is on another disk). Delete it yourself if you want it gone.");
    return false;
  }
  return true;
#endif
#endif
}

uint64_t folderSize(const std::string& path) {
  uint64_t n = 0;
  std::error_code ec;
  if (!fs::is_directory(path, ec)) return 0;
  for (auto it = fs::recursive_directory_iterator(path, fs::directory_options::skip_permission_denied, ec); it != fs::recursive_directory_iterator();
       it.increment(ec)) {
    if (ec) break;
    if (it->is_regular_file(ec)) n += static_cast<uint64_t>(it->file_size(ec));
  }
  return n;
}

std::string formatBytes(uint64_t b) {
  char s[32];
  if (b < 1024) std::snprintf(s, sizeof s, "%llu B", static_cast<unsigned long long>(b));
  else if (b < 1024ull * 1024) std::snprintf(s, sizeof s, "%.0f KB", static_cast<double>(b) / 1024.0);
  else if (b < 1024ull * 1024 * 1024) std::snprintf(s, sizeof s, "%.1f MB", static_cast<double>(b) / (1024.0 * 1024.0));
  else std::snprintf(s, sizeof s, "%.2f GB", static_cast<double>(b) / (1024.0 * 1024.0 * 1024.0));
  return s;
}

}  // namespace gl::collab
