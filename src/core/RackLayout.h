// Gridline — J-84: channel rack row layout.
//
// A rack row is a fixed left section (K-186: the headphones button, which
// mutes on click and solos on right-click; pan knob, volume knob, mixer track
// box, channel name button), the channel's light right after the name
// button (K-127: it glows while the channel plays and shows mute; its clicks
// mute and solo too) and the step area after it. The reference rack keeps its channel buttons whatever
// the pattern length: a long pattern (32 / 64 / 128 steps) or a narrow rack
// scrolls the step grid sideways under a time scroller in the bottom bar;
// the pan and volume knobs never disappear (the old layout dropped them in
// a "compact" mode, so long patterns hid them).
//
// Space is given out in this order:
//   1. the left section's controls (headphones, pan, volume, mixer box): always;
//   2. the name button shrinks from kNameMax towards kNameMin before the
//      steps shrink below their comfortable width (the full name is then in
//      a tooltip; kNameSqueeze for patterns of up to 32 steps, which can
//      then still fit without scrolling), and down to kNameFloor only when even the minimum step
//      viewport would not fit. K-295: never below what the rack's names need to read in full
//      (Input::nameNeed, up to kNameRead): the steps of a pattern of up to 32 then tighten to
//      kStepTight so they still fit (the default rack keeps a 2-bar pattern whole), and past that
//      the last steps scroll, instead of every name being cut to three letters;
//   3. steps shrink from kStepMax to kStepMin;
//   4. past that the step area scrolls horizontally (scrollMax > 0) and the
//      rack shows its time scroller.
// All values are logical pixels (UI scale is applied to the whole frame).
//
// Pure: no UI dependencies, unit tested (tests/test_rack_layout.cpp).
#pragma once

namespace gl::racklayout {

inline constexpr float kXLeft = 2, kXPan = 23, kXVol = 45, kXMix = 69, kNameX = 99;  // left section (row-relative)
inline constexpr float kPhonesW = 18;          // K-186: the headphones (mute / solo) button at kXLeft
inline constexpr float kKnobW = 18;            // pan / volume knob cell (radius 9)
inline constexpr float kMixW = 24;             // mixer track box
inline constexpr float kNameMax = 150, kNameMin = 64, kNameSqueeze = 48, kNameFloor = 36;
inline constexpr float kNameRead = 78;      // K-295: the most a name's own need holds the button open (longer names: tooltip)
inline constexpr float kNameGap = 8;           // name button -> steps (besides the lamp cell between them)
inline constexpr float kLampGap = 4;           // K-127: name button -> light cell (the rest of kNameGap: light cell -> steps)
inline constexpr float kStepMax = 22, kStepComfort = 16, kStepMin = 13;
inline constexpr float kStepTight = 12;  // K-295: up to 32 steps beside names that need their room
inline constexpr float kStepGap = 2, kGroupGap = 4;  // between steps / extra between step groups (one beat each)
inline constexpr float kSelLampW = 14;         // the channel's light cell, between the name and the steps
inline constexpr float kMinView = 2 * (kStepMin + kStepGap);  // smallest step viewport (two steps)

struct Input {
  float avail = 0;  // inner width of the rows pane
  int steps = 16;   // pattern steps (bars x steps per bar)
  float stepMax = kStepMax;  // widest step (a layout whose steps fill the pane passes more)
  float nameNeed = 0;        // K-295: name button width the widest channel name needs (0: unknown)
  int group = 4;             // K-236: steps per group (one beat of the pattern's meter, stepGroup)
  float laneW = 0;           // K-255: the lane column (each row's grid chip) between the light and the steps (0: none)
};
inline constexpr float kLaneW = 34;  // K-255: the lane column when shown

// K-236: steps in one beat of a pattern's meter, the rack's step group (its gap and its colour
// band): 4 in 4/4 and 3/4 at 16 / 12 steps a bar, 2 in 7/8 at 14, 6 in 6/8 (a dotted quarter
// beat). Steps that do not divide into the meter's beats keep groups of four.
int stepGroup(int stepsPerBar, int meterNum, int meterDen);

struct Layout {
  int steps = 16;
  int group = 4;            // K-236: steps per group
  float nameW = kNameMax;
  bool nameShrunk = false;  // below kNameMax: names may be truncated (tooltip)
  float stepsX = 0;         // step viewport left edge (row-relative)
  float viewW = 0;          // visible width of the step area
  float stepW = kStepMax;
  float contentW = 0;       // all steps side by side
  float scrollMax = 0;      // > 0: the step area scrolls horizontally
  float lampX = 0;          // light cell left edge (row-relative), right of the name button
  float laneX = 0;          // K-255: the lane column's left edge (laneW wide; 0 wide when not shown)
  float laneW = 0;
  float rowW = 0;           // whole row (left section + lamp + view)

  // Left edge of step k inside the step content (scroll 0).
  float stepPos(int k) const;
  // Step under content x (scroll included by the caller), -1 outside.
  int stepAt(float x) const;
  bool scrolls() const { return scrollMax > 0; }
};

float contentWidth(int steps, float stepW, int group = 4);  // K-236: `group` steps per group
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

// K-136: the rack's height follows its content. A floating rack is never taller than its
// header, rows and add button: `rowsTop` is where the rows start in the window (title bar and
// header above), `rowsContent` the rows' content height (step lamps, channel rows, automation
// rows, graph editor), `bar` the add button bar under them and `bottomChrome` the window's
// frame below its content. An empty rack keeps room for its hint (`emptyMin` under the header).
inline float fitHeight(float rowsTop, float rowsContent, float bar, float bottomChrome, bool empty, float emptyMin) {
  const float h = rowsTop + (empty ? emptyMin : rowsContent + bar) + bottomChrome;
  return h > 0 ? static_cast<float>(static_cast<int>(h + 0.999f)) : 0.0f;
}
// The rows' region: exactly their content when that is shorter than the room above the add bar
// (the bar then sits under the last row, also in a docked rack), else 0: the region fills the
// room and the list scrolls.
inline float rowsRegion(float rowsContent, float room) { return rowsContent > 0 && rowsContent < room ? rowsContent : 0.0f; }

}  // namespace gl::racklayout
