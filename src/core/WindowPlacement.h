// Gridline — J-113 / J-114 window placement on the monitors (detachable windows):
// keeping a window's title bar on a monitor, recovering windows saved on a
// monitor that is gone, re-anchoring saved positions to where the main window
// is now, picking the monitor a remembered position belongs to, where a panel
// popped out of the main window opens, when a detached window released after
// an OS move docks back, and the layout.ini lines of all that. Pure geometry
// (no UI), header only: used by the app (Viewports.cpp, DialogLayout,
// PluginWindow) and checked by tests/test_j113.cpp.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace gl::place {

struct Rect {
  float x = 0, y = 0, w = 0, h = 0;
  float right() const { return x + w; }
  float bottom() const { return y + h; }
  bool empty() const { return w <= 0 || h <= 0; }
};

inline float overlapArea(const Rect& a, const Rect& b) {
  const float w = std::min(a.right(), b.right()) - std::max(a.x, b.x);
  const float h = std::min(a.bottom(), b.bottom()) - std::max(a.y, b.y);
  return w > 0 && h > 0 ? w * h : 0.0f;
}

// Distance from point (px, py) to rectangle r (0 inside).
inline float distanceTo(const Rect& r, float px, float py) {
  const float dx = std::max({r.x - px, 0.0f, px - r.right()});
  const float dy = std::max({r.y - py, 0.0f, py - r.bottom()});
  return std::sqrt(dx * dx + dy * dy);
}

// The strip a window is grabbed by: its title bar (kTitleH tall, the whole width).
constexpr float kTitleH = 32.0f;
// How much of the title bar must stay on a monitor to count as reachable.
constexpr float kGrabW = 80.0f, kGrabH = 24.0f;

inline Rect titleStrip(const Rect& r) { return Rect{r.x, r.y, r.w, std::min(kTitleH, std::max(1.0f, r.h))}; }

// True when a usable part of the window's title bar (kGrabW x kGrabH, or all
// of it for narrower windows) is on one of the monitors.
inline bool titleReachable(const Rect& r, const std::vector<Rect>& monitors) {
  const Rect t = titleStrip(r);
  const float needW = std::min(kGrabW, t.w), needH = std::min(kGrabH, t.h);
  for (const Rect& m : monitors) {
    const float w = std::min(t.right(), m.right()) - std::max(t.x, m.x);
    const float h = std::min(t.bottom(), m.bottom()) - std::max(t.y, m.y);
    if (w >= needW - 0.5f && h >= needH - 0.5f) return true;
  }
  return false;
}

// Index of the monitor `r` belongs to: the one it overlaps most, else the
// nearest to its centre. -1 when there are no monitors.
inline int monitorFor(const Rect& r, const std::vector<Rect>& monitors) {
  int best = -1;
  float bestArea = 0;
  for (size_t i = 0; i < monitors.size(); ++i) {
    const float a = overlapArea(r, monitors[i]);
    if (a > bestArea) bestArea = a, best = static_cast<int>(i);
  }
  if (best >= 0) return best;
  float bestD = 0;
  const float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
  for (size_t i = 0; i < monitors.size(); ++i) {
    const float d = distanceTo(monitors[i], cx, cy);
    if (best < 0 || d < bestD) bestD = d, best = static_cast<int>(i);
  }
  return best;
}

// Index of the monitor that holds point (px, py), else the nearest one.
inline int monitorAt(float px, float py, const std::vector<Rect>& monitors) {
  int best = -1;
  float bestD = 0;
  for (size_t i = 0; i < monitors.size(); ++i) {
    const float d = distanceTo(monitors[i], px, py);
    if (best < 0 || d < bestD) bestD = d, best = static_cast<int>(i);
  }
  return best;
}

// `r` moved (and shrunk if needed) to lie fully inside `m`.
inline Rect fitInside(Rect r, const Rect& m) {
  r.w = std::min(r.w, m.w);
  r.h = std::min(r.h, m.h);
  r.x = std::clamp(r.x, m.x, m.right() - r.w);
  r.y = std::clamp(r.y, m.y, m.bottom() - r.h);
  return r;
}

// A saved window brought back on screen: unchanged while its title bar is
// reachable on some monitor; otherwise fitted into the monitor it belongs to
// (most overlap, else nearest). With no monitors it is returned as it is.
inline Rect recoverOnScreen(const Rect& r, const std::vector<Rect>& monitors) {
  if (monitors.empty() || titleReachable(r, monitors)) return r;
  return fitInside(r, monitors[static_cast<size_t>(monitorFor(r, monitors))]);
}

// While a window is dragged or rests outside the main window: its position
// kept so the title bar stays reachable on some monitor (the nearest one when
// it would leave them all). The size is never changed.
inline Rect keepTitleOnMonitors(Rect r, const std::vector<Rect>& monitors) {
  if (monitors.empty() || titleReachable(r, monitors)) return r;
  const Rect& m = monitors[static_cast<size_t>(monitorFor(titleStrip(r), monitors))];
  const float needW = std::min(kGrabW, r.w);
  r.x = std::clamp(r.x, m.x - r.w + needW, std::max(m.x - r.w + needW, m.right() - needW));
  r.y = std::clamp(r.y, m.y, std::max(m.y, m.bottom() - kGrabH));
  return r;
}

// Re-anchoring (layout.ini): a position saved in ImGui coordinates relative to
// the main window at `savedMain` (OS units) with interface scale `savedScale`,
// expressed relative to the main window now at `nowMain` with `nowScale`.
// The OS position stays the same.
inline void reanchor(float& x, float& y, float savedMainX, float savedMainY, float savedScale, float nowMainX, float nowMainY,
                     float nowScale) {
  const float s0 = savedScale > 0 ? savedScale : 1.0f, s1 = nowScale > 0 ? nowScale : 1.0f;
  const float osX = savedMainX + x * s0, osY = savedMainY + y * s0;
  x = (osX - nowMainX) / s1;
  y = (osY - nowMainY) / s1;
}

// layout.ini [GridlineViewports][Main]: the main window (OS units) and interface scale that the
// saved positions of detached windows are relative to. One line "MainOs=x,y" / "Scale=s" at a
// time; false for a line it does not know (ignored, so older / newer files still load).
// GitHub issue #50: also the main window itself, "MainSize=w,h" (its size when it was last
// neither maximised nor full screen, OS units), "MainMax=0|1" and "MainFull=0|1". A file without
// them (an older Gridline) opens the main window as before.
struct SavedMain {
  bool have = false;  // MainOs was read: positions can be re-anchored
  float x = 0, y = 0, scale = 1;
  bool haveSize = false;  // MainSize was read
  int w = 0, h = 0;
  bool maximized = false, fullscreen = false;
};
inline bool readSavedMainLine(const char* line, SavedMain& m) {
  float a = 0, b = 0;
  int iw = 0, ih = 0;
  char tail = 0;
  if (std::sscanf(line, "MainOs=%f,%f%c", &a, &b, &tail) == 2 && std::isfinite(a) && std::isfinite(b)) {
    m.x = a, m.y = b, m.have = true;
    return true;
  }
  if (std::sscanf(line, "Scale=%f%c", &a, &tail) == 1 && std::isfinite(a) && a >= 0.25f && a <= 8.0f) {
    m.scale = a;
    return true;
  }
  if (std::sscanf(line, "MainSize=%d,%d%c", &iw, &ih, &tail) == 2 && iw > 0 && ih > 0 && iw < 32768 && ih < 32768) {
    m.w = iw, m.h = ih, m.haveSize = true;
    return true;
  }
  if (std::sscanf(line, "MainMax=%d%c", &iw, &tail) == 1 && (iw == 0 || iw == 1)) {
    m.maximized = iw == 1;
    return true;
  }
  if (std::sscanf(line, "MainFull=%d%c", &iw, &tail) == 1 && (iw == 0 || iw == 1)) {
    m.fullscreen = iw == 1;
    return true;
  }
  return false;
}
// The body of the [Main] section (each line ending in a newline); MainOs only when `have`, the
// window lines only when `haveSize`.
inline std::string savedMainLines(const SavedMain& m) {
  char b[200];
  std::string s;
  if (m.have) {
    std::snprintf(b, sizeof b, "MainOs=%d,%d\nScale=%.4f\n", static_cast<int>(m.x), static_cast<int>(m.y), static_cast<double>(m.scale));
    s += b;
  }
  if (m.haveSize) {
    std::snprintf(b, sizeof b, "MainSize=%d,%d\nMainMax=%d\nMainFull=%d\n", m.w, m.h, m.maximized ? 1 : 0, m.fullscreen ? 1 : 0);
    s += b;
  }
  return s;
}
// The [GridlineViewports][Main] section of a whole layout.ini text, read before the ImGui context
// loads the file (the main window gets its size before the layout is read). False when the text
// has no such section.
inline bool readSavedMainSection(const std::string& ini, SavedMain& m) {
  bool in = false, found = false;
  size_t at = 0;
  while (at < ini.size()) {
    size_t end = ini.find('\n', at);
    if (end == std::string::npos) end = ini.size();
    std::string line = ini.substr(at, end - at);
    at = end + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
    if (!line.empty() && line[0] == '[') {
      in = line == "[GridlineViewports][Main]";
      found = found || in;
      continue;
    }
    if (in && !line.empty()) readSavedMainLine(line.c_str(), m);
  }
  return found;
}

// GitHub issue #50: where and how big the main window opens. `work`: the usable area of each
// display (OS units, the menu bar and panels left out); `current`: the display the window is on now
// (the splash's). Units are SDL's window units throughout, the same ones the size was saved in (points
// on macOS, pixels on Windows and X11, logical pixels on Wayland), so the interface scale does not
// enter: it sizes what is drawn inside the window, never the window.
//  * nothing saved (an older layout.ini, a first start): `def` x `defH`, at most the display less
//    40 x 60, centred on the current display (as before);
//  * a saved size: on the display the saved place belongs to (most overlap, else the nearest: a
//    display that has gone hands over to the nearest one left), shrunk to fit its usable area with
//    `osTitle` above it for the desktop's title bar, never below the minimum size where the display
//    is large enough, at the saved place moved inside that area, or centred when no place was saved;
//  * maximised or full screen when it was left so. Full screen comes back maximised (the size the
//    window fills is the display's either way, and a window that comes back maximised keeps the
//    desktop's title bar and the way back to its old size).
struct MainWindowPlan {
  int x = 0, y = 0, w = 0, h = 0;
  bool maximize = false;
  bool restored = false;  // a saved size was used
  int display = -1;       // index into `work`
};
inline MainWindowPlan planMainWindow(const SavedMain& s, const std::vector<Rect>& work, int current, int minW, int minH, int defW = 1600,
                                     int defH = 960, float osTitle = 32.0f) {
  MainWindowPlan p;
  p.maximize = s.maximized || s.fullscreen;
  if (work.empty()) {  // no display information: the default size, where the system puts it
    p.w = s.haveSize ? std::max(s.w, minW) : defW;
    p.h = s.haveSize ? std::max(s.h, minH) : defH;
    p.restored = s.haveSize;
    return p;
  }
  current = std::clamp(current, 0, static_cast<int>(work.size()) - 1);
  if (!s.haveSize) {
    const Rect& m = work[static_cast<size_t>(current)];
    p.w = std::min(defW, static_cast<int>(m.w) - 40);
    p.h = std::min(defH, static_cast<int>(m.h) - 60);
    p.x = static_cast<int>(m.x) + (static_cast<int>(m.w) - p.w) / 2;
    p.y = static_cast<int>(m.y) + (static_cast<int>(m.h) - p.h) / 2;
    p.display = current;
    return p;
  }
  int mi = current;
  if (s.have) mi = std::max(0, monitorFor(Rect{s.x, s.y, static_cast<float>(s.w), static_cast<float>(s.h)}, work));
  const Rect& m = work[static_cast<size_t>(mi)];
  const float top = std::min(osTitle, std::max(0.0f, m.h - 1.0f));
  const Rect area{m.x, m.y + top, m.w, std::max(1.0f, m.h - top)};
  const float w = std::clamp(static_cast<float>(s.w), std::min(static_cast<float>(minW), area.w), area.w);
  const float h = std::clamp(static_cast<float>(s.h), std::min(static_cast<float>(minH), area.h), area.h);
  Rect r = s.have ? Rect{s.x, s.y, w, h} : Rect{area.x + (area.w - w) * 0.5f, area.y + (area.h - h) * 0.5f, w, h};
  r = fitInside(r, area);
  p.x = static_cast<int>(std::lround(r.x)), p.y = static_cast<int>(std::lround(r.y));
  p.w = static_cast<int>(std::lround(r.w)), p.h = static_cast<int>(std::lround(r.h));
  p.restored = true;
  p.display = mi;
  return p;
}

// J-114: a window placed outside the main window (dropped there, popped out) keeps room for
// its OS title bar above it: its top at least `osTitle` below the top of the monitor it is on.
inline Rect keepOsTitleOnMonitor(Rect r, const std::vector<Rect>& monitors, float osTitle) {
  if (monitors.empty()) return r;
  const Rect& m = monitors[static_cast<size_t>(monitorFor(titleStrip(r), monitors))];
  if (r.y < m.y + osTitle) r.y = m.y + osTitle;
  return r;
}

// J-114: where "Pop out into its own window" opens a panel of w x h that has no remembered
// place outside: beside the main window (`main`), at height `y`, `gap` away from it, on its
// right and else on its left, when it fits fully on a monitor there (`osTitle` above it for
// the OS title bar); else centred on another monitor; else on the main window's monitor,
// offset from its top-left corner (the main window fills that monitor).
inline Rect popOutPlace(float w, float h, float y, const Rect& main, const std::vector<Rect>& monitors, float gap = 24.0f,
                        float osTitle = 32.0f) {
  const Rect right{main.right() + gap, y, w, h}, left{main.x - gap - w, y, w, h};
  if (monitors.empty()) return right;
  auto fits = [&](const Rect& r) {
    for (const Rect& m : monitors)
      if (r.x >= m.x && r.y >= m.y + osTitle && r.right() <= m.right() && r.bottom() <= m.bottom()) return true;
    return false;
  };
  for (const Rect& c : {right, left}) {
    if (fits(c)) return c;
    Rect up = c;  // the same side, moved up or down to fit the monitor's height
    const int mi = monitorAt(c.x + c.w * 0.5f, c.y, monitors);
    if (mi >= 0) {
      const Rect& m = monitors[static_cast<size_t>(mi)];
      up.y = std::clamp(c.y, m.y + osTitle, std::max(m.y + osTitle, m.bottom() - h));
      if (fits(up)) return up;
    }
  }
  const int home = monitorFor(main, monitors);
  for (size_t i = 0; i < monitors.size(); ++i) {
    if (static_cast<int>(i) == home) continue;
    const Rect& m = monitors[i];
    Rect c{m.x + (m.w - w) * 0.5f, m.y + (m.h - h) * 0.5f, w, h};
    return fitInside(c, Rect{m.x, m.y + osTitle, m.w, std::max(1.0f, m.h - osTitle)});
  }
  const Rect& m = monitors[static_cast<size_t>(home < 0 ? 0 : home)];
  return fitInside(Rect{main.x + 48.0f, main.y + 48.0f + osTitle, w, h}, Rect{m.x, m.y + osTitle, m.w, std::max(1.0f, m.h - osTitle)});
}

// J-114: ImGui's split ratio is the share of a node's first child. A panel docked back on side
// `dir` (0 left, 1 right, 2 up, 3 down: ImGuiDir) taking `share` of the node it splits.
inline float splitRatioFor(int dir, float share) {
  share = std::clamp(share, 0.05f, 0.95f);
  return dir == 1 || dir == 3 ? 1.0f - share : share;
}

// J-114 layout.ini [GridlineViewports][Home:<window>] (where a detached window docks back) and
// [GridlineViewports][Out:<window>] (its last place outside, OS units). Lines it does not know
// return false (ignored, so older / newer files still load).
struct Home {
  bool have = false, docked = false;
  unsigned node = 0, parent = 0;  // the dock node it was in; the split node holding that one
  int dir = -1;                   // its side of the split (ImGuiDir), -1 none
  float share = 0.5f;             // its share of the split
  Rect floating;                  // undocked inside the main window: where (ImGui units)
};
inline bool readHomeLine(const char* line, Home& h) {
  unsigned n = 0, p = 0;
  int d = -1;
  float s = 0, x = 0, y = 0, w = 0, hh = 0;
  char tail = 0;
  if (std::sscanf(line, "Dock=0x%x,0x%x,%d,%f%c", &n, &p, &d, &s, &tail) == 4 && d >= -1 && d <= 3 && std::isfinite(s)) {
    h.have = h.docked = true, h.node = n, h.parent = p, h.dir = d, h.share = std::clamp(s, 0.05f, 0.95f);
    return true;
  }
  if (std::sscanf(line, "Float=%f,%f,%f,%f%c", &x, &y, &w, &hh, &tail) == 4 && std::isfinite(x) && std::isfinite(y) && w > 0 && hh > 0) {
    h.have = true, h.docked = false, h.floating = Rect{x, y, w, hh};
    return true;
  }
  return false;
}
inline std::string homeLines(const Home& h) {
  char b[160];
  if (h.docked)
    std::snprintf(b, sizeof b, "Dock=0x%08X,0x%08X,%d,%.4f\n", h.node, h.parent, h.dir, static_cast<double>(h.share));
  else
    std::snprintf(b, sizeof b, "Float=%.0f,%.0f,%.0f,%.0f\n", static_cast<double>(h.floating.x), static_cast<double>(h.floating.y),
                  static_cast<double>(h.floating.w), static_cast<double>(h.floating.h));
  return b;
}
inline bool readOutLine(const char* line, Rect& r) {
  float x = 0, y = 0, w = 0, h = 0;
  char tail = 0;
  if (std::sscanf(line, "Rect=%f,%f,%f,%f%c", &x, &y, &w, &h, &tail) == 4 && std::isfinite(x) && std::isfinite(y) && w > 0 && h > 0 && w < 32768 &&
      h < 32768) {
    r = Rect{x, y, w, h};
    return true;
  }
  return false;
}

// Cascade slot `i` for windows brought back into the main window's work area
// `area`: a window of size w x h, fitted inside.
inline Rect cascade(int i, float w, float h, const Rect& area) {
  const float step = 28.0f * static_cast<float>(i % 10);
  return fitInside(Rect{area.x + 40.0f + step, area.y + 24.0f + step, w, h}, area);
}

// J-158: a plugin editor's window in the main window's work area (areaW x areaH). The editor
// wants `content` px of width; one that reflows goes down to `minContent` px (0: a fixed
// layout); the window adds `chrome` px around it (padding, the vertical scrollbar). Where there
// is room the window has its natural width. Otherwise a reflowing editor narrows first, and what
// still does not fit is capped to the area: the content scrolls sideways (scrollX). The window is
// never taller than the area (the content scrolls down).
struct EditorFit {
  float content = 0;  // width the editor lays itself out at
  float window = 0;   // the window's width
  float maxH = 0;     // the window's height at most
  bool scrollX = false;
};
inline EditorFit fitEditor(float content, float minContent, float chrome, float areaW, float areaH) {
  EditorFit f;
  areaW = std::max(1.0f, areaW);
  f.content = std::max(1.0f, content);
  if (minContent > 0 && f.content + chrome > areaW) f.content = std::max(std::min(minContent, f.content), std::floor(areaW - chrome));
  const float natural = f.content + chrome;
  f.window = std::min(natural, areaW);
  f.scrollX = natural > areaW + 0.5f;
  f.maxH = std::max(1.0f, areaH);
  return f;
}

}  // namespace gl::place
