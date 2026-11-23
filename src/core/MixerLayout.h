// Gridline — mixer layout for thin panes (J-38). Pure geometry, no ImGui, so
// tests can check every width.
//
// Below kColumnBelow logical pixels the mixer stacks: the strips flow into rows
// (master first, then the inserts, wrapping like text), the strips stretch a
// little so each row fills the pane instead of leaving a ragged gap, every row
// keeps a usable fader height (rows scroll vertically rather than shrink), and
// the inspector sits below the strips when the pane is tall enough. Secondary
// elements go first when room runs out: the master's dB scale, then the
// inspector.
#pragma once
#include <vector>

namespace gl::mixlayout {

inline constexpr float kColumnBelow = 520.0f;   // narrower panes stack
inline constexpr float kMinStripW = 54.0f;      // compact strip
inline constexpr float kMaxStripW = 72.0f;      // stretched strips in a column
inline constexpr float kNameMinPx = 40.0f;      // room for a short strip name
inline constexpr float kInspectorMinH = 200.0f;
inline constexpr float kInspectorMaxH = 380.0f;
inline constexpr float kComfortExtra = 110.0f;  // fader travel above the bare minimum strip
inline constexpr float kRowGap = 6.0f;

struct Input {
  float width = 0, height = 0;  // area below the toolbar (logical px)
  int strips = 1;               // master + inserts (master first)
  float masterExtra = 24.0f;    // the master's extra width (wider plate + dB scale)
  float masterScaleW = 16.0f;   // part of masterExtra taken by the dB scale (dropped first)
  float minStripH = 176.0f;     // shortest strip for the current view options
  float bandH = 0;              // group band above each row
  float addButtonW = 36.0f;     // "+" after the last strip
  float padding = 8.0f;         // strips pane padding (each side)
  float scrollbarW = 8.0f;
  float gap = 2.0f;
  // GitHub #58: a width of its own per strip (master first; 0 or missing: the computed width), for
  // slim strips (narrow tracks, folded groups).
  std::vector<float> widths;
};

struct Cell {
  int index = 0;  // 0 = master
  int row = 0;
  float x = 0, w = 0;  // relative to the content origin
};

struct Layout {
  bool column = false;           // stacked thin-pane layout (false: the normal side-by-side mixer)
  bool showInspector = true;
  bool masterScale = true;       // the master keeps its dB scale
  float stripsH = 0, inspectorH = 0;  // column: heights of the strips pane and the inspector below it
  float contentW = 0;            // usable width inside the strips pane
  float stripW = 0;              // insert strip width (master: + masterW - stripW)
  float masterW = 0;
  int perRow = 1, rows = 1;
  float rowH = 0;                // strip height
  float rowPitch = 0;            // distance between row tops (band + strip + gap)
  int addRow = 0;                // "+" button position
  float addX = 0;
  std::vector<Cell> cells;
};

// Comfortable row height in a column (minimum strip + fader travel).
float minRowHeight(const Input& in);
Layout compute(const Input& in);

// Thin-pane toolbar (J-38, J-46): the optional groups after "Slots" and the
// waveform toggle, in order: meter mode, peak hold, routing. `room` is the
// width left at the first optional group; each width includes the gap (and
// divider) before its group. A group shows only when it and the groups before
// it fit; dropped groups stay in the mixer menu.
struct ToolbarFit {
  bool meters = false, hold = false, routing = false;
};
ToolbarFit toolbarFit(float room, float metersW, float holdW, float routingW);

// The side-by-side mixer (kColumnBelow and wider): the master zone (the left dock, master first)
// beside the strips that wrap into rows, the "+" button, the right dock, then the inspector.
// The master zone spans every row, so no empty space is left under it; the strips zone keeps to
// its content and the inspector takes the rest of the width. Dragging the line between them sets
// the inspector's own width (`preferred`, 0 = automatic); the strips zone then scrolls sideways.
inline constexpr float kInspectorMinW = 240.0f;
inline constexpr float kInspectorMaxW = 320.0f;  // automatic width
inline constexpr float kStripsMinW = 160.0f;     // what a dragged inspector leaves the strips
struct WideInput {
  int left = 1;          // strips in the master zone (the master included)
  int middle = 0;        // strips that wrap into rows
  int right = 0;         // strips in the right dock
  int rows = 1;          // 1..4
  float stripW = 66.0f, masterW = 90.0f, gap = 2.0f;
  float dividerW = 12.0f;   // room for the line after the master zone
  float addW = 40.0f;       // the "+" button and its margins
  float padding = 8.0f;     // strips pane padding (each side)
  float tail = 8.0f;        // room after the last strip
  // GitHub #58: slim strips. Widths of the middle strips (empty: all stripW), each row taking
  // perRow() of them in order; the width the left and right docks save against stripW each.
  std::vector<float> middleW;
  float leftSaved = 0, rightSaved = 0;
};
// The width of row `row` of the middle strips (gaps between them included).
float middleRowW(const WideInput& in, int row);
int perRow(int strips, int rows);
// The strips zone's content width (padding included).
float wideContentW(const WideInput& in);
// The inspector's width in a mixer `width` wide, for strips content `contentW` wide.
float wideInspectorW(float width, float contentW, float preferred);
// The height of a strip in the master zone: every row's strip, band and gap.
float masterZoneH(int rows, float rowH, float bandH, float rowGap);

}  // namespace gl::mixlayout
