// Gridline — J-84: channel rack row layout.
//
// A rack row is a fixed left section (mute / level LED, pan knob, volume
// knob, mixer track box, channel name button), the step area and the select
// lamp at its right. The reference rack keeps its channel buttons whatever
// the pattern length: a long pattern (32 / 64 / 128 steps) or a narrow rack
// scrolls the step grid sideways under a time scroller in the bottom bar;
// the pan and volume knobs never disappear (the old layout dropped them in
// a "compact" mode, so long patterns hid them).
//
// Space is given out in this order:
//   1. the left section's controls (LED, pan, volume, mixer box): always;
//   2. the name button shrinks from kNameMax towards kNameMin before the
//      steps shrink below their comfortable width (the full name is then in
//      a tooltip; kNameSqueeze for patterns of up to 32 steps, which can
//      then still fit without scrolling), and down to kNameFloor only when even the minimum step
//      viewport would not fit;
//   3. steps shrink from kStepMax to kStepMin;
//   4. past that the step area scrolls horizontally (scrollMax > 0) and the
//      rack shows its time scroller.
// All values are logical pixels (UI scale is applied to the whole frame).
//
// Pure: no UI dependencies, unit tested (tests/test_rack_layout.cpp).
#pragma once

namespace gl::racklayout {

inline constexpr float kXLed = 2, kXPan = 18, kXVol = 40, kXMix = 64, kNameX = 94;  // left section (row-relative)
inline constexpr float kKnobW = 18;            // pan / volume knob cell (radius 9)
inline constexpr float kMixW = 24;             // mixer track box
inline constexpr float kNameMax = 150, kNameMin = 64, kNameSqueeze = 48, kNameFloor = 36;
inline constexpr float kNameGap = 8;           // name button -> steps
inline constexpr float kStepMax = 22, kStepComfort = 16, kStepMin = 13;
inline constexpr float kStepGap = 2, kGroupGap = 4;  // between steps / extra between groups of four
inline constexpr float kSelLampW = 14;         // select lamp right of the steps
inline constexpr float kMinView = 2 * (kStepMin + kStepGap);  // smallest step viewport (two steps)

struct Input {
  float avail = 0;  // inner width of the rows pane
  int steps = 16;   // pattern steps (bars x steps per bar)
};

struct Layout {
  int steps = 16;
  float nameW = kNameMax;
  bool nameShrunk = false;  // below kNameMax: names may be truncated (tooltip)
  float stepsX = 0;         // step viewport left edge (row-relative)
  float viewW = 0;          // visible width of the step area
  float stepW = kStepMax;
  float contentW = 0;       // all steps side by side
  float scrollMax = 0;      // > 0: the step area scrolls horizontally
  float lampX = 0;          // select lamp left edge (row-relative)
  float rowW = 0;           // whole row (left section + view + lamp)

  // Left edge of step k inside the step content (scroll 0).
  float stepPos(int k) const;
  // Step under content x (scroll included by the caller), -1 outside.
  int stepAt(float x) const;
  bool scrolls() const { return scrollMax > 0; }
};

float contentWidth(int steps, float stepW);
Layout compute(const Input& in);

// Keeps a scroll position inside [0, scrollMax] (a pattern got shorter, the
// rack wider).
float clampScroll(const Layout& l, float scroll);
// Scroll that brings step k fully into view (unchanged when it already is).
float revealStep(const Layout& l, float scroll, int k);

// Time scroller (bottom bar): thumb of a track `trackW` wide for this layout
// and scroll. Returns false when nothing scrolls.
bool thumb(const Layout& l, float scroll, float trackW, float* x0, float* w);
// Track position (0 .. trackW) -> scroll that centres the thumb there.
float scrollForTrack(const Layout& l, float trackX, float trackW);

}  // namespace gl::racklayout
