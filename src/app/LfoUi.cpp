// Gridline - K-247: the LFO board (see LfoUi.h).
#include "LfoUi.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>
#include "../../third_party/imgui/imgui.h"
#include "../../third_party/imgui/imgui_internal.h"
#include "../core/I18n.h"
#include "Keys.h"
#include "../core/Lfo.h"
#include "../core/Macros.h"
#include "../core/Params.h"
#include "App.h"
#include "LayoutPresets.h"
#include "TrackerView.h"
#include "LayoutShellUi.h"
#include "Theme.h"
#include "TrackerLook.h"
#include "TrackerPages.h"
#include "Widgets.h"

namespace gl::lfoui {

using theme::pal;
using i18n::tr;

namespace {

constexpr float kPi = 3.14159265f;
constexpr const char* kPayload = "GL_LFO";

// ---- board state (this run) ----------------------------------------------
ObjectId gSelected = kNoId;
int gFilter = 0;        // 0 all, 1 moving now, 2 no targets
int gSort = 0;          // 0 board order, 1 name, 2 most targets
int gView = 0;          // 0 cards, 1 compact
bool gPopOut = false;   // Studio: undock to a floating full board next frame
int gPopSize = 0;       // frames left to hold the popped-out size (undocking resizes once)
bool gDockBack = false; // Studio: back beside the mixer next frame
char gSearch[96] = {};
// The popovers: the target settings (LFO, target) and the add-target list (LFO).
ObjectId gSetLfo = kNoId;
ParamId gSetTarget{};
bool gOpenSettings = false;
ObjectId gAddLfo = kNoId;
bool gOpenAdd = false;
char gAddSearch[96] = {};
int gAddCat = 0;
std::deque<ParamId> gTouched;  // recently moved controls (newest first), for "Touched"
// Rename in place.
ObjectId gRename = kNoId;
char gRenameBuf[64] = {};
// Dev: what the last frame drew.
struct DevCard {
  ObjectId id;
  ImVec2 mn, mx, handle;
  std::vector<std::pair<ParamId, ImVec2>> chips;
  ImVec2 add, depth;
};
std::vector<DevCard> gDevCards;
bool gDevCompact = false;
int gDevCols = 0;
ImVec2 gDevWin0, gDevWin1;
ImVec2 gPopAt(-1, -1);  // where the next popover opens (a dev command: under its chip); else at the mouse
ObjectId gPickLfo = kNoId;  // Pick on screen: the LFO whose next clicked knob becomes a target
// Under the last item: where a chip's popover opens.
ImVec2 underItem() { return ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + 4); }

bool trackerLook() { return layoutui::active() == layout::Preset::Tracker; }

ImFont* monoFont(App& a) { return a.fontMono ? a.fontMono : ImGui::GetFont(); }

float fsLabel() { return std::min(theme::kFontLabel, ImGui::GetFontSize()); }

void text(ImDrawList* dl, ImFont* f, float size, ImVec2 p, ImU32 c, const char* s) {
  dl->AddText(f, size, ImVec2(std::round(p.x), std::round(p.y)), c, s);
}
float textW(ImFont* f, float size, const char* s) { return f->CalcTextSizeA(size, FLT_MAX, 0, s).x; }
// `s` cut to `maxW` (with an ellipsis) in font `f` at `size`.
const char* fit(ImFont* f, float size, const char* s, float maxW) {
  ImGui::PushFont(f, size);
  const char* r = theme::fitText(s, maxW);
  ImGui::PopFont();
  return r;
}

// The target's move as a chip's amount: "+40%", "-60%", "±25%", "20 to 80%".
std::string amountText(const ModLink& l) {
  char b[48];
  if (l.mode == kModAbsolute) {
    std::snprintf(b, sizeof b, "%d to %d%%", static_cast<int>(std::lround(l.min * 100)), static_cast<int>(std::lround(l.max * 100)));
    return b;
  }
  const int pc = static_cast<int>(std::lround(std::fabs(l.amount) * 100));
  const bool neg = (l.amount < 0) != l.invert;
  if (l.direction == kModBothWays) std::snprintf(b, sizeof b, "\xC2\xB1%d%%", pc);
  else if (l.direction == kModUpOnly) std::snprintf(b, sizeof b, "%s%d%%", neg ? "-" : "+", pc);
  else std::snprintf(b, sizeof b, "%s%d%%", neg ? "+" : "-", pc);
  return b;
}

std::string restartNote(const Song& s, const SongLfo& l) {
  if (l.kind == LfoKind::Random) {  // 0.7.8: when it draws a new value
    char b[160];
    switch (l.trigger) {
      case RandomTrigger::Tempo: std::snprintf(b, sizeof b, tr("a new value every %s"), lfo::rateLabel(l).c_str()); return b;
      case RandomTrigger::Hz: std::snprintf(b, sizeof b, tr("a new value %s"), lfo::rateLabel(l).c_str()); return b;
      case RandomTrigger::Roll: return tr("a new value when you roll");
      default: break;
    }
    const Channel* ch = s.channel(l.noteChannel);
    if (!ch) return tr("a new value on every note");
    std::snprintf(b, sizeof b, tr("a new value on %s notes"), ch->name.c_str());
    return b;
  }
  if (l.restart == LfoRestart::EachBar) return tr("restarts every bar");
  if (l.restart == LfoRestart::Free) return tr("runs freely");
  if (l.restart == LfoRestart::OnPlay) return tr("locked to the song");
  const Channel* ch = s.channel(l.noteChannel);
  if (!ch) return tr("restarts on every note");
  char b[160];
  std::snprintf(b, sizeof b, tr("restarts on %s notes"), ch->name.c_str());
  return b;
}

int targetCount(const Song& s) {
  int n = 0;
  for (const ModLink& l : s.modLinks) n += l.isLfo() && s.lfo(l.sourceOwner) ? 1 : 0;
  return n;
}

bool moving(App& a, const SongLfo& l) {
  const auto rt = a.builder.lfoRuntime(l.id);
  return l.on && rt && rt->running.load(std::memory_order_relaxed) && !lfo::linksOf(a.doc.song(), l.id).empty();
}

// The LFO's output for the picture (x 0..cycles), as the engine plays it.
float waveAt(const SongLfo& l, const lfo::DrawnTable* table, double x) {
  const double ph = x + static_cast<double>(l.phase) / 360.0;
  const double cyc = std::floor(ph);
  const float raw = lfo::shapeValue(l.shape, ph - cyc, static_cast<uint64_t>(std::max(0.0, cyc)), l.pulseWidth, l.randomSteps, l.seed,
                                    table);
  return lfo::shapeOutput(raw, l.depth, l.offset, l.unipolar, 1.0f);
}

void drawSourceTrace(App& a, ImDrawList* dl, ImVec2 mn, ImVec2 mx, const SongLfo& l, float cycles, bool grid, bool dot);
// 0.7.8: a Chaos or Random source's output at x (cycles: Chaos's walk, Random's trigger number), as
// the engine plays it. Random's glide is drawn as the engine's smoothing over a step of the rate.
float sourceAt(const SongLfo& l, double x, double bpm) {
  float raw = 0;
  if (l.kind == LfoKind::Chaos) {
    raw = lfo::chaosValue(l.seed, x, l.character);
  } else {
    const double fl = std::floor(std::max(0.0, x));
    const uint64_t k = static_cast<uint64_t>(fl);
    raw = lfo::randomSample(l.seed, k, l.centred, l.quantise);
    if (l.smoothMs > 0 && k > 0) {
      const bool clocked = l.trigger == RandomTrigger::Tempo || l.trigger == RandomTrigger::Hz;
      SongLfo c = l;
      c.tempoSync = l.trigger == RandomTrigger::Tempo;
      const double stepS = clocked ? 1.0 / std::max(0.01, lfo::rateHz(c, bpm)) : 0.5;  // (notes and rolls: about half a second apart)
      const float prev = lfo::randomSample(l.seed, k - 1, l.centred, l.quantise);
      const double f = (x - fl) * stepS / (l.smoothMs * 0.001);
      raw = static_cast<float>(prev + (raw - prev) * (1.0 - std::exp(-f)));
    }
  }
  return lfo::shapeOutput(raw, l.depth, l.offset, l.unipolar, 1.0f);
}

// The wave picture in [mn, mx]: `cycles` cycles in the LFO's colour over an eighths grid, filled
// under the line, the live dot at the engine's phase. (0.7.8: Chaos shows twice as many cycles of its
// walk and Random four times as many steps, round where they are now.)
void drawWave(App& a, ImDrawList* dl, ImVec2 mn, ImVec2 mx, const SongLfo& l, float cycles, bool grid, bool dot) {
  if (l.kind != LfoKind::Lfo) {
    drawSourceTrace(a, dl, mn, mx, l, cycles * (l.kind == LfoKind::Chaos ? 2.0f : 4.0f), grid, dot);
    return;
  }
  const auto& P = pal();
  const ImU32 c = colour(l.colour);
  lfo::DrawnTable table{};
  const bool drawn = l.shape == LfoShape::Drawn;
  if (drawn) lfo::buildTable(l, table);
  const float pad = 6.0f, w = mx.x - mn.x, h = mx.y - mn.y;
  const float y0 = mn.y + pad, y1 = mx.y - pad;
  auto yOf = [&](float v) { return y1 - (v + 1.0f) * 0.5f * (y1 - y0); };
  if (grid) {
    for (int k = 1; k < 8; ++k) {
      const float x = std::round(mn.x + w * static_cast<float>(k) / 8.0f) + 0.5f;
      dl->AddLine(ImVec2(x, mn.y + 1), ImVec2(x, mx.y - 1), theme::withAlpha(P.text, k % 4 == 0 ? 0.09f : 0.05f));
    }
    const float ym = std::round(yOf(0)) + 0.5f;
    dl->AddLine(ImVec2(mn.x + 1, ym), ImVec2(mx.x - 1, ym), theme::withAlpha(P.text, 0.07f));
  }
  const int n = std::max(16, static_cast<int>(w));
  std::vector<ImVec2> pts;
  pts.reserve(static_cast<size_t>(n) + 1);
  for (int i = 0; i <= n; ++i) {
    const double x = static_cast<double>(i) / n * cycles;
    pts.emplace_back(mn.x + w * static_cast<float>(i) / static_cast<float>(n), yOf(waveAt(l, drawn ? &table : nullptr, x)));
  }
  const float base = yOf(l.unipolar ? 0.0f : 0.0f);
  for (int i = 0; i + 1 < static_cast<int>(pts.size()); ++i) {  // the fill: one quad a column
    const ImVec2 p0 = pts[static_cast<size_t>(i)], p1 = pts[static_cast<size_t>(i) + 1];
    dl->AddQuadFilled(ImVec2(p0.x, base), p0, p1, ImVec2(p1.x, base), theme::withAlpha(c, l.on ? 0.13f : 0.06f));
  }
  dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), l.on ? c : theme::withAlpha(c, 0.45f), ImDrawFlags_None, 1.6f);
  if (!dot) return;
  const auto rt = a.builder.lfoRuntime(l.id);
  if (!rt || !l.on) return;
  const float ph = rt->phaseUi.load(std::memory_order_relaxed);
  const float v = rt->display.load(std::memory_order_relaxed);
  const bool run = rt->running.load(std::memory_order_relaxed);
  const float dx = mn.x + w * (ph / std::max(1.0f, cycles));
  const ImVec2 dc(dx, std::clamp(yOf(v), mn.y + 2, mx.y - 2));
  if (run) dl->AddLine(ImVec2(dx, mn.y + 1), ImVec2(dx, mx.y - 1), theme::withAlpha(c, 0.35f));
  dl->AddCircleFilled(dc, 4.0f, run ? c : theme::withAlpha(c, 0.5f), 16);
  dl->AddCircle(dc, 4.0f, P.surface950, 16, 1.0f);
  (void)h;
}

// 0.7.8: a Chaos source's trace or a Random source's steps over `cycles` (cycles of the walk, steps),
// from where it is now (three quarters along, the live dot there) or from the start when it rests.
void drawSourceTrace(App& a, ImDrawList* dl, ImVec2 mn, ImVec2 mx, const SongLfo& l, float cycles, bool grid, bool dot) {
  const auto& P = pal();
  const ImU32 c = colour(l.colour);
  const float pad = 6.0f, w = mx.x - mn.x;
  const float y0 = mn.y + pad, y1 = mx.y - pad;
  auto yOf = [&](float v) { return y1 - (v + 1.0f) * 0.5f * (y1 - y0); };
  const auto rt = a.builder.lfoRuntime(l.id);
  const bool run = dot && rt && l.on && rt->running.load(std::memory_order_relaxed);
  const double now = run ? rt->xUi.load(std::memory_order_relaxed) : 0.0;
  const double x0 = run ? std::max(0.0, now - cycles * 0.75) : 0.0;
  const bool steps = l.kind == LfoKind::Random;
  if (grid) {
    const int lines = steps ? static_cast<int>(cycles) : 8;
    for (int k = 1; k < lines; ++k) {
      const double gx = steps ? std::ceil(x0) + k - 1 : 0.0;
      const float fx = steps ? static_cast<float>((gx - x0) / cycles) : static_cast<float>(k) / 8.0f;
      if (fx <= 0 || fx >= 1) continue;
      const float xx = std::round(mn.x + w * fx) + 0.5f;
      dl->AddLine(ImVec2(xx, mn.y + 1), ImVec2(xx, mx.y - 1), theme::withAlpha(P.text, 0.05f));
    }
    const float ym = std::round(yOf(0)) + 0.5f;
    dl->AddLine(ImVec2(mn.x + 1, ym), ImVec2(mx.x - 1, ym), theme::withAlpha(P.text, 0.07f));
    if (steps && l.quantise >= 2)  // the levels it snaps to
      for (int q = 0; q < l.quantise; ++q) {
        const float v = lfo::shapeOutput(static_cast<float>(q) / static_cast<float>(l.quantise - 1) * 2.0f - 1.0f, l.depth, l.offset, l.unipolar, 1.0f);
        const float yy = std::round(yOf(v)) + 0.5f;
        for (float xx = mn.x + 2; xx < mx.x - 2; xx += 6) dl->AddLine(ImVec2(xx, yy), ImVec2(std::min(xx + 2, mx.x - 2), yy), theme::withAlpha(c, 0.22f));
      }
  }
  const double bpm = a.doc.song().tempo;
  const int n = std::max(16, static_cast<int>(w));
  std::vector<ImVec2> pts;
  pts.reserve(static_cast<size_t>(n) + 1);
  for (int i = 0; i <= n; ++i) {
    const double x = x0 + static_cast<double>(i) / n * cycles;
    pts.emplace_back(mn.x + w * static_cast<float>(i) / static_cast<float>(n), yOf(sourceAt(l, x, bpm)));
  }
  const float base = yOf(0.0f);
  for (int i = 0; i + 1 < static_cast<int>(pts.size()); ++i) {
    const ImVec2 p0 = pts[static_cast<size_t>(i)], p1 = pts[static_cast<size_t>(i) + 1];
    dl->AddQuadFilled(ImVec2(p0.x, base), p0, p1, ImVec2(p1.x, base), theme::withAlpha(c, l.on ? 0.13f : 0.06f));
  }
  dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), l.on ? c : theme::withAlpha(c, 0.45f), ImDrawFlags_None, 1.6f);
  if (!run) return;
  const float v = rt->display.load(std::memory_order_relaxed);
  const float dx = mn.x + w * static_cast<float>((now - x0) / cycles);
  const ImVec2 dc(dx, std::clamp(yOf(v), mn.y + 2, mx.y - 2));
  dl->AddLine(ImVec2(dx, mn.y + 1), ImVec2(dx, mx.y - 1), theme::withAlpha(c, 0.35f));
  dl->AddCircleFilled(dc, 4.0f, c, 16);
  dl->AddCircle(dc, 4.0f, P.surface950, 16, 1.0f);
}

// A shape's small glyph in [mn, mx].
void shapeGlyph(ImDrawList* dl, ImVec2 mn, ImVec2 mx, LfoShape s, ImU32 c) {
  SongLfo l;
  l.shape = s;
  l.randomSteps = 4;
  l.seed = 7;
  lfo::DrawnTable table{};
  if (s == LfoShape::Drawn) lfo::buildTable(l, table);
  const int n = 24;
  ImVec2 pts[n + 1];
  for (int i = 0; i <= n; ++i) {
    const double x = static_cast<double>(i) / n;
    const float raw = lfo::shapeValue(s, x, 0, 0.5f, 4, 7, s == LfoShape::Drawn ? &table : nullptr);
    pts[i] = ImVec2(mn.x + (mx.x - mn.x) * static_cast<float>(x), mx.y - (raw + 1.0f) * 0.5f * (mx.y - mn.y));
  }
  dl->AddPolyline(pts, n + 1, c, ImDrawFlags_None, 1.3f);
}

// The width miniSeg takes for these labels.
float miniSegWidth(App& a, const char* const* labels, int count) {
  ImFont* f = monoFont(a);
  const float fs = fsLabel() - 1.0f;
  float total = 3;
  for (int i = 0; i < count; ++i) total += std::ceil(textW(f, fs, tr(labels[i]))) + 13;
  return total;
}

// A small segmented control drawn on the draw list (mono labels), `on` lit in the accent.
bool miniSeg(App& a, const char* id, const char* const* labels, int count, int* on, ImVec2 p, float h, float* outW = nullptr,
             const char* const* hints = nullptr) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImFont* f = monoFont(a);
  const float fs = fsLabel() - 1.0f;
  float total = 2;
  std::vector<float> ws;
  for (int i = 0; i < count; ++i) ws.push_back(std::ceil(textW(f, fs, tr(labels[i]))) + 12), total += ws.back() + 1;
  total += 1;
  if (outW) *outW = total;
  dl->AddRectFilled(p, ImVec2(p.x + total, p.y + h), P.field, 4.0f);
  dl->AddRect(p, ImVec2(p.x + total, p.y + h), P.line, 4.0f);
  float x = p.x + 2;
  bool changed = false;
  ImGui::PushID(id);
  for (int i = 0; i < count; ++i) {
    const ImVec2 s0(x, p.y + 2), s1(x + ws[static_cast<size_t>(i)], p.y + h - 2);
    ImGui::SetCursorScreenPos(s0);
    ImGui::PushID(i);
    if (ImGui::InvisibleButton("##s", ImVec2(s1.x - s0.x, s1.y - s0.y)) && *on != i) *on = i, changed = true;
    const bool hov = ImGui::IsItemHovered();
    if (hov && hints && hints[i]) ui::Hint(hints[i]);
    ImGui::PopID();
    const bool lit = *on == i;
    if (lit) {
      dl->AddRectFilled(s0, s1, theme::withAlpha(P.accent, 0.13f), 3.0f);
      dl->AddRect(s0, s1, P.accent, 3.0f);
    } else if (hov) {
      dl->AddRectFilled(s0, s1, theme::withAlpha(P.text, 0.06f), 3.0f);
    }
    const char* lb = tr(labels[i]);
    text(dl, f, fs, ImVec2(s0.x + (s1.x - s0.x - textW(f, fs, lb)) * 0.5f, s0.y + (s1.y - s0.y - fs) * 0.5f), lit ? theme::lit(P.accent) : P.textDim, lb);
    x += ws[static_cast<size_t>(i)] + 1;
  }
  ImGui::PopID();
  return changed;
}

// A chip: a dot, the text, the amount; true when clicked.
bool chip(App& a, const char* id, ImVec2 p, float w, ImU32 dotCol, bool roundDot, const char* label, const char* amt, bool dashed, bool dim,
          bool* rightClick = nullptr) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float h = 22.0f, fs = fsLabel();
  ImGui::SetCursorScreenPos(p);
  const bool click = ImGui::InvisibleButton(id, ImVec2(w, h));
  const bool hov = ImGui::IsItemHovered();
  if (rightClick) *rightClick = ImGui::IsItemClicked(ImGuiMouseButton_Right);
  const ImVec2 q(p.x + w, p.y + h);
  if (dashed) {
    const ImU32 lc = hov ? P.textDim : P.lineStrong;
    for (float x = p.x + 8; x < q.x - 8; x += 6) {
      dl->AddLine(ImVec2(x, p.y + 0.5f), ImVec2(std::min(x + 3, q.x - 8), p.y + 0.5f), lc);
      dl->AddLine(ImVec2(x, q.y - 0.5f), ImVec2(std::min(x + 3, q.x - 8), q.y - 0.5f), lc);
    }
    dl->PathArcTo(ImVec2(p.x + 11, p.y + 11), 10.5f, kPi * 0.5f, kPi * 1.5f, 10);
    dl->PathStroke(lc, ImDrawFlags_None, 1.0f);
    dl->PathArcTo(ImVec2(q.x - 11, p.y + 11), 10.5f, -kPi * 0.5f, kPi * 0.5f, 10);
    dl->PathStroke(lc, ImDrawFlags_None, 1.0f);
  } else {
    dl->AddRectFilled(p, q, hov ? theme::mix(P.field, P.text, 0.06f) : P.field, 11.0f);
    dl->AddRect(p, q, P.line, 11.0f);
  }
  float x = p.x + 6;
  if (dotCol) {
    const ImU32 dc = dim ? theme::withAlpha(dotCol, 0.4f) : dotCol;
    if (roundDot) dl->AddCircleFilled(ImVec2(x + 4.5f, p.y + 11), 4.0f, dc, 12);
    else dl->AddRectFilled(ImVec2(x, p.y + 6.5f), ImVec2(x + 9, p.y + 15.5f), dc, 2.0f);
    x += 14;
  }
  const float amtW = amt && *amt ? textW(monoFont(a), fs - 1, amt) + 6 : 0;
  const char* shown = fit(ImGui::GetFont(), fs, label, q.x - x - amtW - 6);
  text(dl, ImGui::GetFont(), fs, ImVec2(x, p.y + (h - fs) * 0.5f), dashed ? (hov ? P.text : P.textDim) : dim ? P.textFaint : P.text, shown);
  if (amtW > 0) text(dl, monoFont(a), fs - 1, ImVec2(q.x - amtW - 3, p.y + (h - fs + 1) * 0.5f), P.textDim, amt);
  return click;
}

float chipWidth(App& a, const char* label, const char* amt, bool dot) {
  const float fs = fsLabel();
  return std::ceil(6 + (dot ? 14 : 0) + textW(ImGui::GetFont(), fs, label) + (amt && *amt ? textW(monoFont(a), fs - 1, amt) + 6 : 0) + 8);
}

// The round drag handle of an LFO: drag it onto any knob.
void handle(App&, const SongLfo& l, ImVec2 p, ImVec2 size, ImVec2* devAt) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::SetCursorScreenPos(p);
  ImGui::InvisibleButton("##handle", size);
  const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
  if (hov) ui::Hint(l.kind == LfoKind::Lfo ? "Drag onto any knob: this LFO moves it (25 percent around the knob)"
                                            : "Drag onto any knob: this source moves it (25 percent around the knob)");
  const ImVec2 q(p.x + size.x, p.y + size.y), c(std::round(p.x + size.x * 0.5f), std::round(p.y + size.y * 0.5f));
  if (devAt) *devAt = c;
  dl->AddRectFilled(p, q, hov || act ? theme::mix(P.field, P.text, 0.08f) : P.field, 4.0f);
  dl->AddRect(p, q, P.line, 4.0f);
  const ImU32 col = colour(l.colour);
  dl->AddCircle(c, 5.0f, col, 16, 1.5f);
  dl->AddCircleFilled(c, 1.7f, col, 8);
  if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip | ImGuiDragDropFlags_SourceNoDisableHover)) {
    const ObjectId id = l.id;
    ImGui::SetDragDropPayload(kPayload, &id, sizeof id);
    ImGui::EndDragDropSource();
  }
}

// The payload being dragged now (an LFO's handle), kNoId when none.
ObjectId dragged() {
  const ImGuiPayload* p = ImGui::GetDragDropPayload();
  if (!p || !p->IsDataType(kPayload) || p->DataSize != static_cast<int>(sizeof(ObjectId))) return kNoId;
  ObjectId id;
  std::memcpy(&id, p->Data, sizeof id);
  return id;
}

std::string paramShortName(const Song& s, ParamId pid) {
  const std::string full = lfo::targetLabel(s, pid);
  const size_t at = full.rfind(" \xC2\xB7 ");
  return at == std::string::npos ? full : full.substr(at + 4);
}

void showTheKnob(App& a, ParamId pid) {
  const Song& s = a.doc.song();
  if (s.lfo(pid.owner)) {
    gSelected = pid.owner;
    a.showLfoBoard = true;
    return;
  }
  if (s.channel(pid.owner)) {
    a.selChannel = pid.owner;
    a.settingsChannel = pid.owner;
    return;
  }
  if (s.mixerTrack(pid.owner)) {
    a.selMixer = pid.owner;
    if (pid.index >= kSlotBase && pid.index < kSendBase) {
      const int slot = static_cast<int>((pid.index - kSlotBase) / kSlotStride);
      a.fxEditor = {pid.owner, slot};
    } else {
      a.showMixer = true;
      a.focusWindow = "Mixer";
    }
  }
}

// ---- the target settings (popover and knob menu) --------------------------
void targetSettingsBody(App& a, ObjectId lfoId, ParamId target, bool inMenu) {
  const auto& P = pal();
  Song const& s = a.doc.song();
  const ModLink* lp = findLfoLink(s, lfoId, target.owner, target.index);
  const SongLfo* L = s.lfo(lfoId);
  if (!lp || !L) {
    ImGui::TextDisabled("%s", tr("This target is gone."));
    return;
  }
  const ModLink l = *lp;
  const uint64_t key = (target.packed() * 31u) ^ (static_cast<uint64_t>(lfoId) << 40) ^ 0x4C464F0000000000ull;
  auto edit = [&](const char* label, const std::function<void(ModLink&)>& fn, uint64_t k = 0) {
    a.doc.editLfoLink(lfoId, target, label, fn, k);
  };
  ImDrawList* dl = ImGui::GetWindowDrawList();
  // Header: the target, the LFO.
  {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const uint32_t oc = lfo::targetColour(s, target);
    const ImU32 dc = oc ? theme::content(oc) : colour(L->colour);
    dl->AddRectFilled(ImVec2(p.x, p.y + 4), ImVec2(p.x + 9, p.y + 13), dc, 2.0f);
    ImGui::SetCursorScreenPos(ImVec2(p.x + 16, p.y));
    ImGui::PushFont(a.fontBold, 0.0f);
    ImGui::TextUnformatted(lfo::targetLabel(s, target).c_str());
    ImGui::PopFont();
    ImGui::SameLine(0, 16);
    ImGui::TextDisabled("%s", lfo::label(s, lfoId).c_str());
    ImGui::Separator();
  }
  const float labelW = 92.0f, rowW = 300.0f;
  auto row = [&](const char* name) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::vec(P.textDim), "%s", tr(name));
    ImGui::SameLine(labelW);
  };
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 6));
  {
    static const char* const kMoves[] = {i18n::N_("Around the knob"), i18n::N_("Between two values")};
    static const char* const kMoveHints[] = {i18n::N_("The LFO swings the knob around where you set it, by the amount"),
                                             i18n::N_("The LFO sweeps the knob between two values; the knob's own setting is ignored")};
    int m = l.mode == kModAround ? 0 : 1;
    row(i18n::N_("Moves"));
    if (ui::Segmented("##moves", kMoves, 2, &m, 0, 0, kMoveHints)) edit("change LFO target mode", [m](ModLink& x) {
        x.mode = m == 0 ? kModAround : kModAbsolute;
      });
  }
  if (l.mode == kModAround) {
    float amt = l.amount * 100.0f;
    row(i18n::N_("Amount"));
    ImGui::SetNextItemWidth(rowW - labelW);
    if (ImGui::SliderFloat("##amount", &amt, -100.0f, 100.0f, "%+.0f%%")) edit("change LFO amount", [amt](ModLink& x) { x.amount = amt / 100.0f; }, key ^ 1);
    if (ImGui::IsItemDeactivated()) a.doc.endGesture();
    static const char* const kDir[] = {i18n::N_("Both ways"), i18n::N_("Up only"), i18n::N_("Down only")};
    int d = l.direction;
    row(i18n::N_("Direction"));
    if (ui::Segmented("##dir", kDir, 3, &d)) edit("change LFO direction", [d](ModLink& x) { x.direction = static_cast<uint8_t>(d); });
  } else {
    float mn = l.min * 100.0f, mx = l.max * 100.0f;
    row(i18n::N_("From"));
    ImGui::SetNextItemWidth(rowW - labelW);
    if (ImGui::SliderFloat("##from", &mn, 0.0f, 100.0f, "%.0f%%")) edit("change LFO range", [mn](ModLink& x) { x.min = mn / 100.0f; }, key ^ 2);
    if (ImGui::IsItemDeactivated()) a.doc.endGesture();
    row(i18n::N_("To"));
    ImGui::SetNextItemWidth(rowW - labelW);
    if (ImGui::SliderFloat("##to", &mx, 0.0f, 100.0f, "%.0f%%")) edit("change LFO range", [mx](ModLink& x) { x.max = mx / 100.0f; }, key ^ 3);
    if (ImGui::IsItemDeactivated()) a.doc.endGesture();
  }
  {
    static const char* const kInv[] = {i18n::N_("Off"), i18n::N_("On")};
    int inv = l.invert ? 1 : 0;
    row(i18n::N_("Invert"));
    if (ui::Segmented("##inv", kInv, 2, &inv)) edit("invert LFO target", [inv](ModLink& x) { x.invert = inv == 1; });
  }
  {
    row(i18n::N_("Curve"));
    if (ui::BeginCombo("##curve", tr(modCurveName(l.curve)), 160)) {
      for (int c = 0; c < static_cast<int>(ModCurve::Formula); ++c)
        if (ImGui::Selectable(tr(modCurveName(static_cast<ModCurve>(c))), c == static_cast<int>(l.curve)))
          edit("change LFO curve", [c](ModLink& x) { x.curve = static_cast<ModCurve>(c); });
      ImGui::EndCombo();
    }
  }
  {
    float sm = l.smoothMs;
    row(i18n::N_("Smoothing"));
    ImGui::SetNextItemWidth(rowW - labelW);
    if (ImGui::SliderFloat("##smooth", &sm, 0.0f, 500.0f, "%.0f ms")) edit("change LFO smoothing", [sm](ModLink& x) { x.smoothMs = sm; }, key ^ 4);
    if (ImGui::IsItemDeactivated()) a.doc.endGesture();
  }
  ImGui::Separator();
  if (ui::TextButton("Show the knob", false, 0, "Opens the window with this control")) {
    showTheKnob(a, target);
    if (!inMenu) ImGui::CloseCurrentPopup();
  }
  ImGui::SameLine();
  if (ui::TextButton(l.enabled ? "Disable" : "Enable", false, 0, "Stops (or restarts) this LFO moving this control; the link stays"))
    edit(l.enabled ? "disable LFO target" : "enable LFO target", [](ModLink& x) { x.enabled = !x.enabled; });
  ImGui::SameLine(rowW - ui::TextButtonWidth("Remove target") + ImGui::GetStyle().WindowPadding.x - 8);
  if (ui::DangerButton("Remove target")) {
    a.doc.removeLfoLink(lfoId, target);
    if (!inMenu) ImGui::CloseCurrentPopup();
  }
  ImGui::PopStyleVar();
}

// ---- the add-target list -----------------------------------------------
struct Cand {
  ParamId pid;
  std::string group, name;
  bool macro;
};

bool matches(const std::string& hay, const char* needle) {
  if (!*needle) return true;
  std::string h = hay, n = needle;
  std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return h.find(n) != std::string::npos;
}

void addTargetBody(App& a, ObjectId lfoId) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  const SongLfo* L = s.lfo(lfoId);
  if (!L) {
    ImGui::CloseCurrentPopup();
    return;
  }
  ImGui::PushFont(a.fontBold, 0.0f);
  ImGui::TextUnformatted(tr("Add a target to"));
  ImGui::PopFont();
  ImGui::SameLine();
  {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + 4), ImVec2(p.x + 9, p.y + 13), colour(L->colour), 2.0f);
    ImGui::SetCursorScreenPos(ImVec2(p.x + 14, p.y));
    ImGui::TextUnformatted(lfo::label(s, lfoId).c_str());
  }
  ImGui::Separator();
  if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
  ImGui::SetNextItemWidth(326);
  ImGui::InputTextWithHint("##find", tr("Find a control"), gAddSearch, sizeof gAddSearch);
  static const char* const kCats[] = {i18n::N_("Everything"), i18n::N_("Channels"), i18n::N_("Mixer"), i18n::N_("Macros"), i18n::N_("Touched")};
  ui::Segmented("##cat", kCats, 5, &gAddCat, 0, 20);
  // The candidates, grouped by owner (and slot) in the song's order.
  std::vector<Cand> cands;
  std::set<uint64_t> already;
  for (const ModLink* m : lfo::linksOf(s, lfoId)) already.insert(ParamId{m->owner, m->index}.packed());
  auto consider = [&](ParamId pid) {
    if (!lfo::canTarget(s, pid) || already.count(pid.packed()) || pid.owner == lfoId) return;
    const bool mac = macro::isMacroParam(pid) && macro::assigned(s, pid.owner, static_cast<int>(pid.index - kLayerMacroBase));
    if (macro::isMacroParam(pid) && !mac) return;  // unassigned macros stay out of the list (Macros: the ones that drive something)
    const bool isCh = s.channel(pid.owner) != nullptr, isMix = s.mixerTrack(pid.owner) != nullptr;
    if (gAddCat == 1 && (!isCh || mac)) return;
    if (gAddCat == 2 && (!isMix || mac)) return;
    if (gAddCat == 3 && !mac) return;
    const std::string full = lfo::targetLabel(s, pid);
    if (!matches(full, gAddSearch)) return;
    const size_t at = full.rfind(" \xC2\xB7 ");
    Cand c{pid, at == std::string::npos ? std::string() : full.substr(0, at), at == std::string::npos ? full : full.substr(at + 4), mac};
    if (mac) c.group = tr("Macros");
    cands.push_back(std::move(c));
  };
  if (gAddCat == 4) {
    for (ParamId p : gTouched) consider(p);
  } else {
    for (ParamId p : listParams(s)) consider(p);
  }
  ImGui::BeginChild("##tree", ImVec2(326, 260), ImGuiChildFlags_None);
  if (cands.empty()) ImGui::TextDisabled("%s", gAddCat == 4 ? tr("Move a knob, then pick it here.") : tr("Nothing matches."));
  std::string group = "\x01";
  bool groupOpen = true;
  const bool filtering = gAddSearch[0] != 0 || gAddCat == 4;
  int shown = 0;
  for (const Cand& c : cands) {
    if (c.group != group) {
      if (group != "\x01" && groupOpen) ImGui::TreePop();
      group = c.group;
      const uint32_t oc = lfo::targetColour(s, c.pid);
      const ImVec2 p = ImGui::GetCursorScreenPos();
      if (filtering) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
      groupOpen = ImGui::TreeNodeEx(group.empty() ? "##g" : group.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth);
      if (oc || c.macro) {
        const ImU32 dc = c.macro ? colour(6) : theme::content(oc);
        const float tx = p.x + ImGui::GetTreeNodeToLabelSpacing() - 13;
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(tx + 4, p.y + ImGui::GetFrameHeight() * 0.5f), 3.5f, dc, 10);
      }
    }
    if (!groupOpen) continue;
    if (++shown > 400) break;
    ImGui::PushID(static_cast<int>(c.pid.packed() & 0x7FFFFFFF));
    std::string val;
    if (auto d = describeParam(s, c.pid))
      if (auto v = getParam(s, c.pid)) val = c.macro ? tr("macro") : d->format(*v);
    if (ImGui::Selectable(c.name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
      if (!lfo::link(a.doc, lfoId, c.pid)) a.notify(a.doc.lastLinkRefusal().empty() ? tr("This control cannot take an LFO") : a.doc.lastLinkRefusal(), true);
      else ImGui::CloseCurrentPopup();
    }
    if (!val.empty()) {
      ImGui::SameLine(326 - 16 - ImGui::CalcTextSize(val.c_str()).x);
      ImGui::TextColored(theme::vec(P.textFaint), "%s", val.c_str());
    }
    ImGui::PopID();
  }
  if (group != "\x01" && groupOpen) ImGui::TreePop();
  ImGui::EndChild();
  ImGui::Separator();
  {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 c = colour(L->colour);
    const ImVec2 cc(p.x + 8, p.y + ImGui::GetTextLineHeight() * 0.5f);
    dl->AddCircle(cc, 5.5f, c, 16, 1.5f);
    dl->AddCircleFilled(cc, 1.8f, c, 8);
    ImGui::SetCursorScreenPos(ImVec2(p.x + 20, p.y));
    ImGui::TextColored(theme::vec(P.textDim), "%s", tr("Or drag the LFO's handle onto any knob"));
    ImGui::SameLine(0, 12);
    if (ui::TextButton("Pick on screen", false, 0, "The next knob you click anywhere becomes a target (Esc cancels)")) {
      gPickLfo = L->id;
      ImGui::CloseCurrentPopup();
    }
  }
}

// ---- one LFO's menu (the card's "...") ------------------------------------
void lfoMenuItems(App& a, ObjectId id) {
  const Song& s = a.doc.song();
  const SongLfo* L = s.lfo(id);
  if (!L) return;
  if (ImGui::MenuItem(tr("Rename..."))) {
    gRename = id;
    std::snprintf(gRenameBuf, sizeof gRenameBuf, "%s", L->name.c_str());
  }
  if (ImGui::MenuItem(L->on ? tr("Turn off") : tr("Turn on"))) a.doc.editLfo(id, L->on ? "turn LFO off" : "turn LFO on", [](SongLfo& x) { x.on = !x.on; });
  if (ImGui::MenuItem(tr("Duplicate"), nullptr, false, static_cast<int>(s.lfos.size()) < kMaxSongLfos)) {
    SongLfo copy = *L;
    copy.id = kNoId;
    copy.colour = lfo::nextColour(s);
    const int at = s.lfoIndex(id) + 1;
    if (ObjectId nid = a.doc.addLfo(copy, at, "duplicate LFO"); nid != kNoId) gSelected = nid;
  }
  if (ImGui::BeginMenu(tr("Colour"))) {
    for (int k = 0; k < kLfoColours; ++k) {
      ImGui::PushID(k);
      const ImVec2 p = ImGui::GetCursorScreenPos();
      if (ImGui::Selectable("##c", L->colour == k, 0, ImVec2(18, 18))) a.doc.editLfo(id, "change LFO colour", [k](SongLfo& x) { x.colour = k; });
      ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x + 2, p.y + 2), ImVec2(p.x + 16, p.y + 16), colour(k), 3.0f);
      if (k + 1 < kLfoColours) ImGui::SameLine();
      ImGui::PopID();
    }
    ImGui::EndMenu();
  }
  if (L->kind == LfoKind::Lfo && ImGui::BeginMenu(tr("Apply a preset"))) {  // (the presets are LFOs)
    for (const lfo::Preset& p : lfo::presets())
      if (ImGui::MenuItem(tr(p.name))) a.doc.editLfo(id, "apply LFO preset", [&p](SongLfo& x) { lfo::applyPreset(x, p.lfo); });
    ImGui::EndMenu();
  }
  ImGui::Separator();
  if (ImGui::MenuItem(tr("Up only (the knob is the bottom of the swing)"), nullptr, L->unipolar))
    a.doc.editLfo(id, "change LFO polarity", [](SongLfo& x) { x.unipolar = !x.unipolar; });
  if (L->kind != LfoKind::Lfo) {  // 0.7.8: Chaos and Random
    if (L->kind == LfoKind::Random) {
      if (L->trigger == RandomTrigger::Roll && ImGui::MenuItem(tr("Roll a new value"))) lfo::roll(a.doc, id);
      if (ImGui::MenuItem(tr("Values gather round the middle"), nullptr, L->centred))
        a.doc.editLfo(id, "change random distribution", [](SongLfo& x) { x.centred = !x.centred; });
      ui::Hint("Off: every value is as likely as any other. On: values near the middle come more often");
      if (ImGui::BeginMenu(tr("Snap to steps"))) {
        if (ImGui::MenuItem(tr("Off"), nullptr, L->quantise < 2)) a.doc.editLfo(id, "change random steps", [](SongLfo& x) { x.quantise = 0; });
        for (int n : {2, 3, 4, 5, 6, 8, 12, 16}) {
          char b[24];
          std::snprintf(b, sizeof b, tr("%d steps"), n);
          if (ImGui::MenuItem(b, nullptr, L->quantise == n)) a.doc.editLfo(id, "change random steps", [n](SongLfo& x) { x.quantise = n; });
        }
        ImGui::EndMenu();
      }
    }
    int seed = static_cast<int>(L->seed % 1000000u);
    ImGui::SetNextItemWidth(180);
    if (ImGui::DragInt(tr("Seed"), &seed, 1.0f, 0, 999999, "%d", ImGuiSliderFlags_AlwaysClamp))
      a.doc.editLfo(id, "change seed", [seed](SongLfo& x) { x.seed = static_cast<uint32_t>(seed); }, (static_cast<uint64_t>(id) << 8) ^ 0x55);
    if (ImGui::IsItemDeactivated()) a.doc.endGesture();
    ui::Hint("The same seed always plays the same movement, on every computer and in every export");
    if (ImGui::MenuItem(tr("New seed"))) a.doc.editLfo(id, "new seed", [](SongLfo& x) { x.seed = (x.seed * 1664525u + 1013904223u) % 1000000u; });
  }
  if (L->kind == LfoKind::Lfo && L->shape == LfoShape::Square) {
    float pw = L->pulseWidth * 100.0f;
    ImGui::SetNextItemWidth(180);
    if (ImGui::SliderFloat(tr("Pulse width"), &pw, 5.0f, 95.0f, "%.0f%%"))
      a.doc.editLfo(id, "change LFO pulse width", [pw](SongLfo& x) { x.pulseWidth = pw / 100.0f; }, (static_cast<uint64_t>(id) << 8) ^ 0x51);
    if (ImGui::IsItemDeactivated()) a.doc.endGesture();
  }
  if (L->kind == LfoKind::Lfo && (L->shape == LfoShape::RandomHold || L->shape == LfoShape::RandomGlide)) {
    if (ImGui::BeginMenu(tr("Steps per cycle"))) {
      for (int n : {1, 2, 4, 8, 16}) {
        char b[8];
        std::snprintf(b, sizeof b, "%d", n);
        if (ImGui::MenuItem(b, nullptr, L->randomSteps == n)) a.doc.editLfo(id, "change LFO steps", [n](SongLfo& x) { x.randomSteps = n; });
      }
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem(tr("New random values"))) a.doc.editLfo(id, "new LFO random values", [](SongLfo& x) { x.seed = x.seed * 1664525u + 1013904223u; });
  }
  if (L->kind != LfoKind::Random) {
    float fade = L->fadeIn;
    ImGui::SetNextItemWidth(180);
    if (ImGui::SliderFloat(tr("Fade in"), &fade, 0.0f, 8.0f, L->tempoSync ? "%.1f bars" : "%.1f s"))
      a.doc.editLfo(id, "change LFO fade in", [fade](SongLfo& x) { x.fadeIn = fade; }, (static_cast<uint64_t>(id) << 8) ^ 0x52);
    if (ImGui::IsItemDeactivated()) a.doc.endGesture();
  }
  const bool notes = L->kind == LfoKind::Random ? L->trigger == RandomTrigger::Note : L->restart == LfoRestart::EachNote;
  if (notes && ImGui::BeginMenu(L->kind == LfoKind::Random ? tr("A new value on the notes of") : tr("Restart on the notes of"))) {
    if (ImGui::MenuItem(tr("Every channel"), nullptr, L->noteChannel == kNoId))
      a.doc.editLfo(id, "change LFO note channel", [](SongLfo& x) { x.noteChannel = kNoId; });
    for (const Channel& c : s.channels)
      if (ImGui::MenuItem(c.name.c_str(), nullptr, L->noteChannel == c.id))
        a.doc.editLfo(id, "change LFO note channel", [cid = c.id](SongLfo& x) { x.noteChannel = cid; });
    ImGui::EndMenu();
  }
  ImGui::Separator();
  if (ImGui::MenuItem(tr("Edit in a window"))) gSelected = id, a.showLfoDetail = true;
  const int idx = s.lfoIndex(id);
  if (ImGui::MenuItem(tr("Move earlier"), nullptr, false, idx > 0)) a.doc.moveLfo(id, idx - 1);
  if (ImGui::MenuItem(tr("Move later"), nullptr, false, idx + 1 < static_cast<int>(s.lfos.size()))) a.doc.moveLfo(id, idx + 1);
  ImGui::Separator();
  if (ImGui::MenuItem(L->kind == LfoKind::Chaos ? tr("Remove chaos") : L->kind == LfoKind::Random ? tr("Remove random") : tr("Remove LFO"))) {
    a.doc.removeLfo(id);
    if (gSelected == id) gSelected = kNoId;
  }
}

// The rename field over a name (Enter keeps, Esc drops).
bool renameField(App& a, ObjectId id, ImVec2 p, float w) {
  if (gRename != id) return false;
  ImGui::SetCursorScreenPos(p);
  ImGui::SetNextItemWidth(w);
  if (ImGui::IsWindowAppearing() || !ImGui::IsAnyItemActive()) ImGui::SetKeyboardFocusHere();
  const bool done = ImGui::InputText("##rename", gRenameBuf, sizeof gRenameBuf, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
  if (done) {
    const std::string nm = gRenameBuf;
    a.doc.editLfo(id, "rename LFO", [nm](SongLfo& x) { x.name = nm; });
    gRename = kNoId;
  } else if (ImGui::IsKeyPressed(ImGuiKey_Escape) || (ImGui::IsItemDeactivated() && !done)) {
    gRename = kNoId;
  }
  return true;
}

// The rate control: a note value menu (Tempo) or a frequency field (Hz).
void rateControl(App& a, const SongLfo& l, ImVec2 p, float w, float h) {
  ImGui::SetCursorScreenPos(p);
  if (l.kind == LfoKind::Random && (l.trigger == RandomTrigger::Note || l.trigger == RandomTrigger::Roll)) {
    // No clock: Roll draws a value now; Note names the notes it follows (the menu picks the channel).
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, std::max(0.0f, (h - ImGui::GetFontSize()) * 0.5f)));
    if (l.trigger == RandomTrigger::Roll) {
      if (ui::TextButton("Roll", false, w, "A new random value now (one undo step; everyone in a shared session gets the same)")) lfo::roll(a.doc, l.id);
    } else {
      const Channel* ch = a.doc.song().channel(l.noteChannel);
      const std::string lb = ch ? ch->name : std::string(tr("Every note"));
      if (ui::TextButton(fit(ImGui::GetFont(), ImGui::GetFontSize(), lb.c_str(), w - 12), false, w, "The notes that draw a new value: click to pick the channel"))
        ImGui::OpenPopup("##notech");
      if (ImGui::BeginPopup("##notech")) {
        if (ImGui::MenuItem(tr("Every channel"), nullptr, l.noteChannel == kNoId))
          a.doc.editLfo(l.id, "change LFO note channel", [](SongLfo& x) { x.noteChannel = kNoId; });
        for (const Channel& c : a.doc.song().channels)
          if (ImGui::MenuItem(c.name.c_str(), nullptr, l.noteChannel == c.id))
            a.doc.editLfo(l.id, "change LFO note channel", [cid = c.id](SongLfo& x) { x.noteChannel = cid; });
        ImGui::EndPopup();
      }
    }
    ImGui::PopStyleVar();
    return;
  }
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, std::max(0.0f, (h - ImGui::GetFontSize()) * 0.5f)));
  ImGui::PushFont(monoFont(a), 0.0f);
  if (l.tempoSync) {
    const std::string cur = lfo::rateLabel(l);
    ImGui::SetNextItemWidth(w);
    if (ImGui::BeginCombo("##rate", cur.c_str(), ImGuiComboFlags_HeightLarge)) {
      static const char* const kKinds[] = {"", " D", " T"};
      for (int kind = 0; kind < 3; ++kind) {
        if (kind) ImGui::Separator();
        for (int n = 0; n < lfo::kRateCount; ++n) {
          const std::string lb = std::string(lfo::kRateNames[n]) + kKinds[kind];
          if (ImGui::Selectable(lb.c_str(), l.rateNote == n && l.rateKind == kind))
            a.doc.editLfo(l.id, "change LFO rate", [n, kind](SongLfo& x) { x.rateNote = n, x.rateKind = kind; });
        }
      }
      ImGui::EndCombo();
    }
  } else {
    float hz = l.rateHz;
    ImGui::SetNextItemWidth(w);
    if (ImGui::DragFloat("##hz", &hz, 0.01f, lfo::kMinHz, lfo::kMaxHz, "%.2f Hz", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp))
      a.doc.editLfo(l.id, "change LFO rate", [hz](SongLfo& x) { x.rateHz = hz; }, (static_cast<uint64_t>(l.id) << 8) ^ 0x53);
    if (ImGui::IsItemDeactivated()) a.doc.endGesture();
  }
  ImGui::PopFont();
  ImGui::PopStyleVar();
}

// The card's knobs (five for an LFO; 0.7.8: Chaos's and Random's own) at y, across [x0, x1]. Where the
// Depth knob's centre is goes to `depthX` (dev).
struct CardKnob {
  const char* name;
  lfo::LfoParam p;
  bool bipolar;
};
int knobsOf(const SongLfo& l, CardKnob* ks) {
  const lfo::LfoParam rate = l.tempoSync ? lfo::LfoParam::RateNote : lfo::LfoParam::RateHz;
  int n = 0;
  if (l.kind == LfoKind::Chaos) {
    ks[n++] = {i18n::N_("Rate"), rate, false};
    ks[n++] = {i18n::N_("Depth"), lfo::LfoParam::Depth, false};
    ks[n++] = {i18n::N_("Character"), lfo::LfoParam::Character, false};
    ks[n++] = {i18n::N_("Smooth"), lfo::LfoParam::Smooth, false};
    ks[n++] = {i18n::N_("Offset"), lfo::LfoParam::Offset, true};
  } else if (l.kind == LfoKind::Random) {
    if (l.trigger == RandomTrigger::Tempo) ks[n++] = {i18n::N_("Rate"), lfo::LfoParam::RateNote, false};
    if (l.trigger == RandomTrigger::Hz) ks[n++] = {i18n::N_("Rate"), lfo::LfoParam::RateHz, false};
    ks[n++] = {i18n::N_("Depth"), lfo::LfoParam::Depth, false};
    ks[n++] = {i18n::N_("Glide"), lfo::LfoParam::Smooth, false};
    ks[n++] = {i18n::N_("Offset"), lfo::LfoParam::Offset, true};
  } else {
    ks[n++] = {i18n::N_("Rate"), rate, false};
    ks[n++] = {i18n::N_("Phase"), lfo::LfoParam::Phase, false};
    ks[n++] = {i18n::N_("Depth"), lfo::LfoParam::Depth, false};
    ks[n++] = {i18n::N_("Offset"), lfo::LfoParam::Offset, true};
    ks[n++] = {i18n::N_("Smooth"), lfo::LfoParam::Smooth, false};
  }
  return n;
}
void cardKnobs(App& a, const SongLfo& l, float x0, float x1, float y, float r, float* depthX = nullptr) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  CardKnob ks[5];
  const int count = knobsOf(l, ks);
  const float cell = (x1 - x0) / 5.0f;  // (fewer knobs keep the same spacing, centred)
  x0 += (x1 - x0 - cell * static_cast<float>(count)) * 0.5f;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fs = fsLabel();
  for (int i = 0; i < count; ++i) {
    const float cx = std::round(x0 + cell * (static_cast<float>(i) + 0.5f));
    const ParamId pid = lfo::lfoParam(l.id, ks[i].p);
    if (depthX && ks[i].p == lfo::LfoParam::Depth) *depthX = cx;
    ImGui::SetCursorScreenPos(ImVec2(cx - r, y));
    ImGui::PushID(i);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, theme::vec(colour(l.colour)));
    ui::ParamKnob("##k", pid, r, nullptr, ks[i].bipolar);
    ImGui::PopStyleColor();
    ImGui::PopID();
    const char* nm = tr(ks[i].name);
    text(dl, ImGui::GetFont(), fs, ImVec2(cx - textW(ImGui::GetFont(), fs, nm) * 0.5f, y + r * 2 + 3), P.textDim, nm);
    std::string val;
    if (ks[i].p == lfo::LfoParam::RateNote) val = lfo::rateLabel(l);
    else if (auto d = describeParam(s, pid))
      if (auto v = getParam(s, pid)) {
        char b[32];
        if (ks[i].p == lfo::LfoParam::Phase) std::snprintf(b, sizeof b, "%d\xC2\xB0", static_cast<int>(std::lround(*v)));
        else if (ks[i].p == lfo::LfoParam::Depth || ks[i].p == lfo::LfoParam::Character) std::snprintf(b, sizeof b, "%d%%", static_cast<int>(std::lround(*v * 100)));
        else if (ks[i].p == lfo::LfoParam::Offset) std::snprintf(b, sizeof b, "%+d%%", static_cast<int>(std::lround(*v * 100)));
        else if (ks[i].p == lfo::LfoParam::Smooth) std::snprintf(b, sizeof b, "%d ms", static_cast<int>(std::lround(*v)));
        else std::snprintf(b, sizeof b, "%.2f Hz", *v);
        val = std::strcmp(b, "+0%") == 0 ? "0%" : b;
      }
    ImFont* mf = monoFont(a);
    text(dl, mf, fs - 1, ImVec2(cx - textW(mf, fs - 1, val.c_str()) * 0.5f, y + r * 2 + 3 + fs + 1), P.text, val.c_str());
  }
}

// ---- the card ----------------------------------------------------------
constexpr float kHeadH = 32, kWaveH = 74, kShapeH = 22, kRowH = 20, kKnobR = 15, kChipH = 22, kChipGap = 5;

struct ChipLay {
  ParamId pid;
  std::string label, amt;
  float w;
  ImU32 dot;
  bool round, dim;
};

std::vector<ChipLay> chipsOf(App& a, const SongLfo& l) {
  const Song& s = a.doc.song();
  std::vector<ChipLay> v;
  for (const ModLink* m : lfo::linksOf(s, l.id)) {
    const ParamId pid{m->owner, m->index};
    ChipLay c;
    c.pid = pid;
    c.label = lfo::targetLabel(s, pid);
    c.amt = amountText(*m);
    c.round = macro::isMacroParam(pid);
    c.dot = c.round ? colour(6) : colour(l.colour);
    c.dim = !m->enabled;
    c.w = chipWidth(a, c.label.c_str(), c.amt.c_str(), true);
    v.push_back(std::move(c));
  }
  return v;
}

// Rows the chips (and the add chip) take in `w`.
int chipRows(App& a, const std::vector<ChipLay>& chips, float w) {
  float x = 0;
  int rows = 1;
  const float addW = chipWidth(a, tr("+ Add target"), "", false);
  auto place = [&](float cw) {
    cw = std::min(cw, w);
    if (x > 0 && x + cw > w) ++rows, x = 0;
    x += cw + kChipGap;
  };
  for (const ChipLay& c : chips) place(c.w);
  place(addW);
  return rows;
}

float cardHeight(App& a, const SongLfo& l, float w) {
  const int rows = chipRows(a, chipsOf(a, l), w - 20);
  return kHeadH + 8 + kWaveH + 7 + kShapeH + 7 + kRowH + 8 + kKnobR * 2 + 3 + fsLabel() * 2 + 6 + 8 + 16 + static_cast<float>(rows) * (kChipH + kChipGap) + 6;
}

// 0.7.8: the two rows a Chaos or Random source has where an LFO has its shapes and its rate (from y
// across [w0.x, w1.x]; y moves past them as the LFO's rows do).
//   Chaos:  [Free Play Note Bar]            [+- +]      Random: [Tempo Hz Note Roll]          [+- +]
//           [Hz Tempo] [rate]        [seed 1234]               [rate or Roll] [Even Middle] [steps]
const char* const kPolar[] = {"\xC2\xB1", "+"};
const char* const kPolarHints[] = {i18n::N_("Both ways: the knob moves up and down from where it is"), i18n::N_("Up only: the knob is the bottom of the swing")};
void polarity(App& a, const SongLfo& l, ImVec2 at, float h) {
  int up = l.unipolar ? 1 : 0;
  if (miniSeg(a, "##polar", kPolar, 2, &up, at, h, nullptr, kPolarHints))
    a.doc.editLfo(l.id, "change LFO polarity", [up](SongLfo& q) { q.unipolar = up == 1; });
}
void seedField(App& a, const SongLfo& l, ImVec2 at, float w, float h) {
  ImGui::SetCursorScreenPos(at);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, std::max(0.0f, (h - ImGui::GetFontSize()) * 0.5f)));
  ImGui::PushFont(monoFont(a), 0.0f);
  int seed = static_cast<int>(l.seed % 1000000u);
  ImGui::SetNextItemWidth(w);
  char fmt[32];
  std::snprintf(fmt, sizeof fmt, "%s %%d", tr("seed"));
  if (ImGui::DragInt("##seed", &seed, 0.5f, 0, 999999, fmt, ImGuiSliderFlags_AlwaysClamp))
    a.doc.editLfo(l.id, "change seed", [seed](SongLfo& q) { q.seed = static_cast<uint32_t>(seed); }, (static_cast<uint64_t>(l.id) << 8) ^ 0x55);
  if (ImGui::IsItemDeactivated()) a.doc.endGesture();
  if (ImGui::IsItemHovered()) ui::Hint("The seed: drag or double-click to type. The same seed always moves the same way, on every computer");
  ImGui::PopFont();
  ImGui::PopStyleVar();
}
float polarWidth(App& a) { return miniSegWidth(a, kPolar, 2); }
void sourceRows(App& a, const SongLfo& l, ImVec2 w0, ImVec2 w1, float& y) {
  const float pw = polarWidth(a);
  if (l.kind == LfoKind::Chaos) {
    static const char* const kRestart[] = {i18n::N_("Free"), i18n::N_("Play"), i18n::N_("Note"), i18n::N_("Bar")};
    static const char* const kRestartHints[] = {i18n::N_("Runs freely, never restarted"),
                                                i18n::N_("Locked to the song: the same place in the song is the same place on the walk"),
                                                i18n::N_("Starts again on every note of a channel (the card's menu picks which)"),
                                                i18n::N_("Starts again at every bar: each bar moves the same way")};
    int rs = static_cast<int>(l.restart);
    if (miniSeg(a, "##restart", kRestart, 4, &rs, ImVec2(w0.x, y), kShapeH, nullptr, kRestartHints))
      a.doc.editLfo(l.id, "change LFO restart", [rs, ch = a.selChannel](SongLfo& q) {
        q.restart = static_cast<LfoRestart>(rs);
        if (q.restart == LfoRestart::EachNote && q.noteChannel == kNoId) q.noteChannel = ch;
      });
    polarity(a, l, ImVec2(w1.x - pw, y), kShapeH);
    y += kShapeH + 7;
    static const char* const kSync[] = {i18n::N_("Hz"), i18n::N_("Tempo")};
    static const char* const kSyncHints[] = {i18n::N_("The rate in bends a second"), i18n::N_("The rate in note values, following the tempo")};
    int sync = l.tempoSync ? 1 : 0;
    float sw = 0;
    if (miniSeg(a, "##sync", kSync, 2, &sync, ImVec2(w0.x, y), kRowH, &sw, kSyncHints))
      a.doc.editLfo(l.id, "change LFO rate mode", [sync](SongLfo& q) { q.tempoSync = sync == 1; });
    rateControl(a, l, ImVec2(w0.x + sw + 6, y), 76, kRowH);
    const float seedW = std::min(96.0f, w1.x - (w0.x + sw + 6 + 76 + 8));
    if (seedW >= 60) seedField(a, l, ImVec2(w1.x - seedW, y), seedW, kRowH);
    return;
  }
  // Random.
  static const char* const kTrig[] = {i18n::N_("Tempo"), i18n::N_("Hz"), i18n::N_("Note"), i18n::N_("Roll")};
  static const char* const kTrigHints[] = {i18n::N_("A new value every note value of the rate, locked to the song"),
                                           i18n::N_("A new value at a rate in times a second"),
                                           i18n::N_("A new value on every note of a channel"),
                                           i18n::N_("A new value only when you press Roll")};
  int tg = static_cast<int>(l.trigger);
  if (miniSeg(a, "##trigger", kTrig, 4, &tg, ImVec2(w0.x, y), kShapeH, nullptr, kTrigHints))
    a.doc.editLfo(l.id, "change random trigger", [tg, ch = a.selChannel](SongLfo& q) {
      q.trigger = static_cast<RandomTrigger>(tg);
      q.tempoSync = q.trigger != RandomTrigger::Hz;  // (the rate control and knob follow it)
      if (q.trigger == RandomTrigger::Note && q.noteChannel == kNoId) q.noteChannel = ch;
    });
  polarity(a, l, ImVec2(w1.x - pw, y), kShapeH);
  y += kShapeH + 7;
  rateControl(a, l, ImVec2(w0.x, y), 76, kRowH);
  // The steps it snaps to, at the right; the spread between when there is room.
  const float qw = 78;
  ImGui::SetCursorScreenPos(ImVec2(w1.x - qw, y));
  {
    char cur[24];
    if (l.quantise >= 2) std::snprintf(cur, sizeof cur, tr("%d steps"), l.quantise);
    else std::snprintf(cur, sizeof cur, "%s", tr("No steps"));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, std::max(0.0f, (kRowH - ImGui::GetFontSize()) * 0.5f)));
    ImGui::PushFont(monoFont(a), 0.0f);
    ImGui::SetNextItemWidth(qw);
    if (ImGui::BeginCombo("##quant", cur, ImGuiComboFlags_HeightLarge)) {
      if (ImGui::Selectable(tr("No steps"), l.quantise < 2)) a.doc.editLfo(l.id, "change random steps", [](SongLfo& q) { q.quantise = 0; });
      for (int n : {2, 3, 4, 5, 6, 8, 12, 16}) {
        char b[24];
        std::snprintf(b, sizeof b, tr("%d steps"), n);
        if (ImGui::Selectable(b, l.quantise == n)) a.doc.editLfo(l.id, "change random steps", [n](SongLfo& q) { q.quantise = n; });
      }
      ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ui::Hint("Snap every value to this many even steps across the range");
    ImGui::PopFont();
    ImGui::PopStyleVar();
  }
  static const char* const kDist[] = {i18n::N_("Even"), i18n::N_("Middle")};
  static const char* const kDistHints[] = {i18n::N_("Every value is as likely as any other"), i18n::N_("Values near the middle come more often")};
  const float dw = miniSegWidth(a, kDist, 2);
  const float dx = std::round((w0.x + 76 + w1.x - qw - dw) * 0.5f);
  if (dx >= w0.x + 76 + 6 && dx + dw <= w1.x - qw - 6) {
    int c = l.centred ? 1 : 0;
    if (miniSeg(a, "##dist", kDist, 2, &c, ImVec2(dx, y), kRowH, nullptr, kDistHints))
      a.doc.editLfo(l.id, "change random distribution", [c](SongLfo& q) { q.centred = c == 1; });
  }
}

void drawCard(App& a, const SongLfo& l, ImVec2 mn, ImVec2 mx) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImU32 c = colour(l.colour);
  const bool sel = gSelected == l.id;
  const bool tl = trackerLook();
  DevCard dev{l.id, mn, mx, {}, {}, {}, {}};
  ImGui::PushID(static_cast<int>(l.id));
  // The frame (a click anywhere on it selects the LFO).
  ImGui::SetCursorScreenPos(mn);
  ImGui::SetNextItemAllowOverlap();
  if (ImGui::InvisibleButton("##card", ImVec2(mx.x - mn.x, mx.y - mn.y))) gSelected = l.id;
  if (tl) {
    dl->AddRectFilled(mn, mx, P.surface900);
    trackerview::bevelBox(dl, mn, mx, false);
    if (sel) dl->AddRect(mn, mx, P.accent, 0, 0, 1.0f);
  } else {
    dl->AddRectFilled(mn, mx, P.well, 6.0f);
    dl->AddRect(mn, mx, sel ? P.accent : P.line, 6.0f, 0, 1.0f);
    if (sel) dl->AddRect(ImVec2(mn.x - 1, mn.y - 1), ImVec2(mx.x + 1, mx.y + 1), theme::withAlpha(P.accent, 0.35f), 7.0f);
  }
  // Header: the colour bar, on lamp, number, name, shape, the handle and the menu.
  const float hy = mn.y, hb = mn.y + kHeadH;
  dl->AddRectFilled(ImVec2(mn.x, hy), ImVec2(mn.x + 4, hb), c, tl ? 0.0f : 6.0f, ImDrawFlags_RoundCornersTopLeft);
  dl->AddLine(ImVec2(mn.x + 1, hb - 0.5f), ImVec2(mx.x - 1, hb - 0.5f), P.line);
  float x = mn.x + 12;
  {
    ImGui::SetCursorScreenPos(ImVec2(x - 3, hy + kHeadH * 0.5f - 7));
    if (ImGui::InvisibleButton("##on", ImVec2(15, 14))) a.doc.editLfo(l.id, l.on ? "turn LFO off" : "turn LFO on", [](SongLfo& q) { q.on = !q.on; });
    if (ImGui::IsItemHovered()) ui::Hint(l.on ? "On: click to stop this LFO (every target keeps its own value)" : "Off: click to start this LFO");
    ui::Led(dl, ImVec2(x + 4.5f, hy + kHeadH * 0.5f), 4.5f, l.on);
    x += 17;
  }
  const std::string num = lfo::number(s, l.id);
  ImGui::PushFont(a.fontBold, 0.0f);
  const float fs = ImGui::GetFontSize() - 1.5f;
  text(dl, a.fontBold, fs, ImVec2(x, hy + (kHeadH - fs) * 0.5f), P.text, num.c_str());
  x += textW(a.fontBold, fs, num.c_str()) + 7;
  ImGui::PopFont();
  const float handleX = mx.x - 8 - 22 - 4 - 24;
  if (!renameField(a, l.id, ImVec2(x, hy + 5), std::max(40.0f, handleX - x - 8))) {
    const std::string nm = l.name;
    const float shapeW = textW(monoFont(a), fsLabel() - 1, tr(lfo::kindSummary(l)));
    const char* shown = fit(ImGui::GetFont(), fs, nm.c_str(), std::max(10.0f, handleX - x - shapeW - 20));
    text(dl, ImGui::GetFont(), fs, ImVec2(x, hy + (kHeadH - fs) * 0.5f), P.textDim, shown);
    ImGui::SetCursorScreenPos(ImVec2(x, hy + 4));
    const float nw = std::max(30.0f, textW(ImGui::GetFont(), fs, shown));
    ImGui::InvisibleButton("##name", ImVec2(nw, kHeadH - 8));
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      gRename = l.id;
      std::snprintf(gRenameBuf, sizeof gRenameBuf, "%s", l.name.c_str());
    }
    if (ImGui::IsItemClicked()) gSelected = l.id;
    if (ImGui::IsItemHovered()) ui::Hint("Double-click to rename");
    x += nw + 8;
    text(dl, monoFont(a), fsLabel() - 1, ImVec2(x, hy + (kHeadH - fsLabel() + 1) * 0.5f), P.textFaint, tr(lfo::kindSummary(l)));
  }
  handle(a, l, ImVec2(handleX, hy + 5), ImVec2(24, 22), &dev.handle);
  {
    const ImVec2 mp(mx.x - 8 - 22, hy + 5);
    ImGui::SetCursorScreenPos(mp);
    if (ImGui::InvisibleButton("##menu", ImVec2(22, 22))) ImGui::OpenPopup("##lfomenu");
    const bool hov = ImGui::IsItemHovered();
    if (hov) dl->AddRectFilled(mp, ImVec2(mp.x + 22, mp.y + 22), theme::withAlpha(P.text, 0.08f), 4.0f);
    for (int k = -1; k <= 1; ++k) dl->AddCircleFilled(ImVec2(mp.x + 11 + static_cast<float>(k) * 4.5f, mp.y + 11), 1.4f, hov ? P.text : P.textDim, 6);
    if (ImGui::BeginPopup("##lfomenu")) {
      lfoMenuItems(a, l.id);
      ImGui::EndPopup();
    }
  }
  if (!s.lfo(l.id)) {  // removed from its own menu
    ImGui::PopID();
    return;
  }
  // The wave.
  float y = hb + 8;
  const ImVec2 w0(mn.x + 10, y), w1(mx.x - 10, y + kWaveH);
  dl->AddRectFilled(w0, w1, P.surface950, tl ? 0.0f : 4.0f);
  dl->AddRect(w0, w1, P.line, tl ? 0.0f : 4.0f);
  if (tl) trackerview::bevelBox(dl, w0, w1, true);
  drawWave(a, dl, ImVec2(w0.x + 1, w0.y + 1), ImVec2(w1.x - 1, w1.y - 1), l, 2.0f, true, true);
  {
    const std::string rl = lfo::rateLabel(l);
    ImFont* mf = monoFont(a);
    text(dl, mf, fsLabel() - 1.5f, ImVec2(w1.x - 6 - textW(mf, fsLabel() - 1.5f, rl.c_str()), w0.y + 4), P.textFaint, rl.c_str());
  }
  y += kWaveH + 7;
  if (l.kind != LfoKind::Lfo) {
    sourceRows(a, l, w0, w1, y);  // 0.7.8: Chaos's and Random's two rows
  } else {
    // The shapes.
    {
      const float sw = (w1.x - w0.x - 3 * 7) / 8.0f;
      for (int k = 0; k < static_cast<int>(LfoShape::Count); ++k) {
        const ImVec2 s0(std::round(w0.x + static_cast<float>(k) * (sw + 3)), y), s1(std::round(w0.x + static_cast<float>(k) * (sw + 3) + sw), y + kShapeH);
        ImGui::SetCursorScreenPos(s0);
        ImGui::PushID(k);
        const LfoShape sh = static_cast<LfoShape>(k);
        if (ImGui::InvisibleButton("##shape", ImVec2(s1.x - s0.x, kShapeH)) && l.shape != sh) {
          a.doc.editLfo(l.id, "change LFO shape", [sh](SongLfo& q) {
            q.shape = sh;
            if (sh == LfoShape::Drawn && q.points.empty()) q.points = lfo::defaultPoints();
          });
          if (sh == LfoShape::Drawn) gSelected = l.id;
        }
        const bool hov = ImGui::IsItemHovered();
        if (hov) ui::Hint(lfo::shapeName(sh));
        ImGui::PopID();
        const bool on = l.shape == sh;
        dl->AddRectFilled(s0, s1, on ? theme::mix(P.field, P.text, 0.04f) : hov ? theme::mix(P.field, P.text, 0.05f) : P.field, tl ? 0.0f : 3.0f);
        dl->AddRect(s0, s1, on ? c : P.line, tl ? 0.0f : 3.0f);
        const float gw = std::min(22.0f, s1.x - s0.x - 6), gh = 11;
        const ImVec2 g0((s0.x + s1.x - gw) * 0.5f, (s0.y + s1.y - gh) * 0.5f);
        shapeGlyph(dl, g0, ImVec2(g0.x + gw, g0.y + gh), sh, on ? c : P.textFaint);
      }
    }
    y += kShapeH + 7;
    // Hz / Tempo, the rate, Free / Play / Note.
    {
      static const char* const kSync[] = {i18n::N_("Hz"), i18n::N_("Tempo")};
      static const char* const kSyncHints[] = {i18n::N_("The rate in cycles a second"), i18n::N_("The rate in note values, following the tempo")};
      int sync = l.tempoSync ? 1 : 0;
      float sw = 0;
      if (miniSeg(a, "##sync", kSync, 2, &sync, ImVec2(w0.x, y), kRowH, &sw, kSyncHints))
        a.doc.editLfo(l.id, "change LFO rate mode", [sync](SongLfo& q) { q.tempoSync = sync == 1; });
      rateControl(a, l, ImVec2(w0.x + sw + 6, y), 76, kRowH);
      static const char* const kRestart[] = {i18n::N_("Free"), i18n::N_("Play"), i18n::N_("Note")};
      static const char* const kRestartHints[] = {i18n::N_("Runs freely, never restarted"), i18n::N_("Locked to the song: the same place in the song is the same place in the wave"),
                                                  i18n::N_("Restarts on every note of a channel (the card's menu picks which)")};
      int rs = static_cast<int>(l.restart);
      const float rw = miniSegWidth(a, kRestart, 3);
      if (miniSeg(a, "##restart", kRestart, 3, &rs, ImVec2(w1.x - rw, y), kRowH, nullptr, kRestartHints))
        a.doc.editLfo(l.id, "change LFO restart", [rs, ch = a.selChannel](SongLfo& q) {
          q.restart = static_cast<LfoRestart>(rs);
          if (q.restart == LfoRestart::EachNote && q.noteChannel == kNoId) q.noteChannel = ch;
        });
    }
  }
  y += kRowH + 8;
  float depthX = w0.x;
  cardKnobs(a, l, w0.x + 4, w1.x - 4, y, kKnobR, &depthX);
  dev.depth = ImVec2(depthX, y + kKnobR);
  y += kKnobR * 2 + 3 + fsLabel() * 2 + 6 + 8;
  // Targets.
  dl->AddLine(ImVec2(w0.x, y - 0.5f), ImVec2(w1.x, y - 0.5f), P.line);
  y += 6;
  const std::vector<ChipLay> chips = chipsOf(a, l);
  {
    char cap[48];
    std::snprintf(cap, sizeof cap, "%s %d", tr("TARGETS"), static_cast<int>(chips.size()));
    ImFont* mf = a.fontMonoBold ? a.fontMonoBold : monoFont(a);
    text(dl, mf, fsLabel() - 1.5f, ImVec2(w0.x, y), P.textFaint, cap);
    const std::string note = restartNote(s, l);
    text(dl, ImGui::GetFont(), fsLabel() - 0.5f, ImVec2(w1.x - textW(ImGui::GetFont(), fsLabel() - 0.5f, note.c_str()), y - 1), P.textFaint, note.c_str());
  }
  y += 16;
  float cx = w0.x;
  const float cw = w1.x - w0.x;
  int k = 0;
  auto place = [&](float wch) {
    wch = std::min(wch, cw);
    if (cx > w0.x && cx + wch > w1.x) cx = w0.x, y += kChipH + kChipGap;
    const ImVec2 at(cx, y);
    cx += wch + kChipGap;
    return std::make_pair(at, wch);
  };
  for (const ChipLay& ch : chips) {
    const auto [at, wch] = place(ch.w);
    ImGui::PushID(k++);
    bool rc = false;
    if (chip(a, "##chip", at, wch, ch.dot, ch.round, ch.label.c_str(), ch.amt.c_str(), false, ch.dim, &rc) || rc) {
      gSetLfo = l.id, gSetTarget = ch.pid, gOpenSettings = true, gPopAt = underItem();
    }
    if (ImGui::IsItemHovered()) ui::Hint((ch.label + "  " + ch.amt + "   (" + tr("click: its settings") + ")").c_str());
    ImGui::PopID();
    dev.chips.emplace_back(ch.pid, ImVec2(at.x + wch * 0.5f, at.y + kChipH * 0.5f));
  }
  {
    const char* addL = tr("+ Add target");
    const auto [at, wch] = place(chipWidth(a, addL, "", false));
    if (chip(a, "##add", at, wch, 0, false, addL, "", true, false)) gAddLfo = l.id, gOpenAdd = true, gAddSearch[0] = 0, gPopAt = underItem();
    dev.add = ImVec2(at.x + wch * 0.5f, at.y + kChipH * 0.5f);
  }
  ImGui::PopID();
  gDevCards.push_back(std::move(dev));
}

// The dashed "New LFO" card.
void ghostCard(App& a, ImVec2 mn, ImVec2 mx) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const bool full = static_cast<int>(a.doc.song().lfos.size()) >= kMaxSongLfos;
  ImGui::SetCursorScreenPos(mn);
  ImGui::SetNextItemAllowOverlap();
  const bool click = ImGui::InvisibleButton("##ghost", ImVec2(mx.x - mn.x, mx.y - mn.y)) && !full;
  const bool hov = ImGui::IsItemHovered() && !full;
  const ImU32 lc = hov ? P.textDim : P.lineStrong;
  // A dashed outline.
  auto dash = [&](ImVec2 p0, ImVec2 p1) {
    const float len = std::hypot(p1.x - p0.x, p1.y - p0.y);
    for (float t = 0; t < len; t += 7) {
      const float t1 = std::min(len, t + 4);
      dl->AddLine(ImVec2(p0.x + (p1.x - p0.x) * t / len, p0.y + (p1.y - p0.y) * t / len),
                  ImVec2(p0.x + (p1.x - p0.x) * t1 / len, p0.y + (p1.y - p0.y) * t1 / len), lc);
    }
  };
  dash(ImVec2(mn.x + 6, mn.y + 0.5f), ImVec2(mx.x - 6, mn.y + 0.5f));
  dash(ImVec2(mn.x + 6, mx.y - 0.5f), ImVec2(mx.x - 6, mx.y - 0.5f));
  dash(ImVec2(mn.x + 0.5f, mn.y + 6), ImVec2(mn.x + 0.5f, mx.y - 6));
  dash(ImVec2(mx.x - 0.5f, mn.y + 6), ImVec2(mx.x - 0.5f, mx.y - 6));
  const float cxm = (mn.x + mx.x) * 0.5f, cym = (mn.y + mx.y) * 0.5f - 34;
  dl->AddRect(ImVec2(cxm - 21, cym - 21), ImVec2(cxm + 21, cym + 21), lc, 8.0f);
  dl->AddLine(ImVec2(cxm, cym - 9), ImVec2(cxm, cym + 9), hov ? P.text : P.textDim, 2.0f);
  dl->AddLine(ImVec2(cxm - 9, cym), ImVec2(cxm + 9, cym), hov ? P.text : P.textDim, 2.0f);
  const char* t = full ? tr("The board is full (32 sources)") : tr("New LFO");
  text(dl, a.fontBold, ImGui::GetFontSize(), ImVec2(cxm - textW(a.fontBold, ImGui::GetFontSize(), t) * 0.5f, cym + 32), P.text, t);
  const char* lines[3] = {tr("A sine at 1/4, locked to the song."), tr("Drag its handle onto a knob,"), tr("or right-click any knob.")};
  const float fs = fsLabel();
  for (int i = 0; i < 3; ++i)
    text(dl, ImGui::GetFont(), fs, ImVec2(cxm - textW(ImGui::GetFont(), fs, lines[i]) * 0.5f, cym + 54 + static_cast<float>(i) * (fs + 4)), P.textDim, lines[i]);
  if (click)
    if (ObjectId id = lfo::addNew(a.doc); id != kNoId) gSelected = id;
  // 0.7.8: the other sources, as two chips along the foot.
  if (full) return;
  const char* kc = tr("+ Chaos");
  const char* kr = tr("+ Random");
  const float cw = chipWidth(a, kc, "", false), rw = chipWidth(a, kr, "", false);
  const float y = mx.y - 14 - kChipH, x0 = std::round(cxm - (cw + rw + 8) * 0.5f);
  if (y > cym + 54 + 3 * (fs + 4) + 4) {
    if (chip(a, "##newchaos", ImVec2(x0, y), cw, 0, false, kc, "", true, false))
      if (ObjectId id = lfo::addNew(a.doc, LfoKind::Chaos); id != kNoId) gSelected = id;
    if (ImGui::IsItemHovered()) ui::Hint("A new chaos source: a smooth movement that never repeats");
    if (chip(a, "##newrandom", ImVec2(x0 + cw + 8, y), rw, 0, false, kr, "", true, false))
      if (ObjectId id = lfo::addNew(a.doc, LfoKind::Random); id != kNoId) gSelected = id;
    if (ImGui::IsItemHovered()) ui::Hint("A new random source: holds a random value until the next trigger");
  }
}

// ---- the compact row (a narrow place: Studio's mixer column) ---------------
constexpr float kCompactH = 40;

void drawCompactRow(App& a, const SongLfo& l, ImVec2 mn, float w) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 mx(mn.x + w, mn.y + kCompactH);
  const ImU32 c = colour(l.colour);
  const bool sel = gSelected == l.id;
  DevCard dev{l.id, mn, mx, {}, {}, {}, {}};
  ImGui::PushID(static_cast<int>(l.id));
  ImGui::SetCursorScreenPos(mn);
  ImGui::SetNextItemAllowOverlap();
  if (ImGui::InvisibleButton("##row", ImVec2(w, kCompactH))) gSelected = l.id;
  if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##lfomenu");
  dl->AddRectFilled(mn, mx, P.well, 4.0f);
  dl->AddRect(mn, mx, sel ? P.accent : P.line, 4.0f);
  dl->AddRectFilled(mn, ImVec2(mn.x + 4, mx.y), c, 4.0f, ImDrawFlags_RoundCornersLeft);
  float x = mn.x + 10;
  {
    ImGui::SetCursorScreenPos(ImVec2(x - 2, mn.y + 13));
    if (ImGui::InvisibleButton("##on", ImVec2(13, 14))) a.doc.editLfo(l.id, l.on ? "turn LFO off" : "turn LFO on", [](SongLfo& q) { q.on = !q.on; });
    ui::Led(dl, ImVec2(x + 4, mn.y + kCompactH * 0.5f), 4.0f, l.on);
    x += 14;
  }
  const std::string num = lfo::number(s, l.id);
  const float fs = fsLabel() + 0.5f;
  text(dl, a.fontBold, fs, ImVec2(x, mn.y + 5), P.text, num.c_str());
  const float nx = x + textW(a.fontBold, fs, num.c_str()) + 5;
  const float waveW = std::clamp(w * 0.28f, 48.0f, 110.0f);
  const float waveX = mx.x - 6 - 22 - 4 - 22 - 6 - waveW;
  // A wide row: the targets as chips between the names and the wave.
  const bool wide = w > 700;
  const float rightX = wide ? std::min(waveX, x + 250) : waveX;
  if (wide) {
    float cx = rightX + 4;
    const std::vector<ChipLay> chips = chipsOf(a, l);
    int k = 0;
    for (const ChipLay& ch : chips) {
      const float cw = std::min(ch.w, 260.0f);
      if (cx + cw > waveX - 8) {
        char more[16];
        std::snprintf(more, sizeof more, "+%d", static_cast<int>(chips.size()) - k);
        text(dl, monoFont(a), fsLabel(), ImVec2(cx, mn.y + (kCompactH - fsLabel()) * 0.5f), P.textDim, more);
        break;
      }
      ImGui::PushID(k++);
      bool rc = false;
      if (chip(a, "##chip", ImVec2(cx, mn.y + (kCompactH - kChipH) * 0.5f), cw, ch.dot, ch.round, ch.label.c_str(), ch.amt.c_str(), false, ch.dim, &rc) || rc)
        gSetLfo = l.id, gSetTarget = ch.pid, gOpenSettings = true, gPopAt = underItem();
      ImGui::PopID();
      dev.chips.emplace_back(ch.pid, ImVec2(cx + cw * 0.5f, mn.y + kCompactH * 0.5f));
      cx += cw + kChipGap;
    }
    const char* addL = tr("+ Add target");
    const float aw = chipWidth(a, addL, "", false);
    if (k == static_cast<int>(chips.size()) && cx + aw <= waveX - 8 &&
        chip(a, "##add", ImVec2(cx, mn.y + (kCompactH - kChipH) * 0.5f), aw, 0, false, addL, "", true, false))
      gAddLfo = l.id, gOpenAdd = true, gAddSearch[0] = 0, gPopAt = underItem();
  }
  text(dl, ImGui::GetFont(), fs, ImVec2(nx, mn.y + 5), P.textDim, fit(ImGui::GetFont(), fs, l.name.c_str(), std::max(8.0f, rightX - nx - 4)));
  // second line: rate, restart, target count
  {
    char b[96];
    const int n = static_cast<int>(lfo::linksOf(s, l.id).size());
    if (l.kind == LfoKind::Random)
      std::snprintf(b, sizeof b, "%s%s%s \xC2\xB7 %d %s", tr(lfo::triggerName(l.trigger)),
                    l.trigger == RandomTrigger::Tempo || l.trigger == RandomTrigger::Hz ? " " : "",
                    l.trigger == RandomTrigger::Tempo || l.trigger == RandomTrigger::Hz ? lfo::rateLabel(l).c_str() : "", n, n == 1 ? tr("target") : tr("targets"));
    else if (l.kind == LfoKind::Chaos)
      std::snprintf(b, sizeof b, "%s \xC2\xB7 %s \xC2\xB7 %s \xC2\xB7 %d %s", tr(lfo::characterName(l.character)), lfo::rateLabel(l).c_str(),
                    tr(lfo::restartName(l.restart)), n, n == 1 ? tr("target") : tr("targets"));
    else
      std::snprintf(b, sizeof b, "%s \xC2\xB7 %s \xC2\xB7 %d %s", lfo::rateLabel(l).c_str(), tr(lfo::restartName(l.restart)), n, n == 1 ? tr("target") : tr("targets"));
    text(dl, monoFont(a), fsLabel() - 1.5f, ImVec2(x, mn.y + 22), P.textFaint, fit(monoFont(a), fsLabel() - 1.5f, b, std::max(8.0f, rightX - x - 4)));
  }
  const ImVec2 w0(waveX, mn.y + 5), w1(waveX + waveW, mx.y - 5);
  dl->AddRectFilled(w0, w1, P.surface950, 3.0f);
  drawWave(a, dl, w0, w1, l, 1.0f, false, true);
  ImGui::SetCursorScreenPos(w0);
  if (ImGui::InvisibleButton("##wave", ImVec2(w1.x - w0.x, w1.y - w0.y))) gSelected = l.id;
  if (ImGui::IsItemHovered()) ui::Hint("Click to select; the full board (pop out) edits everything");
  handle(a, l, ImVec2(mx.x - 6 - 22 - 4 - 22, mn.y + 9), ImVec2(22, 22), &dev.handle);
  {
    const ImVec2 mp(mx.x - 6 - 22, mn.y + 9);
    ImGui::SetCursorScreenPos(mp);
    if (ImGui::InvisibleButton("##menu", ImVec2(22, 22))) ImGui::OpenPopup("##lfomenu");
    const bool hov = ImGui::IsItemHovered();
    for (int k = -1; k <= 1; ++k) dl->AddCircleFilled(ImVec2(mp.x + 11 + static_cast<float>(k) * 4.5f, mp.y + 11), 1.4f, hov ? P.text : P.textDim, 6);
  }
  if (ImGui::BeginPopup("##lfomenu")) {
    if (ImGui::MenuItem(tr("Add a target..."))) gAddLfo = l.id, gOpenAdd = true, gAddSearch[0] = 0;
    if (ImGui::BeginMenu(tr("Targets"), !lfo::linksOf(s, l.id).empty())) {
      for (const ModLink* m : lfo::linksOf(s, l.id)) {
        const ParamId pid{m->owner, m->index};
        if (ImGui::MenuItem((lfo::targetLabel(s, pid) + "  " + amountText(*m)).c_str())) gSetLfo = l.id, gSetTarget = pid, gOpenSettings = true;
      }
      ImGui::EndMenu();
    }
    ImGui::Separator();
    lfoMenuItems(a, l.id);
    ImGui::EndPopup();
  }
  ImGui::PopID();
  gDevCards.push_back(std::move(dev));
}

// The order the board shows: filtered by the toolbar's filter and search, sorted.
std::vector<const SongLfo*> shownLfos(App& a) {
  const Song& s = a.doc.song();
  std::vector<const SongLfo*> v;
  for (const SongLfo& l : s.lfos) {
    const auto links = lfo::linksOf(s, l.id);
    if (gFilter == 1 && !moving(a, l)) continue;
    if (gFilter == 2 && !links.empty()) continue;
    if (gSearch[0]) {
      bool hit = matches(lfo::label(s, l.id), gSearch);
      for (const ModLink* m : links) hit = hit || matches(lfo::targetLabel(s, {m->owner, m->index}), gSearch);
      if (!hit) continue;
    }
    v.push_back(&l);
  }
  if (gSort == 1)
    std::stable_sort(v.begin(), v.end(), [&](const SongLfo* x, const SongLfo* y) { return lfo::label(s, x->id) < lfo::label(s, y->id); });
  else if (gSort == 2)
    std::stable_sort(v.begin(), v.end(), [&](const SongLfo* x, const SongLfo* y) { return lfo::linksOf(s, x->id).size() > lfo::linksOf(s, y->id).size(); });
  return v;
}

bool showOnKnobs(App& a) { return !a.settings.getBool("ui.lfoHideOnKnobs"); }

// The board's toolbar. `narrow`: the compact place (fewer controls, the pop-out button).
void toolbar(App& a, bool narrow, bool studioDocked) {
  const auto& P = pal();
  const Song& s = a.doc.song();
  ui::BeginToolbar("##lfotools", 42);
  // What fits: 0 everything, 1 no Show on knobs (the ... menu has it), 2 the short count, 3 no sort or view (the menu has them).
  const float W = ImGui::GetContentRegionAvail().x;
  const int tier = W >= 1220 ? 0 : W >= 1080 ? 1 : W >= 940 ? 2 : 3;
  static float rightW[5] = {470.0f, 330.0f, 330.0f, 92.0f, 92.0f};  // the right-hand group as drawn last frame
  float& rw = rightW[narrow ? 4 : tier];
  ImGui::AlignTextToFramePadding();
  ImGui::PushFont(a.fontBold, 0.0f);
  ImGui::TextUnformatted(tr("LFO board"));
  ImGui::PopFont();
  char b[96];
  std::snprintf(b, sizeof b, tr("%d of %d sources \xC2\xB7 %d targets"), static_cast<int>(s.lfos.size()), kMaxSongLfos, targetCount(s));
  ImGui::SameLine(0, 8);
  ImGui::PushFont(monoFont(a), 0.0f);
  ImGui::TextColored(theme::vec(P.textFaint), "%s", narrow || tier >= 2 ? (std::to_string(s.lfos.size()) + "/" + std::to_string(kMaxSongLfos)).c_str() : b);
  ImGui::PopFont();
  ImGui::SameLine(0, 10);
  const bool full = static_cast<int>(s.lfos.size()) >= kMaxSongLfos;
  if (ui::PrimaryButton(narrow ? "+" : "+ New LFO", 0, "A new LFO: a sine at 1/4, locked to the song", !full))
    if (ObjectId id = lfo::addNew(a.doc); id != kNoId) gSelected = id;
  if (!narrow && tier <= 2) {  // 0.7.8: the other sources (else in the ... menu)
    ImGui::SameLine(0, 6);
    if (ui::TextButton("+ Chaos", false, 0, "A new chaos source: a smooth movement that never repeats") && !full)
      if (ObjectId id = lfo::addNew(a.doc, LfoKind::Chaos); id != kNoId) gSelected = id;
    ImGui::SameLine(0, 6);
    if (ui::TextButton("+ Random", false, 0, "A new random source: holds a random value until the next trigger") && !full)
      if (ObjectId id = lfo::addNew(a.doc, LfoKind::Random); id != kNoId) gSelected = id;
  }
  if (!narrow) {
    ImGui::SameLine(0, 10);
    static const char* const kFilt[] = {i18n::N_("All"), i18n::N_("Moving now"), i18n::N_("No targets")};
    ui::Segmented("##filter", kFilt, 3, &gFilter);
    // The search takes what is left before the right-hand group (none when too little).
    ImGui::SameLine(0, 6);
    const float room = ImGui::GetContentRegionAvail().x - rw - 12;
    if (room >= 100) {
      const float w = std::min(room, 220.0f);
      ImGui::SetNextItemWidth(w);
      // 0.7.8: the hint whole or a shorter one, never cut (the box's text starts after its padding)
      const char* hint = tr("Find a source or target");
      if (ImGui::CalcTextSize(hint).x > w - ImGui::GetStyle().FramePadding.x * 2) hint = tr("Find a source");
      if (ImGui::CalcTextSize(hint).x > w - ImGui::GetStyle().FramePadding.x * 2) hint = tr("Find");
      ImGui::InputTextWithHint("##search", hint, gSearch, sizeof gSearch);
    }
  }
  // The right-hand group, against the right edge (its width as drawn last frame).
  ImGui::SameLine(0, 0);
  {
    const float x0 = ImGui::GetCursorPosX(), av = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(av - rw > 6 ? x0 + av - rw : x0 + 6);
  }
  const float groupX = ImGui::GetCursorScreenPos().x;
  static const char* const kSorts[] = {i18n::N_("Board order"), i18n::N_("Name"), i18n::N_("Most targets")};
  static const char* const kViews[] = {i18n::N_("Cards"), i18n::N_("Compact")};
  const bool menuHasSort = narrow || tier >= 3;
  if (!menuHasSort) {
    if (tier == 0) {
      ImGui::AlignTextToFramePadding();
      ImGui::TextColored(theme::vec(P.textDim), "%s", tr("Sort"));
      ImGui::SameLine(0, 6);
    }
    if (ui::BeginCombo("##sort", tr(kSorts[gSort]), 120)) {
      for (int i = 0; i < 3; ++i)
        if (ImGui::Selectable(tr(kSorts[i]), gSort == i)) gSort = i;
      ImGui::EndCombo();
    }
    ImGui::SameLine(0, 6);
    ui::Segmented("##view", kViews, 2, &gView);
    ImGui::SameLine(0, 6);
    if (tier == 0) {
      const bool on = showOnKnobs(a);
      if (ui::TextButton("Show on knobs", on, 0, "Knobs an LFO moves show the swing and a live dot")) a.settings.set("ui.lfoHideOnKnobs", on);
      ImGui::SameLine(0, 6);
    }
  }
  if (studioDocked) {
    if (ui::TextButton("Pop out", false, 0, "The full board in a window of its own")) gPopOut = true;
    ImGui::SameLine(0, 6);
  } else if (layoutui::active() == layout::Preset::Studio) {
    if (ui::TextButton("Dock", false, 0, "Back beside the mixer")) gDockBack = true;
    ImGui::SameLine(0, 6);
  }
  if (ui::TextButton("...", false, 26, "More")) ImGui::OpenPopup("##boardmenu");
  rw = std::ceil(ImGui::GetItemRectMax().x - groupX) + 2;
  if (ImGui::BeginPopup("##boardmenu")) {
    if (ImGui::MenuItem(tr("New chaos"), nullptr, false, !full))
      if (ObjectId id = lfo::addNew(a.doc, LfoKind::Chaos); id != kNoId) gSelected = id;
    if (ImGui::MenuItem(tr("New random"), nullptr, false, !full))
      if (ObjectId id = lfo::addNew(a.doc, LfoKind::Random); id != kNoId) gSelected = id;
    if (ImGui::BeginMenu(tr("New LFO from a preset"), !full)) {
      for (const lfo::Preset& p : lfo::presets())
        if (ImGui::MenuItem(tr(p.name))) {
          SongLfo l = lfo::makeDefault(s);
          lfo::applyPreset(l, p.lfo);
          if (ObjectId id = a.doc.addLfo(l); id != kNoId) gSelected = id;
        }
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem(tr("Edit the selected LFO in a window"), nullptr, false, s.lfo(gSelected) != nullptr)) a.showLfoDetail = true;
    if (ImGui::MenuItem(tr("Show on knobs"), nullptr, showOnKnobs(a))) a.settings.set("ui.lfoHideOnKnobs", showOnKnobs(a));
    if (menuHasSort) {
      ImGui::Separator();
      if (ImGui::BeginMenu(tr("Sort"))) {
        for (int i = 0; i < 3; ++i)
          if (ImGui::MenuItem(tr(kSorts[i]), nullptr, gSort == i)) gSort = i;
        ImGui::EndMenu();
      }
      if (!narrow)
        for (int i = 0; i < 2; ++i)
          if (ImGui::MenuItem(tr(kViews[i]), nullptr, gView == i)) gView = i;
    }
    ImGui::Separator();
    bool any = false;
    for (const SongLfo& l : s.lfos) any = any || lfo::linksOf(s, l.id).empty();
    if (ImGui::MenuItem(tr("Remove the sources with no targets"), nullptr, false, any)) {
      a.doc.beginGroup("remove unused LFOs");
      std::vector<ObjectId> gone;
      for (const SongLfo& l : s.lfos)
        if (lfo::linksOf(s, l.id).empty()) gone.push_back(l.id);
      for (ObjectId id : gone) a.doc.removeLfo(id);
      a.doc.endGroup(false);
    }
    ImGui::EndPopup();
  }
  ui::EndToolbar();
}

void popups(App& a) {
  const ImVec2 at = gPopAt;
  gPopAt = ImVec2(-1, -1);
  const bool openSet = gOpenSettings, openAdd = gOpenAdd;
  if (gOpenSettings) ImGui::OpenPopup("##lfotarget"), gOpenSettings = false;
  if (gOpenAdd) ImGui::OpenPopup("##lfoadd"), gOpenAdd = false;
  // Under the chip; above it when the chip is low on the screen.
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const bool up = at.y > vp->WorkPos.y + vp->WorkSize.y * 0.55f;
  const ImVec2 pos(at.x, up ? at.y - kChipH - 8 : at.y), pivot(0.0f, up ? 1.0f : 0.0f);
  if (openSet && at.x >= 0) ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
  ImGui::SetNextWindowSizeConstraints(ImVec2(330, 0), ImVec2(420, FLT_MAX));
  if (ImGui::BeginPopup("##lfotarget")) {
    targetSettingsBody(a, gSetLfo, gSetTarget, false);
    ImGui::EndPopup();
  }
  if (openAdd && at.x >= 0) ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
  if (ImGui::BeginPopup("##lfoadd")) {
    addTargetBody(a, gAddLfo);
    ImGui::EndPopup();
  }
}

// The board's body in [the cursor, avail]: cards in columns, or compact rows.
void boardBody(App& a, bool compact) {
  const auto& P = pal();
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::vec(P.surface800));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 10));
  ImGui::BeginChild("##rack", avail, ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_None);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
  const ImVec2 o = ImGui::GetCursorScreenPos();
  const float W = ImGui::GetContentRegionAvail().x;
  const std::vector<const SongLfo*> shown = shownLfos(a);
  gDevCards.clear();
  gDevCompact = compact;
  if (compact) {
    gDevCols = 1;
    float y = o.y;
    for (const SongLfo* l : shown) {
      drawCompactRow(a, *l, ImVec2(o.x, y), W);
      y += kCompactH + 6;
    }
    if (shown.empty()) {
      ImGui::SetCursorScreenPos(ImVec2(o.x, y));
      ImGui::TextDisabled("%s", a.doc.song().lfos.empty() ? tr("No LFOs yet: + makes one, or right-click any knob.") : tr("No LFO matches."));
      y += ImGui::GetTextLineHeightWithSpacing();
    }
    ImGui::SetCursorScreenPos(ImVec2(o.x, y));
    ImGui::Dummy(ImVec2(W, 4));
  } else {
    const float gap = 10;
    const int cols = std::clamp(static_cast<int>((W + gap) / (290 + gap)), 1, 4);
    gDevCols = cols;
    const float cw = std::floor((W - gap * static_cast<float>(cols - 1)) / static_cast<float>(cols));
    const size_t n = shown.size() + 1;  // with the New LFO card
    float y = o.y;
    for (size_t r0 = 0; r0 < n; r0 += static_cast<size_t>(cols)) {
      float h = 0;
      for (size_t i = r0; i < std::min(n, r0 + static_cast<size_t>(cols)); ++i) h = std::max(h, i < shown.size() ? cardHeight(a, *shown[i], cw) : 0.0f);
      if (h <= 0) h = cardHeight(a, SongLfo{}, cw);
      for (size_t i = r0; i < std::min(n, r0 + static_cast<size_t>(cols)); ++i) {
        const float x = std::round(o.x + static_cast<float>(i - r0) * (cw + gap));
        if (i < shown.size()) drawCard(a, *shown[i], ImVec2(x, y), ImVec2(x + cw, y + h));
        else ghostCard(a, ImVec2(x, y), ImVec2(x + cw, y + h));
      }
      y += h + gap;
    }
    ImGui::SetCursorScreenPos(ImVec2(o.x, y - gap));
    ImGui::Dummy(ImVec2(W, 1));
  }
  ImGui::EndChild();
}

void trackTouched(App& a) {
  const ParamId t = a.doc.lastTouchedParam();
  if (t.owner == kNoId) return;
  if (!gTouched.empty() && gTouched.front() == t) return;
  gTouched.erase(std::remove(gTouched.begin(), gTouched.end(), t), gTouched.end());
  gTouched.push_front(t);
  while (gTouched.size() > 16) gTouched.pop_back();
}

// ---- the drawn shape editor (detail window) ----------------------------
void drawnEditor(App& a, const SongLfo& l, ImVec2 mn, ImVec2 mx) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::SetCursorScreenPos(mn);
  ImGui::InvisibleButton("##drawn", ImVec2(mx.x - mn.x, mx.y - mn.y), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  const bool hov = ImGui::IsItemHovered();
  const float pad = 6;
  auto toScreen = [&](const LfoPoint& p) { return ImVec2(mn.x + p.x * (mx.x - mn.x), mx.y - pad - (p.y + 1) * 0.5f * (mx.y - mn.y - pad * 2)); };
  auto fromScreen = [&](ImVec2 s) {
    LfoPoint p;
    p.x = std::clamp((s.x - mn.x) / (mx.x - mn.x), 0.0f, 1.0f);
    p.y = std::clamp(((mx.y - pad - s.y) / (mx.y - mn.y - pad * 2)) * 2 - 1, -1.0f, 1.0f);
    return p;
  };
  std::vector<LfoPoint> pts = l.points.empty() ? lfo::defaultPoints() : l.points;
  static int dragIdx = -1;
  const ImVec2 m = ImGui::GetIO().MousePos;
  int near = -1;
  for (size_t i = 0; i < pts.size(); ++i) {
    const ImVec2 sp = toScreen(pts[i]);
    if (std::hypot(sp.x - m.x, sp.y - m.y) < 7) near = static_cast<int>(i);
  }
  const uint64_t key = (static_cast<uint64_t>(l.id) << 8) ^ 0x54;
  if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    if (near >= 0) {
      dragIdx = near;
    } else if (static_cast<int>(pts.size()) < kLfoMaxPoints) {
      LfoPoint np = fromScreen(m);
      pts.push_back(np);
      std::sort(pts.begin(), pts.end(), [](const LfoPoint& x, const LfoPoint& y) { return x.x < y.x; });
      for (size_t i = 0; i < pts.size(); ++i)
        if (pts[i].x == np.x && pts[i].y == np.y) dragIdx = static_cast<int>(i);
      a.doc.editLfo(l.id, "add LFO point", [pts](SongLfo& q) { q.points = pts; }, key);
    }
  }
  if (ImGui::IsItemActive() && dragIdx >= 0 && dragIdx < static_cast<int>(pts.size()) && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0)) {
    LfoPoint np = fromScreen(m);
    np.curve = pts[static_cast<size_t>(dragIdx)].curve;
    const float lo = dragIdx > 0 ? pts[static_cast<size_t>(dragIdx) - 1].x : 0.0f;
    const float hi = dragIdx + 1 < static_cast<int>(pts.size()) ? pts[static_cast<size_t>(dragIdx) + 1].x : 1.0f;
    np.x = std::clamp(np.x, lo, hi);
    pts[static_cast<size_t>(dragIdx)] = np;
    a.doc.editLfo(l.id, "move LFO point", [pts](SongLfo& q) { q.points = pts; }, key);
  }
  if (ImGui::IsItemDeactivated()) dragIdx = -1, a.doc.endGesture();
  if (hov && near >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && pts.size() > 2) {
    pts.erase(pts.begin() + near);
    a.doc.editLfo(l.id, "remove LFO point", [pts](SongLfo& q) { q.points = pts; });
  }
  if (hov) ui::Hint("Click to add a point, drag to move it, right-click removes it");
  dl->AddRectFilled(mn, mx, P.surface950, 4.0f);
  dl->AddRect(mn, mx, P.line, 4.0f);
  SongLfo shown = l;
  shown.points = pts;
  shown.phase = 0, shown.depth = 1, shown.offset = 0;
  drawWave(a, dl, mn, mx, shown, 1.0f, true, false);
  for (size_t i = 0; i < pts.size(); ++i) {
    const ImVec2 sp = toScreen(pts[i]);
    dl->AddCircleFilled(sp, static_cast<int>(i) == near ? 5.0f : 4.0f, colour(l.colour), 12);
    dl->AddCircle(sp, static_cast<int>(i) == near ? 5.0f : 4.0f, P.surface950, 12);
  }
}

bool studioDockedNow() {
  if (layoutui::active() != layout::Preset::Studio) return false;
  const ImGuiWindow* w = ImGui::GetCurrentWindowRead();
  return w && w->DockIsActive;
}

}  // namespace

ImU32 colour(int k) { return theme::litRole(theme::role::kLfo[std::clamp(k, 0, kLfoColours - 1)]); }

void select(ObjectId id) { gSelected = id; }
ObjectId selected() { return gSelected; }

void toggle(App& a) {
  if (!a.showLfoBoard) {
    a.showLfoBoard = true;
    // Its home region shows (Smooth's lower pane, Clips' mixer band).
    if (layoutui::active() == layout::Preset::Logic) layoutui::setRegionVisible(a, "logic.lower", true);
    else if (layoutui::active() == layout::Preset::Live) layoutui::setRegionVisible(a, "live.mixer", true, false);
    a.focusWindow = "LFO board";
    return;
  }
  // Open behind another tab: comes forward; in front: closes.
  const ImGuiWindow* w = ImGui::FindWindowByName(ui::windowTitle("LFO board").c_str());
  const bool front = w && w->WasActive && (!w->DockIsActive || !w->DockNode || !w->DockNode->TabBar || w->DockNode->TabBar->VisibleTabId == w->TabId);
  if (front) a.showLfoBoard = false;
  else a.focusWindow = "LFO board";
}

void drawBoard(App& a) {
  trackTouched(a);
  if (gPickLfo != kNoId) {  // Pick on screen: waiting for a knob
    if (!a.doc.song().lfo(gPickLfo) || ImGui::IsKeyPressed(ImGuiKey_Escape)) gPickLfo = kNoId;
    else ImGui::SetTooltip("%s %s", tr("Click a knob for"), lfo::number(a.doc.song(), gPickLfo).c_str());
  }
  if (!a.showLfoBoard) return;
  const Song& s = a.doc.song();
  if (!s.lfo(gSelected)) gSelected = s.lfos.empty() ? kNoId : s.lfos.front().id;
  const std::string title = ui::windowTitle("LFO board");
  const bool studio = layoutui::active() == layout::Preset::Studio;
  int flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse;
  if (studio) {
    const ImGuiWindow* self = ImGui::FindWindowByName(title.c_str());
    if (gPopOut || gPopSize > 0) {
      const ImGuiViewport* vp = ImGui::GetMainViewport();
      ImGui::SetNextWindowDockID(0, ImGuiCond_Always);
      ImGui::SetNextWindowSize(ImVec2(std::min(1360.0f, vp->WorkSize.x - 80), std::min(860.0f, vp->WorkSize.y - 80)), ImGuiCond_Always);
      ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
      if (gPopOut) ImGui::SetNextWindowFocus(), gPopSize = 3;  // over the floating windows (the channel rack)
      --gPopSize;
      gPopOut = false;
    } else if (!self || !self->WasActive || gDockBack) {
      // Opening: a tab beside the Mixer (where the mixer is docked).
      const ImGuiWindow* mix = ImGui::FindWindowByName(ui::windowTitle("Mixer").c_str());
      if (mix && mix->DockId && (gDockBack || !self || !self->WasActive)) ImGui::SetNextWindowDockID(mix->DockId, ImGuiCond_Always);
      else if (!self) ImGui::SetNextWindowSize(ImVec2(1200, 760), ImGuiCond_FirstUseEver);
      gDockBack = false;
    }
  } else {
    flags = layoutui::dockFlags(title.c_str(), flags);
    ImGui::SetNextWindowSize(ImVec2(1200, 760), ImGuiCond_FirstUseEver);
  }
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  const bool vis = ImGui::Begin(title.c_str(), &a.showLfoBoard, flags);
  ImGui::PopStyleVar();
  if (vis) {
    const ImGuiWindow* win = ImGui::GetCurrentWindowRead();
    gDevWin0 = win->Pos, gDevWin1 = ImVec2(win->Pos.x + win->Size.x, win->Pos.y + win->Size.y);
    const float W = ImGui::GetContentRegionAvail().x;
    const bool docked = studioDockedNow();
    const bool narrow = W < 560;
    // A narrow column (Studio's mixer) or a short band (Clips' mixer band): the compact list.
    const bool compact = narrow || ImGui::GetContentRegionAvail().y < 300 || gView == 1;
    const ObjectId before = gSelected;
    if (trackerpages::active(a)) {  // the Tracker's LFO page: the pages' title bar, its Exit back to the pattern
      trackerpages::pageTitle(a, tr("LFO"), "");  // (the board's own row counts the LFOs)
    }
    toolbar(a, narrow, docked);
    boardBody(a, compact);
    popups(a);
    // Clips: the LFO picked on the board shows in the device band, as a track's devices do.
    if (gSelected != before && gSelected != kNoId && layoutui::active() == layout::Preset::Live) a.showLfoDetail = true;
  }
  ImGui::End();
}

void drawDetail(App& a) {
  if (!a.showLfoDetail) return;
  const Song& s = a.doc.song();
  const std::string title = ui::windowTitle("LFO detail");
  int flags = ImGuiWindowFlags_NoCollapse;
  flags = layoutui::dockFlags(title.c_str(), flags);
  ImGui::SetNextWindowSize(ImVec2(760, 420), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin(title.c_str(), &a.showLfoDetail, flags)) {
    ImGui::End();
    return;
  }
  const SongLfo* L = s.lfo(gSelected);
  if (!L && !s.lfos.empty()) gSelected = s.lfos.front().id, L = &s.lfos.front();
  if (!L) {
    ImGui::TextDisabled("%s", tr("No LFOs yet."));
    if (ui::PrimaryButton("+ New LFO"))
      if (ObjectId id = lfo::addNew(a.doc); id != kNoId) gSelected = id;
    ImGui::End();
    return;
  }
  const SongLfo l = *L;
  const auto& P = pal();
  // A row of LFO tabs to pick which one.
  for (const SongLfo& o : s.lfos) {
    ImGui::PushID(static_cast<int>(o.id));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    if (ui::TextButton(lfo::number(s, o.id).c_str(), o.id == l.id)) gSelected = o.id;
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + ImGui::GetFrameHeight() - 3), ImVec2(ImGui::GetItemRectMax().x, p.y + ImGui::GetFrameHeight()), colour(o.colour));
    ImGui::PopID();
    ImGui::SameLine(0, 4);
    if (ImGui::GetContentRegionAvail().x < 60) ImGui::NewLine();
  }
  ImGui::NewLine();
  ImGui::PushID(static_cast<int>(l.id));
  ImGui::PushFont(a.fontBold, 0.0f);
  ImGui::TextUnformatted(lfo::label(s, l.id).c_str());
  ImGui::PopFont();
  ImGui::SameLine();
  if (l.kind == LfoKind::Random) ImGui::TextDisabled("%s", restartNote(s, l).c_str());
  else ImGui::TextDisabled("%s \xC2\xB7 %s \xC2\xB7 %s", tr(lfo::kindSummary(l)), lfo::rateLabel(l).c_str(), restartNote(s, l).c_str());
  const float W = ImGui::GetContentRegionAvail().x;
  const bool side = W >= 620;  // settings beside the wave, else under it
  const float waveW = side ? std::clamp(W - 340, 280.0f, 620.0f) : W;
  const ImVec2 w0 = ImGui::GetCursorScreenPos(), w1(w0.x + waveW, w0.y + 150);
  // Left: the wave (or the drawn editor), the shapes, the drawn shape's options.
  ImGui::BeginGroup();
  if (l.kind == LfoKind::Lfo && l.shape == LfoShape::Drawn) {
    drawnEditor(a, l, w0, w1);
  } else {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(w0, w1, P.surface950, 4.0f);
    dl->AddRect(w0, w1, P.line, 4.0f);
    drawWave(a, dl, w0, w1, l, 2.0f, true, true);
    ImGui::Dummy(ImVec2(waveW, 150));
  }
  ImGui::SetCursorScreenPos(ImVec2(w0.x, w1.y + 8));
  if (l.kind != LfoKind::Lfo) {  // 0.7.8: Chaos's and Random's own rows
    float yy = w1.y + 8;
    const ImVec2 r1(w0.x + waveW, w1.y);
    sourceRows(a, l, ImVec2(w0.x, w1.y), r1, yy);
    ImGui::SetCursorScreenPos(ImVec2(w0.x, yy + kRowH + 4));
    ImGui::Dummy(ImVec2(waveW, 1));
  } else {
    float x = 0;
    const float gap = 4;
    for (int k = 0; k < static_cast<int>(LfoShape::Count); ++k) {
      const LfoShape sh = static_cast<LfoShape>(k);
      const char* nm = tr(lfo::shapeName(sh));
      const float bw = ImGui::CalcTextSize(nm).x + ImGui::GetStyle().FramePadding.x * 2;
      if (k > 0) {
        if (x + gap + bw <= waveW) ImGui::SameLine(0, gap), x += gap;
        else x = 0;
      }
      ImGui::PushID(k);
      if (ui::TextButton(nm, l.shape == sh))
        a.doc.editLfo(l.id, "change LFO shape", [sh](SongLfo& q) {
          q.shape = sh;
          if (sh == LfoShape::Drawn && q.points.empty()) q.points = lfo::defaultPoints();
        });
      ImGui::PopID();
      x += ImGui::GetItemRectSize().x;
    }
  }
  if (l.kind == LfoKind::Lfo && l.shape == LfoShape::Drawn) {
    bool steps = l.drawnSteps;
    if (ImGui::Checkbox(tr("Steps between the points"), &steps)) a.doc.editLfo(l.id, "change LFO drawn steps", [steps](SongLfo& q) { q.drawnSteps = steps; });
    ImGui::SameLine();
    if (ui::TextButton("Reset the shape")) a.doc.editLfo(l.id, "reset LFO shape", [](SongLfo& q) { q.points = lfo::defaultPoints(); });
  }
  ImGui::EndGroup();
  const float leftBottom = ImGui::GetItemRectMax().y;
  // Right (or below): rate, restart, the five knobs, more settings.
  if (side) ImGui::SetCursorScreenPos(ImVec2(w1.x + 16, w0.y));
  else ImGui::SetCursorScreenPos(ImVec2(w0.x, leftBottom + 8));
  ImGui::BeginGroup();
  {
    if (l.kind == LfoKind::Lfo) {  // (Chaos and Random: these are in their rows under the picture)
      static const char* const kSync[] = {i18n::N_("Hz"), i18n::N_("Tempo")};
      int sync = l.tempoSync ? 1 : 0;
      if (ui::Segmented("##sync", kSync, 2, &sync)) a.doc.editLfo(l.id, "change LFO rate mode", [sync](SongLfo& q) { q.tempoSync = sync == 1; });
      ImGui::SameLine();
      rateControl(a, l, ImGui::GetCursorScreenPos(), 96, ImGui::GetFrameHeight());
      ImGui::Dummy(ImVec2(96, ImGui::GetFrameHeight()));
      static const char* const kRestart[] = {i18n::N_("Free"), i18n::N_("Play"), i18n::N_("Note")};
      int rs = static_cast<int>(l.restart);
      if (ui::Segmented("##restart", kRestart, 3, &rs))
        a.doc.editLfo(l.id, "change LFO restart", [rs, ch = a.selChannel](SongLfo& q) {
          q.restart = static_cast<LfoRestart>(rs);
          if (q.restart == LfoRestart::EachNote && q.noteChannel == kNoId) q.noteChannel = ch;
        });
    }
    const ImVec2 kp = ImGui::GetCursorScreenPos();
    const float kh = 6 + 34 + 3 + fsLabel() * 2 + 10;
    cardKnobs(a, l, kp.x, kp.x + 300, kp.y + 6, 17);
    ImGui::SetCursorScreenPos(kp);
    ImGui::Dummy(ImVec2(300, kh));
    if (ui::TextButton("More settings", false, 0, l.kind == LfoKind::Lfo ? "Fade in, steps, seed, the note channel, colour and more"
                                                                          : "The seed, the note channel, colour and more"))
      ImGui::OpenPopup("##detailmore");
    if (ImGui::BeginPopup("##detailmore")) {
      lfoMenuItems(a, l.id);
      ImGui::EndPopup();
    }
  }
  ImGui::EndGroup();
  ImGui::SetCursorScreenPos(ImVec2(w0.x, std::max(leftBottom, ImGui::GetItemRectMax().y) + 10));
  ImGui::Dummy(ImVec2(1, 1));
  // Targets, each with its settings inline.
  ImGui::Separator();
  ImGui::TextDisabled("%s", tr("TARGETS"));
  for (const ModLink* m : lfo::linksOf(s, l.id)) {
    const ParamId pid{m->owner, m->index};
    ImGui::PushID(static_cast<int>(pid.packed() & 0x7FFFFFFF));
    if (ImGui::TreeNode((lfo::targetLabel(s, pid) + "  " + amountText(*m) + "###t").c_str())) {
      targetSettingsBody(a, l.id, pid, true);
      ImGui::TreePop();
    }
    ImGui::PopID();
  }
  if (ui::TextButton("+ Add target")) gAddLfo = l.id, gOpenAdd = true, gAddSearch[0] = 0, gPopAt = underItem();
  ImGui::PopID();
  popups(a);
  ImGui::End();
}

// ---- knobs ----------------------------------------------------------------
bool canModulate(App& a, ParamId pid) { return lfo::canTarget(a.doc.song(), pid); }

const char* sourceMenuName(LfoKind kind) {
  return kind == LfoKind::Chaos ? i18n::N_("Chaos") : kind == LfoKind::Random ? i18n::N_("Random") : i18n::N_("LFO");
}

bool movedBy(App& a, ParamId pid, LfoKind kind) {
  const Song& s = a.doc.song();
  for (const ModLink* m : lfoLinksOn(s, pid.owner, pid.index))
    if (const SongLfo* l = s.lfo(m->sourceOwner); l && l->kind == kind) return true;
  return false;
}

ObjectId knobNew(App& a, ParamId pid, LfoKind kind) {
  const ObjectId id = lfo::newForKnob(a.doc, pid, nullptr, kind);
  if (id != kNoId) gSelected = id;
  else a.notify(a.doc.lastLinkRefusal().empty() ? tr("This control cannot take this source") : a.doc.lastLinkRefusal(), true);
  return id;
}

bool knobToggle(App& a, ParamId pid, ObjectId source) {
  if (findLfoLink(a.doc.song(), source, pid.owner, pid.index)) return a.doc.removeLfoLink(source, pid);
  if (lfo::link(a.doc, source, pid)) return true;
  a.notify(a.doc.lastLinkRefusal().empty() ? tr("This control cannot take this source") : a.doc.lastLinkRefusal(), true);
  return false;
}

// One source submenu of the knob's Modulate menu (LFO, Chaos, Random): the board's sources of
// that kind (a check on the ones moving the knob; a click links or unlinks), then the new ones.
namespace {
// A source's summary in its submenu row: its shape, character or trigger and, when it has one, its rate.
std::string summaryOf(const SongLfo& l) {
  std::string sc = tr(lfo::kindSummary(l));
  if (l.kind != LfoKind::Random || l.trigger == RandomTrigger::Tempo || l.trigger == RandomTrigger::Hz) sc += " " + lfo::rateLabel(l);
  return sc;
}
}  // namespace

void sourceMenu(App& a, ParamId pid, LfoKind kind) {
  const Song& s = a.doc.song();
  auto item = [&](const SongLfo& l) {
    ImGui::PushID(static_cast<int>(l.id));
    const bool linked = findLfoLink(s, l.id, pid.owner, pid.index) != nullptr;
    const std::string sc = summaryOf(l);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const std::string lb = lfo::label(s, l.id);
    // (the colour swatch sits in the space the leading spaces leave, after the check mark's gutter;
    // the label is fitted to the menu)
    if (ImGui::MenuItem(("    " + std::string(fit(ImGui::GetFont(), ImGui::GetFontSize(), lb.c_str(), 200))).c_str(), sc.c_str(), linked))
      knobToggle(a, pid, l.id);
    const float h = ImGui::GetTextLineHeight();
    const float sx = p.x + static_cast<float>(ImGui::GetCurrentWindow()->DC.MenuColumns.OffsetLabel);
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(sx, p.y + h * 0.5f - 4.0f), ImVec2(sx + 9, p.y + h * 0.5f + 5.0f), colour(l.colour), 2.0f);
    if (ImGui::IsItemHovered()) ui::Hint(linked ? "Click to stop it moving this control" : "Click to let it move this control (25 percent around the knob)");
    ImGui::PopID();
  };
  std::vector<const SongLfo*> mine;
  for (const SongLfo& l : s.lfos)
    if (l.kind == kind) mine.push_back(&l);
  const int firstN = 8;
  for (int i = 0; i < static_cast<int>(mine.size()) && i < firstN; ++i) item(*mine[static_cast<size_t>(i)]);
  if (static_cast<int>(mine.size()) > firstN) {
    char b[48];
    std::snprintf(b, sizeof b, tr("More (%d)"), static_cast<int>(mine.size()) - firstN);
    if (ImGui::BeginMenu(b)) {
      for (size_t i = firstN; i < mine.size(); ++i) item(*mine[i]);
      ImGui::EndMenu();
    }
  }
  if (!mine.empty()) ImGui::Separator();
  const bool full = static_cast<int>(s.lfos.size()) >= kMaxSongLfos;
  const char* newLabel = kind == LfoKind::Chaos ? tr("New chaos") : kind == LfoKind::Random ? tr("New random") : tr("New LFO");
  // (the new source's summary, in the same form as the rows above: "Calm 1/1", "Tempo 1/8", "Sine 1/4")
  const std::string newWhat = summaryOf(lfo::makeDefault(s, kind));
  if (ImGui::MenuItem(newLabel, newWhat.c_str(), false, !full)) knobNew(a, pid, kind);
  if (full && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ui::Hint("The board is full (32 sources)");
  if (kind == LfoKind::Lfo && ImGui::BeginMenu(tr("New LFO from a preset"), !full)) {
    for (const lfo::Preset& p : lfo::presets())
      if (ImGui::MenuItem(tr(p.name)))
        if (ObjectId id = lfo::newForKnob(a.doc, pid, &p.lfo); id != kNoId) gSelected = id;
    ImGui::EndMenu();
  }
}

bool linkedSettingsItems(App& a, ParamId pid) {
  const Song& s = a.doc.song();
  const auto here = lfoLinksOn(s, pid.owner, pid.index);
  for (const ModLink* m : here) {
    ImGui::PushID(static_cast<int>(m->sourceOwner));
    const std::string lb = lfo::number(s, m->sourceOwner) + " " + tr("settings") + " (" + amountText(*m) + ")";
    if (ImGui::BeginMenu(lb.c_str())) {
      targetSettingsBody(a, m->sourceOwner, pid, true);
      ImGui::EndMenu();
    }
    ImGui::PopID();
  }
  return !here.empty();
}

int removeLinks(App& a, ParamId pid) {
  std::vector<ObjectId> from;
  for (const ModLink* m : lfoLinksOn(a.doc.song(), pid.owner, pid.index)) from.push_back(m->sourceOwner);
  for (ObjectId id : from) a.doc.removeLfoLink(id, pid);
  return static_cast<int>(from.size());
}

void openBoardItem(App& a) {
  if (ImGui::MenuItem(tr("Open the LFO board"), keys::label("view.lfoBoard"))) {
    if (layoutui::active() == layout::Preset::Tracker) trackerview::showPage(a, "LFO board");
    else if (!a.showLfoBoard) toggle(a);
    else a.focusWindow = "LFO board";
  }
}
bool altOwned(App& a, ParamId pid) { return pid.owner != kNoId && !lfoLinksOn(a.doc.song(), pid.owner, pid.index).empty(); }

namespace {
// The drop target on the last item (a knob or control of `pid`), the ring while a handle is dragged.
// Pick on screen: while an LFO waits for a knob, every knob that can take it shows a ring and a click links it.
void pickTarget(App& a, ParamId pid, ImVec2 c, float r, bool round, ImVec2 mn, ImVec2 mx) {
  if (gPickLfo == kNoId) return;
  const Song& s = a.doc.song();
  const SongLfo* L = s.lfo(gPickLfo);
  if (!L || !lfo::canTarget(s, pid) || pid.owner == gPickLfo) return;
  const ImU32 col = theme::withAlpha(colour(L->colour), ImGui::IsItemHovered() ? 1.0f : 0.55f);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (round) dl->AddCircle(c, r + 4, col, 32, 2.0f);
  else dl->AddRect(ImVec2(mn.x - 2, mn.y - 2), ImVec2(mx.x + 2, mx.y + 2), col, 3.0f, 0, 2.0f);
  if (ImGui::IsItemActivated()) {
    const ObjectId lid = gPickLfo;
    gPickLfo = kNoId;
    if (!lfo::link(a.doc, lid, pid)) a.notify(a.doc.lastLinkRefusal().empty() ? tr("This control cannot take an LFO") : a.doc.lastLinkRefusal(), true);
    else gSelected = lid;
  }
}

void dropTarget(App& a, ParamId pid, ImVec2 c, float r, bool round, ImVec2 mn, ImVec2 mx) {
  const ObjectId lid = dragged();
  if (lid == kNoId) return;
  const Song& s = a.doc.song();
  const SongLfo* L = s.lfo(lid);
  if (!L || !lfo::canTarget(s, pid) || pid.owner == lid) return;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImU32 col = colour(L->colour);
  bool hover = false;
  if (ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kPayload, ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
      hover = true;
      if (p->IsDelivery()) {
        if (!lfo::link(a.doc, lid, pid)) a.notify(a.doc.lastLinkRefusal().empty() ? tr("This control cannot take an LFO") : a.doc.lastLinkRefusal(), true);
        else gSelected = lid;
      } else {
        ModLink probe = lfo::makeLink(lid, pid);
        std::string why;
        const bool loop = lfo::wouldLoop(s, probe, &why);
        ImGui::BeginTooltip();
        if (loop) ImGui::TextColored(theme::vec(pal().danger), "%s", why.c_str());
        else ImGui::Text("%s %s %s %s", tr("Drop:"), lfo::number(s, lid).c_str(), tr("moves"), paramShortName(s, pid).c_str());
        ImGui::EndTooltip();
      }
    }
    ImGui::EndDragDropTarget();
  }
  if (round) {
    dl->AddCircle(c, r + 4, theme::withAlpha(col, hover ? 1.0f : 0.55f), 32, hover ? 2.5f : 1.5f);
  } else {
    dl->AddRect(ImVec2(mn.x - 2, mn.y - 2), ImVec2(mx.x + 2, mx.y + 2), theme::withAlpha(col, hover ? 1.0f : 0.55f), 3.0f, 0, hover ? 2.5f : 1.5f);
  }
}

// Alt-drag: the amount of the first LFO moving `pid` (the selected LFO's, when it is one of them).
void altDrag(App& a, ParamId pid, bool active) {
  if (!active || !ImGui::GetIO().KeyAlt) return;
  const Song& s = a.doc.song();
  const auto here = lfoLinksOn(s, pid.owner, pid.index);
  if (here.empty()) return;
  const ModLink* m = here.front();
  for (const ModLink* x : here)
    if (x->sourceOwner == gSelected) m = x;
  const ObjectId lid = m->sourceOwner;
  const float dy = ImGui::GetIO().MouseDelta.y;
  if (dy != 0 && m->mode == kModAround) {
    const float amt = std::clamp(m->amount - dy * (ImGui::GetIO().KeyShift ? 0.001f : 0.005f), -1.0f, 1.0f);
    a.doc.editLfoLink(lid, pid, "change LFO amount", [amt](ModLink& x) { x.amount = amt; }, (pid.packed() * 31u) ^ (static_cast<uint64_t>(lid) << 40) ^ 0x4C464F0000000001ull);
  }
  if (const ModLink* now = findLfoLink(s, lid, pid.owner, pid.index))
    ImGui::SetTooltip("%s %s %s", lfo::number(s, lid).c_str(), tr("amount"), amountText(*now).c_str());
}

// The swing of the LFOs moving `pid` (normalized lo..hi around `base`) and the live value.
bool swing(App& a, ParamId pid, double& lo, double& hi, double& live, ImU32& col) {
  const Song& s = a.doc.song();
  const auto here = lfoLinksOn(s, pid.owner, pid.index);
  if (here.empty()) return false;
  auto d = describeParam(s, pid);
  auto v = getParam(s, pid);
  if (!d || !v) return false;
  const double base = d->toNormalized(*v);
  double addLo = 0, addHi = 0;
  bool abs = false;
  double absLo = 1, absHi = 0;
  bool any = false;
  for (const ModLink* m : here) {
    const SongLfo* L = s.lfo(m->sourceOwner);
    if (!L || !L->on || !m->enabled) continue;
    if (!any) col = colour(L->colour);
    any = true;
    lfo::LinkShape k;
    k.amount = m->amount, k.min = m->min, k.max = m->max, k.mode = m->mode, k.direction = m->direction, k.invert = m->invert;
    k.unipolar = L->unipolar, k.curve = m->curve;
    const float vlo = std::clamp(L->unipolar ? L->offset : -L->depth + L->offset, -1.0f, 1.0f);
    const float vhi = std::clamp(L->depth + L->offset, -1.0f, 1.0f);
    const double a0 = lfo::linkValue(k, vlo), a1 = lfo::linkValue(k, vhi), am = lfo::linkValue(k, (vlo + vhi) * 0.5f);
    const double mn = std::min({a0, a1, am}), mx = std::max({a0, a1, am});
    if (m->mode == kModAbsolute) abs = true, absLo = std::min(absLo, mn), absHi = std::max(absHi, mx);
    else addLo += mn, addHi += mx;
  }
  if (!any) return false;
  const double b = abs ? absLo : base;
  lo = std::clamp((abs ? absLo : base) + addLo, 0.0, 1.0);
  hi = std::clamp((abs ? absHi : base) + addHi, 0.0, 1.0);
  (void)b;
  double lv = 0;
  live = a.builder.lfoValue(pid, lv) ? d->toNormalized(lv) : -1.0;
  return true;
}
}  // namespace

void knobOverlay(App& a, ParamId pid, ImVec2 c, float radius, bool active) {
  if (pid.owner == kNoId) return;
  dropTarget(a, pid, c, radius, true, ImVec2(), ImVec2());
  pickTarget(a, pid, c, radius, true, ImVec2(), ImVec2());
  altDrag(a, pid, active);
  if (!showOnKnobs(a)) return;
  double lo = 0, hi = 0, live = -1;
  ImU32 col = 0;
  if (!swing(a, pid, lo, hi, live, col)) return;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float a0 = kPi * 0.75f, a1 = kPi * 2.25f, rr = radius + 3.0f;
  const float s0 = a0 + static_cast<float>(lo) * (a1 - a0), s1 = a0 + static_cast<float>(hi) * (a1 - a0);
  const float stepA = std::max(0.12f, 4.0f / rr);
  for (float t = s0; t <= s1 + 1e-4f; t += stepA) dl->AddCircleFilled(ImVec2(c.x + std::cos(t) * rr, c.y + std::sin(t) * rr), 1.1f, theme::withAlpha(col, 0.85f), 6);
  if (live >= 0) {
    const float an = a0 + static_cast<float>(std::clamp(live, 0.0, 1.0)) * (a1 - a0);
    const ImVec2 dp(c.x + std::cos(an) * rr, c.y + std::sin(an) * rr);
    dl->AddCircleFilled(dp, 3.2f, col, 12);
    dl->AddCircle(dp, 3.2f, pal().surface950, 12, 1.0f);
  }
}

void controlOverlay(App& a, ParamId pid, ImVec2 mn, ImVec2 mx, bool active) {
  if (pid.owner == kNoId) return;
  const ImVec2 c((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f);
  dropTarget(a, pid, c, std::min(mx.x - mn.x, mx.y - mn.y) * 0.5f, false, mn, mx);
  pickTarget(a, pid, c, 0, false, mn, mx);
  altDrag(a, pid, active);
  if (!showOnKnobs(a)) return;
  double lo = 0, hi = 0, live = -1;
  ImU32 col = 0;
  if (!swing(a, pid, lo, hi, live, col)) return;
  // Along the control's foot: the swing dotted, the live value a dot (the editor's own drawing stays).
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float y = mx.y + 2, x0 = mn.x + 2, x1 = mx.x - 2;
  for (float x = x0 + static_cast<float>(lo) * (x1 - x0); x <= x0 + static_cast<float>(hi) * (x1 - x0) + 0.01f; x += 4) dl->AddCircleFilled(ImVec2(x, y), 1.0f, col, 6);
  if (live >= 0) dl->AddCircleFilled(ImVec2(x0 + static_cast<float>(std::clamp(live, 0.0, 1.0)) * (x1 - x0), y), 2.6f, col, 10);
}

// ---- dev ------------------------------------------------------------------
void devCommand(App& a, const std::vector<std::string>& args, int frame) {
  const Song& s = a.doc.song();
  const std::string sub = args.empty() ? "report" : args[0];
  auto lfoAt = [&](size_t i) -> ObjectId {
    if (args.size() <= i) return kNoId;
    const int n = std::atoi(args[i].c_str());
    return n >= 1 && n <= static_cast<int>(s.lfos.size()) ? s.lfos[static_cast<size_t>(n - 1)].id : kNoId;
  };
  auto pidAt = [&](size_t i) -> ParamId {
    if (args.size() <= i + 1) return {};
    return {static_cast<ObjectId>(std::strtoull(args[i].c_str(), nullptr, 10)), static_cast<uint32_t>(std::strtoul(args[i + 1].c_str(), nullptr, 10))};
  };
  if (sub == "new") {  // lfo new [chaos|random]
    const LfoKind kind = args.size() > 1 && args[1] == "chaos" ? LfoKind::Chaos : args.size() > 1 && args[1] == "random" ? LfoKind::Random : LfoKind::Lfo;
    const ObjectId id = lfo::addNew(a.doc, kind);
    std::printf("[lfo] new %llu\n", static_cast<unsigned long long>(id));
  } else if (sub == "link") {
    const ObjectId id = lfoAt(1);
    const ParamId pid = pidAt(2);
    const float amt = args.size() > 4 ? static_cast<float>(std::atof(args[4].c_str())) : 0.25f;
    const bool ok = id != kNoId && lfo::link(a.doc, id, pid, amt);
    std::printf("[lfo] link %s %s\n", ok ? "ok" : "refused", a.doc.lastLinkRefusal().c_str());
  } else if (sub == "preset" && args.size() > 2) {  // lfo preset <n> <preset index>
    const ObjectId id = lfoAt(1);
    const int k = std::atoi(args[2].c_str());
    if (id != kNoId && k >= 0 && k < static_cast<int>(lfo::presets().size()))
      a.doc.editLfo(id, "apply LFO preset", [k](SongLfo& x) { lfo::applyPreset(x, lfo::presets()[static_cast<size_t>(k)].lfo); });
  } else if (sub == "name" && args.size() > 2) {  // lfo name <n> <name>
    const std::string nm = args[2];
    if (const ObjectId id = lfoAt(1); id != kNoId) a.doc.editLfo(id, "rename LFO", [nm](SongLfo& x) { x.name = nm; });
  } else if (sub == "shape" && args.size() > 2) {  // lfo shape <n> <0..7>
    const int k = std::clamp(std::atoi(args[2].c_str()), 0, static_cast<int>(LfoShape::Count) - 1);
    if (const ObjectId id = lfoAt(1); id != kNoId)
      a.doc.editLfo(id, "change LFO shape", [k](SongLfo& x) {
        x.shape = static_cast<LfoShape>(k);
        if (x.shape == LfoShape::Drawn && x.points.empty()) x.points = lfo::defaultPoints();
      });
  } else if (sub == "select") {
    gSelected = lfoAt(1);
  } else if (sub == "open") {
    if (layoutui::active() == layout::Preset::Tracker) trackerview::showPage(a, "LFO board");
    else if (!a.showLfoBoard) toggle(a);
  } else if (sub == "detail") {
    a.showLfoDetail = true;
  } else if (sub == "compact") {
    gView = args.size() > 1 && args[1] == "1" ? 1 : 0;
  } else if (sub == "popout") {
    gPopOut = true;
  } else if (sub == "filter") {
    gFilter = args.size() > 1 ? std::clamp(std::atoi(args[1].c_str()), 0, 2) : 0;
  } else if (sub == "addtarget") {
    gAddLfo = lfoAt(1), gOpenAdd = gAddLfo != kNoId, gAddSearch[0] = 0;
    for (const DevCard& c : gDevCards)
      if (c.id == gAddLfo) gPopAt = ImVec2(c.add.x - 40, c.add.y + 14);
    if (args.size() > 2) std::snprintf(gAddSearch, sizeof gAddSearch, "%s", args[2].c_str());
  } else if (sub == "settings") {
    gSetLfo = lfoAt(1), gSetTarget = pidAt(2), gOpenSettings = gSetLfo != kNoId;
    for (const DevCard& c : gDevCards)
      for (const auto& [p, at] : c.chips)
        if (c.id == gSetLfo && p == gSetTarget) gPopAt = ImVec2(at.x - 60, at.y + 14);
  } else if (sub == "pick") {  // lfo pick <n>: Pick on screen for LFO n
    gPickLfo = lfoAt(1);
  } else if (sub == "toggle") {
    toggle(a);
  } else {
    std::printf("[lfo] frame %d lfos %zu targets %d open %d detail %d compact %d cols %d win %.0f %.0f %.0f %.0f undo %zu\n", frame, s.lfos.size(),
                targetCount(s), a.showLfoBoard ? 1 : 0, a.showLfoDetail ? 1 : 0, gDevCompact ? 1 : 0, gDevCols, static_cast<double>(gDevWin0.x),
                static_cast<double>(gDevWin0.y), static_cast<double>(gDevWin1.x), static_cast<double>(gDevWin1.y), a.doc.history().position());
    for (const DevCard& c : gDevCards) {
      const SongLfo* l = s.lfo(c.id);
      if (!l) continue;
      const auto rt = a.builder.lfoRuntime(c.id);
      std::printf("[lfo] card %s id %llu rect %.0f %.0f %.0f %.0f handle %.0f %.0f add %.0f %.0f depth %.0f %.0f shape %s rate %s restart %s out %.3f running %d\n",
                  lfo::label(s, c.id).c_str(), static_cast<unsigned long long>(c.id), static_cast<double>(c.mn.x), static_cast<double>(c.mn.y),
                  static_cast<double>(c.mx.x), static_cast<double>(c.mx.y), static_cast<double>(c.handle.x), static_cast<double>(c.handle.y),
                  static_cast<double>(c.add.x), static_cast<double>(c.add.y), static_cast<double>(c.depth.x), static_cast<double>(c.depth.y),
                  lfo::shapeName(l->shape), lfo::rateLabel(*l).c_str(),
                  lfo::restartName(l->restart), rt ? static_cast<double>(rt->display.load()) : 0.0, rt && rt->running.load() ? 1 : 0);
      for (const auto& [pid, at] : c.chips) {
        const ModLink* m = findLfoLink(s, c.id, pid.owner, pid.index);
        std::printf("[lfo]   chip %s %s at %.0f %.0f mode %d amount %.3f dir %d invert %d enabled %d\n", lfo::targetLabel(s, pid).c_str(),
                    m ? amountText(*m).c_str() : "?", static_cast<double>(at.x), static_cast<double>(at.y), m ? m->mode : -1,
                    m ? static_cast<double>(m->amount) : 0.0, m ? m->direction : -1, m && m->invert ? 1 : 0, m && m->enabled ? 1 : 0);
      }
    }
  }
  std::fflush(stdout);
}

}  // namespace gl::lfoui
