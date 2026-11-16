// K-315: the Tracker About banner's geometry and timing (src/app/ChromeBannerGeom.h).
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

  // The letters span the banner's text box; the mark is centred.
  float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
  for (const Facet& f : m.text)
    for (const V3& p : f.v) x0 = std::min(x0, p.x), x1 = std::max(x1, p.x), y0 = std::min(y0, p.y), y1 = std::max(y1, p.y);
  expect(std::fabs(x1 - x0 - kTextW) < 0.01f && std::fabs(y1 - y0 - kTextH) < 0.01f, "the text fills its box");
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
  std::vector<Facet> a(K), b(K), sc(K);
  for (Shape s : {Shape::Text, Shape::Logo, Shape::Worms, Shape::Particles}) {
    shape(m, s, 3.7, 12.0f, a.data());
    expect(finite(a), "a shape is finite");
  }

  // Timing: the loop is the segments' sum; where() finds each segment; next skips one.
  const auto& segs = segments();
  double L = 0;
  for (const Segment& s : segs) L += s.seconds;
  expect(std::fabs(L - loopSeconds()) < 1e-9 && L > 15 && L < 40, "the loop length");
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
  expect(segs[0].from == segs.back().to, "the loop ends in the shape it starts from");

  // Continuity: every boundary (and the loop point) joins without a jump.
  bool smooth = true;
  start = 0;
  for (size_t i = 0; i <= segs.size(); ++i) {
    const double t = start;
    evaluate(m, t - 1e-4, a.data(), sc.data());
    evaluate(m, t + 1e-4, b.data(), sc.data());
    const float mv = maxMove(a, b);
    if (mv > 0.25f) {
      smooth = false;
      std::printf("# jump of %.3f at %.3f s (segment %zu)\n", static_cast<double>(mv), t, i);
    }
    if (i < segs.size()) start += segs[i].seconds;
  }
  expect(smooth, "no jumps between segments or at the loop point");
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
  return finish();
}
