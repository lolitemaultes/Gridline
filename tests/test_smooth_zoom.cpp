// Gridline - J-188: smooth zooming (core/SmoothZoom.h) and the motion switch for it.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "Check.h"
#include "../src/core/Motion.h"
#include "../src/core/SmoothSnap.h"
#include "../src/core/SmoothZoom.h"

using namespace gl;
using namespace gl::smoothzoom;

namespace {

constexpr double kDt = 0.016;
constexpr double kMinZ = 0.01, kMaxZ = 8.0;

// The instant zoom the views always did (timeline::zoomAt, float maths).
void zoomAt(float& px, float& scroll, double factor, float anchor) {
  const double tick = (static_cast<double>(anchor) + scroll) / px;
  const double z = std::clamp(px * factor, kMinZ, kMaxZ);
  px = static_cast<float>(z);
  scroll = static_cast<float>(std::max(0.0, tick * z - anchor));
}

// A view: the drawn zoom and scroll, and one frame of it with an optional zoom.
struct View {
  float px = 0.2f, scroll = 400.0f;
  Glide g;
  void frame(double factor, float anchor, bool precise = false, const Params& p = Params(), bool continuous = true) {
    g.begin(px, scroll, continuous, p);
    if (factor != 1.0) zoomAt(px, scroll, factor, anchor);
    g.end(px, scroll, kDt, !precise, p);
  }
  double tickAt(float anchor) const { return (static_cast<double>(anchor) + scroll) / px; }
};

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

}  // namespace

int main() {
  {  // The first frame adopts whatever the view holds.
    View v;
    v.frame(1.2, 300);
    expect(!v.g.active(), "first frame: no glide (nothing drawn before)");
  }
  {  // One wheel notch: glides over several frames, the point under the pointer fixed throughout.
    View v;
    v.frame(1.0, 0);  // drawn once
    float ip = v.px, is = v.scroll;
    zoomAt(ip, is, 1.2, 500);  // the instant result
    const double tick = v.tickAt(500);
    v.frame(1.2, 500);
    expect(v.g.active(), "a wheel notch starts a glide");
    expect(v.px > 0.2f && v.px < ip, "first frame: part of the way");
    int frames = 1;
    double worst = 0, prev = v.px;
    bool monotone = true;
    while (v.g.active() && frames < 200) {
      worst = std::max(worst, std::fabs((v.tickAt(500) - tick) * v.px));
      v.frame(1.0, 0);
      monotone = monotone && v.px >= prev;
      prev = v.px;
      ++frames;
    }
    worst = std::max(worst, std::fabs((v.tickAt(500) - tick) * v.px));
    std::printf("# notch glide: %d frames, worst anchor drift %.4f px\n", frames, worst);
    expect(frames >= 4, "visible: at least four frames in between");
    expect(frames * kDt < 0.4, "short: settled in well under 0.4 s");
    expect(worst < 0.05, "the point under the pointer stays put at every frame");
    expect(monotone, "never overshoots");
    expect(v.px == ip && v.scroll == is, "ends exactly where the instant zoom ends");
    // 1 % of the step left after ~110 ms.
    View w;
    w.frame(1.0, 0);
    const double lz0 = std::log(static_cast<double>(w.px));
    w.frame(1.2, 500);
    for (int i = 1; i < 7; ++i) w.frame(1.0, 0);  // 7 frames = 112 ms
    const double left = std::fabs(std::log(static_cast<double>(w.px) / ip)) / std::fabs(std::log(ip) - lz0);
    expect(left < 0.02, "about 1 % left after ~110 ms");
  }
  {  // Repeated notches retarget smoothly and compose like the instant zoom.
    View v;
    v.frame(1.0, 0);
    float ip = v.px, is = v.scroll;
    for (int k = 0; k < 4; ++k) zoomAt(ip, is, 1.2, 640);
    const double tick = v.tickAt(640);
    double prev = v.px, worst = 0;
    bool smoothStep = true;
    for (int k = 0; k < 4; ++k) {
      v.frame(1.2, 640);
      worst = std::max(worst, std::fabs((v.tickAt(640) - tick) * v.px));
      smoothStep = smoothStep && v.px > prev && v.px / prev < 1.2;  // continuous: no frame jumps a whole notch
      prev = v.px;
      v.frame(1.0, 0);
      worst = std::max(worst, std::fabs((v.tickAt(640) - tick) * v.px));
    }
    expect(smoothStep, "retargeting mid-flight continues from the drawn zoom");
    for (int i = 0; i < 100 && v.g.active(); ++i) v.frame(1.0, 0);
    expect(worst < 0.05, "repeated notches: the anchor stays put");
    expect(v.px == ip && v.scroll == is, "four notches end at four instant notches' zoom and scroll");
  }
  {  // Reversing direction mid-glide.
    View v;
    v.frame(1.0, 0);
    const float p0 = v.px, s0 = v.scroll;
    v.frame(1.2, 300);
    v.frame(1.0 / 1.2, 300);
    for (int i = 0; i < 100 && v.g.active(); ++i) v.frame(1.0, 0);
    expect(near(v.px, p0, 1e-6) && near(v.scroll, s0, 1e-3), "in and out again: back where it was");
  }
  {  // Clamped at the zoom limits: the target is the limit, the glide ends exactly there.
    View v;
    v.px = 7.5f;
    v.frame(1.0, 0);
    v.frame(1.2, 200);
    v.frame(1.2, 200);
    for (int i = 0; i < 100 && v.g.active(); ++i) v.frame(1.0, 0);
    expect(v.px == static_cast<float>(kMaxZ), "zoom in past the limit: ends at the limit");
    View w;
    w.px = 0.011f, w.scroll = 0;
    w.frame(1.0, 0);
    w.frame(1 / 1.2, 200);
    expect(w.g.active(), "zoom out to the limit glides");
    for (int i = 0; i < 100 && w.g.active(); ++i) w.frame(1.0, 0);
    expect(w.px == static_cast<float>(kMinZ) && w.scroll >= 0, "zoom out past the limit: ends at the limit, scroll never negative");
    w.frame(1 / 1.2, 200);
    expect(!w.g.active(), "already at the limit: nothing to glide");
  }
  {  // Smooth zoom off, reduced motion, trackpad: at once.
    Params off;
    off.smooth = false;
    View v;
    v.frame(1.0, 0, false, off);
    float ip = v.px, is = v.scroll;
    zoomAt(ip, is, 1.2, 500);
    v.frame(1.2, 500, false, off);
    expect(!v.g.active() && v.px == ip && v.scroll == is, "smooth zoom off: instant");
    View t;
    t.frame(1.0, 0);
    ip = t.px, is = t.scroll;
    zoomAt(ip, is, 1.03, 500);
    t.frame(1.03, 500, true);
    expect(!t.g.active() && t.px == ip && t.scroll == is, "trackpad (precise): 1:1, no lag");
    // Switched off mid-glide: there at once.
    View m;
    m.frame(1.0, 0);
    ip = m.px, is = m.scroll;
    zoomAt(ip, is, 1.5, 100);
    m.frame(1.5, 100);
    expect(m.g.active(), "glide under way");
    m.frame(1.0, 0, false, off);
    expect(!m.g.active() && m.px == ip && m.scroll == is, "switched off mid-glide: at the target at once");
    // The motion switches: smooth zoom follows the master switch and reduced motion.
    expect(motion::effective(true, true, true, true, true, false).smoothZoom, "smooth zoom on by default");
    expect(!motion::effective(true, true, true, true, false, false).smoothZoom, "smooth zoom setting off");
    expect(!motion::effective(false, true, true, true, true, false).smoothZoom, "animations off: no smooth zoom");
    expect(!motion::effective(true, true, true, true, true, true).smoothZoom, "reduced motion: no smooth zoom");
    expect(motion::effective(true, true, true, true, false).smoothZoom, "the old five-switch form keeps smooth zoom on");
  }
  {  // A view not drawn last frame adopts at once (a hidden editor, a project load).
    View v;
    v.frame(1.0, 0);
    v.px = 1.0f;
    v.frame(1.0, 0, false, Params(), false);
    expect(!v.g.active() && v.px == 1.0f, "not continuous: adopted");
  }
  {  // An outside write (menu, zoom tool) glides from the drawn view; scrolling moves a glide along.
    View v;
    v.frame(1.0, 0);
    v.px = 0.6f, v.scroll = 1200.0f;  // a fit / zoom to selection written between frames
    v.frame(1.0, 0);
    expect(v.g.active() && v.px > 0.2f && v.px < 0.6f, "an outside zoom write glides");
    const double A = v.g.anchorPx();
    const double tickA = v.tickAt(static_cast<float>(A));
    for (int i = 0; i < 100 && v.g.active(); ++i) {
      v.frame(1.0, 0);
      if (v.g.active()) expect(near(v.tickAt(static_cast<float>(A)), tickA, 1e-3), "the fixed point stays put");
    }
    expect(v.px == 0.6f && v.scroll == 1200.0f, "ends at the written view");
    View w, u;  // u: the same glide, not scrolled
    w.frame(1.0, 0), u.frame(1.0, 0);
    w.frame(2.0, 400), u.frame(2.0, 400);
    const float zBefore = w.px;
    w.scroll += 50;  // the wheel scrolled the drawn view
    w.frame(1.0, 0), u.frame(1.0, 0);
    const double moved = static_cast<double>(w.scroll - u.scroll);
    std::printf("# scrolled 50 px during a glide: %.2f px apart one frame later\n", moved);
    expect(w.g.active() && w.px == u.px, "scrolling during a glide: the zoom glides on");
    expect(moved >= 50 && moved <= 50 * static_cast<double>(w.px / zBefore) + 0.01, "and the scroll is carried along (scaled by the zoom since)");
    for (int i = 0; i < 100 && w.g.active(); ++i) w.frame(1.0, 0);
    expect(!w.g.active() && std::isfinite(w.scroll) && w.scroll >= 0, "and it settles");
    // A pure scroll (no zoom change) is never animated by the zoom glide.
    View q;
    q.frame(1.0, 0);
    q.scroll = 900;
    q.frame(1.0, 0);
    expect(!q.g.active() && q.scroll == 900, "a scroll alone applies at once");
  }
  {  // Frame-rate independence: 2 frames of 8 ms land where 1 frame of 16 ms does.
    Glide a, b;
    float pa = 0.2f, sa = 100, pb = 0.2f, sb = 100;
    Params p;
    a.begin(pa, sa, true, p), a.end(pa, sa, kDt, true, p);
    b.begin(pb, sb, true, p), b.end(pb, sb, kDt, true, p);
    a.begin(pa, sa, true, p), zoomAt(pa, sa, 1.5, 300), a.end(pa, sa, 0.016, true, p);
    b.begin(pb, sb, true, p), zoomAt(pb, sb, 1.5, 300), b.end(pb, sb, 0.008, true, p);
    b.begin(pb, sb, true, p), b.end(pb, sb, 0.008, true, p);
    expect(near(pa, pb, 1e-5) && near(sa, sb, 1e-2), "frame-rate independent");
  }
  return finish();
}
