// K-315: the Tracker About banner's geometry and timing (src/app/ChromeBannerGeom.h).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "Check.h"
#include "app/ChromeBannerGeom.h"

using namespace gl::banner;

namespace {
bool finite(const std::vector<Facet>& v) {
  for (const Facet& f : v)
    for (const V3& p : f.v)
      if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return false;
  return true;
}
float maxMove(const std::vector<Facet>& a, const std::vector<Facet>& b) {
  float m = 0;
  for (size_t i = 0; i < a.size(); ++i)
    for (int k = 0; k < 4; ++k)
      m = std::max(m, std::fabs(a[i].v[k].x - b[i].v[k].x) + std::fabs(a[i].v[k].y - b[i].v[k].y) + std::fabs(a[i].v[k].z - b[i].v[k].z));
  return m;
}
// A frame placed in a w x h banner: its screen bounding box.
struct Px {
  float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
};
Px place(const std::vector<Facet>& f, float w, float h, bool* clamped) {
  const ScreenMap sm = screenMap(f.data(), static_cast<int>(f.size()), w, h);
  if (clamped) *clamped = sm.clamped;
  Px p;
  for (const Facet& fa : f)
    for (const V3& v : fa.v) {
      const float id = 1.0f / std::max(kCamD - v.z, 1.0f), x = sm.cx + sm.f * v.x * id, y = sm.cy - sm.f * v.y * id;
      p.x0 = std::min(p.x0, x), p.x1 = std::max(p.x1, x), p.y0 = std::min(p.y0, y), p.y1 = std::max(p.y1, y);
    }
  return p;
}
}  // namespace

int main() {
  const Model& m = model();
  std::printf("# facets: text %d logo %d K %d\n", m.textFacets, m.logoFacets, m.K);
  expect(m.textFacets > 200, "the letters make a mesh");
  expect(m.logoFacets > 50, "the mark makes a mesh");
  expect(m.K >= m.textFacets && m.K >= m.logoFacets, "K holds both");
  const size_t K = static_cast<size_t>(m.K);
  expect(m.text.size() == K && m.logo.size() == K && m.delay.size() == K && m.dir.size() == K && m.rnd.size() == K,
         "every correspondence list has K entries");
  expect(&model() == &m, "the model is built once and cached");

  // The letters span the banner's text box, centred on the origin.
  float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
  for (const Facet& f : m.text)
    for (const V3& p : f.v) x0 = std::min(x0, p.x), x1 = std::max(x1, p.x), y0 = std::min(y0, p.y), y1 = std::max(y1, p.y);
  expect(std::fabs(x1 - x0 - kTextW) < 0.01f && std::fabs(y1 - y0 - kTextH) < 0.01f, "the text fills its box");
  expect(std::fabs(x0 + x1) < 0.01f && std::fabs(y0 + y1) < 0.01f, "the text is centred on the origin");
  // Every text and logo facet has area (padding copies included).
  bool area = true;
  for (const auto* set : {&m.text, &m.logo})
    for (const Facet& f : *set) {
      const V3 a{f.v[2].x - f.v[0].x, f.v[2].y - f.v[0].y, f.v[2].z - f.v[0].z}, b{f.v[3].x - f.v[1].x, f.v[3].y - f.v[1].y, f.v[3].z - f.v[1].z};
      const V3 c{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
      if (c.x * c.x + c.y * c.y + c.z * c.z < 1e-8f) area = false;
    }
  expect(area, "no flat facets in the solids");

  // Every shape gives K finite facets.
  std::vector<Facet> a(K), b(K), c(K), sc(K);
  for (Shape s : {Shape::Text, Shape::Logo, Shape::Worms, Shape::Particles}) {
    shape(m, s, 3.7, 12.0f, a.data());
    expect(finite(a), "a shape is finite");
  }

  // The ease: 0 to 1, at rest at both ends.
  expect(ease(0) == 0 && ease(1) == 1 && std::fabs(ease(0.5f) - 0.5f) < 1e-6f, "the ease runs 0 to 1");
  expect(ease(0.01f) < 1e-4f && 1 - ease(0.99f) < 1e-4f, "the ease starts and ends at rest");

  // Timing: the loop is the segments' sum (round 2: slow, about 72 s); where() finds each segment;
  // next skips one; every state holds or moves for at least 3 s.
  const auto& segs = segments();
  double L = 0;
  float shortest = 1e9f;
  for (const Segment& s : segs) L += s.seconds, shortest = std::min(shortest, s.seconds);
  std::printf("# loop %.1f s, %zu segments, the shortest %.1f s\n", L, segs.size(), static_cast<double>(shortest));
  expect(std::fabs(L - loopSeconds()) < 1e-9 && L >= 60 && L <= 75, "the loop is 60 to 75 s");
  expect(shortest >= 3.0f, "no state or change shorter than 3 s");
  expect(segs.size() >= 12, "the sequence has its steps");
  double start = 0;
  bool found = true, skips = true;
  for (size_t i = 0; i < segs.size(); ++i) {
    const Where w = where(start + segs[i].seconds * 0.5);
    if (w.segment != static_cast<int>(i) || std::fabs(w.u - 0.5f) > 1e-4f) found = false;
    if (std::fabs(nextStart(start + 0.01) - (start + segs[i].seconds)) > 1e-6) skips = false;
    start += segs[i].seconds;
  }
  expect(found, "where() finds every segment and its progress");
  expect(skips, "nextStart() skips to the next segment");
  expect(where(L + 0.1).segment == 0 && where(-0.1).segment == static_cast<int>(segs.size()) - 1, "the loop wraps both ways");
  expect(segs[0].from == segs.back().to && segs[0].spreadFrom == segs.back().spreadTo, "the loop ends in the pose it starts from");

  // Continuity: every boundary (and the loop point) joins without a jump, and at rest: the speed
  // just before a boundary matches the speed just after (no snap, no jerk).
  bool joined = true, still = true;
  start = 0;
  const double h = 1.0 / 60;
  for (size_t i = 0; i <= segs.size(); ++i) {
    const double t = start;
    evaluate(m, t - 1e-4, a.data(), sc.data());
    evaluate(m, t + 1e-4, b.data(), sc.data());
    const float mv = maxMove(a, b);
    if (mv > 0.05f) {
      joined = false;
      std::printf("# jump of %.3f at %.3f s (segment %zu)\n", static_cast<double>(mv), t, i);
    }
    evaluate(m, t - h, a.data(), sc.data());
    evaluate(m, t - 1e-4, b.data(), sc.data());
    const float before = maxMove(a, b);
    evaluate(m, t + 1e-4, a.data(), sc.data());
    evaluate(m, t + h, b.data(), sc.data());
    const float after = maxMove(a, b);
    if (std::fabs(before - after) > 0.02f) {
      still = false;
      std::printf("# speed changes %.3f -> %.3f a frame at %.3f s (segment %zu)\n", static_cast<double>(before), static_cast<double>(after), t, i);
    }
    if (i < segs.size()) start += segs[i].seconds;
  }
  expect(joined, "no jumps between segments or at the loop point");
  expect(still, "no change of speed at a segment boundary or the loop point");

  // Slow: at 60 frames a second no corner moves more than a world unit a frame (the text is 34 wide).
  float fastest = 0;
  double fastAt = 0;
  for (double t = 0; t < L; t += h) {
    evaluate(m, t, a.data(), sc.data());
    evaluate(m, t + h, b.data(), sc.data());
    const float mv = maxMove(a, b);
    if (mv > fastest) fastest = mv, fastAt = t;
  }
  std::printf("# the fastest corner: %.3f units a frame at %.2f s (%s)\n", static_cast<double>(fastest), fastAt, segs[static_cast<size_t>(where(fastAt).segment)].name);
  expect(fastest < 1.0f, "slow: no corner moves more than one unit a 60 Hz frame");

  expect(wireAt(0) == 0 && wireAt(L - 1e-3) == 0, "the wireframe is off at the loop ends");
  float wmax = 0;
  for (double t = 0; t < L; t += 0.05) wmax = std::max(wmax, wireAt(t));
  expect(wmax > 0.9f, "the wireframe phase shows");
  expect(settledAt(segs[0].seconds + 0.5) && !settledAt(0.5), "solids settle after the spin in");
  for (double t = 0; t < L; t += 0.37) {
    evaluate(m, t, a.data(), sc.data());
    if (!finite(a)) {
      expect(false, "evaluate is finite");
      return finish();
    }
  }
  expect(true, "evaluate is finite across the loop");

  // Framing: at sampled times across the whole loop (every 1/30 s), in banner rects like those of
  // the About screen at 1024x640, 1440x900, 1920x1080 and a 2x UI scale (plus a narrow and a very
  // low one), every corner lands inside the rect, clear of its margin, the frame's bounding box is
  // centred to within 1 px, and the loop's one scale fits every frame (no per-frame shrink).
  const Extent& ext = loopExtent();
  std::printf("# loop extent: half %.4f x %.4f (view units), worked out in %.2f ms\n", static_cast<double>(ext.halfW), static_cast<double>(ext.halfH), ext.ms);
  struct R {
    float w, h;
  };
  const R rects[] = {{1018, 96}, {1434, 150}, {1914, 196}, {1434, 104}, {700, 180}, {2400, 64}, {320, 40}};
  bool inside = true, centred = true, oneScale = true;
  float worstOff = 0, worstFill = 1;
  for (const R& r : rects) {
    float fillH = 0;
    for (double t = 0; t < L; t += 1.0 / 30) {
      evaluate(m, t, a.data(), sc.data());
      bool cl = false;
      const Px p = place(a, r.w, r.h, &cl);
      if (cl) oneScale = false;
      const float mx = marginX(r.w), my = marginY(r.h);
      if (p.x0 < mx - 0.01f || p.y0 < my - 0.01f || p.x1 > r.w - mx + 0.01f || p.y1 > r.h - my + 0.01f) {
        if (inside) std::printf("# outside at %.2f s in %.0fx%.0f: %.1f %.1f %.1f %.1f\n", t, static_cast<double>(r.w), static_cast<double>(r.h),
                                static_cast<double>(p.x0), static_cast<double>(p.y0), static_cast<double>(p.x1), static_cast<double>(p.y1));
        inside = false;
      }
      const float off = std::max(std::fabs((p.x0 + p.x1) * 0.5f - r.w * 0.5f), std::fabs((p.y0 + p.y1) * 0.5f - r.h * 0.5f));
      worstOff = std::max(worstOff, off);
      if (off > 1.0f) centred = false;
      fillH = std::max(fillH, (p.y1 - p.y0) / (r.h - 2 * my));
    }
    // The text at rest (the hold) fills most of the usable height or width.
    evaluate(m, segs[0].seconds, a.data(), sc.data());
    const Px p = place(a, r.w, r.h, nullptr);
    const float fill = std::max((p.y1 - p.y0) / (r.h - 2 * marginY(r.h)), (p.x1 - p.x0) / (r.w - 2 * marginX(r.w)));
    worstFill = std::min(worstFill, fill);
    std::printf("# %4.0fx%-3.0f: the text at rest %.0fx%.0f px (fills %.0f%%), tallest frame %.0f%% of the usable height\n", static_cast<double>(r.w),
                static_cast<double>(r.h), static_cast<double>(p.x1 - p.x0), static_cast<double>(p.y1 - p.y0), static_cast<double>(fill * 100),
                static_cast<double>(fillH * 100));
  }
  std::printf("# framing: the worst centre offset %.3f px\n", static_cast<double>(worstOff));
  expect(inside, "every frame of the loop stays inside the banner, clear of the margin");
  expect(centred, "every frame is centred to within 1 px");
  expect(oneScale, "one scale fits the whole loop (no frame shrinks)");
  expect(worstFill > 0.7f, "the text at rest stays large (70% of the usable height or width)");

  // A click's glide: a blend from any frame into the next state's start stays inside too.
  bool glideIn = true;
  for (double t = 0.3; t < L; t += 2.9) {
    evaluate(m, t, a.data(), sc.data());
    evaluate(m, nextStart(t) + 1e-4, b.data(), sc.data());
    for (float g = 0; g <= 1.0f; g += 0.125f) {
      const float e = ease(g);
      for (size_t i = 0; i < K; ++i)
        for (int k = 0; k < 4; ++k) {
          const V3 p = a[i].v[k], q = b[i].v[k];
          c[i].v[k] = {p.x + (q.x - p.x) * e, p.y + (q.y - p.y) * e, p.z + (q.z - p.z) * e};
        }
      const Px p = place(c, 1434, 150, nullptr);
      if (p.x0 < 0 || p.y0 < 0 || p.x1 > 1434 || p.y1 > 150) glideIn = false;
    }
  }
  expect(glideIn, "a click's glide stays inside the banner");
  return finish();
}
