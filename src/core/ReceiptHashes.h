// Gridline - K-321: SHA-256 of the Track Receipt's files, cached per computer.
// The project keeps its trail by the hash of each file (ReceiptOrigin.h, Saved::names and
// Saved::origins). Collab copies are named by their hash already; any other file is hashed once
// and cached by path, size and modification time in <config>/receipt-hashes.json.
#pragma once
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace gl::receipt {

// The hash a file's name gives ("<64 hex>.<ext>", as collab names its copies); "" otherwise.
std::string hashFromName(const std::string& path);

class HashCache {
 public:
  explicit HashCache(std::string file);  // "" = in memory only
  // SHA-256 (lower-case hex) of the file's bytes: from its name, the cache, or read now; ""
  // when it cannot be read. Thread-safe.
  std::string get(const std::string& path);
  bool save();  // writes the cache file when something changed
  size_t hashed() const { return hashed_; }  // files read (not from the cache) since made
  // The cache of this computer, <config>/receipt-hashes.json.
  static HashCache& shared();

 private:
  struct Entry {
    uint64_t size = 0;
    int64_t mtime = 0;
    std::string hash;
  };
  std::string file_;
  std::map<std::string, Entry> entries_;
  bool loaded_ = false, dirty_ = false;
  size_t hashed_ = 0;
  std::mutex mutex_;
  void load();
};

}  // namespace gl::receipt
