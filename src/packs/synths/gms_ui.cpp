// Gridline — "synths" pack: Groove Synth editor (960 × 520 form px).
// The reference instrument draws its own panel (no form tree), so the layout
// follows its control grouping: SYNTH page — three oscillator columns with the
// modulation / noise / unison blocks on the top row, filter, amp envelope and
// master in the middle, the two envelopes, two LFOs and velocity routing at
// the bottom; FX page — the ten fixed effect slots side by side (switch, X / Y,
// LFO division / shape / ×4 / amount) and the master EQ.
#include <cstdlib>
#include <filesystem>
#include <map>
#include "../flc_kit.h"
#include "gms.h"

namespace gl::packs::synths {

namespace {

using namespace gms;
constexpr float kW = 960, kH = 520, kTop = 20;

const skin::Skin& gmsSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x2E3236, 0x24272A, 0x41464B, 0xE8B33C, 0x8A9096, 0xD5DADF);
    s.handle = skin::hex(0x5A6066);
    s.select = skin::hex(0xE8B33C);
    s.accent2 = skin::hex(0x4FB8E8);
    s.lcdBg = skin::hex(0x16181A);
    s.lcdText = skin::hex(0xE8B33C);
    s.pillBg = skin::hex(0x1C1F22);
    s.graphBg = skin::hex(0x16181A);
    s.graphGrid = skin::hex(0x25292C);
    s.graphCurve = skin::hex(0xE8B33C);
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
  if (const char* e = std::getenv("GL_GMS_TAB")) p = std::clamp(std::atoi(e), 0, 1);  // dev: screenshots
  return m[t.key()] = p;
}

void synthPage(App& a, const PluginTarget& t, const PluginState& st, const flk::Ctx& c, const flk::Ctx& cB, const flk::Ctx& cG,
               const flk::Ctx& cR) {
  const float o = kTop;
  const WaveFiles wf = parseWaveFiles(st.data);
  // ---- oscillators
  for (int k = 0; k < kOscs; ++k) {
    const float x = 4 + k * 150.0f;
    char title[16];
    std::snprintf(title, sizeof title, "OSC %d", k + 1);
    flk::section(c, x, o + 4, 146, 166, title);
    flk::menu(c, osc(k, OsWave), x + 8, o + 22, 130, 20);
    const int wave = static_cast<int>(std::lround(k < kOscs ? st.params[static_cast<size_t>(osc(k, OsWave))] : 0));
    std::string file = wave == WvFile ? (wf.path[k].empty() ? "(no file)" : std::filesystem::path(wf.path[k]).stem().string()) : "";
    if (!file.empty()) flk::lcd(c, x + 8, o + 46, 130, 16, file.c_str());
    flk::digi(c, osc(k, OsSemi), x + 12, o + 74, 36, 20);
    flk::textC(c, x + 30, o + 98, "SEMI");
    flk::wheel(c, osc(k, OsCents), x + 58, o + 70, 30, "Cents", skin::WheelBigLine | skin::WheelCentered);
    if (k > 0) flk::wheel(cB, k == 1 ? GOsc2Mix : GOsc3Mix, x + 98, o + 68, 34, "Mix");
    if (k == 1) {
      flk::wheel(c, GOsc2Phase, x + 12, o + 118, 28, "Phase");
      flk::chk(c, GOsc2Retrig, x + 54, o + 120, 80, 13, "RESET");
      flk::chk(c, GOsc2Invert, x + 54, o + 138, 80, 13, "INVERT");
    }
    if (k == 0) {
      flk::text(c, x + 12, o + 120, "OSC 2 MODULATES 1:");
      flk::menu(c, GModKind, x + 12, o + 134, 70, 20);
      flk::wheel(cR, GModAmt, x + 96, o + 120, 34, "Amount");
    }
    if (k == 2) flk::wheel(c, GNoise, x + 12, o + 118, 30, "Noise");
  }
  // ---- unison
  flk::section(c, 454, o + 4, 200, 166, "UNISON");
  flk::digi(cG, GUniVoices, 466, o + 30, 30, 22);
  flk::textC(c, 481, o + 56, "VOICES");
  flk::wheel(cG, GUniDetune, 510, o + 26, 36, "Detune");
  flk::wheel(cG, GUniStereo, 560, o + 26, 36, "Stereo");
  flk::wheel(cG, GUniPhase, 608, o + 26, 36, "Phase");
  flk::chk(c, GUniRetrig, 466, o + 110, 120, 13, "PHASE RESET");
  // ---- master
  flk::section(c, 658, o + 4, 298, 166, "MASTER");
  flk::wheel(c, GVolume, 670, o + 24, 42, "Volume");
  flk::wheel(c, GLevel, 724, o + 28, 34, "Level", skin::WheelBigLine | skin::WheelCentered);
  flk::wheel(c, GPan, 770, o + 28, 34, "Pan", skin::WheelBigLine | skin::WheelCentered);
  flk::wheel(c, GPitch, 816, o + 28, 34, "Pitch", skin::WheelBigLine | skin::WheelCentered);
  flk::digi(c, GOctave, 866, o + 34, 30, 20);
  flk::textC(c, 881, o + 58, "OCT");
  flk::digi(c, GBend, 910, o + 34, 30, 20);
  flk::textC(c, 925, o + 58, "BEND");
  flk::chk(c, GMono, 670, o + 104, 70, 13, "MONO");
  flk::chk(c, GSlide, 670, o + 122, 70, 13, "SLIDE");
  flk::wheel(c, GSlideTime, 748, o + 104, 30, "Slide");
  flk::chk(c, GVelVol, 800, o + 112, 140, 13, "VELOCITY > LEVEL");

  // ---- filter
  flk::section(c, 4, o + 174, 330, 166, "FILTER");
  static const char* kTypes[3] = {"LP", "BP", "HP"};
  flk::sw(c, GFltType, 16, o + 196, 30, 66, kTypes, 3);
  flk::wheel(cB, GCutoff, 64, o + 196, 60, "Cutoff");
  flk::wheel(cB, GReso, 140, o + 204, 44, "Reso");
  flk::wheel(cB, GKey, 200, o + 210, 34, "Key");
  flk::text(c, 16, o + 296, "LADDER 4-POLE  ·  2X OVERSAMPLED");
  // ---- amp envelope
  flk::section(c, 338, o + 174, 618, 166, "AMP ENVELOPE");
  skin::EnvParams ep;
  ep.att = GAmpA, ep.dec = GAmpD, ep.sus = GAmpS, ep.rel = GAmpR;
  skin::envelope(a, t, ep, c.F.p(350, o + 196), c.F.sz(300, 130), c.K);
  const int ap[4] = {GAmpA, GAmpD, GAmpS, GAmpR};
  const char* an[4] = {"Attack", "Decay", "Sustain", "Release"};
  for (int i = 0; i < 4; ++i) flk::wheel(c, ap[i], 672 + i * 70.0f, o + 220, 44, an[i]);

  // ---- envelopes, LFOs, velocity
  for (int k = 0; k < 2; ++k) {
    const float x = 4 + k * 190.0f;
    char title[16];
    std::snprintf(title, sizeof title, "ENV %d", k + 1);
    flk::section(c, x, o + 344, 186, 172, title);
    flk::wheel(cG, eg(k, EgAmt), x + 12, o + 364, 40, "Amount", skin::WheelBigLine | skin::WheelCentered);
    flk::wheel(cG, eg(k, EgAtt), x + 66, o + 368, 34, "Attack");
    flk::wheel(cG, eg(k, EgDec), x + 116, o + 368, 34, "Decay");
    flk::text(c, x + 12, o + 438, "TARGET");
    flk::menu(c, eg(k, EgDest), x + 12, o + 452, 160, 20);
  }
  for (int k = 0; k < 2; ++k) {
    const float x = 384 + k * 236.0f;
    char title[16];
    std::snprintf(title, sizeof title, "LFO %d", k + 1);
    flk::section(c, x, o + 344, 232, 172, title);
    flk::wheel(cR, lfo(k, LfAmt), x + 12, o + 364, 40, "Amount", skin::WheelBigLine | skin::WheelCentered);
    flk::wheel(cR, lfo(k, LfRate), x + 66, o + 368, 34, "Rate");
    flk::menu(c, lfo(k, LfShape), x + 116, o + 364, 104, 20);
    flk::menu(c, lfo(k, LfSync), x + 116, o + 390, 104, 20);
    flk::menu(c, lfo(k, LfDiv), x + 116, o + 416, 104, 20);
    flk::chk(c, lfo(k, LfRetrig), x + 12, o + 424, 90, 13, "RETRIGGER");
    flk::text(c, x + 12, o + 446, "TARGET");
    flk::menu(c, lfo(k, LfDest), x + 12, o + 460, 208, 20);
  }
  flk::section(c, 856, o + 344, 100, 172, "VELOCITY");
  flk::wheel(c, GVelAmt, 882, o + 364, 44, "Amount", skin::WheelBigLine | skin::WheelCentered);
  flk::menu(c, GVelDest, 864, o + 440, 84, 20);
}

void fxPage(const flk::Ctx& c, const flk::Ctx& cB, const flk::Ctx& cR) {
  const float o = kTop;
  for (int k = 0; k < kFxSlots; ++k) {
    const float x = 4 + k * 95.2f;
    flk::section(c, x, o + 4, 91, 360, kFxNames[k]);
    flk::chk(c, fx(k, FfOn), x + 10, o + 26, 70, 14, "ON");
    flk::wheel(cB, fx(k, FfX), x + 20, o + 52, 50, kFxX[k]);
    flk::wheel(cB, fx(k, FfY), x + 24, o + 128, 42, kFxY[k]);
    flk::text(c, x + 8, o + 196, "LFO");
    flk::wheel(cR, fx(k, FfLfo), x + 28, o + 210, 34, "Amount", skin::WheelBigLine | skin::WheelCentered);
    flk::menu(c, fx(k, FfDiv), x + 8, o + 270, 75, 20);
    flk::menu(c, fx(k, FfShape), x + 8, o + 296, 75, 20);
    flk::chk(c, fx(k, FfX4), x + 10, o + 326, 70, 13, "X4");
  }
  flk::section(c, 4, o + 370, 952, 146, "MASTER EQ");
  flk::chk(c, GEqOn, 16, o + 394, 60, 14, "ON");
  static const char* kBand[kEqBands] = {"80", "220", "1.2K", "6K", "12K"};
  for (int b = 0; b < kEqBands; ++b) flk::vsl(cB, GEq1 + b, 120 + b * 60.0f, o + 390, 19, 96, kBand[b]);
  flk::text(c, 460, o + 400, "SHELF / PEAK / PEAK / PEAK / SHELF  ·  +-12 DB");
}

}  // namespace

void drawGmsEditor(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  const skin::Skin& K = gmsSkin();
  static const skin::Skin kBlue = tinted(K, 0x4FB8E8), kGreen = tinted(K, 0x7DCB5A), kRed = tinted(K, 0xE5684D);
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  flk::Ctx c{a, t, F, K, dl}, cB{a, t, F, kBlue, dl}, cG{a, t, F, kGreen, dl}, cR{a, t, F, kRed, dl};
  ImGui::PushID("gl.gms");
  int& page = pageOf(t);
  static const char* kPages[2] = {"SYNTH", "FX"};
  for (int i = 0; i < 2; ++i)
    if (flk::btn(c, kPages[i], 6 + i * 74.0f, 3, 70, 15, kPages[i], page == i)) page = i;
  if (page == 0)
    synthPage(a, t, st, c, cB, cG, cR);
  else
    fxPage(c, cB, cR);
  F.end(kW, kH);
  ImGui::PopID();
}

}  // namespace gl::packs::synths
