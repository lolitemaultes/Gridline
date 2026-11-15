// Gridline - the style of a layout (K-112): density, sizes and control kinds.
//
// A layout changes how the app is built, never what colour it is: this table has no colour,
// theme, font or scale field at all, so the person's theme, accent, channel and track colours,
// fonts and interface size stay theirs in every layout. The layout picks the KIND of control
// its own chrome draws (a stepper, a line-capped fader, a bevel button); the person's control
// style (Settings > Theme > Controls) draws that kind in its own finish. Plugin editors are never
// touched by a layout: they draw inside a base-style scope (app/LayoutStyleUi.h).
//
// Studio is the app as it has always been: every Studio value is the value the interface used
// before layouts had styles, and the app applies nothing for it.
//
// All sizes are logical pixels at 100 % interface scale.
#pragma once
#include <cstdint>
#include <string>
#include "LayoutPreset.h"

namespace gl::layout {

// Where a layout's menus live.
enum class MenuPlace : uint8_t {
  InBar,   // inside the top bar, next to the transport (Studio)
  OwnRow,  // a row of their own above the control bar (the macOS menu bar takes them over there)
};

// Control kinds of the layout's own chrome, by role. Each widget keeps its job; the kind is
// its shape. Studio = the shape the app has always drawn.
enum class KnobKind : uint8_t { Studio, OpenArc, SmallRing, Stepper };
enum class FaderKind : uint8_t { Studio, LineCap, RoundCap, Bar };
enum class ButtonKind : uint8_t { Studio, Flat, Pill, Bevel };
enum class MeterKind : uint8_t { Studio, Twin, Scaled, Segmented };
enum class FieldKind : uint8_t { Studio, FillSlider, Stepper };
enum class PanelShape : uint8_t { Studio, Inset, Card, Square };
enum class TabKind : uint8_t { Studio, Segmented, None, Pages };
enum class HeaderSide : uint8_t { Left, Right };

struct Style {
  Preset preset = Preset::Studio;

  // ---- bars ----
  MenuPlace menus = MenuPlace::InBar;
  float menuRowH = 0;      // OwnRow only
  float controlBarH = 48;  // the transport row
  float toolbarH = 0;      // a second toolbar row (0 = none)
  float statusBarH = 26;   // 0 = no status bar

  // ---- panels (the region shell) ----
  PanelShape panels = PanelShape::Studio;
  TabKind tabs = TabKind::Studio;
  float outerMargin = 4;    // dock area inset from the bars and window edges
  float panelGap = 4;       // splitter gap between docked panels
  float panelRounding = 4;  // floating windows and child panels
  float menuRounding = -1;  // menus and popups (-1: as the panels)
  bool menuHeadings = true; // section headings and title rows in menus (false: plain separators, no title row)
  float tabRounding = 3;

  // ---- density ----
  float listRowH = 22;      // browser and list rows
  float trackRowH = 46;     // playlist / arrangement track rows, as a new view opens
  float inspectorRowH = 24;
  float mixerStripW = 76;   // narrow mixer strip
  float rowSpacing = 6;     // vertical gap between rows of items (ImGui ItemSpacing.y)
  float itemSpacingX = 8;
  float fieldPadX = 8;      // padding inside fields and buttons
  float fieldPadY = 5;
  float frameRounding = 3;  // fields and buttons
  int labelPx = 0;          // label size step (0 = the theme's; 11 or 12 = a denser label face)
  bool monoNumbers = false; // numbers, positions and rows in the monospaced face

  // ---- control kinds of the chrome ----
  KnobKind knob = KnobKind::Studio;
  FaderKind fader = FaderKind::Studio;
  ButtonKind button = ButtonKind::Studio;
  MeterKind meter = MeterKind::Studio;
  FieldKind field = FieldKind::Studio;
  HeaderSide trackHeaders = HeaderSide::Left;
};

// The style of a layout. Always valid; unknown presets give Studio.
const Style& style(Preset p);
// The top bar's height: the menu row (when the menus have one and it is drawn in the window)
// plus the control bar and toolbar.
float topBarHeight(const Style& s, bool menusInWindow);
// True when the style changes nothing (Studio): the app applies no style then.
bool isStudio(const Style& s);
// One line per field, for reports and tests ("controlBarH 45").
std::string describe(const Style& s);

}  // namespace gl::layout
