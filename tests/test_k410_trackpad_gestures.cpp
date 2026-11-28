// Gridline — K-410: trackpad gestures unit tests (core/TrackpadGesture.h).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <vector>

#include "Check.h"
#include "../src/core/Settings.h"
#include "../src/core/SmoothScroll.h"
#include "../src/core/SmoothZoom.h"
#include "../src/core/Song.h"
#include "../src/core/Timeline.h"
#include "../src/core/TrackpadGesture.h"

using namespace gl;
using namespace gl::gesture;

namespace {

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

}  // namespace

int main() {
  // ---- 1. pinchFactor symmetry, finiteness, clamping ----
  {
    expect(near(pinchFactor(0.0), 1.0, 1e-12), "pinchFactor(0) == 1");
    expect(near(pinchFactor(std::numeric_limits<double>::quiet_NaN()), 1.0, 1e-12), "pinchFactor(NaN) == 1");
    expect(near(pinchFactor(std::numeric_limits<double>::infinity()), 1.0, 1e-12), "pinchFactor(Inf) == 1");
    bool symmetric = true;
    for (double m : {0.01, 0.05, 0.15, 0.5, 1.25, 2.0}) {
      const double up = pinchFactor(m);
      const double dn = pinchFactor(-m);
      symmetric = symmetric && up > 1.0 && dn > 0.0 && dn < 1.0 && near(up * dn, 1.0, 1e-12);
    }
    expect(symmetric, "pinchFactor(+m) * pinchFactor(-m) == 1 (exact zero drift)");
    expect(pinchFactor(-100.0) > 0.0 && std::isfinite(pinchFactor(100.0)), "pinchFactor clamps extremes safely");
  }

  // ---- 2. Event queue (post / hasPending / take / clear) ----
  {
    clear();
    expect(!hasPending(), "queue starts empty");
    TrackpadGesture a;
    a.kind = Kind::PinchZoom;
    a.magnification = 0.25f;
    a.x = 400.0f;
    a.y = 300.0f;
    TrackpadGesture b;
    b.kind = Kind::Pan2D;
    b.dx = 12.0f;
    b.dy = -8.0f;
    post(a);
    post(b);
    expect(hasPending(), "hasPending true after post");
    const auto taken = take();
    expect(taken.size() == 2 && taken[0].kind == Kind::PinchZoom && taken[1].kind == Kind::Pan2D, "FIFO order preserved");
    expect(!hasPending() && take().empty(), "take drains the queue");
  }

  // ---- 3. GestureState: enable switch, uiScale, bounds, single-consume, coalescing ----
  {
    GestureState st;
    TrackpadGesture p1{Kind::PinchZoom, Phase::Changed, 500.0f, 300.0f, 0.10f, 0.0f, 0.0f, true, false, 0};
    TrackpadGesture p2{Kind::PinchZoom, Phase::Changed, 510.0f, 304.0f, 0.08f, 0.0f, 0.0f, true, false, 0};
    TrackpadGesture pan{Kind::Pan2D, Phase::Changed, 500.0f, 300.0f, 0.0f, 40.0f, 20.0f, true, false, 0};
    TrackpadGesture discrete{Kind::Pan2D, Phase::Changed, 500.0f, 300.0f, 0.0f, 80.0f, 80.0f, false, true, 0};
    TrackpadGesture smart{Kind::SmartZoom, Phase::Ended, 500.0f, 300.0f, 0.0f, 0.0f, 0.0f, true, false, 0};

    // Disabled: nothing passes through.
    st.beginFrame(false, 1.0f, {p1, p2, pan, smart});
    expect(st.empty(), "disabled GestureState drops all events");
    float mag = 0, x = 0, y = 0, dx = 0, dy = 0;
    expect(!st.consumePinch(true, 0, 0, 1000, 1000, mag, x, y), "disabled: pinch ignored");
    expect(!st.consumeSmartZoom(true, 0, 0, 1000, 1000, x, y), "disabled: smartzoom ignored");
    expect(!st.consumePan2D(true, 0, 0, 1000, 1000, dx, dy), "disabled: pan ignored");

    // Enabled with 2x UI scale: OS points halved into logical px.
    st.beginFrame(true, 2.0f, {p1, p2, pan, discrete, smart});
    expect(!st.consumePinch(false, 0, 0, 1000, 1000, mag, x, y), "not hovered: pinch not consumed");
    expect(!st.consumePinch(true, 0, 0, 100, 100, mag, x, y), "out of bounds: pinch not consumed");
    expect(st.consumePinch(true, 200, 100, 400, 250, mag, x, y), "in bounds at 2x uiScale: pinch consumed");
    expect(near(mag, 0.18, 1e-5) && near(x, 255.0, 1e-4) && near(y, 152.0, 1e-4), "pinches coalesce in log space and scale coords");
    expect(!st.consumePinch(true, 200, 100, 400, 250, mag, x, y), "consumed pinch cannot be double-consumed");

    expect(st.consumePan2D(true, 200, 100, 400, 250, dx, dy), "precise Pan2D consumed");
    expect(near(dx, 20.0, 1e-4) && near(dy, 10.0, 1e-4), "discrete Pan2D ignored; precise Pan2D scaled by uiScale");
    expect(!st.consumePan2D(true, 200, 100, 400, 250, dx, dy), "consumed Pan2D cannot be double-consumed");

    expect(st.consumeSmartZoom(true, 200, 100, 400, 250, x, y), "SmartZoom consumed");
    expect(near(x, 250.0, 1e-4) && near(y, 150.0, 1e-4), "SmartZoom scaled by uiScale");
    expect(!st.consumeSmartZoom(true, 200, 100, 400, 250, x, y), "SmartZoom single-consume");
  }

  // ---- 4. Anchored pinch zoom on a timeline (zero drift < 1.0 px over repeated cycles) ----
  {
    Song song;
    song.ppq = 96;
    float px = 0.25f;
    float scroll = 600.0f;
    const float anchorPx = 420.0f;
    const double tick0 = (static_cast<double>(anchorPx) + static_cast<double>(scroll)) / static_cast<double>(px);
    smoothzoom::Glide glide;
    smoothzoom::Params zp;
    zp.smooth = true;

    // Warm up one frame so Glide has continuous state.
    glide.begin(px, scroll, true, zp);
    glide.end(px, scroll, 0.016, true, zp);

    double worstDrift = 0.0;
    const float initPx = px;
    const float initScroll = scroll;

    // 10 pinch-in steps followed by 10 symmetric pinch-out steps.
    for (int step = 0; step < 20; ++step) {
      const double mag = (step < 10) ? 0.14 : -0.14;
      glide.begin(px, scroll, true, zp);
      timeline::zoomAt(song, px, scroll, pinchFactor(mag), anchorPx);
      glide.end(px, scroll, 0.016, false /* precise 1:1 */, zp);
      expect(!glide.active(), "pinch zoom applies 1:1 with zero glide lag");
      const double drawnAtTick = tick0 * static_cast<double>(px) - static_cast<double>(scroll);
      worstDrift = std::max(worstDrift, std::fabs(drawnAtTick - static_cast<double>(anchorPx)));
    }
    std::printf("# timeline pinch 20-step cycle: worst anchor drift %.5f px\n", worstDrift);
    expect(worstDrift < 0.05, "anchored pinch keeps cursor tick within < 0.05 px across 20 steps");
    expect(near(px, initPx, 1e-4) && near(scroll, initScroll, 0.25), "symmetric pinch in & out returns to start");

    // Extreme zoom clamp safety.
    for (int i = 0; i < 40; ++i) timeline::zoomAt(song, px, scroll, pinchFactor(1.5), anchorPx);
    expect(std::isfinite(px) && std::isfinite(scroll) && px <= static_cast<float>(timeline::maxPxPerTick(song)) + 1e-4f && scroll >= 0.0f,
           "extreme pinch-in clamps cleanly");
    for (int i = 0; i < 60; ++i) timeline::zoomAt(song, px, scroll, pinchFactor(-1.5), anchorPx);
    expect(std::isfinite(px) && std::isfinite(scroll) && px >= static_cast<float>(timeline::minPxPerTick(song)) - 1e-6f && scroll >= 0.0f,
           "extreme pinch-out clamps cleanly");
  }

  // ---- 5. Two-finger 2D pan with SmoothScroll::Axis ----
  {
    smoothscroll::Axis axX, axY;
    smoothscroll::Params sp;
    sp.smooth = true;
    sp.elastic = true;
    double sx = 100.0, sy = 80.0;
    axX.reset(sx);
    axY.reset(sy);
    for (int i = 0; i < 5; ++i) {
      axX.scroll(24.5, false /* precise */, 0.0, 5000.0, sp);
      axY.scroll(13.25, false /* precise */, 0.0, 2000.0, sp);
      sx = axX.update(0.016, 0.0, 5000.0, sp);
      sy = axY.update(0.016, 0.0, 2000.0, sp);
    }
    expect(near(sx, 100.0 + 5.0 * 24.5, 1e-3), "2D pan X moves smoothly 1:1");
    expect(near(sy, 80.0 + 5.0 * 13.25, 1e-3), "2D pan Y moves smoothly 1:1");
  }

  // ---- 6. SmartZoomTimeline & SmartZoomRange toggles ----
  {
    Song song;
    song.ppq = 96;
    SmartZoomTimeline sz;
    const float fitZ = 0.15f, fitS = 0.0f;
    float px = fitZ, scroll = 200.0f;
    const float anchorPx = 300.0f;
    const double tick0 = (static_cast<double>(anchorPx) + static_cast<double>(scroll)) / static_cast<double>(px);

    // First double-tap zooms in around pointer tick.
    sz.toggle(song, px, scroll, anchorPx, fitZ, fitS);
    expect(sz.zoomedIn && px > fitZ * 2.0f, "SmartZoom first tap zooms in");
    const double drawnAfter = tick0 * static_cast<double>(px) - static_cast<double>(scroll);
    expect(std::fabs(drawnAfter - anchorPx) < 0.05, "SmartZoom anchors pointer tick on zoom-in");

    // Second double-tap restores previous view.
    sz.toggle(song, px, scroll, anchorPx, fitZ, fitS);
    expect(!sz.zoomedIn && near(px, fitZ, 1e-5) && near(scroll, 200.0, 1e-2), "SmartZoom second tap restores previous view");

    // If manually zoomed deep, SmartZoom fits and a second tap restores that deep zoom cleanly.
    px = fitZ * 4.0f;
    scroll = 900.0f;
    sz.toggle(song, px, scroll, anchorPx, fitZ, fitS);
    expect(!sz.zoomedIn && sz.hasSaved && near(px, fitZ, 1e-5) && near(scroll, fitS, 1e-2),
           "SmartZoom from manual deep zoom returns to fit");
    sz.toggle(song, px, scroll, anchorPx, fitZ, fitS);
    expect(!sz.hasSaved && near(px, fitZ * 4.0f, 1e-5) && near(scroll, 900.0, 1e-2),
           "SmartZoom second tap from fit restores saved deep zoom");

    // Loupe frame-span SmartZoomRange.
    SmartZoomRange sr;
    const double total = 44100.0 * 4.0;
    double vStart = 0.0, vLen = total;
    const double anchorFrame = total * 0.4;
    const double frac0 = (anchorFrame - vStart) / vLen;
    sr.toggle(vStart, vLen, total, anchorFrame);
    expect(sr.zoomedIn && vLen < total * 0.5, "Loupe SmartZoom zooms in");
    expect(near((anchorFrame - vStart) / vLen, frac0, 1e-6), "Loupe SmartZoom keeps anchor sample fixed");
    sr.toggle(vStart, vLen, total, anchorFrame);
    expect(!sr.zoomedIn && near(vStart, 0.0, 1e-3) && near(vLen, total, 1e-3), "Loupe SmartZoom second tap restores full view");
  }

  // ---- 7. Windows WM_GESTURE & Precision Touchpad trackers ----
  {
    WinZoomTracker zt;
    expect(near(zt.step(100, true, false), 0.0, 1e-9), "WinZoom begin returns 0");
    const float m1 = zt.step(200, false, false);
    const float m2 = zt.step(100, false, true);
    expect(near(m1, std::log(2.0), 1e-5) && near(m2, -std::log(2.0), 1e-5), "WinZoom computes exact log distance ratios");
    expect(near(pinchFactor(m1) * pinchFactor(m2), 1.0, 1e-6), "WinZoom in/out cancels cleanly");
    expect(!zt.active, "WinZoom end clears active state");

    WinPanTracker pt;
    float dx = 0, dy = 0;
    pt.step(500, 400, true, false, dx, dy);
    expect(dx == 0.0f && dy == 0.0f, "WinPan begin returns 0,0");
    pt.step(470, 380, false, false, dx, dy);
    expect(near(dx, 30.0, 1e-6) && near(dy, 20.0, 1e-6), "WinPan converts pointer delta to scroll delta");
    pt.step(460, 380, false, true, dx, dy);
    expect(near(dx, 10.0, 1e-6) && near(dy, 0.0, 1e-6) && !pt.active, "WinPan end applies final delta and resets");

    expect(!isPrecisionWheelDelta(0) && !isPrecisionWheelDelta(120) && !isPrecisionWheelDelta(-120) && !isPrecisionWheelDelta(240),
           "discrete 120-unit wheel clicks are not precision touchpad");
    expect(isPrecisionWheelDelta(15) && isPrecisionWheelDelta(-30) && isPrecisionWheelDelta(60) && isPrecisionWheelDelta(-150),
           "sub-120 deltas are detected as Precision Touchpad");
  }

  // ---- 8. Settings toggle default & persistence ----
  {
    Settings s;
    expect(s.getBool("input.trackpadGestures") == true, "input.trackpadGestures defaults to true");
    expect(s.set("input.trackpadGestures", false), "input.trackpadGestures accepts bool");
    const auto dir = std::filesystem::temp_directory_path() / "gridline_k410_settings_test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const std::string path = (dir / "settings.json").string();
    expect(s.save(path), "settings saved");
    Settings s2;
    expect(s2.load(path) && s2.getBool("input.trackpadGestures") == false, "input.trackpadGestures persists across save/load");
    std::filesystem::remove_all(dir, ec);
  }

  return finish();
}
