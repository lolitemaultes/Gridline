// Gridline — "dynamics" pack, J-19: Overdrive (gl.overdrive), Squeeze
// (gl.squeeze) and Loudness (gl.emphasis) on their reference panels' grids and
// palettes (PluginSkin kit). Overdrive: 420 × 180 panel (two small wheels each
// side of the big pre-amp wheel, x100 pad) plus a shape strip; Squeeze:
// squarize / puncher column, filter column, ring column and mix/gain rails;
// Loudness: wide history display over a control strip, meters on the right,
// option switches along the bottom.
#include "../flc_kit.h"
#include "dyn_widgets.h"

namespace gl::packs::dynamics {

using namespace ui;

namespace {

// Transfer curve inside a graph well (f maps -1..1 input to output).
template <class F>
void curveWell(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const skin::Skin& k, F&& f) {
  skin::graphWell(dl, p0, p1, k);
  float cx = (p0.x + p1.x) * 0.5f, cy = (p0.y + p1.y) * 0.5f, hw = (p1.x - p0.x) * 0.5f - 4, hh = (p1.y - p0.y) * 0.5f - 4;
  dl->AddLine(ImVec2(p0.x + 3, cy), ImVec2(p1.x - 3, cy), k.graphGrid);
  dl->AddLine(ImVec2(cx, p0.y + 3), ImVec2(cx, p1.y - 3), k.graphGrid);
  ImVec2 pts[96];
  for (int i = 0; i < 96; ++i) {
    double x = -1.0 + 2.0 * i / 95.0;
    double y = std::clamp(f(x), -1.1, 1.1);
    pts[i] = ImVec2(cx + static_cast<float>(x) * hw, cy - static_cast<float>(y) * hh);
  }
  dl->PushClipRect(p0, p1, true);
  dl->AddPolyline(pts, 96, k.graphCurve, 0, 1.8f);
  dl->PopClipRect();
}

const skin::Skin& overdriveSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x222222, 0x2B2B2B, 0x333333, 0xE32000, 0x444444, 0xB8B8B8);
    s.graphCurve = skin::hex(0xFF5A3A);
    s.select = skin::hex(0xE32000);
    s.lcdText = skin::hex(0xFF7A5C);
    return s;
  }();
  return k;
}

const skin::Skin& squeezeSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x595A5A, 0x767778, 0x3F484D, 0xFFFFFF, 0x595A5A, 0xEAEAEA);
    s.fillPressed = skin::hex(0xFFA830);
    s.pillBg = skin::hex(0x3D3D3D);
    s.pillText = skin::hex(0xEAEAEA);
    s.handle = skin::hex(0x8B8E97);
    s.rail = skin::hex(0x3F484D);
    s.select = skin::hex(0xFFA830);
    s.lcdText = skin::hex(0xFFD08A);
    s.textDim = skin::hex(0xD8D8D8);
    return s;
  }();
  return k;
}

const skin::Skin& loudnessSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x3E4244, 0x434B4D, 0x327B81, 0xFFFFFF, 0x30373A, 0xE2E2E2);
    s.fillPressed = skin::hex(0xFFFFFF);
    s.pillBg = skin::hex(0xD2D2D2);
    s.pillText = skin::hex(0x292929);
    s.graphBg = skin::hex(0x383636);
    s.graphGrid = skin::hex(0x464444);
    s.graphGridStrong = skin::hex(0x5A5858);
    s.graphText = skin::hex(0x9A9898);
    s.select = skin::hex(0x2E8C95);
    s.accent2 = skin::hex(0xFF8B29);
    s.lcdBg = skin::hex(0x1D1D1D);
    s.lcdText = skin::hex(0xE2E2E2);
    return s;
  }();
  return k;
}

}  // namespace

// ===================================================================== gl.overdrive
void editorOverdriveFl(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  const skin::Skin& K = overdriveSkin();
  constexpr float kW = 420, kH = 236;
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  flk::Ctx c{a, t, F, K, dl};
  flk::wheel(c, OdPreBand, 37, 7, 58, "Pre band");
  flk::wheel(c, OdColor, 37, 95, 58, "Colour");
  flk::wheel(c, OdPreAmp, 147, 21, 120, "Pre amp");
  flk::wheel(c, OdPostFilter, 329, 6, 58, "Post filter");
  flk::wheel(c, OdPostGain, 329, 95, 58, "Post gain");
  // x100 pad (reference: small square pad under the pre-amp wheel).
  {
    bool on = val(st, OdBoost) >= 0.5f;
    ImGui::PushID("x100");
    if (skin::button("##x100", F.p(255, 160), F.sz(16, 16), K, "", on)) pui::setParamValue(a, t, OdBoost, on ? 0 : 1);
    if (ImGui::IsItemHovered()) a.setHint("x100 boost: multiplies the pre-amp drive by 100");
    ImGui::PopID();
    flk::text(c, 276, 162, "x100", on ? K.fill : K.textDim);
  }
  // Shape strip: transfer curve + oversampling.
  skin::well(dl, F.p(5, 184), F.p(kW - 5, kH - 5), K, 4 * F.s);
  double drive = std::pow(10.0, val(st, OdPreAmp) * 2.0) * (val(st, OdBoost) >= 0.5f ? 100.0 : 1.0);
  double color = val(st, OdColor);
  flk::text(c, 12, 190, "SHAPE");
  curveWell(dl, F.p(60, 188), F.p(200, kH - 9), K, [&](double x) { return overdriveCurve(x * drive, color) * 0.8; });
  flk::text(c, 222, 190, "OVERSAMPLING");
  static const char* os[3] = {"OFF", "2X", "4X"};
  flk::sw(c, OdOversample, 222, 204, 186, 22, os, 3, true);
  F.end(kW, kH);
}

// ===================================================================== gl.squeeze
void editorSqueezeFl(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  const skin::Skin& K = squeezeSkin();
  constexpr float kW = 345, kH = 250;
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  flk::Ctx c{a, t, F, K, dl};
  auto bevel = [&](float x, float y, float w, float h) { skin::well(dl, F.p(x, y), F.p(x + w, y + h), K, 2 * F.s); };
  auto caption = [&](float x, float y, float w, const char* s) { skin::pill(dl, F.p(x, y), F.p(x + w, y + 14), K, s, F.font(8)); };
  // A row: label button (66 × 24) + wheel (30) — the reference's left column.
  auto row = [&](int param, float y, const char* lbl) {
    ImVec2 p0 = F.p(9, y + 3), p1 = F.p(75, y + 27);
    dl->AddRectFilled(p0, p1, K.panel, 2 * F.s);
    dl->AddRect(p0, p1, skin::darken(K.panel, 0.35f), 2 * F.s);
    skin::label(dl, (p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f - F.font(7.5f) * 0.55f, lbl, K.text, F.font(7.5f));
    skin::wheel(a, t, param, F.p(69, y), 30 * F.s, K, skin::WheelBigLine);
  };
  const bool ref = val(st, SqModel) >= 0.5f;  // J-26 reference engine
  // Squarize column.
  bevel(5, 5, 98, 88);
  caption(9, 7, 80, ref ? "QUANTIZE" : "SQUARIZE");
  if (!ref) row(SqSquare, 23, "AMOUNT");
  row(ref ? SqStepsRef : SqSteps, 56, "STEPS");
  // Puncher column.
  bevel(5, 98, 98, 147);
  caption(9, 100, 80, "PUNCHER");
  row(ref ? SqPreserveMs : SqPreserve, 116, "PRESERVE");
  row(ref ? SqImpactMs : SqImpact, 148, "IMPACT");
  row(SqRelation, 180, "RELATION");
  {
    static const char* rel[4] = {"FLIP", "SKIP", "HALF", "ROOT"};
    (void)rel;
  }
  row(ref ? SqAmountRef : SqPunchAmount, 212, "AMOUNT");
  // Filter column.
  bevel(108, 5, 98, 159);
  skin::check(a, t, SqFilterOn, F.p(113, 7), F.sz(71, 17), K, "FILTER");
  flk::menu(c, SqFilterType, 113, 28, 87, 18);
  flk::textC(c, 134, 52, "FREQ.");
  flk::textC(c, 182, 52, "RES.");
  skin::wheel(a, t, ref ? SqCutoffPos : SqCutoff, F.p(113, 66), 40 * F.s, K, skin::WheelBigLine);
  skin::wheel(a, t, SqResonance, F.p(160, 66), 40 * F.s, K, skin::WheelBigLine);
  {
    char buf[24];
    double hz = ref ? sqCutoffHz(val(st, SqCutoffPos), a.engine.sampleRate()) : val(st, SqCutoff);
    if (hz >= 1000) std::snprintf(buf, sizeof buf, "%.1fk", hz / 1000);
    else std::snprintf(buf, sizeof buf, "%.0f", hz);
    flk::lcd(c, 113, 110, 40, 13, buf);
    std::snprintf(buf, sizeof buf, "%.0f%%", val(st, SqResonance) * 100);
    flk::lcd(c, 160, 110, 40, 13, buf);
  }
  static const char* pos[2] = {"PRE", "POST"};
  flk::sw(c, SqFilterPos, 113, 131, 87, 27, pos, 2, true);
  // Ring column.
  bevel(211, 5, 62, 159);
  caption(215, 7, 54, ref ? "ENGINE" : "RING");
  if (ref) {
    flk::textC(c, 242, 28, "reference", K.textDim);
  } else {
    flk::wheel(c, SqRingFreq, 222, 28, 40, "FREQ");
    flk::wheel(c, SqRingAmount, 222, 96, 40, "AMOUNT");
  }
  {
    static const char* eng[2] = {"OLD", "REF"};
    flk::sw(c, SqModel, 215, 140, 54, 20, eng, 2, true);
  }
  // Mix / gain rails.
  bevel(278, 5, 62, 159);
  skin::vslider(a, t, SqMix, F.p(283, 10), F.sz(22, 138), K);
  skin::vslider(a, t, ref ? SqGainRef : SqGain, F.p(312, 10), F.sz(22, 138), K);
  flk::textC(c, 294, 151, "MIX", K.text);
  flk::textC(c, 323, 151, "GAIN", K.text);
  // Lo-fi strip (rate divide) where the reference shows its name plate.
  bevel(108, 169, 232, 76);
  caption(113, 172, 52, "LO-FI");
  if (!ref) flk::wheel(c, SqRateDiv, 120, 192, 36, "DIVIDE");
  {
    // Squarized sine preview: steps × amount (reference: round(x·S)/S).
    double amt = ref ? 0.0 : val(st, SqSquare);
    double steps = ref ? std::min(4096.0, 2.0 * val(st, SqStepsRef)) : std::max(2.0, static_cast<double>(val(st, SqSteps)));
    int div = ref ? 1 : std::max(1, ival(st, SqRateDiv));
    ImVec2 p0 = F.p(170, 176), p1 = F.p(334, 239);
    skin::graphWell(dl, p0, p1, K);
    ImVec2 pts[128];
    for (int i = 0; i < 128; ++i) {
      int j = i - i % div;
      double ph = j / 127.0;
      double y = std::sin(ph * 2 * 3.14159265);
      double sq = (y >= 0 ? 1.0 : -1.0) * std::pow(std::fabs(y), 1.0 - 0.9 * amt);
      sq = std::round(sq * steps * 0.5) / (steps * 0.5);
      pts[i] = ImVec2(p0.x + (p1.x - p0.x) * i / 127.0f, (p0.y + p1.y) * 0.5f - static_cast<float>(std::clamp(sq, -1.0, 1.0)) * (p1.y - p0.y) * 0.4f);
    }
    dl->AddPolyline(pts, 128, K.graphCurve, 0, 1.5f);
  }
  F.end(kW, kH);
}

// ===================================================================== gl.emphasis
void editorLoudnessFl(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  const skin::Skin& K = loudnessSkin();
  constexpr float kW = 740, kH = 366;
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  flk::Ctx c{a, t, F, K, dl};
  float gr = 0, in = -120;
  if (auto rt = runtime(a, t)) {
    gr = rt->meter.load();
    in = rt->input.load();
  }
  float outPk = tapPeak(t.tap(a), 800);
  History& h = history(t.key() ^ 0x4c4f5544ull);
  if (h.due(60)) {
    float v[3] = {in, toDb(outPk), gr};
    h.push(v, 3);
  }
  // ---- visual (4, 4, 629 × 246 above the control strip)
  {
    ImVec2 p0 = F.p(4, 4), p1 = F.p(633, 248);
    skin::graphWell(dl, p0, p1, K);
    dl->PushClipRect(p0, p1, true);
    const float lo = -36, hi = 3;
    auto Y = [&](float db) { return p1.y - 6 - (std::clamp(db, lo, hi) - lo) / (hi - lo) * (p1.y - p0.y - 12); };
    for (float db = -30; db <= 0; db += 6) {
      dl->AddLine(ImVec2(p0.x, Y(db)), ImVec2(p1.x, Y(db)), db == 0 ? K.graphGridStrong : K.graphGrid);
      char buf[8];
      std::snprintf(buf, sizeof buf, "%.0f", db);
      skin::text(dl, ImVec2(p0.x + 5, Y(db) - F.font(7.5f) - 1), K.graphText, buf, F.font(7.5f));
    }
    const float vw = p1.x - p0.x;
    const int cols = std::min(History::N, static_cast<int>(vw / 1.5f));
    float dx = vw / cols;
    for (int i = 0; i < cols; ++i) {
      float x = p1.x - (i + 1) * dx;
      float iy = Y(h.at(0, i)), oy = Y(h.at(1, i));
      dl->AddRectFilled(ImVec2(x, iy), ImVec2(x + dx + 0.5f, p1.y), theme::withAlpha(K.rail, 0.45f));
      dl->AddRectFilled(ImVec2(x, oy), ImVec2(x + dx + 0.5f, p1.y), theme::withAlpha(K.text, 0.28f));
      float g = h.at(2, i);
      if (g < -0.05f)
        dl->AddRectFilled(ImVec2(x, p0.y), ImVec2(x + dx + 0.5f, p0.y + std::min(1.0f, -g / 18.0f) * (p1.y - p0.y) * 0.45f),
                          theme::withAlpha(K.accent2C(), 0.55f));
    }
    float cy = Y(val(st, EmCeiling));
    for (float x = p0.x; x < p1.x; x += 8) dl->AddLine(ImVec2(x, cy), ImVec2(x + 4, cy), K.accent2C(), 1.5f);
    skin::text(dl, ImVec2(p1.x - 70 * F.s, cy - F.font(8) - 2), K.accent2C(), "CEILING", F.font(8));
    dl->PopClipRect();
  }
  // ---- meters (GR, peak out L/R, input) + value pills
  {
    char buf[16];
    skin::meter(dl, F.p(646, 17), F.p(660, 287), K, gr, 18, true);
    skin::meter(dl, F.p(665, 17), F.p(687, 287), K, toDb(outPk), 48);
    skin::meter(dl, F.p(689, 17), F.p(711, 287), K, toDb(outPk), 48);
    skin::meter(dl, F.p(716, 17), F.p(728, 287), K, in, 48);
    auto pillAt = [&](float x, const char* s, float y) { skin::pill(dl, F.p(x, y), F.p(x + 22, y + 11), K, s, F.font(6.5f)); };
    std::snprintf(buf, sizeof buf, "%.1f", gr);
    pillAt(642, buf, 296);
    std::snprintf(buf, sizeof buf, "%.0f", std::max(-99.0f, toDb(outPk)));
    pillAt(665, buf, 296);
    pillAt(689, buf, 296);
    std::snprintf(buf, sizeof buf, "%.0f", std::max(-99.0f, in));
    pillAt(711, buf, 296);
    pillAt(642, "GR", 312);
    pillAt(665, "L", 312);
    pillAt(689, "R", 312);
    pillAt(711, "IN", 312);
  }
  // ---- control strip (1, 250, 634 × 95)
  skin::well(dl, F.p(1, 250), F.p(635, 345), K, 3 * F.s);
  auto valPill = [&](float x, float y, int param, float w) {
    auto d = describeParam(a.doc.song(), t.pid(param));
    if (!d) return;
    std::string s = d->format(val(st, param));
    skin::pill(dl, F.p(x, y), F.p(x + w, y + 11), K, s.c_str(), F.font(6.5f));
  };
  flk::wheel(c, EmInput, 19, 262, 54, "Input");
  valPill(18, 333, EmInput, 56);
  flk::text(c, 100, 262, "CHARACTER");
  flk::menu(c, EmCharacter, 100, 280, 80, 20);
  flk::wheel(c, EmLowEmph, 196, 262, 44, "Low", skin::WheelBigLine | skin::WheelCentered);
  flk::wheel(c, EmHighEmph, 251, 262, 44, "High", skin::WheelBigLine | skin::WheelCentered);
  flk::wheel(c, EmClip, 306, 262, 44, "Clip");
  flk::wheel(c, EmRelease, 361, 262, 44, "Release");
  valPill(194, 321, EmLowEmph, 48);
  valPill(249, 321, EmHighEmph, 48);
  valPill(304, 321, EmClip, 48);
  valPill(359, 321, EmRelease, 48);
  static const char* ms[2] = {"L/R", "M/S"};
  flk::text(c, 414, 262, "CHANNELS");
  flk::sw(c, EmStereoMode, 414, 280, 62, 20, ms, 2, true);
  flk::wheel(c, EmLink, 486, 262, 44, "Link");
  valPill(484, 321, EmLink, 48);
  flk::wheel(c, EmCeiling, 563, 262, 54, "Ceiling");
  valPill(562, 333, EmCeiling, 56);
  // ---- bottom switches
  skin::label(dl, F.p(48, 0).x, F.p(0, 349).y, "LOUDNESS", K.accent2C(), F.font(11));
  flk::tog(c, EmDcBlock, 150, 347, 56, 16, "DC BLOCK");
  flk::tog(c, EmClipHard, 210, 347, 64, 16, "HARD CLIP");
  flk::text(c, 286, 350, "OS");
  static const char* os[3] = {"OFF", "2X", "4X"};
  flk::sw(c, EmOversample, 302, 347, 96, 16, os, 3, true);
  flk::tog(c, EmDelta, 410, 347, 48, 16, "DELTA");
  flk::tog(c, EmLevelMatch, 462, 347, 100, 16, "LEVEL MATCHING");
  F.end(kW, kH);
}

}  // namespace gl::packs::dynamics
