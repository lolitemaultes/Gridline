// Gridline — geometry of a hosted plugin's window.
#include "EditorLayout.h"
#include <algorithm>
#include <cmath>

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
    case kBarRoute:
      return 26;
    case kBarCpu:
      return 104;
    case kBarSettings:
    case kBarDock:
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
  for (int i = 0; i < kBarItemCount; ++i)
    if (i != kBarRoute && i != kBarDock) w += barItemWidth(static_cast<BarItem>(i));  // the route and dock buttons share the preset box's room
  // gaps between items, plus a wider separator before the switches and before the readout
  return w + (kBarItemCount - 3) * kGap + 2 * kGap;
}

BarLayout layoutBar(int width, bool route, bool dock) {
  width = std::max(width, kMinWindowW);
  BarLayout L;
  auto put = [&](BarItem it, int x, int row, int w) {
    L.items[it] = Rect{x, rowY(row), w, kItemH};
    L.visible[it] = true;
  };
  const int one = oneRowBarWidth();
  if (width >= one) {
    // < preset > | ON SANDBOX GENERIC ROUTE | ... CPU  [gear]   (preset stretches)
    const int extra = std::min(width - one, kPresetMaxW - kPresetMinW);
    const int spare = width - one - extra;  // room left over in the middle
    const int routeRoom = (route ? barItemWidth(kBarRoute) + kGap : 0) + (dock ? barItemWidth(kBarDock) + kGap : 0);
    const int pw = std::max(24, kPresetMinW + extra - std::max(0, routeRoom - spare));
    int x = kBarPad;
    put(kBarPrev, x, 0, barItemWidth(kBarPrev));
    x += barItemWidth(kBarPrev) + kGap;
    put(kBarPreset, x, 0, pw);
    x += pw + kGap;
    put(kBarNext, x, 0, barItemWidth(kBarNext));
    x += barItemWidth(kBarNext) + 2 * kGap;
    for (BarItem it : {kBarBypass, kBarSandbox, kBarGeneric, kBarRoute, kBarDock}) {
      if ((it == kBarRoute && !route) || (it == kBarDock && !dock)) continue;
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
  // Row 1: ON SANDBOX GENERIC ROUTE ... CPU (when it fits).
  {
    int x = kBarPad;
    for (BarItem it : {kBarBypass, kBarSandbox, kBarGeneric, kBarRoute, kBarDock}) {
      if ((it == kBarRoute && !route) || (it == kBarDock && !dock)) continue;
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

namespace {
int scaled(int units, double scale) { return static_cast<int>(std::lround(units * scale)); }
int unscaled(int pixels, double scale) { return static_cast<int>(std::lround(pixels / scale)); }
}  // namespace

WindowLayout layoutWindow(int pluginW, int pluginH, int panelH, double scale) {
  pluginW = std::max(pluginW, kMinPluginW);
  pluginH = std::max(pluginH, kMinPluginH);
  panelH = std::max(panelH, 0);
  WindowLayout L;
  L.w = std::max(pluginW, scaled(panelH > 0 ? kPanelMinW : kMinWindowW, scale));
  const int barH = scaled(layoutBar(unscaled(L.w, scale)).height, scale);
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

void pluginSizeForWindow(int winW, int winH, int panelH, int& pluginW, int& pluginH, double scale) {
  const int w = std::max(unscaled(winW, scale), kMinWindowW);
  pluginW = std::max(kMinPluginW, winW);
  pluginH = std::max(kMinPluginH, winH - scaled(layoutBar(w).height, scale) - std::max(panelH, 0));
}

WindowLayout barUnits(const WindowLayout& L, double scale) {
  WindowLayout u = L;
  u.w = unscaled(L.w, scale);
  u.h = unscaled(L.h, scale);
  u.bar = Rect{0, unscaled(L.bar.y, scale), u.w, unscaled(L.bar.h, scale)};
  u.panel = Rect{0, unscaled(L.panel.y, scale), u.w, unscaled(L.panel.h, scale)};
  return u;
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

InterfacePlace chooseInterfacePlace(bool settingInside, int pluginChoice, bool canEmbed) {
  if (!canEmbed) return InterfacePlace::OwnWindow;
  if (pluginChoice == kChoiceInside) return InterfacePlace::Inside;
  if (pluginChoice == kChoiceOwnWindow) return InterfacePlace::OwnWindow;
  return settingInside ? InterfacePlace::Inside : InterfacePlace::OwnWindow;
}

int interfaceChoiceFor(bool settingInside, bool inside) {
  if (inside == settingInside) return kChoiceSetting;
  return inside ? kChoiceInside : kChoiceOwnWindow;
}

Rect intersect(const Rect& a, const Rect& b) {
  const int x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y), x1 = std::min(a.right(), b.right()), y1 = std::min(a.bottom(), b.bottom());
  if (x1 <= x0 || y1 <= y0) return Rect{};
  return Rect{x0, y0, x1 - x0, y1 - y0};
}

DockedShow dockedShow(const Rect& full, const Rect& clip, const std::vector<Rect>& covers, bool holesCut) {
  DockedShow d;
  d.visible = intersect(full, clip);
  if (d.visible.empty()) {
    d.visible = Rect{};
    return d;
  }
  // Covers across a whole edge trim what shows (no hole needed: every system can do that).
  for (bool trimmed = true; trimmed && !d.visible.empty();) {
    trimmed = false;
    for (const Rect& c : covers) {
      Rect& v = d.visible;
      const Rect o = intersect(c, v);
      if (o.empty()) continue;
      const bool wide = o.x == v.x && o.w == v.w, tall = o.y == v.y && o.h == v.h;
      if (wide && tall) {
        v = Rect{};
      } else if (wide && o.y == v.y) {
        v = Rect{v.x, o.bottom(), v.w, v.bottom() - o.bottom()};
      } else if (wide && o.bottom() == v.bottom()) {
        v.h = o.y - v.y;
      } else if (tall && o.x == v.x) {
        v = Rect{o.right(), v.y, v.right() - o.right(), v.h};
      } else if (tall && o.right() == v.right()) {
        v.w = o.x - v.x;
      } else {
        continue;
      }
      trimmed = true;
      break;
    }
  }
  if (d.visible.empty()) {
    d.visible = Rect{};
    return d;
  }
  for (const Rect& c : covers) {
    const Rect o = intersect(c, d.visible);
    if (!o.empty()) d.holes.push_back(o);
  }
  if (!d.holes.empty() && !holesCut) {
    d.visible = Rect{};
    d.holes.clear();
  }
  return d;
}

}  // namespace gl::host::layout
