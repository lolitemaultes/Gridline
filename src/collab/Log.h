// Gridline - J-111 collaboration: signed op batches, version vectors, the
// crash-safe op log on disk (append-only, CRC per record, snapshot
// compaction by atomic rename) and the message framing of the protocol.
#pragma once
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#include "Crdt.h"
#include "Crypto.h"

namespace gl::collab {

// site -> highest contiguous batch seq seen.
using VersionVector = std::map<SiteId, uint64_t>;
Json vvToJson(const VersionVector& v);
VersionVector vvFromJson(const Json& j);
bool vvCovers(const VersionVector& have, const VersionVector& need, SiteId except = 0);
std::string siteHex(SiteId s);
SiteId siteFromHex(const std::string& s);

// One flush of one author: ops with consecutive stamps, signed.
struct Batch {
  SiteId site = 0;
  uint64_t seq = 0;
  VersionVector deps;     // what the author had seen (its own entry = seq - 1)
  std::string name;       // author's display name when made (logs, review)
  std::vector<Op> ops;
  std::string sig;        // EdDSA over signedText()
  uint64_t firstT() const { return ops.empty() ? 0 : ops.front().t; }
  uint64_t lastT() const { return ops.empty() ? 0 : ops.back().t; }
  std::string signedText() const;
  Json toJson() const;
  static bool fromJson(const Json& j, Batch& out);
  std::string text() const;  // compact JSON
  static bool parse(const std::string& text, Batch& out);
  void signWith(const crypto::Identity& id) { sig = crypto::sign(id, signedText()); }
  bool verify(const crypto::Key& signPublic) const { return crypto::verify(signPublic, signedText(), sig); }
};

uint32_t crc32(const void* data, size_t n);

// <project>/.gridline-collab: snapshot.json + oplog.<gen>.bin.
class OpLog {
 public:
  ~OpLog();
  // Opens (creating the folder); reads the snapshot (Null when none) and the
  // records of its log, truncating a torn or corrupt tail.
  bool open(const std::string& dir, Json& snapshot, std::vector<std::string>& records, std::string* err = nullptr);
  bool append(const std::string& payload);  // written and flushed (not yet synced)
  void sync();                              // fsync when something was appended
  // Compaction: writes `snapshot` (its "gen" set to the next generation)
  // atomically, starts the next log with `keep`, deletes the old log.
  bool compact(Json snapshot, const std::vector<std::string>& keep, std::string* err = nullptr);
  void close();
  uint64_t bytes() const { return bytes_; }
  size_t records() const { return records_; }
  int generation() const { return gen_; }
  const std::string& dir() const { return dir_; }
  bool isOpen() const { return f_ != nullptr; }
  int64_t truncatedBytes() const { return truncated_; }  // tail dropped at open (tests)

 private:
  std::string logPath(int gen) const;
  bool openLog(int gen, bool truncateAt, uint64_t at);
  std::string dir_;
  std::FILE* f_ = nullptr;
  int gen_ = 0;
  uint64_t bytes_ = 0;
  size_t records_ = 0;
  bool dirty_ = false;
  int64_t truncated_ = 0;
};

// Writes `text` to `path` via a synced temporary file and a rename.
bool writeFileDurable(const std::string& path, const std::string& text, std::string* err = nullptr);
bool readFileText(const std::string& path, std::string& out, size_t maxBytes = 512u << 20);

// ---- protocol messages: [u32 header length][header JSON][binary payload] ----
struct Message {
  Json head = Json::makeObject();
  std::string bin;
  std::string type() const { return head.text("m"); }
  std::string encode() const;
  static bool decode(const std::string& wire, Message& out);
};
inline constexpr int kProtocolVersion = 3;  // 2: membership records, removal / end notices (J-111e); 3: presence probes (J-146)
// Streams of a link: 0 = control + ops (first), 1 = bulk (snapshots, files, avatars).
inline constexpr int kStreamControl = 0;
inline constexpr int kStreamBulk = 1;
inline constexpr int kStreamUnreliable = 2;

}  // namespace gl::collab
