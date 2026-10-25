// Gridline - J-111 collaboration: the replicated project document.
//
// A tree of nodes mirroring the project JSON (core/Project.h), each node with
// last-writer-wins registers (presence, value, list position, clear). Every
// register is a max register over a total order of stamps (hybrid logical
// clock + site id), so any set of ops applied in any order, any number of
// times, gives the same tree. Local changes become ops by diffing the
// document's JSON against the tree (Replica::diff). Design: DESIGN.md / docs.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "../core/Json.h"

namespace gl::collab {

using SiteId = uint64_t;

// (hybrid logical time, site): unique per op, totally ordered.
struct Stamp {
  uint64_t t = 0;
  SiteId site = 0;
  bool operator<(const Stamp& o) const { return t != o.t ? t < o.t : site < o.site; }
  bool operator>(const Stamp& o) const { return o < *this; }
  bool operator==(const Stamp& o) const { return t == o.t && site == o.site; }
  bool operator!=(const Stamp& o) const { return !(*this == o); }
  bool operator<=(const Stamp& o) const { return !(o < *this); }
  bool operator>=(const Stamp& o) const { return !(*this < o); }
  bool zero() const { return t == 0 && site == 0; }
};

// Hybrid logical clock: 48 bits of wall milliseconds, 16 bits of counter.
class Hlc {
 public:
  // Milliseconds since 1970; replaceable (tests).
  std::function<uint64_t()> wallMs;
  Hlc();
  uint64_t tick();                  // a new local event
  void observe(uint64_t remote);    // a remote event was received
  uint64_t last() const { return last_; }
  void restore(uint64_t t) { if (t > last_) last_ = t; }
  static uint64_t msOf(uint64_t t) { return t >> 16; }
  static uint64_t fromMs(uint64_t ms) { return ms << 16; }

 private:
  uint64_t last_ = 0;
};

using Path = std::vector<std::string>;

enum class OpKind : uint8_t {
  Set = 1,    // a primitive value changed in place
  Ins = 2,    // a map key / list element appears with this content (clears older content)
  Del = 3,    // removed (content kept underneath as a tombstone)
  Undel = 4,  // removed node shown again with its content (undo of a delete, restore)
  Mov = 5,    // list element order key
};

struct Op {
  OpKind kind = OpKind::Set;
  uint64_t t = 0;   // hybrid logical time (the stamp's site is the batch author)
  Path path;
  Json value;       // Set: new value; Ins: content
  std::string pos;  // Ins (list element) / Mov: order key
  Json prev;        // Set: the replaced value; Mov: the replaced key (string); else null
};

std::string opKindName(OpKind k);
Json opToJson(const Op& op);
bool opFromJson(const Json& j, Op& out);

struct Node {
  enum class Kind : uint8_t { Unknown = 0, Value, Map, List };
  Kind kind = Kind::Unknown;
  Stamp kindS;
  bool pres = false;
  Stamp presS;
  Stamp clearS;
  Json val;
  Stamp valS;
  std::string pos;
  Stamp posS;
  std::map<std::string, std::unique_ptr<Node>> kids;
};

// Order keys between two keys ("" = open end); strictly increasing, deterministic.
std::string keyBetween(const std::string& a, const std::string& b);
std::vector<std::string> keysBetween(const std::string& a, const std::string& b, size_t n);
// Element id of a list element made by op stamp `s` (index k inside an inserted array: suffix).
std::string elementId(const Stamp& s, int k = -1);

class Replica {
 public:
  Replica();
  Replica(const Replica&) = delete;
  Replica& operator=(const Replica&) = delete;

  // Applies one op of `site`. Returns false for a malformed op (ignored).
  bool apply(const Op& op, SiteId site);

  // The visible JSON at `path` (Null when absent / hidden).
  Json materialize(const Path& path) const;
  bool visible(const Path& path) const;
  const Node* find(const Path& path) const;

  // Ops turning the visible subtree at `path` into `json`. `next()` returns the
  // stamp time of each new op (site = `site`). Not applied: call apply().
  void diff(const Path& path, const Json& json, SiteId site, const std::function<uint64_t()>& next, std::vector<Op>& out) const;

  // Whole state with stamps (snapshots) and back.
  Json toJson() const;
  bool fromJson(const Json& j);
  void clear();
  // Number of nodes (tests, limits).
  size_t nodeCount() const;
  // Drops tombstoned subtrees whose removal stamp is older than `before` (compaction).
  size_t collectTombstones(const Stamp& before);

 private:
  Node root_;
};

// Visible children of a node in order (lists: by position key, then element id).
std::vector<std::pair<const std::string*, const Node*>> visibleKids(const Node& n, const Stamp& anc);
bool nodeVisible(const Node& n, const Stamp& anc);
Json materializeNode(const Node& n, const Stamp& anc);

}  // namespace gl::collab
