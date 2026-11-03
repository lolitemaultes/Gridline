// Gridline: files on mounted web and network drives. See RemoteFs.h.
#include "RemoteFs.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace gl::remotefs {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

double nowSeconds() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }

std::string lowerCopy(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool underPrefix(const std::string& path, const std::string& prefix) {
  if (prefix.empty() || path.compare(0, prefix.size(), prefix) != 0) return false;
  return path.size() == prefix.size() || path[prefix.size()] == '/' || path[prefix.size()] == '\\' || prefix.back() == '/';
}

std::mutex gPrefixMu;
std::vector<std::string> gTestPrefixes;

bool underTestPrefix(const std::string& pathIn) {
  const std::string path = fs::u8path(pathIn).lexically_normal().u8string();
  static const std::vector<std::string> fromEnv = [] {
    std::vector<std::string> v;
    if (const char* e = std::getenv("GRIDLINE_DEV_REMOTE_PATHS")) {
#if defined(_WIN32)
      const char sep = ';';
#else
      const char sep = ':';
#endif
      const std::string s = e;
      for (size_t at = 0; at <= s.size();) {
        size_t end = s.find(sep, at);
        if (end == std::string::npos) end = s.size();
        if (end > at) v.push_back(fs::u8path(s.substr(at, end - at)).lexically_normal().u8string());
        at = end + 1;
      }
    }
    return v;
  }();
  for (const auto& p : fromEnv)
    if (underPrefix(path, p)) return true;
  std::lock_guard<std::mutex> lk(gPrefixMu);
  for (const auto& p : gTestPrefixes)
    if (underPrefix(path, p)) return true;
  return false;
}

#if !defined(_WIN32)
// The mount table, read without contacting any file system, kept for a few seconds.
struct Mount {
  std::string point;
  bool remote = false;
};

std::vector<Mount> readMounts() {
  std::vector<Mount> v;
#if defined(__APPLE__) || defined(__FreeBSD__)
  // MNT_NOWAIT: the kernel's last figures, a stuck mount cannot block this.
  const int n = getfsstat(nullptr, 0, MNT_NOWAIT);
  if (n <= 0) return v;
  std::vector<struct statfs> table(static_cast<size_t>(n) + 8);
  const int got = getfsstat(table.data(), static_cast<int>(table.size() * sizeof(struct statfs)), MNT_NOWAIT);
  for (int i = 0; i < got; ++i) {
    const struct statfs& m = table[static_cast<size_t>(i)];
    const bool nonLocal = !(m.f_flags & MNT_LOCAL);
    const bool special = !std::strcmp(m.f_fstypename, "devfs") || !std::strcmp(m.f_fstypename, "autofs");
    v.push_back({m.f_mntonname, remoteFsType(m.f_fstypename) || (nonLocal && !special)});
  }
#else
  // /proc/self/mountinfo: "... <mount point> <options> [fields] - <fstype> <source> <options>"
  std::ifstream in("/proc/self/mountinfo");
  std::string line;
  while (std::getline(in, line)) {
    size_t at = 0;
    std::string fields[5];
    for (auto& f : fields) {
      const size_t sp = line.find(' ', at);
      if (sp == std::string::npos) break;
      f = line.substr(at, sp - at);
      at = sp + 1;
    }
    const size_t dash = line.find(" - ");
    if (dash == std::string::npos) continue;
    const size_t tb = dash + 3, te = line.find(' ', tb);
    const std::string type = line.substr(tb, te == std::string::npos ? std::string::npos : te - tb);
    std::string point;
    for (size_t i = 0; i < fields[4].size(); ++i) {  // octal escapes for spaces and such
      if (fields[4][i] == '\\' && i + 3 < fields[4].size()) {
        point += static_cast<char>(std::strtol(fields[4].substr(i + 1, 3).c_str(), nullptr, 8));
        i += 3;
      } else {
        point += fields[4][i];
      }
    }
    v.push_back({point, remoteFsType(type)});
  }
#endif
  // Longest first: a mount inside another decides for its own paths.
  std::sort(v.begin(), v.end(), [](const Mount& a, const Mount& b) { return a.point.size() > b.point.size(); });
  return v;
}

bool mountIsRemote(const std::vector<Mount>& mounts, const std::string& path) {
  for (const auto& m : mounts)
    if (underPrefix(path, m.point)) return m.remote;
  return false;
}
#endif

}  // namespace

void setTestPrefixes(std::vector<std::string> prefixes) {
  std::lock_guard<std::mutex> lk(gPrefixMu);
  gTestPrefixes.clear();
  for (const auto& p : prefixes) gTestPrefixes.push_back(fs::u8path(p).lexically_normal().u8string());
}

bool remoteFsType(const std::string& typeIn) {
  const std::string t = lowerCopy(typeIn);
  if (t == "fuseblk" || t == "fuse.fuseblk") return false;  // a local disk through FUSE (NTFS, exFAT)
  if (t.rfind("fuse", 0) == 0 || t.rfind("osxfuse", 0) == 0 || t.rfind("macfuse", 0) == 0) return true;  // rclone, gvfs, sshfs, the portal, ...
  for (const char* r : {"nfs",  "nfs3", "nfs4", "cifs", "smb",       "smb2", "smb3",   "smbfs", "afpfs",  "webdav",   "davfs",  "ncpfs", "ftp",
                        "9p",   "ceph", "glusterfs",    "afs",       "lustre", "beegfs", "gpfs", "virtiofs", "vboxsf", "vmhgfs", "prl_fs", "lifs",
                        "mtp",  "gphotofs", "autofs"})
    if (t == r) return true;
  return false;
}

bool remoteByName(const std::string& path) {
  // File Provider drives (Google Drive, Dropbox, OneDrive, Box on macOS) and iCloud Drive sit on
  // the local disk's file system, so the mount table cannot tell them apart.
  for (const char* part : {"/Library/CloudStorage/", "/Library/Mobile Documents/", "/.CMVolumes/", "/gvfs/", "/.gvfs/"})
    if (path.find(part) != std::string::npos) return true;
  return path.rfind("//", 0) == 0 || path.rfind("\\\\", 0) == 0;  // a network share by name
}

bool isRemote(const std::string& path) {
  if (path.empty()) return false;
  if (underTestPrefix(path) || remoteByName(path)) return true;
#if defined(_WIN32)
  std::wstring w;
  try {
    std::error_code ec;
    w = fs::absolute(fs::u8path(path), ec).wstring();
  } catch (const std::exception&) {
    return false;
  }
  if (w.size() < 3 || w[1] != L':') return false;
  const wchar_t root[4] = {w[0], L':', L'\\', 0};
  if (GetDriveTypeW(root) == DRIVE_REMOTE) return true;
  // Cloud placeholders (OneDrive and others): the attributes say so without fetching the data.
  const DWORD at = GetFileAttributesW(w.c_str());
  if (at == INVALID_FILE_ATTRIBUTES) return false;
  constexpr DWORD kRecallOnOpen = 0x00040000, kPinned = 0x00080000, kUnpinned = 0x00100000, kRecallOnAccess = 0x00400000;
  return (at & (FILE_ATTRIBUTE_OFFLINE | kRecallOnOpen | kRecallOnAccess | kPinned | kUnpinned)) != 0;
#else
  static std::mutex mu;
  static std::vector<Mount> mounts;
  static std::map<std::string, bool> known;  // callers ask every frame: the walk below runs once per table
  static double readAt = -1e9;
  std::lock_guard<std::mutex> lk(mu);
  if (nowSeconds() - readAt > 5.0) {
    mounts = readMounts();
    readAt = nowSeconds();
    known.clear();
  }
  if (const auto it = known.find(path); it != known.end()) return it->second;
  const auto remember = [&](bool r) {
    if (known.size() > 4096) known.clear();
    known[path] = r;
    return r;
  };
  // Walk the path from the top so a symbolic link is seen (a link to a drive's folder), without
  // a single lstat below a remote mount. The folders above a mount are on a local disk.
  std::string cur = path;
  if (cur[0] != '/') {
    std::error_code ec;
    cur = fs::absolute(fs::u8path(path), ec).u8string();
  }
  for (int hops = 0; hops < 8; ++hops) {
    if (mountIsRemote(mounts, cur) || remoteByName(cur) || underTestPrefix(cur)) return remember(true);
    bool relinked = false;
    for (size_t at = 1; at <= cur.size();) {
      size_t end = cur.find('/', at);
      if (end == std::string::npos) end = cur.size();
      const std::string part = cur.substr(0, end);
      struct stat st;
      if (lstat(part.c_str(), &st) != 0) return remember(false);
      if (S_ISLNK(st.st_mode)) {
        char b[4096];
        const ssize_t n = readlink(part.c_str(), b, sizeof b - 1);
        if (n <= 0) return remember(false);
        const std::string target(b, static_cast<size_t>(n));
        const std::string rest = end < cur.size() ? cur.substr(end) : std::string();
        const std::string base = target[0] == '/' ? target : fs::u8path(part).parent_path().u8string() + "/" + target;
        cur = fs::u8path(base + rest).lexically_normal().u8string();
        relinked = true;
        break;
      }
      at = end + 1;
    }
    if (!relinked) return remember(false);
  }
  return remember(false);
#endif
}

// ---------------------------------------------------------------- reading

namespace {

class FileSource final : public ByteSource {
 public:
  FileSource(std::ifstream&& in, int64_t size, std::string path) : in_(std::move(in)), size_(size), path_(std::move(path)) {}
  int64_t size() const override { return size_; }
  int64_t readAt(uint64_t offset, void* out, size_t n, std::string* err) override {
    if (offset >= static_cast<uint64_t>(size_)) return 0;
    errno = 0;
    in_.clear();
    if (offset != pos_) {  // a sequential read never seeks (a seek can reopen the file on a drive)
      in_.seekg(static_cast<std::streamoff>(offset));
      if (!in_) {
        if (err) *err = readError(path_, errno);
        return -1;
      }
      pos_ = offset;
    }
    in_.read(static_cast<char*>(out), static_cast<std::streamsize>(n));
    const std::streamsize got = in_.gcount();
    pos_ += static_cast<uint64_t>(got);
    if (got > 0) return got;
    if (err) *err = readError(path_, errno ? errno : EIO);  // the file said it was longer
    return -1;
  }

 private:
  std::ifstream in_;
  int64_t size_;
  std::string path_;
  uint64_t pos_ = 0;
};

}  // namespace

std::unique_ptr<ByteSource> openFile(const std::string& path, std::string* err) {
  fs::path p;
  try {
    p = fs::u8path(path);
  } catch (const std::exception&) {
    if (err) *err = readError(path, ENOENT);
    return nullptr;
  }
  std::error_code ec;
  const uintmax_t size = fs::file_size(p, ec);
  if (ec) {
    if (err) *err = readError(path, ec.default_error_condition().value());
    return nullptr;
  }
  errno = 0;
  std::ifstream in(p, std::ios::binary);
  if (!in) {
    if (err) *err = readError(path, errno ? errno : EIO);
    return nullptr;
  }
  return std::make_unique<FileSource>(std::move(in), static_cast<int64_t>(size), path);
}

std::string readError(const std::string& path, int errnum) {
  std::string name;
  try {
    name = fs::u8path(path).filename().u8string();
  } catch (const std::exception&) {
    name = path;
  }
  const std::string head = "cannot read " + (name.empty() ? path : name) + ": ";
  switch (errnum) {
    case ENOENT:
    case ENOTDIR: return head + "the file is not there. If it is on a drive, check that the drive is connected and online.";
    case EACCES:
    case EPERM: return head + "no permission to read it. Check its permissions, and on macOS whether Gridline may read that drive.";
    case ENOTCONN:
#ifdef EHOSTDOWN
    case EHOSTDOWN:
#endif
    case ENETDOWN:
    case ENETUNREACH:
    case EHOSTUNREACH:
    case ETIMEDOUT:
#ifdef ESTALE
    case ESTALE:
#endif
      return head + "the drive is not answering. Check that it is connected and online, then try again.";
    case EIO: return head + "the read failed. If the file is on a drive, it may be offline or not fully downloaded; try again.";
    default: break;
  }
  return head + (errnum ? std::string(std::strerror(errnum)) : std::string("the read failed")) + ".";
}

bool readAll(ByteSource& s, std::vector<uint8_t>& out, const ReadWatch& w, std::string* err) {
  out.clear();
  const int64_t size = s.size();
  if (size < 0) {
    if (err) *err = "cannot read the file";
    return false;
  }
  out.resize(static_cast<size_t>(size));
  size_t at = 0;
  while (at < out.size()) {
    if (w.cancelled()) {
      if (err) *err = kCancelled;
      return false;
    }
    const int64_t got = s.readAt(at, out.data() + at, std::min(kReadChunk, out.size() - at), err);
    if (got < 0) return false;
    if (got == 0) {
      if (err) *err = "the file ended early: it may not be fully downloaded";
      return false;
    }
    at += static_cast<size_t>(got);
    if (w.onBytes) w.onBytes(static_cast<uint64_t>(got));
  }
  return true;
}

// ---------------------------------------------------------------- off-thread stat and listing

namespace {

std::atomic<uint64_t> gWalks{0};
// At most this many threads at once: a stuck mount can hold them, and then nothing new starts
// (the callers keep "not known yet", which they must handle anyway).
std::atomic<int> gThreads{0};
constexpr int kMaxThreads = 8;

// Each thread is detached and holds only its own shared cell, so a stat stuck for ever cannot
// hold up the interface or quitting.
struct ProbeCell {
  std::mutex mu;
  Probe last;
  double at = -1e9;
  bool running = false;
};
struct ListCell {
  std::mutex mu;
  std::shared_ptr<const Listing> last;
  double at = -1e9, started = 0;
  bool running = false;
};

std::mutex gCellsMu;
std::map<std::string, std::shared_ptr<ProbeCell>> gProbes;
std::map<std::string, std::shared_ptr<ListCell>> gLists;

template <class Fn>
bool startThread(Fn fn) {
  if (gThreads.fetch_add(1) >= kMaxThreads) {
    gThreads.fetch_sub(1);
    return false;
  }
  try {
    std::thread([fn = std::move(fn)]() mutable {
      fn();
      gThreads.fetch_sub(1);
    }).detach();
  } catch (const std::exception&) {
    gThreads.fetch_sub(1);
    return false;
  }
  return true;
}

std::shared_ptr<ProbeCell> probeCell(const std::string& path) {
  std::lock_guard<std::mutex> lk(gCellsMu);
  auto& c = gProbes[path];
  if (!c) c = std::make_shared<ProbeCell>();
  return c;
}

Probe statNow(const std::string& path) {
  Probe r;
  r.known = true;
  std::error_code ec;
  const fs::file_status st = fs::status(fs::u8path(path), ec);
  r.exists = !ec && fs::exists(st);
  r.isDir = !ec && fs::is_directory(st);
  return r;
}

}  // namespace

Probe probe(const std::string& path, double maxAge) {
  auto c = probeCell(path);
  std::lock_guard<std::mutex> lk(c->mu);
  if (!c->running && nowSeconds() - c->at > maxAge) {
    c->running = startThread([c, path] {
      Probe r;
      try {
        r = statNow(path);
      } catch (const std::exception&) {
        r.known = true;
      }
      std::lock_guard<std::mutex> l2(c->mu);
      c->last = r;
      c->at = nowSeconds();
      c->running = false;
    });
  }
  return c->last;
}

Probe probeWithin(const std::string& path, int ms) {
  Probe r = probe(path, 1.0);
  const auto until = Clock::now() + std::chrono::milliseconds(ms);
  while (!r.known && Clock::now() < until) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    r = probe(path, 1.0);
  }
  return r;
}

Listing listNow(const std::string& dir) {
  Listing l;
  countWalk();
  std::error_code ec;
  const fs::path p = fs::u8path(dir);
  fs::directory_iterator it(p, fs::directory_options::skip_permission_denied, ec), end;
  if (ec) {
    l.errnum = ec.value();
    return l;
  }
  for (; !ec && it != end && l.entries.size() < kMaxListed; it.increment(ec)) {
    DirEntry e;
    e.name = it->path().filename().u8string();
    if (e.name.empty() || e.name[0] == '.') continue;
    std::error_code e2;
    e.isDir = it->is_directory(e2);
    l.entries.push_back(std::move(e));
  }
  l.ok = !ec;
  l.errnum = ec ? ec.value() : 0;
  return l;
}

DirState listAsync(const std::string& dir, double maxAge) {
  std::shared_ptr<ListCell> c;
  {
    std::lock_guard<std::mutex> lk(gCellsMu);
    auto& slot = gLists[dir];
    if (!slot) slot = std::make_shared<ListCell>();
    c = slot;
  }
  std::lock_guard<std::mutex> lk(c->mu);
  const double now = nowSeconds();
  if (!c->running && now - c->at > maxAge) {
    c->started = now;
    c->running = startThread([c, dir] {
      auto l = std::make_shared<Listing>();
      if (const char* d = std::getenv("GRIDLINE_DEV_SLOW_LIST_MS")) std::this_thread::sleep_for(std::chrono::milliseconds(std::atoi(d)));  // dev / tests
      try {
        *l = listNow(dir);
      } catch (const std::exception&) {
        l->errnum = EIO;
      }
      std::lock_guard<std::mutex> l2(c->mu);
      c->last = std::move(l);
      c->at = nowSeconds();
      c->running = false;
    });
  }
  DirState s;
  s.listing = c->last;
  s.pending = c->running;
  s.waited = c->running ? now - c->started : 0.0;
  return s;
}

void forgetListings() {
  std::lock_guard<std::mutex> lk(gCellsMu);
  gLists.clear();
  gProbes.clear();
}

uint64_t walksStarted() { return gWalks.load(std::memory_order_relaxed); }
void countWalk() { gWalks.fetch_add(1, std::memory_order_relaxed); }

}  // namespace gl::remotefs
