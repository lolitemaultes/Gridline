// Gridline - K-321: origin sidecars v1 and v2, the trail through them (see ReceiptOrigin.h).
// The v1 functions (writeOrigin, readOrigin, originChain, mapThrough) moved here from
// TrackReceipt.cpp unchanged.
#include "ReceiptOrigin.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include "Json.h"

namespace gl::receipt {

namespace fs = std::filesystem;

// ---- version 1 (J-219)

std::string originPath(const std::string& file) { return file + ".origin.json"; }

bool writeOrigin(const std::string& file, const Origin& o) {
  Json j = Json::makeObject();
  j.object["gridlineOrigin"] = Json::makeNumber(1);
  j.object["source"] = Json::makeString(o.source);
  j.object["start"] = Json::makeNumber(o.start);
  j.object["end"] = Json::makeNumber(o.end);
  j.object["speed"] = Json::makeNumber(o.speed);
  Json b;
  b.type = Json::Type::Bool;
  b.boolean = o.reverse;
  j.object["reverse"] = b;
  b.boolean = o.rangeKnown;
  j.object["rangeKnown"] = b;
  j.object["kind"] = Json::makeString(o.kind);
  if (!o.sources.empty()) {
    Json a = Json::makeArray();
    for (const auto& s : o.sources) a.array.push_back(Json::makeString(s));
    j.object["sources"] = a;
  }
  std::ofstream f(originPath(file), std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f << jsonStringify(j, 2) << "\n";
  return static_cast<bool>(f);
}

namespace {

bool readText(const std::string& path, std::string& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

// In a package the sources are named relative to the file (ProjectInterop, sidecarsFor).
void beside(std::string& p, const std::string& baseDir) {
  if (p.empty() || baseDir.empty() || p.rfind("sha256:", 0) == 0) return;
  const fs::path f = fs::u8path(p);
  if (!f.is_absolute()) p = (fs::u8path(baseDir) / f).lexically_normal().u8string();
}

// The v1 fields of a sidecar object, exactly as 0.7.7 reads them.
void readV1(const Json& j, Origin& o, const std::string& baseDir) {
  o = Origin{};
  o.source = j.text("source");
  o.start = j.num("start", 0);
  o.end = j.num("end", -1);
  o.speed = j.num("speed", 1);
  if (!(o.speed > 0)) o.speed = 1;
  if (const Json* v = j.find("reverse"); v && v->type == Json::Type::Bool) o.reverse = v->boolean;
  if (const Json* v = j.find("rangeKnown"); v && v->type == Json::Type::Bool) o.rangeKnown = v->boolean;
  o.kind = j.text("kind");
  if (const Json* a = j.find("sources"); a && a->type == Json::Type::Array)
    for (const auto& s : a->array)
      if (s.type == Json::Type::String) o.sources.push_back(s.str);
  beside(o.source, baseDir);
  for (auto& x : o.sources) beside(x, baseDir);
}

std::string dirOf(const std::string& file) { return fs::u8path(file).parent_path().u8string(); }

}  // namespace

bool readOrigin(const std::string& file, Origin& o) {
  std::string text;
  if (!readText(originPath(file), text)) return false;
  Json j;
  if (!jsonParse(text, j) || j.type != Json::Type::Object || !j.find("gridlineOrigin")) return false;
  readV1(j, o, dirOf(file));
  return !o.source.empty();
}

std::vector<Origin> originChain(const std::string& file) {
  std::vector<Origin> chain;
  std::set<std::string> seen{file};
  std::string at = file;
  Origin o;
  while (chain.size() < 8 && readOrigin(at, o)) {
    chain.push_back(o);
    if (!seen.insert(o.source).second) break;  // a loop
    at = o.source;
  }
  return chain;
}

bool mapThrough(const std::vector<Origin>& chain, double fileSeconds, Ranges& r) {
  double len = fileSeconds;
  for (const Origin& o : chain) {
    if (!o.rangeKnown) return false;
    if (len <= 0)
      for (const Range& x : r) len = std::max(len, x.b);
    const double end = o.end >= 0 ? o.end : o.start + len * o.speed;
    Ranges m;
    for (const Range& x : r) {
      Range y = o.reverse ? Range{end - x.b * o.speed, end - x.a * o.speed} : Range{o.start + x.a * o.speed, o.start + x.b * o.speed};
      y.a = std::clamp(y.a, o.start, end), y.b = std::clamp(y.b, o.start, end);
      if (y.b > y.a) m.push_back(y);
    }
    r = unite(m);
    len = 0;  // the next file's length: from its own range end
  }
  return true;
}

// ---- version 2

Origin v1View(const OriginV2& o) {
  if (o.maps.empty()) return o.v1;
  Origin v;
  v.kind = o.v1.kind;
  if (o.maps.size() == 1 && o.complete) {
    const std::vector<Seg> segs = expand(o.maps[0]);
    if (segs.size() == 1) {
      const Seg& g = segs[0];
      const double len = g[1] - g[0], span = std::fabs(g[3] - g[2]);
      const bool whole = o.length <= 0 || std::fabs(g[1] - o.length) < 1e-3;
      if (std::fabs(g[0]) < 1e-6 && whole && len > 0 && span > 0) {
        v.source = o.maps[0].source;
        v.start = std::min(g[2], g[3]);
        v.end = std::max(g[2], g[3]);
        v.speed = span / len;
        v.reverse = g[3] < g[2];
        v.rangeKnown = true;
        return v;
      }
    }
  }
  v.rangeKnown = false;
  v.source = o.maps[0].source;
  for (const SourceMap& m : o.maps)
    if (std::find(v.sources.begin(), v.sources.end(), m.source) == v.sources.end()) v.sources.push_back(m.source);
  return v;
}

std::vector<Seg> expand(const SourceMap& m) {
  std::vector<Seg> out = m.segs;
  const int n = static_cast<int>(m.segs.size());
  for (const SourceMap::Rep& r : m.rep) {
    const int i0 = std::clamp(r.i0, 0, n), i1 = std::clamp(r.i1, i0, n);
    for (int k = 1; k <= std::min(r.count, 1 << 20); ++k)
      for (int i = i0; i < i1; ++i) {
        Seg g = m.segs[static_cast<size_t>(i)];
        g[0] += r.period * k, g[1] += r.period * k;
        out.push_back(g);
      }
  }
  return out;
}

void compactLoops(SourceMap& m) {
  if (!m.rep.empty()) m.segs = expand(m), m.rep.clear();
  const std::vector<Seg>& s = m.segs;
  const int n = static_cast<int>(s.size());
  constexpr double kTol = 1e-6;
  auto same = [&](int a, int b, double period) {
    const Seg &x = s[static_cast<size_t>(a)], &y = s[static_cast<size_t>(b)];
    return std::fabs(y[0] - x[0] - period) < kTol && std::fabs(y[1] - x[1] - period) < kTol && std::fabs(y[2] - x[2]) < kTol &&
           std::fabs(y[3] - x[3]) < kTol;
  };
  std::vector<Seg> out;
  std::vector<SourceMap::Rep> reps;
  int i = 0;
  while (i < n) {
    int bestL = 0, bestC = 0;
    for (int L = 1; L <= 64 && i + 2 * L <= n; ++L) {
      const double period = s[static_cast<size_t>(i + L)][0] - s[static_cast<size_t>(i)][0];
      if (!(period > 0)) continue;
      int c = 0;
      for (;; ++c) {
        const int at = i + (c + 1) * L;
        if (at + L > n) break;
        bool ok = true;
        for (int t = 0; t < L && ok; ++t) ok = same(i + t, at + t, period * (c + 1));
        if (!ok) break;
      }
      if (c >= 1 && c * L > bestC * bestL) bestL = L, bestC = c;
    }
    if (bestL > 0) {
      SourceMap::Rep r;
      r.i0 = static_cast<int>(out.size()), r.i1 = r.i0 + bestL;
      r.period = s[static_cast<size_t>(i + bestL)][0] - s[static_cast<size_t>(i)][0];
      r.count = bestC;
      for (int t = 0; t < bestL; ++t) out.push_back(s[static_cast<size_t>(i + t)]);
      reps.push_back(r);
      i += bestL * (bestC + 1);
    } else {
      out.push_back(s[static_cast<size_t>(i)]);
      ++i;
    }
  }
  m.segs = std::move(out);
  m.rep = std::move(reps);
}

namespace {

Json boolean(bool v) {
  Json b;
  b.type = Json::Type::Bool;
  b.boolean = v;
  return b;
}
Json numbers(std::initializer_list<double> v) {
  Json a = Json::makeArray();
  for (double x : v) a.array.push_back(Json::makeNumber(x));
  return a;
}
std::vector<double> numbersOf(const Json* a) {
  std::vector<double> out;
  if (a && a->type == Json::Type::Array)
    for (const Json& v : a->array)
      if (v.type == Json::Type::Number) out.push_back(v.number);
  return out;
}

}  // namespace

Json originToJson(const OriginV2& o) {
  const Origin v = v1View(o);
  Json j = Json::makeObject();
  const bool two = o.version >= 2 || !o.maps.empty();
  j.object["gridlineOrigin"] = Json::makeNumber(two ? 2 : 1);
  j.object["source"] = Json::makeString(v.source);
  j.object["start"] = Json::makeNumber(v.start);
  j.object["end"] = Json::makeNumber(v.end);
  j.object["speed"] = Json::makeNumber(v.speed);
  j.object["reverse"] = boolean(v.reverse);
  j.object["rangeKnown"] = boolean(v.rangeKnown);
  j.object["kind"] = Json::makeString(v.kind);
  if (!v.sources.empty()) {
    Json a = Json::makeArray();
    for (const auto& s : v.sources) a.array.push_back(Json::makeString(s));
    j.object["sources"] = a;
  }
  if (!two) return j;
  if (o.length > 0) j.object["length"] = Json::makeNumber(o.length);
  if (!o.hash.empty()) j.object["hash"] = Json::makeString(o.hash);
  j.object["complete"] = boolean(o.complete);
  Json maps = Json::makeArray();
  for (const SourceMap& m : o.maps) {
    Json e = Json::makeObject();
    e.object["source"] = Json::makeString(m.source);
    if (!m.sourceHash.empty()) e.object["sourceHash"] = Json::makeString(m.sourceHash);
    if (!m.sourceName.empty()) e.object["sourceName"] = Json::makeString(m.sourceName);
    if (m.sourceLength > 0) e.object["sourceLength"] = Json::makeNumber(m.sourceLength);
    if (!m.segs.empty()) {
      Json a = Json::makeArray();
      for (const Seg& g : m.segs) a.array.push_back(numbers({g[0], g[1], g[2], g[3]}));
      e.object["segs"] = a;
    }
    if (!m.rep.empty()) {
      Json a = Json::makeArray();
      for (const SourceMap::Rep& r : m.rep)
        a.array.push_back(numbers({static_cast<double>(r.i0), static_cast<double>(r.i1), r.period, static_cast<double>(r.count)}));
      e.object["rep"] = a;
    }
    if (!m.ranges.empty()) {
      Json a = Json::makeArray();
      for (const Range& r : m.ranges) a.array.push_back(numbers({r.a, r.b}));
      e.object["ranges"] = a;
    }
    if (m.pitch.size() == 2) e.object["pitch"] = numbers({m.pitch[0], m.pitch[1]});
    if (m.speed.size() == 2) e.object["speed"] = numbers({m.speed[0], m.speed[1]});
    if (m.chops >= 0) e.object["chops"] = Json::makeNumber(m.chops);
    maps.array.push_back(e);
  }
  j.object["maps"] = maps;
  if (!o.ops.empty()) {
    Json a = Json::makeArray();
    for (const auto& s : o.ops) a.array.push_back(Json::makeString(s));
    j.object["ops"] = a;
  }
  return j;
}

bool originFromJson(const Json& j, OriginV2& o, const std::string& baseDir) {
  if (j.type != Json::Type::Object || !j.find("gridlineOrigin")) return false;
  o = OriginV2{};
  readV1(j, o.v1, baseDir);
  o.version = j.num("gridlineOrigin", 1) >= 2 ? 2 : 1;
  o.length = j.num("length", 0);
  o.hash = j.text("hash");
  if (const Json* v = j.find("complete"); v && v->type == Json::Type::Bool) o.complete = v->boolean;
  if (const Json* a = j.find("maps"); a && a->type == Json::Type::Array)
    for (const Json& e : a->array) {
      if (e.type != Json::Type::Object) continue;
      SourceMap m;
      m.source = e.text("source");
      beside(m.source, baseDir);
      m.sourceHash = e.text("sourceHash");
      m.sourceName = e.text("sourceName");
      m.sourceLength = e.num("sourceLength", 0);
      if (const Json* s = e.find("segs"); s && s->type == Json::Type::Array)
        for (const Json& g : s->array) {
          const std::vector<double> v = numbersOf(&g);
          if (v.size() == 4 && v[1] > v[0]) m.segs.push_back({v[0], v[1], v[2], v[3]});
        }
      if (const Json* s = e.find("rep"); s && s->type == Json::Type::Array)
        for (const Json& g : s->array) {
          const std::vector<double> v = numbersOf(&g);
          if (v.size() == 4) m.rep.push_back({jsonInt<int>(v[0]), jsonInt<int>(v[1]), v[2], jsonInt<int>(v[3])});
        }
      if (const Json* s = e.find("ranges"); s && s->type == Json::Type::Array)
        for (const Json& g : s->array) {
          const std::vector<double> v = numbersOf(&g);
          if (v.size() == 2 && v[1] > v[0]) m.ranges.push_back({v[0], v[1]});
        }
      if (std::vector<double> v = numbersOf(e.find("pitch")); v.size() == 2) m.pitch = v;
      if (std::vector<double> v = numbersOf(e.find("speed")); v.size() == 2) m.speed = v;
      m.chops = jsonInt<int>(e.num("chops", -1));
      if (!m.source.empty()) o.maps.push_back(m);
    }
  if (const Json* a = j.find("ops"); a && a->type == Json::Type::Array)
    for (const Json& s : a->array)
      if (s.type == Json::Type::String) o.ops.push_back(s.str);
  return !o.v1.source.empty() || !o.maps.empty();
}

bool writeOriginV2(const std::string& file, const OriginV2& o) {
  std::ofstream f(originPath(file), std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f << jsonStringify(originToJson(o), 2) << "\n";
  return static_cast<bool>(f);
}

bool readOriginV2(const std::string& file, OriginV2& o) {
  std::string text;
  if (!readText(originPath(file), text)) return false;
  Json j;
  if (!jsonParse(text, j)) return false;
  return originFromJson(j, o, dirOf(file)) && !o.v1.source.empty();
}

// ---- the trail

bool Lookup::find(const std::string& file, OriginV2& o) const { return origin ? origin(file, o) : readOriginV2(file, o); }

std::string Lookup::resolve(const std::string& source) const {
  if (source.rfind("sha256:", 0) != 0 || !fileOfHash) return source;
  const std::string p = fileOfHash(source.substr(7));
  return p.empty() ? source : p;
}

Lookup sidecarLookup() { return Lookup{}; }

namespace {

constexpr int kMaxDepth = 8;

struct Walk {
  const Lookup& lookup;
  std::vector<std::string>* warnings;
  std::map<std::string, SourceTrace> out;
  std::vector<std::string> stack;

  void warn(const std::string& w) {
    if (warnings && std::find(warnings->begin(), warnings->end(), w) == warnings->end()) warnings->push_back(w);
  }
  void leaf(const std::string& file, const Ranges& r, bool unknown, int depth) {
    SourceTrace& t = out[file];
    for (const Range& x : r) t.ranges.push_back(x);
    t.ranges = unite(t.ranges);
    t.unknown = t.unknown || unknown;
    t.depth = std::max(t.depth, depth);
  }
  // `len`: the file's length (0 = from its ranges); `unknown`: a step above was not known.
  void go(const std::string& file, const Ranges& r, double len, bool unknown, int depth) {
    if (r.empty()) return;
    if (std::find(stack.begin(), stack.end(), file) != stack.end()) {
      warn("The origins of " + fs::u8path(file).filename().u8string() + " go round in a loop; counted there.");
      return leaf(file, r, unknown, depth);
    }
    OriginV2 o;
    if (!lookup.find(file, o)) return leaf(file, r, unknown, depth);
    if (depth >= kMaxDepth) {
      warn("The origins of " + fs::u8path(file).filename().u8string() + " go back more than 8 steps; counted there.");
      return leaf(file, r, unknown, depth);
    }
    stack.push_back(file);
    if (o.maps.empty()) {
      const Origin& v = o.v1;
      if (unknown || !v.rangeKnown) {
        std::vector<std::string> srcs{v.source};
        for (const std::string& s : v.sources)
          if (std::find(srcs.begin(), srcs.end(), s) == srcs.end()) srcs.push_back(s);
        for (const std::string& s : srcs) go(lookup.resolve(s), r, 0, true, depth + 1);
      } else {
        Ranges m = r;
        mapThrough({v}, len, m);
        go(lookup.resolve(v.source), m, 0, false, depth + 1);
      }
    } else {
      for (const SourceMap& map : o.maps) {
        const std::string src = lookup.resolve(map.source);
        Ranges m;
        if (unknown) {
          m = r;
        } else if (map.segs.empty()) {
          m = map.ranges;  // incomplete: somewhere in what was read
        } else {
          for (const Seg& g : expand(map))
            for (const Range& x : r) {
              const double a = std::max(x.a, g[0]), b = std::min(x.b, g[1]);
              if (!(b > a)) continue;
              const double k = (g[3] - g[2]) / (g[1] - g[0]);
              const double sa = g[2] + (a - g[0]) * k, sb = g[2] + (b - g[0]) * k;
              if (sa != sb) m.push_back({std::min(sa, sb), std::max(sa, sb)});
            }
          m = unite(m);
        }
        go(src, m, map.sourceLength, unknown, depth + 1);
      }
    }
    stack.pop_back();
  }
};

}  // namespace

std::map<std::string, SourceTrace> trace(const std::string& file, const Ranges& ranges, double fileSeconds, const Lookup& lookup,
                                         std::vector<std::string>* warnings) {
  Walk w{lookup, warnings, {}, {}};
  w.go(file, unite(ranges), fileSeconds, false, 0);
  return std::move(w.out);
}

bool hasMaps(const std::string& file, const Lookup& lookup) {
  std::set<std::string> seen;
  std::vector<std::pair<std::string, int>> todo{{file, 0}};
  while (!todo.empty()) {
    const auto [f, depth] = todo.back();
    todo.pop_back();
    if (depth >= kMaxDepth || !seen.insert(f).second) continue;
    OriginV2 o;
    if (!lookup.find(f, o)) continue;
    if (!o.maps.empty()) return true;
    todo.push_back({lookup.resolve(o.v1.source), depth + 1});
    for (const std::string& s : o.v1.sources) todo.push_back({lookup.resolve(s), depth + 1});
  }
  return false;
}

std::vector<Origin> chainOf(const std::string& file, const Lookup& lookup) {
  std::vector<Origin> chain;
  std::set<std::string> seen{file};
  std::string at = file;
  OriginV2 o;
  while (chain.size() < 8 && lookup.find(at, o)) {
    Origin v = o.v1;  // (the v1 fields as read: what 0.7.7 follows)
    v.source = lookup.resolve(v.source);
    for (std::string& s : v.sources) s = lookup.resolve(s);
    chain.push_back(v);
    if (!seen.insert(v.source).second) break;  // a loop
    at = v.source;
  }
  return chain;
}

std::map<std::string, OriginV2> trailOf(const std::string& file, const Lookup& lookup) {
  std::map<std::string, OriginV2> out;
  std::vector<std::pair<std::string, int>> todo{{file, 0}};
  while (!todo.empty()) {
    const auto [f, depth] = todo.back();
    todo.pop_back();
    if (depth >= kMaxDepth + 1 || out.count(f)) continue;
    OriginV2 o;
    if (!lookup.find(f, o)) continue;
    o.v1.source = lookup.resolve(o.v1.source);
    for (std::string& s : o.v1.sources) s = lookup.resolve(s);
    for (SourceMap& m : o.maps) m.source = lookup.resolve(m.source);
    out[f] = o;
    todo.push_back({o.v1.source, depth + 1});
    for (const std::string& s : o.v1.sources) todo.push_back({s, depth + 1});
    for (const SourceMap& m : o.maps) todo.push_back({m.source, depth + 1});
  }
  return out;
}

Lookup trailLookup(const std::map<std::string, OriginV2>& trail) {
  Lookup l;
  auto t = std::make_shared<std::map<std::string, OriginV2>>(trail);
  l.origin = [t](const std::string& file, OriginV2& o) {
    auto it = t->find(file);
    if (it == t->end()) return false;
    o = it->second;
    return true;
  };
  return l;
}

// ---- the project table

std::string tableOrigin(const OriginV2& o) { return jsonCompact(originToJson(o)); }

bool tableFind(const Saved& s, const std::string& hash, OriginV2& o) {
  if (hash.empty()) return false;
  auto it = s.origins.find(hash);
  if (it == s.origins.end()) return false;
  Json j;
  return jsonParse(it->second, j) && originFromJson(j, o) && !o.v1.source.empty();
}

bool rememberOrigins(Saved& s, const Measurement& m) {
  std::map<std::string, std::string> hashOf;
  std::set<std::string> live;
  for (const FileIn& f : m.files)
    if (!f.hash.empty()) hashOf[f.path] = f.hash, live.insert(f.hash);
  const auto named = [&](std::string& p) {
    if (auto it = hashOf.find(p); it != hashOf.end()) p = "sha256:" + it->second;
  };
  bool changed = false;
  for (const FileIn& f : m.files) {
    if (f.hash.empty()) continue;
    if (parseName(f.path).tracklib) {
      std::string& n = s.names[f.hash];
      const std::string want = fs::u8path(f.path).filename().u8string();
      if (n != want) n = want, changed = true;
    }
    if (!f.derived || f.chain.empty()) continue;
    OriginV2 o;
    bool have = false;
    if (auto t = f.trail.find(f.path); t != f.trail.end()) {
      Json j;
      have = jsonParse(t->second, j) && originFromJson(j, o);
    }
    if (!have) o = OriginV2{}, o.v1 = f.chain.front();
    named(o.v1.source);
    for (std::string& x : o.v1.sources) named(x);
    for (SourceMap& mp : o.maps) {
      if (auto it = hashOf.find(mp.source); it != hashOf.end() && mp.sourceHash.empty()) mp.sourceHash = it->second;
      named(mp.source);
    }
    if (o.hash.empty() && o.version >= 2) o.hash = f.hash;
    const std::string text = tableOrigin(o);
    if (s.origins[f.hash] != text) s.origins[f.hash] = text, changed = true;
  }
  for (auto* table : {&s.names, &s.origins})
    for (auto it = table->begin(); it != table->end();) {
      if (live.count(it->first)) {
        ++it;
      } else {
        it = table->erase(it), changed = true;
      }
    }
  return changed;
}

}  // namespace gl::receipt
