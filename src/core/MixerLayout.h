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

}  // namespace gl::mixlayout
