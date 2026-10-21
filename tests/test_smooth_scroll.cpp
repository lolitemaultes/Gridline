// Gridline — J-67: smooth scrolling and elastic edges (core/SmoothScroll.h).
#include <cmath>
#include <vector>

#include "Check.h"
#include "../src/core/SmoothScroll.h"
#include "../src/core/SmoothSnap.h"

using namespace gl::smoothscroll;

namespace {

// Runs `seconds` at `hz`, returning the outputs sampled every `sampleEvery` seconds.
std::vector<double> run(Axis& a, double seconds, double hz, double lo, double hi, const Params& p, double sampleEvery) {
  std::vector<double> out;
  const double dt = 1.0 / hz;
  double t = 0, nextSample = sampleEvery;
  const int n = static_cast<int>(std::lround(seconds * hz));
  for (int i = 0; i < n; ++i) {
    a.update(dt, lo, hi, p);
    t += dt;
    if (t + 1e-9 >= nextSample) {
      out.push_back(a.output());
      nextSample += sampleEvery;
    }
  }
  return out;
}

double maxAbsDiff(const std::vector<double>& a, const std::vector<double>& b) {
  double m = a.size() == b.size() ? 0 : 1e9;
  for (size_t i = 0; i < a.size() && i < b.size(); ++i) m = std::max(m, std::fabs(a[i] - b[i]));
  return m;
}

}  // namespace

int main() {
  const Params p;

  {  // rubber band: 1:1-ish at first, bounded, monotonic, invertible
    bool bounded = true, mono = true, inv = true;
    double prev = 0;
    for (double s = 1; s < 100000; s *= 1.3) {
      const double o = rubber(s, p);
      bounded = bounded && o > 0 && o < p.maxOver;
      mono = mono && o > prev;
      prev = o;
      inv = inv && (o > p.maxOver * 0.99 || std::fabs(unrubber(o, p) - s) < 1e-6 * s + 1e-9);
      if (rubber(-s, p) != -o) inv = false;
    }
    expect(bounded, "rubber: overscroll stays below the bound");
    expect(mono, "rubber: resistance grows but overscroll keeps growing with input");
    expect(inv, "rubber: unrubber inverts it, symmetric in sign");
    expect(rubber(1, p) > 0.5 && rubber(1, p) <= 1, "rubber: small stretches follow nearly 1:1");
  }
  {  // wheel notches classified
    expect(isNotch(1.0f) && isNotch(-1.0f) && isNotch(3.0f), "isNotch: integral wheel steps are notches");
    expect(!isNotch(0.25f) && !isNotch(-1.4f) && !isNotch(0.0f), "isNotch: fractional trackpad deltas are precise");
  }
  {  // a notch glides into place in ~80 ms and lands exactly on the grid step
    Axis a;
    a.reset(0);
    a.scroll(24, true, 0, 1000, p);
    a.update(1.0 / 60, 0, 1000, p);
    const double f1 = a.output();
    expect(f1 > 12 && f1 < 24, "notch: moves at once (more than half in the first 60 Hz frame) without jumping all the way");
    run(a, 0.1, 60, 0, 1000, p, 1);
    expect(a.output() == 24, "notch: arrives exactly on the step within ~100 ms");
    for (int i = 0; i < 3; ++i) a.scroll(24, true, 0, 1000, p);
    double prev = a.output();
    bool mono = true;
    for (int i = 0; i < 30; ++i) {
      a.update(1.0 / 60, 0, 1000, p);
      mono = mono && a.output() >= prev && a.output() <= 96;
      prev = a.output();
    }
    expect(mono, "notch: eased motion never overshoots the target");
    expect(a.output() == 96, "grid landing: four notches of one row land exactly on row 4");
  }
  {  // frame-rate independence (notches, overscroll and spring back)
    Params q = p;
    std::vector<double> r[3];
    const double hz[3] = {30, 60, 240};
    for (int k = 0; k < 3; ++k) {
      Axis a;
      a.reset(950);
      a.scroll(40, true, 0, 1000, q);   // 10 px into the edge
      a.scroll(40, true, 0, 1000, q);   // then 40 more against it
      r[k] = run(a, 0.6, hz[k], 0, 1000, q, 1.0 / 30);
    }
    expect(maxAbsDiff(r[0], r[1]) < 0.75 && maxAbsDiff(r[1], r[2]) < 0.75, "frame-rate independence: 30/60/240 Hz trace the same path (< 0.75 px)");
    expect(r[1].back() == 1000 && r[2].back() == 1000, "frame-rate independence: all settle exactly on the edge");
    Axis b, c;
    b.reset(0);
    c.reset(0);
    b.scroll(60, true, 0, 1000, q);
    c.scroll(60, true, 0, 1000, q);
    const auto rb = run(b, 0.2, 60, 0, 1000, q, 1.0 / 60), rc = run(c, 0.2, 240, 0, 1000, q, 1.0 / 60);
    expect(maxAbsDiff(rb, rc) < 0.05, "frame-rate independence: a notch glide matches at 60 and 240 Hz");
  }
  {  // trackpad (precise) deltas are 1:1: no lag under the finger
    Axis a;
    a.reset(100);
    a.scroll(7.5, false, 0, 1000, p);
    a.update(1.0 / 120, 0, 1000, p);
    expect(a.output() == 107.5, "precise: a trackpad delta moves the view 1:1 in the same frame");
    a.scroll(-20, false, 0, 1000, p);
    a.update(1.0 / 120, 0, 1000, p);
    expect(a.output() == 87.5, "precise: and back, 1:1");
  }
  {  // no jump at the edge: a momentum stream continuing into the bottom edge
    for (int mode = 0; mode < 2; ++mode) {
      const bool notch = mode == 1;
      Axis a;
      a.reset(980);
      double prev = a.output(), maxStep = 0, peak = 0, d = notch ? 30 : 9;
      bool mono = true;
      for (int i = 0; i < 40; ++i) {  // momentum: decaying deltas every frame
        a.scroll(notch ? 30 : d, notch, 0, 1000, p);
        d *= 0.93;
        a.update(1.0 / 60, 0, 1000, p);
        mono = mono && a.output() >= prev - 1e-9;
        maxStep = std::max(maxStep, std::fabs(a.output() - prev));
        peak = std::max(peak, a.output());
        prev = a.output();
      }
      expect(mono, notch ? "edge (wheel): held against the edge the view never moves backwards (no jumping)"
                         : "edge (momentum): held against the edge the view never moves backwards (no jumping)");
      expect(peak > 1000 && peak < 1000 + p.maxOver, notch ? "edge (wheel): elastic overscroll is bounded" : "edge (momentum): elastic overscroll is bounded");
      expect(maxStep <= (notch ? 30 : 9) + 1e-9, "edge: no single frame moves more than the input");
      // input stops: springs back without undershoot or oscillation
      // (a wheel stretch may finish easing out before it turns: one peak, then down)
      bool back = true, falling = false;
      double last = a.output();
      for (int i = 0; i < 60; ++i) {
        a.update(1.0 / 60, 0, 1000, p);
        if (a.output() < last - 1e-9) falling = true;
        back = back && !(falling && a.output() > last + 1e-9) && a.output() >= 1000;
        last = a.output();
      }
      expect(back, "spring back: no oscillation (never rises again once falling), never crosses the edge");
      expect(a.output() == 1000 && !a.animating(), "spring back: rests exactly on the edge within a second");
    }
  }
  {  // the top edge too, and a wheel notch into the edge grows the overscroll smoothly
    Axis a;
    a.reset(0);
    a.scroll(-60, true, 0, 1000, p);
    double prev = 0, maxStep = 0, low = 0;
    for (int i = 0; i < 60; ++i) {
      a.update(1.0 / 60, 0, 1000, p);
      maxStep = std::max(maxStep, std::fabs(a.output() - prev));
      low = std::min(low, a.output());
      prev = a.output();
    }
    expect(low < -5 && low > -p.maxOver, "top edge: a notch past the top stretches a little, bounded");
    expect(maxStep < 12, "top edge: the stretch eases in and out (no frame jumps more than 12 px)");
    expect(a.output() == 0, "top edge: springs back to 0");
  }
  {  // unwinding: scrolling back from an overscroll moves 1:1 and continues into the range
    Axis a;
    a.reset(1000);
    a.scroll(100, false, 0, 1000, p);
    a.update(1.0 / 60, 0, 1000, p);
    const double o = a.output() - 1000;
    expect(o > 0 && o < p.maxOver, "unwind: overscrolled");
    a.scroll(-o - 5, false, 0, 1000, p);
    a.update(1.0 / 60, 0, 1000, p);
    expect(std::fabs(a.output() - 995) < 1e-6, "unwind: back through the edge 1:1 into the range");
  }
  {  // a notch away from a stretched edge still lands on its grid row
    Axis a;
    a.reset(0);
    a.scroll(-30, false, 0, 1000, p);  // stretched past the top
    a.update(1.0 / 60, 0, 1000, p);
    a.scroll(46 * 3, true, 0, 1000, p);
    double prev = a.output();
    bool mono = true;
    for (int i = 0; i < 30; ++i) {
      a.update(1.0 / 60, 0, 1000, p);
      mono = mono && a.output() >= prev - 1e-9;
      prev = a.output();
    }
    expect(a.output() == 138, "grid landing: three track notches from an overscrolled top land exactly on track 3");
    expect(mono, "grid landing: and glide there without turning back");
  }
  {  // axis independence: an axis held at its edge never disturbs the other
    Axis x, y, yAlone;
    x.reset(500);
    y.reset(100);
    yAlone.reset(100);
    double maxDiff = 0;
    for (int i = 0; i < 90; ++i) {
      const double dy = i < 40 ? 6.0 * std::pow(0.95, i) : 0.0;
      x.scroll(i < 40 ? 8.0 : 0.0, false, 0, 500, p);  // x is pinned at its right edge
      y.scroll(dy, false, 0, 1000, p);
      yAlone.scroll(dy, false, 0, 1000, p);
      x.update(1.0 / 60, 0, 500, p);
      y.update(1.0 / 60, 0, 1000, p);
      yAlone.update(1.0 / 60, 0, 1000, p);
      maxDiff = std::max(maxDiff, std::fabs(y.output() - yAlone.output()));
    }
    expect(maxDiff == 0, "axes: the vertical axis is identical whether or not the horizontal one is clamped");
    expect(x.output() == 500, "axes: the clamped axis springs back to its edge");
  }
  {  // settings: elastic off = hard stop, smooth off = notches jump
    Params hard = p;
    hard.elastic = false;
    Axis a;
    a.reset(990);
    bool inRange = true;
    for (int i = 0; i < 20; ++i) {
      a.scroll(10, i % 2 == 0, 0, 1000, hard);
      a.update(1.0 / 60, 0, 1000, hard);
      inRange = inRange && a.output() <= 1000;
    }
    expect(inRange, "elastic off: never past the edge");
    Params jump = p;
    jump.smooth = false;
    Axis b;
    b.reset(0);
    b.scroll(24, true, 0, 1000, jump);
    b.update(1.0 / 60, 0, 1000, jump);
    expect(b.output() == 24, "smooth off: a notch jumps straight to the step");
  }
  {  // outside writes (zoom, fit, follow playback, scrollbar drag) are adopted at once
    Axis a;
    a.reset(0);
    a.scroll(48, true, 0, 1000, p);
    a.update(1.0 / 60, 0, 1000, p);
    expect(!a.sync(a.output()), "sync: the axis' own output is not an outside write");
    expect(!a.sync(static_cast<float>(a.output())), "sync: a float round trip of the output is still its own");
    expect(a.sync(300), "sync: a different value is adopted");
    a.update(1.0 / 60, 0, 1000, p);
    expect(a.output() == 300 && !a.animating(), "sync: adopted without animation (zoom stays anchored)");
  }
  {  // content shrinks under the view: the position follows the new end at once
    Axis a;
    a.reset(800);
    a.update(1.0 / 60, 0, 500, p);
    expect(a.output() == 500, "range: a shrinking range clamps the view");
  }
  // ---- smooth motion of snapped drags (gl::smooth::Glides, shared with J-37) ----
  {  // a snapped step glides quickly and lands exactly on the snapped value
    gl::smooth::Glides g;
    double t = 0;
    g.follow(1, 0, t, true, 0.5);
    t += 1.0 / 60;
    const double f1 = g.follow(1, 96, t, true, 0.5);
    expect(f1 > 48 && f1 < 96, "glide: a grid step moves most of the way in the first frame, not all of it");
    double v = f1;
    for (int i = 0; i < 6; ++i) t += 1.0 / 60, v = g.follow(1, 96, t, true, 0.5);
    expect(v == 96, "glide: lands exactly on the snapped value within ~100 ms");
  }
  {  // frame-rate independence
    gl::smooth::Glides a, b;
    a.follow(1, 0, 0, true, 1e-9);
    b.follow(1, 0, 0, true, 1e-9);
    double va = 0, vb = 0;
    for (int i = 1; i <= 2; ++i) va = a.follow(1, 120, i / 120.0, true, 1e-9);
    for (int i = 1; i <= 8; ++i) vb = b.follow(1, 120, i / 480.0, true, 1e-9);
    expect(std::fabs(va - vb) < 1e-9, "glide: 2 frames at 120 Hz land where 8 frames at 480 Hz do");
  }
  {  // no lag when snapping (or smooth snapping) is off, and continuing from where it is drawn
    gl::smooth::Glides g;
    expect(g.follow(1, 37.5, 0, false, 0.5) == 37.5 && g.size() == 0, "glide: off follows the pointer value exactly (no lag)");
    g.follow(2, 0, 0, true, 0.5);
    g.follow(2, 100, 0.004, true, 0.5);
    const double mid = g.follow(2, 100, 0.008, true, 0.5);
    const double next = g.follow(2, 0, 0.012, true, 0.5);
    expect(next < mid && next > 0, "glide: a target change mid-flight continues from the drawn position (no jump)");
    expect(g.follow(2, 50, 0.02, false, 0.5) == 50 && g.size() == 0, "glide: releasing the drag shows the committed value at once");
    g.follow(3, 0, 0, true, 0.5);
    g.prune(1.0);
    expect(g.size() == 0, "glide: values not followed for a while are forgotten");
  }
  return finish();
}
