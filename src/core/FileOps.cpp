// Gridline: K-18: browser file operations. See FileOps.h.
#include "FileOps.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>
#include <system_error>
#include "Platform.h"
#include "ProjectInterop.h"

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif

namespace fs = std::filesystem;

namespace gl::fileops {

namespace {

std::string upper(std::string s) {
  for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

std::string norm(const std::string& p) {
  std::string g = fs::path(p).lexically_normal().generic_string();
  while (g.size() > 1 && g.back() == '/') g.pop_back();
  return g;
}

bool present(const fs::path& p) {
  std::error_code ec;
  return fs::exists(fs::symlink_status(p, ec));
}

// Renames, or copies and removes when the target is on another drive.
bool moveItem(const fs::path& from, const fs::path& to, std::string* err) {
  std::error_code ec;
  fs::rename(from, to, ec);
  if (!ec) return true;
  if (ec == std::errc::cross_device_link) {
    std::error_code e2;
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::copy_symlinks, e2);
    if (!e2) {
      fs::remove_all(from, e2);
      if (!e2) return true;
    }
    std::error_code e3;
    if (present(to) && present(from)) fs::remove_all(to, e3);  // a half copy goes; the original stays
    if (err) *err = e2.message();
    return false;
  }
  if (err) *err = ec.message();
  return false;
}

// The item's sidecars follow it (a target sidecar that exists already is left alone).
void moveSidecars(const fs::path& from, const fs::path& to, std::vector<Move>& moves) {
  std::error_code ec;
  if (fs::is_directory(to, ec)) return;
  for (const auto& suf : sidecarSuffixes()) {
    const fs::path a = from.string() + suf, b = to.string() + suf;
    if (!present(a) || present(b)) continue;
    if (moveItem(a, b, nullptr)) moves.emplace_back(a.string(), b.string());
  }
}

std::string quoted(const std::string& s) { return "\xE2\x80\x9C" + s + "\xE2\x80\x9D"; }

// "kick.wav" -> "kick 2.wav", folders "Drums" -> "Drums 2".
fs::path uniqueIn(const fs::path& dir, const fs::path& name) {
  fs::path p = dir / name;
  if (!present(p)) return p;
  std::error_code ec;
  const bool folder = fs::is_directory(p, ec);
  const std::string stem = folder ? name.string() : name.stem().string(), ext = folder ? std::string() : name.extension().string();
  for (int k = 2; k < 100000; ++k) {
    p = dir / (stem + " " + std::to_string(k) + ext);
    if (!present(p)) return p;
  }
  return dir / (stem + " " + std::to_string(platform::processId()) + ext);
}

#if !defined(_WIN32)
// freedesktop.org trash: the item into <root>/files, its description into <root>/info.
bool trashFreedesktop(const fs::path& root, const fs::path& item, fs::path& where, std::string* err) {
  std::error_code ec;
  fs::create_directories(root / "files", ec);
  fs::create_directories(root / "info", ec);
  if (!fs::is_directory(root / "files", ec) || !fs::is_directory(root / "info", ec)) {
    if (err) *err = "no trash folder";
    return false;
  }
  where = uniqueIn(root / "files", item.filename());
  while (present(root / "info" / (where.filename().string() + ".trashinfo"))) where = uniqueIn(root / "files", where.filename().string() + " x");
  const fs::path info = root / "info" / (where.filename().string() + ".trashinfo");
  std::string url;
  for (unsigned char c : fs::absolute(item, ec).lexically_normal().string()) {
    if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
      url += static_cast<char>(c);
    } else {
      char b[4];
      std::snprintf(b, sizeof b, "%%%02X", c);
      url += b;
    }
  }
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
  localtime_r(&now, &tm);
  char date[32];
  std::strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", &tm);
  {
    std::ofstream f(info);
    f << "[Trash Info]\nPath=" << url << "\nDeletionDate=" << date << "\n";
    if (!f) {
      if (err) *err = "cannot write to the trash";
      return false;
    }
  }
  if (!moveItem(item, where, err)) {
    fs::remove(info, ec);
    return false;
  }
  return true;
}
#endif

bool trashOne(const fs::path& item, const std::string& trashRoot, std::string& where, std::string* err) {
  if (!trashRoot.empty()) {
#if defined(_WIN32)
    std::error_code ec;
    fs::create_directories(fs::path(trashRoot) / "files", ec);
    const fs::path to = uniqueIn(fs::path(trashRoot) / "files", item.filename());
    if (!moveItem(item, to, err)) return false;
    where = to.string();
    return true;
#else
    fs::path to;
    if (!trashFreedesktop(trashRoot, item, to, err)) return false;
    where = to.string();
    return true;
#endif
  }
#if defined(_WIN32)
  std::wstring w = fs::absolute(item).wstring();
  w.push_back(L'\0');  // double terminated list
  SHFILEOPSTRUCTW op{};
  op.wFunc = FO_DELETE;
  op.pFrom = w.c_str();
  op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
  if (SHFileOperationW(&op) != 0 || op.fAnyOperationsAborted) {
    if (err) *err = "the Recycle Bin refused it";
    return false;
  }
  where.clear();
  return true;
#elif defined(__APPLE__)
  const fs::path root = fs::path(platform::homeDir()) / ".Trash";
  std::error_code ec;
  fs::create_directories(root, ec);
  const fs::path to = uniqueIn(root, item.filename());
  fs::rename(item, to, ec);
  if (ec) {
    if (err) *err = ec == std::errc::cross_device_link ? std::string("it is on another drive (delete it in Finder)") : ec.message();
    return false;
  }
  where = to.string();
  return true;
#else
  const char* xdg = std::getenv("XDG_DATA_HOME");
  const fs::path data = xdg && *xdg ? fs::path(xdg) : fs::path(platform::homeDir()) / ".local" / "share";
  fs::path to;
  if (!trashFreedesktop(data / "Trash", item, to, err)) {
    if (err && err->find("device") != std::string::npos) *err = "it is on another drive (delete it in the file manager)";
    return false;
  }
  where = to.string();
  return true;
#endif
}

}  // namespace

std::string checkName(const std::string& name) {
  if (name.empty() || name.find_first_not_of(' ') == std::string::npos) return "Type a name";
  if (name == "." || name == "..") return "That name is reserved";
  if (name.front() == '.') return "A name starting with a dot would be hidden";
  if (name.size() > 255) return "That name is too long";
  for (unsigned char c : name) {
    if (c < 32) return "Names cannot hold control characters";
    if (std::string("/\\:*?\"<>|").find(static_cast<char>(c)) != std::string::npos)
      return "Names cannot hold these characters: / \\ : * ? \" < > |";
  }
  if (name.back() == '.' || name.back() == ' ') return "A name cannot end with a dot or a space";
  if (name.front() == ' ') return "A name cannot start with a space";
  std::string base = upper(name.substr(0, name.find('.')));
  static const std::set<std::string> kReserved = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8",
                                                  "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
  if (kReserved.count(base)) return "That name is reserved on Windows";
  return "";
}

const std::vector<std::string>& sidecarSuffixes() {
  static const std::vector<std::string> k = {".origin.json", ".source.json"};
  return k;
}

std::vector<std::string> sidecarsOf(const std::string& file) {
  std::vector<std::string> out;
  for (const auto& s : sidecarSuffixes())
    if (present(file + s)) out.push_back(file + s);
  return out;
}

bool isInside(const std::string& p, const std::string& dir) {
  if (p.empty() || dir.empty()) return false;
  const std::string a = norm(p), d = norm(dir);
  if (a == d) return true;
  return a.size() > d.size() && a.compare(0, d.size(), d) == 0 && (a[d.size()] == '/' || d.back() == '/');
}

bool retargetPath(std::string& p, const std::string& from, const std::string& to) {
  if (p.empty() || from.empty() || !fs::path(p).is_absolute()) return false;
  const std::string a = norm(p), f = norm(from);
  if (a == f) {
    if (p == to) return false;
    p = to;
    return true;
  }
  if (a.size() > f.size() && a.compare(0, f.size(), f) == 0 && a[f.size()] == '/') {
    const std::string next = (fs::path(to) / fs::path(a.substr(f.size() + 1))).make_preferred().string();
    if (next == p) return false;
    p = next;
    return true;
  }
  return false;
}

int retargetSong(Song& song, const std::vector<Move>& moves) {
  if (moves.empty()) return 0;
  int n = 0;
  forEachFileRef(song, [&](std::string& p) {
    for (const auto& [from, to] : moves)
      if (retargetPath(p, from, to)) {
        ++n;
        break;
      }
  });
  return n;
}

int referencesTo(const Song& song, const std::string& path) {
  int n = 0;
  for (const auto& p : referencedFiles(song)) n += isInside(p, path) ? 1 : 0;
  return n;
}

Result rename(const std::string& path, const std::string& newName) {
  Result r;
  const fs::path p(path);
  if (!present(p)) {
    r.error = quoted(p.filename().string()) + " is no longer there";
    return r;
  }
  if (std::string e = checkName(newName); !e.empty()) {
    r.error = e;
    return r;
  }
  const fs::path dst = p.parent_path() / newName;
  if (dst == p) {
    r.ok = true;
    r.path = path;
    return r;
  }
  std::error_code ec;
  std::string err;
  if (present(dst)) {
    if (!fs::equivalent(p, dst, ec)) {
      r.error = quoted(newName) + " already exists here";
      return r;
    }
    // Only the letter case changes, on a file system that ignores case: through a free name.
    const fs::path tmp = uniqueIn(p.parent_path(), ".gl-rename-" + std::to_string(platform::processId()));
    if (!moveItem(p, tmp, &err)) {
      r.error = "Could not rename: " + err;
      return r;
    }
    if (!moveItem(tmp, dst, &err)) {
      moveItem(tmp, p, nullptr);
      r.error = "Could not rename: " + err;
      return r;
    }
  } else if (!moveItem(p, dst, &err)) {
    r.error = "Could not rename: " + err;
    return r;
  }
  r.ok = true;
  r.path = dst.string();
  r.moves.emplace_back(path, r.path);
  moveSidecars(p, dst, r.moves);
  return r;
}

Result moveInto(const std::string& path, const std::string& folder) {
  Result r;
  const fs::path p(path), dir(folder);
  std::error_code ec;
  if (!present(p)) {
    r.error = quoted(p.filename().string()) + " is no longer there";
    return r;
  }
  if (!fs::is_directory(dir, ec)) {
    r.error = quoted(dir.filename().string()) + " is not a folder";
    return r;
  }
  if (isInside(folder, path)) {
    r.error = "A folder cannot go into itself";
    return r;
  }
  const fs::path dst = dir / p.filename();
  if (norm(p.parent_path().string()) == norm(folder)) {
    r.ok = true;  // already there
    r.path = path;
    return r;
  }
  if (present(dst)) {
    r.error = quoted(p.filename().string()) + " already exists in " + quoted(dir.filename().string());
    return r;
  }
  std::string err;
  if (!moveItem(p, dst, &err)) {
    r.error = "Could not move: " + err;
    return r;
  }
  r.ok = true;
  r.path = dst.string();
  r.moves.emplace_back(path, r.path);
  moveSidecars(p, dst, r.moves);
  return r;
}

Result makeFolder(const std::string& parent, const std::string& baseName) {
  Result r;
  std::error_code ec;
  if (!fs::is_directory(parent, ec)) {
    r.error = quoted(fs::path(parent).filename().string()) + " is not a folder";
    return r;
  }
  const fs::path p = uniqueIn(parent, baseName);
  if (!fs::create_directory(p, ec) || ec) {
    r.error = "Could not make the folder: " + ec.message();
    return r;
  }
  r.ok = true;
  r.path = p.string();
  return r;
}

Result moveToTrash(const std::string& path, const std::string& trashRoot) {
  Result r;
  const fs::path p(path);
  if (!present(p)) {
    r.error = quoted(p.filename().string()) + " is no longer there";
    return r;
  }
  std::error_code ec;
  const bool file = !fs::is_directory(p, ec);
  std::string where, err;
  if (!trashOne(p, trashRoot, where, &err)) {
    r.error = "Could not move it to the trash: " + err;
    return r;
  }
  r.ok = true;
  r.path = where;
  r.moves.emplace_back(path, where);
  if (file)
    for (const auto& sc : sidecarsOf(path)) {
      std::string w;
      if (trashOne(sc, trashRoot, w, nullptr)) r.moves.emplace_back(sc, w);
    }
  return r;
}

bool moveBack(const std::vector<Move>& moves, std::string* err) {
  bool ok = true;
  for (auto it = moves.rbegin(); it != moves.rend(); ++it) {
    if (!present(it->second) || present(it->first)) {
      ok = false;
      if (err) *err = fs::path(it->first).filename().string() + " cannot go back";
      continue;
    }
    if (!moveItem(it->second, it->first, err)) ok = false;
  }
  return ok;
}

bool moveAgain(const std::vector<Move>& moves, std::string* err) {
  bool ok = true;
  for (const auto& [from, to] : moves) {
    if (!present(from) || present(to)) {
      ok = false;
      if (err) *err = fs::path(from).filename().string() + " cannot move again";
      continue;
    }
    if (!moveItem(from, to, err)) ok = false;
  }
  return ok;
}

FileMoveCommand::FileMoveCommand(std::string label, std::vector<Move> disk, std::vector<Move> refs,
                                 std::function<void(const std::string&, const std::string&)> onMoved)
    : label_(std::move(label)), disk_(std::move(disk)), refs_(std::move(refs)), onMoved_(std::move(onMoved)) {}

void FileMoveCommand::apply(Song& s) {
  if (!first_) {
    moveAgain(disk_);
    if (onMoved_)
      for (const auto& [from, to] : refs_) onMoved_(from, to);
  }
  first_ = false;
  retargetSong(s, refs_);
}

void FileMoveCommand::revert(Song& s) {
  moveBack(disk_);
  std::vector<Move> back;
  for (auto it = refs_.rbegin(); it != refs_.rend(); ++it) back.emplace_back(it->second, it->first);
  retargetSong(s, back);
  if (onMoved_)
    for (const auto& [from, to] : back) onMoved_(from, to);
}

}  // namespace gl::fileops
