// Gridline — Twin String editor on the reference physical string synth's
// 855 × 532 grid and palette: top strip (title, read-out, transpose / tune /
// level, meter); the exciter column (noise level rail, noise rate, high cut
// with its envelope, drive, low cut); the strings block (per string: decay,
// damping, damp shape and offset, invert switches; the damping view switched
// between A and B; string-B ratio wheel; comb ratio rail, level and
// feedback with polarity); mix / stereo / saturation; the amp envelope
// column; the eight resonator bands; the modulation row (free envelope,
// LFO rate, eight slots of source / amount / sign / target); the effects
// row (delay, chorus, reverb). Every control is the shared skin kit's.
#include <cmath>
#include <cstdio>
#include "../flc_kit.h"
#include "models_widgets.h"
#include "strings.h"
#include "strings_laws.h"

namespace gl::packs::models {

using namespace strs;
using namespace ui2;

namespace {

const skin::Skin& twinSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x3B3836, 0x45413E, 0x393A3E, 0xD2B3A7, 0x958269, 0xCFC5A9);
    s.handle = skin::hex(0x212121);
    s.select = skin::hex(0xD2B3A7);
    s.accent2 = skin::hex(0x7FB2D0);
    s.lcdBg = skin::hex(0x262426);
    s.lcdText = skin::hex(0xD3D2B5);
    s.pillBg = skin::hex(0x2D383E);
    s.pillText = skin::hex(0xB8BDC1);
    s.graphBg = skin::hex(0x262426);
    s.graphGrid = skin::hex(0x343133);
    s.graphGridStrong = skin::hex(0x46423F);
    s.graphCurve = skin::hex(0xE6CBBE);
    return s;
  }();
  return k;
}

// Damping view: the loop's ring time across the string's harmonics for the
// selected string (A or B), as set by decay / damping / offset / shape.
void dampingView(const flk::Ctx& c, const PluginState& st, int string, float x, float y, float w, float h) {
  ImDrawList* dl = c.dl;
  ImVec2 p0 = c.F.p(x, y), p1 = c.F.p(x + w, y + h);
  skin::graphWell(dl, p0, p1, c.K);
  const int b = string ? TsS2Decay : TsS1Decay;
  const double decay = val(st, b, 1), damp = val(st, b + 3, 0), ofs = val(st, b + 2, 0), shape = val(st, b + 4, 0);
  const double f0 = 220.0, sr = 44100.0;
  double corner = f0 * std::pow(64.0, 1.0 - ofs) * std::pow(0.02, damp * damp);
  corner = std::clamp(corner, 30.0, sr * 0.45);
  const double a = damp <= 0.0005 ? 0.0 : std::exp(-dsp::kTwoPi * corner / sr), a2 = a * std::clamp(shape, 0.0, 1.0);
  const double t60 = strs::law::tsStringT60(decay);
  // Grid: harmonics 1 .. 32 (log), ring time 10 ms .. 120 s (log).
  for (int hmn : {1, 2, 4, 8, 16, 32}) {
    float gx = p0.x + (p1.x - p0.x) * static_cast<float>(std::log2(hmn) / 5.0);
    dl->AddLine(ImVec2(gx, p0.y + 2), ImVec2(gx, p1.y - 2), c.K.graphGrid, 1.0f);
  }
  ImVec2 prev;
  for (int i = 0; i <= 96; ++i) {
    double hmn = std::pow(2.0, 5.0 * i / 96.0), f = f0 * hmn, w2 = dsp::kTwoPi * f / sr;
    double m = 1;
    for (double aa : {a, a2})
      if (aa > 0) m *= (1 - aa) / std::sqrt(1 - 2 * aa * std::cos(w2) + aa * aa);
    // Fundamental is compensated to the decay knob; partials lose m / m(f0) per period.
    double m0 = 1;
    const double w0 = dsp::kTwoPi * f0 / sr;
    for (double aa : {a, a2})
      if (aa > 0) m0 *= (1 - aa) / std::sqrt(1 - 2 * aa * std::cos(w0) + aa * aa);
    double perPeriod = std::pow(10.0, -3.0 / (t60 * f0)) * m / std::max(1e-9, m0);
    double t = perPeriod < 1 ? -3.0 / (std::log10(std::max(1e-12, perPeriod)) * f0) : 120.0;
    float ny = 0.85f * static_cast<float>(std::clamp(std::log(std::max(0.01, t) / 0.01) / std::log(120.0 / 0.01), 0.0, 1.0));
    ImVec2 q(p0.x + (p1.x - p0.x) * i / 96.0f, p1.y - 3 - (p1.y - p0.y - 6) * ny);
    if (i) dl->AddLine(prev, q, c.K.graphCurve, 1.6f * c.F.s);
    prev = q;
  }
  char buf[48];
  if (t60 >= 120)
    std::snprintf(buf, sizeof buf, "STRING %c  RINGS ON", string ? 'B' : 'A');
  else
    std::snprintf(buf, sizeof buf, "STRING %c  RING %.2f s", string ? 'B' : 'A', t60);
  skin::text(dl, ImVec2(p0.x + 5 * c.F.s, p0.y + 3 * c.F.s), c.K.graphText, buf, c.F.font(7.5f));
}

void meterBox(const flk::Ctx& c, const PluginTarget& t, float x, float y, float w, float h) {
  float pk = 0;
  if (const ScopeTap* tap = t.tap(c.a)) {
    static float buf[512];
    if (tap->read(buf, 512))
      for (float v : buf) pk = std::max(pk, std::fabs(v));
  }
  float db = pk > 1e-6f ? 20.0f * std::log10(pk) : -120.0f;
  ImVec2 p0 = c.F.p(x, y), p1 = c.F.p(x + w, y + h);
  skin::graphWell(c.dl, p0, p1, c.K);
  float f = std::clamp((db + 48) / 48, 0.0f, 1.0f);
  c.dl->AddRectFilled(ImVec2(p0.x + 3, p1.y - 10 * c.F.s), ImVec2(p0.x + 3 + (p1.x - p0.x - 6) * f, p1.y - 4 * c.F.s), c.K.fill, 2);
  flk::text(c, x + 5, y + 5, "OUT", c.K.graphText, 7);
}

}  // namespace

void drawTwinString(App& a, const PluginTarget& t, float width) {
  const PluginState* sp = t.state(a.doc.song());
  if (!sp) return;
  const PluginState st = *sp;
  const skin::Skin& K = twinSkin();
  constexpr float kW = 855, kH = 532;
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  flk::Ctx c{a, t, F, K, dl};
  ImGui::PushID("gl.twinstring");
  static int shownString = 0;  // damping view: A / B (editor-only)

  // ---- top strip
  if (!pui::titleWordmark(dl, "gl.twinstring", F.p(16, 20), F.font(14)))  // J-136: the wordmark
    skin::text(dl, F.p(16, 20), K.select, "TWIN STRING", F.font(14));
  {
    char buf[96];
    auto rd = describeParam(a.doc.song(), t.pid(TsS2Ratio));
    std::snprintf(buf, sizeof buf, "STRING %s   COMB %.2f PERIODS", rd ? rd->format(val(st, TsS2Ratio, 3)).c_str() : "B = A",
                  law::tsCombPeriods(val(st, TsCombRatio, 0.5f)));
    flk::lcd(c, 274, 17, 207, 36, buf);
  }
  {
    const flk::Row row(c, 12, 36);  // J-128: one centre line, one label line
    flk::wheel(c, TsTranspose, 574, 18, 28, "Transp", skin::WheelBigLine | skin::WheelCentered);
    flk::wheel(c, TsTune, 634, 18, 28, "Fine", skin::WheelBigLine | skin::WheelCentered);
    flk::wheel(c, TsLevel, 695, 12, 36, "Level");
  }
  meterBox(c, t, 767, 17, 69, 37);

  // ---- exciter column
  flk::section(c, 4, 64, 196, 290, "EXCITER");
  flk::vsl(c, TsNoiseLevel, 18, 100, 27, 150, "Noise");
  flk::wheel(c, TsHiCut, 86, 84, 45, "Hi cut");
  flk::wheel(c, TsHiRes, 86, 150, 45, "Reso");
  flk::wheel(c, TsDrive, 86, 216, 45, "Drive");
  flk::wheel(c, TsHiAtk, 146, 84, 45, "Attack");
  flk::wheel(c, TsHiDec, 146, 150, 45, "Decay");
  flk::wheel(c, TsHiSus, 146, 216, 45, "Sustain");
  {
    const flk::Row row(c, 282, 45);  // J-128: the noise rate on the bottom row's centre line
    flk::wheel(c, TsNoiseRate, 19, 290, 26, "Rate");
    flk::wheel(c, TsLoCut, 86, 282, 45, "Lo cut");
    flk::wheel(c, TsLoRes, 146, 282, 45, "Reso");
  }

  // ---- strings block
  flk::section(c, 206, 64, 342, 290, "STRINGS");
  static const char* inv[2] = {"+", "-"};
  for (int s = 0; s < 2; ++s) {
    const int b = s ? TsS2Decay : TsS1Decay;
    const float y = s ? 296.0f : 84.0f;
    flk::text(c, 214, s ? 286.0f : 74.0f + 10, s ? "B" : "A", K.select, 9);
    {
      const flk::Row row(c, y, 36);  // J-128: shape / offset on the decay wheels' centre line
      flk::wheel(c, b, 224, y, 36, "Decay");
      flk::wheel(c, b + 3, 280, y, 36, "Damping");
      flk::wheel(c, b + 4, 340, y + 6, 28, "Shape");
      flk::wheel(c, b + 2, 392, y + 6, 28, "Offset");
    }
    flk::sw(c, b + 1, 228, s ? 262.0f : 140.0f, 30, 14, inv, 2, true);
  }
  flk::text(c, 226, 202, "B RATIO");
  flk::digi(c, TsS2Ratio, 226, 214, 40, 17);
  {
    // A / B selector for the damping view.
    if (flk::btn(c, "##viewA", 280, 138, 96, 16, "VIEW STRING A", shownString == 0)) shownString = 0;
    if (flk::btn(c, "##viewB", 380, 138, 96, 16, "VIEW STRING B", shownString == 1)) shownString = 1;
    dampingView(c, st, shownString, 280, 158, 196, 94);
  }
  // Comb filter column.
  flk::text(c, 444, 74 + 10, "COMB");
  flk::wheel(c, TsCombLevel, 494, 84, 36, "Level");
  flk::sw(c, TsCombLevelInv, 497, 140, 30, 14, inv, 2, true);
  flk::text(c, 444, 258, "RATIO");
  flk::hsl(c, TsCombRatio, 280, 262, 196, 14);
  flk::wheel(c, TsCombFb, 494, 296, 36, "Fdbk");
  flk::sw(c, TsCombFbInv, 497, 262, 30, 14, inv, 2, true);

  // ---- mix / stereo / saturation + amp envelope
  flk::section(c, 554, 64, 140, 290, "OUTPUT");
  flk::wheel(c, TsMix, 566, 84, 45, "A / B mix");
  flk::vsl(c, TsStereo, 575, 160, 27, 92, "Spread");
  flk::wheel(c, TsSaturation, 566, 282, 45, "Saturate");
  flk::wheel(c, TsAmpA, 626, 84, 45, "Attack");
  flk::wheel(c, TsAmpD, 626, 150, 45, "Decay");
  flk::wheel(c, TsAmpS, 626, 216, 45, "Sustain");
  flk::wheel(c, TsAmpR, 626, 282, 45, "Release");

  // ---- resonators
  flk::section(c, 698, 64, 153, 290, "BODY BANDS");
  for (int k = 0; k < kTsRes; ++k) {
    const float y = 92.0f + 30.0f * k;
    char n[4];
    std::snprintf(n, sizeof n, "%d", k + 1);
    flk::text(c, 704, y + 6, n, K.textDim, 7.5f);
    skin::wheel(a, t, TsResFb1 + k, F.p(712, y), 20 * F.s, K, skin::WheelBigLine);
    flk::hsl(c, TsResFreq1 + k, 738, y + 2, 86, 16);
    flk::chk(c, TsResOn1 + k, 830, y + 2, 16, 16, nullptr);  // J-128: centred on the band's wheel
  }
  flk::text(c, 706, 334, "FDBK      FREQUENCY      ON", K.textDim, 7);

  // ---- modulation row
  flk::section(c, 4, 360, 847, 96, "MODULATION");
  {
  const flk::Row modRow(c, 390, 30);  // J-128: envelope wheels and mod amounts on one centre line
  flk::wheel(c, TsEgA, 16, 392, 28, "A");
  flk::wheel(c, TsEgD, 54, 392, 28, "D");
  flk::wheel(c, TsEgS, 92, 392, 28, "S");
  flk::wheel(c, TsEgR, 130, 392, 28, "R");
  flk::wheel(c, TsLfoRate, 182, 392, 28, "LFO");
  skin::groupCaption(dl, F, 12, 162, 381, K, "FREE ENVELOPE");
  static const char* sign[2] = {"+", "-"};
  for (int k = 0; k < kTsMods; ++k) {
    const float x = 232.0f + 76.0f * k;
    flk::menu(c, tsMod(k, TmSource), x, 372, 70, 13);
    skin::wheel(a, t, tsMod(k, TmAmount), F.p(x + 4, 390), 30 * F.s, K, skin::WheelBigLine);  // (on modRow)
    flk::sw(c, tsMod(k, TmNegative), x + 40, 398, 26, 14, sign, 2, true);
    flk::menu(c, tsMod(k, TmDest), x, 430, 70, 13);
  }
  }

  // ---- effects row
  flk::section(c, 4, 460, 847, 68, "EFFECTS");
  flk::wheel(c, TsDlyTime, 22, 474, 34, "Time");
  flk::digi(c, TsDlySync, 64, 484, 40, 16);
  flk::wheel(c, TsDlyFb, 112, 474, 34, "Fdbk");
  flk::wheel(c, TsDlySpeed, 160, 474, 34, "Mod rate");
  flk::wheel(c, TsDlyDepth, 208, 474, 34, "Mod depth");
  flk::wheel(c, TsDlyBlur, 256, 474, 34, "Blur");
  flk::wheel(c, TsDlyWet, 304, 474, 34, "Wet");
  flk::chk(c, TsDlyOn, 352, 482, 70, 16, "DELAY");
  flk::wheel(c, TsChoDepth, 448, 474, 34, "Depth");
  flk::wheel(c, TsChoRate, 496, 474, 34, "Rate");
  flk::wheel(c, TsChoWet, 544, 474, 34, "Wet");
  flk::chk(c, TsChoOn, 592, 482, 72, 16, "CHORUS");
  flk::wheel(c, TsRevDecay, 690, 474, 34, "Decay");
  flk::wheel(c, TsRevColor, 732, 474, 34, "Tone");
  flk::wheel(c, TsRevMix, 774, 474, 34, "Wet");
  flk::chk(c, TsRevOn, 812, 482, 34, 16, "REV");
  F.end(kW, kH);
  ImGui::PopID();
}

}  // namespace gl::packs::models
