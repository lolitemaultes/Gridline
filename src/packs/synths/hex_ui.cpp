// Gridline — "synths" pack, J-33 group "hex": Hex Synth editor (gl.hexsynth).
// One dark panel in the reference's grouping: a header strip (program name,
// voices, unison, glide, gain, drive), the six oscillator rows (on, wave,
// pitch, phase, velocity, init → ADSR envelope, pan, level, LFO depths), the
// 6 × 6 modulation grid, filter + its envelope, amp envelope, the two LFOs,
// the 8-slot modulation matrix, the step sequencer, EQ and the two effect
// slots (controls for the chosen effect type). Readouts show the physical
// values through the reference laws (hex_laws.h).
#include <cmath>
#include <cstdio>
#include <string>
#include "../flc_kit.h"
#include "hex.h"
#include "hex_laws.h"
#include "synths_widgets.h"

namespace gl::packs::synths {

using namespace ui2;
using namespace hex;

namespace {

const skin::Skin& hexSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x2A3136, 0x1F2529, 0x3B454C, 0x8FD14F, 0x47525A, 0xDCE4E8);
    s.select = skin::hex(0x8FD14F);
    s.accent2 = skin::hex(0x8FD14F);
    s.lcdBg = skin::hex(0x141A1D);
    s.lcdText = skin::hex(0x8FD14F);
    s.graphBg = skin::hex(0x141A1D);
    s.graphGrid = skin::hex(0x232B30);
    s.graphGridStrong = skin::hex(0x2F393F);
    s.graphCurve = skin::hex(0x8FD14F);
    return s;
  }();
  return k;
}

// Physical readout of parameter p at position x.
std::string readout(int p, double x) {
  char b[48];
  auto secs = [&](double t) {
    if (t < 1) std::snprintf(b, sizeof b, "%.0f ms", t * 1000);
    else std::snprintf(b, sizeof b, "%.2f s", t);
  };
  if (p == HxGain) std::snprintf(b, sizeof b, "%+.1f dB", law::gainDb(x));
  else if (p == HxTranspose || p == HxEnvToPitch) std::snprintf(b, sizeof b, "%+d st", law::transpose(x));
  else if (p == HxAmpA || p == HxAmpD || p == HxFltA || p == HxFltD || p == HxFltR || (p >= HxOscA && p < HxOscS) ||
           (p >= HxOscR && p < HxPan))
    secs(law::time(x));
  else if (p == HxAmpR) secs(law::releaseTime(x));
  else if (p == HxGlide) secs(law::glideTime(x));
  else if (p == HxLfo1Speed || p == HxLfo2Speed) std::snprintf(b, sizeof b, "%.2f Hz", law::lfoHz(x));
  else if (p == HxCutoff) std::snprintf(b, sizeof b, "%.0f Hz", law::fltCutoffHz(x));
  else if (p == HxFltEnvAmt) std::snprintf(b, sizeof b, "%+.0f%%", 100 * law::fltEnvAmount(x));
  else if (p >= HxCoarse && p < HxFine) std::snprintf(b, sizeof b, "%+d st", law::coarse(x));
  else if (p >= HxFine && p < HxShift) std::snprintf(b, sizeof b, "%+.0f ct", law::fine(x) * 100);
  else if (p >= HxShift && p < HxFreeRun) std::snprintf(b, sizeof b, "%+.1f Hz", law::shiftHz(x));
  else if (p >= HxPan && p < HxMix) std::snprintf(b, sizeof b, "%+.0f", x * 100 - 50);
  else if (p >= HxFm && p < HxLfo1Osc) std::snprintf(b, sizeof b, "%.2f", law::fm(x));
  else if (p >= HxLfo1Osc && p < HxSeqPlay) std::snprintf(b, sizeof b, "%+.0f%%", law::lfoOsc(x, (p - HxLfo1Osc) % 6) * 100);
  else if (p == HxSeqSteps) std::snprintf(b, sizeof b, "%d steps", law::seqSteps(x));
  else if (p >= HxSeqStep && p < HxEqOn) {
    const int v = law::seqStep(x);
    if (v == -26) std::snprintf(b, sizeof b, "rest");
    else if (v == -25) std::snprintf(b, sizeof b, "tie");
    else std::snprintf(b, sizeof b, "%+d", v);
  } else if (p >= HxEqBand && p < HxFxOn) std::snprintf(b, sizeof b, "%+.1f dB", law::eqDb(x));
  else if (p >= HxModAmount && p < HxCount) std::snprintf(b, sizeof b, "%+.0f%%", law::modAmount(x) * 100);
  else if (p == HxBendRange) std::snprintf(b, sizeof b, "%d st", law::bendRange(x));
  else std::snprintf(b, sizeof b, "%.0f%%", x * 100);
  return b;
}

// Wheel with the physical readout while hot.
void wh(const flk::Ctx& c, const PluginState& st, int p, float x, float y, float size, const char* label, bool centred = false) {
  skin::wheel(c.a, c.t, p, c.F.p(x, y), size * c.F.s, c.K, skin::WheelBigLine | skin::WheelNoValue | (centred ? skin::WheelCentered : 0));
  const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
  std::string s = hot ? readout(p, val(st, p)) : flk::upper(label);
  if (!s.empty())
    skin::label(c.dl, c.F.p(x + size * 0.5f, 0).x, c.F.p(0, y + size + 1.5f).y, s.c_str(), hot ? c.K.text : c.K.textDim, c.F.font(7.0f));
}

// One-pole envelope preview (attack chases 5, decay / release T / 8.82).
void envView(const flk::Ctx& c, float x, float y, float w, float h, double init, double a, double d, double s, double r) {
  ImVec2 p0 = c.F.p(x, y), p1 = c.F.p(x + w, y + h);
  skin::graphWell(c.dl, p0, p1, c.K);
  const double held = std::max(a + d * 1.5, 0.05), span = held + r * 1.2 + 0.01;
  ImVec2 pts[96];
  double v = init;
  const double dt = span / 95.0;
  for (int i = 0; i < 96; ++i) {
    const double t = i * dt;
    pts[i] = ImVec2(p0.x + static_cast<float>(i / 95.0) * (p1.x - p0.x), p1.y - 2 - static_cast<float>(std::min(1.0, v)) * (p1.y - p0.y - 4));
    double tgt, tau;
    if (t < a) tgt = 5, tau = a / 0.2236212;
    else if (t < held) tgt = s, tau = d / 8.82;
    else tgt = init, tau = r / 8.82;
    v += (tgt - v) * (1.0 - std::exp(-dt / std::max(1e-6, tau)));
    v = std::min(v, 1.0);
  }
  c.dl->AddPolyline(pts, 96, c.K.graphCurve, 0, 1.4f);
}

void fxControls(const flk::Ctx& c, const PluginState& st, int s, float x, float y) {
  const int type = ival(st, HxFxType + s);
  struct K {
    int p;
    const char* n;
  };
  static const K del[] = {{HxDlTime, "Time"}, {HxDlFeedback, "Feedback"}, {HxDlRate, "Rate"}, {HxDlDepth, "Depth"}, {HxDlBlur, "Blur"}, {HxDlMix, "Mix"}};
  static const K cho[] = {{HxChDepth, "Depth"}, {HxChRate, "Rate"}, {HxChMix, "Mix"}};
  static const K rev[] = {{HxRvMix, "Mix"}, {HxRvDecay, "Decay"}, {HxRvDamp, "Damp"}, {HxRvHighCut, "Hi cut"}};
  static const K fla[] = {{HxFlDelay, "Delay"}, {HxFlFeedback, "Feedback"}, {HxFlDepth, "Depth"}, {HxFlRate, "Rate"}, {HxFlMix, "Mix"}};
  static const K pha[] = {{HxPhFreq, "Freq"}, {HxPhRate, "Rate"}, {HxPhDepth, "Depth"}, {HxPhFeedback, "Feedback"}, {HxPhMix, "Mix"}};
  static const K lof[] = {{HxLfBits, "Bits"}, {HxLfRate, "Rate"}, {HxLfFilter, "Filter"}};
  const K* ks = del;
  int n = 6;
  switch (type) {
    case FxChorus: ks = cho, n = 3; break;
    case FxReverb: ks = rev, n = 4; break;
    case FxFlanger: ks = fla, n = 5; break;
    case FxPhaser: ks = pha, n = 5; break;
    case FxLoFi: ks = lof, n = 3; break;
    default: break;
  }
  for (int i = 0; i < n; ++i) wh(c, st, ks[i].p + s, x + 36.f * i, y, 26, ks[i].n);
  if (type == FxDelay) flk::menu(c, HxFxSync + s, x + 220, y + 6, 46, 14);
  if (type == FxFlanger) {
    flk::chk(c, HxFlInvFeedback + s, x + 186, y, 60, 12, "-FB");
    flk::chk(c, HxFlInvMix + s, x + 186, y + 16, 60, 12, "-MIX");
  }
}

}  // namespace

void drawHexSynthEditor(App& a, const PluginTarget& t, float W) {
  const PluginState* sp = t.state(a.doc.song());
  if (!sp) return;
  const PluginState st = *sp;
  const skin::Skin& K = hexSkin();
  constexpr float kW = 980, kH = 600;
  ImGui::PushID("gl.hexsynth");
  skin::Form F = skin::Form::fit(W, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  flk::Ctx c{a, t, F, K, dl};

  // ---- header
  flk::section(c, 4, 4, 972, 40, "");
  skin::text(dl, F.p(14, 15), K.accent2C(), "HEX SYNTH", F.font(15));
  {
    HexData d;
    parseHexData(st.data, d);
    flk::lcd(c, 112, 14, 170, 18, d.name.empty() ? "(untitled)" : d.name.c_str());
  }
  flk::text(c, 296, 10, "VOICES");
  flk::menu(c, HxVoices, 296, 22, 50, 15);
  flk::text(c, 352, 10, "UNISON");
  flk::menu(c, HxUnison, 352, 22, 46, 15);
  flk::text(c, 404, 10, "GLIDE MODE");
  flk::menu(c, HxGlideMode, 404, 22, 104, 15);
  wh(c, st, HxGlide, 516, 8, 20, "Glide");
  wh(c, st, HxUniDetune, 548, 8, 20, "Detune");
  wh(c, st, HxUniPan, 580, 8, 20, "Spread");
  wh(c, st, HxVelCurve, 612, 8, 20, "Vel crv");
  wh(c, st, HxBendRange, 644, 8, 20, "Bend");
  wh(c, st, HxTranspose, 676, 8, 20, "Transp", true);
  wh(c, st, HxDrive, 708, 8, 20, "Drive");
  wh(c, st, HxGain, 740, 8, 20, "Gain", true);

  // ---- oscillators
  flk::section(c, 4, 48, 642, 280, "Oscillators");
  static const char* cols[] = {"Wave", "Coarse", "Fine", "Shift", "Phase", "Vel", "Init", "Atk", "Dec", "Sus", "Rel", "Pan", "Level", "LFO1", "LFO2"};
  for (int k = 0; k < 15; ++k) flk::textC(c, k == 0 ? 76 : 120 + 34.f * (k - 1) + 12, 64, cols[k]);
  for (int k = 0; k < kOscs; ++k) {
    const float y = 74 + 42.f * k;
    char nm[8];
    std::snprintf(nm, sizeof nm, "%d", k + 1);
    flk::tog(c, HxOscOn + k, 10, y + 4, 20, 16, nm);
    flk::menu(c, HxWave + k, 36, y + 5, 78, 15);
    flk::chk(c, HxFreeRun + k, 36, y + 23, 78, 11, "FREE RUN");
    const int ps[14] = {HxCoarse, HxFine, HxShift, HxPhase, HxVelSens, HxInit, HxOscA, HxOscD, HxOscS, HxOscR, HxPan, HxMix, HxLfo1Osc, HxLfo2Osc};
    for (int i = 0; i < 14; ++i) {
      const int p = ps[i] + k;
      const bool centred = ps[i] == HxCoarse || ps[i] == HxFine || ps[i] == HxShift || ps[i] == HxPan || ps[i] == HxLfo1Osc || ps[i] == HxLfo2Osc;
      skin::wheel(a, t, p, F.p(120 + 34.f * i, y + 2), 24 * F.s, K, skin::WheelBigLine | skin::WheelNoValue | (centred ? skin::WheelCentered : 0));
      if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        const std::string s = readout(p, val(st, p));
        skin::label(dl, F.p(120 + 34.f * i + 12, 0).x, F.p(0, y + 28).y, s.c_str(), K.text, F.font(7.0f));
      }
    }
  }

  // ---- FM matrix
  flk::section(c, 650, 48, 160, 186, "Phase mod");
  for (int s = 0; s < kOscs; ++s) {
    char b[4];
    std::snprintf(b, sizeof b, "%d", s + 1);
    flk::textC(c, 680 + 21.f * s + 9, 64, b);
    flk::text(c, 656, 78 + 23.f * s, b);
  }
  for (int d = 0; d < kOscs; ++d)
    for (int s = 0; s < kOscs; ++s) {
      const int p = HxFm + 6 * d + s;
      skin::wheel(a, t, p, F.p(680 + 21.f * s, 74 + 23.f * d), 18 * F.s, K, skin::WheelNoValue);
      if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        char b[64];
        std::snprintf(b, sizeof b, "%d > %d: %s", s + 1, d + 1, readout(p, val(st, p)).c_str());
        skin::label(dl, F.p(730, 0).x, F.p(0, 220).y, b, K.text, F.font(7.0f));
      }
    }
  flk::text(c, 656, 222, "ROW = TARGET  COL = SOURCE", K.textDim, 6.5f);

  // ---- amp envelope + LFOs
  flk::section(c, 814, 48, 162, 92, "Amp");
  envView(c, 820, 64, 150, 30, 0, law::time(val(st, HxAmpA)), law::time(val(st, HxAmpD)), law::squared(val(st, HxAmpS)), law::releaseTime(val(st, HxAmpR)));
  wh(c, st, HxAmpA, 822, 98, 22, "Atk");
  wh(c, st, HxAmpD, 860, 98, 22, "Dec");
  wh(c, st, HxAmpS, 898, 98, 22, "Sus");
  wh(c, st, HxAmpR, 936, 98, 22, "Rel");
  flk::section(c, 814, 142, 162, 92, "LFOs");
  for (int l = 0; l < 2; ++l) {
    const float y = 158 + 38.f * l;
    flk::menu(c, HxWave + kOscs + l, 820, y, 66, 14);
    flk::menu(c, l == 0 ? HxLfo1Sync : HxLfo2Sync, 820, y + 17, 40, 14);
    flk::chk(c, l == 0 ? HxLfo1Free : HxLfo2Free, 864, y + 18, 30, 12, "FREE");
    wh(c, st, l == 0 ? HxLfo1Speed : HxLfo2Speed, 896, y, 22, "Speed");
    if (l == 0) wh(c, st, HxFltLfo1, 936, y, 22, "Filter");
    else wh(c, st, HxLfo2ToPitch, 936, y, 22, "Pitch");
  }

  // ---- filter
  flk::section(c, 650, 238, 326, 90, "Filter");
  flk::menu(c, HxFilterType, 656, 256, 70, 15);
  wh(c, st, HxCutoff, 734, 254, 24, "Cutoff");
  wh(c, st, HxReso, 766, 254, 24, "Reso");
  wh(c, st, HxKeyTrack, 798, 254, 24, "Key");
  wh(c, st, HxVelTrack, 830, 254, 24, "Vel");
  wh(c, st, HxFltEnvAmt, 862, 254, 24, "Env", true);
  wh(c, st, HxEnvToPitch, 894, 254, 24, "Pitch", true);
  envView(c, 656, 276, 70, 46, 0, law::time(val(st, HxFltA)), law::time(val(st, HxFltD)), law::squared(val(st, HxFltS)), law::time(val(st, HxFltR)));
  wh(c, st, HxFltA, 734, 290, 22, "A");
  wh(c, st, HxFltD, 766, 290, 22, "D");
  wh(c, st, HxFltS, 798, 290, 22, "S");
  wh(c, st, HxFltR, 830, 290, 22, "R");

  // ---- modulation matrix
  flk::section(c, 4, 332, 330, 264, "Modulation");
  flk::text(c, 30, 348, "SOURCE");
  flk::text(c, 128, 348, "TARGET");
  flk::text(c, 288, 348, "AMOUNT");
  for (int m = 0; m < kModSlots; ++m) {
    const float y = 362 + 28.f * m;
    char b[4];
    std::snprintf(b, sizeof b, "%d", m + 1);
    flk::text(c, 12, y + 3, b);
    flk::menu(c, HxModSource + m, 24, y, 96, 15);
    flk::menu(c, HxModTarget + m, 124, y, 156, 15);
    skin::wheel(a, t, HxModAmount + m, F.p(294, y - 3), 20 * F.s, K, skin::WheelCentered | skin::WheelNoValue);
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
      skin::label(dl, F.p(260, 0).x, F.p(0, y + 18).y, readout(HxModAmount + m, val(st, HxModAmount + m)).c_str(), K.text, F.font(7.0f));
  }

  // ---- sequencer
  flk::section(c, 338, 332, 400, 140, "Sequencer");
  flk::chk(c, HxSeqPlay, 344, 350, 40, 12, "ON");
  flk::chk(c, HxSeqSwing, 390, 350, 52, 12, "SWING");
  flk::chk(c, HxSeqPingPong, 446, 350, 52, 12, "P-P");
  flk::chk(c, HxSeqRandom, 502, 350, 52, 12, "RND");
  flk::chk(c, HxSeqDual, 558, 350, 52, 12, "DUAL");
  wh(c, st, HxSeqSteps, 700, 344, 20, "Steps");
  {
    const int steps = law::seqSteps(val(st, HxSeqSteps));
    for (int s = 0; s < kSeqSteps; ++s) {
      const float x = 344 + 6.f * s;
      flk::vsl(c, HxSeqStep + s, x, 376, 5, 78, nullptr, s < steps ? K.fill : K.rail);
    }
  }

  // ---- EQ
  flk::section(c, 742, 332, 234, 140, "EQ");
  flk::chk(c, HxEqOn, 748, 350, 40, 12, "ON");
  static const char* eqn[kEqBands] = {"63", "125", "250", "500", "1K", "2K", "4K", "8K"};
  for (int b = 0; b < kEqBands; ++b) flk::vsl(c, HxEqBand + b, 752 + 27.f * b, 370, 12, 76, eqn[b]);

  // ---- effects
  for (int s = 0; s < 2; ++s) {
    const float y = 476 + 60.f * s;
    char title[8];
    std::snprintf(title, sizeof title, "FX %d", s + 1);
    flk::section(c, 338, y, 638, 56, title);
    flk::chk(c, HxFxOn + s, 344, y + 22, 34, 12, "ON");
    flk::menu(c, HxFxType + s, 382, y + 20, 70, 15);
    fxControls(c, st, s, 462, y + 14);
  }
  F.end(kW, kH);
  ImGui::PopID();
}

}  // namespace gl::packs::synths
