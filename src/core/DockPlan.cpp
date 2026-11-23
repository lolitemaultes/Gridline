// Gridline - the dock plan of a workspace. See DockPlan.h.
#include "DockPlan.h"
#include <algorithm>
#include <cmath>
#include <set>
#include "Json.h"

namespace gl::dockplan {

bool Node::operator==(const Node& o) const {
  if (split != o.split) return false;
  if (split) return axis == o.axis && std::fabs(ratio - o.ratio) < 1e-4f && kids == o.kids;
  return windows == o.windows && front == o.front && central == o.central;
}

namespace {

void collect(const Node& n, std::vector<std::string>& out) {
  if (n.split) {
    for (const Node& k : n.kids) collect(k, out);
    return;
  }
  out.insert(out.end(), n.windows.begin(), n.windows.end());
}

float cleanRatio(float r) {
  if (!std::isfinite(r)) return 0.5f;
  return std::clamp(r, kMinRatio, kMaxRatio);
}

float round4(float v) { return std::round(v * 10000.0f) / 10000.0f; }

// Removes refused and repeated windows; folds splits with an empty side. Returns false when the node
// ended up empty (no windows and no central leaf), so its parent folds it away.
bool clean(Node& n, const std::function<bool(const std::string&)>& known, std::set<std::string>& seen, int depth) {
  if (n.split && (n.kids.size() != 2 || depth >= kMaxDepth)) {
    // A broken split (or one too deep): its windows go into one leaf.
    std::vector<std::string> all;
    collect(n, all);
    const bool c = hasCentral(n);
    n = Node{};
    n.windows = all;
    n.central = c;
  }
  if (!n.split) {
    std::vector<std::string> keep;
    for (const std::string& w : n.windows)
      if (!w.empty() && w.size() < 200 && known(w) && seen.insert(w).second && static_cast<int>(seen.size()) <= kMaxWindows) keep.push_back(w);
    n.windows = keep;
    if (std::find(n.windows.begin(), n.windows.end(), n.front) == n.windows.end()) n.front.clear();
    n.kids.clear();
    n.axis = 0;
    n.ratio = 0.5f;
    return !n.windows.empty() || n.central;
  }
  n.axis = n.axis == 1 ? 1 : 0;
  n.ratio = round4(cleanRatio(n.ratio));
  const bool a = clean(n.kids[0], known, seen, depth + 1);
  const bool b = clean(n.kids[1], known, seen, depth + 1);
  if (a && b) return true;
  if (!a && !b) {
    n = Node{};
    return false;
  }
  Node keep = a ? n.kids[0] : n.kids[1];
  n = std::move(keep);
  return true;
}

// Several central leaves: only the first stays central.
void oneCentral(Node& n, bool& found) {
  if (n.split) {
    for (Node& k : n.kids) oneCentral(k, found);
    return;
  }
  if (n.central) {
    if (found) n.central = false;
    found = true;
  }
}

// The leaf along the first sides with the most windows (ties: the first).
Node* firstLeaf(Node& n) {
  if (!n.split) return &n;
  Node* a = firstLeaf(n.kids[0]);
  Node* b = firstLeaf(n.kids[1]);
  return b->windows.size() > a->windows.size() ? b : a;
}

Json nodeJson(const Node& n) {
  Json o = Json::makeObject();
  if (n.split) {
    o.object["split"] = Json::makeString(n.axis == 1 ? "y" : "x");
    o.object["ratio"] = Json::makeNumber(round4(n.ratio));
    Json k = Json::makeArray();
    for (const Node& c : n.kids) k.array.push_back(nodeJson(c));
    o.object["kids"] = k;
    return o;
  }
  Json w = Json::makeArray();
  for (const std::string& s : n.windows) w.array.push_back(Json::makeString(s));
  o.object["windows"] = w;
  if (!n.front.empty()) o.object["front"] = Json::makeString(n.front);
  if (n.central) {
    Json b;
    b.type = Json::Type::Bool;
    b.boolean = true;
    o.object["central"] = b;
  }
  return o;
}

Node nodeFrom(const Json& o, int depth) {
  Node n;
  if (!o.is(Json::Type::Object) || depth > kMaxDepth) return n;
  if (const Json* s = o.find("split"); s && s->is(Json::Type::String)) {
    const Json* k = o.find("kids");
    if (k && k->is(Json::Type::Array) && k->array.size() == 2) {
      n.split = true;
      n.axis = s->str == "y" ? 1 : 0;
      n.ratio = static_cast<float>(o.num("ratio", 0.5));
      n.kids = {nodeFrom(k->array[0], depth + 1), nodeFrom(k->array[1], depth + 1)};
      return n;
    }
  }
  if (const Json* w = o.find("windows"); w && w->is(Json::Type::Array))
    for (const Json& v : w->array)
      if (v.is(Json::Type::String)) n.windows.push_back(v.str);
  n.front = o.text("front");
  if (const Json* c = o.find("central"); c && c->is(Json::Type::Bool)) n.central = c->boolean;
  return n;
}

}  // namespace

bool hasCentral(const Node& n) {
  if (!n.split) return n.central;
  for (const Node& k : n.kids)
    if (hasCentral(k)) return true;
  return false;
}

std::vector<std::string> windows(const Plan& p) {
  std::vector<std::string> out;
  collect(p.root, out);
  for (const Float& f : p.floating) out.push_back(f.window);
  return out;
}

void sanitise(Plan& p, const std::function<bool(const std::string&)>& known) {
  std::set<std::string> seen;
  bool found = false;
  oneCentral(p.root, found);
  if (!clean(p.root, known, seen, 0)) p.root = Node{};
  if (!hasCentral(p.root)) firstLeaf(p.root)->central = true;
  std::vector<Float> fl;
  for (Float f : p.floating) {
    if (f.window.empty() || f.window.size() >= 200 || !known(f.window) || !seen.insert(f.window).second) continue;
    if (static_cast<int>(seen.size()) > kMaxWindows) break;
    auto fin = [](float v, float d) { return std::isfinite(v) ? v : d; };
    f.x = std::round(std::clamp(fin(f.x, 0), -20000.0f, 20000.0f));
    f.y = std::round(std::clamp(fin(f.y, 0), -20000.0f, 20000.0f));
    f.w = std::round(std::clamp(fin(f.w, 400), 60.0f, 20000.0f));
    f.h = std::round(std::clamp(fin(f.h, 300), 60.0f, 20000.0f));
    fl.push_back(f);
  }
  p.floating = fl;
}

std::string toJson(const Plan& p) {
  Json o = Json::makeObject();
  o.object["tree"] = nodeJson(p.root);
  if (!p.floating.empty()) {
    Json fl = Json::makeArray();
    for (const Float& f : p.floating) {
      Json j = Json::makeObject();
      j.object["window"] = Json::makeString(f.window);
      j.object["x"] = Json::makeNumber(std::round(f.x));
      j.object["y"] = Json::makeNumber(std::round(f.y));
      j.object["w"] = Json::makeNumber(std::round(f.w));
      j.object["h"] = Json::makeNumber(std::round(f.h));
      fl.array.push_back(j);
    }
    o.object["floating"] = fl;
  }
  return jsonCompact(o);
}

Plan fromJson(const std::string& text) {
  Plan p;
  Json o;
  if (text.empty() || text.size() > 65536 || !jsonParse(text, o) || !o.is(Json::Type::Object)) return p;
  if (const Json* t = o.find("tree")) p.root = nodeFrom(*t, 0);
  if (const Json* fl = o.find("floating"); fl && fl->is(Json::Type::Array))
    for (const Json& j : fl->array) {
      if (!j.is(Json::Type::Object)) continue;
      Float f;
      f.window = j.text("window");
      f.x = static_cast<float>(j.num("x"));
      f.y = static_cast<float>(j.num("y"));
      f.w = static_cast<float>(j.num("w", 400));
      f.h = static_cast<float>(j.num("h", 300));
      p.floating.push_back(f);
    }
  return p;
}

namespace {
bool similarNode(const Node& a, const Node& b, float tol) {
  if (a.split != b.split) return false;
  if (!a.split) return a.windows == b.windows && a.central == b.central && (a.front.empty() || b.front.empty() || a.front == b.front);
  return a.axis == b.axis && std::fabs(a.ratio - b.ratio) <= tol && similarNode(a.kids[0], b.kids[0], tol) && similarNode(a.kids[1], b.kids[1], tol);
}
}  // namespace

bool similar(const Plan& a, const Plan& b, float tol, float px) {
  if (!similarNode(a.root, b.root, tol) || a.floating.size() != b.floating.size()) return false;
  for (size_t i = 0; i < a.floating.size(); ++i) {
    const Float &f = a.floating[i], &g = b.floating[i];
    if (f.window != g.window || std::fabs(f.x - g.x) > px || std::fabs(f.y - g.y) > px || std::fabs(f.w - g.w) > px || std::fabs(f.h - g.h) > px) return false;
  }
  return true;
}

Float fitInto(const Float& f, float w, float h) {
  Float r = f;
  r.w = std::max(60.0f, std::min(r.w, w));
  r.h = std::max(60.0f, std::min(r.h, h));
  r.x = std::clamp(r.x, 0.0f, std::max(0.0f, w - r.w));
  r.y = std::clamp(r.y, 0.0f, std::max(0.0f, h - r.h));
  return r;
}

}  // namespace gl::dockplan
