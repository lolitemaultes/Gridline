#include "Launcher.h"
#include <algorithm>
#include <cmath>

namespace gl::launcher {

StartupPlan planStartup(const StartupInputs& in) {
  StartupPlan p;
  // The demo song greets a new install once (J-135), even with the launcher on: the
  // launcher waits behind it (the logo shows it). Files on the command line open over it.
  if (!in.demoShown) {
    p.markDemoShown = true;
    if (in.freshInstall) {
      p.action = StartAction::Demo;
      return p;
    }
  }
  if (in.forceLauncher) {
    p.showLauncher = true;
    return p;
  }
  if (in.plainLaunch && in.launcherOn) {
    p.showLauncher = true;
    return p;
  }
  // Without the launcher (or started with files / options): Settings > General > On startup.
  if (in.startupMode == "empty") p.action = StartAction::Empty;
  else if (in.startupMode == "last" && in.lastExists) p.action = StartAction::Last;
  else if (in.defaultTemplate) p.action = StartAction::Template;
  return p;
}

void State::set(bool v, double now) {
  if (v != visible_) changedAt_ = now;
  visible_ = v;
}

void State::showAtStartup(double now) {
  projectOpen_ = false;
  set(true, now);
}

void State::showHome(double now) { set(true, now); }

bool State::back(double now) {
  if (!projectOpen_) return false;
  set(false, now);
  return true;
}

void State::projectOpened(double now) {
  projectOpen_ = true;
  set(false, now);
}

void State::projectClosed(double now) {
  projectOpen_ = false;
  set(true, now);
}

namespace {

// The grid items in `avail` px of width (tile size, columns, rectangles, content height).
void placeItems(const LayoutIn& in, Layout& L, float avail) {
  L.items.clear();
  L.cols = std::max(1, static_cast<int>((avail + kGap) / (kTileMin + kGap)));
  L.tileW = std::min(kTileMax, (avail - kGap * static_cast<float>(L.cols - 1)) / static_cast<float>(L.cols));
  L.tileW = std::floor(L.tileW);
  L.thumbH = std::round(L.tileW * kThumbRatio);
  L.tileH = L.thumbH + kCaptionH;
  float y = 0;
  int col = 0;
  size_t block = 0;
  for (Item it : in.items) {
    if (it == Item::Block) {  // J-146: a full-width panel on rows of its own
      if (col > 0) y += L.tileH + kGap, col = 0;
      const float h = block < in.blockH.size() ? std::max(0.0f, in.blockH[block]) : 0.0f;
      ++block;
      L.items.push_back({0, y, avail, h});
      y += h + kGap;
      continue;
    }
    if (it == Item::Section) {
      if (col > 0) y += L.tileH + kGap, col = 0;
      L.items.push_back({0, y, avail, kSectionH});
      y += kSectionH;
      continue;
    }
    L.items.push_back({static_cast<float>(col) * (L.tileW + kGap), y, L.tileW, L.tileH});
    if (++col == L.cols) col = 0, y += L.tileH + kGap;
  }
  if (col > 0) y += L.tileH;
  else if (y > 0) y -= kGap;
  L.contentH = y;
}

}  // namespace

Layout layout(const LayoutIn& in) {
  Layout L;
  const float W = std::max(1.0f, in.width), H = std::max(1.0f, in.height);
  L.rail = W >= kNarrowBelow;
  const float pad = L.rail ? kPad : kPadNarrow;
  float cx = 0, cy = 0;  // content origin
  if (L.rail) {
    L.railRect = {0, 0, kRailW, H};
    const float x = kRailPad, w = kRailW - 2 * kRailPad;
    float y = 28;
    L.railRows.push_back({RailRow::Brand, {x, y, std::min(w, in.brandW), 28}});
    y += 28 + 32;
    for (int i = 0; i < in.actions; ++i, y += kRailRowH) L.railRows.push_back({RailRow::Action, {x - 8, y, w + 16, kRailRowH}});
    y += 22;
    L.railRows.push_back({RailRow::Label, {x, y, w, 18}});
    y += 18 + 6;
    for (int i = 0; i < in.views; ++i, y += kRailRowH) L.railRows.push_back({RailRow::View, {x - 8, y, w + 16, kRailRowH}});
    // Bottom: settings and the version, anchored to the bottom unless that would cover the rows above.
    const float verH = 16, bottom = H - 20;
    float sy = bottom - verH - 10 - kRailRowH;
    const bool room = sy >= y + 12;
    if (!room) sy = y + 12;
    L.railRows.push_back({RailRow::Settings, {x - 8, sy, w + 16, kRailRowH}});
    if (room) L.railRows.push_back({RailRow::Version, {x, bottom - verH, w, verH}});
    cx = kRailW;
  } else {
    // Narrow: brand left, icon buttons right, on one header row.
    L.brand = {pad, 14, in.brandW, 26};
    const int icons = in.actions + in.views + 1;
    float x = W - pad;
    for (int i = 0; i < icons; ++i) {
      x -= kIconBtn;
      L.headerIcons.insert(L.headerIcons.begin(), {x, 11, kIconBtn, kIconBtn});
      x -= 4;
    }
    // Too many icons for the row: drop the brand's text room first (the brand shrinks to the mark).
    if (L.headerIcons.front().x < L.brand.r() + 8) L.brand.w = std::max(26.0f, L.headerIcons.front().x - 8 - L.brand.x);
    cy = 54;
  }
  const float cw = W - cx;
  // Header: title left; back (if any) and search right.
  const float hy = cy + (L.rail ? 28 : 6);
  float right = cx + cw - pad;
  if (in.hasBack) {
    const float bw = std::min(in.backW, std::max(80.0f, cw * 0.4f));
    L.back = {right - bw, hy + 6, bw, 32};
    right = L.back.x - 10;
  }
  const float titleX = cx + pad;
  const float minTitle = std::min(in.titleW, 120.0f);
  float sw = std::min(kSearchW, right - titleX - minTitle - 16);
  if (sw < kSearchMin) sw = 0;  // no room: the search hides (Ctrl+F still filters)
  if (sw > 0) {
    L.search = {right - sw, hy + 6, sw, 32};
    right = L.search.x - 16;
  }
  L.title = {titleX, hy, std::max(0.0f, std::min(in.titleW, right - titleX)), kHeaderH};
  // Grid.
  const float gy = hy + kHeaderH + 20;
  L.grid = {cx + pad, gy, std::max(1.0f, cw - 2 * pad), std::max(1.0f, H - gy - (L.rail ? 24 : 12))};
  placeItems(in, L, L.grid.w);
  // Tiles overflow: lay out again beside the scrollbar, so it never covers the last column.
  if (L.contentH > L.grid.h && in.scrollbar > 0) placeItems(in, L, std::max(1.0f, L.grid.w - in.scrollbar - 6));
  return L;
}

}  // namespace gl::launcher
