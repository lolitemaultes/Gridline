// Gridline - J-111 collaboration: conflicts of offline edits.
//
// A conflict is two concurrent ops (neither author had seen the other) on the
// same target whose clock times are more than kConflictWindowMs apart: edits
// made while disconnected. Live co-editing is concurrent only within network
// latency and never prompts. Targets: the same property (set / set), delete vs
// edit (a delete and any op inside the deleted object), and the same object
// structurally changed by both (inserts / deletes / moves inside it).
//
// The queue is derived from the ops alone, so every peer computes the same
// one; it is kept in the snapshot so compaction does not lose it. Resolutions
// are signed admin ops in the replica (meta/resolutions).
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "Crdt.h"

namespace gl::collab {

using VersionVector = std::map<SiteId, uint64_t>;
struct Batch;

inline constexpr uint64_t kConflictWindowMs = 10000;

// One op as recorded for the review (who, when, what).
struct SideOp {
  SiteId site = 0;
  uint64_t seq = 0;
  uint32_t index = 0;
  uint64_t t = 0;
  OpKind kind = OpKind::Set;
  Path path;
  Json value, prev;
  std::string name;  // author's name snapshot
  std::string ref() const;  // "site:seq:index"
};

struct ConflictObject {
  std::string object;           // "song/channels/#5"
  std::vector<SideOp> ops;      // every op involved in a conflict on it, by time
  std::set<SiteId> sites() const;
};

// The sides of a conflict object: each author's involved ops, in time order.
struct ConflictSide {
  SiteId site = 0;
  std::string name;
  uint64_t firstT = 0, lastT = 0;
  std::vector<const SideOp*> ops;
};

// The object an op path belongs to ("song/<container>/<element>"; the
// project's name "meta/project/name"), "" for other paths outside the song.
std::string objectOf(const Path& p);
std::string joinPath(const Path& p);
Path splitPath(const std::string& s);

class ConflictTracker {
 public:
  // Called for every applied batch, in causal order.
  void onBatch(const Batch& b, const std::shared_ptr<const VersionVector>& deps);
  // Drops index entries whose batch every member has seen (see Session).
  void prune(const std::function<bool(SiteId site, uint64_t seq)>& stable);
  // Resolution records of the replica (meta/resolutions): refs that are settled.
  void setResolved(std::set<std::string> refs) { resolved_ = std::move(refs); }
  bool isResolved(const SideOp& op) const { return resolved_.count(op.ref()) > 0; }

  const std::map<std::string, ConflictObject>& all() const { return objects_; }
  // Objects with at least one unresolved op.
  std::vector<const ConflictObject*> open() const;
  std::vector<const ConflictObject*> settled() const;
  size_t openCount() const { return open().size(); }
  static std::vector<ConflictSide> sides(const ConflictObject& c);
  // The ops that make `keepSite`'s side current for `object`, given the
  // replica (only ops that change something). `next` stamps them.
  std::vector<Op> keepOps(const std::string& object, SiteId keepSite, const Replica& r, const std::function<uint64_t()>& next) const;

  Json toJson() const;
  void fromJson(const Json& j);
  size_t indexSize() const;
  void clear();

 private:
  struct Entry {
    SideOp op;
    std::shared_ptr<const VersionVector> deps;
  };
  void addConflict(const std::string& object, const SideOp& a, const SideOp& b);
  std::map<std::string, std::vector<Entry>> recent_;  // by object
  std::map<std::string, ConflictObject> objects_;
  std::map<std::string, std::set<std::string>> refsIn_;  // object -> refs already listed
  std::set<std::string> resolved_;
};

}  // namespace gl::collab
