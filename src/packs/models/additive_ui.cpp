// Gridline — Additive editor on the reference subtractive-additive synth's
// form (848 × 500 form px, keyboard row dropped) with two pages:
//   SYNTH     the reference layout: timbre (protection, sub-harmonics, timbre
//             EQ, colour, phase), harmonic mask, volume envelope + pluck,
//             master rails and pitch block, filter (envelope with slopes, LFO,
//             key, width, cutoff, resonance shape / offset / self-oscillation),
//             unison rails, phaser, harmonizer, LFO and the effects row.
//   PARTIALS  Gridline's additive extras: drawn partials, timbre A / B morph,
//             partial shaping (count, tilt, odd/even, prism, scatter, seed,
//             blur), the live spectrum, the full envelopes and modulation depths.
// Each block's wheels take the reference block colour.
#include <cstdlib>
#include <map>
#include "../../app/ContextMenus.h"
#include "../../core/I18n.h"
#include "../../engine/Sample.h"
#include "../flc_kit.h"
#include "../../engine/Dsp.h"
#include "models_widgets.h"

namespace gl::packs::models {

using namespace ui2;

namespace {

constexpr double kRefNote = 48;  // spectrum preview note (C3)
constexpr float kW = 848, kH = 520, kTop = 20;  // 20 px page strip above the reference grid

void spectrumGraph(App& a, const PluginTarget& t, const PluginState& st, ImVec2 size) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p = graphFrame(size, "##spec", "SPECTRUM  C3", a);
  bool act = ImGui::IsItemActive(), hov = ImGui::IsItemHovered();
  const double lo = 20, hi = 20000;
  auto xOf = [&](double hz) { return p.x + static_cast<float>(std::log(hz / lo) / std::log(hi / lo)) * size.x; };
  const float top = p.y + 20, bot = p.y + size.y - 18, h = bot - top;
  auto yOf = [&](double db) { return top + static_cast<float>((12.0 - std::clamp(db, -60.0, 12.0)) / 72.0) * h; };
  freqLabels(a, dl, ImVec2(p.x, p.y), size, lo, hi);
  for (double db : {0.0, -24.0, -48.0}) dl->AddLine(ImVec2(p.x, yOf(db)), ImVec2(p.x + size.x, yOf(db)), P.gridStep);
  AdditiveModel m;
  m.read(st);
  AdditiveData custom;
  if (m.specA == SpCustom || m.specB == SpCustom) {
    parseAdditiveData(st.data, custom);
    m.custom = &custom;
  }
  const double f0 = dsp::noteToHz(kRefNote);
  const double cut = m.cutoff * std::pow(2.0, m.keyTrack * (kRefNote - 60.0) / 12.0);
  float peak = 1e-6f;
  for (int k = 1; k <= m.partials; ++k) peak = std::max(peak, m.timbre(k, m.morph));
  dl->PushClipRect(ImVec2(p.x, top - 2), ImVec2(p.x + size.x, bot), true);
  for (int k = 1; k <= m.partials; ++k) {
    double hz = f0 * m.ratio(k);
    if (hz > hi) break;
    float g = m.timbre(k, m.morph) / peak;
    float gf = g * m.filter(hz, cut) * m.phaser(hz, m.phOffset);
    float x = xOf(hz);
    if (g > 1e-4f) dl->AddLine(ImVec2(x, bot), ImVec2(x, yOf(dsp::gainToDb(g))), theme::withAlpha(P.textFaint, 0.5f), 1.0f);
    if (gf > 1e-4f) dl->AddLine(ImVec2(x, bot), ImVec2(x, yOf(dsp::gainToDb(gf))), k <= kAdDrawn ? P.paper : P.textDim, 2.0f);
  }
  ImVec2 prev, prevP;
  for (int i = 0; i <= 200; ++i) {
    double hz = lo * std::pow(hi / lo, i / 200.0);
    ImVec2 q(xOf(hz), yOf(dsp::gainToDb(std::max(1e-4f, m.filter(hz, cut)))));
    ImVec2 qp(xOf(hz), yOf(dsp::gainToDb(std::max(1e-4f, m.phaser(hz, m.phOffset)))));
    if (i) {
      if (m.fltType != AfOff) dl->AddLine(prev, q, P.accent, 2.0f);
      if (m.phMix > 0) dl->AddLine(prevP, qp, theme::withAlpha(P.accent, 0.45f), 1.0f);
    }
    prev = q;
    prevP = qp;
  }
  if (m.fltType != AfOff) {
    float cx = xOf(cut);
    dl->AddCircleFilled(ImVec2(cx, yOf(dsp::gainToDb(std::max(1e-4f, m.filter(cut, cut))))), 4.5f, P.accent);
  }
  dl->PopClipRect();
  if (act) {
    ImVec2 ms = ImGui::GetIO().MousePos;
    double hz = lo * std::pow(hi / lo, std::clamp((ms.x - p.x) / size.x, 0.0f, 1.0f));
    double base = hz / std::pow(2.0, m.keyTrack * (kRefNote - 60.0) / 12.0);
    a.doc.setParam(t.pid(AdFltCutoff), std::clamp(base, 30.0, 20000.0));
    a.doc.setParam(t.pid(AdFltReso), std::clamp(1.0 - (ms.y - top) / h * 1.6, 0.0, 1.0));
  }
  if (ImGui::IsItemDeactivated()) a.doc.endGesture();
  if (hov) {
    float wh = ImGui::GetIO().MouseWheel;
    if (wh != 0) {
      a.doc.setParam(t.pid(AdFltWidth), std::clamp(static_cast<double>(m.width) * (wh > 0 ? 1.12 : 1 / 1.12), 0.1, 8.0), false);
      ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    }
    a.setHint("Partials after the filter (bright) and before it (faint)   drag: cutoff / resonance · wheel: filter width");
  }
}

// Bar view of the custom timbre table (read-only; the table comes from an
// analysed single-cycle wave or an imported patch).
void customTable(const flk::Ctx& c, const PluginState& st, float x, float y, float w, float h) {
  ImDrawList* dl = c.dl;
  ImVec2 p0 = c.F.p(x, y), p1 = c.F.p(x + w, y + h);
  skin::graphWell(dl, p0, p1, c.K);
  AdditiveData d;
  parseAdditiveData(st.data, d);
  if (d.level.empty()) {
    flk::text(c, x + 6, y + h * 0.5f - 4, "NO CUSTOM TABLE  (USE A SPECTRUM, OR IMPORT A PATCH)");
    return;
  }
  const int n = std::min<int>(static_cast<int>(d.level.size()), 64);
  const float bw = (p1.x - p0.x - 4) / static_cast<float>(n);
  for (int k = 0; k < n; ++k) {
    const float v = std::clamp(std::fabs(d.level[static_cast<size_t>(k)]), 0.0f, 1.0f);
    const float bx = p0.x + 2 + k * bw, by = p1.y - 2 - (p1.y - p0.y - 4) * std::sqrt(v);
    dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + std::max(1.0f, bw - 1), p1.y - 2), c.K.fill);
  }
}

const skin::Skin& additiveSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x4B3A44, 0x56434F, 0x533E4A, 0x33C1E1, 0x929EA5, 0xDCCFD7);
    s.handle = skin::hex(0x5B5F66);
    s.select = skin::hex(0x33C1E1);
    s.accent2 = skin::hex(0xFFA830);
    s.lcdBg = skin::hex(0x262B2F);
    s.lcdText = skin::hex(0xAEB5B7);
    s.pillBg = skin::hex(0x3B2D35);
    s.graphBg = skin::hex(0x2A2128);
    s.graphGrid = skin::hex(0x3A2E36);
    s.graphGridStrong = skin::hex(0x4A3B44);
    s.graphCurve = skin::hex(0x33C179);
    return s;
  }();
  return k;
}

skin::Skin tinted(const skin::Skin& k, uint32_t fill) {
  skin::Skin s = k;
  s.fill = skin::hex(fill);
  s.fillPressed = skin::lighten(s.fill, 0.18f);
  return s;
}

int& pageOf(const PluginTarget& t) {
  static std::map<uint64_t, int> m;
  auto it = m.find(t.key());
  if (it != m.end()) return it->second;
  int p = 0;
  if (const char* e = std::getenv("GL_ADDITIVE_TAB")) p = std::clamp(std::atoi(e), 0, 1);  // dev: screenshots
  return m[t.key()] = p;
}

// A small "VEL" routing toggle.
void vel(const flk::Ctx& c, int param, float x, float y) { flk::tog(c, param, x, y, 29, 14, "VEL"); }

void synthPage(App& a, const PluginTarget& t, const flk::Ctx& c, const flk::Ctx& cG, const flk::Ctx& cO, const flk::Ctx& cP,
               const flk::Ctx& cL, const flk::Ctx& cV, const flk::Ctx& cF) {
  const float o = kTop;
  // ---- timbre: protection, sub-harmonics, timbre EQ, colour, phase
  flk::section(c, 22, o + 2, 222, 154, "TIMBRE");
  flk::vsl(c, AdProtect, 34, o + 18, 17, 80, "PROT");
  flk::vsl(cO, AdSub, 62, o + 18, 17, 80, "S1");
  flk::vsl(cO, AdSub3, 82, o + 18, 17, 80, "S3");
  flk::vsl(cO, AdSub4, 102, o + 18, 17, 80, "S4");
  for (int i = 0; i < kAdTeq; ++i) {
    char lb[4];
    std::snprintf(lb, sizeof lb, "%d", i + 1);
    flk::vsl(c, AdTeqBase + i, 130 + i * 18.0f, o + 18, 17, 80, lb);
  }
  flk::text(c, 30, o + 130, "COLOUR");
  flk::menu(c, AdSpectrumA, 84, o + 126, 112, 22);
  flk::wheel(c, AdPhaseRand, 206, o + 118, 30, "Phase", skin::WheelBigLine | skin::WheelCentered);

  // ---- harmonic mask
  flk::section(c, 248, o + 2, 336, 70, "MASK");
  ImGui::SetCursorScreenPos(c.F.p(256, o + 16));
  barEditor(a, t, *t.state(a.doc.song()), AdMaskBase, kAdMask, c.F.sz(214, 50), "mask", c.K.accent2C(), 1.0f, "Harmonic mask (repeats every 12 partials)");
  {
    const flk::Row row(c, o + 12, 36);  // J-128: one centre line, one label line
    flk::wheel(c, AdMaskMix, 478, o + 12, 36, "Mix");
    flk::wheel(cO, AdMaskLfo, 528, o + 16, 30, "LFO", skin::WheelBigLine | skin::WheelCentered);
  }

  // ---- volume envelope, tremolo, pluck
  flk::section(c, 248, o + 76, 336, 80, "ENVELOPE  ·  PLUCK");
  {
    const flk::Row row(c, o + 86, 38);  // J-128: tremolo, envelope and pluck wheels on one line
    flk::wheel(cO, AdTremolo, 256, o + 92, 30, "Trem");
    flk::wheel(c, AdAttack, 296, o + 88, 34, "Att");
    flk::wheel(c, AdDecay, 336, o + 88, 34, "Dec");
    flk::wheel(c, AdRelease, 376, o + 88, 34, "Rel");
    flk::wheel(cP, AdPluckTime, 420, o + 86, 38, "Pluck");
    flk::wheel(cP, AdPluck, 466, o + 92, 28, "Amt");
  }
  vel(c, AdVelAtt, 298, o + 138);
  vel(c, AdVelRel, 378, o + 138);
  vel(c, AdVelPluck, 424, o + 138);
  flk::chk(c, AdPluckMask, 502, o + 84, 78, 13, "MASKED");
  flk::chk(c, AdPluckAlt, 502, o + 100, 78, 13, "ALT");
  flk::chk(c, AdPluckRel, 502, o + 116, 78, 13, "ON RELEASE");

  // ---- master rails + pitch block
  flk::section(c, 588, o + 2, 110, 154, "MASTER");
  flk::vsl(c, AdVolume, 596, o + 18, 19, 104, "VOL");
  flk::vsl(c, AdPitch, 634, o + 18, 19, 104, "PITCH");
  flk::vsl(c, AdDetune, 664, o + 18, 19, 104, "DET");
  vel(c, AdVelVol, 594, o + 138);

  flk::section(c, 702, o + 2, 140, 154, "PITCH");
  flk::wheel(cO, AdGrit, 712, o + 16, 30, "Grit", skin::WheelBigLine | skin::WheelCentered);
  flk::wheel(cO, AdVibrato, 712, o + 60, 30, "Vib");
  flk::wheel(cO, AdLfoPitch, 712, o + 104, 30, "LFO");
  flk::text(c, 756, o + 20, "MODE");
  flk::menu(c, AdPlayMode, 756, o + 32, 80, 20);
  flk::wheel(c, AdGlide, 762, o + 66, 30, "Glide");
  flk::wheel(c, AdFine, 802, o + 66, 30, i18n::tr("Fine"), skin::WheelBigLine | skin::WheelCentered);  // J-92: fine tune had no control
  vel(c, AdVelPorta, 798, o + 112);

  // ---- filter
  flk::section(c, 22, o + 160, 614, 128, "FILTER");
  flk::chk(c, AdFEnvAdaptive, 30, o + 184, 58, 13, "ADAPT");
  {
    const flk::Row row(c, o + 166, 50);  // J-128: the filter's top wheels on one centre line (o + 191)
    flk::wheel(cG, AdFEnvAtt, 94, o + 170, 40, "Att");
    flk::wheel(cG, AdFEnvDec, 138, o + 170, 40, "Dec");
    flk::wheel(cG, AdFEnvAmt, 184, o + 166, 50, "Env", skin::WheelBigLine | skin::WheelCentered);
    flk::wheel(cO, AdLfoCutoff, 272, o + 176, 30, "LFO", skin::WheelBigLine | skin::WheelCentered);
    flk::wheel(cG, AdFltKey, 306, o + 176, 30, "Key");
    flk::wheel(cG, AdFltWidth, 340, o + 174, 34, "Width");
    flk::wheel(cG, AdFltCutoff, 384, o + 166, 50, "Cutoff");
    flk::wheel(cP, AdFltReso, 442, o + 171, 40, "Reso");
    flk::wheel(cP, AdResOffset, 492, o + 176, 30, "Ofs", skin::WheelBigLine | skin::WheelCentered);
    flk::wheel(cP, AdResOsc, 526, o + 176, 30, "Osc");
  }
  flk::wheel(cG, AdFEnvAttSlope, 99, o + 232, 30, "Slope", skin::WheelBigLine | skin::WheelCentered);
  flk::wheel(cG, AdFEnvDecSlope, 143, o + 232, 30, "Slope", skin::WheelBigLine | skin::WheelCentered);
  vel(c, AdVelFEnv, 194, o + 236);
  flk::menu(c, AdFltType, 272, o + 240, 112, 20);
  flk::menu(c, AdResType, 446, o + 240, 90, 20);
  static const char* kScale[2] = {"OCT", "HZ"};
  flk::sw(c, AdFltScale, 388, o + 262, 40, 13, kScale, 2, true);
  flk::sw(c, AdResScale, 446, o + 262, 40, 13, kScale, 2, true);
  flk::chk(c, AdResTrack, 546, o + 230, 86, 13, "TRACK MOTION");
  flk::chk(c, AdResAdaptive, 546, o + 246, 86, 13, "ADAPT WIDTH");

  // ---- unison
  flk::section(c, 640, o + 160, 202, 128, "UNISON");
  flk::digi(cF, AdUniOrder, 690, o + 184, 24, 22);
  flk::textC(c, 702, o + 210, "ORDER");
  flk::vsl(cF, AdUniSpread, 728, o + 176, 17, 88, "PAN");
  flk::vsl(cF, AdUniDetune, 750, o + 176, 17, 88, "PITCH");
  flk::vsl(cF, AdUniPhase, 772, o + 176, 17, 88, "PHASE");
  flk::vsl(cF, AdUniVar, 794, o + 176, 17, 88, "VAR");
  flk::menu(c, AdUniType, 648, o + 226, 70, 20);
  vel(c, AdVelUni, 686, o + 252);

  // ---- phaser
  flk::section(c, 22, o + 292, 300, 120, "PHASER");
  {
    const flk::Row row(c, o + 298, 50);  // J-128: one label line under the mixed sizes
    flk::wheel(cL, AdPhMix, 94, o + 298, 50, "Mix");
    flk::wheel(cL, AdPhDensity, 148, o + 303, 40, "Width");
    flk::wheel(cL, AdPhOffset, 192, o + 303, 40, "Offset");
    flk::wheel(cL, AdPhSpeed, 236, o + 306, 34, "Motion", skin::WheelBigLine | skin::WheelCentered);
    flk::wheel(cL, AdPhKey, 278, o + 308, 30, "Key", skin::WheelBigLine | skin::WheelCentered);
  }
  flk::menu(c, AdPhType, 30, o + 366, 66, 20);
  flk::sw(c, AdPhScale, 104, o + 368, 40, 13, kScale, 2, true);
  flk::wheel(cO, AdPhWidthLfo, 153, o + 362, 30, "LFO", skin::WheelBigLine | skin::WheelCentered);
  flk::wheel(cO, AdLfoPhaser, 197, o + 362, 30, "LFO", skin::WheelBigLine | skin::WheelCentered);

  // ---- harmonizer
  flk::section(c, 326, o + 292, 310, 120, "HARMONIZER");
  {
    const flk::Row row(c, o + 298, 50);  // J-128
    flk::wheel(cV, AdHzMix, 380, o + 298, 50, "Mix");
    flk::wheel(cV, AdHzWidth, 434, o + 303, 40, "Width");
    flk::wheel(cV, AdHzStrength, 478, o + 308, 30, "Str");
  }
  flk::menu(c, AdHzType, 520, o + 300, 108, 20);
  flk::menu(c, AdHzPos, 520, o + 326, 108, 20);
  flk::wheel(cO, AdHzWidthLfo, 424, o + 362, 30, "LFO", skin::WheelBigLine | skin::WheelCentered);
  vel(c, AdVelHarm, 460, o + 370);

  // ---- LFO
  flk::section(c, 640, o + 292, 202, 120, "LFO");
  flk::menu(c, AdLfoShape, 650, o + 368, 108, 20);
  {
    const flk::Row row(c, o + 298, 50);  // J-128: fade on the speed wheel's centre line
    flk::wheel(cO, AdLfoAttack, 694, o + 306, 30, "Fade");
    flk::wheel(cO, AdLfoRate, 744, o + 298, 50, "Speed");
  }
  flk::chk(c, AdLfoGlobal, 650, o + 394, 70, 13, "GLOBAL");
  flk::chk(c, AdLfoTempo, 764, o + 394, 70, 13, "TEMPO");

  // ---- effects
  flk::section(c, 22, o + 418, 820, 78, "EFFECTS");
  const flk::Row fxRow(c, o + 428, 38);  // J-128: one centre line, one label line
  flk::menu(c, AdChType, 30, o + 434, 90, 20);
  flk::wheel(cV, AdFx + FxChorus, 126, o + 428, 38, "Chorus");
  flk::menu(c, AdDelType, 182, o + 434, 86, 20);
  flk::wheel(cL, AdFx + FxDelay, 278, o + 428, 38, "Delay");
  flk::wheel(cL, AdFx + FxDelayFeedback, 322, o + 432, 30, "Fdbk");
  flk::wheel(cL, AdFx + FxDelayTime, 358, o + 432, 30, "Time");
  flk::menu(c, AdRevType, 404, o + 434, 92, 20);
  flk::wheel(cV, AdFx + FxReverb, 504, o + 428, 38, "Reverb");
  flk::wheel(cV, AdFx + FxRoomSize, 550, o + 432, 30, "Room");
  flk::menu(c, AdCompType, 600, o + 434, 76, 20);
  flk::wheel(c, AdComp, 684, o + 428, 38, "Comp");
  flk::digi(c, AdVoices, 760, o + 437, 30, 20);
  flk::textC(c, 775, o + 467.5f, "VOICES", 0, 7.5f);  // J-128: on the wheels' label line
}

void partialsPage(App& a, const PluginTarget& t, const PluginState& st, const flk::Ctx& c, const flk::Ctx& cG, const flk::Ctx& cO,
                  const flk::Ctx& cP) {
  const float o = kTop;
  const skin::Skin& K = c.K;
  // ---- drawn partials + timbre A / B morph
  flk::section(c, 4, o + 4, 468, 148, "DRAWN PARTIALS  ·  MORPH");
  ImGui::SetCursorScreenPos(c.F.p(12, o + 22));
  barEditor(a, t, st, AdDrawnBase, kAdDrawn, c.F.sz(300, 92), "drawn", K.fill, 1.0f, "Partial");
  flk::wheel(c, AdMorph, 326, o + 24, 34, "Morph");
  flk::wheel(cO, AdMorphEnv, 372, o + 24, 34, "Env", skin::WheelBigLine | skin::WheelCentered);
  flk::wheel(cO, AdLfoMorph, 418, o + 24, 34, "LFO", skin::WheelBigLine | skin::WheelCentered);
  flk::text(c, 12, o + 124, "A");
  flk::menu(c, AdSpectrumA, 24, o + 120, 120, 20);
  flk::text(c, 152, o + 124, "B");
  flk::menu(c, AdSpectrumB, 164, o + 120, 120, 20);
  flk::wheel(cP, AdPluck, 326, o + 86, 34, "Pluck");
  flk::wheel(c, AdSustain, 372, o + 86, 34, "Sustain");
  flk::wheel(cG, AdFEnvSus, 418, o + 86, 34, "F.sus");

  // ---- partial shaping
  flk::section(c, 476, o + 4, 368, 148, "PARTIALS");
  const int pp[8] = {AdPartials, AdTilt, AdOddEven, AdStretch, AdScatter, AdSeed, AdBlur, AdFEnvRel};
  const char* pn[8] = {"Count", "Tilt", "Odd/ev", "Prism", "Scatter", "Seed", "Blur", "F.rel"};
  for (int i = 0; i < 8; ++i) flk::wheel(i == 7 ? cG : c, pp[i], 492 + (i % 4) * 88, o + 24 + (i / 4) * 62, 34, pn[i]);

  // ---- spectrum + custom table
  flk::section(c, 4, o + 156, 840, 196, "");
  ImGui::SetCursorScreenPos(c.F.p(8, o + 160));
  spectrumGraph(a, t, st, c.F.sz(560, 188));
  flk::text(c, 578, o + 162, "CUSTOM TIMBRE TABLE");
  customTable(c, st, 578, o + 176, 260, 150);
  const PluginTarget tc = t;
  if (flk::btn(c, "loadCycle", 578, o + 330, 128, 16, "ANALYSE A CYCLE...", false))
    menus::fileDialog(a, "Analyse single-cycle wave", false, {".wav", ".flac", ".ogg", ".wv"}, "", [&a, tc](const std::string& path) {
      const PluginState* s0 = tc.state(a.doc.song());
      SamplePtr smp = sampleCache().get(path);
      if (!s0 || !smp || smp->left.size() < 8) return;
      AdditiveData d = analyzeCycle(smp->left.data(), static_cast<int>(std::min<size_t>(smp->left.size(), 8192)), 256);
      PluginState ns = *s0;
      ns.data = serializeAdditiveData(d);
      ns.params[AdSpectrumA] = SpCustom;
      setPluginState(a, tc, ns, "Analyse single cycle");
    });
  if (flk::btn(c, "clearCycle", 712, o + 330, 126, 16, "CLEAR TABLE", false) && !st.data.empty()) {
    PluginState ns = st;
    ns.data.clear();
    setPluginState(a, t, ns, "Clear custom table");
  }

  // ---- modulation depths
  flk::section(c, 4, o + 356, 840, 140, "MODULATION");
  flk::wheel(cO, AdLfoPitch, 30, o + 382, 38, "> Pitch");
  flk::wheel(cO, AdLfoCutoff, 100, o + 382, 38, "> Cutoff", skin::WheelBigLine | skin::WheelCentered);
  flk::wheel(cO, AdLfoPhaser, 170, o + 382, 38, "> Phaser", skin::WheelBigLine | skin::WheelCentered);
  flk::wheel(cO, AdLfoMorph, 240, o + 382, 38, "> Morph", skin::WheelBigLine | skin::WheelCentered);
  flk::wheel(cO, AdMaskLfo, 310, o + 382, 38, "> Mask", skin::WheelBigLine | skin::WheelCentered);
  flk::wheel(cO, AdPhWidthLfo, 380, o + 382, 38, "> Density", skin::WheelBigLine | skin::WheelCentered);
  flk::wheel(cO, AdHzWidthLfo, 450, o + 382, 38, "> Harm", skin::WheelBigLine | skin::WheelCentered);
  flk::wheel(cO, AdTremolo, 520, o + 382, 38, "Tremolo");
  flk::menu(c, AdLfoShape, 600, o + 386, 110, 20);
  {
    const flk::Row row(c, o + 379, 44);  // J-128: on the depth wheels' centre line (o + 401)
    flk::wheel(cO, AdLfoRate, 730, o + 379, 44, "Rate");
    flk::wheel(cO, AdLfoAttack, 790, o + 384, 34, "Fade");
  }
}

}  // namespace

void drawAdditive(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  const skin::Skin& K = additiveSkin();
  static const skin::Skin kGreen = tinted(K, 0x33C179), kOrange = tinted(K, 0xFFA830), kPink = tinted(K, 0xCA2972),
                          kLime = tinted(K, 0x67C133), kViolet = tinted(K, 0x9944CB), kFat = [&] {
                            skin::Skin s = tinted(K, 0x33C1B7);
                            s.rail = skin::hex(0x6B6B6B);
                            return s;
                          }();
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  flk::Ctx c{a, t, F, K, dl};
  flk::Ctx cG{a, t, F, kGreen, dl}, cO{a, t, F, kOrange, dl}, cP{a, t, F, kPink, dl}, cL{a, t, F, kLime, dl}, cV{a, t, F, kViolet, dl},
      cF{a, t, F, kFat, dl};
  ImGui::PushID("gl.additive");
  int& page = pageOf(t);
  static const char* kPages[2] = {"SYNTH", "PARTIALS"};
  for (int i = 0; i < 2; ++i)
    if (flk::btn(c, kPages[i], 6 + i * 74.0f, 3, 70, 15, kPages[i], page == i)) page = i;
  if (page == 0)
    synthPage(a, t, c, cG, cO, cP, cL, cV, cF);
  else
    partialsPage(a, t, st, c, cG, cO, cP);
  F.end(kW, kH);
  ImGui::PopID();
}

}  // namespace gl::packs::models
