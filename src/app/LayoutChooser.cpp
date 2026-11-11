// Gridline - the window layout chooser (K-130): the miniature of each layout, its line glyph,
// the card used on the welcome screen and in Settings > Interface, and the two options that
// go with a choice. See LayoutPresets.h.
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/I18n.h"
#include "../core/Settings.h"
#include "../core/Workspace.h"
#include "App.h"
#include "LayoutPresets.h"
#include "SmoothScrollUi.h"
#include "Theme.h"
#include "Widgets.h"

namespace gl::layoutui {

using layout::Preset;
using theme::pal;

namespace {

// ------------------------------------------------------------------ geometry of a layout
// The window the miniature stands for (logical px), and its bars.
constexpr float kVW = 1440, kVH = 900, kStatusH = 26;
float topBarH(Preset p) { return p == Preset::Logic ? 70.0f : 48.0f; }

enum class Pane { Browser, Playlist, Mixer, Rack, Piano, History };
struct Region {
  workspace::Rect r;  // in the dock area
  Pane kind;
  bool floating = false;
};

// The regions of a preset, straight from the layout definitions the dock builder uses.
int regions(Preset p, Region* out) {
  const float w = kVW, h = kVH - topBarH(p) - kStatusH;
  switch (p) {
    case Preset::Live: {
      const layout::LiveLayout L = layout::liveLayout(w, h);
      out[0] = {L.browser, Pane::Browser};
      out[1] = {L.main, Pane::Playlist};
      out[2] = {L.mixer, Pane::Mixer};
      out[3] = {L.bottom, Pane::Piano};
      return 4;
    }
    case Preset::Logic: {
      const layout::LogicLayout L = layout::logicLayout(w, h);
      out[0] = {L.browser, Pane::Browser};
      out[1] = {L.main, Pane::Playlist};
      out[2] = {L.bottom, Pane::Mixer};
      out[3] = {L.right, Pane::History};
      return 4;
    }
    default: {
      const workspace::Layout L = workspace::defaultLayout(w, h, 4);
      out[0] = {L.browser, Pane::Browser};
      out[1] = {L.playlist, Pane::Playlist};
      out[2] = {L.mixer, Pane::Mixer};
      out[3] = {L.rack, Pane::Rack, true};
      return 4;
    }
  }
}

struct Box {
  float x0, y0, x1, y1;
  float w() const { return x1 - x0; }
  float h() const { return y1 - y0; }
  Box inset(float l, float t, float r, float b) const { return {x0 + l, y0 + t, x1 - r, y1 - b}; }
  Box inset(float d) const { return inset(d, d, d, d); }
};
ImVec2 v(float x, float y) { return ImVec2(x, y); }
void fill(ImDrawList* dl, const Box& b, ImU32 c, float r = 0, ImDrawFlags f = 0) {
  if (b.w() > 0 && b.h() > 0) dl->AddRectFilled(v(b.x0, b.y0), v(b.x1, b.y1), c, r, f);
}

// Clip colours of the miniature's arrangement (content colours: they follow the theme like real clips).
constexpr uint32_t kHues[] = {0x5C8DF0, 0x4FBF8B, 0xE9A23B, 0xD8647A, 0x9C7BE2, 0x4CB8C4};

// ------------------------------------------------------------------ the miniature
struct Mini {
  ImDrawList* dl;
  const theme::Palette& P;
  float sx, sy;  // virtual px -> screen px
  float px(float vpx) const { return vpx * sx; }
  float py(float vpx) const { return vpx * sy; }

  // A text line stand-in (a short rounded bar).
  void textBar(float x, float yc, float w, float h, ImU32 c) const {
    if (w < 1 || h < 0.8f) return;
    fill(dl, {x, std::round(yc - h * 0.5f), x + w, std::round(yc - h * 0.5f) + std::max(1.0f, std::round(h))}, c, h * 0.5f);
  }

  void topBar(const Box& b, Preset p) const {
    fill(dl, b, P.chrome);
    dl->AddLine(v(b.x0, b.y1 - 0.5f), v(b.x1, b.y1 - 0.5f), P.line);
    const float h = b.h();
    const ImU32 dim = theme::withAlpha(P.textDim, 0.55f);
    Box ctl = b;
    if (p == Preset::Logic) {  // two rows: menus, then the controls
      const float mh = std::round(h * 26.0f / 70.0f);
      const Box menu{b.x0, b.y0, b.x1, b.y0 + mh};
      float x = menu.x0 + px(52);
      const float widths[] = {26, 22, 30, 36, 28, 24, 30};
      for (float w : widths) {
        textBar(x, (menu.y0 + menu.y1) * 0.5f, px(w), std::max(1.0f, py(6)), dim);
        x += px(w + 18);
      }
      dl->AddLine(v(menu.x0 + px(8), menu.y1 - 0.5f), v(menu.x1 - px(8), menu.y1 - 0.5f), theme::withAlpha(P.line, 0.6f));
      ctl = {b.x0, menu.y1, b.x1, b.y1};
    }
    const float ch = ctl.h(), cy = (ctl.y0 + ctl.y1) * 0.5f;
    // The mark at the left.
    const float ms = std::round(std::clamp(ch * 0.5f, 3.0f, 22.0f));
    if (p != Preset::Logic) {
      if (ms >= 8) theme::drawMark(dl, v(std::round(ctl.x0 + px(14)), std::round(cy - ms * 0.5f)), ms, P.logo, P.chrome);
      else fill(dl, {ctl.x0 + px(14), cy - ms * 0.5f, ctl.x0 + px(14) + ms, cy + ms * 0.5f}, theme::withAlpha(P.logo, 0.8f), 1);
    }
    // Transport, LCD and tempo in the middle (left of centre on the studio bar, as the real one).
    const float bw = std::max(2.0f, std::round(ch * 0.42f));
    float x = std::round(p == Preset::Logic ? ctl.x0 + ctl.w() * 0.36f : ctl.x0 + ctl.w() * 0.27f);
    const ImU32 btn = theme::mix(P.surface800, P.surface700, 0.5f);
    for (int i = 0; i < 3; ++i) {
      const Box k{x, std::round(cy - bw * 0.5f), x + bw, std::round(cy - bw * 0.5f) + bw};
      fill(dl, k, btn, std::max(1.0f, bw * 0.2f));
      const float s = bw * 0.26f;
      const ImVec2 c((k.x0 + k.x1) * 0.5f, (k.y0 + k.y1) * 0.5f);
      if (bw >= 5) {
        if (i == 0) dl->AddTriangleFilled(v(c.x - s * 0.8f, c.y - s), v(c.x - s * 0.8f, c.y + s), v(c.x + s, c.y), theme::lit(P.hot));
        if (i == 1) fill(dl, {c.x - s * 0.8f, c.y - s * 0.8f, c.x + s * 0.8f, c.y + s * 0.8f}, P.textDim);
        if (i == 2) dl->AddCircleFilled(c, s, P.danger);
      }
      x += bw + std::max(1.0f, px(6));
    }
    x += px(10);
    const float lw = std::round(p == Preset::Logic ? ctl.w() * 0.16f : ctl.w() * 0.12f);
    const Box lcd{x, std::round(cy - bw * 0.55f), x + lw, std::round(cy - bw * 0.55f) + std::round(bw * 1.1f)};
    fill(dl, lcd, P.field, std::max(1.0f, bw * 0.15f));
    textBar(lcd.x0 + lcd.w() * 0.12f, cy, lcd.w() * 0.5f, std::max(1.0f, bw * 0.3f), theme::withAlpha(theme::lit(P.hot), 0.85f));
    x = lcd.x1 + px(10);
    const Box tempo{x, lcd.y0, x + std::round(lw * 0.42f), lcd.y1};
    fill(dl, tempo, P.field, std::max(1.0f, bw * 0.15f));
    textBar(tempo.x0 + tempo.w() * 0.2f, cy, tempo.w() * 0.5f, std::max(1.0f, bw * 0.3f), theme::withAlpha(P.text, 0.6f));
    // The output meter and a few tool buttons at the right.
    const float mw = std::round(ctl.w() * 0.07f);
    const Box meter{ctl.x1 - px(16) - mw, std::round(cy - bw * 0.25f), ctl.x1 - px(16), std::round(cy - bw * 0.25f) + std::max(2.0f, std::round(bw * 0.5f))};
    fill(dl, meter, P.field, 1);
    fill(dl, {meter.x0, meter.y0, meter.x0 + meter.w() * 0.62f, meter.y1}, theme::withAlpha(P.meterLow, 0.85f), 1);
    float tx = meter.x0 - px(14);
    for (int i = 0; i < 4; ++i) {
      const float s = std::max(2.0f, std::round(bw * 0.7f));
      tx -= s;
      fill(dl, {tx, std::round(cy - s * 0.5f), tx + s, std::round(cy - s * 0.5f) + s}, i == 2 ? theme::withAlpha(theme::lit(P.hot), 0.7f) : btn, 1);
      tx -= std::max(1.0f, px(5));
    }
  }

  void statusBar(const Box& b) const {
    fill(dl, b, P.chrome);
    dl->AddLine(v(b.x0, b.y0 + 0.5f), v(b.x1, b.y0 + 0.5f), P.line);
    textBar(b.x0 + px(12), (b.y0 + b.y1) * 0.5f, px(220), std::max(1.0f, py(6)), theme::withAlpha(P.textFaint, 0.6f));
    textBar(b.x1 - px(12) - px(90), (b.y0 + b.y1) * 0.5f, px(90), std::max(1.0f, py(6)), theme::withAlpha(P.textFaint, 0.5f));
  }

  void browser(const Box& q) const {
    const float rh = std::max(3.0f, py(22));
    const Box search{q.x0 + px(8), q.y0 + py(6), q.x1 - px(8), q.y0 + py(6) + std::max(2.0f, py(20))};
    fill(dl, search, P.field, std::max(1.0f, py(4)));
    const float lens[] = {0.55f, 0.42f, 0.5f, 0.36f, 0.6f, 0.3f, 0.46f, 0.52f, 0.34f, 0.44f, 0.58f, 0.38f, 0.48f, 0.4f,
                          0.54f, 0.32f, 0.5f, 0.45f, 0.36f, 0.56f, 0.42f, 0.3f, 0.5f, 0.4f, 0.46f, 0.52f, 0.34f, 0.44f};
    const int indent[] = {0, 0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 1, 1, 0, 0, 0};
    float y = search.y1 + py(8);
    const float ih = std::max(1.0f, std::round(rh * 0.3f));
    for (int i = 0; y + rh <= q.y1 && i < 28; ++i, y += rh) {
      const float x = q.x0 + px(10) + px(14) * static_cast<float>(indent[i]);
      const float s = std::max(1.5f, std::round(rh * 0.42f));
      const float yc = y + rh * 0.5f;
      const bool folder = indent[i] == 0;
      fill(dl, {x, std::round(yc - s * 0.5f), x + s, std::round(yc - s * 0.5f) + s}, folder ? theme::withAlpha(theme::lit(P.hot), 0.55f) : theme::withAlpha(P.textFaint, 0.6f),
           std::max(0.5f, s * 0.2f));
      if (i == 3) fill(dl, {q.x0 + 1, y, q.x1 - 1, y + rh}, P.selection);
      textBar(x + s + std::max(2.0f, px(8)), yc, (q.w() - (x - q.x0) - s - px(16)) * lens[i], ih, theme::withAlpha(P.textDim, folder ? 0.7f : 0.5f));
    }
  }

  void playlist(const Box& q0) const {
    Box q = q0;
    // The pattern list at the left (when the view is wide enough for it).
    if (q.w() > px(700)) {
      const Box list{q.x0, q.y0, q.x0 + std::round(px(128)), q.y1};
      fill(dl, list, theme::mix(P.surface900, P.surface800, 0.4f));
      const float rh = std::max(3.0f, std::round(py(24)));
      for (int i = 0; i < 4; ++i) {
        const float y = list.y0 + py(42) + rh * static_cast<float>(i);
        if (y + rh > list.y1) break;
        const Box row{list.x0 + px(6), y + 1, list.x1 - px(6), y + rh - 1};
        fill(dl, row, i == 0 ? theme::mix(P.surface800, P.surface700, 0.6f) : P.surface800, 1);
        fill(dl, {row.x0, row.y0, row.x0 + std::max(1.0f, px(4)), row.y1}, theme::content(kHues[(i + 2) % 6], 0.9f));
        textBar(row.x0 + px(14), (row.y0 + row.y1) * 0.5f, row.w() * 0.4f, std::max(1.0f, std::round(rh * 0.26f)), theme::withAlpha(P.textDim, 0.65f));
      }
      q.x0 = list.x1 + std::max(1.0f, px(4));
    }
    const float ruler = std::max(2.0f, std::round(py(40)));
    const float hw = std::round(std::clamp(q.w() * 0.15f, 6.0f, px(96)));
    const Box head{q.x0, q.y0 + ruler, q.x0 + hw, q.y1};
    const Box well{q.x0 + hw, q.y0 + ruler, q.x1, q.y1};
    fill(dl, {q.x0, q.y0, q.x1, q.y0 + ruler}, theme::mix(P.surface900, P.surface800, 0.6f));
    fill(dl, well, P.well);
    // Bars and beats.
    const int bars = 16;
    for (int i = 1; i < bars; ++i) {
      const float x = std::round(well.x0 + well.w() * static_cast<float>(i) / bars) + 0.5f;
      dl->AddLine(v(x, well.y0), v(x, well.y1), (i % 4) ? P.gridBeat : P.gridBar);
      dl->AddLine(v(x, q.y0 + ruler * ((i % 4) ? 0.75f : 0.5f)), v(x, q.y0 + ruler), theme::withAlpha(P.textFaint, 0.6f));
    }
    // Track rows with clips (start, end in bars): drums all through, the bass and chords coming in.
    struct Clip {
      int row;
      float a, b;
      int hue;
    };
    static const Clip clips[] = {{0, 0, 2, 1},   {0, 2, 4, 1},   {0, 4, 6, 1},   {0, 6, 8, 2},   {0, 8, 10, 1},  {0, 10, 12, 1}, {0, 12, 14, 1},
                                 {0, 14, 16, 2}, {1, 4, 8, 4},   {1, 8, 12, 4},  {1, 12, 16, 4}, {2, 8, 12, 5},  {2, 12, 16, 5}, {3, 6, 8, 3},
                                 {3, 14, 16, 3}, {4, 0, 4, 0},   {4, 8, 12, 0},  {5, 12, 16, 2}, {6, 2, 6, 3},   {7, 10, 14, 5}};
    const float rh = std::max(4.0f, std::round(py(46)));
    const int rows = std::max(1, static_cast<int>(std::ceil(well.h() / rh)));
    const float gy = std::max(0.5f, std::round(rh * 0.04f));
    for (const Clip& c : clips) {
      if (c.row >= rows) continue;
      const float y0 = std::round(well.y0 + rh * static_cast<float>(c.row) + gy), y1 = std::round(well.y0 + rh * static_cast<float>(c.row + 1) - gy);
      const float x0 = std::round(well.x0 + well.w() * c.a / bars) + 1, x1 = std::round(well.x0 + well.w() * c.b / bars) - 1;
      const float band = std::max(1.0f, std::round((y1 - y0) * 0.3f));
      fill(dl, {x0, y0, x1, y1}, theme::content(kHues[c.hue], 0.5f), 1);
      fill(dl, {x0, y0, x1, y0 + band}, theme::content(kHues[c.hue], 0.95f), 1, ImDrawFlags_RoundCornersTop);
      if (y1 - y0 - band >= 4 && x1 - x0 > 6) {  // a hint of the notes inside
        const ImU32 ink = theme::withAlpha(theme::content(kHues[c.hue]), 1.0f);
        const float bh = y1 - y0 - band;
        const int n = std::max(2, static_cast<int>((x1 - x0) / 5.0f));
        for (int k = 0; k < n; ++k) {
          const float nx = x0 + 2 + (x1 - x0 - 4) * static_cast<float>(k) / static_cast<float>(n);
          const float ny = y0 + band + bh * (0.25f + 0.5f * static_cast<float>((k * 7 + c.row * 3) % 5) / 4.0f);
          fill(dl, {std::round(nx), std::round(ny), std::round(nx) + std::max(1.0f, (x1 - x0) / static_cast<float>(n) * 0.5f), std::round(ny) + 1}, ink);
        }
      }
    }
    for (int r = 0; r < rows; ++r) {  // track headers: a name plate with a colour edge
      const Box hb{head.x0, std::round(head.y0 + rh * static_cast<float>(r)) + gy, head.x1 - 1, std::round(head.y0 + rh * static_cast<float>(r + 1)) - gy};
      fill(dl, hb, r < 3 ? theme::mix(P.surface800, theme::content(kHues[(r == 0 ? 1 : r == 1 ? 4 : 5)]), 0.25f) : P.surface800, 1);
      fill(dl, {hb.x0, hb.y0, hb.x0 + std::max(1.0f, px(3)), hb.y1}, r < 3 ? theme::content(kHues[(r == 0 ? 1 : r == 1 ? 4 : 5)]) : P.lineStrong);
      textBar(hb.x0 + std::max(3.0f, px(10)), hb.y0 + std::max(2.0f, rh * 0.28f), hb.w() * 0.42f, std::max(1.0f, std::round(rh * 0.14f)),
              theme::withAlpha(P.text, 0.55f));
    }
    // The song position.
    const float ph = std::round(well.x0 + well.w() * 0.37f) + 0.5f;
    dl->AddLine(v(ph, q.y0 + ruler * 0.4f), v(ph, well.y1), P.playhead, std::max(1.0f, px(1.5f)));
  }

  // Mixer strips (a knob pair, the fader with its meter, the name plate), then the insert's effect slots:
  // beside the strips in a wide pane, under them in a tall one.
  void mixer(const Box& q) const {
    const bool wide = q.w() > q.h() * 1.3f;
    Box strips = q, slots = q;
    if (wide) {
      strips.x1 = std::round(q.x0 + q.w() * 0.7f);
      slots.x0 = strips.x1 + std::max(1.0f, px(10));
    } else {
      strips.y1 = std::round(q.y0 + q.h() * 0.6f);
      slots.y0 = strips.y1 + std::max(1.0f, py(10));
    }
    strips = strips.inset(std::max(1.0f, px(6)), std::max(1.0f, py(6)), std::max(1.0f, px(6)), std::max(1.0f, py(6)));
    const float target = std::max(5.0f, px(68));
    const int n = std::clamp(static_cast<int>(strips.w() / target), 2, 40);
    const float sw = strips.w() / static_cast<float>(n);
    const float levels[] = {0.78f, 0.62f, 0.7f, 0.44f, 0.55f, 0.36f, 0.66f, 0.5f, 0.3f, 0.58f, 0.4f, 0.68f, 0.46f, 0.34f, 0.6f, 0.52f};
    const float faders[] = {0.72f, 0.6f, 0.66f, 0.55f, 0.7f, 0.5f, 0.62f, 0.58f, 0.64f, 0.52f, 0.68f, 0.56f, 0.6f, 0.66f, 0.54f, 0.62f};
    static const int hues[] = {-1, 1, 2, 5, 0, 4, 3, 1, 2, 5, 0, 4, 3, 1, 2, 5};
    for (int i = 0; i < n; ++i) {
      const Box s{std::round(strips.x0 + sw * static_cast<float>(i)) + 1, strips.y0, std::round(strips.x0 + sw * static_cast<float>(i + 1)) - 1, strips.y1};
      fill(dl, s, i == 0 ? theme::mix(P.surface800, P.hot, 0.08f) : P.surface800, std::min(2.0f, sw * 0.1f));
      if (i == 0) dl->AddRect(v(s.x0, s.y0), v(s.x1, s.y1), theme::withAlpha(theme::lit(P.hot), 0.45f), std::min(2.0f, sw * 0.1f));
      if (s.w() < 2) continue;
      // The colour cap with the name.
      const float cap = std::max(1.0f, std::round(s.h() * 0.1f));
      const ImU32 hue = hues[i % 16] < 0 ? theme::withAlpha(theme::lit(P.hot), 0.5f) : theme::content(kHues[hues[i % 16]], 0.45f);
      fill(dl, {s.x0, s.y0, s.x1, s.y0 + cap}, hue, std::min(2.0f, sw * 0.1f), ImDrawFlags_RoundCornersTop);
      textBar(s.x0 + s.w() * 0.12f, s.y0 + cap * 0.62f, s.w() * 0.5f, std::max(1.0f, cap * 0.22f), theme::withAlpha(P.text, 0.7f));
      // Two knobs.
      const float kr = std::min(s.w() * 0.12f, s.h() * 0.045f);
      const float ky = s.y0 + cap + std::max(2.0f, s.h() * 0.1f);
      if (kr >= 1.0f)
        for (int k = 0; k < 2; ++k) dl->AddCircleFilled(v(s.x0 + s.w() * (k ? 0.64f : 0.36f), ky), kr, P.knobFill, 12);
      const Box lane{s.x0 + s.w() * 0.18f, ky + kr + std::max(2.0f, s.h() * 0.06f), s.x1 - s.w() * 0.18f, s.y1 - std::max(2.0f, s.h() * 0.1f)};
      if (lane.h() < 3) continue;
      fill(dl, lane, theme::mix(P.field, P.surface900, 0.3f), 1);
      // The meter pair and the fader cap over the slot.
      const float mw = std::max(1.0f, std::round(lane.w() * 0.12f));
      const float lv = levels[(i * 5 + 3) % 16];
      for (int m = 0; m < 2; ++m) {
        const float mx = std::round(lane.x0 + lane.w() * (m ? 0.76f : 0.62f));
        fill(dl, {mx, lane.y1 - lane.h() * lv * (m ? 0.92f : 1.0f), mx + mw, lane.y1 - 1}, theme::withAlpha(P.meterLow, 0.9f));
      }
      const float fx = std::round(lane.x0 + lane.w() * 0.32f);
      fill(dl, {fx - 0.5f, lane.y0 + 2, fx + 0.5f, lane.y1 - 2}, theme::withAlpha(P.ink, 0.6f));
      const float cy = std::round(lane.y1 - lane.h() * faders[i % 16]);
      const float cw = std::max(2.0f, std::round(lane.w() * 0.36f)), chh = std::max(1.0f, std::round(lane.h() * 0.05f));
      fill(dl, {fx - cw * 0.5f, cy - chh, fx + cw * 0.5f, cy + chh}, theme::mix(P.textDim, P.text, 0.4f), 1);
    }
    // The effect slots of the selected insert: a heading, then a few filled and empty slots.
    slots = slots.inset(std::max(1.0f, px(6)), std::max(1.0f, py(6)), std::max(1.0f, px(6)), std::max(1.0f, py(6)));
    if (slots.w() < 8 || slots.h() < 8) return;
    textBar(slots.x0, slots.y0 + py(10), std::min(slots.w() * 0.4f, px(70)), std::max(1.0f, py(9)), theme::withAlpha(P.text, 0.7f));
    const float rh = std::max(3.0f, std::round(py(30)));
    float y = slots.y0 + py(36);
    for (int i = 0; y + rh <= slots.y1 && i < 10; ++i, y += rh) {
      const float yc = y + rh * 0.5f;
      dl->AddCircleFilled(v(slots.x0 + px(6), yc), std::max(1.0f, rh * 0.14f), i < 2 ? P.led : theme::withAlpha(P.textFaint, 0.4f), 10);
      const Box f{slots.x0 + px(16), y + std::max(0.5f, rh * 0.12f), slots.x1, y + rh - std::max(0.5f, rh * 0.12f)};
      fill(dl, f, P.field, 1);
      textBar(f.x0 + f.w() * 0.06f, yc, f.w() * (i < 2 ? 0.42f : 0.3f), std::max(1.0f, std::round(rh * 0.2f)),
              theme::withAlpha(i < 2 ? P.text : P.textFaint, i < 2 ? 0.6f : 0.45f));
    }
  }

  void rack(const Box& q) const {
    static const unsigned pattern[] = {0x8888u, 0x0808u, 0xAAAAu, 0x2024u};
    const float rh = std::max(4.0f, std::round(py(35)));
    const int rows = std::min(4, std::max(1, static_cast<int>((q.h() - py(8)) / rh)));
    const float y0 = q.y0 + py(6);
    const float nameW = std::round(q.w() * 0.3f);
    const int steps = (q.w() - nameW) / 16.0f >= 3.0f ? 16 : 8;
    const float sx0 = q.x0 + nameW + px(14), sw = (q.x1 - px(10) - sx0) / static_cast<float>(steps);
    for (int r = 0; r < rows; ++r) {
      const float y = std::round(y0 + rh * static_cast<float>(r));
      const float yc = y + rh * 0.5f;
      const float pad = std::max(0.5f, std::round(rh * 0.16f));
      dl->AddCircleFilled(v(q.x0 + px(14), yc), std::max(1.0f, rh * 0.13f), P.led);
      const Box name{q.x0 + px(26), y + pad, q.x0 + nameW, y + rh - pad};
      fill(dl, name, P.field, std::max(1.0f, rh * 0.12f));
      textBar(name.x0 + name.w() * 0.12f, yc, name.w() * 0.55f, std::max(1.0f, rh * 0.18f), theme::withAlpha(P.text, 0.55f));
      for (int s = 0; s < steps; ++s) {
        const int bit = steps == 16 ? s : s * 2;
        const bool on = (pattern[r] >> (15 - bit)) & 1u;
        const bool alt = (bit / 4) % 2 == 1;
        const Box b{std::round(sx0 + sw * static_cast<float>(s)) + 0.5f, y + pad, std::round(sx0 + sw * static_cast<float>(s + 1)) - 0.5f, y + rh - pad};
        fill(dl, b, on ? P.stepOn : alt ? P.stepOffAlt : P.stepOff, std::min(1.5f, b.w() * 0.2f));
      }
    }
  }

  void piano(const Box& q) const {
    const float kw = std::round(std::clamp(q.w() * 0.06f, 5.0f, px(70)));
    const Box keys{q.x0, q.y0, q.x0 + kw, q.y1};
    const Box well{q.x0 + kw, q.y0, q.x1, q.y1};
    fill(dl, keys, P.keyWhite);
    fill(dl, well, P.well);
    const float lane = std::max(2.0f, std::round(py(13)));
    const int lanes = static_cast<int>(q.h() / lane);
    static const bool black[12] = {false, true, false, true, false, false, true, false, true, false, true, false};
    for (int i = 0; i < lanes; ++i) {
      const float y = q.y1 - lane * static_cast<float>(i + 1);
      if (black[i % 12]) {
        fill(dl, {keys.x0, y, keys.x0 + kw * 0.62f, y + lane}, P.keyBlack);
        fill(dl, {well.x0, y, well.x1, y + lane}, theme::withAlpha(P.ink, P.dark ? 0.18f : 0.05f));
      }
      if (i % 12 == 0) dl->AddLine(v(keys.x0, y + lane - 0.5f), v(well.x1, y + lane - 0.5f), P.gridBar);
    }
    const int beats = 16;
    for (int i = 1; i < beats; ++i) {
      const float x = std::round(well.x0 + well.w() * static_cast<float>(i) / beats) + 0.5f;
      dl->AddLine(v(x, well.y0), v(x, well.y1), (i % 4) ? P.gridBeat : P.gridBar);
    }
    struct N {
      int lane;
      float a, b;
    };
    static const N notes[] = {{7, 0, 1.5f},  {11, 0, 1.5f}, {14, 0, 1.5f}, {9, 2, 3.5f},  {12, 2, 3.5f}, {16, 2, 3.5f}, {5, 4, 5.5f},
                              {9, 4, 5.5f},  {12, 4, 5.5f}, {7, 6, 8},     {11, 6, 8},    {14, 6, 8},    {19, 8, 9},    {17, 9, 10},
                              {16, 10, 11}, {14, 11, 12}, {7, 12, 13.5f}, {12, 12, 13.5f}, {16, 14, 16}, {9, 14, 16}};
    const ImU32 col = theme::content(kHues[1], 0.95f);
    for (const N& n : notes) {
      const int l = n.lane * lanes / 24;
      if (l >= lanes) continue;
      const float y = q.y1 - lane * static_cast<float>(l + 1);
      fill(dl, {std::round(well.x0 + well.w() * n.a / beats) + 0.5f, y + 0.5f, std::round(well.x0 + well.w() * n.b / beats) - 0.5f, y + lane - 0.5f}, col,
           std::min(1.5f, lane * 0.25f));
    }
    const float ph = std::round(well.x0 + well.w() * 0.37f) + 0.5f;
    dl->AddLine(v(ph, q.y0), v(ph, q.y1), P.playhead);
  }

  void history(const Box& q) const {
    const float rh = std::max(3.0f, std::round(py(24)));
    const float lens[] = {0.5f, 0.62f, 0.4f, 0.56f, 0.46f, 0.6f, 0.38f, 0.52f, 0.44f, 0.58f, 0.36f, 0.5f, 0.42f, 0.54f, 0.48f, 0.4f, 0.56f, 0.46f,
                          0.6f, 0.38f, 0.52f, 0.44f, 0.5f, 0.42f};
    float y = q.y0 + py(6);
    for (int i = 0; y + rh <= q.y1 && i < 24; ++i, y += rh) {
      const bool cur = i == 6;
      if (cur) fill(dl, {q.x0 + 1, y, q.x1 - 1, y + rh}, P.selection);
      const float yc = y + rh * 0.5f, s = std::max(1.0f, std::round(rh * 0.36f));
      dl->AddCircleFilled(v(q.x0 + px(16), yc), s * 0.5f, i > 6 ? theme::withAlpha(P.textFaint, 0.35f) : theme::withAlpha(P.textDim, 0.7f));
      textBar(q.x0 + px(28), yc, (q.w() - px(40)) * lens[i], std::max(1.0f, std::round(rh * 0.26f)),
              theme::withAlpha(i > 6 ? P.textFaint : P.textDim, i > 6 ? 0.4f : 0.65f));
    }
  }

  // One window: body, a title strip when there is room, then its content.
  void pane(Box r, Pane kind, bool floating, float radius) const {
    if (floating) {
      fill(dl, {r.x0 + 1, r.y0 + 2, r.x1 + 2, r.y1 + 3}, theme::withAlpha(P.ink, P.dark ? 0.35f : 0.12f), radius + 1);
      fill(dl, r, P.surface900, radius);
      dl->AddRect(v(r.x0, r.y0), v(r.x1, r.y1), P.lineStrong, radius);
    } else {
      fill(dl, r, P.surface900, radius);
    }
    const float th = std::round(py(22));
    Box q = r;
    if (th >= 4) {
      const Box strip{r.x0, r.y0, r.x1, r.y0 + th};
      fill(dl, strip, P.surface800, radius, ImDrawFlags_RoundCornersTop);
      textBar(strip.x0 + std::max(2.0f, px(10)), (strip.y0 + strip.y1) * 0.5f, std::min(strip.w() * 0.3f, px(90)), std::max(1.0f, th * 0.26f),
              theme::withAlpha(P.textDim, 0.65f));
      q.y0 = strip.y1;
    }
    q = q.inset(1);
    if (q.w() < 3 || q.h() < 3) return;
    dl->PushClipRect(v(q.x0, q.y0), v(q.x1, q.y1), true);
    switch (kind) {
      case Pane::Browser: browser(q); break;
      case Pane::Playlist: playlist(q); break;
      case Pane::Mixer: mixer(q); break;
      case Pane::Rack: rack(q); break;
      case Pane::Piano: piano(q); break;
      case Pane::History: history(q); break;
    }
    dl->PopClipRect();
  }
};

// ------------------------------------------------------------------ cards
struct Look {
  ImU32 card, cardHover, thumb, line, lineHover;
};
Look look() {
  const auto& P = pal();
  Look l{};
  if (P.dark) {
    l.card = P.surface900;
    l.cardHover = theme::mix(P.surface900, P.surface800, 0.55f);
    l.thumb = P.surface950;
    l.line = theme::mix(P.line, P.surface800, 0.35f);
  } else {
    l.card = theme::mix(P.surface800, P.paper, 0.55f);
    l.cardHover = P.paper;
    l.thumb = P.surface900;
    l.line = P.line;
  }
  l.lineHover = P.lineStrong;
  return l;
}

float ease(ImGuiID id, bool on) {
  static std::map<ImGuiID, float> m;
  float& t = m[id];
  const float target = on ? 1.0f : 0.0f;
  if (!scrollui::animations()) return t = target;
  t += (target - t) * std::min(1.0f, ImGui::GetIO().DeltaTime * 14.0f);
  if (std::fabs(t - target) < 0.01f) t = target;
  return t;
}

// The cards drawn on the last frame (dev report).
struct Drawn {
  int frame = -1;
  std::string where;
  ImVec2 p0[layout::kPresetCount], p1[layout::kPresetCount];
  int n = 0;
};
Drawn gDrawn, gDrawnPrev;

void noteDrawn(int i, ImVec2 p0, ImVec2 p1) {
  const int f = ImGui::GetFrameCount();
  if (gDrawn.frame != f) {
    if (gDrawn.n) gDrawnPrev = gDrawn;
    gDrawn = Drawn{};
    gDrawn.frame = f;
    gDrawn.where = ImGui::GetCurrentWindow() ? ui::stableWindowName(ImGui::GetCurrentWindow()->RootWindow->Name) : "";
  }
  if (i >= 0 && i < layout::kPresetCount) {
    gDrawn.p0[i] = p0;
    gDrawn.p1[i] = p1;
    gDrawn.n = std::max(gDrawn.n, i + 1);
  }
}

constexpr float kCardPad = 8, kNameGap = 12, kNameH = 18, kLineH = 16, kCardBottom = 14;

}  // namespace

// ------------------------------------------------------------------ public
void drawPreview(ImDrawList* dl, ImVec2 p0, ImVec2 p1, Preset preset) {
  const auto& P = pal();
  const float W = p1.x - p0.x, H = p1.y - p0.y;
  if (W < 8 || H < 6) return;
  const Mini m{dl, P, W / kVW, H / kVH};
  const float top = topBarH(preset);
  fill(dl, {p0.x, p0.y, p1.x, p1.y}, P.chrome);
  m.topBar({p0.x, p0.y, p1.x, std::round(p0.y + m.py(top))}, preset);
  m.statusBar({p0.x, std::round(p1.y - m.py(kStatusH)), p1.x, p1.y});
  // The dock area: the panes with a hairline gap, as the docked windows sit.
  const float ay = p0.y + m.py(top);
  const float gap = std::max(1.0f, std::round(m.px(3)));
  const float rad = std::clamp(m.px(4), 1.0f, 3.0f);
  Region rs[4];
  const int n = regions(preset, rs);
  for (int pass = 0; pass < 2; ++pass)  // docked first, then what floats over them
    for (int i = 0; i < n; ++i) {
      if (rs[i].floating != (pass == 1)) continue;
      const workspace::Rect& r = rs[i].r;
      Box b{std::round(p0.x + m.px(r.x)), std::round(ay + m.py(r.y)), std::round(p0.x + m.px(r.x + r.w)), std::round(ay + m.py(r.y + r.h))};
      if (!rs[i].floating) b = b.inset(gap * 0.5f + (r.x <= 0.5f ? gap * 0.5f : 0), gap * 0.5f + (r.y <= 0.5f ? gap * 0.5f : 0), gap * 0.5f, gap * 0.5f);
      m.pane(b, rs[i].kind, rs[i].floating, rad);
    }
}

void drawGlyph(ImDrawList* dl, ImVec2 c, float size, Preset preset, ImU32 col) {
  const float w = std::round(size), h = std::round(size * 0.78f);
  const float x0 = std::round(c.x - w * 0.5f) + 0.5f, y0 = std::round(c.y - h * 0.5f) + 0.5f, x1 = x0 + w - 1, y1 = y0 + h - 1;
  const float th = 1.0f;
  const float sx = (x1 - x0) / kVW, sy = (y1 - y0) / kVH;
  const float top = y0 + std::max(2.0f, std::round(topBarH(preset) * sy * 1.3f));
  Region rs[4];
  const int n = regions(preset, rs);
  const float ah = (y1 - top) / (kVH - topBarH(preset) - kStatusH);
  // The main view softly filled, so each glyph reads at a glance.
  for (int i = 0; i < n; ++i) {
    const workspace::Rect& r = rs[i].r;
    const float rx0 = std::round(x0 + r.x * sx), ry0 = std::round(top + r.y * ah), rx1 = std::round(x0 + (r.x + r.w) * sx), ry1 = std::round(top + (r.y + r.h) * ah);
    if (rs[i].kind == Pane::Playlist) dl->AddRectFilled(v(rx0, ry0), v(rx1, ry1), theme::withAlpha(col, 0.22f));
    if (rs[i].floating) {
      dl->AddRectFilled(v(rx0 + 0.5f, ry0 + 0.5f), v(rx1 - 0.5f, ry1 - 0.5f), theme::withAlpha(col, 0.85f), 1.0f);
      continue;
    }
    // Dividers: each region's left and top edges inside the frame.
    if (r.x > 0.5f) dl->AddLine(v(rx0, ry0), v(rx0, ry1), col, th);
    if (r.y > 0.5f) dl->AddLine(v(rx0, ry0), v(rx1, ry0), col, th);
  }
  dl->AddLine(v(x0, top), v(x1, top), col, th);
  dl->AddRect(v(x0, y0), v(x1, y1), col, 2.0f, 0, th);
}

CardGrid cardGrid(float width, int count, float maxCardW, float minCardW) {
  CardGrid g;
  g.gap = width >= 560 ? theme::kS4 : theme::kS3;
  count = std::max(1, count);
  g.cols = count;
  while (g.cols > 1 && (width - g.gap * static_cast<float>(g.cols - 1)) / static_cast<float>(g.cols) < minCardW) --g.cols;
  g.cardW = std::floor(std::min(maxCardW, (width - g.gap * static_cast<float>(g.cols - 1)) / static_cast<float>(g.cols)));
  g.cardW = std::max(g.cardW, 96.0f);
  g.thumbH = std::round((g.cardW - 2 * kCardPad) * 0.625f);  // the window's own 16:10
  g.cardH = kCardPad + g.thumbH + kNameGap + kNameH + 4 + 2 * kLineH + kCardBottom;
  return g;
}

float CardGrid::height(int count) const {
  const int rows = (std::max(1, count) + cols - 1) / cols;
  return static_cast<float>(rows) * cardH + static_cast<float>(rows - 1) * gap;
}
float CardGrid::width(int count) const {
  const int c = std::min(cols, std::max(1, count));
  return static_cast<float>(c) * cardW + static_cast<float>(c - 1) * gap;
}

bool card(App& a, int index, ImVec2 pos, const CardGrid& g, bool focused, bool keyNav) {
  const auto& P = pal();
  const Look L = look();
  const Preset p = static_cast<Preset>(index);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p1(pos.x + g.cardW, pos.y + g.cardH);
  ImGui::PushID(index);
  ImGui::SetCursorScreenPos(pos);
  const bool clicked = ImGui::InvisibleButton("##layoutcard", ImVec2(g.cardW, g.cardH));
  const bool hov = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
  const float t = ease(ImGui::GetItemID(), hov || (focused && keyNav));
  ImGui::PopID();
  noteDrawn(index, pos, p1);
  const bool on = active() == p;
  const float r = theme::kRadiusLg + 2;
  dl->AddRectFilled(pos, p1, theme::mix(L.card, L.cardHover, t), r);
  // The miniature, on its own inset well.
  const ImVec2 t0(pos.x + kCardPad, pos.y + kCardPad), t1(p1.x - kCardPad, pos.y + kCardPad + g.thumbH);
  dl->AddRectFilled(ImVec2(t0.x - 1, t0.y - 1), ImVec2(t1.x + 1, t1.y + 1), L.thumb, theme::kRadius + 1);
  dl->PushClipRect(t0, t1, true);
  drawPreview(dl, t0, t1, p);
  dl->PopClipRect();
  dl->AddRect(ImVec2(t0.x - 0.5f, t0.y - 0.5f), ImVec2(t1.x + 0.5f, t1.y + 0.5f), theme::withAlpha(L.line, 0.9f), theme::kRadius + 1);
  if (!on && t > 0) dl->AddRectFilled(t0, t1, theme::withAlpha(P.paper, (P.dark ? 0.035f : 0.0f) * t));
  // Name, the preview badge, one line.
  const float cx = pos.x + kCardPad + 4, cw = g.cardW - 2 * (kCardPad + 4);
  const float ny = std::round(t1.y + kNameGap);
  const char* name = i18n::tr(layout::displayName(p));
  const float nameW = a.fontBold ? a.fontBold->CalcTextSizeA(theme::kFontBody, FLT_MAX, 0, name).x : 0;
  float badgeW = 0;
  if (layout::isPreview(p)) {
    const char* tag = i18n::tr(layout::previewTag());
    const ImVec2 ts = a.fontRegular->CalcTextSizeA(theme::kFontLabel, FLT_MAX, 0, tag);
    badgeW = std::round(ts.x + 12);
    const ImVec2 b0(std::round(cx + nameW + 8), ny + 1), b1(b0.x + badgeW, ny + 17);
    if (b1.x <= cx + cw) {
      dl->AddRectFilled(b0, b1, theme::withAlpha(P.accent, P.dark ? 0.16f : 0.11f), 8.0f);
      dl->AddText(a.fontRegular, theme::kFontLabel, ImVec2(std::round(b0.x + 6), std::round(b0.y + (16 - ts.y) * 0.5f)), theme::mix(P.accent, P.text, P.dark ? 0.45f : 0.2f),
                  tag);
    } else {
      badgeW = 0;
    }
  }
  theme::textEllipsis(dl, a.fontBold, theme::kFontBody, ImVec2(cx, ny), cw - (badgeW > 0 ? badgeW + 8 : 0), P.text, name);
  const char* line = i18n::tr(layout::blurb(p));
  const float ly = ny + kNameH + 4;
  dl->PushClipRect(ImVec2(cx, ly), ImVec2(cx + cw, ly + 2 * kLineH), true);
  dl->AddText(a.fontRegular, theme::kFontCaption, ImVec2(cx, ly), P.textDim, line, nullptr, cw);
  dl->PopClipRect();
  // Outline: quiet; hover stronger; the layout in use an accent ring with a check badge.
  if (on) {
    dl->AddRect(ImVec2(pos.x + 0.5f, pos.y + 0.5f), ImVec2(p1.x - 0.5f, p1.y - 0.5f), P.accent, r, 0, 2.0f);
    const float br = 10;
    const ImVec2 bc(t1.x - br - 6, t0.y + br + 6);
    dl->AddCircleFilled(bc, br + 2, theme::withAlpha(L.card, 0.9f), 24);
    dl->AddCircleFilled(bc, br, P.accent, 24);
    ui::drawIcon(dl, ui::Icon::Check, bc, 12, theme::contrastText(P.accent));
  } else {
    dl->AddRect(pos, p1, theme::mix(L.line, L.lineHover, t), r, 0, 1.0f);
  }
  if (focused && keyNav) dl->AddRect(ImVec2(pos.x - 3, pos.y - 3), ImVec2(p1.x + 3, p1.y + 3), theme::withAlpha(P.accent, 0.7f), r + 3, 0, 2.0f);
  if (held) dl->AddRectFilled(pos, p1, theme::withAlpha(P.ink, P.dark ? 0.10f : 0.04f), r);
  if (hov) {
    if (!layout::isPreview(p)) ui::Hint(line);
    else ui::Hint(std::string(line) + ". " + i18n::tr("Preview: the windows, shortcuts and menus are in place; more of this style comes in later updates."));
  }
  return clicked;
}

int cards(App& a, float width, float maxCardW, int focus, bool keyNav) {
  const CardGrid g = cardGrid(width, layout::kPresetCount, maxCardW, 150.0f);
  const ImVec2 o = ImGui::GetCursorScreenPos();
  int picked = -1;
  for (int i = 0; i < layout::kPresetCount; ++i) {
    const int row = i / g.cols, col = i % g.cols;
    const ImVec2 p(o.x + static_cast<float>(col) * (g.cardW + g.gap), o.y + static_cast<float>(row) * (g.cardH + g.gap));
    if (card(a, i, p, g, focus == i, keyNav)) picked = i;
  }
  ImGui::SetCursorScreenPos(o);
  ImGui::Dummy(ImVec2(g.width(layout::kPresetCount), g.height(layout::kPresetCount)));
  if (picked >= 0) choose(a, static_cast<Preset>(picked), layout::linkedFromSettings(a.settings));
  return picked;
}

namespace {
struct OptDrawn {
  int frame = -1;
  ImVec2 p0, p1;
  bool on = false;
};
OptDrawn gOpt[2];  // the two options drawn last (dev report)

// A pill switch with a label and a one-line hint under it. Returns true when flipped.
bool optionSwitch(App& a, const char* id, const char* label, const char* hint, bool* on, float width) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 o = ImGui::GetCursorScreenPos();
  const float sw = 30, sh = 16, rowH = 38;
  const char* lt = i18n::tr(label);
  const char* ht = i18n::tr(hint);
  const float textW = std::max(a.fontRegular->CalcTextSizeA(theme::kFontBody, FLT_MAX, 0, lt).x, a.fontRegular->CalcTextSizeA(theme::kFontCaption, FLT_MAX, 0, ht).x);
  const float w = std::min(width, sw + 12 + textW + 4);
  ImGui::PushID(id);
  const bool clicked = ImGui::InvisibleButton("##switch", ImVec2(w, rowH));
  const bool hov = ImGui::IsItemHovered();
  const float t = ease(ImGui::GetID("##anim"), *on);
  const float h = ease(ImGui::GetID("##hov"), hov);
  ImGui::PopID();
  if (clicked) *on = !*on;
  gOpt[std::string(id) == "keys" ? 0 : 1] = {ImGui::GetFrameCount(), o, ImVec2(o.x + w, o.y + rowH), *on};
  const ImVec2 s0(o.x, std::round(o.y + 3)), s1(s0.x + sw, s0.y + sh);
  dl->AddRectFilled(s0, s1, theme::mix(theme::mix(P.field, P.surface700, 0.4f * h), P.accent, t), sh * 0.5f);
  if (t < 1) dl->AddRect(s0, s1, theme::withAlpha(P.lineStrong, 1.0f - t), sh * 0.5f);
  const float kx = s0.x + sh * 0.5f + (sw - sh) * t;
  dl->AddCircleFilled(ImVec2(kx, s0.y + sh * 0.5f), sh * 0.5f - 2.5f, theme::mix(P.textDim, P.paper, std::max(t, P.dark ? 0.0f : 0.6f)), 16);
  const float tx = s1.x + 12;
  theme::textEllipsis(dl, a.fontRegular, theme::kFontBody, ImVec2(tx, std::round(o.y + 2)), o.x + w - tx, P.text, lt);
  theme::textEllipsis(dl, a.fontRegular, theme::kFontCaption, ImVec2(tx, std::round(o.y + 21)), o.x + w - tx, P.textFaint, ht);
  if (ImGui::IsItemFocused()) ui::FocusRing();
  return clicked;
}
}  // namespace

void linkedOptions(App& a, float width) {
  static layout::Linked devLinked = layout::linkedFromSettings(a.settings);  // a --layout run: this run only
  layout::Linked linked = hasDevOverride() ? devLinked : layout::linkedFromSettings(a.settings);
  const float colW = width >= 560 ? std::floor((width - theme::kS6) * 0.5f) : width;
  bool changed = optionSwitch(a, "keys", i18n::N_("Matching shortcuts"), i18n::N_("The layout's own key set comes with it"), &linked.keys, colW);
  if (width >= 560) ImGui::SameLine(0, theme::kS6);
  changed |= optionSwitch(a, "menus", i18n::N_("Matching menus"), i18n::N_("The layout's own menu bar comes with it"), &linked.menus, colW);
  if (!changed) return;
  if (hasDevOverride()) {
    devLinked = linked;
    return;
  }
  a.settings.set("ui.layoutLinkKeys", linked.keys);
  a.settings.set("ui.layoutLinkMenus", linked.menus);
}

std::string devCards(const App& a) {
  const Drawn& d = gDrawn.frame >= ImGui::GetFrameCount() - 1 ? gDrawn : gDrawnPrev;
  std::string out;
  char b[160];
  for (int i = 0; i < d.n; ++i) {
    std::snprintf(b, sizeof b, "layoutcard %d %s x=%.0f y=%.0f w=%.0f h=%.0f in=%s\n", i, layout::id(static_cast<Preset>(i)), static_cast<double>(d.p0[i].x),
                  static_cast<double>(d.p0[i].y), static_cast<double>(d.p1[i].x - d.p0[i].x), static_cast<double>(d.p1[i].y - d.p0[i].y), d.where.c_str());
    out += b;
  }
  for (int i = 0; i < 2; ++i) {
    const OptDrawn& o = gOpt[i];
    if (o.frame < ImGui::GetFrameCount() - 1) continue;
    std::snprintf(b, sizeof b, "layoutoption %s x=%.0f y=%.0f w=%.0f h=%.0f on=%d\n", i ? "menus" : "keys", static_cast<double>(o.p0.x), static_cast<double>(o.p0.y),
                  static_cast<double>(o.p1.x - o.p0.x), static_cast<double>(o.p1.y - o.p0.y), o.on ? 1 : 0);
    out += b;
  }
  std::snprintf(b, sizeof b, "layout active=%s setting=%s keys=%s menus=%s\n", layout::id(active()), layout::id(layout::current(a.settings)),
                layout::id(layout::keyProfile(a.settings)), layout::id(layout::menuStyle(a.settings)));
  out += b;
  return out;
}

}  // namespace gl::layoutui
