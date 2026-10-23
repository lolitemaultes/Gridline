// Gridline — "synths" pack: Analog editor (J-19) on the reference
// two-oscillator analog synth's 1080 × 574 panel grid and palette: master /
// velocity strip and scope on top; the oscillators with their rail sliders,
// wave buttons and modulation switches, and the mixer column; the filter
// (mode switch, response, rails), filter / amp / mod envelopes and the two
// LFOs; then voice / glide, arpeggiator, modulation matrix, unison and the
// effects block along the bottom. Every control is the shared skin kit's.
#include "../../core/PluginBrand.h"
#include "../flc_kit.h"
#include "synths_widgets.h"

namespace gl::packs::synths {

using namespace ui2;

namespace {

const skin::Skin& analogSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x212527, 0x2A2F33, 0x4A5156, 0xDEDDDA, 0x3C4348, 0xB9BFC3);
    s.handle = skin::hex(0x32383D);
    s.select = skin::hex(0xD98509);
    s.accent2 = skin::hex(0xC9AA44);
    s.lcdBg = skin::hex(0x171A1C);
    s.lcdText = skin::hex(0xE8C77A);
    s.pillBg = skin::hex(0x3A4045);
    s.graphBg = skin::hex(0x171A1C);
    s.graphGrid = skin::hex(0x262B2E);
    s.graphGridStrong = skin::hex(0x343A3E);
    s.graphCurve = skin::hex(0xE8C77A);
    return s;
  }();
  return k;
}

void oscGlyph(ImDrawList* dl, int i, ImVec2 c, float w, float h, ImU32 col) {
  static const int g[5] = {0, 1, 5, 3, 4};
  glyph(dl, g[std::clamp(i, 0, 4)], c, w, h, col);
}
void lfoGlyph(ImDrawList* dl, int i, ImVec2 c, float w, float h, ImU32 col) {
  static const int g[7] = {0, 1, 5, 6, 3, 7, 8};
  glyph(dl, g[std::clamp(i, 0, 6)], c, w, h, col);
}

void oscSection(const flk::Ctx& c, const PluginState& st, int k, float x0) {
  static const char* titles[3] = {"OSC 1", "OSC 2", "OSC 3"};
  static const char* modSrc[3] = {"BY OSC 3", "BY OSC 1", "BY OSC 2"};
  flk::section(c, x0, 52, 300, 184, titles[k]);
  flk::chk(c, anOsc(k, OscOn), x0 + 60, 55, 50, 12, "ON");
  bool on = val(st, anOsc(k, OscOn)) >= 0.5f;
  skin::iconSel(c.a, c.t, anOsc(k, OscWave), c.F.p(x0 + 8, 72), c.F.sz(150, 24), c.K, 5, oscGlyph);
  // Wave preview (pulse width, sync restarts).
  {
    ImVec2 p0 = c.F.p(x0 + 166, 58), p1 = c.F.p(x0 + 292, 120);
    skin::graphWell(c.dl, p0, p1, c.K);
    int wave = ival(st, anOsc(k, OscWave));
    double pw = val(st, anOsc(k, OscPw));
    int mod = ival(st, anOsc(k, OscMod));
    int src = (k + 2) % 3;
    double ratio = std::pow(2.0, (val(st, anOsc(k, OscCoarse)) - val(st, anOsc(src, OscCoarse))) / 12.0);
    auto fn = [&](double ph) {
      double q = mod == ModSync ? std::fmod(ph * std::max(1.0, ratio), 1.0) : ph;
      switch (wave) {
        case WaveSine: return std::sin(dsp::kTwoPi * q);
        case WaveTriangle: return 1.0 - 4.0 * std::fabs(q - 0.5);
        case WaveSaw: return 2.0 * q - 1.0;
        case WavePulse: return q < pw ? 1.0 : -1.0;
        default: return hashUnit(static_cast<uint32_t>(ph * 300)) * 0.8;
      }
    };
    cycleGraph(c.dl, p0, ImVec2(p1.x - p0.x, p1.y - p0.y), fn, on ? c.K.graphCurve : c.K.textDim, 1.4f);
  }
  const char* names[4] = {"COARSE", "FINE", "WIDTH", "DEPTH"};
  const int fields[4] = {OscCoarse, OscFine, OscPw, OscModAmt};
  for (int i = 0; i < 4; ++i) flk::vsl(c, anOsc(k, fields[i]), x0 + 14 + i * 40, 108, 23, 104, names[i]);
  flk::textC(c, x0 + 250, 126, modSrc[k]);
  static const char* mods[4] = {"OFF", "SYNC", "RING", "FM"};
  flk::sw(c, anOsc(k, OscMod), x0 + 214, 140, 72, 76, mods, 4);
}

void envSection(const flk::Ctx& c, const char* title, int A, float x0, float w) {
  flk::section(c, x0, 240, w, 156, title);
  static const char* n[4] = {"A", "D", "S", "R"};
  for (int i = 0; i < 4; ++i) flk::vsl(c, A + i, x0 + 10 + i * 29, 262, 23, 92, n[i]);
}

void lfoSection(const flk::Ctx& c, const PluginState& st, int l, float x0) {
  flk::section(c, x0, 240, 148, 156, l == 0 ? "LFO 1" : "LFO 2");
  skin::iconSel(c.a, c.t, anLfo(l, LfoShapeP), c.F.p(x0 + 8, 258), c.F.sz(132, 20), c.K, 7, lfoGlyph);
  bool sync = val(st, anLfo(l, LfoSync)) >= 0.5f;
  flk::vsl(c, sync ? anLfo(l, LfoDiv) : anLfo(l, LfoRate), x0 + 10, 286, 23, 68, sync ? "DIV" : "RATE");
  flk::vsl(c, anLfo(l, LfoDelay), x0 + 40, 286, 23, 68, "FADE");
  flk::vsl(c, anLfo(l, LfoAmt), x0 + 70, 286, 23, 68, "AMT");
  flk::chk(c, anLfo(l, LfoSync), x0 + 98, 290, 48, 14, "SYNC");
  flk::chk(c, anLfo(l, LfoRetrig), x0 + 98, 310, 48, 14, "RTRG");
  flk::text(c, x0 + 8, 375, "TO");
  flk::menu(c, anLfo(l, LfoDest), x0 + 24, 372, 116, 18);
}

}  // namespace

void drawAnalogEditor(App& a, const PluginTarget& t, float W) {
  const PluginState* sp = t.state(a.doc.song());
  if (!sp) return;
  const PluginState st = *sp;
  const skin::Skin& K = analogSkin();
  constexpr float kW = 1080, kH = 574;
  ImGui::PushID("gl.analog");
  skin::Form F = skin::Form::fit(W, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  flk::Ctx c{a, t, F, K, dl};

  // ---- top strip: name, velocity, master, scope
  skin::text(dl, F.p(12, 14), K.accent2C(), pluginTitle("gl.analog").c_str(), F.font(13));
  flk::text(c, 112, 18, "VELOCITY");
  flk::wheel(c, AnVelAmp, 180, 6, 26, "> Vol");
  flk::wheel(c, AnVelFilter, 222, 6, 26, "> Cut");
  flk::text(c, 520, 18, "MASTER");
  flk::wheel(c, AnTranspose, 576, 6, 26, "Trans", skin::WheelBigLine | skin::WheelCentered);
  flk::wheel(c, AnFine, 620, 6, 26, "Fine", skin::WheelBigLine | skin::WheelCentered);
  flk::wheel(c, AnVolume, 664, 6, 26, "Vol");
  flk::wheel(c, AnPan, 708, 6, 26, "Pan", skin::WheelBigLine | skin::WheelCentered);
  ImGui::SetCursorScreenPos(F.p(770, 4));
  liveScope(a, t, F.sz(306, 42));

  // ---- oscillators + mixer
  for (int k = 0; k < 3; ++k) oscSection(c, st, k, 4 + k * 304);
  flk::section(c, 916, 52, 160, 184, "MIXER");
  static const char* oct[2] = {"-1", "-2"};
  static const char* wv[2] = {"SIN", "SQR"};
  flk::text(c, 924, 73, "SUB");
  flk::sw(c, AnSubOct, 950, 70, 56, 18, oct, 2, true);
  flk::sw(c, AnSubWave, 1012, 70, 58, 18, wv, 2, true);
  const int mixP[5] = {anOsc(0, OscLevel), anOsc(1, OscLevel), anOsc(2, OscLevel), AnSubLevel, AnNoise};
  const char* mixN[5] = {"OSC1", "OSC2", "OSC3", "SUB", "NOISE"};
  for (int i = 0; i < 5; ++i) flk::vsl(c, mixP[i], 924 + i * 30, 104, 23, 108, mixN[i]);

  // ---- filter, envelopes, LFOs
  flk::section(c, 4, 240, 330, 156, "FILTER");
  static const char* modes[8] = {"LP12", "LP24", "BP12", "BP24", "HP12", "HP24", "NOTCH", "PEAK"};
  flk::sw(c, AnFltMode, 10, 258, 46, 132, modes, 8);
  {
    ImVec2 p0 = F.p(62, 258), p1 = F.p(326, 300);
    ImVec2 sz(p1.x - p0.x, p1.y - p0.y);
    ImGui::SetCursorScreenPos(p0);
    ImGui::InvisibleButton("##flt", sz);
    filterDrag(a, t, p0, sz, AnCutoff, AnReso, 1);
    skin::graphWell(dl, p0, p1, K);
    filterCurve(dl, p0, sz, ival(st, AnFltMode), val(st, AnCutoff), val(st, AnReso), K.graphCurve);
  }
  const int fP[6] = {AnCutoff, AnReso, AnFltEnv, AnKeyTrack, AnDrive, AnVelFilter};
  const char* fN[6] = {"CUT", "RES", "ENV", "KEY", "DRIVE", "VEL"};
  for (int i = 0; i < 6; ++i) flk::vsl(c, fP[i], 70 + i * 44, 308, 23, 70, fN[i]);
  envSection(c, "FILTER ENV", AnFenA, 338, 128);
  envSection(c, "AMP ENV", AnAmpA, 470, 128);
  flk::section(c, 602, 240, 170, 156, "MOD ENV");
  {
    static const char* n[4] = {"A", "D", "S", "R"};
    for (int i = 0; i < 4; ++i) flk::vsl(c, AnModA + i, 612 + i * 29, 262, 23, 92, n[i]);
    flk::wheel(c, AnModAmt, 732, 270, 30, "Amt", skin::WheelBigLine | skin::WheelCentered);
    flk::text(c, 610, 375, "TO");
    flk::menu(c, AnModDest, 626, 372, 140, 18);
  }
  lfoSection(c, st, 0, 776);
  lfoSection(c, st, 1, 928);

  // ---- voice, arpeggiator, matrix, unison, effects
  flk::section(c, 4, 400, 140, 170, "VOICE");
  static const char* pm[3] = {"POLY", "MONO", "LEGATO"};
  flk::sw(c, AnPlayMode, 10, 420, 64, 60, pm, 3);
  static const char* gm[2] = {"ALWAYS", "OVERLAP"};
  flk::text(c, 80, 420, "GLIDE");
  flk::sw(c, AnGlideMode, 80, 432, 58, 40, gm, 2);
  flk::wheel(c, AnGlide, 20, 494, 40, "Glide");
  flk::text(c, 82, 488, "VOICES");
  flk::digi(c, AnVoices, 82, 502, 52, 18);

  flk::section(c, 148, 400, 176, 170, "ARPEGGIATOR");
  flk::tog(c, AnArpOn, 156, 420, 40, 20, "ON");
  flk::menu(c, AnArpMode, 200, 420, 118, 20);
  flk::vsl(c, AnArpOct, 168, 450, 23, 90, "OCT");
  flk::vsl(c, AnArpDiv, 222, 450, 23, 90, "RATE");
  flk::vsl(c, AnArpGate, 276, 450, 23, 90, "GATE");

  flk::section(c, 328, 400, 224, 170, "MOD MATRIX");
  flk::text(c, 336, 418, "SOURCE");
  flk::text(c, 430, 418, "TARGET");
  for (int r = 0; r < kAnMatrixRows; ++r) {
    float y = 430 + r * 34;
    ImGui::PushID(r);
    flk::menu(c, anMx(r, 0), 336, y, 88, 20);
    flk::menu(c, anMx(r, 1), 428, y, 88, 20);
    skin::wheel(a, t, anMx(r, 2), F.p(520, y - 3), 26 * F.s, K, skin::WheelBigLine | skin::WheelCentered);
    ImGui::PopID();
  }

  flk::section(c, 556, 400, 80, 170, "UNISON");
  flk::wheel(c, AnUniVoices, 582, 418, 28, "Voices");
  flk::wheel(c, AnUniDetune, 582, 466, 28, "Detune");
  flk::wheel(c, AnUniSpread, 582, 514, 28, "Spread");

  flk::section(c, 640, 400, 436, 170, "EFFECTS");
  flk::wheel(c, AnFxDrive, 652, 440, 40, "Drive");
  struct G {
    const char* name;
    int on, k1, k2, k3;
    const char *l1, *l2, *l3;
  };
  const G groups[3] = {{"CHORUS", FxChoOn, FxChoRate, FxChoDepth, FxChoMix, "RATE", "DEPTH", "MIX"},
                       {"DELAY", FxDlyOn, FxDlyDiv, FxDlyFb, FxDlyMix, "TIME", "FDBK", "MIX"},
                       {"REVERB", FxRevOn, FxRevSize, FxRevDamp, FxRevMix, "SIZE", "DAMP", "MIX"}};
  for (int g = 0; g < 3; ++g) {
    float x0 = 712 + g * 120;
    flk::chk(c, AnFxDrive + groups[g].on, x0, 420, 100, 14, groups[g].name);
    flk::vsl(c, AnFxDrive + groups[g].k1, x0 + 2, 442, 23, 96, groups[g].l1);
    flk::vsl(c, AnFxDrive + groups[g].k2, x0 + 38, 442, 23, 96, groups[g].l2);
    flk::vsl(c, AnFxDrive + groups[g].k3, x0 + 74, 442, 23, 96, groups[g].l3);
  }
  F.end(kW, kH);
  ImGui::PopID();
}

}  // namespace gl::packs::synths
