// Gridline - K-315: the Tracker About banner (see ChromeBanner.h).
#include "ChromeBanner.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "../../third_party/imgui/imgui_internal.h"
#include "ChromeBannerGeom.h"
#include "SmoothScrollUi.h"
#include "Widgets.h"

namespace gl::chromebanner {
namespace {

using banner::Facet;
using banner::V3;
using banner::kCamD;
using Clock = std::chrono::steady_clock;

constexpr int kMaxEdges = 4;         // per facet in the wireframe
constexpr double kSkipSeconds = 0.8; // a click's glide from where the banner was to the next state (a transition: 2x round 2)

V3 sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 norm(V3 a) {
  const float l = std::sqrt(dot(a, a));
  return l > 1e-9f ? V3{a.x / l, a.y / l, a.z / l} : V3{0, 0, 1};
}
float lerp(float a, float b, float u) { return a + (b - a) * u; }

struct State {
  bool built = false;
  std::vector<Facet> world, scratch, from;  // from: where a click's glide starts
  std::vector<float> depth;
  std::vector<int> order;
  std::vector<ImVec2> scr;      // 4 per facet, in banner pixels (relative to p0)
  std::vector<ImU32> col;       // 4 per facet
  std::vector<ImDrawVert> solid, lines;  // the cache, relative to p0 (moved on emit)
  double t = 0;                 // the banner's own loop clock (runs only while it is drawn)
  double bars = 0;              // the copper bars' clock (a click does not jump it)
  bool frozen = false;          // dev: held at t
  int staticIdx = 0;            // reduced motion: which still frame
  double glide = -1;            // seconds into a click's glide (< 0: none)
  float wireFrom = 0;           // the wireframe amount the glide starts from
  float curWire = 0;            // the wireframe amount on screen
  double lastShown = -1;        // the time the cache was last worked out for
  double lastGlide = -1;
  ImVec2 lastSize{0, 0};
  Clock::time_point lastDraw{};
  bool animating = false;
  // Measured cost (in-app counters; never a profiler).
  double buildMs = 0, fitMs = 0;
  long long draws = 0, updates = 0, clamped = 0;
  double drawSum = 0, drawMax = 0, updSum = 0, updMax = 0;
};
State& st() {
  static State s;
  return s;
}

void buildOnce(State& s) {
  if (s.built) return;
  const Clock::time_point t0 = Clock::now();
  const banner::Model& m = banner::model();
  const size_t K = static_cast<size_t>(m.K);
  s.world.resize(K);
  s.scratch.resize(K);
  s.from.resize(K);
  s.depth.resize(K);
  s.order.resize(K);
  s.scr.resize(K * 4);
  s.col.resize(K * 4);
  s.solid.reserve(K * 4);
  s.lines.reserve(K * 4 * kMaxEdges);
  s.buildMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  s.fitMs = banner::loopExtent().ms;  // the loop's widest frame, sampled once
  s.built = true;
}

// The chrome: the reflected ray's height looks up a sky over a hard dark horizon over a warm ground.
ImU32 chrome(float e, float lit, float spec, float alpha) {
  e = std::clamp(e, -1.0f, 1.0f);
  float r, g, b;
  if (e > 0.0f) {  // sky: a steel blue grey at the horizon to white overhead
    const float k = std::pow(e, 0.6f);
    r = lerp(84, 255, k), g = lerp(98, 255, k), b = lerp(128, 255, k);
  } else if (e > -0.08f) {  // the hard dark horizon line
    r = 22, g = 22, b = 30;
  } else {  // ground: a bright warm silver just under the horizon, darker further down
    const float k = std::min(1.0f, (-e - 0.08f) / 0.92f);
    r = lerp(226, 116, k), g = lerp(214, 106, k), b = lerp(196, 98, k);
  }
  const float add = spec * 255.0f;
  auto ch = [&](float v) { return static_cast<int>(std::clamp(v * lit + add, 0.0f, 255.0f)); };
  return IM_COL32(ch(r), ch(g), ch(b), static_cast<int>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f));
}

void quadVerts(std::vector<ImDrawVert>& out, const ImVec2* p, const ImU32* c, ImVec2 uv) {
  for (int k = 0; k < 4; ++k) out.push_back(ImDrawVert{p[k], uv, c[k]});
}

// Works out the frame at time t for a banner of size sz into the vertex cache. `glide` (0..1, or
// below 0 for none) blends from the frame a click left (s.from) into this one.
void update(State& s, double t, ImVec2 sz, ImVec2 uv, float glide) {
  const banner::Model& m = banner::model();
  const int K = m.K;
  banner::evaluate(m, t, s.world.data(), s.scratch.data());
  bool settled = banner::settledAt(t);
  float wire = banner::wireAt(t);
  if (glide >= 0 && glide < 1) {  // every facet corresponds in every shape: a per-corner blend
    const float e = banner::ease(glide);
    for (size_t i = 0; i < s.world.size(); ++i)
      for (int k = 0; k < 4; ++k) {
        V3& v = s.world[i].v[k];
        const V3 a = s.from[i].v[k];
        v = {lerp(a.x, v.x, e), lerp(a.y, v.y, e), lerp(a.z, v.z, e)};
      }
    wire = lerp(s.wireFrom, wire, e);
    settled = false;
  }
  s.curWire = wire;
  // One scale for the whole loop (the widest state fits, with a margin); each frame centred.
  const banner::ScreenMap sm = banner::screenMap(s.world.data(), K, sz.x, sz.y);
  if (sm.clamped) ++s.clamped;
  const V3 cam{0, 0, kCamD};
  const V3 L = norm({-0.45f, 0.75f, 0.55f});
  int n = 0;
  for (int i = 0; i < K; ++i) {
    const Facet& fa = s.world[static_cast<size_t>(i)];
    V3 nrm = norm(cross(sub(fa.v[2], fa.v[0]), sub(fa.v[3], fa.v[1])));
    const V3 c{(fa.v[0].x + fa.v[1].x + fa.v[2].x + fa.v[3].x) * 0.25f, (fa.v[0].y + fa.v[1].y + fa.v[2].y + fa.v[3].y) * 0.25f,
               (fa.v[0].z + fa.v[1].z + fa.v[2].z + fa.v[3].z) * 0.25f};
    const V3 toC = sub(c, cam);
    if (dot(nrm, toC) > 0) {
      if (settled && wire <= 0) continue;  // a closed solid: its back faces stay hidden
      nrm = {-nrm.x, -nrm.y, -nrm.z};
    }
    bool behind = false;
    for (int k = 0; k < 4; ++k) {
      const V3 v = fa.v[k];
      const float d = kCamD - v.z;
      if (d < 1.0f) behind = true;
      const float id = 1.0f / std::max(d, 1.0f);
      // Sub-pixel corners (no grid snap): slow motion glides instead of stepping a pixel at a time.
      s.scr[static_cast<size_t>(i * 4 + k)] = ImVec2(sm.cx + sm.f * v.x * id, sm.cy - sm.f * v.y * id);
      const V3 V = norm(sub(v, cam));
      const float vn = dot(V, nrm);
      const V3 R{V.x - 2 * vn * nrm.x, V.y - 2 * vn * nrm.y, V.z - 2 * vn * nrm.z};
      const float e = R.y * 2.0f + R.x * 0.3f + v.y * (0.5f / 4.5f);  // plus a sweep down the letters: the classic chrome band
      const float sp = std::pow(std::max(0.0f, dot(R, L)), 28.0f) * 0.85f;
      const float lit = 0.62f + 0.38f * std::max(0.0f, dot(nrm, L));
      s.col[static_cast<size_t>(i * 4 + k)] = chrome(e, lit, sp, 1.0f - wire);
    }
    if (behind) continue;
    s.depth[static_cast<size_t>(i)] = dot(toC, toC);
    s.order[static_cast<size_t>(n++)] = i;
  }
  // Painter's order: the farthest first.
  std::sort(s.order.begin(), s.order.begin() + n, [&](int a, int b) { return s.depth[static_cast<size_t>(a)] > s.depth[static_cast<size_t>(b)]; });
  s.solid.clear();
  s.lines.clear();
  if (wire < 1.0f)
    for (int j = 0; j < n; ++j) {
      const size_t i = static_cast<size_t>(s.order[static_cast<size_t>(j)]);
      quadVerts(s.solid, &s.scr[i * 4], &s.col[i * 4], uv);
    }
  if (wire > 0.0f) {  // every edge, brighter nearer, a pale steel blue
    const float dn = (kCamD - 20) * (kCamD - 20), df = (kCamD + 20) * (kCamD + 20);
    for (int j = 0; j < n; ++j) {
      const size_t i = static_cast<size_t>(s.order[static_cast<size_t>(j)]);
      const float near = std::clamp((df - s.depth[i]) / (df - dn), 0.0f, 1.0f);
      const int a = static_cast<int>(wire * (90 + 165 * near));
      const ImU32 lc = IM_COL32(static_cast<int>(lerp(150, 230, near)), static_cast<int>(lerp(175, 240, near)), 255, a);
      const ImU32 lcs[4] = {lc, lc, lc, lc};
      for (int k = 0; k < kMaxEdges; ++k) {
        const ImVec2 p = s.scr[i * 4 + static_cast<size_t>(k)], q = s.scr[i * 4 + static_cast<size_t>((k + 1) % 4)];
        const float dx = q.x - p.x, dy = q.y - p.y, l = std::sqrt(dx * dx + dy * dy);
        if (l < 0.5f) continue;
        const float ox = -dy / l * 0.6f, oy = dx / l * 0.6f;
        const ImVec2 pts[4] = {ImVec2(p.x + ox, p.y + oy), ImVec2(q.x + ox, q.y + oy), ImVec2(q.x - ox, q.y - oy), ImVec2(p.x - ox, p.y - oy)};
        quadVerts(s.lines, pts, lcs, uv);
      }
    }
  }
}

void emit(ImDrawList* dl, const std::vector<ImDrawVert>& v, ImVec2 o) {
  constexpr size_t kChunk = 4 * 4096;  // quads a reserve (16-bit indices)
  for (size_t at = 0; at < v.size(); at += kChunk) {
    const size_t nv = std::min(kChunk, v.size() - at);
    dl->PrimReserve(static_cast<int>(nv / 4 * 6), static_cast<int>(nv));
    const ImDrawIdx base = static_cast<ImDrawIdx>(dl->_VtxCurrentIdx);
    for (size_t q = 0; q < nv; q += 4) {
      const ImDrawIdx b = static_cast<ImDrawIdx>(base + q);
      dl->PrimWriteIdx(b), dl->PrimWriteIdx(static_cast<ImDrawIdx>(b + 1)), dl->PrimWriteIdx(static_cast<ImDrawIdx>(b + 2));
      dl->PrimWriteIdx(b), dl->PrimWriteIdx(static_cast<ImDrawIdx>(b + 2)), dl->PrimWriteIdx(static_cast<ImDrawIdx>(b + 3));
    }
    for (size_t k = 0; k < nv; ++k) {
      const ImDrawVert& d = v[at + k];
      dl->PrimWriteVtx(ImVec2(o.x + d.pos.x, o.y + d.pos.y), d.uv, d.col);
    }
  }
}

// Copper bars in the brand orange, drifting slowly behind the letters (half see-through: the stars show).
void copper(ImDrawList* dl, ImVec2 p0, ImVec2 p1, double t) {
  const float H = p1.y - p0.y, bh = std::max(6.0f, std::round(H * 0.11f));
  for (int b = 0; b < 3; ++b) {
    const float ph = static_cast<float>(b) * 2.1f;
    const float yc = p0.y + H * 0.5f + H * 0.36f * std::sin(static_cast<float>(std::fmod(t * 0.55, 6.283185307179586 * 1000)) + ph);
    const float y0 = yc - bh * 0.5f, y1 = yc + bh * 0.5f;
    const int a = 120 - b * 25;
    const ImU32 edge = IM_COL32(96, 52, 10, a), mid = IM_COL32(255, 181, 71, a);
    dl->AddRectFilledMultiColor(ImVec2(p0.x, y0), ImVec2(p1.x, yc), edge, edge, mid, mid);
    dl->AddRectFilledMultiColor(ImVec2(p0.x, yc), ImVec2(p1.x, y1), mid, mid, edge, edge);
  }
}

void scanlines(ImDrawList* dl, ImVec2 p0, ImVec2 p1) {
  const ImU32 c = IM_COL32(0, 0, 0, 40);
  for (float y = std::floor(p0.y) + 1; y < p1.y; y += 2) dl->AddRectFilled(ImVec2(p0.x, y), ImVec2(p1.x, y + 1), c);
}

// The still frames under reduced motion: the text in three-quarter view, the mark, the wireframe.
double stillTime(int idx) {
  const auto& segs = banner::segments();
  const int pick[3] = {1, 7, 12};
  const float u[3] = {0.25f, 0.06f, 0.2f};
  const int w = ((idx % 3) + 3) % 3;
  double start = 0;
  for (int i = 0; i < pick[w]; ++i) start += segs[static_cast<size_t>(i)].seconds;
  return start + segs[static_cast<size_t>(pick[w])].seconds * u[w];
}

}  // namespace

float heightFor(float bandH, float fontSize) {
  // Under it: a gap and three centred lines of text, the last ending level with the bottom of the
  // Exit button (bottom left, beside the lines, never under them), plus the band's margin.
  const float below = std::round(fontSize * 0.9f) + 3 * (fontSize + 3) + 4;
  return std::round(std::max(fontSize * 2.5f, bandH - 8 - below));
}

void draw(ImDrawList* dl, ImVec2 p0, ImVec2 p1) {
  State& s = st();
  const ImVec2 sz(std::floor(p1.x - p0.x), std::floor(p1.y - p0.y));
  if (sz.x < 40 || sz.y < 16) return;
  if (!s.built) buildOnce(s);
  const Clock::time_point t0 = Clock::now();
  const bool still = !scrollui::animations();
  s.animating = !still && !s.frozen;
  const double L = banner::loopSeconds();
  if (s.animating) {
    const double dt = std::min(0.1, static_cast<double>(ImGui::GetIO().DeltaTime));
    s.t = std::fmod(s.t + dt, L);  // the loop point is seamless, so the clock wraps there
    s.bars += dt;
    if (s.glide >= 0) s.glide += dt;
  }
  if (s.glide >= kSkipSeconds || !s.animating) s.glide = -1;
  const double shown = s.frozen ? s.t : still ? stillTime(s.staticIdx) : s.t;
  // Worked out again whenever the time moved (every frame while it runs: smooth at any display
  // rate, about 0.15 ms); a held or still banner only re-emits its cache.
  if (shown != s.lastShown || s.glide != s.lastGlide || sz.x != s.lastSize.x || sz.y != s.lastSize.y) {
    const Clock::time_point u0 = Clock::now();
    update(s, shown, sz, dl->_Data->TexUvWhitePixel, s.glide < 0 ? -1.0f : static_cast<float>(s.glide / kSkipSeconds));
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - u0).count();
    s.updSum += ms, s.updMax = std::max(s.updMax, ms), ++s.updates;
    s.lastShown = shown, s.lastGlide = s.glide, s.lastSize = sz;
  }
  const ImVec2 o(p0.x, p0.y);
  dl->PushClipRect(p0, p1, true);
  copper(dl, p0, p1, s.frozen || still ? shown : s.bars);
  emit(dl, s.solid, o);
  emit(dl, s.lines, o);
  scanlines(dl, p0, p1);
  dl->PopClipRect();
  // A click glides on to the next state (under reduced motion: the next still, at once).
  const ImVec2 back = ImGui::GetCursorScreenPos();
  ImGui::SetCursorScreenPos(p0);
  if (ImGui::InvisibleButton("##k315banner", sz)) {
    if (s.animating) {
      // Start the glide from what is on screen now (mid-glide too), into the next state's start.
      s.from = s.world;
      s.wireFrom = s.curWire;
      s.t = std::fmod(banner::nextStart(s.t) + 1e-4, L);
      s.glide = 0;
    } else if (s.frozen) {
      s.t = banner::nextStart(s.t) + 1e-4;
    } else {
      ++s.staticIdx;
    }
    s.lastShown = -1;
  }
  ui::Hint("Click for the next shape");
  ImGui::SetCursorScreenPos(back);
  s.lastDraw = Clock::now();
  const double ms = std::chrono::duration<double, std::milli>(s.lastDraw - t0).count();
  s.drawSum += ms, s.drawMax = std::max(s.drawMax, ms), ++s.draws;
}

bool wantsFrames() {
  const State& s = st();
  return s.animating && s.draws > 0 && Clock::now() - s.lastDraw < std::chrono::milliseconds(250);
}

bool devCommand(const std::string& cmd, const std::vector<std::string>& args, int frame) {
  if (cmd != "banner") return false;
  State& s = st();
  const std::string w = args.empty() ? "stats" : args[0];
  if (w == "time" && args.size() > 1) {
    s.t = std::max(0.0, std::atof(args[1].c_str())), s.frozen = true, s.glide = -1, s.lastShown = -1;
  } else if (w == "run") {
    s.frozen = false;
  } else if (w == "next") {
    s.t = banner::nextStart(s.t) + 1e-4, s.lastShown = -1;
  } else if (w == "reset") {  // dev: zero the cost counters
    s.draws = s.updates = s.clamped = 0, s.drawSum = s.drawMax = s.updSum = s.updMax = 0;
  } else {
    const banner::Where at = banner::where(s.t);
    std::printf("banner stats frame %d: facets %d build %.3f ms fit %.3f ms draws %lld mean %.4f ms max %.4f ms updates %lld mean %.4f ms "
                "max %.4f ms clamped %lld solid-verts %zu line-verts %zu segment %s\n",
                frame, banner::model().K, s.buildMs, s.fitMs, s.draws, s.draws ? s.drawSum / static_cast<double>(s.draws) : 0.0, s.drawMax,
                s.updates, s.updates ? s.updSum / static_cast<double>(s.updates) : 0.0, s.updMax, s.clamped, s.solid.size(), s.lines.size(),
                banner::segments()[static_cast<size_t>(at.segment)].name);
    std::fflush(stdout);
  }
  return true;
}

}  // namespace gl::chromebanner
