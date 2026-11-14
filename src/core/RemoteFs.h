// Gridline: files on mounted web and network drives (Google Drive, Dropbox, OneDrive, iCloud,
// rclone, gvfs, SMB, NFS, ...). A read there can download, stall for minutes or fail, a folder
// listing can be a network call, and a mount can stop answering altogether, so nothing on such a
// path may run on the interface thread and nothing may walk it.
//
//   isRemote    never touches the drive: the mount table, cached, and the path's name. Safe on the
//               interface thread.
//   ByteSource  reads a file in chunks from any thread, 64-bit offsets, with the reason on failure.
//   probe/list  an off-thread stat and folder listing. They return what is known and never wait;
//               a stuck mount leaves its thread stuck, not the caller.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gl::remotefs {

// The path is on a web or network drive. Does not contact the drive.
bool isRemote(const std::string& path);
// Tests and dev checks: these prefixes count as remote. Also read from GRIDLINE_DEV_REMOTE_PATHS
// (separated by the system's path list separator).
void setTestPrefixes(std::vector<std::string> prefixes);
// The decision for a file system name and a path, without the mount table (tests).
bool remoteFsType(const std::string& type);
bool remoteByName(const std::string& path);

// A readable file. readAt returns the bytes read (0 at the end of the file), or -1 with `err` set.
class ByteSource {
 public:
  virtual ~ByteSource() = default;
  virtual int64_t size() const = 0;
  virtual int64_t readAt(uint64_t offset, void* out, size_t n, std::string* err) = 0;
};
std::unique_ptr<ByteSource> openFile(const std::string& path, std::string* err);
// The message for a read that failed: the file by its name and what to check.
std::string readError(const std::string& path, int errnum);

// Reports bytes as they arrive (from the reading thread) and says whether to go on.
struct ReadWatch {
  const std::atomic<bool>* cancel = nullptr;
  std::function<void(uint64_t bytes)> onBytes;  // bytes since the last call
  bool cancelled() const { return cancel && cancel->load(std::memory_order_relaxed); }
};
inline constexpr const char* kCancelled = "cancelled";  // the `err` of a read stopped by `cancel`
inline constexpr size_t kReadChunk = 256 * 1024;
// The whole file into memory, chunk by chunk.
bool readAll(ByteSource& s, std::vector<uint8_t>& out, const ReadWatch& w, std::string* err);

// What is known about a path, from a stat on another thread. known = false until it has answered
// (and while a mount does not answer). A later call after `maxAge` seconds starts a new stat and
// goes on returning the last answer meanwhile.
struct Probe {
  bool known = false, exists = false, isDir = false;
};
Probe probe(const std::string& path, double maxAge = 10.0);
// Blocks at most `ms` for a first answer (for a click that needs one).
Probe probeWithin(const std::string& path, int ms);

struct DirEntry {
  std::string name;  // file name, UTF-8
  bool isDir = false;
  bool hidden = false;   // a dot name (listed only with kListHidden)
  int64_t size = -1;     // bytes (files, with kListDetails), -1 = not known
  int64_t mtime = 0;     // seconds since 1970 (with kListDetails), 0 = not known
};
// listNow / listAsync options: hidden entries too, and each entry's size and time (a stat each).
inline constexpr unsigned kListHidden = 1, kListDetails = 2;
struct Listing {
  bool ok = false;                // the folder could be read
  int errnum = 0;                 // when not ok
  std::vector<DirEntry> entries;  // at most kMaxListed, in the order read; hidden files left out
};
inline constexpr size_t kMaxListed = 20000;
// One folder, on this thread. Counted by walksStarted().
Listing listNow(const std::string& dir, unsigned flags = 0);
// The folder's last listing, from another thread. `listing` is null until the first has finished;
// `pending` is true while one runs, for `waited` seconds so far.
struct DirState {
  std::shared_ptr<const Listing> listing;
  bool pending = false;
  double waited = 0;
};
DirState listAsync(const std::string& dir, double maxAge = 5.0, unsigned flags = 0);
void forgetListings();

// How many folder reads Gridline has started (listNow, and every recursive walk through
// countWalk). Opening a project never moves it (the tests hold it to that).
uint64_t walksStarted();
void countWalk();

}  // namespace gl::remotefs
