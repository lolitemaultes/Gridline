// Gridline - the style of a layout (K-112). See LayoutStyle.h.
#include "LayoutStyle.h"
#include <cstdio>

namespace gl::layout {

namespace {

Style studio() { return Style{}; }  // the defaults are Studio's own values

// Live: a flat control bar under its own menu row, inset rounded panels on a backdrop with gaps,
// small fields and rows, open-arc knobs, line-capped faders, flat buttons, twin meters,
// track headers on the right.
Style live() {
  Style s;
  s.preset = Preset::Live;
  s.menus = MenuPlace::OwnRow;
  s.menuRowH = 24;
  s.controlBarH = 45;
  s.statusBarH = 28;
  s.panels = PanelShape::Inset;
  s.tabs = TabKind::None;
  s.outerMargin = 8;
  s.panelGap = 6;
  s.panelRounding = 6;
  s.menuRounding = 0;  // menus: square corners, flat rows
  s.menuHeadings = false;  // sections split by lines only, no title row
  s.tabRounding = 4;
  s.listRowH = 19;
  s.trackRowH = 56;
  s.inspectorRowH = 20;
  s.mixerStripW = 76;
  s.rowSpacing = 4;
  s.itemSpacingX = 6;
  s.fieldPadX = 6;
  s.fieldPadY = 3;
  s.frameRounding = 2;
  s.labelPx = 11;
  s.knob = KnobKind::OpenArc;
  s.fader = FaderKind::LineCap;
  s.button = ButtonKind::Flat;
  s.meter = MeterKind::Twin;
  s.field = FieldKind::FillSlider;
  s.trackHeaders = HeaderSide::Right;
  return s;
}

// Logic: one control bar of pills and a centred display under the menu row, hairline separators,
// card-shaped inspector and panes with segmented title tabs, small-ring knobs, round-capped faders,
// pill buttons, scaled meters, no status bar.
Style logic() {
  Style s;
  s.preset = Preset::Logic;
  s.menus = MenuPlace::OwnRow;
  s.menuRowH = 24;
  s.controlBarH = 64;
  s.statusBarH = 0;
  s.panels = PanelShape::Card;
  s.tabs = TabKind::Segmented;
  s.outerMargin = 0;
  s.panelGap = 1;
  s.panelRounding = 8;
  s.tabRounding = 6;
  s.listRowH = 20;
  s.trackRowH = 44;
  s.inspectorRowH = 22;
  s.mixerStripW = 96;
  s.rowSpacing = 5;
  s.itemSpacingX = 8;
  s.fieldPadX = 8;
  s.fieldPadY = 4;
  s.frameRounding = 6;
  s.labelPx = 12;
  s.knob = KnobKind::SmallRing;
  s.fader = FaderKind::RoundCap;
  s.button = ButtonKind::Pill;
  s.meter = MeterKind::Scaled;
  s.field = FieldKind::Studio;
  return s;
}

// Tracker: a thin menu row, a compact transport row, square frames with hairlines, steppers and
// bevel buttons, segmented meters, numbers in the monospaced face, the densest rows.
Style tracker() {
  Style s;
  s.preset = Preset::Tracker;
  s.menus = MenuPlace::OwnRow;
  s.menuRowH = 22;
  s.controlBarH = 32;
  s.statusBarH = 0;  // K-106 cp16: no app status bar; messages go to the pages' own lines
  s.panels = PanelShape::Square;
  s.tabs = TabKind::Pages;
  s.outerMargin = 2;
  s.panelGap = 2;
  s.panelRounding = 0;
  s.tabRounding = 0;
  s.listRowH = 16;
  s.trackRowH = 30;
  s.inspectorRowH = 18;
  s.mixerStripW = 64;
  s.rowSpacing = 3;
  s.itemSpacingX = 4;
  s.fieldPadX = 4;
  s.fieldPadY = 2;
  s.frameRounding = 0;
  s.labelPx = 11;
  s.monoNumbers = true;
  s.knob = KnobKind::Stepper;
  s.fader = FaderKind::Bar;
  s.button = ButtonKind::Bevel;
  s.meter = MeterKind::Segmented;
  s.field = FieldKind::Stepper;
  return s;
}

}  // namespace

const Style& style(Preset p) {
  static const Style s[kLayoutCount] = {studio(), live(), logic(), tracker()};
  const int i = static_cast<int>(p);
  return i >= 0 && i < kLayoutCount ? s[i] : s[0];
}

float topBarHeight(const Style& s, bool menusInWindow) {
  return s.controlBarH + s.toolbarH + (s.menus == MenuPlace::OwnRow && menusInWindow ? s.menuRowH : 0.0f);
}

bool isStudio(const Style& s) { return s.preset == Preset::Studio; }

std::string describe(const Style& s) {
  char b[1024];
  std::snprintf(b, sizeof b,
                "preset %s\nmenus %d menuRowH %.0f controlBarH %.0f toolbarH %.0f statusBarH %.0f\n"
                "panels %d tabs %d outerMargin %.0f panelGap %.0f panelRounding %.0f tabRounding %.0f\n"
                "listRowH %.0f trackRowH %.0f inspectorRowH %.0f mixerStripW %.0f rowSpacing %.0f itemSpacingX %.0f "
                "fieldPad %.0f %.0f frameRounding %.0f labelPx %d monoNumbers %d\n"
                "knob %d fader %d button %d meter %d field %d trackHeaders %d\n",
                id(s.preset), static_cast<int>(s.menus), static_cast<double>(s.menuRowH), static_cast<double>(s.controlBarH),
                static_cast<double>(s.toolbarH), static_cast<double>(s.statusBarH), static_cast<int>(s.panels), static_cast<int>(s.tabs),
                static_cast<double>(s.outerMargin), static_cast<double>(s.panelGap), static_cast<double>(s.panelRounding),
                static_cast<double>(s.tabRounding), static_cast<double>(s.listRowH), static_cast<double>(s.trackRowH),
                static_cast<double>(s.inspectorRowH), static_cast<double>(s.mixerStripW), static_cast<double>(s.rowSpacing),
                static_cast<double>(s.itemSpacingX), static_cast<double>(s.fieldPadX), static_cast<double>(s.fieldPadY),
                static_cast<double>(s.frameRounding), s.labelPx, s.monoNumbers ? 1 : 0, static_cast<int>(s.knob), static_cast<int>(s.fader),
                static_cast<int>(s.button), static_cast<int>(s.meter), static_cast<int>(s.field), static_cast<int>(s.trackHeaders));
  return b;
}

}  // namespace gl::layout
