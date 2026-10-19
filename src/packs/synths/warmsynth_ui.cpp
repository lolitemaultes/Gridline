// Gridline — "synths" pack, J-22: Warm Trio editor (gl.warmsynth) on the
// reference subtractive synth's 562 × 376 form and slate-teal palette: three
// oscillator rows (on, shape selector, width / coarse / fine, level, LFO and
// envelope depth, +1 / +2 / warm buttons, ring), the amp envelope with its
// display, the filter envelope + filter (cut, emphasis, LFO, key, high /
// band outputs, track amp), the LFO (shape, rate, delay, retrigger), chorus
// and a key strip. Drawn with the shared skin kit.
#include <cmath>
#include "../flc_kit.h"
#include "dco.h"
#include "synths_widgets.h"

namespace gl::packs::synths {

using namespace ui2;
using namespace dco;

namespace {

const skin::Skin& warmSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x384449, 0x2C3539, 0x56656B, 0x9FD4C6, 0x46545A, 0xC9D6D9);
    s.select = skin::hex(0x9FD4C6);
    s.accent2 = skin::hex(0xE0B460);
    s.lcdBg = skin::hex(0x1C2326);
    s.lcdText = skin::hex(0x9FD4C6);
    s.graphBg = skin::hex(0x1C2326);
    s.graphGrid = skin::hex(0x2A3336);
    s.graphGridStrong = skin::hex(0x34403F);
    s.graphCurve = skin::hex(0x9FD4C6);
    return s;
  }();
  return k;
}

void shapeGlyph(ImDrawList* dl, int i, ImVec2 c, float w, float h, ImU32 col) {
  static const int g[5] = {3, 2, 1, 4, 0};  // pulse, saw, triangle, noise, sine
  glyph(dl, g[std::clamp(i, 0, 4)], c, w, h, col);
}
void lfoGlyph(ImDrawList* dl, int i, ImVec2 c, float w, float h, ImU32 col) {
  static const int g[4] = {3, 6, 1, 7};  // square, ramp, triangle, random
  glyph(dl, g[std::clamp(i, 0, 3)], c, w, h, col);
}

// Envelope display: attack 8 s·a³, decay 8 s·d², sustain, release 8 s·r².
void envView(const flk::Ctx& c, const PluginState& st, int a, float x, float y, float w, float h) {
  ImVec2 p0 = c.F.p(x, y), p1 = c.F.p(x + w, y + h);
  skin::graphWell(c.dl, p0, p1, c.K);
  const double A = 8 * std::pow(val(st, a), 3), D = 8 * std::pow(val(st, a + 1), 2), S = val(st, a + 2), R = 8 * std::pow(val(st, a + 3), 2);
  const double total = std::max(0.05, A + D + 0.5 + R);
  auto X = [&](double sec) { return p0.x + static_cast<float>(sec / total) * (p1.x - p0.x); };
  auto Y = [&](double lv) { return p1.y - 2 - static_cast<float>(lv) * (p1.y - p0.y - 4); };
  ImVec2 pts[5] = {ImVec2(X(0), Y(0)), ImVec2(X(A), Y(1)), ImVec2(X(A + D), Y(S)), ImVec2(X(A + D + 0.5), Y(S)), ImVec2(X(total), Y(0))};
  c.dl->AddPolyline(pts, 5, c.K.graphCurve, 0, 1.5f);
}

}  // namespace

void drawWarmSynthEditor(App& a, const PluginTarget& t, float W) {
  const PluginState* sp = t.state(a.doc.song());
  if (!sp) return;
  const PluginState st = *sp;
  const skin::Skin& K = warmSkin();
  constexpr float kW = 562, kH = 376;
  ImGui::PushID("gl.warmsynth");
  skin::Form F = skin::Form::fit(W, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  flk::Ctx c{a, t, F, K, dl};

  // ---- oscillators (rows at y = 6 / 103 / 200)
  for (int k = 0; k < 3; ++k) {
    const float y0 = 97.f * k;
    ImGui::PushID(k);
    char name[8];
    std::snprintf(name, sizeof name, "OSC %d", k + 1);
    flk::text(c, 8, y0 + 12, name, K.text, 9.0f);
    flk::chk(c, WmOsc1On + k, 58, y0 + 6, 21, 16, "");
    skin::iconSel(a, t, WmOsc1Shape + k, F.p(94, y0 + 7), F.sz(161, 26), K, 5, shapeGlyph);
    const int b = 3 * k;
    flk::wheel(c, WmOsc1Pw + b, 8, y0 + 46, 34, "Width");
    flk::wheel(c, WmOsc1Coarse + b, 51, y0 + 46, 34, "Coarse", skin::WheelBigLine | skin::WheelCentered);
    flk::wheel(c, WmOsc1Fine + b, 94, y0 + 46, 34, "Fine", skin::WheelBigLine | skin::WheelCentered);
    flk::wheel(c, WmOsc1Level + b, 137, y0 + 46, 34, "Level");
    flk::wheel(c, WmOsc1Lfo + b, 180, y0 + 46, 34, "LFO", skin::WheelBigLine | skin::WheelCentered);
    flk::wheel(c, WmOsc1Env + b, 223, y0 + 46, 34, "Env", skin::WheelBigLine | skin::WheelCentered);
    flk::chk(c, WmOsc1Oct1 + b, 267, y0 + 45, 26, 16, "1");
    flk::chk(c, WmOsc1Oct2 + b, 293, y0 + 45, 26, 16, "2");
    flk::chk(c, WmOsc1Warm + b, 267, y0 + 66, 52, 16, "WARM");
    ImGui::PopID();
  }
  flk::chk(c, WmRing, 267, 11, 68, 16, "RING 1x2");

  // ---- amp envelope (top right)
  flk::text(c, 347, 14, "AMP", K.text, 9.0f);
  envView(c, st, WmAmpA, 433, 7, 121, 34);
  const char* adsr[4] = {"Att", "Dec", "Sus", "Rel"};
  for (int i = 0; i < 4; ++i) flk::wheel(c, WmAmpA + i, 347 + 43.f * i, 46, 34, adsr[i]);
  flk::wheel(c, WmVcaVel, 519, 46, 34, "Velo");

  // ---- filter envelope + filter
  flk::text(c, 347, 111, "FILTER", K.text, 9.0f);
  flk::chk(c, WmSvfOn, 380, 103, 21, 16, "");
  envView(c, st, WmSvfA, 433, 104, 121, 34);
  for (int i = 0; i < 4; ++i) flk::wheel(c, WmSvfA + i, 347 + 43.f * i, 145, 34, adsr[i]);
  flk::wheel(c, WmSvfEnv, 519, 145, 34, "Env", skin::WheelBigLine | skin::WheelCentered);
  flk::wheel(c, WmSvfLfo, 347, 195, 34, "LFO", skin::WheelBigLine | skin::WheelCentered);
  flk::wheel(c, WmSvfKb, 390, 194, 34, "Key");
  flk::chk(c, WmTrackAmp, 479, 195, 75, 16, "TRACK AMP");
  flk::wheel(c, WmCut, 347, 243, 34, "Cut");
  flk::wheel(c, WmEmph, 390, 243, 34, "Emph");
  flk::wheel(c, WmSvfHigh, 433, 243, 34, "High");
  flk::wheel(c, WmSvfBand, 476, 243, 34, "Band");

  // ---- LFO
  flk::text(c, 347, 303, "LFO", K.text, 9.0f);
  flk::chk(c, WmLfoOn, 380, 297, 21, 16, "");
  skin::iconSel(a, t, WmLfoShape, F.p(436, 299), F.sz(108, 26), K, 4, lfoGlyph);
  flk::wheel(c, WmLfoRate, 347, 323, 34, "Rate");
  flk::wheel(c, WmLfoDelay, 390, 323, 34, "Delay");
  flk::chk(c, WmRetrig, 438, 340, 76, 16, "RETRIGGER");
  flk::chk(c, WmChorus, 518, 340, 40, 16, "CHO");

  // ---- key strip + name
  skin::keyboard("##warmkb", F.p(4, 300), F.sz(315, 72), 36, 84, K);
  skin::text(dl, F.p(440, 360), K.accent2C(), "WARM TRIO", F.font(10));
  F.end(kW, kH);
  ImGui::PopID();
}

}  // namespace gl::packs::synths
