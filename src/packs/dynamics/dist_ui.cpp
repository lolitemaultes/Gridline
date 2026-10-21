// Gridline — "dynamics" pack: editor for Transient Split (gl.transplit), laid
// out on the reference transient-splitter panel's grid (740 × 366 form units,
// J-23): a wide history display on top; below it the detector column
// (sensitivity, detection speed, hold / release), then a transient and a
// sustain section, each with an on/off box, a level rail, an effect menu and
// a 2 × 2 wheel block (effect mix, tilt, effect A / B); the bottom strip holds
// Gridline's band split / drive / mix / gain and the link / look-ahead switches.
// Overdrive / Squeeze: dist_fl_ui.cpp; Clip Dist (gl.fastdist): app/PluginEditorsK3.cpp.
#include "../../core/I18n.h"
#include "../flc_kit.h"
#include "dyn_widgets.h"

namespace gl::packs::dynamics {

using namespace ui;

namespace {

const skin::Skin& transplitSkin() {
  static const skin::Skin k = [] {
    // Reference colour roles: slate panel, dark display, light value labels,
    // teal latch switches, dark menu wells.
    skin::Skin s = skin::Skin::fl(0x3E4244, 0x46494B, 0x2C3032, 0xFFB238, 0x30373A, 0xD2D2D2);
    s.fillPressed = skin::hex(0xFFD08A);
    s.pillBg = skin::hex(0x111111);
    s.pillText = skin::hex(0xD2D2D2);
    s.graphBg = skin::hex(0x383636);
    s.graphGrid = skin::hex(0x464444);
    s.graphGridStrong = skin::hex(0x5A5858);
    s.graphText = skin::hex(0x9A9898);
    s.select = skin::hex(0x124D54);
    s.accent2 = skin::hex(0x4FC3CF);
    s.lcdBg = skin::hex(0x1C2022);
    s.lcdText = skin::hex(0xE2E2E2);
    s.handle = skin::hex(0x8B8E97);
    return s;
  }();
  return k;
}

// Captions of an effect slot's A / B wheels by effect kind.
void fxLabels(int kind, const char*& a, const char*& b) {
  static const char* A[TsFxCount] = {"Drive", "Spacing", "Depth", "Depth", "Feedback", "-"};
  static const char* B[TsFxCount] = {"Tone", "Decay", "Size", "Length", "Time", "-"};
  kind = std::clamp(kind, 0, TsFxCount - 1);
  a = A[kind], b = B[kind];
}

// One transient / sustain section at form x0 (reference columns: on box at
// x0, rail at x0 + 38, menu at x0 + 111, wheels at x0 + 90 and x0 + 165).
// J-45 reference engine: the slot's stored controls for an effect kind
// (mix, A, B; -1 = none) — saturation drive / warmth, bounce time / amp
// curve, diffusion depth / delay, delay feedback / time (synced or free).
void refSlot(const PluginState& st, int side, int kind, int& mix, int& a, int& b) {
  mix = a = b = -1;
  switch (kind) {
    case TsFxSaturate: mix = 13 + 3 * side, a = 14 + 3 * side, b = 15 + 3 * side; break;
    case TsFxBounce: mix = 19 + 3 * side, a = 21 + 3 * side, b = 20 + 3 * side; break;
    case TsFxDiffuse: mix = 7 + 3 * side, a = 8 + 3 * side, b = 9 + 3 * side; break;
    case TsFxEcho: {
      const int base = 25 + 4 * side;
      mix = base, a = base + 3, b = val(st, tsRaw(40 + side)) >= 0.5f ? base + 2 : base + 1;
      break;
    }
    default: return;
  }
  mix = tsRaw(mix), a = tsRaw(a), b = tsRaw(b);
}

void section(const flk::Ctx& c, const PluginState& st, int side, float x0) {
  const bool ref = val(st, TsModel) >= 0.5f;
  const int o = side * kTsSlotStride;
  const int onP = ref ? tsRaw(36 + side) : side ? TsSusOn : TsTransOn, lvlP = ref ? tsRaw(3 + side) : side ? TsRelease : TsAttack,
            tiltP = ref ? tsRaw(5 + side) : side ? TsSusTilt : TsTransTilt;
  const int syncP = ref ? tsRaw(40 + side) : side ? TsSusSync : TsTransSync;
  const bool on = val(st, onP) >= 0.5f;
  flk::chk(c, onP, x0, 206, 16, 14, nullptr);
  flk::text(c, x0 + 72, 211, side ? "BODY" : "ATTACK", on ? c.K.text : c.K.textDim);
  // Level rail with its value plate above.
  {
    char txt[24];
    // Reference level law: -120 + 132·n^(1/4) dB (n = stored / 12800).
    const double db = ref ? -120.0 + 132.0 * std::pow(std::clamp(val(st, lvlP) / 12800.0, 0.0, 1.0), 0.25) : 0.0;
    const double g = ref ? std::pow(10.0, db / 20.0) : tsLevelGain(val(st, lvlP));
    if (!on || g <= 1e-6) std::snprintf(txt, sizeof txt, "OFF");
    else std::snprintf(txt, sizeof txt, "%+.1f", 20.0 * std::log10(g));
    skin::valueStrip(c.dl, c.F.p(x0 + 32, 209), c.F.p(x0 + 68, 221), c.K, txt, c.F.font(7.5f));
    flk::vsl(c, lvlP, x0 + 38, 224, 24, 98, "Level");
  }
  const int fxP = ref ? tsRaw(38 + side) : TsTransFx + o;
  const int fx = static_cast<int>(std::lround(val(st, fxP)));
  flk::menu(c, fxP, x0 + 111, 209, 79, 13);
  const char *la, *lb;
  fxLabels(fx, la, lb);
  int mixP = TsTransFxMix + o, aP = TsTransFxA + o, bP = TsTransFxB + o;
  if (ref) refSlot(st, side, fx, mixP, aP, bP);
  if (mixP >= 0) flk::wheel(c, mixP, x0 + 90, 229, 42, "Mix");
  flk::wheel(c, tiltP, x0 + 165, 229, 42, "Tilt", skin::WheelBigLine | skin::WheelCentered);
  if (aP >= 0) flk::wheel(c, aP, x0 + 90, 287, 42, la);
  if (bP >= 0) flk::wheel(c, bP, x0 + 165, 287, 42, lb);
  if (fx == TsFxEcho) flk::tog(c, syncP, x0 + 206, 328, 12, 13, "T");
}

// History display: input level (grey), transient part (accent) and sustain
// part (teal) over the last few seconds, dB scale.
void display(const flk::Ctx& c, App& a, const PluginTarget& t) {
  const skin::Skin& K = c.K;
  float in = -120, tr = -120, su = -120;
  if (auto rt = runtime(a, t)) {
    in = rt->input.load();
    tr = rt->bands[0].load();
    su = rt->bands[1].load();
  }
  History& h = history(t.key() ^ 0x54534c54ull);
  if (h.due(60)) {
    float v[3] = {in, tr, su};
    h.push(v, 3);
  }
  const ImVec2 p0 = c.F.p(4, 4), p1 = c.F.p(736, 200);
  skin::graphWell(c.dl, p0, p1, K);
  c.dl->PushClipRect(p0, p1, true);
  auto Y = [&](float db) { return p1.y - (std::clamp(db, -60.0f, 0.0f) + 60.0f) / 60.0f * (p1.y - p0.y - 8); };
  for (int db = -48; db < 0; db += 12) {
    const float y = Y(static_cast<float>(db));
    c.dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), K.graphGrid);
    char lab[8];
    std::snprintf(lab, sizeof lab, "%d", db);
    skin::text(c.dl, ImVec2(p0.x + 4, y - c.F.font(7.0f) - 1), K.graphText, lab, c.F.font(7.0f));
  }
  const float w = p1.x - p0.x;
  const int cols = std::min(History::N, static_cast<int>(w / 1.5f));
  const float dx = w / static_cast<float>(cols);
  for (int i = 0; i < cols; ++i) {
    const float x = p1.x - static_cast<float>(i + 1) * dx;
    c.dl->AddRectFilled(ImVec2(x, Y(h.at(0, i))), ImVec2(x + dx + 0.5f, p1.y), theme::withAlpha(K.graphText, 0.25f));
  }
  for (int k = 1; k <= 2; ++k) {
    ImVec2 prev;
    for (int i = 0; i < cols; ++i) {
      const ImVec2 q(p1.x - static_cast<float>(i) * dx, Y(h.at(k, i)));
      if (i) c.dl->AddLine(prev, q, k == 1 ? K.fill : K.accent2C(), 1.5f * c.F.s);
      prev = q;
    }
  }
  skin::text(c.dl, c.F.p(600, 10), K.fill, "TRANSIENT", c.F.font(7.5f));
  skin::text(c.dl, c.F.p(668, 10), K.accent2C(), "SUSTAIN", c.F.font(7.5f));
  c.dl->PopClipRect();
}

}  // namespace

// ===================================================================== gl.transplit
void editorTransplit(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  const skin::Skin& K = transplitSkin();
  constexpr float kW = 740, kH = 366;
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  flk::Ctx c{a, t, F, K, dl};
  display(c, a, t);

  // Detector column (J-45 reference engine: its stored controls).
  const bool ref = val(st, TsModel) >= 0.5f;
  const int sensP = ref ? tsRaw(0) : TsSensitivity;
  flk::text(c, 47, 208, "SENSITIVITY");
  flk::wheel(c, sensP, 47, 223, 80, nullptr);
  {
    char txt[16];
    std::snprintf(txt, sizeof txt, "%.0f", val(st, sensP) * (ref ? 0.01 : 100.0));
    flk::textC(c, 87, 259, txt, K.text, 9.0f);
  }
  flk::text(c, 8, 315, "DETECT");
  flk::menu(c, ref ? tsRaw(35) : TsAttackShape, 84, 313, 56, 13);
  flk::wheel(c, ref ? tsRaw(1) : TsHold, 160, 212, 46, "Hold");
  flk::wheel(c, ref ? tsRaw(2) : TsReleaseShape, 160, 279, 46, "Release");

  section(c, st, 0, 246);
  section(c, st, 1, 492);

  // Bottom strip: name, Gridline's band split / drive / mix / gain, switches.
  skin::text(dl, F.p(8, 349), K.textDim, "TRANSIENT SPLIT", F.font(10.0f));
  struct Small {
    int param;
    const char* cap;
    float x;
  } smalls[] = {{TsSplitFreq, "SPLIT", 170}, {TsBalance, "BAL", 248}, {TsDrive, "DRIVE", 316}, {TsMix, "MIX", 392}, {TsGain, "GAIN", 460}};
  if (!ref)  // Gridline's own controls (the reference engine has none of them)
    for (const auto& s : smalls) {
      flk::text(c, s.x, 351, s.cap);
      flk::digi(c, s.param, s.x + 30, 347, 34, 15);
    }
  flk::tog(c, ref ? tsRaw(34) : TsStereoLink, 576, 347, 66, 18, "STEREO LINK");
  flk::tog(c, ref ? tsRaw(33) : TsLookahead, 645, 347, 66, 18, "LOOK-AHEAD");
  F.end(kW, kH);
}

// ===================================================================== gl.hitloop
// J-45 Hit Looper: levels | loops | variation | detector, in sections.
void editorHitLoop(App& a, const PluginTarget& t, float width) {
  {  // J-68: envelope graph: input (grey) and output with the loops (accent) over the last seconds
    float in = -120, out = -120;
    if (auto rt = runtime(a, t)) in = rt->input.load(), out = rt->bands[0].load();
    History& h = history(t.key() ^ 0x484C4556ull);
    if (h.due(60)) {
      float v[2] = {in, out};
      h.push(v, 2);
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + width - 24, p0.y + 96);
    const auto& P = theme::pal();
    pui::displayFrame(dl, p0, p1);
    dl->PushClipRect(p0, p1, true);
    auto Y = [&](float db) { return p1.y - 3 - (std::clamp(db, -60.0f, 0.0f) + 60.0f) / 60.0f * (p1.y - p0.y - 8); };
    for (int db = -48; db < 0; db += 12) dl->AddLine(ImVec2(p0.x, Y(static_cast<float>(db))), ImVec2(p1.x, Y(static_cast<float>(db))), theme::withAlpha(P.text, 0.08f));
    const float w = p1.x - p0.x;
    const int cols = std::min(History::N, static_cast<int>(w / 1.5f));
    const float dx = w / static_cast<float>(cols);
    for (int i = 0; i < cols; ++i) {
      const float x = p1.x - static_cast<float>(i + 1) * dx;
      dl->AddRectFilled(ImVec2(x, Y(h.at(0, i))), ImVec2(x + dx + 0.5f, p1.y), theme::withAlpha(P.text, 0.18f));
    }
    ImVec2 prev;
    for (int i = 0; i < cols; ++i) {
      const ImVec2 q(p1.x - static_cast<float>(i) * dx, Y(h.at(1, i)));
      if (i) dl->AddLine(prev, q, P.accent, 1.5f);
      prev = q;
    }
    dl->AddText(ImVec2(p0.x + 6, p0.y + 4), theme::withAlpha(P.text, 0.6f), i18n::tr("Input"));
    dl->AddText(ImVec2(p0.x + 60, p0.y + 4), P.accent, i18n::tr("Output"));
    dl->PopClipRect();
    ImGui::Dummy(ImVec2(width - 24, 100));
  }
  const float avail = width - 24, wL = avail * 0.18f, wP = avail * 0.34f, wV = avail * 0.24f, wD = avail * 0.24f;
  pui::beginSection("LEVELS", wL);
  pui::knobCell(a, t, HlDry, "Dry", wL - 12);
  pui::knobCell(a, t, HlLevelA, "Loop A", wL - 12);
  pui::knobCell(a, t, HlLevelB, "Loop B", wL - 12);
  pui::endSection();
  ImGui::SameLine(0, 8);
  pui::beginSection("LOOPS", wP);
  pui::choiceButtons(a, t, HlMode, wP - 12);
  pui::knobCell(a, t, HlMinLength, "Length", wP - 12);
  pui::knobCell(a, t, HlRatio, "A / B", wP - 12);
  pui::knobCell(a, t, HlBounce, "Bounce", wP - 12);
  pui::endSection();
  ImGui::SameLine(0, 8);
  pui::beginSection("VARIATION", wV);
  pui::choiceButtons(a, t, HlFixed, wV - 12);
  pui::knobCell(a, t, HlReverse, "Reverse", wV - 12);
  pui::knobCell(a, t, HlOctave, "Octave", wV - 12);
  pui::knobCell(a, t, HlOctaveUp, "Up / down", wV - 12);
  pui::knobCell(a, t, HlIgnore, "Skip", wV - 12);
  pui::endSection();
  ImGui::SameLine(0, 8);
  pui::beginSection("DETECTOR", wD);
  pui::knobCell(a, t, HlSensitivity, "Sensitivity", wD - 12);
  pui::knobCell(a, t, HlSpread, "Spread", wD - 12);
  pui::choiceButtons(a, t, HlSource, wD - 12);
  pui::choiceButtons(a, t, HlStereo, wD - 12);
  pui::toggleButton(a, t, HlFreeze, "Hold loops");
  pui::toggleButton(a, t, HlFlushOnStop, "Clear on stop");
  pui::endSection();
}

}  // namespace gl::packs::dynamics
