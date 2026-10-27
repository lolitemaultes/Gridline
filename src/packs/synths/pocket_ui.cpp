// Gridline — "synths" pack, J-33 group "pocket": Pocket Synth editor
// (gl.pocketsynth): voice, oscillator, filter + envelope, LFO, amp envelope and
// the effect strip, with physical readouts of the reference laws.
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include "../flc_kit.h"
#include "pocket.h"
#include "synths_widgets.h"

namespace gl::packs::synths {

using namespace ui2;
using namespace pocket;

namespace {

const skin::Skin& pocketSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x2E2B33, 0x1E1C22, 0x3E3A45, 0xF29A3C, 0x4A4552, 0xE8E2EE);
    s.select = skin::hex(0xF29A3C);
    s.accent2 = skin::hex(0xF29A3C);
    s.lcdBg = skin::hex(0x16141A);
    s.lcdText = skin::hex(0xF29A3C);
    return s;
  }();
  return k;
}

std::string readout(int p, double x) {
  char b[48];
  static const char* kinds[10] = {"Saw", "Saw + saw", "Pulse", "Saw + square", "Square + square", "Supersaw", "Bell",
                                  "Hollow saw", "Spread octave", "Spread fifth"};
  static const char* rates[12] = {"1/4", "3/8", "1/2", "3/4", "1 beat", "2 beats", "1 bar", "2 bars", "4 bars", "16 bars", "32 bars", "64 bars"};
  auto coef = [&](double c) {  // per-sample one-pole → time constant
    const double t = 1.0 / std::max(1e-12, c) / 44100.0;
    if (t < 1) std::snprintf(b, sizeof b, "%.0f ms", t * 1000);
    else std::snprintf(b, sizeof b, "%.2f s", t);
  };
  auto f4 = [](double v) { const double f = 0.95 * v + 0.05; return f * f * f * f; };
  switch (p) {
    case PkVoiceMode: return std::array<const char*, 3>{"Poly", "Mono", "Lead"}[std::min(2, static_cast<int>(2 * x))];
    case PkTranspose: std::snprintf(b, sizeof b, "%+d st", static_cast<int>(24 * x - 12)); return b;
    case PkOscKind: return kinds[std::min(9, static_cast<int>(9 * x))];
    case PkLfoShape: return std::array<const char*, 3>{"Sine", "Saw", "Square"}[std::min(2, static_cast<int>(2 * x))];
    case PkLfoDest: return std::array<const char*, 3>{"Cutoff", "Pitch", "Osc mod"}[std::min(2, static_cast<int>(2 * x))];
    case PkWheelDest: return x >= 1 ? "LFO" : "Cutoff";
    case PkLfoRate:
    case PkPhaserRate: return rates[std::min(11, static_cast<int>(11 * x))];
    case PkDelayTime: return std::array<const char*, 4>{"1/4", "2/4", "3/4", "4/4"}[std::min(3, static_cast<int>(3 * x))];
    case PkFltAmount:
    case PkLfoAmount: std::snprintf(b, sizeof b, "%+.0f%%", 200 * x - 100); return b;
    case PkCutoff: std::snprintf(b, sizeof b, "%.0f Hz", std::exp2(4 + 10.3 * x)); return b;
    case PkAmpA:
    case PkFltA:
      if (x < 0.001) return "0 ms";
      coef(4e-7 / f4(x));
      return b;
    case PkAmpD:
    case PkFltD:
    case PkFltR:
      if (x < 0.001) return "0 ms";
      coef(4e-6 / f4(x));
      return b;
    case PkAmpR: {
      const double f = 0.95 * x + 0.05, f2 = 0.95 * f + 0.05;
      coef(4e-5 / (f2 * f2 * f2 * f2));
      return b;
    }
    default: std::snprintf(b, sizeof b, "%.0f%%", 100 * x); return b;
  }
}

void wh(const flk::Ctx& c, const PluginState& st, int p, float x, float y, float size, const char* label, bool centred = false) {
  skin::wheel(c.a, c.t, p, c.F.p(x, y), size * c.F.s, c.K, skin::WheelBigLine | skin::WheelNoValue | (centred ? skin::WheelCentered : 0));
  const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
  const std::string s = hot ? readout(p, val(st, p)) : flk::upper(label);
  skin::label(c.dl, c.F.p(x + size * 0.5f, 0).x, c.F.p(0, y + size + 1.5f).y, s.c_str(), hot ? c.K.text : c.K.textDim, c.F.font(7.0f));
}

}  // namespace

void drawPocketSynthEditor(App& a, const PluginTarget& t, float W) {
  const PluginState* sp = t.state(a.doc.song());
  if (!sp) return;
  const PluginState st = *sp;
  const skin::Skin& K = pocketSkin();
  constexpr float kW = 760, kH = 300;
  ImGui::PushID("gl.pocketsynth");
  skin::Form F = skin::Form::fit(W, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  flk::Ctx c{a, t, F, K, dl};

  if (!pui::titleWordmark(dl, "gl.pocketsynth", F.p(12, 10), F.font(14)))  // J-136: the wordmark

    skin::text(dl, F.p(12, 10), K.accent2C(), "POCKET SYNTH", F.font(14));
  flk::lcd(c, 130, 8, 140, 16, readout(PkOscKind, val(st, PkOscKind)).c_str());

  flk::section(c, 4, 30, 150, 124, "Voice");
  wh(c, st, PkVoiceMode, 12, 50, 30, "Mode");
  wh(c, st, PkTranspose, 58, 50, 30, "Transp", true);
  wh(c, st, PkSlide, 104, 50, 30, "Slide");
  wh(c, st, PkWheelDest, 12, 104, 30, "Wheel to");
  wh(c, st, PkWheelAmount, 58, 104, 30, "Wheel");

  flk::section(c, 158, 30, 150, 124, "Oscillator");
  wh(c, st, PkOscKind, 166, 50, 30, "Kind");
  wh(c, st, PkOscMod, 212, 50, 30, "Mod");
  wh(c, st, PkNoise, 258, 50, 30, "Noise");

  flk::section(c, 312, 30, 280, 124, "Filter");
  wh(c, st, PkCutoff, 320, 50, 30, "Cutoff");
  wh(c, st, PkPeak, 364, 50, 30, "Peak");
  wh(c, st, PkKeyTrack, 408, 50, 30, "Key");
  wh(c, st, PkVelTrack, 452, 50, 30, "Vel");
  wh(c, st, PkFltAmount, 496, 50, 30, "Env", true);
  wh(c, st, PkFltA, 320, 104, 30, "Atk");
  wh(c, st, PkFltD, 364, 104, 30, "Dec");
  wh(c, st, PkFltS, 408, 104, 30, "Sus");
  wh(c, st, PkFltR, 452, 104, 30, "Rel");

  flk::section(c, 596, 30, 160, 124, "LFO");
  wh(c, st, PkLfoShape, 604, 50, 30, "Shape");
  wh(c, st, PkLfoDest, 650, 50, 30, "Target");
  wh(c, st, PkLfoRate, 696, 50, 30, "Rate");
  wh(c, st, PkLfoAmount, 604, 104, 30, "Amount", true);

  flk::section(c, 4, 158, 196, 138, "Amp");
  wh(c, st, PkAmpA, 12, 180, 30, "Atk");
  wh(c, st, PkAmpD, 58, 180, 30, "Dec");
  wh(c, st, PkAmpS, 104, 180, 30, "Sus");
  wh(c, st, PkAmpR, 150, 180, 30, "Rel");

  flk::section(c, 204, 158, 552, 138, "Effects");
  flk::chk(c, PkDist, 212, 178, 60, 12, "DRIVE");
  wh(c, st, PkDrive, 212, 196, 26, "Drive");
  wh(c, st, PkDecimate, 246, 196, 26, "Crush");
  flk::chk(c, PkDelay, 290, 178, 60, 12, "DELAY");
  wh(c, st, PkDelayTime, 290, 196, 26, "Time");
  wh(c, st, PkDelayFeedback, 324, 196, 26, "Fdbk");
  wh(c, st, PkDelayFilter, 358, 196, 26, "Tone");
  wh(c, st, PkDelayMix, 392, 196, 26, "Mix");
  flk::chk(c, PkChorus, 436, 178, 60, 12, "CHORUS");
  wh(c, st, PkChorusRate, 436, 196, 26, "Rate");
  wh(c, st, PkChorusDepth, 470, 196, 26, "Depth");
  wh(c, st, PkChorusMix, 504, 196, 26, "Mix");
  flk::chk(c, PkPhaser, 548, 178, 60, 12, "PHASER");
  wh(c, st, PkPhaserRate, 548, 196, 26, "Rate");
  wh(c, st, PkPhaserWidth, 582, 196, 26, "Width");
  wh(c, st, PkPhaserFeedback, 616, 196, 26, "Fdbk");
  wh(c, st, PkPhaserMix, 650, 196, 26, "Mix");
  wh(c, st, PkPhaserFreq, 684, 196, 26, "Center");
  F.end(kW, kH);
  ImGui::PopID();
}

}  // namespace gl::packs::synths
