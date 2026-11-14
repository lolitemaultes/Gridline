// Gridline - the Stage view's maths (see StageMap.h).
#include "StageMap.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <numeric>
#include "I18n.h"

namespace gl::stage {

float overlapArea(const Rect& a, const Rect& b) {
  const float w = std::min(a.x1, b.x1) - std::max(a.x0, b.x0), h = std::min(a.y1, b.y1) - std::max(a.y0, b.y0);
  return w > 0 && h > 0 ? w * h : 0.0f;
}

// ---------------------------------------------------------------- the stage on screen
float Geo::halfAt(float y) const {
  const float t = frontY > backY ? (y - backY) / (frontY - backY) : 1.0f;
  return backHalf + (frontHalf - backHalf) * t;
}

float Geo::yOf(double d) const {
  d = std::clamp(d, 0.0, 1.0);
  const double f = d * (1.0 + kPerspective) / (1.0 + kPerspective * d);
  return static_cast<float>(rowFront - (rowFront - rowBack) * f);
}

double Geo::dOfY(float y) const {
  if (rowFront <= rowBack) return 0;
  const double f = std::clamp(static_cast<double>(rowFront - y) / static_cast<double>(rowFront - rowBack), 0.0, 1.0);
  return std::clamp(f / (1.0 + kPerspective - kPerspective * f), 0.0, 1.0);
}

float Geo::reach(double d) const { return std::max(10.0f * scale, halfAt(yOf(d)) - 76.0f * scale); }

float Geo::xOf(double pan, double d) const { return cx + static_cast<float>(std::clamp(pan, -1.0, 1.0)) * reach(d); }

double Geo::panOfX(float x, double d) const { return std::clamp(static_cast<double>(x - cx) / static_cast<double>(reach(d)), -1.0, 1.0); }

float Geo::side(double d) const {
  return (kTokenFront + (kTokenBack - kTokenFront) * static_cast<float>(std::clamp(d, 0.0, 1.0))) * scale * tokenScale;
}

Rect Geo::token(double pan, double d) const {
  const float s = side(d), x = xOf(pan, d), y = yOf(d);
  return {x - s * 0.5f, y - s * 0.5f, x + s * 0.5f, y + s * 0.5f};
}

float Geo::footRy(double d) const { return (9.0f * side(d) / (kTokenFront * scale) + 2.0f) * scale; }

float Geo::footRx(double d, double sep) const {
  // Mono: just around the token; as recorded (100 %): an eighth of the stage's half width more;
  // doubled (200 %): a quarter.
  const double w = widthPercent(std::clamp(sep, -1.0, 1.0)) / 100.0;
  return side(d) * 0.5f + 10.0f * scale + static_cast<float>(w * 0.12) * reach(d);
}

double Geo::sepOfFootRx(double d, float rx) const {
  const double w = static_cast<double>(rx - side(d) * 0.5f - 10.0f * scale) / (0.12 * static_cast<double>(reach(d)));
  return std::clamp(w - 1.0, -1.0, 1.0);
}

bool Geo::inside(float x, float y) const {
  if (y < backY || y > frontY + kBulge * scale) return false;
  return std::fabs(x - cx) <= halfAt(std::min(y, frontY));
}

Geo layout(float x0, float y0, float x1, float y1, float scale, int tracks) {
  Geo g;
  g.scale = scale;
  const float W = std::max(1.0f, x1 - x0);
  g.cx = std::round((x0 + x1) * 0.5f);
  g.backY = std::round(y0 + 20.0f * scale);
  g.frontY = std::round(std::max(g.backY + 120.0f * scale, y1 - 88.0f * scale));
  const float depth = g.frontY - g.backY;
  g.frontHalf = std::max(80.0f * scale, std::min(W * 0.5f - std::max(16.0f * scale, W * 0.073f), depth * 1.31f));
  g.backHalf = g.frontHalf * 0.656f;
  g.rowFront = g.frontY - std::min(73.0f * scale, depth * 0.157f);
  g.rowBack = g.backY + std::min(63.0f * scale, depth * 0.135f);
  g.listenerY = g.frontY + (kBulge + 32.0f) * scale;
  g.tokenScale = (tracks >= kCrowded ? 0.86f : 1.0f) * std::clamp(depth / (466.0f * scale), 0.7f, 1.0f);
  return g;
}

double snapDepth(double d) { return std::clamp(std::round(d * 4.0) / 4.0, 0.0, 1.0); }

double snapPan(double pan, double zone) { return std::fabs(pan) < zone ? 0.0 : std::clamp(pan, -1.0, 1.0); }

// ---------------------------------------------------------------- groups
namespace {
size_t findRoot(std::vector<size_t>& p, size_t i) {
  while (p[i] != i) i = p[i] = p[p[i]];
  return i;
}
}  // namespace

std::vector<std::vector<size_t>> groups(const std::vector<Pos>& pts, const std::map<ObjectId, ObjectId>* prev) {
  const size_t n = pts.size();
  std::vector<size_t> parent(n);
  std::iota(parent.begin(), parent.end(), size_t{0});
  auto key = [&](size_t i) -> ObjectId {
    if (!prev) return kNoId;
    auto it = prev->find(pts[i].id);
    return it == prev->end() ? kNoId : it->second;
  };
  for (size_t i = 0; i < n; ++i)
    for (size_t j = i + 1; j < n; ++j) {
      const double dp = std::fabs(pts[i].pan - pts[j].pan), dd = std::fabs(pts[i].depth - pts[j].depth);
      bool same = dp <= kJoin + 1e-9 && dd <= kJoin + 1e-9;
      if (!same && dp <= kLeave && dd <= kLeave) {
        const ObjectId ki = key(i);
        same = ki != kNoId && ki == key(j);  // they were one group: the wider tolerance holds them
      }
      if (same) parent[findRoot(parent, i)] = findRoot(parent, j);
    }
  std::map<size_t, std::vector<size_t>> by;
  for (size_t i = 0; i < n; ++i) by[findRoot(parent, i)].push_back(i);
  std::vector<std::vector<size_t>> out;
  for (auto& [r, m] : by)
    if (m.size() >= 2) out.push_back(m);
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.front() < b.front(); });
  return out;
}

Pos spotOf(const std::vector<Pos>& pts, const std::vector<size_t>& members) {
  Pos p;
  if (members.empty()) return p;
  for (size_t i : members) p.pan += pts[i].pan, p.depth += pts[i].depth;
  p.pan /= static_cast<double>(members.size());
  p.depth /= static_cast<double>(members.size());
  p.id = pts[members.front()].id;
  return p;
}

Tray trayLayout(const Geo& g, Pt spot, int count, float minX, float maxX, float minY, float maxY) {
  Tray t;
  const float s = g.scale;
  t.spot = spot;
  t.token = kTokenTray * s;
  count = std::max(count, 1);
  int cols = count <= 4 ? count : count <= 12 ? 6 : count <= 16 ? 8 : 10;
  cols = std::min(cols, count);
  // A narrow stage: fewer columns, so the tray stays inside it (the rest go behind "+N").
  while (cols > 1 && static_cast<float>(cols) * kCellW * s + 22.0f * s > maxX - minX) --cols;
  const int rows = std::min(kTrayRows, (count + cols - 1) / cols);
  t.cols = cols, t.rows = rows;
  const int cap = cols * rows;
  t.shown = count > cap ? cap - 1 : count;
  t.more = count - t.shown;
  const float cw = kCellW * s, ch = kCellH * s, px = 11.0f * s, py = 7.0f * s;
  const float w = cols * cw + 2 * px, h = rows * ch + 2 * py;
  float x = std::round(spot.x - w * 0.5f);
  x = std::clamp(x, minX, std::max(minX, maxX - w));
  float y1 = spot.y - 22.0f * s;
  t.above = y1 - h >= minY;
  float y0 = t.above ? y1 - h : spot.y + 22.0f * s;
  if (!t.above && y0 + h > maxY) y0 = std::max(minY, maxY - h);
  y0 = std::round(y0);
  t.box = {x, y0, x + w, y0 + h};
  const int cells = t.shown + (t.more > 0 ? 1 : 0);
  for (int i = 0; i < cells; ++i) {
    const int r = i / cols, c = i % cols;
    const float cx0 = x + px + static_cast<float>(c) * cw, cy0 = y0 + py + static_cast<float>(r) * ch;
    t.cells.push_back({cx0, cy0, cx0 + cw, cy0 + ch});
  }
  return t;
}

int trayCellAt(const Tray& t, float x, float y) {
  for (size_t i = 0; i < t.cells.size(); ++i)
    if (t.cells[i].contains(x, y)) return static_cast<int>(i);
  return -1;
}

// ---------------------------------------------------------------- dropping without overlaps
int joinTarget(const Geo& g, double pan, double depth, Pt pointer, const std::vector<Obstacle>& obs, bool exact) {
  const Rect r = g.token(pan, depth);
  int best = -1;
  float bestArea = 1e30f;
  for (size_t k = 0; k < obs.size(); ++k)  // the pointer over a token or tray: that one (the smallest, the most specific)
    if (obs[k].r.contains(pointer.x, pointer.y) && obs[k].r.area() < bestArea) best = static_cast<int>(k), bestArea = obs[k].r.area();
  if (best >= 0) return best;
  float bestShare = 0;
  const float area = std::max(1.0f, r.area());
  for (size_t k = 0; k < obs.size(); ++k) {
    const float share = overlapArea(r, obs[k].r) / area;
    if (share > bestShare) bestShare = share, best = static_cast<int>(k);
  }
  if (best < 0) return -1;
  return (exact ? bestShare > 0 : bestShare >= kJoinShare) ? best : -1;
}

namespace {
bool clear(const Geo& g, double pan, double depth, const std::vector<Obstacle>& obs) {
  const Rect r = g.token(pan, depth).grown(kGap * g.scale);
  for (const Obstacle& o : obs)
    if (overlapArea(r, o.r) > 0) return false;
  return true;
}
}  // namespace

Drop resolveDrop(const Geo& g, double pan, double depth, Pt pointer, const std::vector<Obstacle>& obs, bool exact) {
  Drop d;
  pan = std::clamp(pan, -1.0, 1.0);
  depth = std::clamp(depth, 0.0, 1.0);
  d.pan = pan, d.depth = depth;
  if (const int t = joinTarget(g, pan, depth, pointer, obs, exact); t >= 0) {
    d.kind = Drop::Join;
    d.pan = obs[static_cast<size_t>(t)].pan, d.depth = obs[static_cast<size_t>(t)].depth;
    d.key = obs[static_cast<size_t>(t)].key;
    return d;
  }
  if (clear(g, pan, depth, obs)) return d;
  // The nearest free spot: along the row first (the side away from what is in the way first), then a
  // row nearer or further.
  auto search = [&](double dep, double& out) {
    const float px = g.xOf(pan, dep);
    float pull = 0;
    for (const Obstacle& o : obs)
      if (overlapArea(g.token(pan, dep).grown(kGap * g.scale), o.r) > 0) pull += (o.r.x0 + o.r.x1) * 0.5f - px;
    const double first = pull > 0 ? -1.0 : 1.0;
    for (int k = 1; k <= 400; ++k)
      for (double sgn : {first, -first}) {
        const double p = pan + sgn * 0.005 * k;
        if (p < -1.0 || p > 1.0) continue;
        if (clear(g, p, dep, obs)) {
          out = p;
          return true;
        }
      }
    return false;
  };
  double p = 0;
  for (double dep : {depth, depth + 0.25, depth - 0.25, depth + 0.5, depth - 0.5}) {
    if (dep < -1e-9 || dep > 1.0 + 1e-9) continue;
    if (search(std::clamp(dep, 0.0, 1.0), p)) {
      d.kind = Drop::Nudged;
      d.pan = p, d.depth = std::clamp(dep, 0.0, 1.0);
      return d;
    }
  }
  return d;  // a stage packed full: where it was dropped (the display keeps tokens apart)
}

std::vector<float> separate(const std::vector<Rect>& tokens, float gap) {
  const size_t n = tokens.size();
  std::vector<float> off(n, 0.0f);
  std::vector<size_t> idx(n);
  std::iota(idx.begin(), idx.end(), size_t{0});
  auto cy = [&](size_t i) { return (tokens[i].y0 + tokens[i].y1) * 0.5f; };
  auto cx = [&](size_t i) { return (tokens[i].x0 + tokens[i].x1) * 0.5f; };
  std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return cy(a) < cy(b) || (cy(a) == cy(b) && cx(a) < cx(b)); });
  for (size_t s = 0; s < n;) {  // rows: tokens whose rectangles share most of their height
    size_t e = s + 1;
    while (e < n && std::fabs(cy(idx[e]) - cy(idx[s])) < 0.5f * tokens[idx[s]].h()) ++e;
    std::vector<size_t> row(idx.begin() + static_cast<long>(s), idx.begin() + static_cast<long>(e));
    std::sort(row.begin(), row.end(), [&](size_t a, size_t b) { return cx(a) < cx(b) || (cx(a) == cx(b) && a < b); });
    for (int pass = 0; pass < 12; ++pass) {
      bool moved = false;
      for (size_t k = 0; k + 1 < row.size(); ++k) {
        const size_t a = row[k], b = row[k + 1];
        const float need = (tokens[a].w() + tokens[b].w()) * 0.5f + gap - ((cx(b) + off[b]) - (cx(a) + off[a]));
        if (need > 0.01f) {
          off[a] -= need * 0.5f;
          off[b] += need * 0.5f;
          moved = true;
        }
      }
      if (!moved) break;
    }
    s = e;
  }
  return off;
}

// ---------------------------------------------------------------- labels
namespace {
const char* const kEllipsis = "\xE2\x80\xA6";

std::vector<std::string> codepoints(const std::string& s) {
  std::vector<std::string> out;
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    const size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    out.push_back(s.substr(i, std::min(len, s.size() - i)));
    i += len;
  }
  return out;
}

std::vector<std::string> words(const std::string& s) {
  std::vector<std::string> out;
  std::string w;
  for (char c : s) {
    if (c == ' ') {
      if (!w.empty()) out.push_back(w), w.clear();
    } else {
      w += c;
    }
  }
  if (!w.empty()) out.push_back(w);
  return out;
}

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool numberish(const std::string& w) {
  bool digit = false;
  for (char c : w) {
    if (std::isdigit(static_cast<unsigned char>(c))) digit = true;
    else if (c != '-' && c != '.' && c != '_' && c != '#') return false;
  }
  return digit;
}

// The ending that tells this name apart ("" none).
std::string ending(const std::string& name, const std::vector<std::string>& others) {
  const std::vector<std::string> w = words(name);
  if (w.size() >= 2)
    for (const std::string& o : others) {  // (a) a twin with the same first word: the word where they differ
      if (o == name) continue;
      const std::vector<std::string> v = words(o);
      if (v.empty() || lower(v[0]) != lower(w[0])) continue;
      for (size_t i = 1; i < w.size(); ++i)
        if (i >= v.size() || lower(v[i]) != lower(w[i])) return w[i];
    }
  if (w.size() >= 2) {  // (b) a numbered, copy, side or bracket ending
    const std::string& last = w.back();
    const std::string l = lower(last);
    if (numberish(last) || l == "copy" || l == "l" || l == "r" || l == "left" || l == "right" || l == "mid" || l == "side") return last;
    if (const size_t b = name.find_last_of("(["); b != std::string::npos && (name.back() == ')' || name.back() == ']')) return name.substr(b);
  }
  if (w.size() == 1) {  // (c) digits ending a name without spaces
    size_t k = name.size();
    while (k > 0 && std::isdigit(static_cast<unsigned char>(name[k - 1]))) --k;
    if (k > 0 && k < name.size()) return name.substr(k);
  }
  return {};
}

std::string trimRight(std::string s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '-' || s.back() == '_')) s.pop_back();
  return s;
}
}  // namespace

std::string cutLabel(const std::string& name, float budget, const Measure& m, const std::vector<std::string>& others) {
  if (name.empty() || m(name) <= budget) return name;
  const std::vector<std::string> cps = codepoints(name);
  const std::string end = ending(name, others);
  auto fit = [&](const std::string& tail) -> std::string {
    std::string prefix;
    std::string best;
    for (size_t k = 0; k < cps.size(); ++k) {
      prefix += cps[k];
      const std::string t = trimRight(prefix) + kEllipsis + tail;
      if (m(t) > budget) break;
      best = t;
    }
    return best;
  };
  if (!end.empty() && end.size() < name.size())
    if (std::string t = fit(end); !t.empty()) return t;
  if (std::string t = fit(""); !t.empty()) return t;
  return cps.empty() ? std::string() : cps[0] + kEllipsis;
}

std::vector<LabelOut> layoutLabels(const std::vector<LabelIn>& in, const Measure& m, float scale) {
  const size_t n = in.size();
  std::vector<LabelOut> out(n);
  std::vector<float> w(n), natural(n), lo(n);
  std::vector<std::string> names;
  for (const LabelIn& l : in) names.push_back(l.name);
  for (size_t i = 0; i < n; ++i) {
    natural[i] = m(in[i].name);
    lo[i] = 2.2f * in[i].token;
    w[i] = std::min(natural[i], 3.2f * in[i].token);
    out[i].y = in[i].y;
    out[i].visible = !in[i].chip;
  }
  // Rows, left to right.
  std::vector<size_t> idx(n);
  std::iota(idx.begin(), idx.end(), size_t{0});
  std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return in[a].y < in[b].y || (in[a].y == in[b].y && in[a].cx < in[b].cx); });
  std::vector<std::vector<size_t>> rows;
  for (size_t i : idx) {
    if (!out[i].visible) continue;
    if (rows.empty() || std::fabs(in[i].y - in[rows.back().front()].y) > 6.0f * scale) rows.emplace_back();
    rows.back().push_back(i);
  }
  const float gap = 8.0f * scale;
  for (auto& row : rows) {
    std::sort(row.begin(), row.end(), [&](size_t a, size_t b) { return in[a].cx < in[b].cx; });
    // Neighbours share the space: the longer label gives way first, then both.
    for (int pass = 0; pass < 3; ++pass)
      for (size_t k = 0; k + 1 < row.size(); ++k) {
        const size_t a = row[k], b = row[k + 1];
        float excess = (w[a] + w[b]) * 0.5f + gap - (in[b].cx - in[a].cx);
        if (excess <= 0) continue;
        const size_t longer = w[a] >= w[b] ? a : b, shorter = longer == a ? b : a;
        const float give = std::min(excess * 2.0f, std::max(0.0f, w[longer] - std::max(lo[longer], w[shorter])));
        w[longer] -= give;
        excess -= give * 0.5f;
        if (excess > 0) {
          const float ga = std::min(excess, std::max(0.0f, w[a] - lo[a])), gb = std::min(excess, std::max(0.0f, w[b] - lo[b]));
          w[a] -= ga, w[b] -= gb;
        }
      }
    // Too tight: every other label drops a line, and two dropped labels that still meet: one hides.
    bool tight = false;
    for (size_t k = 0; k < row.size(); ++k) tight = tight || (w[row[k]] < 56.0f * scale && natural[row[k]] > w[row[k]] + 0.5f);
    bool collide = false;
    for (size_t k = 0; k + 1 < row.size(); ++k) collide = collide || (w[row[k]] + w[row[k + 1]]) * 0.5f + gap > in[row[k + 1]].cx - in[row[k]].cx + 0.5f;
    if (tight || collide) {
      for (size_t k = 1; k < row.size(); k += 2) {
        out[row[k]].dropped = true;
        out[row[k]].y += 14.0f * scale;
      }
      // Each line again with the room it has now (labels two apart are the new neighbours).
      for (int line = 0; line < 2; ++line) {
        std::vector<size_t> l;
        for (size_t k = static_cast<size_t>(line); k < row.size(); k += 2) l.push_back(row[k]);
        for (size_t k = 0; k < l.size(); ++k) {
          const float left = k > 0 ? in[l[k]].cx - in[l[k - 1]].cx : 1e9f, right = k + 1 < l.size() ? in[l[k + 1]].cx - in[l[k]].cx : 1e9f;
          w[l[k]] = std::max(w[l[k]], std::min({natural[l[k]], 3.2f * in[l[k]].token, std::min(left, right) - gap}));
        }
      }
    }
  }
  for (size_t i = 0; i < n; ++i) {
    out[i].text = cutLabel(in[i].name, w[i], m, names);
    out[i].w = m(out[i].text);
    float x = std::round(in[i].cx - out[i].w * 0.5f);
    const float lo2 = in[i].minX + 8.0f * scale, hi = in[i].maxX - 8.0f * scale - out[i].w;
    if (hi >= lo2) x = std::clamp(x, lo2, hi);  // labels slide inside the stage; the token never moves
    out[i].x = x;
  }
  // Labels on one line that still meet: the later one hides (its name shows on hover).
  std::vector<size_t> vis;
  for (size_t i : idx)
    if (out[i].visible) vis.push_back(i);
  for (size_t a = 0; a < vis.size(); ++a)
    for (size_t b = a + 1; b < vis.size(); ++b) {
      LabelOut& A = out[vis[a]];
      LabelOut& B = out[vis[b]];
      if (!A.visible || !B.visible || std::fabs(A.y - B.y) > 6.0f * scale) continue;
      if (A.x < B.x + B.w + 2.0f * scale && B.x < A.x + A.w + 2.0f * scale) B.visible = false;
    }
  return out;
}

std::string initials(const std::string& name) {
  const std::vector<std::string> w = words(name);
  auto up = [](char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); };
  if (w.size() >= 2) return std::string{up(w[0][0]), up(w[1][0])};
  if (w.empty()) return "?";
  std::string caps;
  for (char c : w[0])
    if (std::isupper(static_cast<unsigned char>(c)) && caps.size() < 2) caps += c;
  if (caps.size() == 2) return caps;
  return std::string{up(w[0][0])};
}

// ---------------------------------------------------------------- arranging
std::vector<Pos> spreadOut(const std::vector<Pos>& tracks, double minSpacing) {
  std::vector<Pos> out = tracks;
  // Rows: tracks at one distance (within 0.02), in the order they first appear.
  std::vector<std::vector<size_t>> rows;
  std::vector<double> rowDepth;
  for (size_t i = 0; i < tracks.size(); ++i) {
    size_t r = 0;
    while (r < rows.size() && std::fabs(rowDepth[r] - tracks[i].depth) > 0.02) ++r;
    if (r == rows.size()) rows.emplace_back(), rowDepth.push_back(tracks[i].depth);
    rows[r].push_back(i);
  }
  auto place = [&](const std::vector<size_t>& row, double depth) {
    const size_t m = row.size();
    const double sp = m > 1 ? std::min(0.3, 1.8 / static_cast<double>(m - 1)) : 0.0;
    for (size_t k = 0; k < m; ++k) {
      double pan;
      if (m % 2 == 1) {  // centre, then left and right in turn
        const double step = static_cast<double>((k + 1) / 2);
        pan = k == 0 ? 0.0 : (k % 2 == 1 ? -1.0 : 1.0) * step * sp;
      } else {
        const double step = static_cast<double>(k / 2) + 0.5;
        pan = (k % 2 == 0 ? -1.0 : 1.0) * step * sp;
      }
      out[row[k]].pan = std::clamp(pan, -1.0, 1.0);
      out[row[k]].depth = depth;
    }
  };
  for (size_t r = 0; r < rows.size(); ++r) {
    const auto& row = rows[r];
    const size_t m = row.size();
    const double sp = m > 1 ? std::min(0.3, 1.8 / static_cast<double>(m - 1)) : 1.0;
    if (m > 2 && sp < minSpacing) {  // crowded: every other track one step back (nearer at the back)
      std::vector<size_t> a, b;
      for (size_t k = 0; k < m; ++k) (k % 2 == 0 ? a : b).push_back(row[k]);
      const double d2 = rowDepth[r] + 0.12 <= 1.0 ? rowDepth[r] + 0.12 : rowDepth[r] - 0.12;
      place(a, rowDepth[r]);
      place(b, std::clamp(d2, 0.0, 1.0));
    } else {
      place(row, rowDepth[r]);
    }
  }
  return out;
}

// ---------------------------------------------------------------- words
std::string panWords(double pan) {
  const int v = static_cast<int>(std::lround(std::fabs(pan) * 100.0));
  if (v == 0) return "Centre";
  char b[32];
  std::snprintf(b, sizeof b, "%d%% %s", v, pan < 0 ? "left" : "right");
  return b;
}

const char* distanceWord(double d) {
  return d < 0.125 ? i18n::N_("Front") : d < 0.375 ? i18n::N_("Near") : d < 0.625 ? i18n::N_("Middle") : d < 0.875 ? i18n::N_("Far") : i18n::N_("Back");
}

const char* distanceHint(double d) {
  return d < 0.125 ? i18n::N_("close and clear")
         : d < 0.375 ? i18n::N_("close, a little room")
         : d < 0.625 ? i18n::N_("in the middle, more room")
         : d < 0.875 ? i18n::N_("softer, more room")
                     : i18n::N_("distant and soft");
}

const char* widthWord(double sep) {
  return sep <= -0.95 ? i18n::N_("Mono")
         : sep < -0.5 ? i18n::N_("Narrow")
         : sep < -0.15 ? i18n::N_("Medium")
         : sep <= 0.15 ? i18n::N_("Natural")
         : sep <= 0.6 ? i18n::N_("Wide")
                      : i18n::N_("Very wide");
}

const char* focusWord(double f) { return f < -0.15 ? i18n::N_("Soft") : f <= 0.15 ? i18n::N_("Natural") : i18n::N_("Present"); }

const char* focusHint(double f) {
  return f < -0.15 ? i18n::N_("softer, more room") : f <= 0.15 ? i18n::N_("as it is") : i18n::N_("dry and upfront");
}

const char* strengthWord(double s) { return s < 0.33 ? i18n::N_("Gentle") : s <= 0.67 ? i18n::N_("Natural") : i18n::N_("Strong"); }

const char* roomSizeWord(double s) {
  return s < 0.3 ? i18n::N_("Small") : s < 0.55 ? i18n::N_("Medium") : s < 0.8 ? i18n::N_("Large") : i18n::N_("Hall");
}

const char* stageWidthWord(double sep) {
  return sep <= -0.6 ? i18n::N_("Narrow") : sep < -0.15 ? i18n::N_("Medium") : sep <= 0.15 ? i18n::N_("Wide") : i18n::N_("Very wide");
}

double widthPercent(double sep) { return 100.0 * (1.0 + std::clamp(sep, -1.0, 1.0)); }

double levelDb(double volume) { return volume > 1e-4 ? 40.0 * std::log10(volume / 0.8) : -INFINITY; }

const char* moveDepthLine(double fromD, double toD) {
  if (toD > fromD + 0.02) return i18n::N_("Further back: quieter, more room");
  if (toD < fromD - 0.02) return i18n::N_("Nearer: louder and clearer");
  return i18n::N_("Same distance");
}

const char* moveSideLine(double fromPan, double toPan) {
  if (toPan > fromPan + 0.02) return i18n::N_("More to the right");
  if (toPan < fromPan - 0.02) return i18n::N_("More to the left");
  return "";
}

}  // namespace gl::stage
