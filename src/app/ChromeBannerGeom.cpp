// Gridline - K-315: the Tracker About banner's geometry and timing (see ChromeBannerGeom.h).
#include "ChromeBannerGeom.h"

#include <algorithm>
#include <chrono>
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


// A smooth window over a segment: 0 at both ends, 1 in the middle, eased in over the first `edge`
// of it and out over the last.
float window(float u, float edge) { return ease(u / edge) * ease((1 - u) / edge); }

}  // namespace

float ease(float u) {
  u = std::clamp(u, 0.0f, 1.0f);
  return u * u * u * (u * (u * 6 - 15) + 10);
}

const std::vector<Segment>& segments() {
  // Spaced out (round 2), transitions twice as fast (round 3): each state holds for several
  // seconds at the round 2 speed (its sway, turn, wriggle and twist unchanged); each change (a
  // morph from one shape to another) takes half its round 2 time, 1.5 to 3.5 s. Every move still
  // eases in and out (quintic: at rest at both ends), so the shorter changes start and stop as
  // softly as before.
  //                         name          s    from              to            turns sway   tilt   twist wire sprF  sprT  stag
  static const std::vector<Segment> s = {
      {"spin in", 3.5f, Shape::Particles, Shape::Text, 1, 0, 0, 0, 0, 22, 22, 0.35f},
      {"hold", 7.0f, Shape::Text, Shape::Text, 0, 0.35f, 0.06f, 0, 0, 0, 0, 0},
      {"dissolve", 2.0f, Shape::Text, Shape::Worms, 0, 0, 0.08f, 0, 0, 0, 0, 0.45f},
      {"worms", 7.0f, Shape::Worms, Shape::Worms, 0, 0.25f, 0.2f, 0, 0, 0, 0, 0},
      {"reform", 2.0f, Shape::Worms, Shape::Text, 0, 0, 0.08f, 0, 0, 0, 0, 0.45f},
      {"hold", 4.0f, Shape::Text, Shape::Text, 0, 0.25f, 0, 0, 0, 0, 0, 0},
      {"to logo", 1.75f, Shape::Text, Shape::Logo, 0, 0, 0, 0, 0, 0, 0, 0.3f},
      {"logo", 8.0f, Shape::Logo, Shape::Logo, 1, 0, 0.1f, 0, 0, 0, 0, 0},
      {"explode", 1.5f, Shape::Logo, Shape::Particles, 0, 0, 0, 0, 0, 4, 14, 0.1f},
      {"drift", 5.0f, Shape::Particles, Shape::Particles, 0, 0.3f, 0, 0, 0, 14, 16, 0},
      {"gather", 2.0f, Shape::Particles, Shape::Text, 0, 0, 0, 0, 0, 16, 16, 0.3f},
      {"twist", 6.0f, Shape::Text, Shape::Text, 0, 0, 0, 1.0f, 0, 0, 0, 0},
      {"wireframe", 7.0f, Shape::Text, Shape::Text, 1, 0, 0.12f, 0, 1, 0, 0, 0},
      {"shatter", 1.75f, Shape::Text, Shape::Particles, 0, 0, 0, 0, 0, 4, 22, 0.3f},
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
  return s.wire * window(w.u, 0.2f);
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
  if (s == Shape::Worms) {  // slow sine worms, kept inside the letters' height
    constexpr int W = 10;
    for (size_t i = 0; i < K; ++i) {
      const int w = static_cast<int>(i % W);
      const float ph = static_cast<float>(w) * 1.7f;
      const float yo = -2.9f + 5.8f * static_cast<float>(w) / (W - 1);
      auto at = [&](float u) {
        return V3{-17.0f + 34.0f * u, yo + 1.0f * std::sin(6.0f * u + 0.9f * tf + ph) + 0.35f * std::sin(13.0f * u - 1.2f * tf + 2 * ph),
                  3.0f * std::cos(4.5f * u + 0.7f * tf + ph)};
      };
      const float u0 = m.delay[i] * 0.96f, u1 = u0 + 0.04f;
      const float th = 9.0f * u0 + 0.8f * tf + ph;
      const V3 n{0, 0.26f * std::cos(th), 0.26f * std::sin(th)};
      const V3 a = at(u0), b = at(u1);
      out[i] = Facet{{a - n, b - n, b + n, a + n}};
    }
    return;
  }
  // Particles: a small chip per facet on its own direction, turning as it flies. The cloud is a
  // flat, wide ellipsoid (the banner is wide and low), so it fills the banner without spilling.
  for (size_t i = 0; i < K; ++i) {
    const V3 d = m.dir[i];
    const float r = spread * (0.55f + 0.45f * m.rnd[i]);
    const V3 c{d.x * r, d.y * r * 0.2f, d.z * r * 0.45f};
    const V3 ref = std::fabs(d.y) < 0.9f ? V3{0, 1, 0} : V3{1, 0, 0};
    V3 a = norm(cross(d, ref)), b = cross(d, a);
    const float ang = m.rnd[i] * 6.2832f + spread * 0.2f, ca = std::cos(ang), sa = std::sin(ang);
    const V3 a2 = a * ca + b * sa, b2 = b * ca - a * sa;
    const float sz = 0.32f;
    a = a2 * sz, b = b2 * sz;
    out[i] = Facet{{c - a - b, c + a - b, c + a + b, c - a + b}};
  }
}

void evaluate(const Model& m, double t, Facet* out, Facet* scratch) {
  const Where w = where(t);
  const Segment& s = segments()[static_cast<size_t>(w.segment)];
  const float u = w.u, e = ease(u);
  const float spread = s.spreadFrom + (s.spreadTo - s.spreadFrom) * e;
  const size_t K = static_cast<size_t>(m.K);
  shape(m, s.from, t, spread, out);
  if (s.to != s.from) {
    shape(m, s.to, t, spread, scratch);
    const float st = s.stagger;
    for (size_t i = 0; i < K; ++i) {
      const float b = ease((u - st * m.delay[i]) / (1 - st));
      if (b <= 0) continue;
      for (int k = 0; k < 4; ++k) out[i].v[k] = out[i].v[k] + (scratch[i].v[k] - out[i].v[k]) * b;
    }
  }
  // Every amount rides the eased progress, so each starts and ends at rest (no jerk at a boundary).
  const float tw = s.twist * std::sin(kPi * e);
  const float ry = s.turns * 2 * kPi * e + s.sway * std::sin(2 * kPi * e);
  const float rx = s.tilt * std::sin(kPi * e);
  const float cy = std::cos(ry), sy = std::sin(ry), cx = std::cos(rx), sx = std::sin(rx);
  // While the letters turn, they ease back a little: the near end of the text would otherwise
  // swell out of the banner's height side on.
  const float dolly = s.to == Shape::Text && s.turns > 0 ? 14.0f * std::sin(kPi * e) : 0.0f;
  for (size_t i = 0; i < K; ++i)
    for (V3& v : out[i].v) {
      if (tw != 0) {  // a slow wave of twist about the long axis, running left to right
        const float a = tw * std::sin(0.32f * v.x - 3 * kPi * u);
        const float c = std::cos(a), sn = std::sin(a);
        v = {v.x, v.y * c - v.z * sn, v.y * sn + v.z * c};
      }
      // The lean first (in the shape's own frame), then the turn: a turn never swings the ends of
      // the text up or down out of the banner.
      v = {v.x, v.y * cx - v.z * sx, v.y * sx + v.z * cx};
      v = {v.x * cy + v.z * sy, v.y, -v.x * sy + v.z * cy - dolly};
    }
}

namespace {
struct Box {
  float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
};
Box viewBox(const Facet* f, int K) {
  Box b;
  for (int i = 0; i < K; ++i)
    for (const V3& v : f[i].v) {
      const float d = std::max(kCamD - v.z, 1.0f), x = v.x / d, y = v.y / d;
      b.x0 = std::min(b.x0, x), b.x1 = std::max(b.x1, x), b.y0 = std::min(b.y0, y), b.y1 = std::max(b.y1, y);
    }
  return b;
}
}  // namespace

const Extent& loopExtent() {
  static const Extent e = [] {
    const auto t0 = std::chrono::steady_clock::now();
    const Model& m = model();
    std::vector<Facet> a(static_cast<size_t>(m.K)), sc(static_cast<size_t>(m.K));
    Extent x;
    const double L = loopSeconds(), step = 0.4;
    for (double t = 0; t < L; t += step) {
      evaluate(m, t, a.data(), sc.data());
      const Box b = viewBox(a.data(), m.K);
      x.halfW = std::max(x.halfW, (b.x1 - b.x0) * 0.5f), x.halfH = std::max(x.halfH, (b.y1 - b.y0) * 0.5f);
    }
    // A little head room for the moments between samples.
    x.halfW *= 1.02f, x.halfH *= 1.02f;
    x.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return x;
  }();
  return e;
}

float marginX(float w) { return std::round(std::max(8.0f, w * 0.03f)); }
float marginY(float h) { return std::round(std::max(4.0f, h * 0.07f)); }

ScreenMap screenMap(const Facet* world, int K, float w, float h) {
  const Extent& e = loopExtent();
  const float aw = std::max(1.0f, w * 0.5f - marginX(w)), ah = std::max(1.0f, h * 0.5f - marginY(h));
  ScreenMap s;
  s.f = std::min(aw / e.halfW, ah / e.halfH);
  const Box b = viewBox(world, K);
  const float hw = (b.x1 - b.x0) * 0.5f, hh = (b.y1 - b.y0) * 0.5f;
  if (hw * s.f > aw || hh * s.f > ah) {  // wider than any sample of the loop: shrink this frame to fit
    s.f = std::min(hw > 0 ? aw / hw : s.f, hh > 0 ? ah / hh : s.f);
    s.clamped = true;
  }
  s.cx = w * 0.5f - s.f * (b.x0 + b.x1) * 0.5f;
  s.cy = h * 0.5f + s.f * (b.y0 + b.y1) * 0.5f;
  return s;
}

}  // namespace gl::banner
