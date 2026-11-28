// Gridline - K-404: pure geometry and visibility culling for the Studio Device chain.
//
// Each device card snaps to the Device chain panel's full inner height. Built-in plugin editors
// scale crisply to fit the available body height (clamped to [kMinScale, kMaxScale]); collapsed
// devices become slim vertical strips (kCollapsedW) and off-screen devices are culled (visible =
// false) so neither runs editor drawing, scopes or meters.
//
// Pure header (no ImGui / SDL dependency), unit-tested in tests/test_k404_device_chain.cpp.
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace gl::devchain {

// K-405 forward compatibility: Row is shipped in 0.7.9; Braid (Graph view) joins in 0.8.0.
enum class ViewMode : int { Row = 0 };

constexpr float kHeaderH = 26.0f;       // consistent sleek device header bar across every slot
constexpr float kCollapsedW = 30.0f;    // slim vertical strip when collapsed (zero scopes/editor)
constexpr float kHostedCardW = 248.0f;  // compact card width for installed third-party plugins (0.7.9)
constexpr float kAddCardW = 136.0f;     // "+ Add effect" end-of-chain card width
constexpr float kGap = 3.0f;            // crisp edge-to-edge seam between cards
constexpr float kSignalGap = 14.0f;     // slim signal-flow chevron gap after the instrument
constexpr float kPadX = 4.0f;           // inner horizontal padding around the scaled editor body
constexpr float kPadY = 4.0f;           // inner vertical padding around the scaled editor body
constexpr float kMinCardH = 88.0f;      // minimum card height even in a tiny panel
constexpr float kMinExpandedW = 212.0f; // minimum expanded card width so header buttons never collide
constexpr float kMinScale = 0.35f;      // smallest crisp vector fit scale (compact lower pane)
constexpr float kMaxScale = 1.35f;      // largest crisp vector fit scale (tall panel)

struct DeviceSpec {
  float designW = 600.0f;     // editor design width at 100 % (logical px)
  float designH = 360.0f;     // measured or estimated design height at 100 % (logical px)
  bool collapsed = false;     // true: renders as a slim kCollapsedW vertical strip
  bool hosted = false;        // true: installed plugin compact card (0.7.9)
  bool isInstrument = false;  // true: signal-flow chevron sits after this card when effects follow
};

struct DeviceBox {
  float x = 0.0f;        // card left offset inside the horizontal row (from 0)
  float w = 0.0f;        // snapped card width
  float h = 0.0f;        // snapped card height (matches panelH)
  float bodyW = 0.0f;    // inner body width (w - 2 * kPadX)
  float bodyH = 0.0f;    // inner body height (h - kHeaderH - 2 * kPadY)
  float scale = 1.0f;    // vector scale for built-in editor (1.0 for hosted / collapsed)
  float scaledW = 0.0f;  // floor(designW * scale)
  float scaledH = 0.0f;  // ceil(designH * scale)
  bool visible = true;   // intersects horizontal viewport [scrollX, scrollX + viewW]
};

struct RowLayout {
  std::vector<DeviceBox> boxes;
  float addX = 0.0f;    // left offset of the "+ Add effect" card
  float totalW = 0.0f;  // total scrollable row width
  float cardH = 0.0f;   // snapped height of every card in the row
};

// Vector scale that fits an editor of `designH` design pixels into `bodyH` logical pixels.
inline float fitScale(float bodyH, float designH) {
  if (!(bodyH > 0.0f) || !(designH > 0.0f)) return 1.0f;
  const float raw = bodyH / designH;
  const float clamped = std::clamp(raw, kMinScale, kMaxScale);
  // Quantise to 0.5 % steps to avoid sub-pixel shimmer on fractional heights.
  return std::floor(clamped * 200.0f + 0.001f) / 200.0f;
}

// Fallback design height before an editor has been measured once.
inline float estimateDesignHeight(float designW) {
  return std::clamp(std::round(designW * 0.60f), 200.0f, 520.0f);
}

// Computes snapped edge-to-edge card geometry and horizontal viewport culling for the chain row.
inline RowLayout computeRow(const std::vector<DeviceSpec>& specs, float panelH, float scrollX, float viewW, bool hasAddCard = true) {
  RowLayout out;
  out.cardH = std::max(kMinCardH, std::floor(panelH));
  const float bodyH = std::max(24.0f, out.cardH - kHeaderH - 2.0f * kPadY);
  const float view0 = scrollX;
  const float view1 = scrollX + std::max(0.0f, viewW);
  float x = 0.0f;
  out.boxes.reserve(specs.size());
  for (size_t i = 0; i < specs.size(); ++i) {
    const DeviceSpec& sp = specs[i];
    if (i > 0) {
      x += specs[i - 1].isInstrument ? kSignalGap : kGap;
    }
    DeviceBox b;
    b.x = x;
    b.h = out.cardH;
    b.bodyH = bodyH;
    if (sp.collapsed) {
      b.w = kCollapsedW;
      b.bodyW = 0.0f;
      b.scale = 1.0f;
      b.scaledW = 0.0f;
      b.scaledH = 0.0f;
    } else if (sp.hosted) {
      b.w = kHostedCardW;
      b.bodyW = std::max(40.0f, b.w - 2.0f * kPadX);
      b.scale = 1.0f;
      b.scaledW = b.bodyW;
      b.scaledH = bodyH;
    } else {
      const float dw = std::max(160.0f, sp.designW);
      const float dh = sp.designH > 16.0f ? sp.designH : estimateDesignHeight(dw);
      b.scale = fitScale(bodyH, dh);
      b.scaledW = std::floor(dw * b.scale + 0.01f);
      b.scaledH = std::ceil(dh * b.scale - 0.01f);
      b.w = std::max(kMinExpandedW, b.scaledW + 2.0f * kPadX);
      b.bodyW = std::max(40.0f, b.w - 2.0f * kPadX);
    }
    // Viewport culling: a card is visible iff it overlaps [view0, view1] by > 1 px.
    b.visible = (viewW <= 0.0f) ? true : (b.x + b.w > view0 + 1.0f && b.x < view1 - 1.0f);
    x += b.w;
    out.boxes.push_back(b);
  }
  if (hasAddCard) {
    if (!specs.empty()) x += specs.back().isInstrument ? kSignalGap : kGap;
    out.addX = x;
    x += kAddCardW;
  } else {
    out.addX = x;
  }
  out.totalW = x;
  return out;
}

// Remaps an effect slot index `slot` after `moveSlot(owner, from, to)` so UI state (collapsed,
// selected) follows the moved device.
inline int remapSlotOnMove(int slot, int from, int to) {
  if (slot == from) return to;
  if (from < to && slot > from && slot <= to) return slot - 1;
  if (from > to && slot >= to && slot < from) return slot + 1;
  return slot;
}

}  // namespace gl::devchain
