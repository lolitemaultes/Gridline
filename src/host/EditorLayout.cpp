// Gridline — geometry of a hosted plugin's window.
#include "EditorLayout.h"
#include <algorithm>

namespace gl::host::layout {

int barItemWidth(BarItem item) {
  switch (item) {
    case kBarPrev:
    case kBarNext:
      return 22;
    case kBarPreset:
      return kPresetMinW;
    case kBarBypass:
      return 30;
    case kBarSandbox:
      return 62;
    case kBarGeneric:
      return 62;
    case kBarCpu:
      return 104;
    case kBarSettings:
      return 26;
    default:
      return 0;
  }
}

namespace {
const int kItemH = kBarRowH - 2 * kBarPad + 4;  // 22: items fill the row minus padding
int rowY(int row) { return row * kBarRowH + (kBarRowH - kItemH) / 2; }
}  // namespace

int oneRowBarWidth() {
  int w = 2 * kBarPad;
  for (int i = 0; i < kBarItemCount; ++i) w += barItemWidth(static_cast<BarItem>(i));
  // gaps between items, plus a wider separator before the switches and before the readout
  return w + (kBarItemCount - 1) * kGap + 2 * kGap;
}

BarLayout layoutBar(int width) {
  width = std::max(width, kMinWindowW);
  BarLayout L;
  auto put = [&](BarItem it, int x, int row, int w) {
    L.items[it] = Rect{x, rowY(row), w, kItemH};
    L.visible[it] = true;
  };
  const int one = oneRowBarWidth();
  if (width >= one) {
    // < preset > | ON SANDBOX GENERIC | ... CPU  [gear]   (preset stretches)
    const int extra = std::min(width - one, kPresetMaxW - kPresetMinW);
    int x = kBarPad;
    put(kBarPrev, x, 0, barItemWidth(kBarPrev));
    x += barItemWidth(kBarPrev) + kGap;
    put(kBarPreset, x, 0, kPresetMinW + extra);
    x += kPresetMinW + extra + kGap;
    put(kBarNext, x, 0, barItemWidth(kBarNext));
    x += barItemWidth(kBarNext) + 2 * kGap;
    for (BarItem it : {kBarBypass, kBarSandbox, kBarGeneric}) {
      put(it, x, 0, barItemWidth(it));
      x += barItemWidth(it) + kGap;
    }
    // Readout and settings stay on the right edge.
    const int gx = width - kBarPad - barItemWidth(kBarSettings);
    put(kBarSettings, gx, 0, barItemWidth(kBarSettings));
    put(kBarCpu, gx - kGap - barItemWidth(kBarCpu), 0, barItemWidth(kBarCpu));
    L.rows = 1;
    L.height = kBarRowH;
    return L;
  }
  // Two rows. Row 0: < preset > ... [gear]; the preset takes what is left.
  {
    const int gx = width - kBarPad - barItemWidth(kBarSettings);
    put(kBarSettings, gx, 0, barItemWidth(kBarSettings));
    int x = kBarPad;
    put(kBarPrev, x, 0, barItemWidth(kBarPrev));
    x += barItemWidth(kBarPrev) + kGap;
    const int room = gx - kGap - barItemWidth(kBarNext) - kGap - x;
    const int pw = std::max(24, std::min(room, kPresetMaxW));
    put(kBarPreset, x, 0, pw);
    put(kBarNext, x + pw + kGap, 0, barItemWidth(kBarNext));
  }
  // Row 1: ON SANDBOX GENERIC ... CPU (when it fits).
  {
    int x = kBarPad;
    for (BarItem it : {kBarBypass, kBarSandbox, kBarGeneric}) {
      put(it, x, 1, barItemWidth(it));
      x += barItemWidth(it) + kGap;
    }
    const int cw = barItemWidth(kBarCpu);
    if (x + kGap + cw <= width - kBarPad) put(kBarCpu, width - kBarPad - cw, 1, cw);
  }
  L.rows = 2;
  L.height = 2 * kBarRowH;
  return L;
}

WindowLayout layoutWindow(int pluginW, int pluginH, int panelH) {
  pluginW = std::max(pluginW, kMinPluginW);
  pluginH = std::max(pluginH, kMinPluginH);
  panelH = std::max(panelH, 0);
  WindowLayout L;
  L.w = std::max(pluginW, panelH > 0 ? kPanelMinW : kMinWindowW);
  const int barH = layoutBar(L.w).height;
  L.plugin = Rect{(L.w - pluginW) / 2, 0, pluginW, pluginH};
  L.bar = Rect{0, pluginH, L.w, barH};
  L.panel = Rect{0, pluginH + barH, L.w, panelH};
  L.h = pluginH + barH + panelH;
  return L;
}

WindowLayout layoutBarWindow(int width, int panelH) {
  WindowLayout L;
  panelH = std::max(panelH, 0);
  L.w = std::max(width, panelH > 0 ? kPanelMinW : kMinWindowW);
  const int barH = layoutBar(L.w).height;
  L.plugin = Rect{0, 0, 0, 0};
  L.bar = Rect{0, 0, L.w, barH};
  L.panel = Rect{0, barH, L.w, panelH};
  L.h = barH + panelH;
  return L;
}

void pluginSizeForWindow(int winW, int winH, int panelH, int& pluginW, int& pluginH) {
  const int w = std::max(winW, kMinWindowW);
  pluginW = std::max(kMinPluginW, winW);
  pluginH = std::max(kMinPluginH, winH - layoutBar(w).height - std::max(panelH, 0));
}

Rect fitOnScreen(Rect r, const Rect& work) {
  if (work.empty()) return r;
  if (r.right() > work.right()) r.x = work.right() - r.w;
  if (r.bottom() > work.bottom()) r.y = work.bottom() - r.h;
  r.x = std::max(r.x, work.x);
  r.y = std::max(r.y, work.y);
  return r;
}

Rect placeWindow(int w, int h, const Rect& work, bool remembered, int rx, int ry, int index) {
  Rect r{0, 0, w, h};
  if (remembered) {
    r.x = rx;
    r.y = ry;
  } else {
    const int step = 26 * (index % 8);
    r.x = work.x + (work.w - w) / 2 + step;
    r.y = work.y + std::max(0, (work.h - h) / 3) + step;
  }
  return fitOnScreen(r, work);
}

EditorMode chooseEditorMode(bool pluginHasInterface, bool userChoseGeneric) {
  return pluginHasInterface && !userChoseGeneric ? EditorMode::Native : EditorMode::Generic;
}

}  // namespace gl::host::layout
