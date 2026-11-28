// Gridline — K-410: platform-agnostic trackpad gesture bus (see TrackpadGesture.h).
#include "TrackpadGesture.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <mutex>

namespace gl::gesture {

namespace {
std::atomic<bool> gEnabled{true};
std::mutex& queueMutex() {
  static auto* m = new std::mutex;
  return *m;
}
std::vector<TrackpadGesture>& queueStorage() {
  static auto* q = new std::vector<TrackpadGesture>;
  return *q;
}
}  // namespace

double pinchFactor(double magnification) {
  if (!std::isfinite(magnification) || magnification == 0.0) return 1.0;
  return std::exp(std::clamp(magnification, -2.0, 2.0));
}

bool enabled() { return gEnabled.load(std::memory_order_relaxed); }

void setEnabled(bool on) { gEnabled.store(on, std::memory_order_relaxed); }

void post(const TrackpadGesture& g) {
  std::lock_guard<std::mutex> lock(queueMutex());
  auto& q = queueStorage();
  if (q.size() < 256) q.push_back(g);
}

bool hasPending() {
  std::lock_guard<std::mutex> lock(queueMutex());
  return !queueStorage().empty();
}

std::vector<TrackpadGesture> take() {
  std::lock_guard<std::mutex> lock(queueMutex());
  std::vector<TrackpadGesture> out;
  out.swap(queueStorage());
  return out;
}

void clear() {
  std::lock_guard<std::mutex> lock(queueMutex());
  queueStorage().clear();
}

void GestureState::beginFrame(bool gesturesEnabled, float uiScale, const std::vector<TrackpadGesture>& events) {
  items_.clear();
  if (!gesturesEnabled || events.empty()) return;
  const float invScale = (uiScale > 0.0f && std::isfinite(uiScale)) ? (1.0f / uiScale) : 1.0f;
  items_.reserve(events.size());
  for (TrackpadGesture g : events) {
    if (!std::isfinite(g.x) || !std::isfinite(g.y)) continue;
    if (g.kind == Kind::PinchZoom && !std::isfinite(g.magnification)) continue;
    if (g.kind == Kind::Pan2D && (!std::isfinite(g.dx) || !std::isfinite(g.dy))) continue;
    if (!g.logical) {
      g.x *= invScale;
      g.y *= invScale;
      g.dx *= invScale;
      g.dy *= invScale;
      g.logical = true;
    }
    items_.push_back(Item{g, false});
  }
}

void GestureState::endFrame() { items_.clear(); }

bool GestureState::matches(const Item& it, Kind kind, bool hovered, float x0, float y0, float x1, float y1, bool ignoreBounds) const {
  if (it.consumed || !hovered || it.g.kind != kind) return false;
  if (kind == Kind::Pan2D && !it.g.precise) return false;
  if (ignoreBounds) return true;
  return it.g.x >= x0 && it.g.x <= x1 && it.g.y >= y0 && it.g.y <= y1;
}

bool GestureState::consumePinch(bool hovered, float x0, float y0, float x1, float y1, float& magnificationOut, float& outX, float& outY,
                                bool ignoreBounds) {
  double sum = 0.0;
  bool any = false;
  for (Item& it : items_) {
    if (!matches(it, Kind::PinchZoom, hovered, x0, y0, x1, y1, ignoreBounds)) continue;
    it.consumed = true;
    sum += static_cast<double>(it.g.magnification);
    outX = it.g.x;
    outY = it.g.y;
    any = true;
  }
  if (!any) return false;
  magnificationOut = static_cast<float>(sum);
  return true;
}

bool GestureState::consumeSmartZoom(bool hovered, float x0, float y0, float x1, float y1, float& outX, float& outY, bool ignoreBounds) {
  for (Item& it : items_) {
    if (!matches(it, Kind::SmartZoom, hovered, x0, y0, x1, y1, ignoreBounds)) continue;
    it.consumed = true;
    outX = it.g.x;
    outY = it.g.y;
    return true;
  }
  return false;
}

bool GestureState::consumePan2D(bool hovered, float x0, float y0, float x1, float y1, float& dxOut, float& dyOut, bool ignoreBounds) {
  double sx = 0.0, sy = 0.0;
  bool any = false;
  for (Item& it : items_) {
    if (!matches(it, Kind::Pan2D, hovered, x0, y0, x1, y1, ignoreBounds)) continue;
    it.consumed = true;
    sx += static_cast<double>(it.g.dx);
    sy += static_cast<double>(it.g.dy);
    any = true;
  }
  if (!any) return false;
  dxOut = static_cast<float>(sx);
  dyOut = static_cast<float>(sy);
  return true;
}

void SmartZoomTimeline::toggle(const Song& s, float& pxPerTick, float& scrollX, float anchorPx, float fitPxPerTick, float fitScrollX,
                               float minScrollX, double zoomMul) {
  if (!(pxPerTick > 0.0f) || !std::isfinite(pxPerTick)) return;
  const float safeFit = static_cast<float>(timeline::clampZoom(s, fitPxPerTick > 0.0f ? fitPxPerTick : pxPerTick));
  const float safeFitScroll = std::max(minScrollX, std::isfinite(fitScrollX) ? fitScrollX : minScrollX);

  if (hasSaved && savedPx > 0.0f && targetPx > 0.0f) {
    const double drift = std::fabs(std::log(static_cast<double>(pxPerTick) / static_cast<double>(targetPx)));
    if (drift < 0.25) {
      pxPerTick = static_cast<float>(timeline::clampZoom(s, savedPx));
      scrollX = std::max(minScrollX, savedScroll);
      hasSaved = false;
      zoomedIn = false;
      return;
    }
  }

  savedPx = pxPerTick;
  savedScroll = scrollX;
  hasSaved = true;

  // Already zoomed noticeably past the fitted view: smart-zoom fits and remembers previous view.
  if (static_cast<double>(pxPerTick) > static_cast<double>(safeFit) * 1.35) {
    pxPerTick = safeFit;
    scrollX = safeFitScroll;
    targetPx = pxPerTick;
    zoomedIn = false;
    return;
  }

  // Otherwise zoom in smoothly around the pointer's tick and remember where we came from.
  const double mul = std::max(1.25, zoomMul);
  timeline::zoomAt(s, pxPerTick, scrollX, mul, anchorPx);
  scrollX = std::max(minScrollX, scrollX);
  targetPx = pxPerTick;
  zoomedIn = true;
}

void SmartZoomRange::toggle(double& viewStart, double& viewLen, double totalFrames, double anchorFrame, double zoomMul) {
  const double n = std::max(64.0, totalFrames);
  if (!(viewLen > 0.0) || !std::isfinite(viewLen)) viewLen = n;

  if (hasSaved && savedLen > 0.0 && targetLen > 0.0) {
    const double drift = std::fabs(std::log(viewLen / targetLen));
    if (drift < 0.25) {
      viewLen = std::clamp(savedLen, 16.0, n * 1.02);
      viewStart = std::clamp(savedStart, 0.0, std::max(0.0, n - viewLen));
      hasSaved = false;
      zoomedIn = false;
      return;
    }
  }

  savedStart = viewStart;
  savedLen = viewLen;
  hasSaved = true;

  if (viewLen < n * 0.65) {
    viewStart = 0.0;
    viewLen = n;
    targetLen = viewLen;
    zoomedIn = false;
    return;
  }

  const double mul = std::max(1.25, zoomMul);
  const double nextLen = std::clamp(viewLen / mul, 16.0, n * 1.02);
  const double t = std::clamp((anchorFrame - viewStart) / viewLen, 0.0, 1.0);
  viewStart = std::clamp(anchorFrame - t * nextLen, 0.0, std::max(0.0, n - nextLen));
  viewLen = nextLen;
  targetLen = nextLen;
  zoomedIn = true;
}

float WinZoomTracker::step(uint64_t dist, bool begin, bool end) {
  if (begin) {
    active = dist > 0;
    lastDist = dist;
    return 0.0f;
  }
  float mag = 0.0f;
  if (active && lastDist > 0 && dist > 0) {
    mag = static_cast<float>(std::log(static_cast<double>(dist) / static_cast<double>(lastDist)));
  }
  if (end) {
    active = false;
    lastDist = 0;
  } else if (dist > 0) {
    lastDist = dist;
    active = true;
  }
  return mag;
}

void WinPanTracker::step(int x, int y, bool begin, bool end, float& outDx, float& outDy) {
  outDx = 0.0f;
  outDy = 0.0f;
  if (begin) {
    active = true;
    lastX = x;
    lastY = y;
    return;
  }
  if (active) {
    outDx = -static_cast<float>(x - lastX);
    outDy = -static_cast<float>(y - lastY);
  }
  if (end) {
    active = false;
  } else {
    lastX = x;
    lastY = y;
    active = true;
  }
}

bool isPrecisionWheelDelta(int wheelDelta) {
  const int a = std::abs(wheelDelta);
  return a > 0 && (a % 120) != 0;
}

}  // namespace gl::gesture
