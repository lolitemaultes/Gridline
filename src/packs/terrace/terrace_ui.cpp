// Gridline — "terrace" pack: the Terrace editor. Four (or three) band rows stacked under the
// crossover, each a full step lane with its Steps, Size, Swing, Depth and Curve and the lane tabs
// (Vol, Cut, Pan); the modulation, the route list and the sidechain in a column on the right; the
// patterns, the chain and the global settings along the bottom. Drawn at 1240 x 780 form units,
// scaled to the window.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "../../app/App.h"
#include "../../app/DrawSmoothUi.h"
#include "../../app/LayoutAudit.h"
#include "../../app/PluginSkin.h"
#include "../../app/PluginUI.h"
#include "../../app/Theme.h"
#include "../../app/Widgets.h"
#include "../../core/I18n.h"
#include "../../core/MenuOps.h"
#include "../../core/Params.h"
#include "../../core/Presets.h"
#include "../../engine/Program.h"
#include "../PacksUi.h"
#include "../tideline/tideline_source.h"
#include "terrace.h"

namespace gl::packs::terrace {

namespace {

using i18n::tr;
constexpr float kW = 1240, kH = 780;
constexpr float kTextScale = 1.06f;  // the design's type sizes, as Gridline's fonts draw them
constexpr float kPi = 3.14159265f;

// The band rows' grid (form units). The right-hand column lines up with the four-band grid: the
// modulation panel ends with the second band row, the routes start with the third, and the
// follower's knobs sit on the second row's knob line.
constexpr float kRowsTop = 186, kRowsH = 504, kRowGap = 8;
float rowH(int bands) { return kRowsH / static_cast<float>(bands) - kRowGap; }
float rowTop(int b, int bands) { return kRowsTop + (rowH(bands) + kRowGap) * static_cast<float>(b); }
// The knob centre line of a band row at y, h tall (the lane tabs' text sits on its value line).
float knobLine(float y, float h) { return y + 52 + 14 + std::max(0.0f, (h - 118) * 0.35f); }

// ------------------------------------------------------------------ look

struct Look {
  bool dark = true;
  ImU32 top, win, panel, well, field, line, line2, btn, seg;
  ImU32 text, dim, faint, acc, accInk, led, knob, knobTrack, pointer;
  ImU32 band[kBands], lfo[kLfos], fol, sc;
};

Look look() {
  Look L;
  const theme::Palette& P = theme::pal();
  L.dark = P.dark;
  if (P.dark) {
    L.top = theme::rgb(0x262C30), L.win = theme::rgb(0x353D42), L.panel = theme::rgb(0x434C51), L.well = theme::rgb(0x30373B);
    L.field = theme::rgb(0x23282B), L.line = theme::rgb(0x293034), L.line2 = theme::rgb(0x5A6368), L.btn = theme::rgb(0x434C51);
    L.seg = theme::rgb(0x353D42), L.text = P.text, L.dim = P.textDim, L.faint = P.textFaint;
    L.knob = theme::mix(P.text, P.textDim, 0.08f), L.knobTrack = theme::rgb(0x2A3034), L.pointer = theme::rgb(0x262C30);
  } else {
    L.top = P.chrome, L.win = P.surface900, L.panel = P.surface800, L.well = P.well, L.field = P.field, L.line = P.line, L.line2 = P.lineStrong;
    L.btn = P.surface700, L.seg = P.surface900, L.text = P.text, L.dim = P.textDim, L.faint = P.textFaint;
    L.knob = theme::rgb(0x4F595E), L.knobTrack = theme::withAlpha(P.text, 0.16f), L.pointer = theme::mix(P.paper, P.text, 0.04f);
  }
  L.acc = theme::rgb(0xFF9E3D), L.accInk = theme::rgb(0x1B1206);
  L.led = theme::lit(theme::rgb(0xA8E44A));
  const uint32_t bc[kBands] = {0x4FCFE0, 0x86E0C2, 0xF2C451, 0xF58BB8};
  for (int b = 0; b < kBands; ++b) L.band[b] = P.dark ? theme::rgb(bc[b]) : theme::mix(theme::rgb(bc[b]), theme::rgb(0x1E2427), 0.28f);
  L.lfo[0] = theme::lit(theme::rgb(0xD673EA)), L.lfo[1] = theme::lit(theme::rgb(0xB58CFF));
  L.fol = theme::lit(theme::rgb(0x9BD65A)), L.sc = theme::lit(theme::rgb(0x6E9BFF));
  return L;
}

ImU32 sourceColour(const Look& L, int src) {
  switch (src) {
    case SrcLfo1: return L.lfo[0];
    case SrcLfo2: return L.lfo[1];
    case SrcFollower: return L.fol;
    case SrcSidechain: return L.sc;
    default: return L.faint;
  }
}
const char* sourceName(int src) {
  static const char* const n[SrcCount] = {i18n::N_("Off"), i18n::N_("LFO 1"), i18n::N_("LFO 2"), i18n::N_("Follower"), i18n::N_("Sidechain")};
  return src >= 0 && src < SrcCount ? n[src] : "";
}

// ------------------------------------------------------------------ editor state

struct Ui {
  int lane[kBands] = {0, 0, 0, 0};  // the lane each band row shows
  int sel = 1;                      // the selected band
  int mode = 0;                     // what drawing edits: 0 level, 1 chance, 2 repeats
  int routeTop = 0;                 // the route list's scroll
  bool haveClip = false;
  float clipVal[kBands][kLanes][kSteps] = {};
  uint8_t clipChance[kBands][kLanes][kSteps] = {}, clipHits[kBands][kLanes][kSteps] = {};
  int lastPreset = -1;
  std::string presetName;
  // the step menu (right-click or right-drag over a lane): the steps it acts on
  bool rDrag = false;
  int mBand = -1, mLane = 0, mPat = 0, mK0 = 0, mK1 = 0;
  char saveName[64] = {};
};
std::map<uint64_t, Ui>& uis() {
  static std::map<uint64_t, Ui> m;
  return m;
}

struct BankCache {
  std::string data;
  std::unique_ptr<Bank> bank;
};
const Bank& bankOf(uint64_t key, const std::string& data) {
  static std::map<uint64_t, BankCache> m;
  BankCache& c = m[key];
  if (!c.bank || c.data != data) {
    if (!c.bank) c.bank = std::make_unique<Bank>();
    parse(data.c_str(), *c.bank);
    c.data = data;
  }
  return *c.bank;
}

struct Ctx {
  App& a;
  const PluginTarget& t;
  skin::Form F;
  ImDrawList* dl;
  const PluginState& st;
  Ui& u;
  const Look& L;
  const Bank& bank;
  Live live;
  bool hasLive = false;
  int bands = 4;
  int readouts = 0;  // lanes drawn with their ruler readout this frame (the dev report)
  ImVec2 p(float x, float y) const { return F.p(x, y); }
  float s(float v) const { return F.u(v); }
};

struct Dev {
  bool report = false;
  int preset = -1;  // "terrace preset <n>": load factory preset n on the next frame
  bool presetList = false;  // "terrace presets": open the preset list
} gDev;

const PluginInfo* info() { return findPlugin(kType); }

double val(const Ctx& c, int k) {
  const PluginInfo* in = info();
  if (!in || k < 0 || k >= TrCount) return 0;
  const double raw = static_cast<size_t>(k) < c.st.params.size() ? c.st.params[static_cast<size_t>(k)] : in->params[static_cast<size_t>(k)].def;
  return in->params[static_cast<size_t>(k)].clamp(raw);
}
int ival(const Ctx& c, int k) { return static_cast<int>(std::lround(val(c, k))); }
void setParam(Ctx& c, int k, double v, bool merge = false) { pui::setParamValue(c.a, c.t, k, v, merge); }

// One undo step with these parameters changed (route edits set several at once).
void setParams(Ctx& c, std::initializer_list<std::pair<int, double>> kv, const char* what) {
  PluginState ns = c.st;
  normalizePluginState(ns);
  for (auto [k, v] : kv)
    if (k >= 0 && static_cast<size_t>(k) < ns.params.size()) ns.params[static_cast<size_t>(k)] = static_cast<float>(v);
  if (ns.params != c.st.params) setPluginState(c.a, c.t, ns, what);
}

void commitBank(Ctx& c, const Bank& b, const char* what, uint64_t mergeKey = 0) {
  PluginState ns = c.st;
  ns.data = withBank(c.st.data, b);
  if (ns.data != c.st.data) setPluginState(c.a, c.t, ns, what, mergeKey);
}

// ------------------------------------------------------------------ text and shapes

ImFont* fReg() { return app().fontRegular ? app().fontRegular : ImGui::GetFont(); }
ImFont* fBold() { return app().fontBold ? app().fontBold : fReg(); }
ImFont* fMono() { return app().fontMono ? app().fontMono : fReg(); }
ImFont* fMonoBold() { return app().fontMonoBold ? app().fontMonoBold : fMono(); }

ImVec2 textSize(const Ctx& c, ImFont* f, float size, const char* s, float spacing = 0) {
  const ImVec2 t = f->CalcTextSizeA(c.s(size * kTextScale), 1e9f, 0, s);
  return ImVec2(t.x / c.F.s + spacing * static_cast<float>(std::max<size_t>(1, std::strlen(s)) - 1), t.y / c.F.s);
}
// Text with its top-left at (x, y) in form units; `spacing` adds letter spacing (caps captions).
void text(Ctx& c, ImFont* f, float size, float x, float y, ImU32 col, const char* s, float spacing = 0) {
  const float px = c.s(size * kTextScale);
  if (spacing <= 0) {
    const ImVec2 p = c.p(x, y);
    c.dl->AddText(f, px, ImVec2(std::round(p.x), std::round(p.y)), col, s);
    return;
  }
  float cx = x;
  for (const char* q = s; *q;) {
    const char* e = q + 1;
    while (*e && (static_cast<unsigned char>(*e) & 0xC0) == 0x80) ++e;  // one UTF-8 character
    const ImVec2 p = c.p(cx, y);
    c.dl->AddText(f, px, ImVec2(std::round(p.x), std::round(p.y)), col, q, e);
    cx += f->CalcTextSizeA(px, 1e9f, 0, q, e).x / c.F.s + spacing;
    q = e;
  }
}
void textC(Ctx& c, ImFont* f, float size, float cx, float y, ImU32 col, const char* s, float spacing = 0) {
  text(c, f, size, cx - textSize(c, f, size, s, spacing).x * 0.5f, y, col, s, spacing);
}
// The font's ascent at `size` in form units (whole pixels at the drawn size).
float ascent(const Ctx& c, ImFont* f, float size) { return f->GetFontBaked(c.s(size * kTextScale))->Ascent / c.F.s; }
// Text whose baseline sits at `base` (form units): texts of any font and size on one baseline line
// up to the pixel (the baseline is snapped, the top is the baseline less the font's own ascent).
void textB(Ctx& c, ImFont* f, float size, float x, float base, ImU32 col, const char* s, float spacing = 0) {
  const float bp = std::floor(c.p(0, base).y + 0.502f);  // ties (half pixels at x1.5) always round down the page
  text(c, f, size, x, (bp - c.F.o.y) / c.F.s - ascent(c, f, size), col, s, spacing);
}
// The baseline of a row of text centred in a box of height h at y: the one Inter Regular at
// `ref` would have, so every label, button, field and switch of a row shares it.
float rowBase(const Ctx& c, float y, float h, float ref = 11) { return y + (h - ref * kTextScale) * 0.5f + ascent(c, fReg(), ref); }
// Vertically centred in a box of height h at y, on the row's baseline.
void textV(Ctx& c, ImFont* f, float size, float x, float y, float h, ImU32 col, const char* s, float spacing = 0, float ref = 11) {
  textB(c, f, size, x, rowBase(c, y, h, ref), col, s, spacing);
}
void rect(Ctx& c, float x, float y, float w, float h, ImU32 col, float r = 0, ImDrawFlags fl = 0) {
  c.dl->AddRectFilled(c.p(x, y), c.p(x + w, y + h), col, c.s(r), fl);
}
void rectLine(Ctx& c, float x, float y, float w, float h, ImU32 col, float r = 0, float th = 1) {
  c.dl->AddRect(c.p(x, y), c.p(x + w, y + h), col, c.s(r), 0, std::max(1.0f, c.s(th)));
}

// ------------------------------------------------------------------ controls (form units)

bool hit(Ctx& c, const char* id, float x, float y, float w, float h, bool* hov = nullptr) {
  ImGui::SetCursorScreenPos(c.p(x, y));
  ImGui::InvisibleButton(id, ImVec2(std::max(1.0f, c.s(w)), std::max(1.0f, c.s(h))));
  if (hov) *hov = ImGui::IsItemHovered();
  return ImGui::IsItemClicked(ImGuiMouseButton_Left);
}

// A button as the design draws it (.btn): slate face, the accent face when on.
bool button(Ctx& c, const char* id, float x, float y, float w, float h, const char* label, bool on, float size = 11, const char* tip = nullptr) {
  bool hov = false;
  const bool clicked = hit(c, id, x, y, w, h, &hov);
  const Look& L = c.L;
  const ImU32 face = on ? L.acc : hov ? theme::mix(L.btn, L.text, 0.08f) : L.btn;
  rect(c, x, y, w, h, face, 4);
  rectLine(c, x, y, w, h, on ? L.acc : L.line, 4);
  ImFont* f = on ? fBold() : fReg();
  const ImVec2 ts = textSize(c, f, size, label);
  textV(c, f, size, x + (w - ts.x) * 0.5f, y, h, on ? L.accInk : L.text, label);
  if (hov && tip) c.a.setHint(tip);
  return clicked;
}

// A segmented switch (.seg) over a choice parameter, or over `cur` when param < 0. Returns the
// width; *picked gets a click.
float segmented(Ctx& c, const char* id, float x, float y, float h, const char* const* labels, int count, int cur, int* picked, float size = 11) {
  const Look& L = c.L;
  float w[8], total = 0;
  for (int i = 0; i < count && i < 8; ++i) w[i] = textSize(c, fReg(), size, labels[i]).x + 16, total += w[i];
  total += static_cast<float>(count - 1);
  rect(c, x, y, total, h, L.seg, 4);
  float cx = x;
  ImGui::PushID(id);
  for (int i = 0; i < count && i < 8; ++i) {
    ImGui::PushID(i);
    bool hov = false;
    if (hit(c, "##seg", cx, y, w[i], h, &hov) && picked) *picked = i;
    ImGui::PopID();
    const ImDrawFlags fl = count == 1 ? 0 : i == 0 ? ImDrawFlags_RoundCornersLeft : i == count - 1 ? ImDrawFlags_RoundCornersRight : ImDrawFlags_RoundCornersNone;
    if (i == cur) rect(c, cx, y, w[i], h, L.acc, 4, fl);
    else if (hov) rect(c, cx, y, w[i], h, theme::withAlpha(L.text, 0.07f), 4, fl);
    if (i > 0) c.dl->AddLine(c.p(cx - 0.5f, y), c.p(cx - 0.5f, y + h), L.line, std::max(1.0f, c.s(1)));
    ImFont* f = i == cur ? fBold() : fReg();
    const ImVec2 ts = textSize(c, f, size, labels[i]);
    textV(c, f, size, cx + (w[i] - ts.x) * 0.5f, y, h, i == cur ? L.accInk : L.dim, labels[i]);
    cx += w[i] + 1;
  }
  ImGui::PopID();
  rectLine(c, x, y, total, h, L.line, 4);
  return total;
}
float segParam(Ctx& c, int k, float x, float y, float h, const char* const* labels, int count, float size = 11) {
  int picked = -1;
  char id[16];
  std::snprintf(id, sizeof id, "seg%d", k);
  const float w = segmented(c, id, x, y, h, labels, count, ival(c, k), &picked, size);
  if (picked >= 0 && picked != ival(c, k)) setParam(c, k, picked);
  return w;
}
float segWidth(const Ctx& c, const char* const* labels, int count, float size = 11) {
  float total = static_cast<float>(count - 1);
  for (int i = 0; i < count; ++i) total += textSize(c, fReg(), size, labels[i]).x + 16;
  return total;
}

// A combo box face (.combo): field, text, chevron. True on a click.
bool comboFace(Ctx& c, const char* id, float x, float y, float w, float h, const char* label, bool mono = true, ImU32 dot = 0) {
  bool hov = false;
  const bool clicked = hit(c, id, x, y, w, h, &hov);
  const Look& L = c.L;
  rect(c, x, y, w, h, hov ? theme::mix(L.field, L.text, 0.05f) : L.field, 4);
  rectLine(c, x, y, w, h, L.line, 4);
  float tx = x + 7;
  if (dot) c.dl->AddCircleFilled(c.p(x + 10, y + h * 0.5f), c.s(3.5f), dot, 12), tx = x + 18;
  c.dl->PushClipRect(c.p(x, y), c.p(x + w - 16, y + h), true);
  textV(c, mono ? fMono() : fReg(), 11, tx, y, h, L.text, label);
  c.dl->PopClipRect();
  const ImVec2 a0 = c.p(x + w - 11, y + h * 0.5f - 1.5f);
  const float d = c.s(4);
  c.dl->AddTriangleFilled(ImVec2(a0.x - d, a0.y), ImVec2(a0.x + d, a0.y), ImVec2(a0.x, a0.y + d), L.dim);
  return clicked;
}

// An LED toggle (.led): the lamp and its label.
bool led(Ctx& c, const char* id, float x, float y, const char* label, bool on, float* width = nullptr) {
  const Look& L = c.L;
  const float w = textSize(c, fReg(), 11, label).x + 15, h = 16;
  if (width) *width = w;
  bool hov = false;
  const bool clicked = hit(c, id, x, y - 1, w, h + 2, &hov);
  const ImVec2 d = c.p(x + 4, y + h * 0.5f);
  if (on) c.dl->AddCircleFilled(d, c.s(6.5f), theme::withAlpha(L.led, 0.28f), 16);
  c.dl->AddCircleFilled(d, c.s(4), on ? L.led : L.dark ? theme::rgb(0x20262A) : theme::mix(L.panel, L.text, 0.22f), 16);
  c.dl->AddCircle(d, c.s(4), L.line, 16, std::max(1.0f, c.s(1)));
  textV(c, fReg(), 11, x + 13, y, h, on ? L.text : (hov ? L.text : L.dim), label);
  return clicked;
}

// A source chip (.chip): coloured dot and name. True on a click.
bool chip(Ctx& c, const char* id, float x, float y, float w, const char* label, ImU32 dot, bool caps = false) {
  const Look& L = c.L;
  bool hov = false;
  const bool clicked = hit(c, id, x, y, w, 20, &hov);
  rect(c, x, y, w, 20, hov ? theme::mix(L.win, L.text, 0.06f) : L.win, 10);
  rectLine(c, x, y, w, 20, L.line, 10);
  c.dl->AddCircleFilled(c.p(x + 11, y + 10), c.s(3.5f), dot, 12);
  textV(c, caps ? fMono() : fReg(), caps ? 10 : 10.5f, x + 20, y, 20, L.text, label, caps ? 0.6f : 0);
  return clicked;
}
float chipWidth(const Ctx& c, const char* label, bool caps = false) { return textSize(c, caps ? fMono() : fReg(), caps ? 10 : 10.5f, label, caps ? 0.6f : 0).x + 30; }

// ---- values as the editor writes them

std::string fmtHz(double f) {
  char b[24];
  if (f >= 1000) std::snprintf(b, sizeof b, f >= 9950 ? "%.0f k" : "%.3g k", f / 1000.0);
  else std::snprintf(b, sizeof b, "%.0f", f);
  return b;
}

std::string display(const Ctx& c, int k, double v) {
  char b[48];
  const PluginInfo* in = info();
  if (k >= TrBand0 && k < TrLfo0) {
    const int bk = (k - TrBand0) % BpCount;
    if (bk == BpGain) return std::snprintf(b, sizeof b, "%+.1f dB", v), b;
    if (bk < kLanes * LpCount) {
      const int lp = bk % LpCount;
      if (lp == LpSwing || lp == LpDepth) return std::snprintf(b, sizeof b, "%.0f%%", v * 100), b;
      if (lp == LpCurve) return std::snprintf(b, sizeof b, "%.0f", v * 100), b;
    }
  }
  if (k >= TrLfo0 && k < TrLfo0 + kLfos * LfCount) {
    const int i = (k - TrLfo0) / LfCount, lk = (k - TrLfo0) % LfCount;
    if (lk == LfRate || lk == LfHz) {
      if (ival(c, lfoParam(i, LfSync)) != 0) return kLfoRateNames[std::clamp(ival(c, lfoParam(i, LfRate)), 0, kLfoRates - 1)];
      return std::snprintf(b, sizeof b, val(c, lfoParam(i, LfHz)) < 10 ? "%.2f Hz" : "%.1f Hz", val(c, lfoParam(i, LfHz))), b;
    }
    if (lk == LfShape) {
      static const char* const sh[ShCount] = {i18n::N_("Sine"), i18n::N_("Tri"), i18n::N_("Saw"), i18n::N_("Ramp"), i18n::N_("Square"), i18n::N_("Steps"), i18n::N_("Drift")};
      return tr(sh[std::clamp(static_cast<int>(std::lround(v)), 0, ShCount - 1)]);
    }
    if (lk == LfPhase) return std::snprintf(b, sizeof b, "%.0f\xC2\xB0", v), b;
    if (lk == LfAmount) return std::snprintf(b, sizeof b, "%.0f%%", v * 100), b;
  }
  switch (k) {
    case TrFolAttack:
    case TrFolRelease:
    case TrScRelease: return std::snprintf(b, sizeof b, v < 10 ? "%.1f ms" : "%.0f ms", v), b;
    case TrFolGain: return std::snprintf(b, sizeof b, "%+.0f dB", v), b;
    case TrScThreshold: return std::snprintf(b, sizeof b, "%.0f dB", v), b;
    case TrFolAmount:
    case TrScAmount:
    case TrMix: return std::snprintf(b, sizeof b, "%.0f%%", v * 100), b;
    case TrSmooth: return std::snprintf(b, sizeof b, v < 10 ? "%.1f ms" : "%.0f ms", v), b;
    case TrOutput: return std::snprintf(b, sizeof b, "%.1f dB", v), b;
    default: break;
  }
  return in ? in->params[static_cast<size_t>(k)].format(v) : std::string();
}

// The route target of a parameter control (TgNone when it has none).
int targetOf(int k, int laneShown) {
  if (k >= TrBand0 && k < TrLfo0) {
    const int b = (k - TrBand0) / BpCount, bk = (k - TrBand0) % BpCount;
    if (bk == BpGain) return bandTarget(b, TbGain);
    if (bk < kLanes * LpCount) {
      const int lp = bk % LpCount;
      if (lp == LpSwing) return bandTarget(b, TbSwing);
      if (lp == LpDepth) return bandTarget(b, TbVolDepth + laneShown);
      if (lp == LpSize) return bandTarget(b, TbSize);
    }
  }
  if (k == TrSplit1) return TgSplit1;
  if (k == TrSplit2) return TgSplit2;
  if (k == TrSplit3) return TgSplit3;
  if (k == TrMix) return TgMix;
  if (k == TrOutput) return TgOutput;
  return TgNone;
}

// The first route onto `target` (-1: none).
int routeOnto(const Ctx& c, int target) {
  if (target == TgNone) return -1;
  for (int r = 0; r < kRoutes; ++r)
    if (ival(c, routeParam(r, RtSource)) != SrcOff && ival(c, routeParam(r, RtTarget)) == target) return r;
  return -1;
}
int freeRoute(const Ctx& c) {
  for (int r = 0; r < kRoutes; ++r)
    if (ival(c, routeParam(r, RtSource)) == SrcOff) return r;
  return -1;
}
void addRoute(Ctx& c, int src, int target, double amount) {
  const int r = freeRoute(c);
  if (r < 0 || target == TgNone) return;
  setParams(c, {{routeParam(r, RtSource), src}, {routeParam(r, RtTarget), target}, {routeParam(r, RtAmount), amount}}, tr("Add route"));
}

// A modulation source dropped on a control: a route from it onto the control's target.
void dropTarget(Ctx& c, int target) {
  if (target == TgNone || !ImGui::BeginDragDropTarget()) return;
  if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("GL_TERRACE_SRC")) {
    const int src = *static_cast<const int*>(pl->Data);
    if (src > SrcOff && src < SrcCount) addRoute(c, src, target, 0.5);
  }
  ImGui::EndDragDropTarget();
}
void dragSource(Ctx& c, int src, const char* label) {
  if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
    ImGui::SetDragDropPayload("GL_TERRACE_SRC", &src, sizeof src);
    ImGui::TextUnformatted(label);
    ImGui::EndDragDropSource();
  }
  (void)c;
}

// A parameter knob (as the design draws it): the track arc, the value arc in `col`, the light face
// and its pointer; the value and the label under it. A route onto it shows as a thin outer arc.
void knob(Ctx& c, int k, float cx, float cy, float r, ImU32 col, const char* label, int laneShown = 0) {
  const PluginInfo* in = info();
  if (!in) return;
  const ParamDesc& d = in->params[static_cast<size_t>(k)];
  double v = val(c, k);
  const ImVec2 C = c.p(cx, cy);
  const float R = c.s(r);
  ImGui::PushID(k);
  ImGui::SetCursorScreenPos(ImVec2(C.x - R - c.s(3), C.y - R - c.s(3)));
  ImGui::InvisibleButton("##k", ImVec2(R * 2 + c.s(6), R * 2 + c.s(6)));
  pui::dragParamItem(c.a, c.t.pid(k), d, v, 160.0f, false);
  const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
  const int target = targetOf(k, laneShown);
  dropTarget(c, target);
  ImGui::PopID();
  const bool bip = d.min < 0 && d.max > 0;
  const float a0 = kPi * 0.75f, a1 = kPi * 2.25f;
  const float n = static_cast<float>(d.toNormalized(v));
  const float an = a0 + (a1 - a0) * std::clamp(n, 0.0f, 1.0f);
  const float from = bip ? kPi * 1.5f : a0;
  const float th = std::max(1.0f, c.s(3));
  c.dl->PathArcTo(C, R, a0, a1, 40);
  c.dl->PathStroke(c.L.knobTrack, 0, th);
  if (std::fabs(an - from) > 0.01f) {
    c.dl->PathArcTo(C, R, std::min(from, an), std::max(from, an), 32);
    c.dl->PathStroke(col, 0, th);
  }
  const int route = routeOnto(c, target);
  if (route >= 0) {  // the route's reach around the knob
    const double amt = val(c, routeParam(route, RtAmount));
    const float ae = a0 + (a1 - a0) * std::clamp(n + static_cast<float>(amt) * 0.5f, 0.0f, 1.0f);
    c.dl->PathArcTo(C, R + c.s(3), std::min(an, ae), std::max(an, ae), 24);
    c.dl->PathStroke(sourceColour(c.L, ival(c, routeParam(route, RtSource))), 0, std::max(1.0f, c.s(1.6f)));
  }
  const float fr = R - c.s(4);
  c.dl->AddCircleFilled(C, fr, hot ? theme::mix(c.L.knob, c.L.dark ? theme::pal().paper : theme::pal().ink, 0.08f) : c.L.knob, 32);
  const ImVec2 u(std::cos(an), std::sin(an));
  c.dl->AddLine(C, ImVec2(C.x + u.x * (R - c.s(6)), C.y + u.y * (R - c.s(6))), c.L.pointer, std::max(1.0f, c.s(2)));
  if (label) {
    const std::string vs = display(c, k, v);
    // on snapped baselines, so the lane tabs beside a band's knobs share the value line exactly
    const float base = cy + r + 4 + ascent(c, fReg(), 10.5f);
    textB(c, fReg(), 10.5f, cx - textSize(c, fReg(), 10.5f, vs.c_str()).x * 0.5f, base, c.L.text, vs.c_str());
    textB(c, fReg(), 10.5f, cx - textSize(c, fReg(), 10.5f, label).x * 0.5f, base + 13, c.L.dim, label);
  }
}

// ------------------------------------------------------------------ panels

void panel(Ctx& c, float x, float y, float w, float h, const char* title, const char* note, ImU32 border = 0) {
  const Look& L = c.L;
  audit::group(c.p(x, y), c.p(x + w, y + h), "panel");  // its rows are compared inside it
  rect(c, x, y, w, h, L.panel, 6);
  c.dl->AddLine(c.p(x, y + 22), c.p(x + w, y + 22), L.line, std::max(1.0f, c.s(1)));
  textV(c, fBold(), 10.5f, x + 10, y, 22, L.dim, title, 1.0f);
  if (note && *note) {
    const float right = x + w - 10;
    textV(c, fMono(), 10.5f, right - textSize(c, fMono(), 10.5f, note, 0.2f).x, y, 22, L.faint, note, 0.2f);
  }
  rectLine(c, x, y, w, h, border ? border : L.line, 6);
}

// ------------------------------------------------------------------ the top bar

void loadFactory(Ctx& c, int index) {
  const PluginInfo* in = info();
  if (!in || index < 0 || static_cast<size_t>(index) >= in->presets.size()) return;
  PluginState s = presetStateKeepingData(*in, static_cast<size_t>(index), c.st.data);
  s.data = withNoteChannel(s.data, noteChannel(c.st.data));  // the note source channel belongs to the project
  s.original = c.st.original;
  setPluginState(c.a, c.t, s, std::string(tr("Load preset")) + " " + in->presets[static_cast<size_t>(index)].name);
  c.u.lastPreset = index;
  c.u.presetName = in->presets[static_cast<size_t>(index)].name;
  pui::presetLoaded(c.t, kType, c.u.presetName, index);
}

void topBar(Ctx& c) {
  const Look& L = c.L;
  audit::group(c.p(0, 0), c.p(kW, 44), "bar");
  rect(c, 0, 0, kW, 44, L.top, 0);
  c.dl->AddLine(c.p(0, 43.5f), c.p(kW, 43.5f), L.line, std::max(1.0f, c.s(1)));
  const float wm = pui::wordmarkAt(c.dl, kType, c.p(14, 10), c.s(24), L.text) / c.F.s;
  // the preset box: steps through the factory sounds; the name opens the list
  const PluginInfo* in = info();
  const int np = in ? static_cast<int>(in->presets.size()) : 1;
  const float px = 14 + std::max(wm, 120.0f) + 12, pw = 250, py = 9, ph = 26;
  rect(c, px, py, pw, ph, L.field, 4);
  rectLine(c, px, py, pw, ph, L.line, 4);
  bool hl = false, hr = false, hm = false;
  if (hit(c, "##pprev", px, py, 22, ph, &hl)) loadFactory(c, ((c.u.lastPreset < 0 ? 0 : c.u.lastPreset) - 1 + np) % np);
  if (hit(c, "##pnext", px + pw - 22, py, 22, ph, &hr)) loadFactory(c, (c.u.lastPreset + 1) % np);
  if (hit(c, "##pname", px + 22, py, pw - 44, ph, &hm) || gDev.presetList) ImGui::OpenPopup("##presets"), gDev.presetList = false;
  textV(c, fBold(), 14, px + 7, py - 1, ph, hl ? L.text : L.dim, "\xE2\x80\xB9", 0, 14);
  textV(c, fBold(), 14, px + pw - 13, py - 1, ph, hr ? L.text : L.dim, "\xE2\x80\xBA", 0, 14);
  const std::string name = c.u.presetName.empty() ? std::string(tr("Init")) : c.u.presetName;
  c.dl->PushClipRect(c.p(px + 22, py), c.p(px + pw - 80, py + ph), true);
  textV(c, fReg(), 12, px + 22, py - 1, ph, L.text, name.c_str(), 0, 12);
  c.dl->PopClipRect();
  textV(c, fMono(), 9.5f, px + pw - 22 - textSize(c, fMono(), 9.5f, c.u.lastPreset >= 0 ? tr("FACTORY") : tr("YOURS"), 0.5f).x - 6, py, ph, L.faint,
        c.u.lastPreset >= 0 ? tr("FACTORY") : tr("YOURS"), 0.5f);
  ImGui::SetNextWindowPos(c.p(px, py + ph + 2));
  if (ImGui::BeginPopup("##presets")) {
    ImGui::TextDisabled("%s", tr("Factory"));
    // the factory presets under their headings, in two columns (the second from the pan presets on)
    static const char* const kHeadings[] = {i18n::N_("Chops and stutters"), i18n::N_("Pumping and ducking"), i18n::N_("Triplets and odd grids"),
                                            i18n::N_("Pan and width"),      i18n::N_("Filter rhythms"),      i18n::N_("Slow textures"),
                                            i18n::N_("Fills and chains"),   i18n::N_("Mix glue")};
    if (ImGui::BeginTable("##ptable", 2, ImGuiTableFlags_SizingFixedFit)) {
      ImGui::TableNextColumn();
      std::string last;
      bool second = false;
      for (int i = 0; i < np; ++i) {
        const std::string g = presetGroup(in->presets[static_cast<size_t>(i)].name);
        if (!second && g == kHeadings[3]) ImGui::TableNextColumn(), second = true, last.clear();
        if (g != last && !g.empty() && g != "Start") {
          if (!last.empty()) ImGui::Spacing();
          ImGui::TextDisabled("%s", tr(g.c_str()));
        }
        last = g;
        ImGui::PushID(i);
        if (ImGui::Selectable(in->presets[static_cast<size_t>(i)].name.c_str(), c.u.lastPreset == i)) loadFactory(c, i);
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
    const std::string root = presetRoot(c.a.configDir());
    const auto users = listPresets(root, kType);
    ImGui::Separator();
    ImGui::TextDisabled("%s", tr("Yours"));
    if (users.empty()) ImGui::TextDisabled("%s", tr("(none saved yet)"));
    for (const auto& e : users) {
      const std::string l = (e.folder.empty() ? e.name : e.folder + "/" + e.name) + "##u" + e.file;
      if (ImGui::Selectable(l.c_str(), c.u.lastPreset < 0 && c.u.presetName == e.name)) {
        PluginState ns;
        if (loadPreset(e.file, kType, ns)) {
          if (ns.data.empty()) ns.data = c.st.data;
          setPluginState(c.a, c.t, ns, tr("Load preset"));
          c.u.lastPreset = -1, c.u.presetName = e.name;
          pui::presetLoaded(c.t, kType, e.name);
        }
      }
    }
    ImGui::Separator();
    ImGui::SetNextItemWidth(170);
    if (c.u.saveName[0] == 0) std::snprintf(c.u.saveName, sizeof c.u.saveName, "%s", name.c_str());
    const bool enter = ImGui::InputText("##tsave", c.u.saveName, sizeof c.u.saveName, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button(tr("Save preset")) || enter) {
      const std::string clean = safePresetName(c.u.saveName);
      if (!clean.empty() && savePreset(root, c.st, clean, "Terrace")) {
        c.u.presetName = clean, c.u.lastPreset = -1;
        pui::presetLoaded(c.t, kType, clean);
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::EndPopup();
  }
  // right: Bands, Undo, Mix
  float x = kW - 14;
  {  // Mix: drag it, or click it for a list
    const float w = 70, h = 22, y = 11;
    x -= w;
    ImGui::PushID("mix");
    ImGui::SetCursorScreenPos(c.p(x, y));
    ImGui::InvisibleButton("##mix", ImVec2(c.s(w), c.s(h)));
    const bool hov = ImGui::IsItemHovered();
    double v = val(c, TrMix);
    const ImVec2 press = ImGui::GetIO().MouseClickedPos[0];
    pui::dragParamItem(c.a, c.t.pid(TrMix), info()->params[TrMix], v, 200.0f, true);
    dropTarget(c, TgMix);
    if (ImGui::IsItemDeactivated()) {
      const ImVec2 m = ImGui::GetIO().MousePos;
      if (std::fabs(m.x - press.x) < 3 && std::fabs(m.y - press.y) < 3) ImGui::OpenPopup("##mixl");
    }
    rect(c, x, y, w, h, hov ? theme::mix(L.field, L.text, 0.05f) : L.field, 4);
    rectLine(c, x, y, w, h, L.line, 4);
    const std::string vs = display(c, TrMix, v);
    textV(c, fMonoBold(), 11, x + 8, y, h, L.text, vs.c_str());
    const ImVec2 a0 = c.p(x + w - 11, y + h * 0.5f - 1.5f);
    const float d = c.s(4);
    c.dl->AddTriangleFilled(ImVec2(a0.x - d, a0.y), ImVec2(a0.x + d, a0.y), ImVec2(a0.x, a0.y + d), L.dim);
    ImGui::SetNextWindowPos(c.p(x, y + h + 2));
    if (ImGui::BeginPopup("##mixl")) {
      for (int p : {100, 75, 50, 25, 0}) {
        char b[8];
        std::snprintf(b, sizeof b, "%d%%", p);
        if (ImGui::Selectable(b, std::lround(v * 100) == p)) setParam(c, TrMix, p / 100.0);
      }
      ImGui::EndPopup();
    }
    ImGui::PopID();
    x -= 8 + textSize(c, fReg(), 10.5f, tr("Mix")).x;
    textV(c, fReg(), 10.5f, x, y, h, L.dim, tr("Mix"));
  }
  {
    const char* ul = tr("Undo");
    const float w = textSize(c, fReg(), 11.5f, ul).x + 20;
    x -= 14 + w;
    if (button(c, "##undo", x, 10, w, 24, ul, false, 11.5f, tr("Undo the last edit"))) c.a.doc.undo();
  }
  {
    static const char* const bl[2] = {"3", "4"};
    const float w = segWidth(c, bl, 2, 11);
    x -= 10 + w;
    segParam(c, TrBands, x, 11, 22, bl, 2);
    x -= 8 + textSize(c, fReg(), 10.5f, tr("Bands")).x;
    textV(c, fReg(), 10.5f, x, 11, 22, L.dim, tr("Bands"));
  }
}

// ------------------------------------------------------------------ the crossover

float freqX(double f, float x0, float w) { return x0 + w * static_cast<float>((std::log10(std::clamp(f, 20.0, 20000.0)) - std::log10(20.0)) / 3.0); }
double xFreq(float x, float x0, float w) { return 20.0 * std::pow(10.0, 3.0 * std::clamp((x - x0) / w, 0.0f, 1.0f)); }

void crossover(Ctx& c) {
  const Look& L = c.L;
  panel(c, 12, 56, 920, 122, tr("CROSSOVER"), tr("drag a split, click a band to select it"));
  const float wx = 22, wy = 86, ww = 898, wh = 82;
  rect(c, wx, wy, ww, wh, L.well, 4);
  const float x0 = wx + 1, w = ww - 2, y0 = wy + 1, h = wh - 2;
  c.dl->PushClipRect(c.p(wx, wy), c.p(wx + ww, wy + wh), true);
  for (double f : {50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0})
    c.dl->AddLine(c.p(freqX(f, x0, w), y0), c.p(freqX(f, x0, w), y0 + h), theme::withAlpha(L.line, 0.9f), std::max(1.0f, c.s(1)));
  // the live spectrum (what plays through the slot)
  {
    constexpr int N = 128;
    float db[N];
    pui::spectrum(c.t.tap(c.a), c.t.key() ^ 0x5452ull, N, db, c.a.engine.sampleRate());
    ImVec2 pts[N + 2];
    for (int i = 0; i < N; ++i) {
      const float yy = std::clamp(h * (-std::max(-120.0f, db[i]) / 60.0f), 4.0f, h);
      pts[i] = c.p(x0 + w * static_cast<float>(i) / (N - 1), y0 + yy);
    }
    for (int i = 0; i + 1 < N; ++i) {
      const ImVec2 q[4] = {pts[i], pts[i + 1], c.p(x0 + w * static_cast<float>(i + 1) / (N - 1), y0 + h), c.p(x0 + w * static_cast<float>(i) / (N - 1), y0 + h)};
      c.dl->AddConvexPolyFilled(q, 4, theme::withAlpha(L.text, 0.10f));
    }
    c.dl->AddPolyline(pts, N, theme::withAlpha(L.text, 0.45f), 0, std::max(1.0f, c.s(1)));
  }
  const int bands = c.bands;
  double edge[5] = {20, val(c, TrSplit1), val(c, TrSplit2), val(c, TrSplit3), 20000};
  if (bands == 3) edge[3] = 20000;
  for (int b = 0; b < bands; ++b) {
    const float bx0 = freqX(edge[b], x0, w), bx1 = freqX(edge[b + 1], x0, w);
    rect(c, bx0, y0, bx1 - bx0, h, theme::withAlpha(L.band[b], b == c.u.sel ? 0.16f : 0.06f));
    rect(c, bx0 + 1, y0 + h - 3, bx1 - bx0 - 2, 3, L.band[b]);
    textC(c, fBold(), 10.5f, (bx0 + bx1) * 0.5f, y0 + 6, L.band[b], tr(bandCaps(b, bands)), 0.8f);
  }
  c.dl->PopClipRect();
  for (int k = 0; k < bands - 1; ++k) {
    const int pk = TrSplit1 + k;
    const float sx = freqX(edge[k + 1], x0, w);
    ImGui::PushID(pk);
    ImGui::SetCursorScreenPos(c.p(sx - 14, y0));
    ImGui::InvisibleButton("##split", ImVec2(c.s(28), c.s(h)));
    const bool act = ImGui::IsItemActive(), hov = ImGui::IsItemHovered();
    if (act && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0)) {
      const float mx = (ImGui::GetIO().MousePos.x - c.F.o.x) / c.F.s;
      const double lo = k == 0 ? 20.0 : edge[k] * 1.26, hi = k == bands - 2 ? 20000.0 : edge[k + 2] / 1.26;
      setParam(c, pk, std::clamp(xFreq(mx, x0, w), lo, hi), true);
    }
    if (ImGui::IsItemDeactivated()) c.a.doc.endGesture();
    if (hov || act) {
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
      c.a.setHint(std::string(tr("Split")) + " " + std::to_string(k + 1) + ": " + fmtHz(edge[k + 1]) + (edge[k + 1] < 1000 ? " Hz" : "Hz"));
    }
    dropTarget(c, TgSplit1 + k);
    ImGui::PopID();
    c.dl->AddLine(c.p(sx, y0), c.p(sx, y0 + h), theme::withAlpha(L.text, hov || act ? 1.0f : 0.75f), std::max(1.0f, c.s(1.2f)));
    const std::string lab = fmtHz(edge[k + 1]);
    rect(c, sx - 15, y0 + h - 22, 30, 15, L.top, 3);
    rectLine(c, sx - 15, y0 + h - 22, 30, 15, L.line2, 3);
    textC(c, fMonoBold(), 9.5f, sx, y0 + h - 21, L.text, lab.c_str());
  }
  for (int b = 0; b < bands; ++b) {  // after the splits: a split takes the press over the band under it
    const float bx0 = freqX(edge[b], x0, w), bx1 = freqX(edge[b + 1], x0, w);
    char id[16];
    std::snprintf(id, sizeof id, "##xb%d", b);
    const float hx0 = b > 0 ? bx0 + 14 : bx0, hx1 = b < bands - 1 ? bx1 - 14 : bx1;  // the split handles keep their own room
    if (hx1 > hx0 + 1 && hit(c, id, hx0, y0, hx1 - hx0, h)) c.u.sel = b;
  }
}

// ------------------------------------------------------------------ the band rows

int viewCols(int steps) { return std::clamp((steps + 15) / 16 * 16, 16, kSteps); }

// Beats in one step of band b's lane li (Auto: 16 steps to the pattern Length).
double laneStepBeats(const Ctx& c, int b, int li) {
  const int size = std::clamp(ival(c, laneParam(b, li, LpSize)), 0, pp::kShaperRates - 1);
  const double len = kLengthBeats[std::clamp(ival(c, TrLength), 0, 3)];
  return size == 0 ? len / pp::kShaperPoints : pp::kShaperRateBeats[size];
}

// The columns a lane shows: rows of 16 for 16 steps or more; a shorter lane shows one bar of its
// step size, so 3 steps of 1/2T fill the lane and 4 steps of 1/16 take a quarter of it.
int laneCols(const Ctx& c, int b, int li, int steps) {
  if (steps >= 16) return viewCols(steps);
  const int perBar = static_cast<int>(std::lround(4.0 / laneStepBeats(c, b, li)));
  return std::clamp(perBar, steps, 16);
}

// How long a lane of `steps` steps of `stepBeats` beats lasts before it loops: "1 bar", "3 beats", "5/16".
std::string loopLength(int steps, double stepBeats) {
  const double beats = steps * stepBeats;
  char t[48];
  auto whole = [](double v) { return std::fabs(v - std::round(v)) < 1e-6; };
  if (whole(beats / 4)) {
    const int n = static_cast<int>(std::lround(beats / 4));
    return i18n::plural(n, i18n::N_("%d bar"), i18n::N_("%d bars"));
  }
  if (whole(beats)) return i18n::plural(static_cast<long>(std::lround(beats)), i18n::N_("%d beat"), i18n::N_("%d beats"));
  if (whole(beats * 4)) std::snprintf(t, sizeof t, "%ld/16", std::lround(beats * 4));
  else if (whole(beats * 8)) std::snprintf(t, sizeof t, "%ld/32", std::lround(beats * 8));
  else if (whole(beats * 3)) std::snprintf(t, sizeof t, tr("%ld triplet eighths"), std::lround(beats * 3));
  else if (whole(beats * 6)) std::snprintf(t, sizeof t, tr("%ld triplet sixteenths"), std::lround(beats * 6));
  else std::snprintf(t, sizeof t, tr("%.2f beats"), beats);
  return t;
}

// "3 steps = 1 bar": the lane's length in time.
std::string laneReadout(const Ctx& c, int b, int li) {
  const int steps = std::clamp(ival(c, laneParam(b, li, LpSteps)) + 1, 1, kSteps);
  char t[96];
  std::snprintf(t, sizeof t, tr("%d steps = %s"), steps, loopLength(steps, laneStepBeats(c, b, li)).c_str());
  return t;
}

// Steps per bar of a step size ("3 per bar" for 1/2T), "" for Auto.
std::string perBarText(int size) {
  if (size <= 0 || size >= pp::kShaperRates) return {};
  char t[48];
  std::snprintf(t, sizeof t, tr("%ld per bar"), std::lround(4.0 / pp::kShaperRateBeats[size]));
  return t;
}

// The quick lane grids of the step menu (each fills one bar).
struct QuickGrid {
  const char* name;
  int steps, size;
};
const QuickGrid kQuickGrids[] = {
    {i18n::N_("Straight sixteenths"), 16, 3}, {i18n::N_("Straight eighths"), 8, 5},   {i18n::N_("Quarter notes"), 4, 7},
    {i18n::N_("Triplet sixteenths"), 24, 4},  {i18n::N_("Triplet eighths"), 12, 6},   {i18n::N_("Triplet quarters"), 6, 8},
    {i18n::N_("Three across the bar"), 3, 10},
};

std::string rangeText(double lo, double hi) {
  if (hi < 1000) return fmtHz(lo) + " - " + fmtHz(hi) + " Hz";
  if (lo < 1000) return fmtHz(lo) + " Hz - " + fmtHz(hi);
  return fmtHz(lo) + " - " + fmtHz(hi);
}

// The step menu: split the steps into repeats, their chance and level, reset them, the lane's grid.
// Each choice is one undo step.
void stepMenu(Ctx& c, int b, int li, int pat, int lo, int hi) {
  const Bank& bank = c.bank;
  char head[64];
  if (lo == hi) std::snprintf(head, sizeof head, tr("Step %d"), lo + 1);
  else std::snprintf(head, sizeof head, tr("Steps %d to %d"), lo + 1, hi + 1);
  ImGui::TextDisabled("%s", head);
  const char* const rowNames[3] = {tr("Split into"), tr("Chance"), tr("Level")};
  float labelW = 0;
  for (const char* r : rowNames) labelW = std::max(labelW, ImGui::CalcTextSize(r).x);
  labelW += ImGui::GetStyle().ItemSpacing.x * 2;
  const float bw = ImGui::GetFrameHeight() * 1.7f;
  auto edit = [&](const char* what, const std::function<void(Bank&, int)>& f) {
    auto e = std::make_unique<Bank>(bank);
    for (int k = lo; k <= hi; ++k) f(*e, k);
    finish(*e);
    commitBank(c, *e, what);
    ImGui::CloseCurrentPopup();
  };
  auto rowLabel = [&](const char* label) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(labelW);
  };
  auto allAre = [&](const std::function<bool(int)>& f) {
    for (int k = lo; k <= hi; ++k)
      if (!f(k)) return false;
    return true;
  };
  // repeats
  rowLabel(rowNames[0]);
  for (int n = 1; n <= 4; ++n) {
    char t[8];
    std::snprintf(t, sizeof t, "%d", n);
    ImGui::PushID(n);
    if (n > 1) ImGui::SameLine(0, 3);
    const bool on = allAre([&](int k) { return bank.hits[pat][b][li][k] == n; });
    if (ui::TextButton(t, on, bw, n == 1 ? i18n::N_("Play the step once") : i18n::N_("Play the step as quick repeated hits")))
      edit(tr("Split steps"), [&](Bank& e, int k) { e.hits[pat][b][li][k] = static_cast<uint8_t>(n); });
    {
      pui::DevPoints d;
      const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
      d.x0 = r0.x, d.y0 = r0.y, d.x1 = r1.x, d.y1 = r1.y;
      char nm[32];
      std::snprintf(nm, sizeof nm, "terrace.split.%d", n);
      pui::devPointsSet(nm, d);
    }
    ImGui::PopID();
  }
  ImGui::SameLine();
  ImGui::TextDisabled("%s", tr("hits"));
  // chance
  rowLabel(rowNames[1]);
  static const int kChances[4] = {100, 75, 50, 25};
  for (int i = 0; i < 4; ++i) {
    char t[8];
    std::snprintf(t, sizeof t, "%d%%", kChances[i]);
    ImGui::PushID(10 + i);
    if (i) ImGui::SameLine(0, 3);
    const bool on = allAre([&](int k) { return bank.chance[pat][b][li][k] == kChances[i]; });
    if (ui::TextButton(t, on, bw)) edit(tr("Step chance"), [&](Bank& e, int k) { e.chance[pat][b][li][k] = static_cast<uint8_t>(kChances[i]); });
    ImGui::PopID();
  }
  // level
  rowLabel(rowNames[2]);
  static const char* const kLevelNames[kLanes][3] = {{i18n::N_("Full"), i18n::N_("Half"), i18n::N_("Off")},
                                                     {i18n::N_("Open"), i18n::N_("Half"), i18n::N_("Closed")},
                                                     {i18n::N_("Left"), i18n::N_("Centre"), i18n::N_("Right")}};
  static const float kLevels[kLanes][3] = {{1, .5f, 0}, {1, .5f, 0}, {-1, 0, 1}};
  for (int i = 0; i < 3; ++i) {
    ImGui::PushID(20 + i);
    if (i) ImGui::SameLine(0, 3);
    const float v = kLevels[li][i];
    const bool on = allAre([&](int k) { return std::fabs(bank.val[pat][b][li][k] - v) < 1e-4f; });
    if (ui::TextButton(kLevelNames[li][i], on, bw * 4.0f / 3.0f + 1)) edit(tr("Step level"), [&](Bank& e, int k) { e.val[pat][b][li][k] = v; });
    ImGui::PopID();
  }
  ImGui::Separator();
  if (ImGui::MenuItem(lo == hi ? tr("Reset step") : tr("Reset steps")))
    edit(tr("Reset steps"), [&](Bank& e, int k) { e.val[pat][b][li][k] = laneDefault(li), e.chance[pat][b][li][k] = 100, e.hits[pat][b][li][k] = 1; });
  ImGui::Separator();
  const bool gridOpen = ImGui::BeginMenu(tr("Lane grid"));
    {
      pui::DevPoints d;
      const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
      d.x0 = r0.x, d.y0 = r0.y, d.x1 = r1.x, d.y1 = r1.y;
      pui::devPointsSet("terrace.gridmenu", d);
    }
  if (gridOpen) {
    const int curSteps = ival(c, laneParam(b, li, LpSteps)) + 1, curSize = ival(c, laneParam(b, li, LpSize));
    for (const QuickGrid& g : kQuickGrids) {
      const int gi = static_cast<int>(&g - kQuickGrids);
      char detail[32];
      std::snprintf(detail, sizeof detail, "%d x %s", g.steps, info()->params[static_cast<size_t>(laneParam(b, li, LpSize))].choices[static_cast<size_t>(g.size)].c_str());
      const bool picked = ImGui::MenuItem(tr(g.name), detail, curSteps == g.steps && curSize == g.size);
      {
        pui::DevPoints d;
        const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
        d.x0 = r0.x, d.y0 = r0.y, d.x1 = r1.x, d.y1 = r1.y;
        char nm[32];
        std::snprintf(nm, sizeof nm, "terrace.grid.%d", gi);
        pui::devPointsSet(nm, d);
      }
      if (picked) {
        setParams(c, {{laneParam(b, li, LpSteps), g.steps - 1}, {laneParam(b, li, LpSize), g.size}}, tr("Lane grid"));
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::EndMenu();
  }
}

// The step lane of band b (the lane its row shows), drawn and edited at (x, y, w, h).
void laneWell(Ctx& c, int b, float x, float y, float w, float h, int pat) {
  const Look& L = c.L;
  const int li = c.u.lane[b], mode = c.u.mode;
  const int steps = std::clamp(ival(c, laneParam(b, li, LpSteps)) + 1, 1, kSteps);
  const int cols = laneCols(c, b, li, steps);
  const double stepBeats = laneStepBeats(c, b, li);
  const bool bipolar = li == LnPan && mode == 0;
  const ImU32 col = L.band[b];
  rect(c, x, y, w, h, L.well, 4);
  rectLine(c, x, y, w, h, L.line, 4);
  // a ruler strip on top: the bars and beats, and how long the lane loops
  const float rh = h >= 60 ? 12.0f : 0.0f;
  const float ix = x + 1, iw = w - 2, iy = y + 1 + rh, ih = h - 2 - rh;
  const float cw = iw / static_cast<float>(cols);
  const float mid = bipolar ? iy + ih * 0.5f : iy + ih;
  const Bank& bank = c.bank;
  auto shown = [&](int k) -> float {
    if (mode == 1) return bank.chance[pat][b][li][k] / 100.f;
    if (mode == 2) return (bank.hits[pat][b][li][k] - 1) / 3.f;
    return bank.val[pat][b][li][k];
  };
  ImGui::PushID(b * 16 + li * 4 + mode);
  ImGui::SetCursorScreenPos(c.p(x, y));
  ImGui::InvisibleButton("##steps", ImVec2(c.s(w), c.s(h)));
  const ImGuiID id = ImGui::GetItemID();
  const bool active = ImGui::IsItemActive(), hovered = ImGui::IsItemHovered();
  {
    pui::DevPoints d;
    const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
    d.x0 = r0.x, d.y0 = r0.y, d.x1 = r1.x, d.y1 = r1.y;
    char nm[32];
    std::snprintf(nm, sizeof nm, "terrace.lane.%d", b);
    pui::devPointsSet(nm, d);
  }
  c.dl->PushClipRect(c.p(x, y), c.p(x + w, y + h), true);
  // the beats and bars in time (a triplet grid sits visibly against them)
  const double spanBeats = cols * stepBeats;
  auto beatX = [&](double beat) { return ix + iw * static_cast<float>(beat / spanBeats); };
  for (int bt = 0; bt < spanBeats - 1e-6; ++bt)
    c.dl->AddLine(c.p(beatX(bt), iy), c.p(beatX(bt), iy + ih), bt % 4 == 0 ? theme::withAlpha(L.top, 0.9f) : theme::withAlpha(L.top, 0.55f), std::max(1.0f, c.s(1)));
  // the step menu's steps
  const bool menuHere = c.u.mBand == b && c.u.mLane == li && (c.u.rDrag || ImGui::IsPopupOpen("##stepmenu"));
  const int mLo = std::min(c.u.mK0, c.u.mK1), mHi = std::max(c.u.mK0, c.u.mK1);
  if (menuHere) rect(c, ix + mLo * cw, iy, (mHi - mLo + 1) * cw, ih, theme::withAlpha(L.text, 0.10f), 2);
  for (int k = 0; k < cols; ++k) {
    const float x0 = ix + k * cw;
    if (k >= steps) continue;
    const float v = shown(k);
    const int chance = bank.chance[pat][b][li][k], hits = bank.hits[pat][b][li][k];
    float yy = bipolar ? iy + ih * 0.5f - v * (ih * 0.5f - 3) : iy + ih - v * (ih - 4);
    if (mode == 2) yy = std::min(yy, iy + ih - 4);
    float op = 0.35f + 0.5f * std::fabs(v);
    if (mode == 0 && chance < 100) op *= 0.5f;
    const float pad = cw > 8 ? 1.5f : 0.7f;
    const float y0 = std::min(yy, mid), y1 = std::max(std::max(yy, mid), std::min(yy, mid) + 1);
    if (mode == 0 && hits > 1) {  // a split step: its hits as separate blocks, each as long as it plays
      const float sub = (cw - 2 * pad) / static_cast<float>(hits);
      const float gap = std::max(1.5f, std::min(sub * 0.45f, 4.0f));
      for (int j = 0; j < hits; ++j) rect(c, x0 + pad + sub * j, y0, std::max(1.0f, sub - gap), y1 - y0, theme::withAlpha(col, op), 1.5f);
      if (cw >= 20 && y0 - iy >= 13) {
        char t[8];
        std::snprintf(t, sizeof t, "x%d", hits);
        textC(c, fMono(), 9, x0 + cw * 0.5f, y0 - 12, L.dim, t);
      }
    } else {
      rect(c, x0 + pad, y0, cw - 2 * pad, y1 - y0, theme::withAlpha(col, mode == 0 ? op : 0.75f), 2);
    }
    if (mode == 0 && chance < 100) c.dl->AddCircleFilled(c.p(x0 + cw * 0.5f, iy + 5), c.s(std::min(2.0f, cw * 0.25f)), L.acc, 10);
    if (mode != 0 && cw >= 14 && (mode == 1 ? chance < 100 : hits > 1)) {
      char t[8];
      std::snprintf(t, sizeof t, "%d", mode == 1 ? chance : hits);
      textC(c, fMono(), 9, x0 + cw * 0.5f, std::max(iy + 2, y0 - 13), L.dim, t);
    }
  }
  if (steps < cols) {  // steps past the lane's length are not played
    const float xe = ix + steps * cw;
    rect(c, xe, iy, ix + iw - xe, ih, theme::withAlpha(L.top, 0.75f));
    c.dl->AddLine(c.p(xe, iy), c.p(xe, iy + ih), L.acc, std::max(1.0f, c.s(1.5f)));
  }
  for (int bt = 1; bt < spanBeats - 1e-6; ++bt)  // the beats over the steps too, so a long step shows where the beats fall in it
    c.dl->AddLine(c.p(beatX(bt), iy), c.p(beatX(bt), iy + ih), theme::withAlpha(L.well, bt % 4 == 0 ? 0.7f : 0.45f), std::max(1.0f, c.s(1)));
  if (rh > 0) {  // the ruler: bar numbers and beat ticks, the lane's length on the right
    const float ry = y + 1;
    rect(c, ix, ry, iw, rh, theme::withAlpha(L.top, 0.55f), 3, ImDrawFlags_RoundCornersTop);
    for (int bt = 0; bt < spanBeats - 1e-6; ++bt) {
      const float bx = beatX(bt);
      if (bt % 4 == 0) {
        char t[8];
        std::snprintf(t, sizeof t, "%d", bt / 4 + 1);
        c.dl->AddLine(c.p(bx, ry), c.p(bx, ry + rh), theme::withAlpha(L.text, 0.35f), std::max(1.0f, c.s(1)));
        text(c, fMono(), 8.5f, bx + 3, ry + 0.5f, L.faint, t);
      } else {
        c.dl->AddLine(c.p(bx, ry + rh - 4), c.p(bx, ry + rh), theme::withAlpha(L.text, 0.3f), std::max(1.0f, c.s(1)));
      }
    }
    const std::string rd = laneReadout(c, b, li);
    const float tw = textSize(c, fMono(), 8.5f, rd.c_str()).x;
    rect(c, ix + iw - tw - 10, ry, tw + 10, rh, theme::withAlpha(L.top, 0.9f), 3, ImDrawFlags_RoundCornersTopRight);
    text(c, fMono(), 8.5f, ix + iw - tw - 5, ry + 0.5f, L.dim, rd.c_str());
    ++c.readouts;
  }
  if (menuHere) rectLine(c, ix + mLo * cw, iy, (mHi - mLo + 1) * cw, ih, L.acc, 2, 1.5f);
  const double curve = val(c, laneParam(b, li, LpCurve));
  if (mode == 0 && std::fabs(curve) > 1e-4) {  // the line as played between the steps
    constexpr int np = 240;
    ImVec2 pts[np + 1];
    const float* vals = bank.val[pat][b][li];
    for (int i = 0; i <= np; ++i) {
      double f = static_cast<double>(i) / np * cols;
      if (f >= steps) f = std::fmod(f, static_cast<double>(steps));
      const int k = std::min(steps - 1, static_cast<int>(f));
      const double a0 = vals[k], b0 = vals[(k + 1) % steps], fr = f - k;
      const double v = a0 + (b0 - a0) * std::pow(fr, std::pow(2.0, -curve * 3.0));
      pts[i] = c.p(ix + iw * i / np, bipolar ? iy + ih * 0.5f - static_cast<float>(v) * (ih * 0.5f - 3) : iy + ih - static_cast<float>(v) * (ih - 4));
    }
    c.dl->AddPolyline(pts, np + 1, theme::withAlpha(L.text, 0.7f), 0, std::max(1.0f, c.s(1.3f)));
  }
  if (c.hasLive && c.live.playing && c.live.play[b][li] >= 0) {
    const float px = ix + iw * std::clamp(c.live.play[b][li] / static_cast<float>(cols), 0.0f, 1.0f);
    c.dl->AddLine(c.p(px, iy), c.p(px, iy + ih), L.acc, std::max(1.0f, c.s(1.5f)));
  }
  c.dl->PopClipRect();
  {  // drawing (fast strokes fill every step between samples; steps stay raw)
    const ImVec2 m = ImGui::GetIO().MousePos;
    const ImVec2 p0 = c.p(ix, iy), p1 = c.p(ix + iw, iy + ih);
    const float fy = (m.y - p0.y) / std::max(1.0f, p1.y - p0.y);
    const float v = bipolar ? std::clamp(1 - fy * 2, -1.0f, 1.0f) : std::clamp(1 - fy, 0.0f, 1.0f);
    float cur[kSteps];
    for (int k = 0; k < cols; ++k) cur[k] = shown(k);
    drawui::Spec spec;
    spec.surface = "steps";
    spec.raw = true;
    spec.lo = bipolar ? -1.0f : 0.0f;
    if (const std::vector<float>* res = drawui::stroke(c.a, id, active, (m.x - p0.x) / std::max(1.0f, p1.x - p0.x), v, cur, cols, spec)) {
      auto e = std::make_unique<Bank>(bank);
      for (int k = 0; k < cols; ++k) {
        const float r = (*res)[static_cast<size_t>(k)];
        if (mode == 1) e->chance[pat][b][li][k] = static_cast<uint8_t>(std::clamp(std::lround(r * 100.0), 0L, 100L));
        else if (mode == 2) e->hits[pat][b][li][k] = static_cast<uint8_t>(std::clamp(1L + std::lround(r * 3.0), 1L, 4L));
        else e->val[pat][b][li][k] = r;
      }
      finish(*e);
      static const char* what[3] = {i18n::N_("Draw steps"), i18n::N_("Draw step chance"), i18n::N_("Draw step repeats")};
      commitBank(c, *e, tr(what[mode]), 0x5452000000000000ull ^ c.t.key() ^ static_cast<uint64_t>(b * 97 + li * 13 + mode * 1000 + pat * 7));
      c.u.sel = b;
    }
  }
  if (ImGui::IsItemDeactivated()) c.a.doc.endGesture();
  {  // right-click a step, or right-drag across steps: the step menu
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 p0 = c.p(ix, iy), p1 = c.p(ix + iw, iy + ih);
    const int under = std::clamp(static_cast<int>((io.MousePos.x - p0.x) / std::max(1.0f, p1.x - p0.x) * static_cast<float>(cols)), 0, std::max(0, steps - 1));
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) c.u.rDrag = true, c.u.mBand = b, c.u.mLane = li, c.u.mPat = pat, c.u.mK0 = c.u.mK1 = under;
    if (c.u.rDrag && c.u.mBand == b && c.u.mLane == li) {
      if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) c.u.mK1 = under;
      else c.u.rDrag = false, ImGui::OpenPopup("##stepmenu");
    }
    // Alt + wheel over a step: its repeats, 1 to 4 (round)
    if (hovered && io.KeyAlt) {
      ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
      if (io.MouseWheel != 0) {
        const int now = bank.hits[pat][b][li][under];
        const int next = (now - 1 + (io.MouseWheel > 0 ? 1 : 3)) % 4 + 1;
        auto e = std::make_unique<Bank>(bank);
        e->hits[pat][b][li][under] = static_cast<uint8_t>(next);
        finish(*e);
        commitBank(c, *e, tr("Step repeats"));
        c.u.sel = b;
      }
    }
  }
  if (ImGui::BeginPopup("##stepmenu")) {
    if (c.u.mBand == b && c.u.mLane == li) stepMenu(c, b, li, c.u.mPat, std::min(c.u.mK0, c.u.mK1), std::max(c.u.mK0, c.u.mK1));
    ImGui::EndPopup();
  }
  if (hovered)
    c.a.setHint(mode == 1 ? tr("Draw each step's chance to play: drag across the steps. Right-click a step for its menu")
                : mode == 2 ? tr("Draw each step's repeats (1 to 4 hits): drag across the steps. Right-click a step for its menu")
                            : tr("Drag to draw. Right-click a step to split it into repeats, set its chance or the lane's grid. Alt + wheel: repeats"));
  ImGui::PopID();
}

void stepsPopup(Ctx& c, int k) {
  if (!ImGui::BeginPopup("##stepsp")) return;
  const int cur = ival(c, k) + 1;
  for (int i = 1; i <= kSteps; ++i) {
    if ((i - 1) % 8) ImGui::SameLine(0, 2);
    char b[8];
    std::snprintf(b, sizeof b, "%d", i);
    ImGui::PushID(i);
    if (ui::TextButton(b, i == cur, 28)) setParam(c, k, i - 1), ImGui::CloseCurrentPopup();
    ImGui::PopID();
  }
  ImGui::EndPopup();
}

void sizePopup(Ctx& c, int k) {
  if (!ImGui::BeginPopup("##sizep")) return;
  const ParamDesc& d = info()->params[static_cast<size_t>(k)];
  for (size_t i = 0; i < d.choices.size(); ++i) {  // each size with how many of its steps fill a bar
    const std::string per = i == 0 ? std::string(tr("16 steps to the Length")) : perBarText(static_cast<int>(i));
    if (ImGui::MenuItem(d.choices[i].c_str(), per.c_str(), static_cast<int>(i) == ival(c, k))) setParam(c, k, static_cast<double>(i));
  }
  ImGui::EndPopup();
}

// The band's gain as a small drag button ("Gain" at 0 dB, else its value).
void gainButton(Ctx& c, int b, float x, float y, float w, float h) {
  const Look& L = c.L;
  const int k = bandParam(b, BpGain);
  double v = val(c, k);
  ImGui::PushID(k);
  ImGui::SetCursorScreenPos(c.p(x, y));
  ImGui::InvisibleButton("##gain", ImVec2(c.s(w), c.s(h)));
  pui::dragParamItem(c.a, c.t.pid(k), info()->params[static_cast<size_t>(k)], v, 160.0f, false);
  const bool hov = ImGui::IsItemHovered() || ImGui::IsItemActive();
  dropTarget(c, bandTarget(b, TbGain));
  ImGui::PopID();
  rect(c, x, y, w, h, hov ? theme::mix(L.btn, L.text, 0.08f) : L.btn, 4);
  rectLine(c, x, y, w, h, routeOnto(c, bandTarget(b, TbGain)) >= 0 ? sourceColour(L, ival(c, routeParam(routeOnto(c, bandTarget(b, TbGain)), RtSource))) : L.line, 4);
  char t[24];
  if (std::fabs(v) < 0.05) std::snprintf(t, sizeof t, "\xE2\x8A\x95 %s", tr("Gain"));
  else std::snprintf(t, sizeof t, "%+.1f dB", v);
  const ImVec2 ts = textSize(c, fReg(), 10.5f, t);
  textV(c, fReg(), 10.5f, x + (w - ts.x) * 0.5f, y, h, std::fabs(v) < 0.05 ? L.text : L.acc, t);
}

void bandRow(Ctx& c, int b, float y, float h, int pat) {
  const Look& L = c.L;
  const int bands = c.bands;
  const bool sel = b == c.u.sel;
  audit::group(c.p(12, y), c.p(932, y + h), "panel");
  rect(c, 12, y, 920, h, L.panel, 6);
  rect(c, 12, y, 4, h, L.band[b], 6, ImDrawFlags_RoundCornersLeft);
  ImGui::PushID(b);
  if (hit(c, "##selrow", 16, y, 156, 40)) c.u.sel = b;  // the name and range (the buttons under it are their own)
  // name and range
  const float capBase = y + 6 + ascent(c, fBold(), 10);  // the band name on the STEPS / SIZE captions' baseline
  textB(c, fBold(), 11, 26, capBase, L.band[b], tr(bandCaps(b, bands)), 0.8f);
  const double edge[5] = {20, val(c, TrSplit1), val(c, TrSplit2), bands == 3 ? 20000 : val(c, TrSplit3), 20000};
  const std::string rng = rangeText(edge[b], b == bands - 1 ? 20000.0 : edge[b + 1]);
  textV(c, fReg(), 11, 26, y + 19, 22, L.faint, rng.c_str());  // on the combo text
  // solo, mute, gain
  const float by = y + 44;
  {
    const bool solo = ival(c, bandParam(b, BpSolo)) != 0, mute = ival(c, bandParam(b, BpMute)) != 0;
    if (button(c, "##solo", 26, by, 24, 20, "S", solo, 10.5f, tr("Solo this band"))) setParam(c, bandParam(b, BpSolo), solo ? 0 : 1);
    {
      pui::DevPoints d;
      const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
      d.x0 = r0.x, d.y0 = r0.y, d.x1 = r1.x, d.y1 = r1.y;
      d.pts.push_back({0.5, 0.5, solo ? 1.0 : 0.0});
      char nm[32];
      std::snprintf(nm, sizeof nm, "terrace.solo.%d", b);
      pui::devPointsSet(nm, d);
    }
    if (button(c, "##mute", 54, by, 24, 20, "M", mute, 10.5f, tr("Mute this band"))) setParam(c, bandParam(b, BpMute), mute ? 0 : 1);
    gainButton(c, b, 82, by, 62, 20);
  }
  // the lane tabs, their text on the knob values' line
  const float ky = knobLine(y, h);
  {
    static const char* const ln[kLanes] = {i18n::N_("Vol"), i18n::N_("Cut"), i18n::N_("Pan")};
    float x = 26;
    for (int l = 0; l < kLanes; ++l) {
      const char* s = tr(ln[l]);
      const float w = textSize(c, fReg(), 10.5f, s).x + 14;
      char id[16];
      std::snprintf(id, sizeof id, "##lane%d", l);
      const float tabY = ky + 11 + 4 + ascent(c, fReg(), 10.5f) - rowBase(c, 0, 20);  // text on the knob values' baseline
      if (button(c, id, x, tabY, w, 20, s, c.u.lane[b] == l, 10.5f)) c.u.lane[b] = l, c.u.sel = b;
      x += w + 3;
    }
  }
  // the lane
  const float wellH = h - 18;
  laneWell(c, b, 184, y + 8, 560, wellH, pat);
  // steps, size; swing, depth, curve of the lane shown
  const int li = c.u.lane[b];
  textB(c, fBold(), 10, 756, capBase, L.faint, tr("STEPS"), 1.0f);
  textB(c, fBold(), 10, 840, capBase, L.faint, tr("SIZE"), 1.0f);
  {
    const int k = laneParam(b, li, LpSteps);
    char t[8];
    std::snprintf(t, sizeof t, "%d", ival(c, k) + 1);
    ImGui::PushID(k);
    if (comboFace(c, "##steps", 756, y + 19, 76, 22, t)) ImGui::OpenPopup("##stepsp");
    if (ImGui::IsItemHovered()) {
      const std::string tip = std::string(tr("Steps in this lane before it loops")) + ": " + laneReadout(c, b, li);
      ui::Tooltip(tip.c_str());
    }
    ImGui::SetNextWindowPos(c.p(756, y + 43));
    stepsPopup(c, k);
    ImGui::PopID();
  }
  {
    const int k = laneParam(b, li, LpSize);
    const ParamDesc& d = info()->params[static_cast<size_t>(k)];
    const int sz = ival(c, k);
    ImGui::PushID(k);
    const bool open = comboFace(c, "##size", 840, y + 19, 76, 22, d.choices[static_cast<size_t>(sz)].c_str());
    if (ImGui::IsItemHovered()) {
      char tip[160];
      const std::string per = perBarText(sz);
      std::snprintf(tip, sizeof tip, tr("Step length: %s%s%s. %s"), d.choices[static_cast<size_t>(sz)].c_str(), per.empty() ? "" : ", ", per.c_str(),
                    laneReadout(c, b, li).c_str());
      ui::Tooltip(tip);
    }
    dropTarget(c, bandTarget(b, TbSize));
    if (open) ImGui::OpenPopup("##sizep");
    ImGui::SetNextWindowPos(c.p(840, y + 43));
    sizePopup(c, k);
    ImGui::PopID();
  }
  knob(c, laneParam(b, li, LpSwing), 785, ky, 11, L.acc, tr("Swing"), li);
  knob(c, laneParam(b, li, LpDepth), 843, ky, 11, L.acc, tr("Depth"), li);
  knob(c, laneParam(b, li, LpCurve), 901, ky, 11, L.acc, tr("Curve"), li);
  ImGui::PopID();
  rectLine(c, 12, y, 920, h, sel ? L.line2 : L.line, 6);
}

// ------------------------------------------------------------------ modulation

void lfoWell(Ctx& c, int i, float x, float y, float w, float h) {
  const Look& L = c.L;
  rect(c, x, y, w, h, L.well, 4);
  rectLine(c, x, y, w, h, L.line, 4);
  c.dl->AddLine(c.p(x + 1, y + h * 0.5f), c.p(x + w - 1, y + h * 0.5f), theme::withAlpha(L.line, 0.9f), std::max(1.0f, c.s(1)));
  const int shape = ival(c, lfoParam(i, LfShape));
  const double phase = val(c, lfoParam(i, LfPhase)) / 360.0;
  constexpr int np = 160;
  ImVec2 pts[np + 1];
  for (int k = 0; k <= np; ++k) {
    const double cyc = 2.0 * k / np + phase;
    const double fl = std::floor(cyc), p = cyc - fl;
    double v;
    switch (shape) {
      case ShSine: v = std::sin(2 * kPi * p); break;
      case ShTriangle: v = p < 0.25 ? 4 * p : p < 0.75 ? 2 - 4 * p : 4 * p - 4; break;
      case ShSawUp: v = 2 * p - 1; break;
      case ShSawDown: v = 1 - 2 * p; break;
      case ShSquare: v = p < 0.5 ? 1 : -1; break;
      default: {  // a picture of random steps (the engine's dice differ per cycle)
        static const double r[4] = {0.6, -0.5, 0.15, -0.85};
        const double a = r[static_cast<int>(fl) & 3], bb = r[(static_cast<int>(fl) + 1) & 3];
        v = shape == ShRandom ? a : a + (bb - a) * (0.5 - 0.5 * std::cos(kPi * p));
      }
    }
    pts[k] = c.p(x + 1 + (w - 2) * k / np, y + h * 0.5f - static_cast<float>(v) * (h * 0.5f - 4));
  }
  c.dl->AddPolyline(pts, np + 1, L.lfo[i], 0, std::max(1.0f, c.s(1.6f)));
  if (c.hasLive && c.live.playing) {  // where it is now
    const float vy = y + h * 0.5f - c.live.lfo[i] * (h * 0.5f - 4);
    c.dl->AddCircleFilled(c.p(x + w - 6, vy), c.s(3), L.lfo[i], 12);
  }
}

void modulation(Ctx& c) {
  const Look& L = c.L;
  const float px = 942, pw = 286, py = 56;
  panel(c, px, py, pw, rowTop(2, 4) - kRowGap - py, tr("MODULATION"), tr("drag a source onto any knob"));
  for (int i = 0; i < kLfos; ++i) {
    const float y = py + 30 + 126 * i;
    char nm[16], id[16];
    std::snprintf(nm, sizeof nm, "LFO %d", i + 1);
    std::snprintf(id, sizeof id, "##lfo%d", i);
    chip(c, id, px + 10, y, chipWidth(c, nm), nm, L.lfo[i]);
    dragSource(c, SrcLfo1 + i, nm);
    if (ImGui::IsItemHovered()) c.a.setHint(tr("Drag onto a knob, the size box, the gain or a split to route this source there"));
    static const char* const sh[2] = {i18n::N_("Sync"), i18n::N_("Hz")};
    const char* sl[2] = {tr(sh[0]), tr(sh[1])};
    const float sw = segWidth(c, sl, 2, 11);
    int picked = -1;
    std::snprintf(id, sizeof id, "sync%d", i);
    segmented(c, id, px + pw - 10 - sw, y - 1, 22, sl, 2, ival(c, lfoParam(i, LfSync)) ? 0 : 1, &picked);
    if (picked >= 0) setParam(c, lfoParam(i, LfSync), picked == 0 ? 1 : 0);
    lfoWell(c, i, px + 10, y + 26, pw - 20, 36);
    const float ky = y + 26 + 36 + 4 + 14;
    const bool sync = ival(c, lfoParam(i, LfSync)) != 0;
    knob(c, lfoParam(i, sync ? LfRate : LfHz), 983, ky, 11, L.lfo[i], tr("Rate"));
    knob(c, lfoParam(i, LfShape), 1041, ky, 11, L.lfo[i], tr("Shape"));
    knob(c, lfoParam(i, LfPhase), 1099, ky, 11, L.lfo[i], tr("Phase"));
    knob(c, lfoParam(i, LfAmount), 1157, ky, 11, L.lfo[i], tr("Amount"));
    {  // restart: free or at each pattern start
      const bool rs = ival(c, lfoParam(i, LfRetrig)) == 1;
      char rid[16];
      std::snprintf(rid, sizeof rid, "##rst%d", i);
      const char* rl = tr("Restart");
      float lw = 0;
      const float lx = px + 10 + chipWidth(c, nm) + 10;
      if (led(c, rid, lx, y + 2, rl, rs, &lw)) setParam(c, lfoParam(i, LfRetrig), rs ? 0 : 1);
      if (ImGui::IsItemHovered()) c.a.setHint(tr("Restart: the LFO starts again with each pattern (and each note that restarts the lanes)"));
    }
  }
  {
    const float ky = knobLine(rowTop(1, 4), rowH(4));  // on the second band row's knob line
    const float y = ky - 38;
    const char* fn = tr("FOLLOWER");
    chip(c, "##fol", px + 10, y, chipWidth(c, fn, true), fn, L.fol, true);
    dragSource(c, SrcFollower, tr("Follower"));
    if (ImGui::IsItemHovered()) c.a.setHint(tr("Drag onto a knob, the size box, the gain or a split to route this source there"));
    // what it listens to
    static const char* const srcN[kBands + 1] = {i18n::N_("Input"), "", "", "", ""};
    const int fs = ival(c, TrFolSource);
    const std::string cur = fs == 0 ? std::string(tr(srcN[0])) : std::string(tr(bandName(fs - 1, c.bands)));
    if (comboFace(c, "##folsrc", px + pw - 10 - 104, y - 1, 104, 22, cur.c_str(), false)) ImGui::OpenPopup("##folsrcp");
    if (ImGui::IsItemHovered()) c.a.setHint(tr("What the follower listens to: the input or one band"));
    ImGui::SetNextWindowPos(c.p(px + pw - 114, y + 23));
    if (ImGui::BeginPopup("##folsrcp")) {
      if (ImGui::Selectable(tr("Input"), fs == 0)) setParam(c, TrFolSource, 0);
      for (int b = 0; b < c.bands; ++b)
        if (ImGui::Selectable(tr(bandName(b, c.bands)), fs == b + 1)) setParam(c, TrFolSource, b + 1);
      ImGui::EndPopup();
    }
    knob(c, TrFolAttack, 983, ky, 11, L.fol, tr("Attack"));
    knob(c, TrFolRelease, 1041, ky, 11, L.fol, tr("Release"));
    knob(c, TrFolGain, 1099, ky, 11, L.fol, tr("Gain"));
    knob(c, TrFolAmount, 1157, ky, 11, L.fol, tr("Amount"));
  }
}

void routes(Ctx& c) {
  const Look& L = c.L;
  const float px = 942, pw = 286, py = rowTop(2, 4), ph = 630 - kRowGap - py;  // from the third band row's top
  int used[kRoutes], n = 0;
  for (int r = 0; r < kRoutes; ++r)
    if (ival(c, routeParam(r, RtSource)) != SrcOff) used[n++] = r;
  char note[24];
  std::snprintf(note, sizeof note, tr("%d of %d"), n, kRoutes);
  panel(c, px, py, pw, ph, tr("ROUTES"), note);
  constexpr int kRows = 5;
  const int rows = n + (n < kRoutes ? 1 : 0);  // the routes and the add row
  c.u.routeTop = std::clamp(c.u.routeTop, 0, std::max(0, rows - kRows));
  if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseHoveringRect(c.p(px, py + 24), c.p(px + pw, py + ph)) && ImGui::GetIO().MouseWheel != 0)
    c.u.routeTop = std::clamp(c.u.routeTop - static_cast<int>(ImGui::GetIO().MouseWheel), 0, std::max(0, rows - kRows));
  for (int row = c.u.routeTop; row < std::min(rows, c.u.routeTop + kRows); ++row) {
    const float y = py + 30 + 30 * (row - c.u.routeTop);
    ImGui::PushID(row);
    if (row == n) {  // add a route
      const char* al = tr("+ Add route");
      if (button(c, "##add", px + 10, y, textSize(c, fReg(), 10.5f, al).x + 18, 20, al, false, 10.5f,
                 tr("Add a route from LFO 1 to the selected band's depth (then pick the source, the target and the amount)")))
        addRoute(c, SrcLfo1, bandTarget(c.u.sel, TbVolDepth), 0.5);
      ImGui::PopID();
      continue;
    }
    const int r = used[row];
    const int src = ival(c, routeParam(r, RtSource)), tgt = ival(c, routeParam(r, RtTarget));
    if (chip(c, "##src", px + 10, y, 86, tr(sourceName(src)), sourceColour(L, src))) ImGui::OpenPopup("##srcp");
    ImGui::SetNextWindowPos(c.p(px + 10, y + 22));
    if (ImGui::BeginPopup("##srcp")) {
      for (int s = SrcLfo1; s < SrcCount; ++s)
        if (ImGui::Selectable(tr(sourceName(s)), s == src)) setParam(c, routeParam(r, RtSource), s);
      ImGui::Separator();
      if (ImGui::Selectable(tr("Remove route"))) setParams(c, {{routeParam(r, RtSource), SrcOff}, {routeParam(r, RtTarget), TgNone}, {routeParam(r, RtAmount), 0}}, tr("Remove route"));
      ImGui::EndPopup();
    }
    textV(c, fReg(), 11, px + 102, y, 20, L.dim, "\xE2\x86\x92");
    {
      bool hov = false;
      const std::string tn = targetName(tgt, c.bands);
      if (hit(c, "##tgt", px + 118, y, 106, 20, &hov)) ImGui::OpenPopup("##tgtp");
      c.dl->PushClipRect(c.p(px + 118, y), c.p(px + 226, y + 20), true);
      textV(c, fReg(), 11, px + 118, y, 20, hov ? L.acc : L.text, tn.c_str());
      c.dl->PopClipRect();
      if (hov) c.a.setHint(std::string(tr("Target")) + ": " + tn);
      ImGui::SetNextWindowPos(c.p(px + 118, y + 22));
      if (ImGui::BeginPopup("##tgtp")) {
        for (int b = 0; b < c.bands; ++b)
          if (ImGui::BeginMenu(tr(bandName(b, c.bands)))) {
            for (int k = 0; k < TbCount; ++k) {
              const int tt = bandTarget(b, k);
              if (ImGui::MenuItem(targetName(tt, c.bands).c_str(), nullptr, tt == tgt)) setParam(c, routeParam(r, RtTarget), tt);
            }
            ImGui::EndMenu();
          }
        for (int tt = TgSplit1; tt < TgCount; ++tt) {
          if (c.bands == 3 && tt == TgSplit3) continue;
          if (ImGui::MenuItem(targetName(tt, c.bands).c_str(), nullptr, tt == tgt)) setParam(c, routeParam(r, RtTarget), tt);
        }
        ImGui::EndPopup();
      }
    }
    {  // the amount: drag it (step size routes read in steps of the size list)
      const int k = routeParam(r, RtAmount);
      double v = val(c, k);
      ImGui::SetCursorScreenPos(c.p(px + 228, y));
      ImGui::InvisibleButton("##amt", ImVec2(c.s(pw - 238), c.s(20)));
      pui::dragParamItem(c.a, c.t.pid(k), info()->params[static_cast<size_t>(k)], v, 200.0f, false);
      const bool hov = ImGui::IsItemHovered() || ImGui::IsItemActive();
      char t[16];
      const bool sizeT = tgt >= TgBand0 && tgt < TgSplit1 && (tgt - TgBand0) % TbCount == TbSize;
      if (sizeT) std::snprintf(t, sizeof t, "%+ld", std::lround(v * 4));
      else std::snprintf(t, sizeof t, "%+.0f%%", v * 100);
      textB(c, fMono(), 10.5f, px + pw - 10 - textSize(c, fMono(), 10.5f, t).x, rowBase(c, y, 20), hov ? L.acc : L.text, t);
    }
    ImGui::PopID();
  }
  if (rows > kRows) {  // the scroll position
    const float th = (ph - 34) * kRows / rows, ty = py + 28 + (ph - 34 - th) * c.u.routeTop / std::max(1, rows - kRows);
    rect(c, px + pw - 4, ty, 2, th, theme::withAlpha(L.text, 0.3f), 1);
  }
}

// ------------------------------------------------------------------ the sidechain

std::string trackName(const Song& s, ObjectId id) {
  const int i = s.mixerIndex(id);
  return i >= 0 ? mixerTrackName(s, static_cast<size_t>(i)) : std::string(tr("(missing track)"));
}

void sidechain(Ctx& c) {
  const Look& L = c.L;
  const float px = 942, pw = 286, py = 630;
  panel(c, px, py, pw, 140, tr("SIDECHAIN"), tr("from another track"));
  {  // the panel's title is a source too
    ImGui::SetCursorScreenPos(c.p(px, py));
    ImGui::InvisibleButton("##scsrc", ImVec2(c.s(90), c.s(22)));
    dragSource(c, SrcSidechain, tr("Sidechain"));
    if (ImGui::IsItemHovered()) c.a.setHint(tr("Drag onto a knob, the size box, the gain or a split to route this source there"));
  }
  const Song& s = c.a.doc.song();
  const int listen = ival(c, TrScListen), scIn = ival(c, TrScIn), nsrc = ival(c, TrNoteSrc);
  // what the Source box says
  std::string level, notes;
  if (!c.t.isChannel()) {
    const std::vector<ObjectId> keys = tideline::sidechainKeys(s, c.t.owner);
    if (scIn > 0) {
      const ObjectId src = tideline::routeSourceTrack(s, c.t.owner, scIn);
      level = src != kNoId ? trackName(s, src) : std::string(tr("(missing input route)"));
    } else if (scIn < 0) {
      for (size_t k = 0; k < keys.size(); ++k) level += (k ? " + " : "") + trackName(s, keys[k]);
    }
  }
  if (nsrc == NsChannel) {
    const Channel* ch = s.channel(noteChannel(c.st.data));
    notes = ch ? ch->name : std::string();
  } else if (nsrc == NsInput) {
    notes = tr("MIDI input");
  } else {
    const int port = ival(c, TrNotePort);
    notes = port < 0 ? std::string(tr("any MIDI Out port")) : std::string(tr("MIDI Out port")) + " " + std::to_string(port);
  }
  std::string preview;
  if (listen == LsLevel) preview = level.empty() ? std::string(tr("Pick a track")) : level + " " + tr("(track level)");
  else if (listen == LsNotes) preview = notes.empty() ? std::string(tr("Pick a channel")) : notes + " " + tr("(notes)");
  else preview = (level.empty() ? std::string(tr("no track")) : level) + " + " + (notes.empty() ? std::string(tr("no notes")) : notes);
  textV(c, fReg(), 10.5f, px + 10, py + 30, 22, L.dim, tr("Source"));
  ImGui::PushID("scsrcbox");
  if (comboFace(c, "##src", px + 62, py + 30, 200, 22, preview.c_str(), false)) ImGui::OpenPopup("##srcl");
  if (ImGui::IsItemHovered())
    c.a.setHint(tr("What the sidechain listens to: a mixer track or a channel's level (routed here as a sidechain), or a channel's notes"));
  {
    pui::DevPoints d;
    const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
    d.x0 = r0.x, d.y0 = r0.y, d.x1 = r1.x, d.y1 = r1.y;
    d.pts.push_back({static_cast<double>(listen), static_cast<double>(scIn), static_cast<double>(noteChannel(c.st.data))});
    pui::devPointsSet("terrace.source", d);
  }
  ImGui::SetNextWindowPos(c.p(px + 62, py + 54));
  if (ImGui::BeginPopup("##srcl")) {
    if (!c.t.isChannel()) {
      if (listen != LsNotes) {
        ImGui::TextDisabled("%s", tr("Track level"));
        const std::vector<ObjectId> keys = tideline::sidechainKeys(s, c.t.owner);
        for (size_t i = 1; i < s.mixer.size(); ++i) {
          const MixerTrack& m = s.mixer[i];
          if (m.id == c.t.owner || ops::mixerTrackIsFree(s, m.id)) continue;
          const bool keyed = std::find(keys.begin(), keys.end(), m.id) != keys.end();
          const bool selt = (scIn < 0 && keyed) || (scIn > 0 && tideline::routeSourceTrack(s, c.t.owner, scIn) == m.id);
          const std::string l = mixerTrackName(s, i) + "##mt" + std::to_string(m.id);
          if (ImGui::Selectable(l.c_str(), selt)) linkLevelTrack(c.a.doc, c.t.owner, c.t.slot, m.id);
        }
        if (ImGui::Selectable(tr("None"), scIn == 0)) clearLevelSource(c.a.doc, c.t.owner, c.t.slot);
        ImGui::Separator();
      }
      if (!s.channels.empty()) {
        ImGui::TextDisabled("%s", listen == LsLevel ? tr("Channel level") : listen == LsNotes ? tr("Channel notes") : tr("Channel level and notes"));
        for (const Channel& ch : s.channels) {
          const std::string l = ch.name + "##ch" + std::to_string(ch.id);
          if (ImGui::Selectable(l.c_str(), nsrc == NsChannel && noteChannel(c.st.data) == ch.id && listen != LsLevel)) linkChannel(c.a.doc, c.t.owner, c.t.slot, ch.id);
        }
        ImGui::Separator();
      }
      ImGui::TextDisabled("%s", tr("Notes"));
      if (ImGui::Selectable(tr("Any MIDI Out port"), nsrc == NsPorts && ival(c, TrNotePort) < 0 && listen != LsLevel)) setNoteSource(c.a.doc, c.t.owner, c.t.slot, NsPorts, -1);
      if (ImGui::Selectable(tr("MIDI input (controllers)"), nsrc == NsInput && listen != LsLevel)) setNoteSource(c.a.doc, c.t.owner, c.t.slot, NsInput);
      ImGui::Separator();
      bool np = ival(c, TrNotePatterns) != 0;
      const std::string base = info()->params[TrNoteBase].format(val(c, TrNoteBase));
      const std::string npl = std::string(tr("Notes pick patterns")) + " (" + base + " " + tr("and up") + ")";
      if (ImGui::MenuItem(npl.c_str(), nullptr, np)) setParam(c, TrNotePatterns, np ? 0 : 1);
      int nb = ival(c, TrNoteBase);
      ImGui::SetNextItemWidth(160);
      if (ImGui::SliderInt(tr("First pattern note"), &nb, 0, 120, base.c_str())) setParam(c, TrNoteBase, nb, true);
      if (ImGui::IsItemDeactivatedAfterEdit()) c.a.doc.endGesture();
    }
    ImGui::EndPopup();
  }
  ImGui::PopID();
  // the Listen row shares its centre line with the pattern buttons beside it
  textV(c, fReg(), 10.5f, px + 10, py + 59, 22, L.dim, tr("Listen"));
  {
    static const char* const ll[3] = {i18n::N_("Level"), i18n::N_("Notes"), i18n::N_("Both")};
    const char* l3[3] = {tr(ll[0]), tr(ll[1]), tr(ll[2])};
    segParam(c, TrScListen, px + 62, py + 59, 22, l3, 3);
  }
  const float ky = py + 84 + 14;
  knob(c, TrScThreshold, 975, ky, 11, L.sc, tr("Threshold"));
  knob(c, TrScRelease, 1033, ky, 11, L.sc, tr("Release"));
  knob(c, TrScAmount, 1091, ky, 11, L.sc, tr("Amount"));
  {
    const bool on = ival(c, TrScRetrig) != 0;
    if (led(c, "##retrig", 1128, py + 86, tr("Retrig"), on)) setParam(c, TrScRetrig, on ? 0 : 1);
    if (ImGui::IsItemHovered()) c.a.setHint(tr("Retrig: each note restarts the lanes"));
  }
  if (c.hasLive && c.live.sidechain > 0.01f) {  // the sidechain's level now
    const float lv = std::clamp(c.live.sidechain, 0.0f, 1.0f);
    rect(c, px + pw - 14, py + 30, 4, 50, theme::withAlpha(L.sc, 0.18f), 2);
    rect(c, px + pw - 14, py + 30 + 50 * (1 - lv), 4, 50 * lv, L.sc, 2);
  }
}

// ------------------------------------------------------------------ patterns and global

void copyPattern(Ctx& c, int pat) {
  for (int b = 0; b < kBands; ++b)
    for (int l = 0; l < kLanes; ++l)
      for (int k = 0; k < kSteps; ++k)
        c.u.clipVal[b][l][k] = c.bank.val[pat][b][l][k], c.u.clipChance[b][l][k] = c.bank.chance[pat][b][l][k], c.u.clipHits[b][l][k] = c.bank.hits[pat][b][l][k];
  c.u.haveClip = true;
}
void pastePattern(Ctx& c, int pat, bool clearIt) {
  auto e = std::make_unique<Bank>(c.bank);
  for (int b = 0; b < kBands; ++b)
    for (int l = 0; l < kLanes; ++l)
      for (int k = 0; k < kSteps; ++k) {
        e->val[pat][b][l][k] = clearIt ? laneDefault(l) : c.u.clipVal[b][l][k];
        e->chance[pat][b][l][k] = clearIt ? 100 : c.u.clipChance[b][l][k];
        e->hits[pat][b][l][k] = clearIt ? 1 : c.u.clipHits[b][l][k];
      }
  finish(*e);
  commitBank(c, *e, clearIt ? tr("Clear pattern") : tr("Paste pattern"));
}

void patternsRow(Ctx& c, int pat) {
  const Look& L = c.L;
  const float y = 690;
  textV(c, fBold(), 10.5f, 12, y, 20, L.faint, tr("PATTERNS"), 1.0f);
  const int playing = c.hasLive && c.live.playing ? c.live.pattern : -1;
  for (int k = 0; k < kPatterns; ++k) {
    char b[4], id[16];
    std::snprintf(b, sizeof b, "%d", k + 1);
    std::snprintf(id, sizeof id, "##pat%d", k);
    const float x = 76 + 34 * k;
    if (button(c, id, x, y, 26, 20, b, k == pat, 10.5f)) setParam(c, TrPattern, k);
    if (ImGui::IsItemHovered()) c.a.setHint(tr("Pattern: click to show and play it; right-click to copy, paste or clear it"));
    if (k == playing && k != pat) rectLine(c, x - 1, y - 1, 28, 22, L.acc, 5, 1.5f);
    ImGui::PushID(k);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##patm");
    if (ImGui::BeginPopup("##patm")) {
      if (ImGui::MenuItem(tr("Copy pattern"))) copyPattern(c, k);
      if (ImGui::MenuItem(tr("Paste pattern"), nullptr, false, c.u.haveClip)) pastePattern(c, k, false);
      if (ImGui::MenuItem(tr("Clear pattern"))) pastePattern(c, k, true);
      ImGui::EndPopup();
    }
    ImGui::PopID();
  }
  const bool chainOn = ival(c, TrChain) != 0;
  float lw = 0;
  if (led(c, "##chain", 355, y + 2, tr("Chain"), chainOn, &lw)) setParam(c, TrChain, chainOn ? 0 : 1);
  if (ImGui::IsItemHovered()) c.a.setHint(tr("Chain: play the slots in turn, one Length each"));
  const float sx0 = std::max(406.0f, 355 + lw + 10);
  for (int k = 0; k < kChain; ++k) {
    const float x = sx0 + 30 * k;
    const int v = c.bank.chain[k];
    char b[4], id[16];
    std::snprintf(b, sizeof b, v >= 0 ? "%d" : "-", v + 1);
    std::snprintf(id, sizeof id, "##ch%d", k);
    bool hov = false;
    const bool clicked = hit(c, id, x, y, 22, 20, &hov);
    const bool now = c.hasLive && c.live.playing && chainOn && c.live.chainSlot == k;
    rect(c, x, y, 22, 20, now ? theme::withAlpha(L.acc, 0.3f) : hov ? theme::mix(L.btn, L.text, 0.08f) : L.btn, 4);
    rectLine(c, x, y, 22, 20, v >= 0 ? L.acc : L.line, 4);
    textV(c, fReg(), 10.5f, x + 11 - textSize(c, fReg(), 10.5f, b).x * 0.5f, y, 20, v >= 0 ? L.text : L.faint, b);
    int nv = v;
    if (clicked) nv = v + 1 >= kPatterns ? -1 : v + 1;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) nv = -1;
    if (hov && ImGui::GetIO().MouseWheel != 0) nv = std::clamp(v + (ImGui::GetIO().MouseWheel > 0 ? 1 : -1), -1, kPatterns - 1);
    if (hov) c.a.setHint(tr("Chain slot: click for the next pattern, right-click to empty it, or roll the wheel"));
    if (nv != v) {
      auto e = std::make_unique<Bank>(c.bank);
      e->chain[k] = static_cast<int8_t>(nv);
      finish(*e);
      commitBank(c, *e, tr("Chain slot"), 0x5443000000000000ull ^ c.t.key() ^ static_cast<uint64_t>(k));
    }
  }
}

void globalRow(Ctx& c) {
  const Look& L = c.L;
  const float y = 722, h = 22;
  float x = 12;
  textV(c, fBold(), 10.5f, x, y, h, L.faint, tr("GLOBAL"), 1.0f);
  x += textSize(c, fBold(), 10.5f, tr("GLOBAL"), 1.0f).x + 10;
  auto label = [&](const char* s) {
    textV(c, fReg(), 10.5f, x, y, h, L.dim, s);
    x += textSize(c, fReg(), 10.5f, s).x + 7;
  };
  label(tr("Length"));
  {
    static const char* const ll[4] = {i18n::N_("1/2"), i18n::N_("1 bar"), i18n::N_("2 bars"), i18n::N_("4 bars")};
    const char* l4[4] = {tr(ll[0]), tr(ll[1]), tr(ll[2]), tr(ll[3])};
    x += segParam(c, TrLength, x, y, h, l4, 4) + 12;
  }
  auto field = [&](int k, float w) {
    double v = val(c, k);
    ImGui::PushID(k);
    ImGui::SetCursorScreenPos(c.p(x, y));
    ImGui::InvisibleButton("##f", ImVec2(c.s(w), c.s(h)));
    pui::dragParamItem(c.a, c.t.pid(k), info()->params[static_cast<size_t>(k)], v, 200.0f, false);
    const bool hov = ImGui::IsItemHovered() || ImGui::IsItemActive();
    dropTarget(c, targetOf(k, 0));
    ImGui::PopID();
    rect(c, x, y, w, h, hov ? theme::mix(L.field, L.text, 0.05f) : L.field, 4);
    rectLine(c, x, y, w, h, L.line, 4);
    const std::string vs = display(c, k, v);
    textV(c, fMono(), 11, x + 7, y, h, L.text, vs.c_str());
    const ImVec2 a0 = c.p(x + w - 11, y + h * 0.5f - 1.5f);
    const float d = c.s(4);
    c.dl->AddTriangleFilled(ImVec2(a0.x - d, a0.y), ImVec2(a0.x + d, a0.y), ImVec2(a0.x, a0.y + d), L.dim);
    x += w + 12;
  };
  label(tr("Smooth"));
  field(TrSmooth, 66);
  label(tr("Slope"));
  {
    static const char* const sl[3] = {"12 dB", "24 dB", "48 dB"};
    x += segParam(c, TrSlope, x, y, h, sl, 3) + 12;
  }
  label(tr("Output"));
  field(TrOutput, 74);
  // what drawing edits
  label(tr("Draw"));
  {
    static const char* const ml[3] = {i18n::N_("Level"), i18n::N_("Chance"), i18n::N_("Repeats")};
    const char* m3[3] = {tr(ml[0]), tr(ml[1]), tr(ml[2])};
    int picked = -1;
    const float sw = segmented(c, "drawmode", x, y, h, m3, 3, c.u.mode, &picked, 10.5f);
    if (picked >= 0) c.u.mode = picked;
    if (ImGui::IsMouseHoveringRect(c.p(x, y), c.p(x + sw, y + h)))
      c.a.setHint(tr("What dragging across a lane draws: each step's level, its chance to play, or its repeats (one step split into 2 to 4 hits)"));
  }
}

void devReport(Ctx& c) {
  if (!gDev.report) return;
  gDev.report = false;
  std::printf("terrace frame %d: bands %d pattern %d undo %zu solo", ImGui::GetFrameCount(), c.bands, ival(c, TrPattern), c.a.doc.history().position());
  for (int b = 0; b < kBands; ++b) std::printf(" %d", ival(c, bandParam(b, BpSolo)));
  std::printf(" lanes");
  for (int b = 0; b < kBands; ++b) std::printf(" %d", c.u.lane[b]);
  int drawn = 0;
  for (int b = 0; b < kBands; ++b)
    for (int k = 0; k < kSteps; ++k) drawn += std::fabs(c.bank.val[ival(c, TrPattern)][b][LnVol][k] - 1.0f) > 1e-4f;
  std::printf(" drawn %d hits", drawn);
  for (int b = 0; b < kBands; ++b) {  // repeats over one hit on each band's volume lane, then its steps and size
    int extra = 0;
    for (int k = 0; k < kSteps; ++k) extra += c.bank.hits[ival(c, TrPattern)][b][LnVol][k] - 1;
    std::printf(" %d", extra);
  }
  std::printf(" grid");
  for (int b = 0; b < kBands; ++b) std::printf(" %d/%d", ival(c, laneParam(b, LnVol, LpSteps)) + 1, ival(c, laneParam(b, LnVol, LpSize)));
  std::printf(" readouts %d", c.readouts);  // (user 2026-11-21: every lane shows its readout)
  std::printf("\n");
  std::fflush(stdout);
}

}  // namespace

bool devCommand(App& a, const std::string& cmd, const std::vector<std::string>& args) {
  (void)a;
  if (cmd != "terrace") return false;
  if (!args.empty() && args[0] == "presets") {
    gDev.presetList = true;
    return true;
  }
  if (args.size() >= 2 && args[0] == "preset") {
    gDev.preset = std::atoi(args[1].c_str());
    return true;
  }
  gDev.report = true;
  return true;
}

bool drawEditor(App& a, const PluginTarget& t, float width) {
  const PluginState* sp = t.state(a.doc.song());
  if (!sp || sp->type != kType) return false;
  const PluginState st = *sp;
  Ui& u = uis()[t.key()];
  const Look L = look();
  const Bank& bank = bankOf(t.key(), st.data);
  skin::Form F = skin::Form::fit(width, kW);
  Ctx c{a, t, F, ImGui::GetWindowDrawList(), st, u, L, bank, {}, false, 4};
  if (!t.isChannel())
    if (auto rt = a.builder.effectRuntime(t.owner, t.slot); rt && rt->fx) c.hasLive = liveView(rt->fx.get(), c.live);
  c.bands = ival(c, TrBands) == 0 ? 3 : 4;
  u.sel = std::clamp(u.sel, 0, c.bands - 1);
  {  // the preset name: follow what the header knows (a browser drop names it there)
    std::string type;
    bool onB = false;
    int index = -1;
    const std::string shown = pui::devHeaderPreset(t, &type, &onB, &index);
    if (!shown.empty() && type == kType && shown != u.presetName) u.presetName = shown, u.lastPreset = index;
  }
  if (gDev.preset >= 0) loadFactory(c, gDev.preset), gDev.preset = -1;
  ImGui::PushID("gl.terrace");
  rect(c, 0, 0, kW, kH, L.win, 0);
  topBar(c);
  crossover(c);
  const int pat = std::clamp(ival(c, TrPattern), 0, kPatterns - 1);
  for (int b = 0; b < c.bands; ++b) bandRow(c, b, rowTop(b, c.bands), rowH(c.bands), pat);
  modulation(c);
  routes(c);
  sidechain(c);
  patternsRow(c, pat);
  globalRow(c);
  devReport(c);
  F.end(kW, kH);
  ImGui::PopID();
  return true;
}

}  // namespace gl::packs::terrace
