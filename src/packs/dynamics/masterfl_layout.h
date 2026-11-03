// Gridline - Crest's editor layout (K-40): one grid, as a pure function of the
// form size, so a test can prove it without drawing. Form units are logical
// pixels at the plugin's own width (kW); skin::Form scales them to the window.
//
//   curve  | monitor                   displays, the same height
//   readout| tools                     one strip of equal-height controls
//   tabs | MODE | GAIN | ENVELOPE | BANDS | SPLIT
//
// The lower deck has twelve equal columns and two rows: every knob of the deck
// has one size and sits on a crossing of the grid, every caption on its row's
// one label line, and selectors and buttons are centred on the row's knob line.
#pragma once
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace gl::packs::dynamics::crest {

constexpr float kW = 860;
constexpr float kMargin = 6, kGutter = 4;  // around the form, between neighbouring cells
constexpr float kDisplayH = 280;           // the curve is square, the monitor takes the rest
constexpr float kStripH = 30, kControlH = 24, kGlyphW = 24;  // the tool strip and its controls
constexpr float kHeadH = 20, kTabsW = 84;
constexpr float kKnob = 46, kLabelH = 14, kLabelGap = 2;  // one knob size, one caption height
constexpr float kRowPad = 6, kRowGap = 6;
constexpr int kColumns = 12, kRows = 2;

struct Rect {
  float x = 0, y = 0, w = 0, h = 0;
  float x1() const { return x + w; }
  float y1() const { return y + h; }
  float cx() const { return x + w * 0.5f; }
  float cy() const { return y + h * 0.5f; }
  Rect scaled(float s) const { return Rect{x * s, y * s, w * s, h * s}; }
};

enum class Kind { Knob, Label, Control };

// One placed element. Knobs and their captions carry their grid crossing.
struct Item {
  std::string id;
  Kind kind = Kind::Control;
  Rect r;
  int row = -1, col = -1;
};

struct Cell {
  const char* title = nullptr;  // nullptr: no header strip
  Rect r;
  int firstCol = 0, cols = 0;
};

struct Layout {
  float W = kW, H = 0;
  Rect curve, monitor, strip, readout;
  Rect menu, speedLabel, speed, heat, views, pause, store, flip;
  Rect glyph[7];
  int glyphs = 7;
  Cell tabs, mode, gain, env, mix, split;
  Rect bandTab[4];
  Rect modeCell[4];
  int modes = 4;
  float colX[kColumns] = {};   // centre of each deck column
  float knobY[kRows] = {};     // centre of each deck row
  float labelY[kRows] = {};    // top of each row's caption line
  std::vector<Item> items;     // every control and caption, for the checks

  Rect knobAt(int row, int col) const { return Rect{colX[col] - kKnob * 0.5f, knobY[row] - kKnob * 0.5f, kKnob, kKnob}; }
  // A selector or button on the row's knob line, `cols` columns wide, starting at column col.
  Rect controlAt(int row, int col, float w, int cols = 1) const {
    const float cx = 0.5f * (colX[col] + colX[col + cols - 1]);
    return Rect{cx - w * 0.5f, knobY[row] - kControlH * 0.5f, w, kControlH};
  }
  Rect labelAt(int row, int col, int cols = 1) const {
    const float pitch = colX[1] - colX[0];
    const float cx = 0.5f * (colX[col] + colX[col + cols - 1]);
    const float w = pitch * static_cast<float>(cols) - 2;
    return Rect{cx - w * 0.5f, labelY[row], w, kLabelH};
  }
};

// `reference`: the current engine (four modes, display options, views, pause,
// linear phase). The older engine has three modes, four display options and
// its saturation mode in the place of the linear-phase button.
inline Layout makeLayout(bool reference) {
  Layout l;
  auto add = [&l](const std::string& id, Kind k, const Rect& r, int row = -1, int col = -1) { l.items.push_back(Item{id, k, r, row, col}); };
  const float inner = kW - 2 * kMargin;
  // ---- displays
  l.curve = Rect{kMargin, kMargin, kDisplayH, kDisplayH};
  l.monitor = Rect{l.curve.x1() + kGutter, kMargin, kW - kMargin - l.curve.x1() - kGutter, kDisplayH};
  add("curve", Kind::Control, l.curve);
  add("monitor", Kind::Control, l.monitor);
  // ---- tool strip: the readout under the curve; tools from both ends of the monitor's width
  l.strip = Rect{kMargin, l.curve.y1() + kGutter, inner, kStripH};
  const float cy = l.strip.y + (kStripH - kControlH) * 0.5f;
  auto tool = [&](float x, float w) { return Rect{x, cy, w, kControlH}; };
  l.readout = Rect{l.strip.x + 8, cy, l.curve.w - 16, kControlH};
  float x = l.monitor.x;
  l.menu = tool(x, kGlyphW);
  add("menu", Kind::Control, l.menu);
  x += kGlyphW + 6;
  l.glyphs = reference ? 7 : 4;
  for (int i = 0; i < l.glyphs; ++i) {
    l.glyph[i] = tool(x, kGlyphW);
    add("glyph" + std::to_string(i), Kind::Control, l.glyph[i]);
    x += kGlyphW + 2;
  }
  float r = l.strip.x1() - 4;
  auto fromRight = [&](float w, float gap = 2) {
    const Rect q = tool(r - w, w);
    r -= w + gap;
    return q;
  };
  l.flip = fromRight(34);
  l.store = fromRight(24, 8);
  add("flip", Kind::Control, l.flip);
  add("store", Kind::Control, l.store);
  if (reference) {
    l.pause = fromRight(24, 4);
    l.views = fromRight(122, 4);
    l.heat = fromRight(kGlyphW, 8);
    add("pause", Kind::Control, l.pause);
    add("views", Kind::Control, l.views);
    add("heat", Kind::Control, l.heat);
  }
  // SPEED takes what is left between the two ends, up to a sensible length.
  const float speedRoom = r - x - 6;
  const float captionW = 40, sliderW = std::clamp(speedRoom - captionW, 20.0f, 120.0f);
  l.speedLabel = tool(x + 6, captionW);
  l.speed = Rect{l.speedLabel.x1(), cy + 6, sliderW, kControlH - 12};
  add("speed", Kind::Control, l.speed);
  add("speedLabel", Kind::Label, l.speedLabel);
  // ---- lower deck: tabs, then twelve equal columns grouped into five cells
  const float deckY = l.strip.y1() + kGutter;
  const float rowH = kKnob + kLabelGap + kLabelH;
  const float bodyTop = deckY + kHeadH + kRowPad;
  const float cellH = kHeadH + kRowPad + 2 * rowH + kRowGap + kRowPad;
  for (int row = 0; row < kRows; ++row) {
    const float top = bodyTop + static_cast<float>(row) * (rowH + kRowGap);
    l.knobY[row] = top + kKnob * 0.5f;
    l.labelY[row] = top + kKnob + kLabelGap;
  }
  static constexpr int kCellCols[5] = {2, 2, 4, 1, 3};
  const float pitch = (inner - kTabsW - 5 * kGutter) / static_cast<float>(kColumns);
  l.tabs = Cell{nullptr, Rect{kMargin, deckY, kTabsW, cellH}, 0, 0};
  Cell* cells[5] = {&l.mode, &l.gain, &l.env, &l.mix, &l.split};
  static const char* const kTitles[5] = {"MODE", "GAIN", "ENVELOPE", "BANDS", "SPLIT"};
  float cellX = l.tabs.r.x1() + kGutter;
  int col = 0;
  for (int c = 0; c < 5; ++c) {
    const float w = pitch * static_cast<float>(kCellCols[c]);
    *cells[c] = Cell{kTitles[c], Rect{cellX, deckY, w, cellH}, col, kCellCols[c]};
    for (int k = 0; k < kCellCols[c]; ++k) l.colX[col + k] = cellX + pitch * (static_cast<float>(k) + 0.5f);
    col += kCellCols[c];
    cellX += w + kGutter;
  }
  l.H = deckY + cellH + kMargin;
  // Band tabs fill their cell, one above the other.
  {
    const float pad = 4, gap = 3, h = (cellH - 2 * pad - 3 * gap) * 0.25f;
    for (int i = 0; i < 4; ++i) {
      l.bandTab[i] = Rect{l.tabs.r.x + pad, l.tabs.r.y + pad + static_cast<float>(i) * (h + gap), kTabsW - 2 * pad, h};
      add("band" + std::to_string(i), Kind::Control, l.bandTab[i]);
    }
  }
  // Mode: a block of two by two buttons on the first row's knob line.
  l.modes = reference ? 4 : 3;
  {
    const float gap = 4, side = 6;
    const float w = (l.mode.r.w - 2 * side - gap) * 0.5f;
    const float top = l.knobY[0] - kControlH - gap * 0.5f + kLabelH * 0.5f;
    for (int i = 0; i < l.modes; ++i) {
      l.modeCell[i] = Rect{l.mode.r.x + side + static_cast<float>(i % 2) * (w + gap), top + static_cast<float>(i / 2) * (kControlH + gap), w, kControlH};
      add("mode" + std::to_string(i), Kind::Control, l.modeCell[i]);
    }
  }
  // Knobs and their captions.
  struct K {
    const char* id;
    int row, col;
  };
  static const K kKnobs[] = {{"stereo", 1, 0}, {"pre", 0, 2},     {"post", 0, 3},    {"satThres", 1, 2}, {"satCeil", 1, 3},
                             {"attack", 0, 4}, {"release", 0, 5}, {"release2", 0, 6}, {"sustain", 0, 7},
                             {"delay", 0, 8},  {"mix", 1, 8},     {"low", 0, 9},     {"high", 0, 10},   {"lowCut", 0, 11}};
  for (const K& k : kKnobs) {
    add(k.id, Kind::Knob, l.knobAt(k.row, k.col), k.row, k.col);
    add(std::string(k.id) + ".label", Kind::Label, l.labelAt(k.row, k.col), k.row, k.col);
  }
  // Selectors and buttons on the second row, captioned on its label line.
  const float kSmall = pitch - 8;
  add("solo", Kind::Control, l.controlAt(1, 1, kSmall), 1, 1);
  add("atkCurve", Kind::Control, l.controlAt(1, 4, kSmall), 1, 4);
  add("atkCurve.label", Kind::Label, l.labelAt(1, 4), 1, 4);
  add("relCurve", Kind::Control, l.controlAt(1, 5, kSmall), 1, 5);
  add("relCurve.label", Kind::Label, l.labelAt(1, 5), 1, 5);
  add("detector", Kind::Control, l.controlAt(1, 6, pitch * 2 - 8, 2), 1, 6);
  add("detector.label", Kind::Label, l.labelAt(1, 6, 2), 1, 6);
  add("lowSlope", Kind::Control, l.controlAt(1, 9, kSmall), 1, 9);
  add("lowSlope.label", Kind::Label, l.labelAt(1, 9), 1, 9);
  add("highSlope", Kind::Control, l.controlAt(1, 10, kSmall), 1, 10);
  add("highSlope.label", Kind::Label, l.labelAt(1, 10), 1, 10);
  add("option", Kind::Control, l.controlAt(1, 11, kSmall), 1, 11);
  add("option.label", Kind::Label, l.labelAt(1, 11), 1, 11);
  return l;
}

inline Rect itemRect(const Layout& l, const char* id) {
  for (const Item& i : l.items)
    if (i.id == id) return i.r;
  return Rect{};
}

// What a check finds: empty when clean. Pure geometry at a form scale.
inline std::vector<std::string> checkLayout(const Layout& l, float scale, float width = -1) {
  std::vector<std::string> out;
  const float eps = 0.01f;
  auto scaled = [&](const Item& i) { return i.r.scaled(scale); };
  for (size_t a = 0; a < l.items.size(); ++a)
    for (size_t b = a + 1; b < l.items.size(); ++b) {
      const Rect p = scaled(l.items[a]), q = scaled(l.items[b]);
      if (p.x < q.x1() - eps && q.x < p.x1() - eps && p.y < q.y1() - eps && q.y < p.y1() - eps) out.push_back("overlap " + l.items[a].id + " / " + l.items[b].id);
    }
  const float formW = (width > 0 ? width : l.W) * scale, formH = l.H * scale;
  for (const Item& i : l.items) {
    const Rect p = scaled(i);
    if (p.x < -eps || p.y < -eps || p.x1() > formW + eps || p.y1() > formH + eps) out.push_back("outside the form: " + i.id);
    if (i.kind == Kind::Knob && (std::fabs(i.r.w - kKnob) > eps || std::fabs(i.r.h - kKnob) > eps)) out.push_back("knob size: " + i.id);
    if (i.kind == Kind::Knob && (std::fabs(i.r.cy() - l.knobY[i.row]) > eps || std::fabs(i.r.cx() - l.colX[i.col]) > eps)) out.push_back("knob off the grid: " + i.id);
    if (i.kind == Kind::Label && i.row >= 0 && std::fabs(i.r.y - l.labelY[i.row]) > eps) out.push_back("caption off the row's line: " + i.id);
    if (i.kind == Kind::Control && i.row >= 0 && std::fabs(i.r.cy() - l.knobY[i.row]) > eps) out.push_back("control off the row's knob line: " + i.id);
  }
  const Cell* cells[5] = {&l.mode, &l.gain, &l.env, &l.mix, &l.split};
  for (const Cell* c : cells) {
    if (c->r.y1() > l.H - kMargin + eps) out.push_back(std::string("cell below the form: ") + c->title);
    for (int k = 0; k < c->cols; ++k) {
      const float half = (l.colX[1] - l.colX[0]) * 0.5f;
      if (l.colX[c->firstCol + k] - half < c->r.x - eps || l.colX[c->firstCol + k] + half > c->r.x1() + eps) out.push_back(std::string("column outside its cell: ") + c->title);
    }
  }
  if (std::fabs(l.split.r.x1() - (l.W - kMargin)) > 0.5f) out.push_back("the deck does not reach the right margin");
  return out;
}

}  // namespace gl::packs::dynamics::crest
