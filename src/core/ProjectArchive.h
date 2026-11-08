// Gridline: opening a zipped project (K-42, K-45). Which file in the archive is the project, and the
// cache on this computer where every archive is unpacked; nothing is ever written beside an archive.
//
// An unchanged archive opens from its earlier copy without reading it again, and when a drive is
// offline the last copy opens with a note. What happens to a copy:
//  * While a running Gridline uses it, that process holds a lock on it (CopyHold). The lock lives in
//    an open file, so a crash cannot leave it behind. Nothing removes a held copy.
//  * When its project closes, a copy of an archive on a local disk is deleted (sweepCache): the
//    archive is right there. A copy from a web or network drive stays, inside the size limit, as the
//    offline copy that also avoids a slow read (closedCopyStays: the one rule).
//  * A copy a saved project points at is marked (pinUnpacked) and stays. So does one a crash-recovery
//    file points at; one a backup points at stays while that backup is recent (CacheRefs). Trimming
//    takes those backed-up copies last.
//  * Whatever cannot be decided stays.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "Platform.h"
#include "RemoteFs.h"
#include "Song.h"
#include "Zip.h"

namespace gl::archive {

// What an archive holds besides the project and never unpacks: macOS resource forks, folder
// settings, thumbnails.
bool isJunkEntry(const std::string& name);

// The projects in an archive, best first. Gridline projects outrank FL Studio projects, which
// outrank other .json files (those only count when nothing else is there); copies in backup folders
// only count when nothing else is there. Then the name of the archive, then the shallowest.
struct ProjectPick {
  std::vector<std::string> names;  // entry names, best first
  bool ambiguous = false;          // several equal candidates: the person should choose
};
ProjectPick pickProject(const std::vector<ZipEntry>& entries, const std::string& zipStem);

// A running Gridline's claim on a copy in the cache: released when the last reference goes (the copy
// itself is removed later, by sweepCache, when no one else needs it).
struct CopyUse {
  std::string folder;
  platform::FileLock lock;
};
using CopyHold = std::shared_ptr<CopyUse>;

// Folders whose files can point into the cache and keep a copy alive.
struct CacheRefs {
  std::vector<std::string> recovery;  // crash recovery files: they keep a copy however old they are
  std::vector<std::string> backups;   // timed backups: they keep a copy for kBackupRefDays
  // The folders of this computer's configuration: <config>/gridline/recovery and .../Backup.
  // A crash recovery copy written there is seen by itself; recovery written elsewhere can mark its
  // folders with pinUnpacked.
  static CacheRefs standard();
};
inline constexpr int kBackupRefDays = 14;
inline constexpr uint64_t kCacheLimit = 16ull << 30;  // the cache's size limit

struct UnpackRequest {
  std::string zipPath;
  std::string cacheRoot;                        // "" = defaultCacheRoot()
  uint64_t cacheLimit = kCacheLimit;            // unpinned copies are trimmed down to this
  const std::atomic<bool>* cancel = nullptr;    // checked between chunks; a stuck read simply never returns
  std::function<void(uint64_t bytes)> onBytes;  // bytes read since the last call
  std::function<void(uint64_t total)> onTotal;  // bytes to read, once known
  std::function<void(const std::string& entry, size_t index, size_t count)> onEntry;
  // Several equal projects: the index to open (the first is the suggestion), or -1 to stop.
  // Unset: the first.
  std::function<int(const std::vector<std::string>& names)> choose;
  std::function<std::unique_ptr<remotefs::ByteSource>(const std::string& path, std::string* err)> open;  // tests; default remotefs::openFile
  const CacheRefs* refs = nullptr;              // tests; default CacheRefs::standard()
};

struct UnpackResult {
  std::string projectFile;   // the project inside the unpacked folder
  std::string folder;        // the unpacked folder
  bool inCache = false;      // in the local cache (the project is not saved over; Save asks where)
  bool fromDrive = false;    // the archive is on a web or network drive
  CopyHold use;              // keep it while the project is open; empty: locks do not work here
  bool reused = false;       // the earlier copy, nothing was read
  bool offlineCopy = false;  // the archive could not be reached: the last copy opened
  size_t otherProjects = 0;  // other projects in the archive
  std::vector<std::string> junkSkipped;
};

std::string defaultCacheRoot();
// A closed project's copy stays when it came from a web or network drive; a local one goes. The whole
// rule: change it here. (Copies made before this rule existed count as from a drive.)
inline constexpr bool closedCopyStays(bool fromDrive) { return fromDrive; }
// Unpacks the archive for opening. false + `err` on failure (remotefs::kCancelled when stopped), and
// nothing half-unpacked is left in the cache. Blocks: call it from a worker.
bool unpackForOpening(const UnpackRequest& r, UnpackResult& out, std::string* err);

// K-76: a copy made from a file rather than unpacked from an archive (a tracker module's samples):
// the same cache, manifest, lock and rules as an unpacked archive. `write` fills the folder it is
// given and lists the files it wrote (relative); an unchanged source finds its earlier copy whole
// and writes nothing. Blocks: call it from a worker.
struct MakeRequest {
  std::string sourcePath;
  std::string cacheRoot;                        // "" = defaultCacheRoot()
  uint64_t size = 0;                            // the source's size (with its time: which version)
  uint64_t cacheLimit = kCacheLimit;
  std::function<bool(const std::string& folder, std::vector<std::string>& names, std::string* err)> write;
  const CacheRefs* refs = nullptr;              // tests; default CacheRefs::standard()
};
bool makeCopy(const MakeRequest& r, UnpackResult& out, std::string* err);
// The song refers to files in the cache (it was saved from a project opened out of an archive):
// marks those folders so nothing removes them. Returns how many folders it marked.
int pinUnpacked(const Song& song, const std::string& cacheRoot = "");
// The folders in the cache `song` refers to, without touching the disk.
std::vector<std::string> copiesUsedBy(const Song& song, const std::string& cacheRoot = "");
// Claims a copy that is there. Empty when it is not there or its lock cannot be had.
CopyHold holdCopy(const std::string& folder, const std::string& cacheRoot = "");
// K-47: keeps the copies `song` refers to for something that outlives the project they came from, the
// clipboard for one. As long as the holds live nothing removes them. Where a lock cannot be had the
// copy is marked (pinUnpacked) instead, so it stays until the person clears it: the sound never goes.
std::vector<CopyHold> holdCopiesOf(const Song& song, const std::string& cacheRoot = "");
// Removes copies, oldest first, until the cache holds at most `limit` bytes. Copies in use, marked
// folders, copies crash recovery points at, `keep` and folders used in the last `minAgeSeconds` stay;
// copies a recent backup points at go last. Returns how many folders went.
int trimCache(const std::string& cacheRoot, uint64_t limit, const std::string& keep = "", int64_t minAgeSeconds = 3600, const CacheRefs* refs = nullptr);

// What a sweep or a clear did.
struct CacheReport {
  int removed = 0;       // copies gone and half-unpacked folders
  uint64_t freed = 0;
  int keptOpen = 0;      // in use by a running Gridline
  int keptSaved = 0;     // a saved project points at it
  int keptRecovery = 0;  // a crash recovery file or a recent backup points at it
  int keptDrive = 0;     // from a web or network drive (sweep only: the offline copy)
  int keptBusy = 0;      // could not be removed now (Windows: a file in it is open); tried again next start
  int keptUnknown = 0;   // could not be decided, or not a folder Gridline made
  uint64_t bytesLeft = 0;
  int kept() const { return keptOpen + keptSaved + keptRecovery + keptDrive + keptBusy + keptUnknown; }
};

// The total size of the cache.
uint64_t cacheBytes(const std::string& cacheRoot = "");
// After a project closed, and at startup for what a crash left: removes copies of local archives,
// half-unpacked folders and interrupted removals nothing needs, then trims to `limit`. Slow: call it
// from a worker. Touches nothing outside the cache's root.
CacheReport sweepCache(const std::string& cacheRoot = "", uint64_t limit = kCacheLimit, const CacheRefs* refs = nullptr);
// Clear cache: as the sweep, and copies from drives go too.
CacheReport clearCache(const std::string& cacheRoot = "", const CacheRefs* refs = nullptr);

}  // namespace gl::archive
