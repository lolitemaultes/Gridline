// Gridline - J-111 collaboration: conflict detection and resolution ops. See Conflicts.h.
#include "Conflicts.h"
#include <algorithm>
#include "Log.h"

namespace gl::collab {

namespace {

constexpr char kSep = '\x1f';

bool isPrefix(const Path& a, const Path& b) {
  if (a.size() > b.size()) return false;
  return std::equal(a.begin(), a.end(), b.begin());
}

bool structural(OpKind k) { return k == OpKind::Ins || k == OpKind::Del || k == OpKind::Undel || k == OpKind::Mov; }

bool primSame(const Json& a, const Json& b) { return jsonCompact(a) == jsonCompact(b); }

bool conflicting(const SideOp& a, const SideOp& b) {
  if (a.kind == OpKind::Set && b.kind == OpKind::Set) return a.path == b.path && !primSame(a.value, b.value);
  if (a.kind == OpKind::Del && b.kind == OpKind::Del && a.path == b.path) return false;  // both deleted it
  if (a.kind == OpKind::Del && isPrefix(a.path, b.path)) return true;
  if (b.kind == OpKind::Del && isPrefix(b.path, a.path)) return true;
  if (a.kind == OpKind::Mov && b.kind == OpKind::Mov && a.path == b.path) return !primSame(a.value, b.value);
  return structural(a.kind) && structural(b.kind) && !(a.kind == OpKind::Mov && b.kind == OpKind::Mov);
}

uint64_t depOf(const VersionVector& v, SiteId s) {
  auto it = v.find(s);
  return it == v.end() ? 0 : it->second;
}

Json sideToJson(const SideOp& o) {
  Json j = Json::makeObject();
  j.object["a"] = Json::makeString(siteHex(o.site));
  j.object["q"] = Json::makeNumber(static_cast<double>(o.seq));
  j.object["i"] = Json::makeNumber(o.index);
  j.object["t"] = Json::makeString(siteHex(o.t));
  j.object["k"] = Json::makeNumber(static_cast<int>(o.kind));
  Json p = Json::makeArray();
  for (const auto& s : o.path) p.array.push_back(Json::makeString(s));
  j.object["p"] = std::move(p);
  j.object["v"] = o.value;
  j.object["r"] = o.prev;
  j.object["n"] = Json::makeString(o.name);
  return j;
}

bool sideFromJson(const Json& j, SideOp& o) {
  o.site = siteFromHex(j.text("a"));
  o.seq = static_cast<uint64_t>(j.num("q", 0));
  o.index = static_cast<uint32_t>(j.num("i", 0));
  o.t = siteFromHex(j.text("t"));
  const int k = static_cast<int>(j.num("k", 1));
  if (k < 1 || k > 5 || !o.site) return false;
  o.kind = static_cast<OpKind>(k);
  o.path.clear();
  if (const Json* p = j.find("p"); p && p->is(Json::Type::Array))
    for (const auto& s : p->array) o.path.push_back(s.str);
  if (const Json* v = j.find("v")) o.value = *v;
  if (const Json* r = j.find("r")) o.prev = *r;
  o.name = j.text("n");
  return true;
}

}  // namespace

std::string SideOp::ref() const { return siteHex(site) + ":" + std::to_string(seq) + ":" + std::to_string(index); }

std::set<SiteId> ConflictObject::sites() const {
  std::set<SiteId> s;
  for (const auto& o : ops) s.insert(o.site);
  return s;
}

std::string joinPath(const Path& p) {
  std::string s;
  for (size_t i = 0; i < p.size(); ++i) {
    if (i) s += kSep;
    s += p[i];
  }
  return s;
}

Path splitPath(const std::string& s) {
  Path p;
  std::string cur;
  for (char c : s) {
    if (c == kSep) {
      p.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!s.empty()) p.push_back(cur);
  return p;
}

std::string objectOf(const Path& p) {
  // the project's shared name is reviewed like a property of the song (J-111f)
  if (p.size() == 3 && p[0] == "meta" && p[1] == "project" && p[2] == "name") return joinPath(p);
  if (p.empty() || p[0] != "song") return {};
  if (p.size() >= 3) return joinPath(Path(p.begin(), p.begin() + 3));
  return joinPath(p);
}

void ConflictTracker::clear() {
  recent_.clear();
  objects_.clear();
  refsIn_.clear();
  resolved_.clear();
}

void ConflictTracker::onBatch(const Batch& b, const std::shared_ptr<const VersionVector>& deps) {
  for (size_t i = 0; i < b.ops.size(); ++i) {
    const Op& op = b.ops[i];
    const std::string obj = objectOf(op.path);
    if (obj.empty()) continue;
    SideOp so;
    so.site = b.site;
    so.seq = b.seq;
    so.index = static_cast<uint32_t>(i);
    so.t = op.t;
    so.kind = op.kind;
    so.path = op.path;
    so.value = op.kind == OpKind::Mov ? Json::makeString(op.pos) : (op.kind == OpKind::Set ? op.value : Json());
    so.prev = op.prev;
    so.name = b.name;
    auto& list = recent_[obj];
    for (const auto& e : list) {
      if (e.op.site == so.site) continue;
      const bool concurrent = depOf(*deps, e.op.site) < e.op.seq && depOf(*e.deps, so.site) < so.seq;
      if (!concurrent) continue;
      const uint64_t ma = Hlc::msOf(e.op.t), mb = Hlc::msOf(so.t);
      const uint64_t gap = ma > mb ? ma - mb : mb - ma;
      if (gap <= kConflictWindowMs) continue;
      if (conflicting(e.op, so)) addConflict(obj, e.op, so);
    }
    list.push_back(Entry{std::move(so), deps});
  }
}

void ConflictTracker::addConflict(const std::string& object, const SideOp& a, const SideOp& b) {
  auto& c = objects_[object];
  c.object = object;
  auto& refs = refsIn_[object];
  for (const SideOp* o : {&a, &b})
    if (refs.insert(o->ref()).second) c.ops.push_back(*o);
  std::sort(c.ops.begin(), c.ops.end(), [](const SideOp& x, const SideOp& y) { return x.t != y.t ? x.t < y.t : x.site < y.site; });
}

void ConflictTracker::prune(const std::function<bool(SiteId, uint64_t)>& stable) {
  for (auto it = recent_.begin(); it != recent_.end();) {
    auto& v = it->second;
    v.erase(std::remove_if(v.begin(), v.end(), [&](const Entry& e) { return stable(e.op.site, e.op.seq); }), v.end());
    it = v.empty() ? recent_.erase(it) : std::next(it);
  }
}

std::vector<const ConflictObject*> ConflictTracker::open() const {
  std::vector<const ConflictObject*> out;
  for (const auto& [k, c] : objects_) {
    bool anyOpen = false;
    for (const auto& o : c.ops)
      if (!resolved_.count(o.ref())) anyOpen = true;
    if (anyOpen) out.push_back(&c);
  }
  return out;
}

std::vector<const ConflictObject*> ConflictTracker::settled() const {
  std::vector<const ConflictObject*> out;
  for (const auto& [k, c] : objects_) {
    bool allDone = !c.ops.empty();
    for (const auto& o : c.ops)
      if (!resolved_.count(o.ref())) allDone = false;
    if (allDone) out.push_back(&c);
  }
  return out;
}

std::vector<ConflictSide> ConflictTracker::sides(const ConflictObject& c) {
  std::map<SiteId, ConflictSide> m;
  for (const auto& o : c.ops) {
    auto& s = m[o.site];
    s.site = o.site;
    if (!o.name.empty()) s.name = o.name;
    if (s.ops.empty()) s.firstT = o.t;
    s.lastT = o.t;
    s.ops.push_back(&o);
  }
  std::vector<ConflictSide> out;
  for (auto& [k, s] : m) out.push_back(std::move(s));
  std::sort(out.begin(), out.end(), [](const ConflictSide& a, const ConflictSide& b) { return a.firstT < b.firstT; });
  return out;
}

std::vector<Op> ConflictTracker::keepOps(const std::string& object, SiteId keepSite, const Replica& r,
                                         const std::function<uint64_t()>& next) const {
  std::vector<Op> out;
  auto it = objects_.find(object);
  if (it == objects_.end()) return out;
  std::vector<const SideOp*> use;
  for (const auto& o : it->second.ops)
    if (!resolved_.count(o.ref())) use.push_back(&o);
  if (use.empty())
    for (const auto& o : it->second.ops) use.push_back(&o);  // restoring a settled conflict
  // group by (path, register class)
  std::map<std::pair<std::string, int>, std::vector<const SideOp*>> groups;
  for (const SideOp* o : use) {
    const int cls = o->kind == OpKind::Set ? 0 : o->kind == OpKind::Mov ? 1 : 2;
    groups[{joinPath(o->path), cls}].push_back(o);
  }
  for (auto& [key, ops] : groups) {
    std::sort(ops.begin(), ops.end(), [](const SideOp* a, const SideOp* b) { return a->t < b->t; });
    const SideOp* lastKeep = nullptr;
    for (const SideOp* o : ops)
      if (o->site == keepSite) lastKeep = o;
    const SideOp* first = ops.front();
    const Path path = splitPath(key.first);
    const Node* n = r.find(path);
    Op op;
    op.path = path;
    if (key.second == 0) {
      const Json target = lastKeep ? lastKeep->value : first->prev;
      if (target.is(Json::Type::Null) && !lastKeep) continue;  // nothing known to go back to
      if (n && primSame(n->val, target)) continue;
      op.kind = OpKind::Set;
      op.value = target;
      op.prev = n ? n->val : Json();
    } else if (key.second == 1) {
      const std::string target = lastKeep ? lastKeep->value.str : first->prev.str;
      if (target.empty() || (n && n->pos == target)) continue;
      op.kind = OpKind::Mov;
      op.pos = target;
      op.prev = Json::makeString(n ? n->pos : std::string());
    } else {
      const bool target = lastKeep ? lastKeep->kind != OpKind::Del : first->kind != OpKind::Ins && first->kind != OpKind::Undel;
      const bool now = r.visible(path);
      if (now == target || !n) continue;
      op.kind = target ? OpKind::Undel : OpKind::Del;
    }
    op.t = next();
    out.push_back(std::move(op));
  }
  return out;
}

Json ConflictTracker::toJson() const {
  Json j = Json::makeObject();
  Json objs = Json::makeArray();
  for (const auto& [k, c] : objects_) {
    Json o = Json::makeObject();
    o.object["o"] = Json::makeString(k);
    Json ops = Json::makeArray();
    for (const auto& s : c.ops) ops.array.push_back(sideToJson(s));
    o.object["ops"] = std::move(ops);
    objs.array.push_back(std::move(o));
  }
  j.object["objects"] = std::move(objs);
  Json rec = Json::makeArray();
  for (const auto& [k, list] : recent_)
    for (const auto& e : list) {
      Json x = sideToJson(e.op);
      x.object["d"] = vvToJson(*e.deps);
      rec.array.push_back(std::move(x));
    }
  j.object["recent"] = std::move(rec);
  return j;
}

void ConflictTracker::fromJson(const Json& j) {
  clear();
  if (const Json* objs = j.find("objects"); objs && objs->is(Json::Type::Array))
    for (const auto& o : objs->array) {
      ConflictObject c;
      c.object = o.text("o");
      if (const Json* ops = o.find("ops"); ops && ops->is(Json::Type::Array))
        for (const auto& s : ops->array) {
          SideOp so;
          if (sideFromJson(s, so)) {
            refsIn_[c.object].insert(so.ref());
            c.ops.push_back(std::move(so));
          }
        }
      if (!c.object.empty()) objects_[c.object] = std::move(c);
    }
  if (const Json* rec = j.find("recent"); rec && rec->is(Json::Type::Array))
    for (const auto& x : rec->array) {
      Entry e;
      if (!sideFromJson(x, e.op)) continue;
      e.deps = std::make_shared<const VersionVector>(vvFromJson(x.find("d") ? *x.find("d") : Json()));
      const std::string obj = objectOf(e.op.path);
      if (!obj.empty()) recent_[obj].push_back(std::move(e));
    }
}

size_t ConflictTracker::indexSize() const {
  size_t n = 0;
  for (const auto& [k, v] : recent_) n += v.size();
  return n;
}

}  // namespace gl::collab
