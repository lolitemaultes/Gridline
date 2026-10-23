// Gridline - J-99: smooth freehand drawing (core/DrawSmooth.h).
#include <cmath>
#include <vector>

#include "Check.h"
#include "../src/core/Document.h"
#include "../src/core/DrawSmooth.h"

using namespace gl;
using namespace gl::drawsmooth;

namespace {

int countTouched(const std::vector<uint8_t>& t) {
  int c = 0;
  for (uint8_t v : t) c += v ? 1 : 0;
  return c;
}

// Drags across with a sample every `dt` seconds through the given points.
void drag(Stroke& s, const std::vector<std::pair<double, double>>& pts, double dt) {
  double t = 0;
  for (const auto& p : pts) {
    s.add(p.first, p.second, t);
    t += dt;
  }
}

}  // namespace

int main() {
  // --- fast strokes with large jumps: no gaps, monotonic fill --------------
  for (bool raw : {false, true}) {
    const int n = 128;
    std::vector<float> zero(n, 0.0f);
    Stroke s;
    Options o;
    o.raw = raw;
    s.begin(zero.data(), n, o);
    // Three frames cross the whole surface: 0.05 -> 0.5 -> 0.95, rising 0.1 -> 0.9.
    drag(s, {{0.05, 0.1}, {0.5, 0.5}, {0.95, 0.9}}, 1.0 / 60);
    const auto& t = s.touched();
    const int first = cellAt({}, n, 0.05), last = cellAt({}, n, 0.95);
    bool gapless = true;
    for (int i = first; i <= last; ++i) gapless = gapless && t[static_cast<size_t>(i)];
    expect(gapless, raw ? "fast stroke (raw): every cell between samples is filled" : "fast stroke: every cell between samples is filled");
    bool mono = true;
    const auto& r = s.result();
    for (int i = first + 1; i <= last; ++i) mono = mono && r[static_cast<size_t>(i)] >= r[static_cast<size_t>(i - 1)] - 1e-6f;
    expect(mono, raw ? "fast stroke (raw): the fill rises monotonically" : "fast stroke: the fill rises monotonically");
    expect(countTouched(t) == last - first + 1, "fast stroke: exactly the crossed cells are touched");
  }
  // Right-to-left strokes fill too.
  {
    const int n = 32;
    std::vector<float> v(n, 0.5f);
    Stroke s;
    s.begin(v.data(), n, Options{});
    drag(s, {{0.9, 1.0}, {0.1, 0.0}}, 1.0 / 60);
    expect(countTouched(s.touched()) == cellAt({}, n, 0.9) - cellAt({}, n, 0.1) + 1, "backward stroke: no gaps");
  }
  // Log-frequency axis: linear bins shown on a log display interpolate along the display.
  {
    const int n = 64;
    Axis ax(n);
    for (int i = 0; i < n; ++i) {
      const double hz = 20.0 + (20000.0 - 20.0) * i / (n - 1);
      ax[static_cast<size_t>(i)] = std::log(hz / 20.0) / std::log(1000.0);
    }
    std::vector<float> v(n, 0.0f);
    Stroke s;
    Options o;
    o.raw = true;
    s.begin(v.data(), n, o, ax);
    drag(s, {{0.0, 0.0}, {1.0, 1.0}}, 1.0 / 60);
    expect(countTouched(s.touched()) == n, "log axis: a full sweep touches every bin");
    const int mid = cellAt(ax, n, 0.5);
    const double expectV = ax[static_cast<size_t>(mid)];
    expect(std::fabs(s.result()[static_cast<size_t>(mid)] - expectV) < 1e-4, "log axis: values follow the display position");
  }

  // --- jitter: smoothed within tolerance -----------------------------------
  {
    const int n = 96;
    std::vector<float> v(n, 0.0f);
    Stroke smooth, raw;
    Options o;
    smooth.begin(v.data(), n, o);
    Options r = o;
    r.raw = true;
    raw.begin(v.data(), n, r);
    std::vector<std::pair<double, double>> pts;
    unsigned seed = 12345;
    for (int k = 0; k <= 120; ++k) {  // a slow 2 s sweep at 60 Hz with +-0.04 hand jitter
      seed = seed * 1103515245u + 12345u;
      const double noise = ((seed >> 16) & 0x7FFF) / 32767.0 * 2 - 1;
      pts.push_back({0.02 + 0.96 * k / 120.0, 0.5 + 0.04 * noise});
    }
    drag(smooth, pts, 1.0 / 60);
    drag(raw, pts, 1.0 / 60);
    double devS = 0, devR = 0, maxS = 0;
    int cnt = 0;
    for (int i = 4; i < n - 4; ++i) {
      if (!smooth.touched()[static_cast<size_t>(i)]) continue;
      const double ds = smooth.result()[static_cast<size_t>(i)] - 0.5, dr = raw.result()[static_cast<size_t>(i)] - 0.5;
      devS += ds * ds;
      devR += dr * dr;
      maxS = std::max(maxS, std::fabs(ds));
      ++cnt;
    }
    devS = std::sqrt(devS / std::max(1, cnt));
    devR = std::sqrt(devR / std::max(1, cnt));
    std::printf("# jitter rms raw %.4f smooth %.4f max %.4f\n", devR, devS, maxS);
    expect(cnt > n / 2, "jitter: the sweep covers the surface");
    expect(devS < devR * 0.5, "jitter: smoothing halves the jitter (rms)");
    expect(maxS < 0.025, "jitter: smoothed output stays within +-0.025 of the line");
  }
  // A deliberate edge survives (edge-preserving): a step from 0.1 to 0.9.
  {
    const int n = 64;
    std::vector<float> v(n, 0.5f);
    Stroke s;
    s.begin(v.data(), n, Options{});
    std::vector<std::pair<double, double>> pts;
    for (int k = 0; k <= 30; ++k) pts.push_back({0.1 + 0.39 * k / 30.0, 0.1});
    for (int k = 0; k <= 30; ++k) pts.push_back({0.51 + 0.39 * k / 30.0, 0.9});
    drag(s, pts, 1.0 / 60);
    expect(s.result()[static_cast<size_t>(cellAt({}, n, 0.3))] < 0.15f && s.result()[static_cast<size_t>(cellAt({}, n, 0.7))] > 0.85f,
           "edge: both plateaus keep their level");
  }

  // --- untouched cells unchanged ---------------------------------------------
  {
    const int n = 200;
    std::vector<float> v(n);
    for (int i = 0; i < n; ++i) v[static_cast<size_t>(i)] = static_cast<float>(0.5 + 0.4 * std::sin(i * 0.37));
    Options o;
    o.amount = 1.0;
    Stroke s;
    s.begin(v.data(), n, o);
    drag(s, {{0.40, 0.2}, {0.45, 0.8}, {0.5, 0.3}}, 1.0 / 60);
    bool same = true;
    int changed = 0;
    for (int i = 0; i < n; ++i) {
      if (s.touched()[static_cast<size_t>(i)]) {
        changed += s.result()[static_cast<size_t>(i)] != v[static_cast<size_t>(i)];
        continue;
      }
      same = same && s.result()[static_cast<size_t>(i)] == v[static_cast<size_t>(i)];  // bit-identical
    }
    expect(same, "untouched cells are bit-identical");
    expect(changed > 0, "touched cells changed");
    // The pass on its own never writes outside the touched set either.
    std::vector<uint8_t> none(n, 0);
    std::vector<float> out;
    smoothTouched(v, none, out, {}, 1.0, false, 1.0f);
    expect(out == v, "pass with nothing touched is the identity");
  }

  // --- cyclic waves wrap continuously ------------------------------------------
  {
    const int n = 256;
    std::vector<float> v(n, 0.0f);
    std::vector<uint8_t> all(n, 1);
    // A jagged cycle with its loop seam at index 0 / n-1.
    for (int i = 0; i < n; ++i) v[static_cast<size_t>(i)] = static_cast<float>(std::sin(2 * 3.14159265358979 * i / n) + ((i & 1) ? 0.08 : -0.08));
    std::vector<float> cyc, lin;
    smoothTouched(v, all, cyc, {}, 1.0, true, 2.0f);
    smoothTouched(v, all, lin, {}, 1.0, false, 2.0f);
    // Across the seam the cyclic result steps like anywhere else in the cycle.
    double maxStep = 0;
    for (int i = 1; i < n; ++i) maxStep = std::max(maxStep, static_cast<double>(std::fabs(cyc[static_cast<size_t>(i)] - cyc[static_cast<size_t>(i - 1)])));
    const double seam = std::fabs(cyc[0] - cyc[static_cast<size_t>(n - 1)]);
    std::printf("# cyclic seam %.4f max step %.4f\n", seam, maxStep);
    expect(seam <= maxStep * 1.05 + 1e-6, "cyclic: the loop seam is as smooth as the rest of the cycle");
    double mean = 0, meanIn = 0;
    for (int i = 0; i < n; ++i) mean += cyc[static_cast<size_t>(i)], meanIn += v[static_cast<size_t>(i)];
    expect(std::fabs(mean - meanIn) / n < 1e-3, "cyclic: no DC shift");
    expect(lin[0] == v[0] && lin[static_cast<size_t>(n - 1)] == v[static_cast<size_t>(n - 1)], "table: the end points are kept");
    // A stroke near the end of a cycle smooths with the start of the cycle.
    Options o;
    o.cyclic = true;
    o.amount = 1.0;
    std::vector<float> flat(n, 0.0f);
    Stroke s;
    s.begin(flat.data(), n, o);
    drag(s, {{0.97, 0.6}, {0.99, 0.6}, {1.0, 0.6}}, 1.0 / 60);
    expect(s.result()[static_cast<size_t>(n - 1)] < 0.6f && s.result()[0] == 0.0f, "cyclic stroke: wraps for its neighbours, cell 0 untouched");
  }

  // --- Alt draws raw ------------------------------------------------------------
  {
    const int n = 16;
    std::vector<float> v(n, 0.0f);
    Options o;
    o.raw = true;
    o.amount = 1.0;
    Stroke s;
    s.begin(v.data(), n, o);
    drag(s, {{0.0, 1.0}, {0.49, 1.0}, {0.51, 0.0}, {1.0, 0.0}}, 1.0 / 60);
    bool stepped = true;
    for (int i = 0; i < n; ++i) stepped = stepped && (s.result()[static_cast<size_t>(i)] == (i < n / 2 ? 1.0f : 0.0f));
    expect(stepped, "raw: a hard step stays exactly as drawn");
    Stroke m;
    o.raw = false;
    m.begin(v.data(), n, o);
    drag(m, {{0.0, 1.0}, {0.49, 1.0}, {0.51, 0.0}, {1.0, 0.0}}, 1.0 / 60);
    bool differs = false;
    for (int i = 0; i < n; ++i) differs = differs || m.result()[static_cast<size_t>(i)] != s.result()[static_cast<size_t>(i)];
    expect(differs, "smooth: the same stroke is filtered");
    o.amount = 0;
    Stroke z;
    z.begin(v.data(), n, o);
    drag(z, {{0.0, 1.0}, {0.49, 1.0}, {0.51, 0.0}, {1.0, 0.0}}, 1.0 / 60);
    expect(z.result() == s.result(), "amount 0 equals raw");
  }

  // --- breakpoint curves: the curve outside the stroke is kept -------------------
  {
    const std::vector<CurvePoint> pts = {{0.0, 0.5}, {0.2, 0.9}, {0.5, 0.1}, {0.8, 0.7}, {1.0, 0.5}};
    auto evalAt = [](const std::vector<CurvePoint>& p, double x) {
      if (x <= p.front().x) return p.front().v;
      for (size_t i = 1; i < p.size(); ++i)
        if (x <= p[i].x) return p[i - 1].v + (p[i].v - p[i - 1].v) * (x - p[i - 1].x) / std::max(1e-12, p[i].x - p[i - 1].x);
      return p.back().v;
    };
    const int n = 96;
    std::vector<float> cur(n);
    for (int i = 0; i < n; ++i) cur[static_cast<size_t>(i)] = static_cast<float>(evalAt(pts, (i + 0.5) / n));
    Stroke s;
    s.begin(cur.data(), n, Options{});
    drag(s, {{0.40, 0.3}, {0.55, 0.6}, {0.62, 0.4}}, 1.0 / 60);
    double xa = 0, xb = 0;
    expect(touchedSpan(s.touched(), 1.0, xa, xb) && xa < 0.41 && xb > 0.61, "curve: the touched span covers the stroke");
    const auto out = spliceIntoCurve(pts, 1.0, s.result(), s.touched(), evalAt(pts, xa), evalAt(pts, xb));
    bool same = true;
    for (int k = 0; k <= 1000; ++k) {
      const double x = k / 1000.0;
      if (x >= xa && x <= xb) continue;
      same = same && std::fabs(evalAt(out, x) - evalAt(pts, x)) < 1e-9;
    }
    expect(same, "curve: outside the stroke the curve is unchanged");
    const int c = cellAt({}, n, 0.55);
    expect(std::fabs(evalAt(out, (c + 0.5) / n) - s.result()[static_cast<size_t>(c)]) < 1e-6 && s.result()[static_cast<size_t>(c)] > 0.4f,
           "curve: inside, the drawn values replace the old points");
  }

  // --- one stroke is one undo step (the editors' protocol) ---------------------
  {
    Document doc;
    const ObjectId ch = doc.addChannel("Synth", "gl.synth");
    const std::vector<float> before = doc.song().channel(ch)->plugin.params;
    const size_t pos0 = doc.history().position();
    auto stroke = [&](uint64_t key, float val) {
      doc.endGesture();  // stroke start: never merge into the previous stroke
      for (int f = 0; f < 10; ++f)
        doc.editChannel(ch, "draw", [&](Channel& c) { c.plugin.params[static_cast<size_t>(f % 3)] = val + 0.01f * f; }, key);
      doc.endGesture();  // stroke end
    };
    stroke(0x44525731, 0.3f);
    expect(doc.history().position() == pos0 + 1, "undo: ten frames of one stroke are one step");
    stroke(0x44525731, 0.6f);
    expect(doc.history().position() == pos0 + 2, "undo: the next stroke with the same surface key is its own step");
    doc.undo();
    doc.undo();
    expect(doc.song().channel(ch)->plugin.params == before, "undo: two undos restore the state before both strokes");
  }
  return finish();
}
