// Gridline: opening a zipped project. See ProjectArchive.h.
#include "ProjectArchive.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <cstdlib>
#include <sstream>
#include <thread>
#include "Crash.h"
#include "Platform.h"
#include "Project.h"
#include "ProjectFiles.h"
#include "ProjectInterop.h"

namespace gl::archive {

namespace fs = std::filesystem;

namespace {

constexpr const char* kManifest = ".gridline-unpacked";
constexpr const char* kPinned = ".gridline-kept";
constexpr const char* kLocalSource = ".gridline-local";  // the archive is on a local disk
constexpr const char* kPartTag = ".part-";               // being unpacked
constexpr const char* kTrashTag = ".removing-";          // taken out of use, being deleted

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool endsWith(const std::string& s, const char* tail) {
  const size_t n = std::char_traits<char>::length(tail);
  return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}

std::string baseName(const std::string& name) {
  const size_t at = name.find_last_of('/');
  return at == std::string::npos ? name : name.substr(at + 1);
}

bool inBackupFolder(const std::string& lowerName) {
  size_t at = 0;
  for (size_t slash; (slash = lowerName.find('/', at)) != std::string::npos; at = slash + 1) {
    const std::string part = lowerName.substr(at, slash - at);
    if (part == "backup" || part == "backups" || part == "autosave" || part == "autosaves") return true;
  }
  const std::string base = lowerName.substr(at);
  return base.find("(backup ") != std::string::npos || base.find("overwritten at") != std::string::npos;
}

std::string fnvHex(const std::string& s) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (const unsigned char c : s) {
    h ^= c;
    h *= 0x100000001b3ull;
  }
  char b[20];
  std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(h));
  return b;
}

std::string safeStem(std::string s) {
  for (char& c : s)
    if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || static_cast<unsigned char>(c) < 32) c = '_';
  if (s.size() > 48) s.resize(48);
  while (!s.empty() && (s.back() == ' ' || s.back() == '.' || (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80)) s.pop_back();
  if (!s.empty() && (static_cast<unsigned char>(s.back()) & 0x80)) s.pop_back();  // (a cut character's lead byte)
  return s.empty() ? std::string("project") : s;
}

struct Manifest {
  std::string source;
  uint64_t size = 0;
  int64_t mtime = 0;
  std::vector<std::string> names;  // unpacked entries, relative
};

bool readManifest(const fs::path& folder, Manifest& m) {
  std::ifstream in(folder / kManifest, std::ios::binary);
  std::string line;
  if (!in || !std::getline(in, line) || line != "gridline-unpacked 1") return false;
  if (!std::getline(in, m.source)) return false;
  if (!std::getline(in, line)) return false;
  m.size = std::strtoull(line.c_str(), nullptr, 10);
  if (!std::getline(in, line)) return false;
  m.mtime = std::strtoll(line.c_str(), nullptr, 10);
  m.names.clear();
  while (std::getline(in, line))
    if (!line.empty()) m.names.push_back(line);
  return true;
}

bool writeManifest(const fs::path& folder, const Manifest& m) {
  std::ofstream o(folder / kManifest, std::ios::binary | std::ios::trunc);
  o << "gridline-unpacked 1\n" << m.source << "\n" << m.size << "\n" << m.mtime << "\n";
  for (const auto& n : m.names) o << n << "\n";
  return static_cast<bool>(o);
}

// The copy is whole: every file it listed is still there.
bool copyIsWhole(const fs::path& folder, const Manifest& m) {
  std::error_code ec;
  for (const auto& n : m.names)
    if (!fs::is_regular_file(folder / fs::u8path(n), ec)) return false;
  return !m.names.empty();
}

std::vector<ZipEntry> namedEntries(const std::vector<std::string>& names) {
  std::vector<ZipEntry> v;
  for (const auto& n : names) {
    ZipEntry e;
    e.name = n;
    e.size = 1;
    v.push_back(std::move(e));
  }
  return v;
}

uint64_t folderBytes(const fs::path& dir) {
  uint64_t n = 0;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    if (it->is_regular_file(e2)) n += static_cast<uint64_t>(it->file_size(e2));
  }
  return n;
}

int64_t ageSeconds(fs::file_time_type t) {
  const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  return now - platform::fileTimeSeconds(t);
}

// Decides which project to open out of the unpacked names. false: the person stopped it.
bool chooseProject(const UnpackRequest& r, const std::vector<ZipEntry>& entries, const std::string& stem, std::string& chosen, UnpackResult& out,
                   std::string* err) {
  const ProjectPick pick = pickProject(entries, stem);
  if (pick.names.empty()) {
    if (err) *err = "the archive holds no project file";
    return false;
  }
  size_t index = 0;
  if (pick.ambiguous && r.choose) {
    const int c = r.choose(pick.names);
    if (c < 0 || static_cast<size_t>(c) >= pick.names.size()) {
      if (err) *err = remotefs::kCancelled;
      return false;
    }
    index = static_cast<size_t>(c);
  }
  out.otherProjects = pick.names.size() - 1;
  chosen = pick.names[index];
  return true;
}

// Newest whole copy of this archive in the cache.
bool lastCopyOf(const fs::path& cache, const std::string& source, fs::path& folder, Manifest& found) {
  std::error_code ec;
  fs::file_time_type best{};
  bool have = false;
  for (fs::directory_iterator it(cache, ec), end; !ec && it != end; it.increment(ec)) {
    Manifest m;
    if (!readManifest(it->path(), m) || m.source != source || !copyIsWhole(it->path(), m)) continue;
    std::error_code e2;
    const auto t = fs::last_write_time(it->path() / kManifest, e2);
    if (!have || t > best) {
      have = true;
      best = t;
      folder = it->path();
      found = std::move(m);
    }
  }
  return have;
}


// ---- locks: <root>.locks/<folder name> says who uses a copy (or an unpack in progress).
// Holders keep it shared; whoever removes a copy needs it exclusive. Every step that takes a copy's
// lock, deletes a lock file, or gives a copy its name or takes it away happens under the short
// <root>.lock, so "free" and "gone" are one step for every process. Both live beside the root: what
// is inside the root is copies only.

fs::path locksDir(const fs::path& root) { return fs::path(root.u8string() + ".locks"); }
fs::path lockFile(const fs::path& folder) { return locksDir(folder.parent_path()) / folder.filename(); }

class CacheGuard {
 public:
  explicit CacheGuard(const fs::path& root) {
    std::error_code ec;
    fs::create_directories(locksDir(root), ec);
    const std::string file = root.u8string() + ".lock";
    for (int i = 0; i < 150 && !lock_.tryLock(file, platform::FileLock::Mode::Exclusive); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  bool ok() const { return lock_.held(); }

 private:
  platform::FileLock lock_;
};

// (under the guard)
CopyHold holdLocked(const fs::path& folder) {
  auto use = std::make_shared<CopyUse>();
  use->folder = folder.u8string();
  if (!use->lock.tryLock(lockFile(folder).u8string(), platform::FileLock::Mode::Shared)) return nullptr;
  return use;
}

fs::path trashName(const fs::path& folder) {
  static std::atomic<unsigned> serial{0};
  return fs::path(folder.u8string() + kTrashTag + std::to_string(platform::processId()) + "-" + std::to_string(serial.fetch_add(1)));
}

enum class Taken { Yes, InUse, Busy };

// (under the guard) No one holds it: it takes a name no one looks for, in one step, and only the
// deleting is left. Where a file in it is open the name cannot change (Windows): nothing was touched
// and it is tried again another time.
Taken takeOutOfUse(const fs::path& folder, fs::path& trash) {
  platform::FileLock free;
  const fs::path lf = lockFile(folder);
  if (!free.tryLock(lf.u8string(), platform::FileLock::Mode::Exclusive)) return Taken::InUse;
  trash = trashName(folder);
  std::error_code ec;
  fs::rename(folder, trash, ec);
  if (ec) return Taken::Busy;
  free.unlock();
  fs::remove(lf, ec);
  return Taken::Yes;
}

void deleteTrash(const fs::path& trash) {
  std::error_code ec;
  fs::remove_all(trash, ec);  // (what cannot go now stays under its name: the next sweep tries again)
}

bool hasTag(const std::string& name, const char* tag) { return name.find(tag) != std::string::npos; }

// A place that is not a cache of ours is never swept.
bool sweepableRoot(const fs::path& root) { return !root.empty() && root.has_filename() && root.has_parent_path() && root != root.root_path(); }

// The part of a copy's name that is the same however a path is written in a project ("-<hash>").
std::string tokenOf(const fs::path& folder) {
  const std::string n = folder.filename().u8string();
  const size_t dash = n.rfind('-');
  return dash != std::string::npos && n.size() - dash == 17 ? n.substr(dash) : n;
}

struct Candidate {
  fs::path dir;
  std::string token;
  uint64_t bytes = 0;
  fs::file_time_type at{};
  bool recovery = false;   // a crash recovery file points at it
  bool backup = false;     // a recent backup points at it
  bool undecided = false;  // something that could point at it could not be read
};

// Looks through the files in `folder` (and one level below) for the candidates' names. maxAge < 0:
// any age. false: something could not be read, so nothing can be ruled out.
bool scanRefs(const std::string& folderIn, int64_t maxAge, std::vector<Candidate>& cands, bool Candidate::*mark) {
  const fs::path folder = fs::u8path(folderIn);
  std::error_code ec;
  if (!fs::exists(folder, ec)) return !ec || ec == std::errc::no_such_file_or_directory;
  bool allRead = true;
  std::vector<char> buf(1u << 20);
  size_t longest = 0;
  for (const auto& c : cands) longest = std::max(longest, c.token.size());
  for (fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
    if (ec) return false;
    if (it.depth() >= 1) it.disable_recursion_pending();
    std::error_code e2;
    if (!it->is_regular_file(e2)) continue;
    if (maxAge >= 0) {
      const auto t = it->last_write_time(e2);
      if (!e2 && ageSeconds(t) > maxAge) continue;
    }
    std::ifstream in(it->path(), std::ios::binary);
    if (!in) {
      allRead = false;
      continue;
    }
    std::string window;
    while (in) {
      in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
      window.append(buf.data(), static_cast<size_t>(in.gcount()));
      for (auto& c : cands)
        if (!(c.*mark) && window.find(c.token) != std::string::npos) c.*mark = true;
      if (window.size() > longest) window.erase(0, window.size() - longest);  // (a name across two reads)
    }
    if (in.bad()) allRead = false;
  }
  return ec ? false : allRead;
}

// Who else points at these candidates.
void markRefs(const CacheRefs& refs, std::vector<Candidate>& cands) {
  if (cands.empty()) return;
  bool allRead = true;
  for (const auto& f : refs.recovery) allRead = scanRefs(f, -1, cands, &Candidate::recovery) && allRead;
  for (const auto& f : refs.backups) allRead = scanRefs(f, int64_t{kBackupRefDays} * 86400, cands, &Candidate::backup) && allRead;
  if (!allRead)
    for (auto& c : cands) c.undecided = true;
}

// Lock files of copies that are gone.
void dropStaleLocks(const fs::path& root) {
  CacheGuard guard(root);
  if (!guard.ok()) return;
  std::error_code ec;
  const fs::path dir = locksDir(root);
  std::vector<fs::path> stale;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    if (!fs::exists(root / it->path().filename(), e2) && !e2) stale.push_back(it->path());
  }
  for (const auto& p : stale) {
    platform::FileLock l;
    if (l.tryLock(p.u8string(), platform::FileLock::Mode::Exclusive)) {
      l.unlock();
      std::error_code e2;
      fs::remove(p, e2);
    }
  }
}

CacheReport cleanCache(const fs::path& root, bool wholeCache, const CacheRefs& refs) {
  CacheReport rep;
  if (!sweepableRoot(root)) return rep;
  std::vector<Candidate> cands;
  std::error_code ec;
  for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    const fs::file_status st = it->symlink_status(e2);
    if (e2 || !fs::is_directory(st)) {  // (a link or a stray file is not ours)
      ++rep.keptUnknown;
      continue;
    }
    const fs::path dir = it->path();
    const std::string name = dir.filename().u8string();
    if (hasTag(name, kTrashTag)) {  // an interrupted removal
      const uint64_t n = folderBytes(dir);
      deleteTrash(dir);
      rep.freed += n;
      continue;
    }
    if (hasTag(name, kPartTag)) {
      // An unpack holds its lock from before its first byte. Earlier versions had none: theirs goes
      // only when it is a day old.
      const bool hasLock = fs::exists(lockFile(dir), e2);
      if (!hasLock) {
        std::error_code e3;
        const auto t = fs::last_write_time(dir, e3);
        if (e2 || e3 || ageSeconds(t) <= 86400) {
          ++rep.keptUnknown;
          continue;
        }
      }
      const uint64_t n = folderBytes(dir);
      fs::path trash;
      Taken t = Taken::InUse;
      {
        CacheGuard guard(root);
        if (guard.ok()) t = takeOutOfUse(dir, trash);
      }
      if (t == Taken::Yes) {
        deleteTrash(trash);
        ++rep.removed;
        rep.freed += n;
      } else if (t == Taken::Busy) {
        ++rep.keptBusy;
      }  // (InUse: an unpack is running; it is not a copy yet and is not counted)
      continue;
    }
    Manifest m;
    if (!readManifest(dir, m)) {
      ++rep.keptUnknown;
      continue;
    }
    if (fs::exists(dir / kPinned, e2) || e2) {
      ++rep.keptSaved;
      continue;
    }
    const bool fromDrive = !fs::exists(dir / kLocalSource, e2);
    if (e2) {
      ++rep.keptUnknown;
      continue;
    }
    if (!wholeCache && closedCopyStays(fromDrive)) {
      ++rep.keptDrive;
      continue;
    }
    {  // Held now: no need to read anything about it.
      CacheGuard guard(root);
      platform::FileLock probe;
      if (!guard.ok() || !probe.tryLock(lockFile(dir).u8string(), platform::FileLock::Mode::Exclusive)) {
        ++rep.keptOpen;
        continue;
      }
    }
    Candidate c;
    c.dir = dir;
    c.token = tokenOf(dir);
    cands.push_back(std::move(c));
  }
  markRefs(refs, cands);
  for (auto& c : cands) {
    if (c.undecided) {
      ++rep.keptUnknown;
      continue;
    }
    if (c.recovery || c.backup) {
      ++rep.keptRecovery;
      continue;
    }
    const uint64_t n = folderBytes(c.dir);
    fs::path trash;
    Taken t = Taken::InUse;
    bool pinnedNow = false;
    {
      CacheGuard guard(root);
      std::error_code e2;
      pinnedNow = fs::exists(c.dir / kPinned, e2) || e2;  // (saved meanwhile)
      if (guard.ok() && !pinnedNow) t = takeOutOfUse(c.dir, trash);
    }
    if (pinnedNow) {
      ++rep.keptSaved;
    } else if (t == Taken::Yes) {
      deleteTrash(trash);
      ++rep.removed;
      rep.freed += n;
    } else if (t == Taken::Busy) {
      ++rep.keptBusy;
    } else {
      ++rep.keptOpen;
    }
  }
  dropStaleLocks(root);
  return rep;
}

// A folder path the song refers to, in the cache: the top folder below the root.
std::vector<std::string> foldersUnder(const Song& song, const fs::path& root) {
  auto forward = [](std::string s) {
#if defined(_WIN32)
    std::replace(s.begin(), s.end(), '\\', '/');
#endif
    return s;
  };
  const std::string prefix = forward(root.u8string()) + "/";
  std::map<std::string, bool> found;
  for (const auto& pIn : referencedFiles(song)) {
    const std::string p = forward(pIn);
    if (p.rfind(prefix, 0) != 0) continue;
    const size_t slash = p.find('/', prefix.size());
    if (slash != std::string::npos) found[p.substr(0, slash)] = true;
  }
  std::vector<std::string> out;
  for (const auto& kv : found) out.push_back(fs::u8path(kv.first).make_preferred().u8string());
  return out;
}

}  // namespace

CacheRefs CacheRefs::standard() {
  const fs::path cfg = fs::u8path(platform::configRoot()) / "gridline";
  CacheRefs r;
  r.recovery.push_back(crash::recoveryDir(cfg.u8string()));
  r.backups.push_back((cfg / "Backup").u8string());
  return r;
}

bool isJunkEntry(const std::string& name) {
  const std::string l = lower(name);
  if (l.rfind("__macosx/", 0) == 0 || l.find("/__macosx/") != std::string::npos) return true;
  const std::string b = baseName(l);
  return b == ".ds_store" || b == "thumbs.db" || b == "desktop.ini" || b.rfind("._", 0) == 0;
}

ProjectPick pickProject(const std::vector<ZipEntry>& entries, const std::string& zipStem) {
  struct Cand {
    std::string name;
    int kind;
    bool backup, stem;
    long depth;
  };
  std::vector<Cand> all;
  const std::string wantStem = lower(zipStem);
  for (const auto& e : entries) {
    if (e.dir() || e.size == 0 || isJunkEntry(e.name)) continue;
    const std::string l = lower(e.name);
    int kind = -1;
    if (endsWith(l, ".gridline.json")) kind = 0;
    else if (endsWith(l, ".flp")) kind = 1;
    else if (endsWith(l, ".json")) kind = 2;
    if (kind < 0) continue;
    const std::string own = lower(projectStemOf(baseName(e.name)));
    all.push_back({e.name, kind, inBackupFolder(l), own == wantStem, static_cast<long>(std::count(l.begin(), l.end(), '/'))});
  }
  ProjectPick p;
  if (all.empty()) return p;
  // Only the best kind, and copies in backup folders only when nothing else is there.
  int best = 3;
  for (const auto& c : all) best = std::min(best, c.kind);
  all.erase(std::remove_if(all.begin(), all.end(), [&](const Cand& c) { return c.kind != best; }), all.end());
  const bool anyMain = std::any_of(all.begin(), all.end(), [](const Cand& c) { return !c.backup; });
  if (anyMain) all.erase(std::remove_if(all.begin(), all.end(), [](const Cand& c) { return c.backup; }), all.end());
  std::stable_sort(all.begin(), all.end(), [](const Cand& a, const Cand& b) {
    if (a.stem != b.stem) return a.stem;
    if (a.depth != b.depth) return a.depth < b.depth;
    return lower(a.name) < lower(b.name);
  });
  for (const auto& c : all) p.names.push_back(c.name);
  // One carrying the archive's own name settles it (FL's backups and alternates sit beside it).
  p.ambiguous = all.size() > 1 && !all.front().stem;
  return p;
}

std::string defaultCacheRoot() {
  // (GRIDLINE_CACHE_DIR: the isolated cache of scripted runs, as Gridline's other caches have it)
  if (const char* e = std::getenv("GRIDLINE_CACHE_DIR"); e && *e) return (fs::u8path(e) / "unpacked").u8string();
  return (fs::u8path(platform::cacheRoot()) / "gridline" / "unpacked").u8string();
}

int trimCache(const std::string& cacheRoot, uint64_t limit, const std::string& keep, int64_t minAgeSeconds, const CacheRefs* refsIn) {
  const fs::path root = fs::u8path(cacheRoot);
  if (!sweepableRoot(root)) return 0;
  std::vector<Candidate> copies;
  uint64_t total = 0;
  std::error_code ec;
  for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    if (!fs::is_directory(it->symlink_status(e2))) continue;
    const std::string name = it->path().filename().u8string();
    if (hasTag(name, kTrashTag) || hasTag(name, kPartTag)) continue;
    const auto stamp = fs::last_write_time(it->path() / kManifest, e2);
    if (e2) continue;  // (not a copy)
    const uint64_t bytes = folderBytes(it->path());
    total += bytes;
    if (it->path().u8string() == keep || fs::exists(it->path() / kPinned, e2) || ageSeconds(stamp) < minAgeSeconds) continue;
    Candidate c;
    c.dir = it->path();
    c.token = tokenOf(c.dir);
    c.bytes = bytes;
    c.at = stamp;
    copies.push_back(std::move(c));
  }
  if (total <= limit) return 0;
  markRefs(refsIn ? *refsIn : CacheRefs::standard(), copies);
  copies.erase(std::remove_if(copies.begin(), copies.end(), [](const Candidate& c) { return c.recovery || c.undecided; }), copies.end());
  std::sort(copies.begin(), copies.end(), [](const Candidate& a, const Candidate& b) {
    if (a.backup != b.backup) return b.backup;  // a backup's samples go last
    return a.at < b.at;
  });
  int removed = 0;
  for (const auto& c : copies) {
    if (total <= limit) break;
    fs::path trash;
    Taken t = Taken::InUse;
    {
      CacheGuard guard(root);
      if (guard.ok()) t = takeOutOfUse(c.dir, trash);
    }
    if (t == Taken::Yes) {
      deleteTrash(trash);
      total -= std::min(total, c.bytes);
      ++removed;
    }
  }
  return removed;
}

std::vector<std::string> copiesUsedBy(const Song& song, const std::string& cacheRootIn) {
  return foldersUnder(song, fs::u8path(cacheRootIn.empty() ? defaultCacheRoot() : cacheRootIn));
}

int pinUnpacked(const Song& song, const std::string& cacheRootIn) {
  int n = 0;
  for (const auto& folder : copiesUsedBy(song, cacheRootIn)) {
    std::ofstream o(fs::u8path(folder) / kPinned, std::ios::binary);
    n += static_cast<bool>(o) ? 1 : 0;
  }
  return n;
}

CopyHold holdCopy(const std::string& folder, const std::string& cacheRootIn) {
  const fs::path root = fs::u8path(cacheRootIn.empty() ? defaultCacheRoot() : cacheRootIn);
  const fs::path dir = fs::u8path(folder);
  std::error_code ec;
  CacheGuard guard(root);
  if (!guard.ok() || !fs::is_directory(dir, ec)) return nullptr;
  return holdLocked(dir);
}

uint64_t cacheBytes(const std::string& cacheRootIn) {
  const fs::path root = fs::u8path(cacheRootIn.empty() ? defaultCacheRoot() : cacheRootIn);
  return sweepableRoot(root) ? folderBytes(root) : 0;  // (never a walk of anything that is not a cache)
}

CacheReport sweepCache(const std::string& cacheRootIn, uint64_t limit, const CacheRefs* refsIn) {
  const std::string rootName = cacheRootIn.empty() ? defaultCacheRoot() : cacheRootIn;
  const CacheRefs refs = refsIn ? *refsIn : CacheRefs::standard();
  CacheReport rep = cleanCache(fs::u8path(rootName), false, refs);
  rep.removed += trimCache(rootName, limit, "", 3600, &refs);
  rep.bytesLeft = cacheBytes(rootName);
  return rep;
}

CacheReport clearCache(const std::string& cacheRootIn, const CacheRefs* refsIn) {
  const std::string rootName = cacheRootIn.empty() ? defaultCacheRoot() : cacheRootIn;
  CacheReport rep = cleanCache(fs::u8path(rootName), true, refsIn ? *refsIn : CacheRefs::standard());
  rep.bytesLeft = cacheBytes(rootName);
  return rep;
}

bool unpackForOpening(const UnpackRequest& r, UnpackResult& out, std::string* err) {
  const fs::path zp = fs::u8path(r.zipPath);
  const std::string stem = projectStemOf(r.zipPath);
  const fs::path cache = fs::u8path(r.cacheRoot.empty() ? defaultCacheRoot() : r.cacheRoot);
  const bool remote = remotefs::isRemote(r.zipPath);
  out.fromDrive = remote;
  out.inCache = true;
  auto stopped = [&] { return r.cancel && r.cancel->load(std::memory_order_relaxed); };
  auto cancelled = [&] {
    if (err) *err = remotefs::kCancelled;
    return false;
  };

  std::string openErr;
  std::unique_ptr<remotefs::ByteSource> src = r.open ? r.open(r.zipPath, &openErr) : remotefs::openFile(r.zipPath, &openErr);
  if (stopped()) return cancelled();
  if (!src) {
    // Not reachable: the last copy of an archive from a drive still opens.
    fs::path folder;
    Manifest m;
    std::error_code ec;
    bool found = false;
    if (remote && fs::is_directory(cache, ec)) {
      CacheGuard guard(cache);
      found = lastCopyOf(cache, r.zipPath, folder, m);
      if (found) out.use = holdLocked(folder);
    }
    if (found) {
      std::string chosen;
      if (!chooseProject(r, namedEntries(m.names), stem, chosen, out, err)) return false;
      out.projectFile = (folder / fs::u8path(chosen)).u8string();
      out.folder = folder.u8string();
      out.reused = out.offlineCopy = true;
      return true;
    }
    if (err) *err = openErr;
    return false;
  }

  // The stamp says which version this is (a reader of tests has none: the size alone then).
  uint64_t size = static_cast<uint64_t>(std::max<int64_t>(src->size(), 0));
  int64_t mtime = 0;
  {
    std::error_code ec;
    const auto t = fs::last_write_time(zp, ec);
    if (!ec) mtime = platform::fileTimeSeconds(t);
  }
  if (stopped()) return cancelled();

  const std::string versionKey = r.zipPath + "|" + std::to_string(size) + "|" + std::to_string(mtime);
  const std::string namePrefix = safeStem(stem) + "-";
  fs::path folder = cache / fs::u8path(namePrefix + fnvHex(versionKey));
  auto sameVersion = [&](const Manifest& m) { return m.source == r.zipPath && m.size == size && m.mtime == mtime; };
  {
    // The copy that is there, claimed while the guard says no one is removing it.
    Manifest m;
    bool whole = false;
    {
      std::error_code ec;
      if (fs::is_directory(folder, ec)) {
        CacheGuard guard(cache);
        whole = readManifest(folder, m) && sameVersion(m) && copyIsWhole(folder, m);
        if (whole) {
          out.use = holdLocked(folder);
          fs::last_write_time(folder / kManifest, fs::file_time_type::clock::now(), ec);  // used now: the last to be trimmed
        }
      }
    }
    if (whole) {
      std::string chosen;
      if (!chooseProject(r, namedEntries(m.names), stem, chosen, out, err)) return false;
      out.projectFile = (folder / fs::u8path(chosen)).u8string();
      out.folder = folder.u8string();
      out.reused = true;
      return true;
    }
  }

  std::vector<ZipEntry> all;
  if (!readZipDirectory(*src, all, err)) return false;
  if (stopped()) return cancelled();
  std::vector<ZipEntry> entries;
  for (auto& e : all) {
    if (isJunkEntry(e.name)) out.junkSkipped.push_back(e.name);
    else entries.push_back(std::move(e));
  }
  std::string chosen;
  if (!chooseProject(r, entries, stem, chosen, out, err)) return false;

  // Room for it, before a byte is written (or downloaded).
  uint64_t packed = 0, unpacked = 0;
  for (const auto& e : entries) {
    packed += e.packed;
    unpacked += e.size;
  }
  if (r.onTotal) r.onTotal(packed);
  std::error_code ec;
  fs::create_directories(cache, ec);
  auto spaceOk = [&] {
    std::error_code e2;
    const fs::space_info sp = fs::space(cache, e2);
    return e2 || sp.available >= unpacked + (64u << 20);
  };
  if (!spaceOk()) trimCache(cache.u8string(), 0, "", 0, r.refs);
  if (!spaceOk()) {
    char b[160];
    std::snprintf(b, sizeof b, "not enough disk space to unpack it: it needs %.1f GB. Free some space and try again.",
                  static_cast<double>(unpacked) / (1024.0 * 1024.0 * 1024.0));
    if (err) *err = b;
    return false;
  }

  // A copy is unpacked under a working name, with its lock held from before the first byte, and takes
  // its real name when whole.
  static std::atomic<unsigned> serial{0};
  const fs::path work(folder.u8string() + kPartTag + std::to_string(platform::processId()) + "-" + std::to_string(serial.fetch_add(1)));
  platform::FileLock workLock;
  {
    CacheGuard guard(cache);
    if (guard.ok()) workLock.tryLock(lockFile(work).u8string(), platform::FileLock::Mode::Exclusive);
  }
  auto forgetWorkLock = [&] {
    workLock.unlock();
    std::error_code e2;
    CacheGuard guard(cache);
    if (guard.ok()) fs::remove(lockFile(work), e2);
  };
  auto dropWork = [&] {
    std::error_code e2;
    fs::remove_all(work, e2);
    forgetWorkLock();
  };
  ZipExtractOptions zo;
  zo.watch.cancel = r.cancel;
  zo.watch.onBytes = r.onBytes;
  const size_t count = entries.size();
  if (r.onEntry) zo.onEntry = [&](const ZipEntry& e, size_t i) { r.onEntry(baseName(e.name), i, count); };
  std::vector<std::string> files;
  if (!extractZipEntries(*src, entries, work.u8string(), &files, zo, err)) {
    dropWork();
    return false;
  }
  src.reset();
  Manifest m;
  m.source = r.zipPath;
  m.size = size;
  m.mtime = mtime;
  for (const auto& e : entries)
    if (!e.dir()) m.names.push_back(e.name);
  bool marked = writeManifest(work, m);
  if (marked && !remote) marked = static_cast<bool>(std::ofstream(work / kLocalSource, std::ios::binary));
  if (!marked) {
    dropWork();
    if (err) *err = fileWriteError((work / kManifest).u8string(), ENOSPC);
    return false;
  }
  bool renamed = false, already = false;
  fs::path damaged;
  {
    CacheGuard guard(cache);
    Manifest have;
    if (readManifest(folder, have) && sameVersion(have) && copyIsWhole(folder, have)) {
      already = true;  // (another Gridline finished the same archive first)
      out.use = holdLocked(folder);
    } else {
      if (fs::exists(folder, ec)) {  // a damaged earlier copy under this name
        fs::path trash;
        if (takeOutOfUse(folder, trash) == Taken::Yes) damaged = trash;
        else folder = cache / fs::u8path(namePrefix + fnvHex(versionKey + "|" + work.filename().u8string()));  // someone still reads it
      }
      fs::rename(work, folder, ec);
      renamed = !ec;
      if (renamed) out.use = holdLocked(folder);
    }
  }
  if (!damaged.empty()) deleteTrash(damaged);
  if (already) {
    dropWork();
  } else if (!renamed) {
    const int code = ec.value();
    dropWork();
    if (err) *err = fileWriteError(folder.u8string(), code);
    return false;
  } else {
    forgetWorkLock();
  }
  trimCache(cache.u8string(), r.cacheLimit, folder.u8string(), 3600, r.refs);
  out.projectFile = (folder / fs::u8path(chosen)).u8string();
  out.folder = folder.u8string();
  return true;
}

}  // namespace gl::archive
