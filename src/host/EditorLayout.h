// Gridline — geometry of a hosted plugin's window (pure functions, tested).
//
// An installed plugin opens as itself: its own interface fills the top of a
// Gridline window, a compact control bar (presets, bypass, sandbox, generic,
// CPU / latency, settings) runs under it at the plugin's width, and the
// optional settings panel (processing, MIDI, sidechain, parameters) unfolds
// under the bar, growing the window. Collapsing the panel restores the
// plugin + bar size. The bar never covers the plugin.
//
//   +---------------------------+  y = 0
//   |   plugin's own interface  |  plugin rect (centred when narrower than
//   |                           |  the minimum window width)
//   +---------------------------+
//   | < preset >  ON SB GEN ... |  bar: one row, or two when narrow
//   +---------------------------+
//   |  settings panel (opt.)    |  panel rect (0 high when collapsed)
//   +---------------------------+
//
// Units: the window system's logical units (points on macOS, pixels on X11),
// the same units plugin editors report their size in.
#pragma once
#include <cstdint>

namespace gl::host::layout {

inline constexpr int kBarRowH = 28;      // one row of the control bar
inline constexpr int kBarPad = 5;        // bar inner padding
inline constexpr int kGap = 4;           // gap between bar items
inline constexpr int kMinWindowW = 232;  // narrowest window (tiny plugins sit centred)
inline constexpr int kPanelH = 340;      // height of the unfolded settings panel
inline constexpr int kPanelMinW = 460;   // the unfolded panel widens narrow windows to this
inline constexpr int kMinPluginW = 64, kMinPluginH = 48;

struct Rect {
  int x = 0, y = 0, w = 0, h = 0;
  int right() const { return x + w; }
  int bottom() const { return y + h; }
  bool empty() const { return w <= 0 || h <= 0; }
  bool overlaps(const Rect& o) const { return !empty() && !o.empty() && x < o.right() && o.x < right() && y < o.bottom() && o.y < bottom(); }
  bool contains(const Rect& o) const { return o.x >= x && o.y >= y && o.right() <= right() && o.bottom() <= bottom(); }
  bool operator==(const Rect& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
};

// Items of the control bar, in reading order.
enum BarItem : int {
  kBarPrev = 0,  // previous preset
  kBarPreset,    // preset name / menu (stretches)
  kBarNext,      // next preset
  kBarBypass,    // plugin on / bypassed
  kBarSandbox,   // run in sandbox
  kBarGeneric,   // generic parameter editor instead of the plugin's interface
  kBarRoute,     // J-195: route the instrument's channel to a free mixer track (channels only)
  kBarCpu,       // CPU and latency readout (dropped first when narrow)
  kBarSettings,  // unfold the settings panel
  kBarItemCount
};

// Preferred / minimum widths of each item.
int barItemWidth(BarItem item);
inline constexpr int kPresetMinW = 96, kPresetMaxW = 300;

struct BarLayout {
  int height = kBarRowH;
  int rows = 1;
  Rect items[kBarItemCount];
  bool visible[kBarItemCount] = {};
};
// Lays the bar out for a window `width` wide (clamped to kMinWindowW): one
// row when everything fits, otherwise presets + settings on the first row and
// the switches (and the readout, when it fits) on the second. The route button
// takes its room from the preset box on one row (never from the row's width);
// `route`: the bar has it (an instrument's window, not an effect's). Item rects
// are relative to the bar's top-left corner.
BarLayout layoutBar(int width, bool route = false);
// Narrowest width that keeps the bar on one row.
int oneRowBarWidth();

struct WindowLayout {
  int w = 0, h = 0;  // window content size
  Rect plugin, bar, panel;
};
// Window for a plugin view of pluginW x pluginH with the panel folded
// (panelH = 0) or unfolded (panelH > 0).
WindowLayout layoutWindow(int pluginW, int pluginH, int panelH);
// Window without a plugin view (a sandboxed plugin's bar window: the
// interface lives in the sandbox process' own window above it).
WindowLayout layoutBarWindow(int width, int panelH);
// The user resized the window to winW x winH: the plugin size this asks
// for (before the plugin's own constraints).
void pluginSizeForWindow(int winW, int winH, int panelH, int& pluginW, int& pluginH);

// Moves a window rect inside the screen work area (never resizes; the
// top-left corner wins when the window is larger than the area).
Rect fitOnScreen(Rect r, const Rect& work);
// Where a new plugin window goes: its remembered position, or cascaded from
// the work area's centre by `index` windows.
Rect placeWindow(int w, int h, const Rect& work, bool remembered, int rx, int ry, int index);

// Which editor a plugin opens with.
enum class EditorMode {
  Native,   // the plugin's own interface (always the default when it has one)
  Generic,  // Gridline's parameter list: plugins without an interface, or the user asked for it
};
EditorMode chooseEditorMode(bool pluginHasInterface, bool userChoseGeneric);

}  // namespace gl::host::layout
