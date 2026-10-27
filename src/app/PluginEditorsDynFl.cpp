// Gridline — dynamics editors on the reference form geometry (κ2). Each editor
// places its controls at the measured positions of the reference layout
// (form pixels, scaled to the window) and uses its own palette roles.
#include "../core/I18n.h"
#include "../core/PluginBrand.h"
#include "PluginEditorsDynFl.h"
#include <map>
#include "../engine/Dsp.h"
#include "../engine/GraphModels.h"
#include "../engine/LiveGraph.h"
#include "PluginEditorsCommon.h"

namespace gl {

using namespace pp;
using skin::hex;

namespace dynfl {

LiveMeters liveMeters(App& a, const PluginTarget& t) {
  LiveMeters m;
  if (t.isChannel()) return m;
  if (auto rt = a.builder.effectRuntime(t.owner, t.slot)) {
    m.gr = rt->meter.load();
    m.in = rt->input.load();
    for (int k = 0; k < 4; ++k) m.b[k] = rt->bands[k].load();
  }
  return m;
}

ScrollHistory::ScrollHistory() {
  for (auto& l : v)
    for (float& x : l) x = -200.f;
}

void ScrollHistory::push(float speed, const float* lanes, int n) {
  if (frozen) return;
  // speed 0..1 → 0.25 .. 4 columns per frame (0.5 → 1)
  acc += std::pow(2.0f, (speed - 0.5f) * 4.0f);
  while (acc >= 1.0f) {
    acc -= 1.0f;
    head = (head + 1) % kCols;
    for (int k = 0; k < kLanes; ++k) v[k][head] = k < n ? lanes[k] : -200.f;
  }
}

ScrollHistory& history(uint64_t key) {
  static std::map<uint64_t, ScrollHistory> h;
  return h[key];
}

EditorUi& editorUi(uint64_t key) {
  static std::map<uint64_t, EditorUi> u;
  return u[key];
}

}  // namespace dynfl

namespace {

using namespace dynfl;
using ed::ival;
using ed::val;

// Store / flip with the spare state (in-editor A/B as in the reference).
void spareButtons(App& a, const PluginTarget& t, const skin::Form& f, float x, float y, ImU32 bg, ImU32 fg) {
  EditorUi& ui = editorUi(t.key());
  const PluginState st = *t.state(a.doc.song());
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (skin::button(f, "store", x, y, 24, 24, nullptr, bg, fg, "Store in spare state")) ui.spare = st.params;
  {  // glyph: arrow down into a tray
    // The old glyph's ink: flat-ended strokes from the stem top (c.y - s) to the tray (c.y + s, 1.5 px), s either side.
    const ImVec2 c = f.p(x + 12, y + 12);
    const float s = f.d(5);
    ui::drawIconFit(dl, ui::Icon::StoreTray, ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s + 0.75f), fg);
  }
  if (skin::button(f, "flip", x + 26, y, 24, 24, nullptr, bg, fg, "Flip with spare state (A/B compare)") && !ui.spare.empty()) {
    PluginState ns = st;
    std::vector<float> keep = st.params;
    ns.params = ui.spare;
    ui.spare = keep;
    normalizePluginState(ns);
    setPluginState(a, t, ns, "flip with spare state");
  }
  {  // glyph: two opposing arrows
    ui::drawIconGlyph(dl, ui::Icon::SwapArrows, f.p(x + 38, y + 12), f.d(5) * 1.2f, 1.5f, fg);
  }
}

// Display-option glyphs: SVG icons on the old glyphs' grid (half the glyph box per unit).
void glyphBar(ImDrawList* dl, ImVec2 c, float s, ImU32 col) { ui::drawIconGlyph(dl, ui::Icon::DisplayBar, c, s * 0.5f, 0.0f, col); }
void glyphPeaks(ImDrawList* dl, ImVec2 c, float s, ImU32 col) { ui::drawIconGlyph(dl, ui::Icon::DisplayPeaks, c, s * 0.5f, 0.0f, col); }
void glyphEnv(ImDrawList* dl, ImVec2 c, float s, ImU32 col) { ui::drawIconGlyph(dl, ui::Icon::DisplayEnvelope, c, s * 0.5f, 1.4f, col); }
void glyphLevels(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {  // old: two flat-ended 1.4 px bars, 0.35 s either side
  ui::drawIconFit(dl, ui::Icon::DisplayLevels, ImVec2(c.x - s * 0.35f, c.y - s * 0.12f - 0.7f), ImVec2(c.x + s * 0.35f, c.y + s * 0.12f + 0.7f), col);
}

// ======================================================================= Limiter
// Reference form 626 x 300: scrolling display on top (with a narrow option
// strip on its left), three captioned sections below: loudness (gain,
// saturation, ceiling / threshold-knee-ratio), envelope (attack, release,
// sustain + curves) and noise gate (release, gain, threshold); LIMIT / COMP
// target tabs switch the middle controls between the limiter and the
// compressor.
void editorLimiter(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  ImDrawList* dl = ImGui::GetWindowDrawList();
  LiveMeters m = liveMeters(a, t);
  EditorUi& ui = editorUi(t.key());
  const bool comp = ival(st, LimView) == 1;
  // Palette roles (reference form colours).
  const ImU32 panel = hex(0x5F686D), settings = hex(0x566065), envBevel = hex(0x3E474D), top = hex(0x2B3136), strip = hex(0x262626);
  const ImU32 lblText = hex(0xC9D5D4), lblBg = hex(0x31373B), secBg = hex(0x2B3136);
  skin::WheelLook wGain{hex(0x3F484D), hex(0x6543AA), hex(0x8515FF), hex(0x3F484D)};
  skin::WheelLook wSat{hex(0x3F484D), hex(0xB12526), hex(0xD22E2F), hex(0x3F484D)};
  skin::WheelLook wCeil{hex(0x3F484D), hex(0x52952E), hex(0x7AF400), hex(0x3F484D)};
  skin::WheelLook wComp{hex(0x3F484D), hex(0x00AEC1), hex(0x00CAF4), hex(0x3F484D)};
  skin::WheelLook wEnv{hex(0x3F484D), hex(0x7D908C), hex(0xA7C1BC), hex(0x3F484D)};
  skin::WheelLook wEnvC{hex(0x3F484D), hex(0x677576), hex(0x829293), hex(0x3F484D)};
  skin::WheelLook wGate{hex(0x3F484D), hex(0xCEA245), hex(0xE2BD62), hex(0x3F484D)};
  skin::DigiLook digi{hex(0x6A787B), hex(0xFFFFFF)};
  skin::TabLook tabs{hex(0x3E474D), hex(0x2F383D), hex(0x9FA8AD), hex(0xFFFFFF)};

  skin::Form f = skin::formAt(626, 300, width);
  dl->AddRectFilled(f.p(0, 0), f.p(626, 300), panel);
  skin::bevel(dl, f, 0, 176, 626, 124, settings, skin::BevelPlain);
  skin::gradient(dl, f, 0, 176, 626, 124, skin::lighten(settings, 0.06f), skin::darken(settings, 0.12f));
  skin::bevel(dl, f, 114, 179, 347, 117, envBevel, skin::BevelRounded);
  skin::bevel(dl, f, 0, 0, 626, 160, top, skin::BevelPlain);
  skin::bevel(dl, f, 4, 3, 21, 83, strip, skin::BevelRoundedBottom);

  // ---- scrolling display (Visual 29,0 597x160)
  const float vx = 29, vy = 0, vw = 597, vh = 160;
  ScrollHistory& h = history(t.key() ^ 0x4C494D3200000000ull);
  const float pre = val(st, LimGain);
  float lanes[5] = {m.in, m.b[3], m.b[0] + m.b[1], m.b[2], m.in + pre};
  h.push(ui.speed, lanes, 5);
  ImVec2 v0 = f.p(vx, vy), v1 = f.p(vx + vw, vy + vh);
  dl->AddRectFilled(v0, v1, hex(0x1E2326));
  const float lo = -60, hi = 6;
  auto yOf = [&](float db) { return v1.y - (std::clamp(db, lo, hi) - lo) / (hi - lo) * (v1.y - v0.y); };
  for (int db = 0; db >= -54; db -= 6) {
    float y = yOf(static_cast<float>(db));
    dl->AddLine(ImVec2(v0.x, y), ImVec2(v1.x, y), db == 0 ? hex(0x4A545A) : hex(0x2C3337), 1.0f);
    char b[8];
    std::snprintf(b, sizeof b, "%d", db);
    skin::label(dl, f, vx + vw - 26, (y - f.o.y) / f.s - 6, 24, 12, b, hex(0x6F7B80), 0, 8.5f, 2, false);
  }
  const int cols = static_cast<int>(v1.x - v0.x);
  for (int x = 0; x < cols; ++x) {
    int age = cols - 1 - x;
    if (age >= ScrollHistory::kCols) continue;
    float px = v0.x + static_cast<float>(x);
    if (ui.vis[0]) {  // input peaks (after the input gain)
      float in = h.at(4, age);
      if (in > lo) dl->AddLine(ImVec2(px, yOf(in)), ImVec2(px, v1.y), hex(0xB978FF, 0.35f));
    }
    if (ui.vis[1]) {  // output peaks
      float out = h.at(1, age);
      if (out > lo) dl->AddLine(ImVec2(px, yOf(out)), ImVec2(px, v1.y), hex(0xA4E85F, 0.55f));
    }
  }
  if (ui.vis[2]) {  // gain envelopes: reduction hanging from 0 dB
    std::vector<ImVec2> pts, gate;
    pts.reserve(static_cast<size_t>(cols));
    for (int x = 0; x < cols; ++x) {
      int age = cols - 1 - x;
      if (age >= ScrollHistory::kCols) continue;
      float g = h.at(2, age), gt = h.at(3, age);
      if (g < -199) continue;
      pts.push_back(ImVec2(v0.x + x, yOf(g)));
      gate.push_back(ImVec2(v0.x + x, yOf(std::max(lo, gt))));
    }
    if (pts.size() > 1) dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), hex(0xE5E2DE), 0, 1.3f);
    if (val(st, LimGateThresh, -80) > -79.9f && gate.size() > 1)
      dl->AddPolyline(gate.data(), static_cast<int>(gate.size()), hex(0xCEA245, 0.8f), 0, 1.0f);
  }
  if (ui.vis[3]) {  // level markers
    auto marker = [&](float db, ImU32 col, const char* name) {
      float y = yOf(db);
      for (float x = v0.x; x < v1.x - 30; x += 8) dl->AddLine(ImVec2(x, y), ImVec2(x + 4, y), col, 1.0f);
      ImGui::PushFont(a.fontBold, f.d(8.5f));
      dl->AddText(ImVec2(v0.x + 4, y - f.d(11)), col, name);
      ImGui::PopFont();
    };
    marker(val(st, LimCeiling), hex(0x7AF400, 0.8f), "CEIL");
    if (val(st, LimCompThresh) < -0.01f) marker(val(st, LimCompThresh), hex(0x00CAF4, 0.8f), "THRES");
    if (val(st, LimGateThresh, -80) > -79.9f) marker(val(st, LimGateThresh), hex(0xCEA245, 0.8f), "NOISE");
  }
  // J-68: compressor overlay. With the compressor on, the newest columns'
  // input levels run through the compressor's own static curve (engine law,
  // tabulated once per parameter change) and draw as strokes fading with age:
  // what the compressor does to the material just played.
  if (graphmodel::limiterCompOn(st)) {
    constexpr int kTab = 133;  // -60 .. +6 dB in 0.5 dB steps
    const uint64_t in = livegraph::Hasher().floats(st.params).value();
    const livegraph::Curve& tab = livegraph::memo<livegraph::Curve>(t.key(), 0x4C434F4D).get(in, [&](livegraph::Curve& o) {
      o.resize(kTab);
      for (int i = 0; i < kTab; ++i) o[static_cast<size_t>(i)] = static_cast<float>(graphmodel::limiterCompGainDb(st, -60.0 + 0.5 * i));
    });
    auto gainAt = [&](float db) {
      float u = std::clamp((db + 60.0f) * 2.0f, 0.0f, kTab - 1.001f);
      int i = static_cast<int>(u);
      return tab[static_cast<size_t>(i)] + (tab[static_cast<size_t>(i + 1)] - tab[static_cast<size_t>(i)]) * (u - i);
    };
    const int fade = std::min(cols, 120);
    for (int age = 0; age < fade && age < ScrollHistory::kCols; ++age) {
      float inDb = h.at(4, age);
      if (inDb <= lo) continue;
      float g = gainAt(inDb);
      if (std::fabs(g) < 0.05f) continue;
      float px = v1.x - 1 - static_cast<float>(age);
      dl->AddLine(ImVec2(px, yOf(inDb)), ImVec2(px, yOf(inDb + g)), hex(0x00CAF4, 0.75f * (1.0f - static_cast<float>(age) / fade)), 1.0f);
    }
  }
  {  // gain-reduction indicator on the display's left edge, lit while reducing
    const bool reducing = m.gr < -0.05f;
    ImVec2 c(v0.x + f.d(5), (v0.y + v1.y) * 0.5f);
    float s = f.d(5);
    dl->AddTriangleFilled(ImVec2(c.x - s * 0.6f, c.y - s), ImVec2(c.x - s * 0.6f, c.y + s), ImVec2(c.x + s * 0.6f, c.y),
                          reducing ? hex(0xE5E2DE, 0.9f) : hex(0x4A545A, 0.8f));
  }
  if (h.frozen) skin::label(dl, f, vx + 6, vh - 18, 60, 14, "FROZEN", hex(0xE5E2DE), hex(0x000000, 0.5f), 9);
  ImGui::SetCursorScreenPos(v0);
  ImGui::InvisibleButton("##vis", ImVec2(v1.x - v0.x, v1.y - v0.y));
  if (ImGui::IsItemClicked()) h.frozen = !h.frozen;
  skin::hint(a, "Input (purple) and output (green) peaks, gain envelope (white) · click to freeze");

  // ---- option strip
  if (skin::menuButton(f, "menu", 5, 5, 18, 18, hex(0xD3E1E4))) ImGui::OpenPopup("##limopt");
  if (ImGui::BeginPopup("##limopt")) {
    bool safe = val(st, LimSafety) >= 0.5f;
    if (ImGui::MenuItem(i18n::tr("0.2 dB safety margin"), nullptr, safe)) pui::setParamValue(a, t, LimSafety, safe ? 0.0 : 1.0);
    ImGui::EndPopup();
  }
  skin::glyphToggle(f, "vi", 5, 28, 18, 13, ui.vis[0], hex(0xB978FF), glyphBar, "Show input peaks");
  skin::glyphToggle(f, "vo", 5, 41, 18, 13, ui.vis[1], hex(0xA4E85F), glyphPeaks, "Show output peaks");
  skin::glyphToggle(f, "vg", 5, 54, 18, 13, ui.vis[2], hex(0xE5E2DE), glyphEnv, "Show analysis and gain envelopes");
  skin::glyphToggle(f, "vl", 5, 69, 18, 13, ui.vis[3], hex(0xB978FF), glyphLevels, "Show level markers");
  skin::slider(f, "speed", 4, 88, 21, 65, ui.speed, hex(0x373737), hex(0x515F62), "Scrolling speed");

  // ---- section captions
  skin::caption(dl, f, 2, 160, 279, 15, "LOUDNESS", lblText, secBg);
  skin::caption(dl, f, 286, 160, 173, 15, "ENVELOPE", lblText, secBg);
  skin::caption(dl, f, 464, 160, 160, 15, "NOISE GATE", lblText, secBg);

  // ---- loudness
  auto knobLabel = [&](float x, float w, const char* s) { skin::label(dl, f, x, 232, w, 14, s, lblText, lblBg, 9.0f); };
  skin::wheel(a, t, LimGain, f, 8, 184, 46, 46, wGain);
  knobLabel(13, 36, "GAIN");
  skin::wheel(a, t, LimSaturation, f, 62, 184, 46, 46, wSat);
  knobLabel(67, 36, "SAT");
  if (!comp) {
    skin::wheel(a, t, LimCeiling, f, 122, 184, 46, 46, wCeil);
    knobLabel(127, 36, "CEIL");
  } else {
    skin::wheel(a, t, LimCompThresh, f, 122, 184, 46, 46, wComp);
    knobLabel(127, 36, "THRES");
    skin::wheel(a, t, LimCompShape, f, 176, 184, 46, 46, wComp);
    knobLabel(181, 36, "KNEE");
    skin::wheel(a, t, LimCompRatio, f, 230, 184, 46, 46, wComp);
    knobLabel(235, 36, "RATIO");
  }
  // ---- envelope
  const skin::WheelLook& we = comp ? wEnvC : wEnv;
  skin::wheel(a, t, comp ? LimCompAttack : LimAttack, f, 290, 184, 46, 46, we);
  knobLabel(295, 36, "ATT");
  skin::wheel(a, t, comp ? LimCompRelease : LimRelease, f, 346, 184, 46, 46, we);
  knobLabel(351, 36, "REL");
  skin::wheel(a, t, comp ? LimCompRms : LimHold, f, 406, 184, 46, 46, we);
  knobLabel(406, 46, "SUSTAIN");
  if (!comp) {
    skin::bracket(dl, f, 313, 369, 252, hex(0x999A9B));
    skin::label(dl, f, 297, 256, 32, 14, "CURVE", lblText, 0, 8.5f);
    skin::label(dl, f, 353, 256, 32, 14, "CURVE", lblText, 0, 8.5f);
    skin::digi(a, t, LimAtkCurve, f, 300, 272, 26, 21, digi);
    skin::digi(a, t, LimRelCurve, f, 356, 272, 26, 21, digi);
  } else {
    skin::label(dl, f, 295, 256, 36, 14, "CURVE", lblText, 0, 8.5f);
    skin::label(dl, f, 343, 256, 52, 14, "SIDECHAIN", lblText, 0, 8.5f);
    skin::digi(a, t, LimCompCurve, f, 300, 272, 26, 21, digi);
    skin::digi(a, t, LimSidechain, f, 351, 272, 37, 21, digi, ival(st, LimSidechain) > 0 ? "1" : "-");
  }
  // ---- noise gate
  skin::wheel(a, t, LimGateRelease, f, 466, 184, 46, 46, wGate);
  knobLabel(471, 36, "REL");
  skin::wheel(a, t, LimGateFloor, f, 520, 184, 46, 46, wGate);
  knobLabel(525, 36, "GAIN");
  skin::wheel(a, t, LimGateThresh, f, 574, 184, 46, 46, wGate);
  knobLabel(579, 36, "THRES");
  // ---- target tabs + spare state
  static const char* tabNames[2] = {"LIMIT", "COMP"};
  skin::tabs(a, t, LimView, f, 114, 276, 83, 20, tabNames, 2, tabs);
  spareButtons(a, t, f, 572, 271, 0, hex(0xB9C5C4));
  skin::reserve(f);
}

// ======================================================================= Compressor
// Reference form 500 x 105: two rows of three rounded cells (threshold, ratio,
// gain / attack, release, type); each cell = wheel on the left, caption pill
// and value readout on the right. Gridline's extra controls sit in a strip
// underneath (form y 110-168), styled with the same palette.
const char* const kCompTypes[8] = {"Hard", "Medium", "Vintage", "Soft", "Hard · fast rel", "Medium · fast rel", "Vintage · fast rel",
                                    "Soft · fast rel"};
int compType(const PluginState& st) {
  static const float knees[4] = {0, 6, 7, 15};
  float k = val(st, CmpKnee, 6);
  bool vint = ival(st, CmpCharacter) == 1;
  int base = -1;
  for (int i = 0; i < 4; ++i)
    if (std::fabs(k - knees[i]) < 0.01f && (i == 2) == vint) base = i;
  if (base < 0) return -1;
  return base + (val(st, CmpFastRelease) >= 0.5f ? 4 : 0);
}
void setCompType(App& a, const PluginTarget& t, const PluginState& st, int type) {
  static const float knees[4] = {0, 6, 7, 15};
  type = (type % 8 + 8) % 8;
  PluginState ns = st;
  ns.params[CmpKnee] = knees[type % 4];
  ns.params[CmpCharacter] = type % 4 == 2 ? 1.f : 0.f;
  ns.params[CmpFastRelease] = type >= 4 ? 1.f : 0.f;
  setPluginState(a, t, ns, "compressor type", t.key() ^ 0x7479706500000000ull);
}

void editorComp(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  ImDrawList* dl = ImGui::GetWindowDrawList();
  LiveMeters m = liveMeters(a, t);
  const ImU32 panel = hex(0x31373B), cell = hex(0x3C4346), pill = hex(0x444D53), pillText = hex(0xA2B0B7), valBg = hex(0x656F74),
              pink = hex(0xE5B3DD);
  skin::WheelLook w{hex(0x464E51), pink, pink, hex(0x3F484D)};
  w.gap = 0.8976f;  // ~308 degree sweep
  skin::Form f = skin::formAt(500, 170, width);
  dl->AddRectFilled(f.p(0, 0), f.p(500, 170), panel);
  struct CellDef {
    float x, y;
    int param;
    const char* cap;
  };
  const CellDef cells[6] = {{5, 5, CmpThreshold, "THRESHOLD"}, {170, 5, CmpRatio, "RATIO"}, {335, 5, CmpOutput, "GAIN"},
                            {5, 55, CmpAttack, "ATTACK"},      {170, 55, CmpRelease, "RELEASE"}, {335, 55, -1, "TYPE"}};
  char buf[48];
  for (const CellDef& c : cells) {
    skin::bevel(dl, f, c.x, c.y, 160, 45, cell, skin::BevelRounded);
    skin::label(dl, f, c.x + 54, c.y, 106, 16, c.cap, pillText, pill, 8.5f, 1, true, true);
    const char* txt = buf;
    if (c.param == CmpThreshold) std::snprintf(buf, sizeof buf, "%.1f dB", val(st, CmpThreshold));
    else if (c.param == CmpRatio) std::snprintf(buf, sizeof buf, "%.1f : 1", val(st, CmpRatio, 4));
    else if (c.param == CmpOutput) std::snprintf(buf, sizeof buf, "%+.1f dB", val(st, CmpOutput));
    else if (c.param == CmpAttack) std::snprintf(buf, sizeof buf, "%.1f ms", val(st, CmpAttack) * 1000.0f);
    else if (c.param == CmpRelease) std::snprintf(buf, sizeof buf, "%.0f ms", val(st, CmpRelease) * 1000.0f);
    else txt = compType(st) >= 0 ? kCompTypes[compType(st)] : "Custom";
    float vw = c.param == CmpOutput ? 59 : 102;
    skin::label(dl, f, c.x + 55, c.y + 26, vw, 14, txt, pink, valBg, 9.0f, 1, false, true);
    if (c.param >= 0) {
      skin::wheel(a, t, c.param, f, c.x + 2, c.y + 3, 46, 40, w);
    } else {  // type wheel: steps through the eight types
      int ty = std::max(0, compType(st));
      ImVec2 p0 = f.p(c.x + 2, c.y + 3);
      ImGui::SetCursorScreenPos(p0);
      ImGui::InvisibleButton("##ctype", ImVec2(f.d(46), f.d(40)));
      ImGuiIO& io = ImGui::GetIO();
      static float acc = 0;
      if (ImGui::IsItemActivated()) acc = 0;
      if (ImGui::IsItemActive()) {
        acc -= io.MouseDelta.y;
        if (std::fabs(acc) >= 12) {
          setCompType(a, t, st, std::clamp(ty + (acc > 0 ? 1 : -1), 0, 7));
          acc = 0;
        }
      }
      if (ImGui::IsItemDeactivated()) a.doc.endGesture();
      if (ImGui::IsItemHovered() && io.MouseWheel != 0) setCompType(a, t, st, std::clamp(ty + (io.MouseWheel > 0 ? 1 : -1), 0, 7));
      if (ImGui::IsItemHovered()) a.setHint(std::string("Type: ") + kCompTypes[ty] + "   (drag or scroll to change)");
      // Draw a wheel-like dial at the type position (8 detents).
      ImVec2 cc(p0.x + f.d(23), p0.y + f.d(20));
      float r = f.d(19);
      const float a0 = 3.14159f * 0.5f + w.gap * 0.5f, a1 = 3.14159f * 2.5f - w.gap * 0.5f;
      float an = a0 + (a1 - a0) * ty / 7.0f;
      dl->PathArcTo(cc, r - f.d(1.6f), a0, a1, 40);
      dl->PathStroke(w.rail, 0, f.d(3.2f));
      dl->PathArcTo(cc, r - f.d(1.6f), a0, an, 40);
      dl->PathStroke(pink, 0, f.d(3.2f));
      dl->AddCircleFilled(cc, r - f.d(5), w.cap, 32);
      dl->PathLineTo(cc);  // a path: AddLine is half a pixel off centre
      dl->PathLineTo(ImVec2(cc.x + std::cos(an) * (r - f.d(6)), cc.y + std::sin(an) * (r - f.d(6))));
      dl->PathStroke(w.pointer, 0, f.d(1.6f));
    }
  }
  // Sidechain input selector inside the gain cell; options button in the type cell.
  skin::digi(a, t, CmpSidechain, f, 463, 30, 29, 18, skin::DigiLook{pill, pink}, ival(st, CmpSidechain) > 0 ? "1" : "-");
  if (skin::menuButton(f, "copt", 480, 55, 11, 16, pillText)) ImGui::OpenPopup("##compopt");
  if (ImGui::BeginPopup("##compopt")) {
    bool lim = val(st, CmpLimiter, 1) >= 0.5f;
    if (ImGui::MenuItem(i18n::tr("Output limiter (64-sample look-ahead)"), nullptr, lim)) pui::setParamValue(a, t, CmpLimiter, lim ? 0.0 : 1.0);
    ImGui::EndPopup();
  }
  // ---- Gridline extras strip
  skin::bevel(dl, f, 5, 110, 490, 55, cell, skin::BevelRounded);
  skin::label(dl, f, 9, 112, 60, 12, "EXTRA", pillText, 0, 7.5f, 0);
  skin::WheelLook ws = w;
  ws.fill = ws.fillPressed = hex(0xA2B0B7);
  struct Small {
    float x;
    int param;
    const char* cap;
  };
  const Small sm[5] = {{14, CmpKnee, "KNEE"}, {66, CmpMakeup, "MAKEUP"}, {118, CmpLookahead, "AHEAD"}, {170, CmpMix, "MIX"}, {222, CmpKeyHpf, "KEY HP"}};
  for (const Small& s : sm) {
    skin::wheel(a, t, s.param, f, s.x + 8, 122, 30, 28, ws);
    skin::label(dl, f, s.x, 151, 46, 11, s.cap, pillText, 0, 7.5f);
  }
  static const char* det[2] = {"PEAK", "RMS"};
  skin::TabLook tl{pill, hex(0x262B2E), pillText, pink};
  skin::tabs(a, t, CmpDetector, f, 280, 124, 70, 16, det, 2, tl);
  skin::label(dl, f, 280, 142, 70, 11, "DETECTOR", pillText, 0, 7.5f);
  skin::ledToggle(a, t, CmpLimiter, f, 360, 124, 62, 16, "LIMIT", pill, pink, pillText);
  skin::label(dl, f, 360, 142, 62, 11, "0 dBFS", pillText, 0, 7.5f);
  // Gain-reduction meter (horizontal, right end of the strip).
  skin::label(dl, f, 430, 124, 60, 11, "REDUCTION", pillText, 0, 7.0f);
  ImVec2 g0 = f.p(432, 138), g1 = f.p(488, 148);
  skin::insetField(dl, g0, g1, hex(0x1E2326), f.d(2));
  for (int db = 6; db < 24; db += 6) {  // J-93: 6 dB ticks so the meter reads at rest
    const float x = std::round(g1.x - (g1.x - g0.x) * db / 24.0f) + 0.5f;
    dl->AddLine(ImVec2(x, g0.y + 2), ImVec2(x, g1.y - 2), IM_COL32(255, 255, 255, 28), 1.0f);
  }
  float k = std::clamp(-m.gr / 24.0f, 0.0f, 1.0f);
  if (k > 0)
    skin::gradientRect(dl, ImVec2(g1.x - (g1.x - g0.x) * k, g0.y + 1), ImVec2(g1.x - 1, g1.y - 1), skin::lighten(pink, 0.2f), pink, f.d(1.5f));
  skin::reserve(f);
}

// ======================================================================= Soft clipper
// Reference form 270 x 130: threshold and post wheels on the left with the
// plugin caption under them, a tall output meter, and the square transfer
// graph on the right (drag in it to move the threshold). Gridline's extras
// (ceiling mode, oversampling, input gain, mix) in a strip underneath.
double clipCurve(double x, double T, bool toFull) {
  double a = std::fabs(x), y;
  if (toFull) {
    if (a <= T) return x;
    double range = std::max(1.5e-5, 1.0 - T);
    y = T + range * (1.0 - std::exp(-(a - T) / range));
  } else {
    double k = T * 2.0 / 3.0, span = T - k;
    if (a <= k) return x;
    y = k + span * (1 - std::exp(-(a - k) / span));
  }
  return x < 0 ? -y : y;
}

void editorClipper(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  ImDrawList* dl = ImGui::GetWindowDrawList();
  LiveMeters m = liveMeters(a, t);
  const ImU32 panel = hex(0x2E383C), lblText = hex(0x8D989E), lblBg = hex(0x31373B);
  skin::WheelLook w{hex(0x8D989E), hex(0xFFFFFF), hex(0xFFFFFF), hex(0x3F484D)};
  w.gap = 0.8976f;
  w.ruler = true;
  w.railWidth = 0.22f;
  skin::Form f = skin::formAt(270, 185, width);
  dl->AddRectFilled(f.p(0, 0), f.p(270, 185), panel);
  skin::wheel(a, t, ClpThreshold, f, 7, 32, 46, 46, w);
  skin::label(dl, f, 14, 80, 32, 15, "THRES", lblText, lblBg, 8.5f);
  skin::wheel(a, t, ClpPostGain, f, 62, 32, 46, 46, w);
  skin::label(dl, f, 70, 80, 30, 15, "POST", lblText, lblBg, 8.5f);
  if (!pui::titleWordmarkIn(dl, "gl.clipper", f.p(7, 106), f.p(109, 122), f.d(13.0f), 0, hex(0xD8D8D8)))  // J-136: the wordmark
    skin::label(dl, f, 7, 106, 102, 16, pluginTitle("gl.clipper").c_str(), hex(0x9F9F9F), 0, 13.0f, 0);
  // Output meter.
  skin::MeterLook ml;
  ml.bg = hex(0x161C1F);
  ml.rangeDb = 48;
  skin::meter(dl, f, 117, 2, 20, 126, m.b[0], ml);
  dl->AddRect(f.p(117, 2), f.p(137, 128), hex(0x46545A), f.d(1.5f));
  // Transfer graph (input -> output, linear amplitude 0..1.25 like the reference view).
  const double T = dsp::dbToGain(val(st, ClpThreshold, -2.144f));
  const bool full = ival(st, ClpCeilingMode, 1) == 1;
  const double inG = dsp::dbToGain(val(st, ClpInput)), post = dsp::dbToGain(val(st, ClpPostGain));
  ImVec2 g0 = f.p(140, 0), g1 = f.p(270, 130);
  dl->AddRectFilled(g0, g1, hex(0x1B2225));
  auto X = [&](double v) { return g0.x + static_cast<float>(v / 1.5) * (g1.x - g0.x); };
  auto Y = [&](double v) { return g1.y - static_cast<float>(std::clamp(v, 0.0, 1.5) / 1.5) * (g1.y - g0.y); };
  for (int k = 1; k < 6; ++k) {
    float gx = g0.x + (g1.x - g0.x) * k / 6.0f, gy = g0.y + (g1.y - g0.y) * k / 6.0f;
    dl->AddLine(ImVec2(gx, g0.y), ImVec2(gx, g1.y), hex(0x2A3438));
    dl->AddLine(ImVec2(g0.x, gy), ImVec2(g1.x, gy), hex(0x2A3438));
  }
  dl->AddLine(ImVec2(X(1.0), g0.y), ImVec2(X(1.0), g1.y), hex(0x46545A));
  dl->AddLine(ImVec2(g0.x, Y(1.0)), ImVec2(g1.x, Y(1.0)), hex(0x46545A));
  dl->AddLine(ImVec2(g0.x, g1.y), ImVec2(X(1.5), Y(1.5)), hex(0x3A464C));
  ImVec2 pts[97];
  for (int i = 0; i <= 96; ++i) {
    double x = 1.5 * i / 96.0;
    pts[i] = ImVec2(X(x), Y(clipCurve(x * inG, T, full) * post));
  }
  dl->AddPolyline(pts, 97, hex(0xFFFFFF), 0, f.d(1.2f));
  dl->AddLine(ImVec2(X(T / inG), g0.y), ImVec2(X(T / inG), g1.y), hex(0x8D989E, 0.5f));
  float inPk = static_cast<float>(dsp::dbToGain(m.in));
  if (m.in > -90) dl->AddCircleFilled(ImVec2(X(std::min(1.5, static_cast<double>(inPk))), Y(clipCurve(inPk * inG, T, full) * post)), f.d(2.5f), hex(0x3D63DD));
  ImGui::SetCursorScreenPos(g0);
  ImGui::InvisibleButton("##clipvis", ImVec2(g1.x - g0.x, g1.y - g0.y));
  if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0)) {
    double x = std::clamp((ImGui::GetIO().MousePos.x - g0.x) / (g1.x - g0.x) * 1.5 * inG, 1.0 / 128.0, 127.0 / 128.0);
    a.doc.setParam(t.pid(ClpThreshold), dsp::gainToDb(x), true);
  }
  if (ImGui::IsItemDeactivated()) a.doc.endGesture();
  skin::hint(a, "Transfer curve (input across, output up) · drag to move the threshold");
  // ---- Gridline extras
  skin::bevel(dl, f, 3, 134, 264, 48, lblBg, skin::BevelRounded);
  static const char* modes[2] = {"THRES", "0 dB"};
  static const char* os[3] = {"OFF", "2x", "4x"};
  skin::TabLook tl{hex(0x3A464C), hex(0x1B2225), lblText, hex(0xFFFFFF)};
  skin::tabs(a, t, ClpCeilingMode, f, 8, 142, 70, 15, modes, 2, tl);
  skin::label(dl, f, 8, 160, 70, 11, "CEILING", lblText, 0, 7.5f);
  skin::tabs(a, t, ClpOversample, f, 84, 142, 76, 15, os, 3, tl);
  skin::label(dl, f, 84, 160, 76, 11, "OVERSAMPLE", lblText, 0, 7.5f);
  skin::WheelLook ws = w;
  ws.ruler = false;
  ws.railWidth = 0.18f;
  skin::wheel(a, t, ClpInput, f, 172, 138, 28, 28, ws);
  skin::label(dl, f, 166, 167, 40, 11, "INPUT", lblText, 0, 7.5f);
  skin::wheel(a, t, ClpMix, f, 222, 138, 28, 28, ws);
  skin::label(dl, f, 216, 167, 40, 11, "MIX", lblText, 0, 7.5f);
  skin::reserve(f);
}

// ======================================================================= Multiband compressor
// Reference form 800 x 155: master column (filter-type button, limiter switch,
// output meter, gain), the band display (spectrum + band shapes, speed slider,
// four edge wheels) and three band blocks (state switch, threshold / ratio /
// knee, attack / release / gain, level + reduction meters).
void editorMultiband(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  ImDrawList* dl = ImGui::GetWindowDrawList();
  LiveMeters m = liveMeters(a, t);
  EditorUi& ui = editorUi(t.key());
  const ImU32 panel = hex(0x3F4649), visBevel = hex(0x555B5E), bandLbl = hex(0x5B6164), lblText = hex(0xB9BFC3), dimText = hex(0x828A8C),
              orange = hex(0xFFA830);
  skin::WheelLook wb{hex(0xFFFFFF), hex(0xFFFFFF), orange, hex(0x3F484D)};
  wb.ruler = true;
  wb.railWidth = 0.2f;
  skin::WheelLook wf{hex(0x575D5F), hex(0x939596), orange, hex(0x3F484D)};
  skin::WheelLook wg = wb;
  wg.rail = hex(0xC1AEAF);
  skin::Form f = skin::formAt(800, 155, width);
  dl->AddRectFilled(f.p(0, 0), f.p(800, 155), panel);
  const bool indep = ival(st, MbEdgeMode) == 1;
  // ---- master column
  skin::MeterLook ml;
  ml.bg = hex(0x262B2D);
  float masterDb = ui.vis[4] ? m.b[3] : m.in;
  skin::meter(dl, f, 7, 6, 13, 128, masterDb, ml);
  if (skin::button(f, "mio", 4, 137, 18, 12, ui.vis[4] ? "OUT" : "IN", hex(0x4A5154), lblText, "Meter: input / output")) ui.vis[4] = !ui.vis[4];
  static const char* ftNames[3] = {"IIR", "FIR", "LR"};
  int ft = std::clamp(ival(st, MbFilterType), 0, 2);
  if (skin::button(f, "iirfir", 32, 6, 34, 30, ftNames[ft], hex(0x4A5154), lblText,
                   "Band filters: Butterworth IIR / linear-phase FIR / Linkwitz-Riley (flat) — click to change"))
    pui::setParamValue(a, t, MbFilterType, (ft + 1) % 3);
  skin::ledToggle(a, t, MbLimiter, f, 24, 46, 47, 18, "LIMIT", hex(0x4A5154), orange, lblText);
  skin::wheel(a, t, MbOutput, f, 27, 98, 40, 40, wb);
  skin::label(dl, f, 33, 139, 28, 10, "GAIN", lblText, 0, 7.5f);
  // ---- band display
  skin::bevel(dl, f, 76, 6, 200, 142, visBevel, skin::BevelRounded);
  ImVec2 v0 = f.p(80, 11), v1 = f.p(272, 87);
  dl->AddRectFilled(v0, v1, hex(0x2B3033), f.d(2));
  const int N = 96;
  float spec[N];
  pui::spectrum(t.tap(a), t.key(), N, spec, a.engine.sampleRate());
  auto xOf = [&](double hz) { return pui::freqToX(hz, v0.x, v1.x - v0.x); };
  for (int k = 0; k + 1 < N; ++k) {
    float x0 = v0.x + (v1.x - v0.x) * k / N, x1 = v0.x + (v1.x - v0.x) * (k + 1) / N;
    float hh = std::clamp((spec[k] + 84.0f) / 84.0f, 0.0f, 1.0f) * (v1.y - v0.y);
    dl->AddRectFilled(ImVec2(x0, v1.y - hh), ImVec2(x1, v1.y), hex(0x939596, 0.25f));
  }
  double eL = val(st, MbLowXover, 300.3f), eH = val(st, MbHighXover, 9236.7f);
  double eML = indep ? val(st, MbMidLowEdge, 300.3f) : eL, eMH = indep ? val(st, MbMidHighEdge, 9236.7f) : eH;
  // Band shapes (J-68: the engine's own band filters — IIR / FIR / LR — cached
  // per parameter change) offset by the live reduction and the band gain.
  const ImU32 bandCol[3] = {hex(0xFF8C5A), hex(0xF2D35B), hex(0x6FB7FF)};
  struct BandResp {
    double hz[64], g[3][64];
  };
  const double mbSr = a.engine.sampleRate();
  const BandResp& br = livegraph::memo<BandResp>(t.key(), 0x4D424E44).get(livegraph::Hasher().floats(st.params).d(mbSr).value(), [&](BandResp& o) {
    for (int i = 0; i < 64; ++i) o.hz[i] = 20.0 * std::pow(1000.0, i / 63.0);
    graphmodel::multibandBandResponses(st, mbSr, o.hz, 64, o.g[0], o.g[1], o.g[2]);
  });
  for (int b = 0; b < 3; ++b) {
    ImVec2 pts[64];
    for (int i = 0; i < 64; ++i) {
      const double hz = br.hz[i], g = br.g[b][i];
      double gr = ival(st, MbLowState + b) == 2 ? -60.0 : static_cast<double>(m.b[b]);
      double db = dsp::gainToDb(std::max(1e-4, g)) + gr + val(st, MbLowGain + b * 3);
      pts[i] = ImVec2(xOf(hz), v0.y + (v1.y - v0.y) * static_cast<float>(std::clamp(-db / 36.0 + 0.12, 0.0, 1.0)));
    }
    dl->AddPolyline(pts, 64, ival(st, MbLowState + b) == 2 ? theme::withAlpha(bandCol[b], 0.25f) : bandCol[b], 0, f.d(1.0f));
  }
  ImGui::SetCursorScreenPos(v0);
  ImGui::InvisibleButton("##mbvis", ImVec2(v1.x - v0.x, v1.y - v0.y));
  {
    static int drag = -1;
    ImGuiIO& io = ImGui::GetIO();
    const int edgeParams[4] = {MbLowXover, MbMidLowEdge, MbMidHighEdge, MbHighXover};
    const double edges[4] = {eL, eML, eMH, eH};
    if (ImGui::IsItemActivated()) {
      drag = 0;
      for (int k = 1; k < 4; ++k)
        if (std::fabs(io.MousePos.x - xOf(edges[k])) < std::fabs(io.MousePos.x - xOf(edges[drag]))) drag = k;
      if (!indep) drag = drag <= 1 ? 0 : 3;
    }
    if (ImGui::IsItemActive() && drag >= 0 && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1))
      a.doc.setParam(t.pid(edgeParams[drag]), pui::xToFreq(io.MousePos.x, v0.x, v1.x - v0.x), true);
    if (ImGui::IsItemDeactivated()) a.doc.endGesture(), drag = -1;
    for (double e : edges) dl->AddLine(ImVec2(xOf(e), v0.y), ImVec2(xOf(e), v1.y), hex(0xFFFFFF, 0.35f));
  }
  skin::hint(a, "Input spectrum and band shapes · drag near an edge to move it");
  skin::label(dl, f, 150, 89, 34, 11, "SPEED", lblText, 0, 7.5f);
  skin::slider(f, "mbspeed", 186, 90, 87, 9, ui.speed, hex(0x5D6466), hex(0x6E6E6E), "Display speed");
  skin::wheel(a, t, MbLowXover, f, 82, 103, 36, 36, wf);
  skin::wheel(a, t, indep ? MbMidLowEdge : MbLowXover, f, 132, 103, 36, 36, wf);
  skin::wheel(a, t, indep ? MbMidHighEdge : MbHighXover, f, 182, 103, 36, 36, wf);
  skin::wheel(a, t, MbHighXover, f, 233, 103, 36, 36, wf);
  {  // edge labels + link toggle
    char b0[16], b1[16];
    std::snprintf(b0, sizeof b0, "%.0f", eL);
    std::snprintf(b1, sizeof b1, eH >= 1000 ? "%.1fk" : "%.0f", eH >= 1000 ? eH / 1000 : eH);
    skin::label(dl, f, 80, 139, 40, 8, b0, dimText, 0, 7.0f);
    skin::label(dl, f, 231, 139, 40, 8, b1, dimText, 0, 7.0f);
    if (skin::button(f, "link", 160, 139, 32, 8, indep ? "SPLIT" : "LINKED", 0, indep ? orange : dimText,
                     "Mid band edges: linked to the low / high edges, or independent"))
      pui::setParamValue(a, t, MbEdgeMode, indep ? 0.0 : 1.0);
  }
  // ---- band blocks (low, mid, high)
  static const char* names[3] = {"LOW BAND", "MID BAND", "HIGH BAND"};
  static const char* stNames[3] = {"ON", "MUTE", "BYP"};
  const float bx[3] = {283, 455, 627};
  for (int b = 0; b < 3; ++b) {
    float x = bx[b];
    skin::label(dl, f, x, 6, 132, 17, "", lblText, bandLbl, 8.0f);
    skin::label(dl, f, x + 54, 6, 78, 17, names[b], lblText, 0, 8.5f, 2);
    {  // state switch: active / muted / bypassed (cells in the reference order)
      int stv = ival(st, MbLowState + b);  // 0 compress, 1 bypass, 2 mute
      int cell = stv == 0 ? 0 : stv == 2 ? 1 : 2;
      for (int c = 0; c < 3; ++c) {
        ImVec2 p0 = f.p(x + 2 + c * 17, 8), p1 = f.p(x + 2 + c * 17 + 16, 21);
        ImGui::PushID(b * 3 + c);
        ImGui::SetCursorScreenPos(p0);
        if (ImGui::InvisibleButton("##st", ImVec2(p1.x - p0.x, p1.y - p0.y))) pui::setParamValue(a, t, MbLowState + b, c == 0 ? 0 : c == 1 ? 2 : 1);
        if (ImGui::IsItemHovered()) a.setHint(c == 0 ? "Band active (compressing)" : c == 1 ? "Band muted" : "Band bypassed (no compression)");
        ImGui::PopID();
        dl->AddRectFilled(p0, p1, c == cell ? (c == 0 ? orange : c == 1 ? hex(0xE5484D) : hex(0x939596)) : hex(0x4A5154), f.d(2));
        skin::label(dl, f, x + 2 + c * 17, 8, 16, 13, stNames[c], c == cell ? hex(0x1E2224) : dimText, 0, 6.0f);
      }
    }
    skin::wheel(a, t, MbLowThresh + b * 3, f, x, 37, 40, 40, wb);
    skin::label(dl, f, x + 2, 79, 36, 9, "THRES", lblText, 0, 7.5f);
    skin::wheel(a, t, MbLowRatio + b * 3, f, x + 46, 37, 40, 40, wb);
    skin::label(dl, f, x + 50, 79, 32, 9, "RATIO", lblText, 0, 7.5f);
    skin::wheel(a, t, MbLowHard + b, f, x + 92, 37, 40, 40, wb);
    skin::label(dl, f, x + 98, 79, 28, 9, "KNEE", lblText, 0, 7.5f);
    skin::wheel(a, t, MbLowAtk + b, f, x, 98, 40, 40, wb);
    skin::label(dl, f, x + 4, 139, 32, 9, "ATT", lblText, 0, 7.5f);
    skin::wheel(a, t, MbLowRel + b, f, x + 46, 98, 40, 40, wb);
    skin::label(dl, f, x + 52, 139, 28, 9, "REL", lblText, 0, 7.5f);
    skin::wheel(a, t, MbLowGain + b * 3, f, x + 92, 98, 40, 40, wg);
    skin::label(dl, f, x + 100, 139, 24, 9, "GAIN", lblText, 0, 7.5f);
    // Band level (from the spectrum) and reduction meters.
    double lo = b == 0 ? 20 : b == 1 ? eML : eH, hi = b == 0 ? eL : b == 1 ? eMH : 20000;
    double e = 0;
    for (int k = 0; k < N; ++k) {
      double hz = 20.0 * std::pow(1000.0, (k + 0.5) / N);
      if (hz >= lo && hz < hi) e += std::pow(10.0, spec[k] / 10.0);
    }
    float bandDb = e > 0 ? static_cast<float>(10.0 * std::log10(e)) + 6.0f : -120.f;
    skin::meter(dl, f, x + 140, 6, 12, 128, bandDb, ml);
    skin::MeterLook rl = ml;
    rl.high = orange;
    rl.rangeDb = 24;
    skin::meter(dl, f, x + 158, 6, 6, 128, m.b[b], rl, true);
    skin::label(dl, f, x + 155, 139, 12, 9, "C", lblText, 0, 7.5f);
  }
  skin::reserve(f);
}

// ======================================================================= Transient shaper
// Reference form 547 x 318: input / output meters at the sides, split and drive
// / balance wheels around two big attack and release wheels with their shape
// selectors, gain on the right, the effect switch bottom-left and the
// scrolling output graph across the bottom (click to freeze).
void shapeIcon(ImDrawList* dl, ImVec2 c, float w, float h, int shape, bool attack, ImU32 col) {
  // A small envelope: rise then fall; the bent segment follows the shape (sharp / medium / slow).
  ImVec2 pts[24];
  for (int i = 0; i < 24; ++i) {
    float u = i / 23.0f;
    float k = shape == 0 ? 6.0f : shape == 1 ? 2.5f : 1.0f;
    float y;
    if (attack) y = 1.0f - std::exp(-k * u * 3.0f) * (1.0f - u);  // rising edge
    else y = std::exp(-k * u * 3.0f) * (1.0f - u * 0.2f);         // falling tail
    pts[i] = ImVec2(c.x - w * 0.5f + w * u, c.y + h * 0.5f - h * std::clamp(y, 0.0f, 1.0f));
  }
  dl->AddPolyline(pts, 24, col, 0, 1.4f);
}

void editorTransient(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  ImDrawList* dl = ImGui::GetWindowDrawList();
  LiveMeters m = liveMeters(a, t);
  EditorUi& ui = editorUi(t.key());
  const ImU32 panel = hex(0x262C2F), lime = hex(0xB1E758), lbl = hex(0x8E989D), digiBg = hex(0x434B50), digiText = hex(0x9FA8AD);
  skin::WheelLook w{hex(0x5A656B), lime, lime, hex(0x62676A)};
  w.ruler = true;
  w.railWidth = 0.16f;
  w.dents = 5;
  skin::Form f = skin::formAt(547, 318, width);
  dl->AddRectFilled(f.p(0, 0), f.p(547, 318), panel);
  // Meters (input left, output right).
  skin::MeterLook ml;
  ml.bg = hex(0x1A1F21);
  skin::meter(dl, f, 4, 4, 24, 170, m.in, ml);
  skin::meter(dl, f, 519, 4, 24, 170, m.b[0], ml);
  skin::label(dl, f, 0, 174, 32, 8, "IN", lbl, 0, 7.0f);
  skin::label(dl, f, 515, 174, 32, 8, "OUT", lbl, 0, 7.0f);
  // Wheels.
  skin::wheel(a, t, TrSplit, f, 54, 27, 54, 54, w);
  skin::label(dl, f, 46, 84, 70, 10, "SPLIT FREQ", lbl, 0, 8.0f);
  skin::wheel(a, t, TrAttack, f, 132, 34, 90, 90, w);
  skin::label(dl, f, 137, 124, 80, 12, "ATTACK", lbl, 0, 9.5f);
  skin::wheel(a, t, TrDrive, f, 248, 27, 54, 54, w);
  skin::label(dl, f, 240, 84, 70, 10, "DRIVE", lbl, 0, 8.0f);
  {  // drive LED
    ImVec2 c = f.p(275.5f, 96.5f);
    float d = val(st, TrDrive);
    dl->AddRectFilled(ImVec2(c.x - f.d(7), c.y - f.d(3)), ImVec2(c.x + f.d(7), c.y + f.d(3)), d > 0.001f ? hex(0xDA3003) : hex(0x3A2420), f.d(2));
  }
  skin::wheel(a, t, TrSustain, f, 327, 34, 90, 90, w);
  skin::label(dl, f, 332, 124, 80, 12, "RELEASE", lbl, 0, 9.5f);
  skin::wheel(a, t, TrBalance, f, 248, 111, 54, 54, w);
  skin::label(dl, f, 236, 164, 78, 9, "SPLIT BALANCE", lbl, 0, 7.5f);
  skin::wheel(a, t, TrOutput, f, 440, 27, 54, 54, w);
  skin::label(dl, f, 432, 84, 70, 10, "GAIN", lbl, 0, 8.0f);
  // Shape selectors (drag / scroll / click to step) with envelope icons.
  for (int k = 0; k < 2; ++k) {
    int param = k == 0 ? TrAttackSpeed : TrReleaseSpeed;
    float x = k == 0 ? 154 : 349, y = k == 0 ? 140 : 142;
    skin::digi(a, t, param, f, x, y, 46, 24, skin::DigiLook{digiBg, digiBg}, " ");
    shapeIcon(dl, f.p(x + 23, y + 12), f.d(30), f.d(14), std::clamp(ival(st, param, 1), 0, 2), k == 0, digiText);
  }
  // Effect switch.
  {
    bool on = val(st, TrEnabled, 1) >= 0.5f;
    if (skin::button(f, "fxon", 39, 149, 43, 26, nullptr, on ? hex(0x3C4A2E) : hex(0x30363A), lime, "Effect on / off"))
      pui::setParamValue(a, t, TrEnabled, on ? 0.0 : 1.0);
    ImVec2 c = f.p(49, 162);
    dl->AddCircleFilled(c, f.d(4), on ? lime : hex(0x4A5154), 16);
    skin::label(dl, f, 54, 149, 28, 26, "FX", on ? hex(0xB3BABD) : lbl, 0, 9.0f);
  }
  // Mix (Gridline) as a small wheel beside the effect switch.
  skin::WheelLook wm = w;
  wm.ruler = false;
  wm.dents = 0;
  skin::wheel(a, t, TrMix, f, 92, 149, 26, 26, wm);
  skin::label(dl, f, 86, 175, 38, 8, "MIX", lbl, 0, 7.0f);
  // Scrolling output graph.
  ScrollHistory& h = history(t.key() ^ 0x5452414E32000000ull);
  float lanes[3] = {m.in, m.b[0], m.gr};
  h.push(ui.speed, lanes, 3);
  ImVec2 g0 = f.p(13, 190), g1 = f.p(540, 309);
  dl->AddRectFilled(g0, g1, hex(0x1A1F21));
  dl->AddRect(g0, g1, hex(0x676767));
  const float lo = -60, hi = 0;
  auto yOf = [&](float db) { return g1.y - (std::clamp(db, lo, hi) - lo) / (hi - lo) * (g1.y - g0.y); };
  for (int db = -12; db > -60; db -= 12) dl->AddLine(ImVec2(g0.x, yOf(static_cast<float>(db))), ImVec2(g1.x, yOf(static_cast<float>(db))), hex(0x2A3134));
  int cols = static_cast<int>(g1.x - g0.x) - 2;
  std::vector<ImVec2> gl;
  for (int x = 0; x < cols; ++x) {
    int age = cols - 1 - x;
    if (age >= ScrollHistory::kCols) continue;
    float px = g0.x + 1 + x;
    float in = h.at(0, age), out = h.at(1, age), g = h.at(2, age);
    if (ui.vis[1] && in > lo) dl->AddLine(ImVec2(px, yOf(in)), ImVec2(px, g1.y), hex(0xBCFF31, 0.25f));
    if (ui.vis[0] && out > lo) dl->AddLine(ImVec2(px, yOf(out)), ImVec2(px, g1.y), hex(0xFFA830, 0.6f));
    if (g > -199) gl.push_back(ImVec2(px, (g0.y + g1.y) * 0.5f - std::clamp(g, -24.0f, 24.0f) / 24.0f * (g1.y - g0.y) * 0.5f));
  }
  if (ui.vis[2] && gl.size() > 1) dl->AddPolyline(gl.data(), static_cast<int>(gl.size()), hex(0xDA3003), 0, 1.3f);
  if (h.frozen) skin::label(dl, f, 20, 196, 52, 12, "FROZEN", hex(0xE5E2DE), hex(0x000000, 0.5f), 8);
  ImGui::SetCursorScreenPos(g0);
  ImGui::InvisibleButton("##tpvis", ImVec2(g1.x - g0.x, g1.y - g0.y));
  bool visHover = ImGui::IsItemHovered();
  if (ImGui::IsItemClicked()) h.frozen = !h.frozen;
  skin::hint(a, "Output (orange), input (green) and transient gain (red) · click to freeze");
  // Visual controls (shown while hovering the graph, like the reference).
  static float hoverT = 0;
  hoverT = visHover ? 1.0f : std::max(0.0f, hoverT - ImGui::GetIO().DeltaTime * 2);
  if (hoverT > 0 || ImGui::IsItemActive()) {
    skin::bevel(dl, f, 15, 192, 21, 83, hex(0x333333), skin::BevelRounded);
    skin::glyphToggle(f, "tvi", 16, 195, 18, 13, ui.vis[1], hex(0xBCFF31), glyphBar, "Show input peaks");
    skin::glyphToggle(f, "tvo", 16, 208, 18, 13, ui.vis[0], hex(0xFFA830), glyphPeaks, "Show output peaks");
    skin::glyphToggle(f, "tvg", 16, 222, 18, 13, ui.vis[2], hex(0xDA3003), glyphEnv, "Show transient processing");
    skin::slider(f, "tvspeed", 15, 238, 21, 36, ui.speed, hex(0x333333), hex(0x515F62), "Scrolling speed");
  }
  skin::reserve(f);
}

}  // namespace

bool drawDynamicsFlEditor(App& a, const PluginTarget& t, float width) {
  const PluginState* st = t.state(a.doc.song());
  if (!st) return false;
  const std::string& ty = st->type;
  ImGui::PushID("dynfl");
  bool done = true;
  if (ty == "gl.limiter") editorLimiter(a, t, width);
  else if (ty == "gl.comp") editorComp(a, t, width);
  else if (ty == "gl.clipper") editorClipper(a, t, width);
  else if (ty == "gl.multiband") editorMultiband(a, t, width);
  else if (ty == "gl.transient") editorTransient(a, t, width);
  else done = false;
  ImGui::PopID();
  return done;
}

}  // namespace gl
