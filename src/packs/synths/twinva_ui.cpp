// Gridline — "synths" pack, J-22: Twin VA editor on the reference two-
// oscillator VA's 855 × 521 grid and palette (near-black panel, yellow value
// arcs, light rails, slate selector wells, blue digit wheels): voice row
// (unison, polyphony, unison detune / spread, octaver, mono, glide modes,
// glide, transpose, fine, gain); modulation row (wheel target + amount + bend
// range, LFO target / amount / rate / shape / retrigger / sync, envelope
// target / amount / ADSR, amplifier ADSR); oscillator + filter + arpeggiator
// row; effects + trance-gate row; wheels and the audition keyboard.
#include <cstdio>
#include <string>
#include "../../app/App.h"
#include "../../app/InstrumentSkin.h"
#include "../../engine/Dsp.h"
#include "va.h"
#include "va_ui.h"

namespace gl::packs::synths::va {

namespace {

using iskin::rgb;
constexpr float kW = 855, kH = 521;

iskin::Palette twinPalette() {
  iskin::Palette p;
  p.bgTop = rgb(0x262826);
  p.bgBottom = rgb(0x1D1F1D);
  p.panel = rgb(0x272C2E);
  p.panelLine = rgb(0x111312);
  p.rail = rgb(0x5E605E);
  p.fill = rgb(0xE1CD28);
  p.fill2 = rgb(0xE0E0E0);
  p.fill3 = rgb(0xE17A28);
  p.cap = rgb(0x3A3C3A);
  p.handle = rgb(0x212121);
  p.label = rgb(0x9A9D9A);
  p.text = rgb(0xE6E6E0);
  p.textDim = rgb(0x8C8F8C);
  p.lcdBg = rgb(0x4D5CD8);
  p.lcdText = rgb(0xF2F4FF);
  p.led = rgb(0xE1CD28);
  p.ledOff = rgb(0x3A3C3A);
  return p;
}

}  // namespace

void drawTwinVaEditor(App& a, const PluginTarget& t, float width) {
  const PluginState* sp = t.state(a.doc.song());
  if (!sp) return;
  const PluginState st = *sp;
  iskin::Skin k = iskin::begin(width, kW, kH, twinPalette());
  ImGui::PushID("gl.twinva");
  iskin::background(k, 0, 0, kW, kH, k.c.bgTop, k.c.bgBottom, 4);
  const ImU32 Y = k.c.fill;
  auto W = [&](int p, float x, float y, const char* cap, bool centred = false) { wheelCap(a, t, k, p, x, y, 40, Y, cap, centred); };
  auto S = [&](int p, float x, float y, const char* cap, bool centred = false) { sliderCap(a, t, k, p, x, y, 25, 72, Y, cap, centred); };
  auto B = [&](int p, float x, float y, const char* cap) { toggleCap(a, t, k, p, x, y, 31, 31, cap); };

  // Title strip.
  if (!pui::titleWordmarkIn(k.dl(), "gl.twinva", k.at(16, 14), k.at(16 + 200, 14 + 22), k.px(16), 0, k.c.text))  // J-136: the wordmark
    iskin::text(k, 16, 14, 200, 22, "TWIN VA", 16, k.c.fill, 0, false, true);
  iskin::bevel(k, 240, 14, 240, 22, k.c.panel, k.c.panelLine, 3);
  {
    std::string name = dataLine(st.data, "name");
    iskin::text(k, 248, 14, 230, 22, name.empty() ? "two-oscillator VA" : name.c_str(), 11, k.c.textDim);
  }
  liveScopeAt(a, t, k, 500, 12, 340, 26);

  // ---- voice row (y 64..130)
  panel(k, 8, 60, 839, 74, "VOICE");
  digiCap(a, t, k, TwUnison, 288, 91, 35, 17, "UNISON", [](double v) { return std::to_string(static_cast<int>(v) + 1); });
  digiCap(a, t, k, TwPoly, 343, 91, 35, 17, "POLY", [](double v) { return std::to_string(static_cast<int>(v) + 1); });
  W(TwUniDetune, 395, 78, "DETUNE");
  W(TwUniPan, 442, 78, "SPREAD");
  B(TwOctaver, 493, 88, "OCT");
  B(TwMono, 528, 88, "MONO");
  B(TwGlideHeld, 563, 88, "HELD");
  B(TwGlideStatic, 599, 88, "STATIC");
  W(TwGlide, 643, 78, "GLIDE");
  W(TwTranspose, 702, 78, "TRANS", true);
  W(TwFine, 745, 78, "FINE", true);
  W(TwGain, 800, 78, "GAIN");
  iskin::text(k, 20, 80, 250, 14, "unison voices / polyphony / glide", 9, k.c.textDim);

  // ---- modulation row (y 140..250)
  panel(k, 8, 140, 130, 110, "WHEEL");
  static const char* dest5[5] = {"CUT", "FRQ A", "FRQ AB", "PW A", "PW AB"};
  static const char* wheel5[5] = {"CUT", "LFO", "FRQ A", "PW A", "PW AB"};
  static const char* dest6[6] = {"CUT", "FRQ A", "FRQ AB", "PW A", "PW AB", "PAN"};
  selCap(a, t, k, TwMwDest, 22, 169, 45, 60, wheel5, 5);
  W(TwMwAmt, 88, 156, "AMOUNT", true);
  digiCap(a, t, k, TwBendRange, 91, 212, 35, 17, "", [](double v) { return "PB " + std::to_string(static_cast<int>(v) + 1); });
  panel(k, 144, 140, 262, 110, "LFO");
  selCap(a, t, k, TwLfoDest, 152, 157, 45, 72, dest6, 6);
  S(TwLfoAmt, 215, 158, "AMT", true);
  S(TwLfoRate, 257, 158, "RATE");
  static const char* shapes[5] = {"SAW", "PULSE", "TRI", "SINE", "RAND"};
  selCap(a, t, k, TwLfoShape, 296, 169, 45, 60, shapes, 5);
  B(TwLfoRetrig, 365, 163, "RETRIG");
  digiCap(a, t, k, TwLfoSync, 363, 212, 35, 17, "", [](double v) {
    static const char* n[11] = {"FREE", "1/16", "1/8", "3/16", "1/4", "3/8", "1/2", "3/4", "1/1", "2/1", "4/1"};
    return std::string(n[std::clamp(static_cast<int>(v), 0, 10)]);
  });
  panel(k, 412, 140, 258, 110, "ENVELOPE");
  selCap(a, t, k, TwEgDest, 420, 169, 45, 60, dest5, 5);
  S(TwEgAmt, 483, 158, "AMT", true);
  S(TwEgA, 526, 158, "A");
  S(TwEgD, 561, 158, "D");
  S(TwEgS, 597, 158, "S");
  S(TwEgR, 633, 158, "R");
  panel(k, 676, 140, 171, 110, "AMPLIFIER");
  S(TwAmpA, 694, 158, "A");
  S(TwAmpD, 730, 158, "D");
  S(TwAmpS, 766, 158, "S");
  S(TwAmpR, 802, 158, "R");

  // ---- oscillators / filter / arpeggiator (y 256..330)
  panel(k, 8, 256, 450, 76, "OSCILLATORS");
  W(TwBalance, 19, 271, "A / B");
  W(TwNoise, 67, 271, "NOISE");
  W(TwAPw, 124, 271, "PW A");
  shapeBtn(a, t, k, TwAShape, 169, 279, "A");
  B(TwRing, 206, 279, "RING");
  B(TwSync, 243, 279, "SYNC");
  W(TwBPw, 293, 271, "PW B");
  shapeBtn(a, t, k, TwBShape, 335, 279, "B");
  W(TwBPitch, 370, 271, "PITCH", true);
  W(TwBDetune, 413, 271, "DETUNE");
  panel(k, 464, 256, 230, 76, "FILTER");
  W(TwCutoff, 477, 271, "CUT");
  W(TwReso, 521, 271, "RES");
  W(TwKeyTrk, 565, 271, "KEY");
  W(TwVelTrk, 610, 271, "VEL");
  static const char* modes[3] = {"LP", "BP", "HP"};
  selCap(a, t, k, TwFltMode, 656, 269, 32, 36, modes, 3);
  panel(k, 700, 256, 147, 76, "");
  iskin::text(k, 793, 257, 50, 12, "ARP", 8, k.c.textDim, 0, false, true);
  static const char* arp[5] = {"OFF", "UP", "DOWN", "U+D", "RND"};
  selCap(a, t, k, TwArpMode, 702, 261, 46, 60, arp, 5);
  static const char* rng[4] = {"1", "2", "3", "4"};
  selCap(a, t, k, TwArpRange, 757, 273, 32, 48, rng, 4);
  digiCap(a, t, k, TwArpSync, 800, 272, 35, 17, "STEP", [](double v) {
    static const char* n[7] = {"1/16", "1/8", "1/4", "1/32", "3/16", "3/8", "1/2"};
    return std::string(n[std::clamp(static_cast<int>(v), 0, 6)]);
  });

  // ---- effects / gate (y 338..400)
  panel(k, 8, 338, 362, 64, "DELAY");
  B(TwDlyOn, 16, 356, "ON");
  const int dp[7] = {TwDlyL, TwDlyR, TwDlyFb, TwDlyDepth, TwDlyLoCut, TwDlyHiCut, TwDlyWet};
  const char* dn[7] = {"LEFT", "RIGHT", "FDBK", "DEPTH", "LO-CUT", "HI-CUT", "WET"};
  for (int i = 0; i < 7; ++i) wheelCap(a, t, k, dp[i], 56.f + 45 * i, 346, 32, Y, dn[i]);
  panel(k, 376, 338, 178, 64, "CHORUS");
  B(TwChoOn, 382, 356, "ON");
  const int cp[3] = {TwChoDepth, TwChoRate, TwChoWet};
  const char* cn[3] = {"DEPTH", "RATE", "WET"};
  for (int i = 0; i < 3; ++i) wheelCap(a, t, k, cp[i], 422.f + 45 * i, 346, 32, Y, cn[i]);
  panel(k, 560, 338, 287, 64, "TRANCE GATE");
  wheelCap(a, t, k, TwGateSmooth, 569, 346, 32, Y, "SMOOTH");
  wheelCap(a, t, k, TwGateMix, 613, 346, 32, Y, "MIX");
  digiCap(a, t, k, TwGateSync, 659, 362, 44, 17, "", [](double v) {
    static const char* n[7] = {"1/16", "1/8", "1/4", "1/32", "3/16", "3/8", "1/2"};
    return std::string(n[std::clamp(static_cast<int>(v), 0, 6)]);
  });
  for (int s = 0; s < kTwGateSteps; ++s) stepBox(a, t, k, TwGate1 + s, 713.f + 16 * (s % 8), 346.f + 23 * (s / 8), 13, 20);

  // ---- wheels + keyboard (y 406..)
  iskin::bevel(k, 8, 404, 839, 110, k.c.panel, k.c.panelLine, 3);
  iskin::text(k, 30, 494, 60, 12, "BEND", 8, k.c.textDim, 1);
  iskin::bevel(k, 47, 412, 19, 78, rgb(0x141514), k.c.panelLine, 3);
  iskin::slider(a, t, k, TwModWheel, 97, 412, 19, 78, k.c.fill2, false);
  iskin::text(k, 77, 494, 60, 12, "MOD", 8, k.c.textDim, 1);
  iskin::KeyColors kc;
  kc.pressed = k.c.fill;
  iskin::keyboard(a, t, k, 165, 411, 666, 100, 36, 42, kc);
  iskin::end(k, kH);
  ImGui::PopID();
}

}  // namespace gl::packs::synths::va
