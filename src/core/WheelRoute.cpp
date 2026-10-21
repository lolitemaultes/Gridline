// Gridline — J-83: wheel routing over controls in scrolling panes. See WheelRoute.h.
#include "WheelRoute.h"

namespace gl::wheelroute {

Mode modeFromSetting(double v) {
  if (v == 0) return Mode::Adjust;
  if (v == 1) return Mode::Scroll;
  return kDefaultMode;
}

bool paneScrolls(Mode m, bool narrow) {
  switch (m) {
    case Mode::Adjust: return false;
    case Mode::Scroll: return true;
    case Mode::Auto: break;
  }
  return narrow;
}

bool rackScrolls(Mode m, bool rowsScroll, bool stepsScroll) { return paneScrolls(m, rowsScroll || stepsScroll); }

void Router::beginFrame(double now, float wheel) {
  now_ = now;
  wheel_ = wheel;
  claimed_ = false;
  focusSeen_ = false;
  if (wheel != 0) {
    if (now - last_ > kGestureGap) latch_ = Latch::None;  // a new gesture
    last_ = now;
  }
}

void Router::endFrame() {
  if (wheel_ != 0) {
    if (!claimed_) latch_ = Latch::Scroll;
    else if (latch_ == Latch::None) latch_ = Latch::Adjust;
  }
  if (!focusSeen_) focus_ = 0;
}

bool Router::scrolling() const { return latch_ == Latch::Scroll && now_ - last_ <= kGestureGap; }

Route Router::route(const Query& q) const {
  if (!q.paneScrolls) return Route::Adjust;   // the normal mixer: as before
  if (scrolling()) return Route::Scroll;      // a running pane scroll (momentum included) stays one
  if (q.modifier || q.focused) return Route::Adjust;
  return Route::Scroll;
}

float Router::scale(const Query& q) { return q.paneScrolls && q.modifier ? kFineScale : 1.0f; }

}  // namespace gl::wheelroute
