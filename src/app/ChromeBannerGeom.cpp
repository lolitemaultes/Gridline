// Gridline - K-315: the Tracker About banner's geometry and timing (see ChromeBannerGeom.h).
#include "ChromeBannerGeom.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace gl::banner {
namespace {

constexpr float kPi = 3.14159265358979f;

V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 norm(V3 a) {
  const float l = std::sqrt(dot(a, a));
  return l > 1e-9f ? a * (1.0f / l) : V3{0, 0, 1};
}
float smooth(float u) {
  u = std::clamp(u, 0.0f, 1.0f);
  return u * u * (3 - 2 * u);
}

// ---- cell grids: each cell empty, full, or a diagonal half (the corner named is the filled one) ----
// '#': full; 'A': upper left; 'B': upper right; 'C': lower left; 'D': lower right; anything else empty.
struct Grid {
  int cols = 0, rows = 0;
  std::vector<float> xs, ys;  // cols + 1 and rows + 1 edges; ys run upwards (row 0 is the bottom)
  std::vector<char> cell;     // rows * cols, row 0 the bottom
  char at(int c, int r) const { return c < 0 || r < 0 || c >= cols || r >= rows ? '.' : cell[static_cast<size_t>(r * cols + c)]; }
};

// Which sides of a cell are solid along their whole length: 0 left, 1 bottom, 2 right, 3 top.
bool sideSolid(char k, int side) {
  switch (k) {
    case '#': return true;
    case 'A': return side == 0 || side == 3;
    case 'B': return side == 2 || side == 3;
    case 'C': return side == 0 || side == 1;
    case 'D': return side == 2 || side == 1;
    default: return false;
  }
}

enum Kind { kFront = 0, kBack = 1, kWall = 2 };
struct Built {
  Facet f;
  int kind;
  float cx, cy;
};

// A quad (or triangle: d == c) turned so its normal points along `out`.
Facet oriented(V3 a, V3 b, V3 c, V3 d, V3 out) {
  const V3 n = cross(c - a, d - b);
  if (dot(n, out) < 0) return Facet{{a, d, c, b}};
  return Facet{{a, b, c, d}};
}

void extrude(const Grid& g, float depth, std::vector<Built>& out) {
  const float zf = depth * 0.5f, zb = -depth * 0.5f;
  for (int r = 0; r < g.rows; ++r)
    for (int c = 0; c < g.cols; ++c) {
      const char k = g.at(c, r);
      if (k != '#' && k != 'A' && k != 'B' && k != 'C' && k != 'D') continue;
      const float x0 = g.xs[static_cast<size_t>(c)], x1 = g.xs[static_cast<size_t>(c + 1)];
      const float y0 = g.ys[static_cast<size_t>(r)], y1 = g.ys[static_cast<size_t>(r + 1)];
      const float BL[2] = {x0, y0}, BR[2] = {x1, y0}, TR[2] = {x1, y1}, TL[2] = {x0, y1};
      const float* poly[4];
      int n = 0;
      switch (k) {  // counter-clockwise seen from the front (+z)
        case '#': poly[0] = BL, poly[1] = BR, poly[2] = TR, poly[3] = TL, n = 4; break;
        case 'A': poly[0] = BL, poly[1] = TR, poly[2] = TL, n = 3; break;
        case 'B': poly[0] = TL, poly[1] = BR, poly[2] = TR, n = 3; break;
        case 'C': poly[0] = BL, poly[1] = BR, poly[2] = TL, n = 3; break;
        default: poly[0] = BL, poly[1] = BR, poly[2] = TR, n = 3; break;
      }
      auto P = [&](int i, float z) { return V3{poly[i % n][0], poly[i % n][1], z}; };
      const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
      const int last = n == 4 ? 3 : 2;
      out.push_back({oriented(P(0, zf), P(1, zf), P(2, zf), P(last, zf), {0, 0, 1}), kFront, cx, cy});
      out.push_back({oriented(P(0, zb), P(1, zb), P(2, zb), P(last, zb), {0, 0, -1}), kBack, cx, cy});
      for (int i = 0; i < n; ++i) {
        const float* p = poly[i];
        const float* q = poly[(i + 1) % n];
        int side = -1;  // a cell side, or the diagonal (-1)
        if (p[0] == x0 && q[0] == x0) side = 0;
        else if (p[1] == y0 && q[1] == y0) side = 1;
        else if (p[0] == x1 && q[0] == x1) side = 2;
        else if (p[1] == y1 && q[1] == y1) side = 3;
        if (side >= 0) {
          static const int dc[4] = {-1, 0, 1, 0}, dr[4] = {0, -1, 0, 1};
          if (sideSolid(g.at(c + dc[side], r + dr[side]), (side + 2) % 4)) continue;  // joined to its neighbour
        }
        const V3 a{p[0], p[1], zf}, b{q[0], q[1], zf}, cB{q[0], q[1], zb}, d{p[0], p[1], zb};
        const V3 outward{q[1] - p[1], -(q[0] - p[0]), 0};
        out.push_back({oriented(a, b, cB, d, outward), kWall, (p[0] + q[0]) * 0.5f, (p[1] + q[1]) * 0.5f});
      }
    }
}

// The letters, top row first (9 rows: 2 ascender, 5 x-height, 2 descender), drawn for Gridline.
struct Glyph {
  int w;
  const char* rows[9];
};
const Glyph kGlyphs[] = {
    {5, {".....", ".....", "D####", "#...#", "#...#", "#...#", "B####", "....#", "####A"}},  // g
    {4, {"....", "....", "#D#C", "#...", "#...", "#...", "#...", "....", "...."}},           // r
    {1, {"#", ".", "#", "#", "#", "#", "#", ".", "."}},                                    // i
    {5, {"....#", "....#", "D####", "#...#", "#...#", "#...#", "B####", ".....", "....."}},  // d
    {1, {"#", "#", "#", "#", "#", "#", "#", ".", "."}},                                    // l
    {1, {"#", ".", "#", "#", "#", "#", "#", ".", "."}},                                    // i
    {5, {".....", ".....", "#D##C", "#...#", "#...#", "#...#", "#...#", ".....", "....."}},  // n
    {5, {".....", ".....", "D###C", "#...#", "#####", "#....", "B###A", ".....", "....."}},  // e
};

Grid textGrid() {
  Grid g;
  g.rows = 9;
  for (const Glyph& gy : kGlyphs) g.cols += gy.w + 1;
  g.cols -= 1;
  g.cell.assign(static_cast<size_t>(g.rows * g.cols), '.');
  int x = 0;
  for (const Glyph& gy : kGlyphs) {
    for (int r = 0; r < 9; ++r)
      for (int c = 0; c < gy.w; ++c) g.cell[static_cast<size_t>((8 - r) * g.cols + x + c)] = gy.rows[r][c];
    x += gy.w + 1;
  }
  for (int c = 0; c <= g.cols; ++c) g.xs.push_back(static_cast<float>(c) - g.cols * 0.5f);
  for (int r = 0; r <= g.rows; ++r) g.ys.push_back(static_cast<float>(r) - g.rows * 0.5f);
  return g;
}

// The Gridline mark: a 120u tile, four 10u bars centred at 40u and 80u inset 24u, cut through it;
// the corners cut on the diagonal (the low-res take on its 28u rounding). 9 units tall.
Grid logoGrid() {
  Grid g;
  const float e[] = {0, 12, 24, 35, 45, 60, 75, 85, 96, 108, 120};
  g.cols = g.rows = 10;
  const float s = 9.0f / 120.0f;
  for (float v : e) g.xs.push_back((v - 60) * s), g.ys.push_back((v - 60) * s);
  g.cell.assign(100, '#');
  auto inBar = [](float v) { return (v > 35 && v < 45) || (v > 75 && v < 85); };
  for (int r = 0; r < 10; ++r)
    for (int c = 0; c < 10; ++c) {
      const float cx = (e[c] + e[c + 1]) * 0.5f, cy = (e[r] + e[r + 1]) * 0.5f;
      const bool inside = cx > 24 && cx < 96 && cy > 24 && cy < 96;
      if (inside && (inBar(cx) || inBar(cy))) g.cell[static_cast<size_t>(r * 10 + c)] = '.';
    }
  g.cell[0] = 'B', g.cell[9] = 'A', g.cell[90] = 'D', g.cell[99] = 'C';  // bottom left, bottom right, top left, top right
  return g;
}

uint32_t hash(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}
float rnd01(uint32_t i, uint32_t salt) { return static_cast<float>(hash(i * 2654435761u + salt) & 0xFFFFFF) / 16777215.0f; }

Model build() {
  std::vector<Built> t, l;
  extrude(textGrid(), 2.4f, t);
  extrude(logoGrid(), 2.8f, l);
  Model m;
  m.textFacets = static_cast<int>(t.size());
  m.logoFacets = static_cast<int>(l.size());
  auto order = [](const Built& a, const Built& b) {
    if (a.kind != b.kind) return a.kind < b.kind;
    if (a.cx != b.cx) return a.cx < b.cx;
    return a.cy < b.cy;
  };
  std::stable_sort(t.begin(), t.end(), order);
  std::stable_sort(l.begin(), l.end(), order);
  // Per kind (fronts to fronts, backs to backs, walls to walls), the smaller list padded with
  // copies spread evenly through it, so both have the same count and run left to right together.
  for (int kind = 0; kind < 3; ++kind) {
    std::vector<const Built*> tk, lk;
    for (const Built& b : t)
      if (b.kind == kind) tk.push_back(&b);
    for (const Built& b : l)
      if (b.kind == kind) lk.push_back(&b);
    const size_t n = std::max(tk.size(), lk.size());
    for (size_t i = 0; i < n && !tk.empty() && !lk.empty(); ++i) {
      const Built* a = tk[i * tk.size() / n];
      const Built* b = lk[i * lk.size() / n];
      m.text.push_back(a->f);
      m.logo.push_back(b->f);
      m.delay.push_back(std::clamp((a->cx + kTextW * 0.5f) / kTextW, 0.0f, 1.0f));
    }
  }
  m.K = static_cast<int>(m.text.size());
  for (int i = 0; i < m.K; ++i) {
    const uint32_t u = static_cast<uint32_t>(i);
    const float z = rnd01(u, 11) * 2 - 1, a = rnd01(u, 23) * 2 * kPi, r = std::sqrt(std::max(0.0f, 1 - z * z));
    m.dir.push_back({r * std::cos(a), r * std::sin(a), z});
    m.rnd.push_back(rnd01(u, 37));
  }
  return m;
}

// The segments' eased progress through their turns.
float spinEase(const Segment& s, float u) { return s.easeOutSpin ? 1 - (1 - u) * (1 - u) * (1 - u) : smooth(u); }

}  // namespace

const std::vector<Segment>& segments() {
  //                         name         s     from            to            turns sway  tilt  twist wire sprF  sprT stag  easeOut
  static const std::vector<Segment> s = {
      {"spin in", 2.4f, Shape::Particles, Shape::Text, 2, 0, 0, 0, 0, 28, 28, 0.35f, true},
      {"hold", 2.0f, Shape::Text, Shape::Text, 0, 0.38f, 0.08f, 0, 0, 0, 0, 0, false},
      {"dissolve", 1.2f, Shape::Text, Shape::Worms, 0, 0, 0.1f, 0, 0, 0, 0, 0.45f, false},
      {"worms", 2.8f, Shape::Worms, Shape::Worms, 0, 0.3f, 0.3f, 0, 0, 0, 0, 0, false},
      {"reform", 1.2f, Shape::Worms, Shape::Text, 0, 0, 0.1f, 0, 0, 0, 0, 0.45f, false},
      {"hold", 1.2f, Shape::Text, Shape::Text, 0, 0.3f, 0, 0, 0, 0, 0, 0, false},
      {"to logo", 0.5f, Shape::Text, Shape::Logo, 0, 0, 0, 0, 0, 0, 0, 0.2f, false},
      {"logo spin", 3.0f, Shape::Logo, Shape::Logo, 2, 0, 0.18f, 0, 0, 0, 0, 0, false},
      {"explode", 0.8f, Shape::Logo, Shape::Particles, 0, 0, 0, 0, 0, 4, 14, 0.1f, false},
      {"drift", 1.0f, Shape::Particles, Shape::Particles, 0, 0.3f, 0, 0, 0, 14, 18, 0, false},
      {"gather", 1.0f, Shape::Particles, Shape::Text, 0, 0, 0, 0, 0, 18, 18, 0.3f, false},
      {"twist", 3.0f, Shape::Text, Shape::Text, 0, 0, 0, 1.3f, 0, 0, 0, 0, false},
      {"wireframe", 2.6f, Shape::Text, Shape::Text, 1, 0, 0.25f, 0, 1, 0, 0, 0, false},
      {"shatter", 1.0f, Shape::Text, Shape::Particles, 0, 0, 0, 0, 0, 4, 28, 0.3f, false},
  };
  return s;
}

double loopSeconds() {
  static const double L = [] {
    double l = 0;
    for (const Segment& s : segments()) l += s.seconds;
    return l;
  }();
  return L;
}

Where where(double t) {
  const double L = loopSeconds();
  t = std::fmod(t, L);
  if (t < 0) t += L;
  const auto& segs = segments();
  double start = 0;
  for (size_t i = 0; i < segs.size(); ++i) {
    const double end = start + segs[i].seconds;
    if (t < end || i + 1 == segs.size())
      return {static_cast<int>(i), static_cast<float>(std::clamp((t - start) / segs[i].seconds, 0.0, 1.0))};
    start = end;
  }
  return {};
}

double nextStart(double t) {
  const double L = loopSeconds();
  const double base = std::floor(t / L) * L;
  const Where w = where(t);
  double start = base;
  for (int i = 0; i <= w.segment; ++i) start += segments()[static_cast<size_t>(i)].seconds;
  return start;
}

float wireAt(double t) {
  const Where w = where(t);
  const Segment& s = segments()[static_cast<size_t>(w.segment)];
  if (s.wire <= 0) return 0;
  return s.wire * smooth(w.u / 0.15f) * smooth((1 - w.u) / 0.15f);
}

bool settledAt(double t) {
  const Segment& s = segments()[static_cast<size_t>(where(t).segment)];
  return s.from == s.to && (s.from == Shape::Text || s.from == Shape::Logo);
}

const Model& model() {
  static const Model m = build();
  return m;
}

void shape(const Model& m, Shape s, double t, float spread, Facet* out) {
  const size_t K = static_cast<size_t>(m.K);
  if (s == Shape::Text || s == Shape::Logo) {
    const std::vector<Facet>& src = s == Shape::Text ? m.text : m.logo;
    std::copy(src.begin(), src.end(), out);
    return;
  }
  const float tf = static_cast<float>(t);
  if (s == Shape::Worms) {
    constexpr int W = 10;
    for (size_t i = 0; i < K; ++i) {
      const int w = static_cast<int>(i % W);
      const float ph = static_cast<float>(w) * 1.7f;
      const float yo = -4.2f + 8.4f * static_cast<float>(w) / (W - 1);
      auto at = [&](float u) {
        return V3{-18.0f + 36.0f * u, yo + 1.9f * std::sin(6.0f * u + 2.3f * tf + ph) + 0.7f * std::sin(13.0f * u - 3.1f * tf + 2 * ph),
                  3.2f * std::cos(4.5f * u + 1.7f * tf + ph)};
      };
      const float u0 = m.delay[i] * 0.96f, u1 = u0 + 0.04f;
      const float th = 9.0f * u0 + 2.0f * tf + ph;
      const V3 n{0, 0.26f * std::cos(th), 0.26f * std::sin(th)};
      const V3 a = at(u0), b = at(u1);
      out[i] = Facet{{a - n, b - n, b + n, a + n}};
    }
    return;
  }
  // Particles: a small chip per facet on its own direction, turning as it flies.
  for (size_t i = 0; i < K; ++i) {
    const V3 d = m.dir[i];
    const V3 c = d * (spread * (0.55f + 0.45f * m.rnd[i]));
    const V3 ref = std::fabs(d.y) < 0.9f ? V3{0, 1, 0} : V3{1, 0, 0};
    V3 a = norm(cross(d, ref)), b = cross(d, a);
    const float ang = m.rnd[i] * 6.2832f + spread * 0.35f, ca = std::cos(ang), sa = std::sin(ang);
    const V3 a2 = a * ca + b * sa, b2 = b * ca - a * sa;
    const float sz = 0.32f;
    a = a2 * sz, b = b2 * sz;
    out[i] = Facet{{c - a - b, c + a - b, c + a + b, c - a + b}};
  }
}

void evaluate(const Model& m, double t, Facet* out, Facet* scratch) {
  const Where w = where(t);
  const Segment& s = segments()[static_cast<size_t>(w.segment)];
  const float u = w.u;
  const float spread = s.spreadFrom + (s.spreadTo - s.spreadFrom) * u;
  const size_t K = static_cast<size_t>(m.K);
  shape(m, s.from, t, spread, out);
  if (s.to != s.from) {
    shape(m, s.to, t, spread, scratch);
    const float st = s.stagger;
    for (size_t i = 0; i < K; ++i) {
      const float b = smooth((u - st * m.delay[i]) / (1 - st));
      if (b <= 0) continue;
      for (int k = 0; k < 4; ++k) out[i].v[k] = out[i].v[k] + (scratch[i].v[k] - out[i].v[k]) * b;
    }
  }
  const float tw = s.twist * std::sin(kPi * u);
  const float ry = s.turns * 2 * kPi * spinEase(s, u) + s.sway * std::sin(2 * kPi * u);
  const float rx = s.tilt * std::sin(kPi * u);
  const float cy = std::cos(ry), sy = std::sin(ry), cx = std::cos(rx), sx = std::sin(rx);
  for (size_t i = 0; i < K; ++i)
    for (V3& v : out[i].v) {
      if (tw != 0) {  // a wave of twist about the long axis, running left to right
        const float a = tw * std::sin(0.32f * v.x - 4 * kPi * u);
        const float c = std::cos(a), sn = std::sin(a);
        v = {v.x, v.y * c - v.z * sn, v.y * sn + v.z * c};
      }
      v = {v.x * cy + v.z * sy, v.y, -v.x * sy + v.z * cy};
      v = {v.x, v.y * cx - v.z * sx, v.y * sx + v.z * cx};
    }
}

}  // namespace gl::banner
