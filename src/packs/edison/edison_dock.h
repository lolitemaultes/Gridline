// Gridline - "edison" pack, K-15: Loupe's form laid out for the space it is given.
//
// Floating (its own window, J-207) the form keeps its design: one head of three rows
// (transport and record options; the nine menus and the tool strip; the info line) at
// 1100 px, scaled down to 60 % on a small work area, the waveform 250..600 px high.
//
// Pinned into the main layout the space is whatever the dock gives:
//  * wide (>= kCompactW): the same head, controls at their design size (never stretched:
//    the scale stops at 100 %), the extra width goes to the name field and the waveform,
//    and the waveform takes all the height that is left;
//  * compact (a side column, a narrow split): the head is stacked: transport; record
//    options; the menus
//    (two rows when one row would squeeze the words); the tool strip wrapped over as many
//    rows as it needs; the name; cursor and length. Controls at 70..85 %;
//  * never laid out below kMinW x (head + kMinWave): a smaller dock scrolls.
// Pure (tests/test_k19.cpp).
#pragma once
#include <algorithm>
#include <cmath>

namespace gl::packs::edison::dock {

constexpr float kDesignW = 1100;   // the form's design width (edison_style.h kDesignW)
constexpr float kCompactW = 660;   // narrower than this: the compact form (the floating form's narrowest)
constexpr float kMinW = 260;       // the narrowest form; a narrower dock scrolls sideways
constexpr float kMinWave = 140;    // lanes + ruler + scroll bar + bottom bar, at 100 %
// Design metrics (edison_style.h): margin, head buttons, fields, gaps.
constexpr float kPad = 8, kBtn = 28, kField = 24, kCellGap = 2, kGroupGap = 8;
// The tool strip's groups (buttons per group), left to right.
constexpr int kToolGroups[] = {3, 4, 1, 3, 6, 1};

struct Layout {
  bool compact = false;
  float formW = kDesignW;  // the width the form is laid out at
  float scale = 1;         // metrics scale (edison_style Metrics::s)
  float pad = kPad, btn = kBtn, fld = kField;
  // Head rows (y from the form's top); wide: menus and tools share yMenus.
  float yTransport = 0, yRec = 0, yMenus = 0, yTools = 0, yInfo = 0, yInfo2 = 0;  // yRec: compact only (wide: on the transport row)
  int menuRows = 1, toolRows = 1;
  float headH = 0;  // where the waveform editor starts
  float waveH = 0, minWaveH = 0;
};

inline float px(float v, float s) { return std::round(v * s); }

// Rows the tool strip needs between x0 and x1 (wrapping a button that would cross x1;
// a group gap is dropped at the start of a row).
inline int toolRowsFor(float x0, float x1, float btn, float cg, float gg) {
  int rows = 1;
  float x = x0;
  bool first = true;
  for (int n : kToolGroups) {
    if (!first && x > x0) x += gg - cg;
    first = false;
    for (int i = 0; i < n; ++i) {
      if (x > x0 && x + btn > x1 + 0.5f) {
        ++rows;
        x = x0;
      }
      x += btn + cg;
    }
  }
  return rows;
}

// The form in its own window: `width` already fitted to the work area (>= 660), the
// waveform 250..600 px within maxHeight (J-207, unchanged).
inline Layout floatingLayout(float width, float maxHeight) {
  Layout l;
  l.formW = std::floor(width);
  l.scale = std::clamp(width / kDesignW, 0.6f, 1.0f);
  const float s = l.scale;
  l.pad = px(kPad, s), l.btn = px(kBtn, s), l.fld = px(kField, s);
  l.yTransport = l.pad;
  l.yMenus = l.yTools = l.yTransport + l.btn + l.pad;
  l.yInfo = l.yMenus + l.btn + l.pad;
  l.headH = l.yInfo + l.fld + l.pad;
  l.minWaveH = px(250, s);
  l.waveH = std::clamp(std::floor(maxHeight - l.headH - l.pad), px(250, s), px(600, s));
  return l;
}

// The form pinned into a dock space of availW x availH logical px.
inline Layout layoutFor(float availW, float availH) {
  Layout l;
  const float W = std::floor(std::max(availW, kMinW));
  l.formW = W;
  l.compact = W < kCompactW;
  l.scale = l.compact ? std::clamp(W / 400.0f, 0.7f, 0.85f) : std::clamp(W / kDesignW, 0.6f, 1.0f);
  const float s = l.scale;
  l.pad = px(kPad, s), l.btn = px(kBtn, s), l.fld = px(kField, s);
  l.minWaveH = px(kMinWave, s);
  l.yTransport = l.pad;
  if (!l.compact) {
    l.yMenus = l.yTools = l.yTransport + l.btn + l.pad;
    l.yInfo = l.yMenus + l.btn + l.pad;
    l.headH = l.yInfo + l.fld + l.pad;
  } else {
    const float L = l.pad, R = W - l.pad;
    const float cg = px(kCellGap, s), gg = px(kGroupGap, s);
    l.yRec = l.yTransport + l.btn + l.pad;  // record options under the transport
    l.yMenus = l.yRec + l.fld + l.pad;
    l.menuRows = (R - L) / 9.0f < px(52, s) ? 2 : 1;  // nine words in one row only when each gets room
    l.yTools = l.yMenus + static_cast<float>(l.menuRows) * l.btn + l.pad;
    l.toolRows = toolRowsFor(L, R, l.btn, cg, gg);
    l.yInfo = l.yTools + static_cast<float>(l.toolRows) * (l.btn + cg) - cg + l.pad;
    l.yInfo2 = l.yInfo + l.fld + px(4, s);
    l.headH = l.yInfo2 + l.fld + l.pad;
  }
  l.waveH = std::max(l.minWaveH, std::floor(availH - l.headH - l.pad));
  return l;
}

}  // namespace gl::packs::edison::dock
