// Gridline: the Open / Save dialog's model. See FileDialogModel.h.
#include "FileDialogModel.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include "I18n.h"
#include "Platform.h"
#include "RemoteFs.h"
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <sys/statvfs.h>
#include <unistd.h>
#endif

namespace gl::filedlg {

namespace fs = std::filesystem;

namespace {

std::string lowerCopy(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool startsWith(const std::string& s, const char* p) { return s.compare(0, std::strlen(p), p) == 0; }

// "/media" and "/media/x", not "/mediafoo".
bool under(const std::string& path, const std::string& dir) {
  if (path.size() < dir.size() || path.compare(0, dir.size(), dir) != 0) return false;
  return path.size() == dir.size() || path[dir.size()] == '/';
}

// Mount table names escape spaces and such as \040.
std::string unescapeMount(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 3 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])) &&
        std::isdigit(static_cast<unsigned char>(s[i + 2])) && std::isdigit(static_cast<unsigned char>(s[i + 3]))) {
      out += static_cast<char>(std::strtol(s.substr(i + 1, 3).c_str(), nullptr, 8));
      i += 3;
    } else {
      out += s[i];
    }
  }
  return out;
}

std::string baseName(const std::string& p) {
  const size_t s = p.find_last_of('/');
  return s == std::string::npos ? p : p.substr(s + 1);
}

}  // namespace

// ---- drives and volumes ----

bool keepMount(const std::string& device, const std::string& point, const std::string& typeIn, VolumeKind* kind) {
  const std::string type = lowerCopy(typeIn);
  // File systems that hold the user's files. Everything else (proc, sysfs, tmpfs, devtmpfs, cgroup,
  // overlay, squashfs (snaps), autofs, fuse portals, binfmt, ...) is the system's own.
  static const std::set<std::string> local = {"ext2", "ext3", "ext4", "xfs", "btrfs", "zfs", "f2fs", "jfs", "reiserfs", "bcachefs",
                                              "vfat", "msdos", "fat", "exfat", "ntfs", "ntfs3", "fuseblk", "fuse.ntfs-3g", "hfs", "hfsplus",
                                              "apfs", "ufs", "ffs", "msdosfs", "ext2fs", "fusefs.ntfs", "fuse.exfat"};
  static const std::set<std::string> optical = {"iso9660", "udf", "cd9660"};
  static const std::set<std::string> network = {"nfs", "nfs4", "cifs", "smb3", "smbfs", "fuse.sshfs", "fuse.rclone", "davfs", "fuse.davfs2",
                                                "9p", "afs", "ceph", "glusterfs", "fuse.gvfsd-fuse", "fuse.s3fs", "sshfs"};
  VolumeKind k;
  if (local.count(type)) k = VolumeKind::Internal;
  else if (optical.count(type)) k = VolumeKind::Optical;
  else if (network.count(type) || startsWith(type, "nfs") || remotefs::remoteFsType(type)) k = VolumeKind::Network;
  else return false;
  if (point.empty() || point[0] != '/') return false;
  if (k == VolumeKind::Internal && startsWith(device, "/dev/loop")) return false;  // disk images (snaps and such)
  // Media mounted for the user, and /mnt: shown whatever their file system.
  const bool media = under(point, "/media") || under(point, "/run/media") || under(point, "/mnt");
  if (!media && point != "/") {
    static const char* const system[] = {"/boot", "/efi", "/proc", "/sys", "/dev", "/run", "/snap", "/var", "/usr", "/opt",
                                         "/tmp", "/srv", "/etc", "/nix", "/gnu", "/recovery", "/lib", "/bin", "/sbin", "/root"};
    for (const char* s : system)
      if (under(point, s)) return false;
    // Inside a home folder: only hidden helper mounts would be there (flatpak documents and such).
    if (point.find("/.") != std::string::npos) return false;
  }
  if (point == "/") k = VolumeKind::System;
  else if (k == VolumeKind::Internal && (under(point, "/media") || under(point, "/run/media"))) k = VolumeKind::Removable;
  if (startsWith(device, "/dev/sr") || startsWith(device, "/dev/cd")) k = VolumeKind::Optical;
  if (kind) *kind = k;
  return true;
}

std::vector<Volume> parseLinuxMounts(const std::string& text, const std::string& user) {
  std::vector<Volume> v;
  std::set<std::string> seen;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream ls(line);
    std::string dev, point, type;
    if (!(ls >> dev >> point >> type)) continue;
    dev = unescapeMount(dev);
    point = unescapeMount(point);
    VolumeKind k;
    if (!keepMount(dev, point, type, &k)) continue;
    if (!seen.insert(point).second) continue;  // mounted twice (bind mounts): once
    Volume vol;
    vol.path = point;
    vol.kind = k;
    vol.name = point == "/" ? i18n::N_("File system") : baseName(point);
    if (!user.empty() && (point == "/media/" + user || point == "/run/media/" + user)) continue;  // the folder that holds the media
    v.push_back(std::move(vol));
  }
  // The root first, then by name.
  std::stable_sort(v.begin(), v.end(), [](const Volume& a, const Volume& b) {
    if ((a.path == "/") != (b.path == "/")) return a.path == "/";
    return naturalLess(a.name, b.name);
  });
  return v;
}

#if defined(_WIN32)
namespace {
std::string narrowW(const wchar_t* w) {
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return {};
  std::string s(static_cast<size_t>(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}
}  // namespace
#endif

std::vector<Volume> detectVolumes() {
  std::vector<Volume> v;
#if defined(_WIN32)
  // No "insert a disk" boxes for an empty card reader or optical drive.
  const UINT oldMode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
  const DWORD mask = GetLogicalDrives();
  for (int i = 0; i < 26; ++i) {
    if (!(mask & (1u << i))) continue;
    wchar_t root[4] = {static_cast<wchar_t>(L'A' + i), L':', L'\\', 0};
    const UINT type = GetDriveTypeW(root);
    if (type == DRIVE_NO_ROOT_DIR || type == DRIVE_UNKNOWN) continue;
    Volume vol;
    vol.path = narrowW(root);
    vol.kind = type == DRIVE_REMOVABLE ? VolumeKind::Removable
               : type == DRIVE_REMOTE  ? VolumeKind::Network
               : type == DRIVE_CDROM   ? VolumeKind::Optical
                                       : VolumeKind::Internal;
    std::string label;
    if (type != DRIVE_REMOTE) {  // a network drive can take its time to answer: letter only
      wchar_t name[MAX_PATH + 1] = {0};
      if (GetVolumeInformationW(root, name, MAX_PATH + 1, nullptr, nullptr, nullptr, nullptr, 0)) label = narrowW(name);
      ULARGE_INTEGER freeB, totalB;
      if (GetDiskFreeSpaceExW(root, &freeB, &totalB, nullptr)) {
        vol.freeBytes = static_cast<int64_t>(freeB.QuadPart);
        vol.totalBytes = static_cast<int64_t>(totalB.QuadPart);
      }
    }
    wchar_t sys[MAX_PATH + 1] = {0};
    if (GetWindowsDirectoryW(sys, MAX_PATH) && towupper(sys[0]) == root[0]) vol.kind = VolumeKind::System;
    const std::string letter = vol.path.substr(0, 2);
    vol.name = label.empty() ? letter : label + " (" + letter + ")";
    v.push_back(std::move(vol));
  }
  SetErrorMode(oldMode);
#elif defined(__APPLE__)
  // The boot volume (named as in /Volumes, where a link points at "/") and every browsable volume
  // under /Volumes, from the kernel's table without waiting on any of them (MNT_NOWAIT).
  std::string rootName = "Macintosh HD";
  {
    std::error_code ec;
    for (fs::directory_iterator it("/Volumes", ec), end; !ec && it != end; it.increment(ec)) {
      std::error_code e2;
      if (it->is_symlink(e2) && fs::read_symlink(it->path(), e2) == "/") rootName = it->path().filename().string();
    }
  }
  const int n = getfsstat(nullptr, 0, MNT_NOWAIT);
  if (n > 0) {
    std::vector<struct statfs> table(static_cast<size_t>(n) + 8);
    const int got = getfsstat(table.data(), static_cast<int>(table.size() * sizeof(struct statfs)), MNT_NOWAIT);
    for (int i = 0; i < got; ++i) {
      const struct statfs& m = table[static_cast<size_t>(i)];
      const std::string point = m.f_mntonname, type = lowerCopy(m.f_fstypename);
      const bool root = point == "/";
      if (!root && (!startsWith(point, "/Volumes/") || (m.f_flags & MNT_DONTBROWSE))) continue;
      Volume vol;
      vol.path = point;
      vol.name = root ? rootName : point.substr(9);
      if (root) vol.kind = VolumeKind::System;
      else if (!(m.f_flags & MNT_LOCAL) || remotefs::remoteFsType(type) || type == "smbfs" || type == "afpfs" || type == "webdav")
        vol.kind = VolumeKind::Network;
      else if (type == "cd9660" || type == "udf" || type == "cddafs") vol.kind = VolumeKind::Optical;
      else vol.kind = VolumeKind::Removable;  // anything else under /Volumes is an attached disk
      if (vol.kind != VolumeKind::Network) {
        vol.freeBytes = static_cast<int64_t>(m.f_bavail) * static_cast<int64_t>(m.f_bsize);
        vol.totalBytes = static_cast<int64_t>(m.f_blocks) * static_cast<int64_t>(m.f_bsize);
      }
      v.push_back(std::move(vol));
    }
  }
  std::stable_sort(v.begin(), v.end(), [](const Volume& a, const Volume& b) {
    if ((a.kind == VolumeKind::System) != (b.kind == VolumeKind::System)) return a.kind == VolumeKind::System;
    return naturalLess(a.name, b.name);
  });
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
  struct statfs* table = nullptr;
  const int got = getmntinfo(&table, MNT_NOWAIT);
  std::set<std::string> seen;
  for (int i = 0; i < got; ++i) {
    const struct statfs& m = table[i];
    VolumeKind k;
    if (!keepMount(m.f_mntfromname, m.f_mntonname, m.f_fstypename, &k) || !seen.insert(m.f_mntonname).second) continue;
    Volume vol;
    vol.path = m.f_mntonname;
    vol.kind = k;
    vol.name = vol.path == "/" ? i18n::N_("File system") : baseName(vol.path);
    if (k != VolumeKind::Network) {
      vol.freeBytes = static_cast<int64_t>(m.f_bavail) * static_cast<int64_t>(m.f_bsize);
      vol.totalBytes = static_cast<int64_t>(m.f_blocks) * static_cast<int64_t>(m.f_bsize);
    }
    v.push_back(std::move(vol));
  }
#else
  std::string text;
  {
    std::ifstream in("/proc/self/mounts");
    std::stringstream ss;
    ss << in.rdbuf();
    text = ss.str();
  }
  const char* u = std::getenv("USER");
  v = parseLinuxMounts(text, u ? u : "");
  for (auto& vol : v) {
    if (vol.kind == VolumeKind::Network) continue;  // statvfs can hang on a stuck network mount
    struct statvfs s;
    if (statvfs(vol.path.c_str(), &s) == 0) {
      vol.freeBytes = static_cast<int64_t>(s.f_bavail) * static_cast<int64_t>(s.f_frsize);
      vol.totalBytes = static_cast<int64_t>(s.f_blocks) * static_cast<int64_t>(s.f_frsize);
    }
  }
#endif
  return v;
}

namespace {
std::atomic<std::vector<Volume> (*)()> gReader{nullptr};
double nowSeconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
}  // namespace

struct VolumeWatch::Cell {
  std::mutex mu;
  std::shared_ptr<const std::vector<Volume>> last;
  double finishedAt = -1e9, startedAt = -1e9;
  bool running = false;
  uint64_t reads = 0;
};

void VolumeWatch::setReaderForTests(std::vector<Volume> (*reader)()) { gReader.store(reader); }

uint64_t VolumeWatch::reads() const {
  if (!cell_) return 0;
  std::lock_guard<std::mutex> lk(cell_->mu);
  return cell_->reads;
}

std::shared_ptr<const std::vector<Volume>> VolumeWatch::poll(double maxAge) {
  if (!cell_) cell_ = std::make_shared<Cell>();
  std::shared_ptr<Cell> c = cell_;
  std::lock_guard<std::mutex> lk(c->mu);
  const double now = nowSeconds();
  // A read that has not answered in 5 s is left to itself; the next one may start.
  const bool stuck = c->running && now - c->startedAt > 5.0;
  if ((!c->running || stuck) && now - c->finishedAt > maxAge) {
    c->running = true;
    c->startedAt = now;
    const double started = now;
    try {
      std::thread([c, started] {
        auto reader = gReader.load();
        auto v = std::make_shared<std::vector<Volume>>(reader ? reader() : detectVolumes());
        std::lock_guard<std::mutex> l2(c->mu);
        if (started < c->startedAt) return;  // a newer read took over
        c->last = std::move(v);
        c->finishedAt = nowSeconds();
        c->running = false;
        ++c->reads;
      }).detach();
    } catch (const std::exception&) {
      c->running = false;
    }
  }
  return c->last;
}

// ---- places ----

std::string xdgUserDir(const std::string& text, const std::string& key, const std::string& home) {
  std::istringstream in(text);
  std::string line;
  const std::string want = "XDG_" + key + "_DIR";
  while (std::getline(in, line)) {
    size_t a = line.find_first_not_of(" \t");
    if (a == std::string::npos || line[a] == '#') continue;
    if (line.compare(a, want.size(), want) != 0) continue;
    size_t eq = line.find('=', a + want.size());
    if (eq == std::string::npos) continue;
    std::string val = line.substr(eq + 1);
    while (!val.empty() && (val.back() == '\r' || val.back() == ' ')) val.pop_back();
    if (val.size() >= 2 && val.front() == '"' && val.back() == '"') val = val.substr(1, val.size() - 2);
    if (startsWith(val, "$HOME")) val = home + val.substr(5);
    if (val.empty() || val[0] != '/') continue;
    while (val.size() > 1 && val.back() == '/') val.pop_back();
    if (val == home) return "";  // "not used" in the XDG convention
    return val;
  }
  return "";
}

std::vector<Place> standardPlaces(const std::string& home, const std::string& userDirsText) {
  std::vector<Place> v;
  if (home.empty()) return v;
  const fs::path h = fs::u8path(home);
  v.push_back({i18n::N_("Home"), home, kIconHome});
  struct Std {
    const char* name;
    const char* key;
    const char* folder;
    int icon;
  };
  static const Std list[] = {{i18n::N_("Desktop"), "DESKTOP", "Desktop", kIconDesktop},
                             {i18n::N_("Documents"), "DOCUMENTS", "Documents", kIconDocuments},
                             {i18n::N_("Music"), "MUSIC", "Music", kIconMusic},
                             {i18n::N_("Downloads"), "DOWNLOAD", "Downloads", kIconDownloads}};
  for (const auto& s : list) {
    std::string p = userDirsText.empty() ? std::string() : xdgUserDir(userDirsText, s.key, home);
    if (p.empty()) p = (h / s.folder).u8string();
    v.push_back({s.name, p, s.icon});
  }
  return v;
}

// ---- lists ----

std::vector<std::string> decodeList(const std::string& text) {
  std::vector<std::string> v;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty() && !contains(v, line)) v.push_back(line);
  }
  return v;
}

std::string encodeList(const std::vector<std::string>& items) {
  std::string s;
  for (const auto& i : items) {
    if (i.empty() || i.find('\n') != std::string::npos) continue;
    if (!s.empty()) s += '\n';
    s += i;
  }
  return s;
}

bool contains(const std::vector<std::string>& items, const std::string& path) {
  return std::find(items.begin(), items.end(), path) != items.end();
}

bool setPinned(std::vector<std::string>& items, const std::string& path, bool pinned) {
  if (path.empty()) return false;
  auto it = std::find(items.begin(), items.end(), path);
  if (pinned == (it != items.end())) return false;
  if (pinned) items.push_back(path);
  else items.erase(it);
  return true;
}

void pushRecent(std::vector<std::string>& items, const std::string& path, size_t max) {
  if (path.empty()) return;
  items.erase(std::remove(items.begin(), items.end(), path), items.end());
  items.insert(items.begin(), path);
  if (items.size() > max) items.resize(max);
}

std::string lastFolder(const std::string& text, const std::string& purpose) {
  for (const auto& line : decodeList(text)) {
    const size_t tab = line.find('\t');
    if (tab != std::string::npos && line.compare(0, tab, purpose) == 0 && tab == purpose.size()) return line.substr(tab + 1);
  }
  return "";
}

std::string setLastFolder(const std::string& text, const std::string& purpose, const std::string& folder, size_t max) {
  std::vector<std::string> lines = decodeList(text);
  lines.erase(std::remove_if(lines.begin(), lines.end(),
                             [&](const std::string& l) { return l.size() > purpose.size() && l.compare(0, purpose.size(), purpose) == 0 && l[purpose.size()] == '\t'; }),
              lines.end());
  if (!purpose.empty() && !folder.empty() && purpose.find('\t') == std::string::npos) lines.insert(lines.begin(), purpose + '\t' + folder);
  if (lines.size() > max) lines.resize(max);
  return encodeList(lines);
}

// ---- paths ----

std::vector<Crumb> splitBreadcrumb(const std::string& pathIn, bool windows) {
  std::vector<Crumb> v;
  if (pathIn.empty()) return v;
  std::string path = pathIn;
  const auto isSep = [&](char c) { return c == '/' || (windows && c == '\\'); };
  const char sep = windows ? '\\' : '/';
  if (windows)
    for (char& c : path)
      if (c == '/') c = '\\';
  size_t at = 0;
  std::string acc;
  if (windows && path.size() >= 2 && isSep(path[0]) && isSep(path[1])) {
    // \\server\share is one part: the share's root.
    size_t s1 = path.find('\\', 2);
    size_t s2 = s1 == std::string::npos ? std::string::npos : path.find('\\', s1 + 1);
    const std::string root = path.substr(0, s2);
    v.push_back({root, root + "\\"});
    acc = root;
    at = s2 == std::string::npos ? path.size() : s2 + 1;
  } else if (windows && path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':') {
    const std::string drive = std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(path[0])))) + ":";
    v.push_back({drive, drive + "\\"});
    acc = drive;
    at = path.size() > 2 && isSep(path[2]) ? 3 : 2;
  } else if (isSep(path[0])) {
    v.push_back({std::string(1, sep), std::string(1, sep)});
    acc = "";
    at = 1;
  }
  while (at < path.size()) {
    size_t e = at;
    while (e < path.size() && !isSep(path[e])) ++e;
    if (e > at) {
      const std::string part = path.substr(at, e - at);
      if (part != ".") {
        if (v.empty() && acc.empty()) acc = part;
        else acc += sep + part;
        v.push_back({part, acc});
      }
    }
    at = e + 1;
  }
  return v;
}

void splitTyped(const std::string& typed, bool windows, std::string* folder, std::string* rest) {
  size_t s = typed.find_last_of(windows ? "/\\" : "/");
  if (s == std::string::npos) {
    if (folder) *folder = "";
    if (rest) *rest = typed;
    return;
  }
  if (folder) *folder = typed.substr(0, s + 1);
  if (rest) *rest = typed.substr(s + 1);
}

std::string completePath(const std::string& typed, const std::vector<std::string>& names, bool windows) {
  std::string folder, rest;
  splitTyped(typed, windows, &folder, &rest);
  const std::string lr = lowerCopy(rest);
  std::vector<const std::string*> hits;
  for (const auto& n : names) {
    std::string bare = n;
    if (!bare.empty() && bare.back() == '/') bare.pop_back();
    if (bare.empty() || (bare[0] == '.' && (rest.empty() || rest[0] != '.'))) continue;
    if (lowerCopy(bare).compare(0, lr.size(), lr) == 0 && bare.size() >= lr.size()) hits.push_back(&n);
  }
  if (hits.empty()) return typed;
  if (hits.size() == 1) {
    std::string n = *hits[0];
    if (!n.empty() && n.back() == '/') {
      n.pop_back();
      return folder + n + (windows ? "\\" : "/");
    }
    return folder + n;
  }
  // The longest start every match shares (case ignored; the first match's spelling).
  std::string first = *hits[0];
  if (!first.empty() && first.back() == '/') first.pop_back();
  size_t common = first.size();
  for (size_t i = 1; i < hits.size(); ++i) {
    std::string h = *hits[i];
    if (!h.empty() && h.back() == '/') h.pop_back();
    size_t k = 0;
    while (k < common && k < h.size() && std::tolower(static_cast<unsigned char>(h[k])) == std::tolower(static_cast<unsigned char>(first[k]))) ++k;
    common = k;
  }
  if (common <= rest.size()) return typed;
  return folder + first.substr(0, common);
}

// ---- entries ----

FileKind kindOf(const std::string& name, bool isDir) {
  if (isDir) return FileKind::Folder;
  const std::string l = lowerCopy(name);
  const auto ends = [&](const char* e) {
    const size_t n = std::strlen(e);
    return l.size() > n && l.compare(l.size() - n, n, e) == 0;
  };
  for (const char* e : {".gridline", ".gridlinez", ".gridline.json", ".gridline.zip", ".flp", ".xm", ".mod"})
    if (ends(e)) return FileKind::Project;
  for (const char* e : {".wav", ".wave", ".flac", ".mp3", ".ogg", ".wv", ".aif", ".aiff", ".aifc", ".m4a", ".aac", ".caf", ".rx2", ".rex", ".rcy"})
    if (ends(e)) return FileKind::Audio;
  for (const char* e : {".mid", ".midi", ".rmi"})
    if (ends(e)) return FileKind::Midi;
  for (const char* e : {".fst", ".fxp", ".fxb", ".vstpreset", ".aupreset", ".gllayer", ".gltrack", ".nki", ".sfz", ".sf2"})
    if (ends(e)) return FileKind::Preset;
  return FileKind::Other;
}

const char* kindLabel(FileKind k, const std::string& name) {
  switch (k) {
    case FileKind::Folder: return i18n::N_("Folder");
    case FileKind::Project: return i18n::N_("Project");
    case FileKind::Audio: return i18n::N_("Audio");
    case FileKind::Midi: return i18n::N_("MIDI");
    case FileKind::Preset: return i18n::N_("Preset");
    case FileKind::Other: break;
  }
  static thread_local char buf[32];
  const size_t dot = name.find_last_of('.');
  if (dot == std::string::npos || dot == 0 || name.size() - dot > 9) return i18n::N_("File");
  std::string e = name.substr(dot + 1);
  for (char& c : e) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  std::snprintf(buf, sizeof buf, "%s", e.c_str());
  return buf;
}

bool naturalLess(const std::string& a, const std::string& b) {
  size_t i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    const unsigned char ca = static_cast<unsigned char>(a[i]), cb = static_cast<unsigned char>(b[j]);
    if (std::isdigit(ca) && std::isdigit(cb)) {
      size_t ie = i, je = j;
      while (ie < a.size() && a[ie] == '0') ++ie;
      while (je < b.size() && b[je] == '0') ++je;
      size_t ie2 = ie, je2 = je;
      while (ie2 < a.size() && std::isdigit(static_cast<unsigned char>(a[ie2]))) ++ie2;
      while (je2 < b.size() && std::isdigit(static_cast<unsigned char>(b[je2]))) ++je2;
      if (ie2 - ie != je2 - je) return ie2 - ie < je2 - je;
      const int c = a.compare(ie, ie2 - ie, b, je, je2 - je);
      if (c != 0) return c < 0;
      i = ie2;
      j = je2;
      continue;
    }
    const int la = std::tolower(ca), lb = std::tolower(cb);
    if (la != lb) return la < lb;
    ++i;
    ++j;
  }
  if ((a.size() - i) != (b.size() - j)) return a.size() - i < b.size() - j;
  return a < b;  // equal but for case: a stable order all the same
}

void sortEntries(std::vector<Entry>& v, SortKey key, bool descending) {
  std::stable_sort(v.begin(), v.end(), [&](const Entry& x, const Entry& y) {
    if (x.isDir != y.isDir) return x.isDir;  // folders first, whichever way
    const auto by = [&](bool less, bool greater) {
      if (less != greater) return descending ? greater : less;
      return descending ? naturalLess(y.name, x.name) : naturalLess(x.name, y.name);
    };
    switch (key) {
      case SortKey::Name: return descending ? naturalLess(y.name, x.name) : naturalLess(x.name, y.name);
      case SortKey::Size: return by(x.size < y.size, x.size > y.size);
      case SortKey::Modified: return by(x.mtime < y.mtime, x.mtime > y.mtime);
      case SortKey::Kind: {
        const std::string kx = kindLabel(x.kind, x.name), ky = kindLabel(y.kind, y.name);
        return by(kx < ky, kx > ky);
      }
    }
    return false;
  });
}

std::string sizeText(int64_t bytes) {
  if (bytes < 0) return "";
  char buf[32];
  if (bytes < 1024) {
    std::snprintf(buf, sizeof buf, "%lld bytes", static_cast<long long>(bytes));
    return buf;
  }
  static const char* const units[] = {"KB", "MB", "GB", "TB", "PB"};
  double v = static_cast<double>(bytes) / 1024.0;
  int u = 0;
  while (v >= 1000.0 && u < 4) {
    v /= 1024.0;
    ++u;
  }
  std::snprintf(buf, sizeof buf, v < 10.0 ? "%.1f %s" : "%.0f %s", v, units[u]);
  return buf;
}

}  // namespace gl::filedlg
