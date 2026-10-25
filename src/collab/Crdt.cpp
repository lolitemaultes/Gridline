// Gridline - J-111 collaboration: replicated document. See Crdt.h.
#include "Crdt.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <set>

namespace gl::collab {

namespace {

const char kDigits[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
constexpr int kBase = 62;

int digitOf(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'Z') return 10 + (c - 'A');
  if (c >= 'a' && c <= 'z') return 36 + (c - 'a');
  return 0;
}

std::string hex(uint64_t v) {
  char buf[24];
  std::snprintf(buf, sizeof buf, "%llx", static_cast<unsigned long long>(v));
  return buf;
}

uint64_t unhex(const std::string& s, bool* ok = nullptr) {
  uint64_t v = 0;
  bool good = !s.empty() && s.size() <= 16;
  for (char c : s) {
    int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? 10 + c - 'a' : (c >= 'A' && c <= 'F') ? 10 + c - 'A' : -1;
    if (d < 0) {
      good = false;
      break;
    }
    v = (v << 4) | static_cast<uint64_t>(d);
  }
  if (ok) *ok = good;
  return good ? v : 0;
}

std::string stampText(const Stamp& s) { return hex(s.t) + ":" + hex(s.site); }
Stamp stampFrom(const std::string& s) {
  Stamp r;
  const size_t c = s.find(':');
  if (c == std::string::npos) return r;
  r.t = unhex(s.substr(0, c));
  r.site = unhex(s.substr(c + 1));
  return r;
}

Node::Kind kindOf(const Json& j) {
  switch (j.type) {
    case Json::Type::Object: return Node::Kind::Map;
    case Json::Type::Array: return Node::Kind::List;
    default: return Node::Kind::Value;
  }
}

bool primEq(const Json& a, const Json& b) {
  if (a.type != b.type) return false;
  switch (a.type) {
    case Json::Type::Null: return true;
    case Json::Type::Bool: return a.boolean == b.boolean;
    case Json::Type::Number: return a.number == b.number;
    case Json::Type::String: return a.str == b.str;
    default: return false;
  }
}

Stamp maxS(const Stamp& a, const Stamp& b) { return a < b ? b : a; }

Node* child(Node& n, const std::string& k) {
  auto& p = n.kids[k];
  if (!p) p = std::make_unique<Node>();
  return p.get();
}

// Writes `j` as the content of `n` at stamp `s` (an insert).
void writeContent(Node& n, const Json& j, const Stamp& s) {
  const Node::Kind k = kindOf(j);
  if (s > n.kindS) {
    n.kind = k;
    n.kindS = s;
  }
  if (s > n.presS) {
    n.pres = true;
    n.presS = s;
  }
  n.clearS = maxS(n.clearS, s);
  switch (k) {
    case Node::Kind::Value:
      if (s > n.valS) {
        n.val = j;
        n.valS = s;
      }
      break;
    case Node::Kind::Map:
      for (const auto& [key, v] : j.object) writeContent(*child(n, key), v, s);
      break;
    case Node::Kind::List: {
      const auto keys = keysBetween("", "", j.array.size());
      std::set<std::string> used;  // ids taken by this array (a repeated id gets a derived one)
      for (size_t i = 0; i < j.array.size(); ++i) {
        const Json& e = j.array[i];
        std::string id;
        if (const Json* jid = e.is(Json::Type::Object) ? e.find("id") : nullptr; jid && jid->is(Json::Type::Number))
          id = "#" + std::to_string(static_cast<long long>(jid->number));
        if (!id.empty() && !used.insert(id).second) id.clear();
        if (id.empty()) id = elementId(s, static_cast<int>(i));
        Node* c = child(n, id);
        writeContent(*c, e, s);
        if (s > c->posS) {
          c->pos = keys[i];
          c->posS = s;
        }
      }
      break;
    }
    case Node::Kind::Unknown: break;
  }
}

bool nodeEqJson(const Node& n, const Stamp& anc, const Json& j) {
  if (n.kind != kindOf(j)) return false;
  switch (n.kind) {
    case Node::Kind::Value: return primEq(n.val, j);
    case Node::Kind::Map: {
      const Stamp a = maxS(anc, n.clearS);
      size_t count = 0;
      for (const auto& [k, c] : n.kids) {
        if (!nodeVisible(*c, a)) continue;
        ++count;
        const Json* v = j.find(k);
        if (!v || !nodeEqJson(*c, a, *v)) return false;
      }
      return count == j.object.size();
    }
    case Node::Kind::List: {
      const auto kids = visibleKids(n, anc);
      if (kids.size() != j.array.size()) return false;
      const Stamp a = maxS(anc, n.clearS);
      for (size_t i = 0; i < kids.size(); ++i)
        if (!nodeEqJson(*kids[i].second, a, j.array[i])) return false;
      return true;
    }
    default: return false;
  }
}

// Longest increasing subsequence of `v` (values >= 0); returns a mask of kept positions.
std::vector<bool> lisMask(const std::vector<int>& v) {
  std::vector<int> tails, tailIdx, prev(v.size(), -1);
  for (size_t i = 0; i < v.size(); ++i) {
    if (v[i] < 0) continue;
    auto it = std::lower_bound(tails.begin(), tails.end(), v[i]);
    const size_t p = static_cast<size_t>(it - tails.begin());
    if (it == tails.end()) {
      tails.push_back(v[i]);
      tailIdx.push_back(static_cast<int>(i));
    } else {
      *it = v[i];
      tailIdx[p] = static_cast<int>(i);
    }
    prev[i] = p > 0 ? tailIdx[p - 1] : -1;
  }
  std::vector<bool> keep(v.size(), false);
  int k = tailIdx.empty() ? -1 : tailIdx.back();
  while (k >= 0) {
    keep[static_cast<size_t>(k)] = true;
    k = prev[static_cast<size_t>(k)];
  }
  return keep;
}

struct DiffCtx {
  SiteId site;
  const std::function<uint64_t()>& next;
  std::vector<Op>& out;
};

void diffNode(const Node& n, const Stamp& anc, Path& path, const Json& j, DiffCtx& c);

void emitIns(Path& path, const Json& j, const std::string& pos, DiffCtx& c) {
  Op op;
  op.kind = OpKind::Ins;
  op.t = c.next();
  op.path = path;
  op.value = j;
  op.pos = pos;
  c.out.push_back(std::move(op));
}

void emitDel(const Path& path, DiffCtx& c) {
  Op op;
  op.kind = OpKind::Del;
  op.t = c.next();
  op.path = path;
  c.out.push_back(std::move(op));
}

std::string keyedId(const Json& e) {
  if (!e.is(Json::Type::Object)) return {};
  const Json* jid = e.find("id");
  if (!jid || !jid->is(Json::Type::Number)) return {};
  return "#" + std::to_string(static_cast<long long>(jid->number));
}

void diffList(const Node& n, const Stamp& anc, Path& path, const Json& arr, DiffCtx& c) {
  const auto old = visibleKids(n, anc);
  const Stamp a = maxS(anc, n.clearS);
  const size_t on = old.size(), nn = arr.array.size();
  std::vector<int> newToOld(nn, -1);
  bool keyed = nn > 0;
  for (const auto& e : arr.array)
    if (keyedId(e).empty()) keyed = false;
  for (const auto& o : old)
    if ((*o.first)[0] != '#') keyed = false;
  std::vector<std::string> newIds(nn);
  if (keyed) {
    std::map<std::string, int> idx;
    for (size_t i = 0; i < on; ++i) idx[*old[i].first] = static_cast<int>(i);
    std::set<std::string> used;
    for (size_t i = 0; i < nn; ++i) {
      std::string id = keyedId(arr.array[i]);
      if (!used.insert(id).second) {
        keyed = false;
        break;
      }
      newIds[i] = id;
      auto it = idx.find(id);
      if (it != idx.end()) newToOld[i] = it->second;
    }
    if (!keyed) std::fill(newToOld.begin(), newToOld.end(), -1);
  }
  if (!keyed) {
    newIds.assign(nn, std::string());
    size_t p = 0;
    while (p < on && p < nn && nodeEqJson(*old[p].second, a, arr.array[p])) {
      newToOld[p] = static_cast<int>(p);
      ++p;
    }
    size_t s = 0;
    while (s < on - p && s < nn - p && nodeEqJson(*old[on - 1 - s].second, a, arr.array[nn - 1 - s])) {
      newToOld[nn - 1 - s] = static_cast<int>(on - 1 - s);
      ++s;
    }
    const size_t om = on - p - s, nm = nn - p - s;
    auto pairGap = [&](size_t o0, size_t o1, size_t n0, size_t n1) {
      for (size_t k = 0; o0 + k < o1 && n0 + k < n1; ++k) newToOld[n0 + k] = static_cast<int>(o0 + k);
    };
    if (om == nm || om == 0 || nm == 0 || om * nm > 1000000) {
      pairGap(p, p + om, p, p + nm);
    } else {
      std::vector<std::string> os(om), ns(nm);
      for (size_t i = 0; i < om; ++i) os[i] = jsonCompact(materializeNode(*old[p + i].second, a));
      for (size_t i = 0; i < nm; ++i) ns[i] = jsonCompact(arr.array[p + i]);
      std::vector<uint32_t> dp((om + 1) * (nm + 1), 0);
      auto at = [&](size_t i, size_t k) -> uint32_t& { return dp[i * (nm + 1) + k]; };
      for (size_t i = om; i-- > 0;)
        for (size_t k = nm; k-- > 0;) at(i, k) = os[i] == ns[k] ? at(i + 1, k + 1) + 1 : std::max(at(i + 1, k), at(i, k + 1));
      size_t i = 0, k = 0, gi = 0, gk = 0;
      while (i < om && k < nm) {
        if (os[i] == ns[k]) {
          pairGap(p + gi, p + i, p + gk, p + k);
          newToOld[p + k] = static_cast<int>(p + i);
          ++i;
          ++k;
          gi = i;
          gk = k;
        } else if (at(i + 1, k) >= at(i, k + 1)) {
          ++i;
        } else {
          ++k;
        }
      }
      pairGap(p + gi, p + om, p + gk, p + nm);
    }
  }
  // deletions
  std::vector<bool> matchedOld(on, false);
  for (int o : newToOld)
    if (o >= 0) matchedOld[static_cast<size_t>(o)] = true;
  for (size_t i = 0; i < on; ++i)
    if (!matchedOld[i]) {
      path.push_back(*old[i].first);
      emitDel(path, c);
      path.pop_back();
    }
  // content of matched elements
  for (size_t i = 0; i < nn; ++i) {
    if (newToOld[i] < 0) continue;
    const auto& o = old[static_cast<size_t>(newToOld[i])];
    path.push_back(*o.first);
    diffNode(*o.second, a, path, arr.array[i], c);
    path.pop_back();
  }
  // order: the longest increasing run of old positions keeps its keys
  std::vector<bool> stable = lisMask(newToOld);
  size_t i = 0;
  while (i < nn) {
    if (stable[i]) {
      ++i;
      continue;
    }
    size_t j = i;
    while (j < nn && !stable[j]) ++j;
    std::string left;
    for (size_t b = i; b-- > 0;)
      if (stable[b]) {
        left = old[static_cast<size_t>(newToOld[b])].second->pos;
        break;
      }
    // the right neighbour must sort strictly after the left one (equal keys tie on ids)
    while (j < nn) {
      const std::string& r = old[static_cast<size_t>(newToOld[j])].second->pos;
      if (left.empty() || r > left) break;
      stable[j] = false;
      ++j;
      while (j < nn && !stable[j]) ++j;
    }
    const std::string right = j < nn ? old[static_cast<size_t>(newToOld[j])].second->pos : std::string();
    const auto keys = keysBetween(left, right, j - i);
    for (size_t k = i; k < j; ++k) {
      const std::string& key = keys[k - i];
      if (newToOld[k] >= 0) {
        const auto& o = old[static_cast<size_t>(newToOld[k])];
        Op op;
        op.kind = OpKind::Mov;
        op.t = c.next();
        op.path = path;
        op.path.push_back(*o.first);
        op.pos = key;
        op.prev = Json::makeString(o.second->pos);
        c.out.push_back(std::move(op));
      } else {
        Op op;
        op.kind = OpKind::Ins;
        op.t = c.next();
        op.path = path;
        op.path.push_back(keyed ? newIds[k] : elementId(Stamp{op.t, c.site}));
        op.value = arr.array[k];
        op.pos = key;
        c.out.push_back(std::move(op));
      }
    }
    i = j;
  }
}

void diffNode(const Node& n, const Stamp& anc, Path& path, const Json& j, DiffCtx& c) {
  if (n.kind != kindOf(j)) {
    emitIns(path, j, std::string(), c);
    return;
  }
  switch (n.kind) {
    case Node::Kind::Value:
      if (!primEq(n.val, j)) {
        Op op;
        op.kind = OpKind::Set;
        op.t = c.next();
        op.path = path;
        op.value = j;
        op.prev = n.val;
        c.out.push_back(std::move(op));
      }
      break;
    case Node::Kind::Map: {
      const Stamp a = maxS(anc, n.clearS);
      for (const auto& [k, ch] : n.kids) {
        if (!nodeVisible(*ch, a)) continue;
        path.push_back(k);
        if (const Json* v = j.find(k))
          diffNode(*ch, a, path, *v, c);
        else
          emitDel(path, c);
        path.pop_back();
      }
      for (const auto& [k, v] : j.object) {
        auto it = n.kids.find(k);
        if (it != n.kids.end() && nodeVisible(*it->second, a)) continue;
        path.push_back(k);
        emitIns(path, v, std::string(), c);
        path.pop_back();
      }
      break;
    }
    case Node::Kind::List: diffList(n, anc, path, j, c); break;
    default: break;
  }
}

Json nodeToJson(const Node& n) {
  Json j = Json::makeObject();
  j.object["k"] = Json::makeNumber(static_cast<int>(n.kind));
  if (!n.kindS.zero()) j.object["ks"] = Json::makeString(stampText(n.kindS));
  if (n.pres) j.object["p"] = Json::makeNumber(1);
  if (!n.presS.zero()) j.object["ps"] = Json::makeString(stampText(n.presS));
  if (!n.clearS.zero()) j.object["c"] = Json::makeString(stampText(n.clearS));
  if (n.kind == Node::Kind::Value || !n.valS.zero()) {
    j.object["v"] = n.val;
    if (!n.valS.zero()) j.object["vs"] = Json::makeString(stampText(n.valS));
  }
  if (!n.posS.zero()) {
    j.object["o"] = Json::makeString(n.pos);
    j.object["os"] = Json::makeString(stampText(n.posS));
  }
  if (!n.kids.empty()) {
    Json kids = Json::makeObject();
    for (const auto& [k, c] : n.kids) kids.object[k] = nodeToJson(*c);
    j.object["n"] = std::move(kids);
  }
  return j;
}

bool nodeFromJson(const Json& j, Node& n, int depth) {
  if (!j.is(Json::Type::Object) || depth > 200) return false;
  const int k = static_cast<int>(j.num("k", 0));
  if (k < 0 || k > 3) return false;
  n.kind = static_cast<Node::Kind>(k);
  n.kindS = stampFrom(j.text("ks"));
  n.pres = j.num("p", 0) != 0;
  n.presS = stampFrom(j.text("ps"));
  n.clearS = stampFrom(j.text("c"));
  if (const Json* v = j.find("v")) n.val = *v;
  n.valS = stampFrom(j.text("vs"));
  n.pos = j.text("o");
  n.posS = stampFrom(j.text("os"));
  if (const Json* kids = j.find("n"); kids && kids->is(Json::Type::Object))
    for (const auto& [key, cj] : kids->object) {
      auto c = std::make_unique<Node>();
      if (!nodeFromJson(cj, *c, depth + 1)) return false;
      n.kids[key] = std::move(c);
    }
  return true;
}

size_t countNodes(const Node& n) {
  size_t c = 1;
  for (const auto& [k, ch] : n.kids) c += countNodes(*ch);
  return c;
}

size_t collect(Node& n, const Stamp& anc, const Stamp& before) {
  size_t removed = 0;
  const Stamp a = maxS(anc, n.clearS);
  for (auto it = n.kids.begin(); it != n.kids.end();) {
    Node& c = *it->second;
    const bool dead = (!c.pres && c.presS < before) || (c.presS < a && a < before);
    if (dead) {
      removed += countNodes(c);
      it = n.kids.erase(it);
    } else {
      removed += collect(c, a, before);
      ++it;
    }
  }
  return removed;
}

}  // namespace

// ---------------------------------------------------------------- clock ----
Hlc::Hlc() {
  wallMs = [] {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
  };
}

uint64_t Hlc::tick() {
  const uint64_t cand = fromMs(wallMs());
  last_ = std::max(cand, last_ + 1);
  return last_;
}

void Hlc::observe(uint64_t remote) { last_ = std::max(last_, remote); }

// ----------------------------------------------------------------- keys ----
std::string keyBetween(const std::string& a, const std::string& b) {
  if (!b.empty() && a >= b) return a + "V";  // invalid input: still a key after `a`
  std::string out;
  bool bOpen = b.empty();
  for (size_t i = 0;; ++i) {
    const int da = i < a.size() ? digitOf(a[i]) : 0;
    const int db = bOpen ? kBase : (i < b.size() ? digitOf(b[i]) : 0);
    if (da == db) {
      out.push_back(kDigits[da]);
      continue;
    }
    if (db - da > 1) {
      out.push_back(kDigits[(da + db) / 2]);
      return out;
    }
    out.push_back(kDigits[da]);
    bOpen = true;  // the rest only has to exceed a's remainder
  }
}

std::vector<std::string> keysBetween(const std::string& a, const std::string& b, size_t n) {
  std::vector<std::string> out;
  if (n == 0) return out;
  const std::string mid = keyBetween(a, b);
  const size_t left = (n - 1) / 2;
  out = keysBetween(a, mid, left);
  out.push_back(mid);
  auto right = keysBetween(mid, b, n - 1 - left);
  out.insert(out.end(), right.begin(), right.end());
  return out;
}

std::string elementId(const Stamp& s, int k) {
  std::string id = hex(s.t) + "." + hex(s.site & 0xffffffffu);
  if (k >= 0) id += "." + std::to_string(k);
  return id;
}

// ------------------------------------------------------------------ ops ----
std::string opKindName(OpKind k) {
  switch (k) {
    case OpKind::Set: return "set";
    case OpKind::Ins: return "ins";
    case OpKind::Del: return "del";
    case OpKind::Undel: return "undel";
    case OpKind::Mov: return "mov";
  }
  return "?";
}

Json opToJson(const Op& op) {
  Json j = Json::makeObject();
  j.object["k"] = Json::makeNumber(static_cast<int>(op.kind));
  j.object["t"] = Json::makeString(hex(op.t));
  Json p = Json::makeArray();
  for (const auto& s : op.path) p.array.push_back(Json::makeString(s));
  j.object["p"] = std::move(p);
  if (op.kind == OpKind::Set || op.kind == OpKind::Ins) j.object["v"] = op.value;
  if (!op.pos.empty()) j.object["o"] = Json::makeString(op.pos);
  if (!op.prev.is(Json::Type::Null)) j.object["r"] = op.prev;
  return j;
}

bool opFromJson(const Json& j, Op& out) {
  if (!j.is(Json::Type::Object)) return false;
  const int k = static_cast<int>(j.num("k", 0));
  if (k < 1 || k > 5) return false;
  out.kind = static_cast<OpKind>(k);
  bool ok = false;
  out.t = unhex(j.text("t"), &ok);
  if (!ok) return false;
  const Json* p = j.find("p");
  if (!p || !p->is(Json::Type::Array) || p->array.empty() || p->array.size() > 64) return false;
  out.path.clear();
  for (const auto& s : p->array) {
    if (!s.is(Json::Type::String) || s.str.empty()) return false;
    out.path.push_back(s.str);
  }
  if (const Json* v = j.find("v")) out.value = *v;
  out.pos = j.text("o");
  if (const Json* r = j.find("r")) out.prev = *r;
  if ((out.kind == OpKind::Set) && (out.value.is(Json::Type::Object) || out.value.is(Json::Type::Array))) return false;
  if (out.kind == OpKind::Mov && out.pos.empty()) return false;
  return true;
}

// ---------------------------------------------------------------- nodes ----
bool nodeVisible(const Node& n, const Stamp& anc) {
  if (n.kind == Node::Kind::Unknown || !n.pres || n.presS < anc) return false;
  if (n.kind == Node::Kind::Value && n.valS < maxS(anc, n.clearS)) return false;
  return true;
}

std::vector<std::pair<const std::string*, const Node*>> visibleKids(const Node& n, const Stamp& anc) {
  std::vector<std::pair<const std::string*, const Node*>> out;
  const Stamp a = maxS(anc, n.clearS);
  for (const auto& [k, c] : n.kids)
    if (nodeVisible(*c, a)) out.emplace_back(&k, c.get());
  if (n.kind == Node::Kind::List)
    std::sort(out.begin(), out.end(), [](const auto& x, const auto& y) {
      if (x.second->pos != y.second->pos) return x.second->pos < y.second->pos;
      return *x.first < *y.first;
    });
  return out;
}

Json materializeNode(const Node& n, const Stamp& anc) {
  switch (n.kind) {
    case Node::Kind::Value: return n.val;
    case Node::Kind::Map: {
      Json j = Json::makeObject();
      const Stamp a = maxS(anc, n.clearS);
      for (const auto& [k, c] : n.kids)
        if (nodeVisible(*c, a)) j.object.emplace(k, materializeNode(*c, a));
      return j;
    }
    case Node::Kind::List: {
      Json j = Json::makeArray();
      const Stamp a = maxS(anc, n.clearS);
      const auto kids = visibleKids(n, anc);
      j.array.reserve(kids.size());
      for (const auto& kc : kids) j.array.push_back(materializeNode(*kc.second, a));
      return j;
    }
    default: return Json();
  }
}

// -------------------------------------------------------------- replica ----
Replica::Replica() { clear(); }

void Replica::clear() {
  root_ = Node();
  root_.kind = Node::Kind::Map;
  root_.pres = true;
}

const Node* Replica::find(const Path& path) const {
  const Node* n = &root_;
  for (const auto& seg : path) {
    auto it = n->kids.find(seg);
    if (it == n->kids.end()) return nullptr;
    n = it->second.get();
  }
  return n;
}

bool Replica::visible(const Path& path) const {
  const Node* n = &root_;
  Stamp anc;
  for (const auto& seg : path) {
    anc = maxS(anc, n->clearS);
    auto it = n->kids.find(seg);
    if (it == n->kids.end() || !nodeVisible(*it->second, anc)) return false;
    n = it->second.get();
  }
  return true;
}

Json Replica::materialize(const Path& path) const {
  const Node* n = &root_;
  Stamp anc;
  for (const auto& seg : path) {
    anc = maxS(anc, n->clearS);
    auto it = n->kids.find(seg);
    if (it == n->kids.end() || !nodeVisible(*it->second, anc)) return Json();
    n = it->second.get();
  }
  return materializeNode(*n, anc);
}

bool Replica::apply(const Op& op, SiteId site) {
  if (op.path.empty() || op.path.size() > 64) return false;
  const Stamp s{op.t, site};
  Node* n = &root_;
  for (const auto& seg : op.path) n = child(*n, seg);
  switch (op.kind) {
    case OpKind::Set:
      if (op.value.is(Json::Type::Object) || op.value.is(Json::Type::Array)) return false;
      if (s > n->valS) {
        n->val = op.value;
        n->valS = s;
      }
      if (n->kind == Node::Kind::Unknown) n->kind = Node::Kind::Value;
      break;
    case OpKind::Ins:
      writeContent(*n, op.value, s);
      if (!op.pos.empty() && s > n->posS) {
        n->pos = op.pos;
        n->posS = s;
      }
      break;
    case OpKind::Del:
    case OpKind::Undel:
      if (s > n->presS) {
        n->pres = op.kind == OpKind::Undel;
        n->presS = s;
      }
      break;
    case OpKind::Mov:
      if (op.pos.empty()) return false;
      if (s > n->posS) {
        n->pos = op.pos;
        n->posS = s;
      }
      break;
  }
  return true;
}

void Replica::diff(const Path& path, const Json& json, SiteId site, const std::function<uint64_t()>& next, std::vector<Op>& out) const {
  DiffCtx c{site, next, out};
  Path p = path;
  const Node* n = &root_;
  Stamp anc;
  for (const auto& seg : path) {
    anc = maxS(anc, n->clearS);
    auto it = n->kids.find(seg);
    if (it == n->kids.end() || !nodeVisible(*it->second, anc)) {
      emitIns(p, json, std::string(), c);
      return;
    }
    n = it->second.get();
  }
  diffNode(*n, anc, p, json, c);
}

Json Replica::toJson() const { return nodeToJson(root_); }

bool Replica::fromJson(const Json& j) {
  Node n;
  if (!nodeFromJson(j, n, 0)) return false;
  root_ = std::move(n);
  root_.kind = Node::Kind::Map;
  root_.pres = true;
  return true;
}

size_t Replica::nodeCount() const { return countNodes(root_); }

size_t Replica::collectTombstones(const Stamp& before) { return collect(root_, Stamp{}, before); }

}  // namespace gl::collab
