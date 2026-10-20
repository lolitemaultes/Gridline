// Gridline — "synths" pack, J-22: DCO Synth editor (gl.dcosynth) on the
// reference two-DCO synth's 1080 × 574 panel (control positions taken from
// its form): master strip; DCO 1 (LFO / env amounts, PWM, waveform buttons,
// sub), cross mod, DCO 2 and the mixer; the filter row (slope, cutoff, reso,
// envelope / LFO amounts, key follow, filter FM, high pass), VCA, the two
// envelopes and LFOs; voice / portamento, arpeggiator, the paged 8-slot
// matrix, the chorus block and the paged effects panel. The "One DCO" panel
// shows the classic single-DCO front panel on its own 1080 × 217 form.
// Everything is drawn with the shared skin kit (no reference artwork).
#include <map>
#include "../../core/I18n.h"
#include "../flc_kit.h"
#include "dco.h"
#include "synths_common.h"
#include "synths_widgets.h"

namespace gl::packs::synths {

using namespace ui2;
using namespace dco;

namespace {

const skin::Skin& dcoSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x212527, 0x2A2F33, 0x4A5156, 0xE8C77A, 0x3C4348, 0xB9BFC3);
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
const skin::Skin& fxSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = dcoSkin();
    s.well = skin::hex(0x404040);
    return s;
  }();
  return k;
}

std::map<uint64_t, int>& pages() {
  static std::map<uint64_t, int> m;
  return m;
}

// Waveform buttons in the reference's colours (pulse gold, saw amber, sub orange).
constexpr uint32_t kPulseCol = 0xC8B26A, kSawCol = 0xC9AA44, kSubCol = 0xD98509;

void waveButtons(const flk::Ctx& c, float x0, float y, int pulse, int saw, int sub) {
  const int prm[3] = {pulse, saw, sub};
  const uint32_t col[3] = {kPulseCol, kSawCol, kSubCol};
  const char* name[3] = {"PULSE", "SAW", "SUB"};
  for (int i = 0; i < 3; ++i) {
    skin::Skin k = c.K;
    k.fill = skin::hex(col[i]);
    k.select = skin::hex(col[i]);
    skin::toggle(c.a, c.t, prm[i], c.F.p(x0 + 36 * i, y), c.F.sz(33, 33), k, name[i], c.F.font(7.0f));
  }
}

void drawPanelTwo(App& a, const PluginTarget& t, const PluginState& st, float W) {
  const skin::Skin& K = dcoSkin();
  constexpr float kW = 1080, kH = 574;
  skin::Form F = skin::Form::fit(W, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  flk::Ctx c{a, t, F, K, dl};
  const int pageKey = static_cast<int>(t.key() & 0xffff);
  (void)pageKey;

  // ---- master strip
  flk::tog(c, DcHold, 27, 17, 33, 19, "HOLD");
  static const char* oct[3] = {"-1", "0", "+1"};
  flk::sw(c, DcOctave, 94, 17, 54, 18, oct, 3, true);
  flk::text(c, 160, 22, "OCTAVE");
  flk::wheel(c, DcXOffset, 653, 15, 24, "X", skin::WheelBigLine | skin::WheelCentered);
  flk::wheel(c, DcYOffset, 689, 15, 24, "Y", skin::WheelBigLine | skin::WheelCentered);
  flk::wheel(c, DcMasterTune, 725, 15, 24, "Tune", skin::WheelBigLine | skin::WheelCentered);
  flk::wheel(c, DcVolume, 761, 15, 24, "Vol");
  flk::wheel(c, DcPan, 797, 15, 24, "Pan", skin::WheelBigLine | skin::WheelCentered);
  skin::text(dl, F.p(842, 18), K.accent2C(), "DCO SYNTH", F.font(15));
  ImGui::SetCursorScreenPos(F.p(250, 8));
  liveScope(a, t, F.sz(360, 38));

  // ---- DCO 1
  flk::section(c, 6, 52, 345, 176, "DCO 1");
  flk::chk(c, DcLfo1ToDco1Wide, 32, 77, 14, 13, "");
  flk::chk(c, DcLfo2ToDco1Wide, 58, 77, 14, 13, "");
  flk::text(c, 76, 79, "WIDE");
  flk::vsl(c, DcLfo1ToDco1, 29, 108, 23, 104, "LFO1");
  flk::vsl(c, DcLfo2ToDco1, 55, 108, 23, 104, "LFO2");
  flk::vsl(c, DcEnv2ToDco1, 81, 108, 23, 104, "ENV2");
  flk::vsl(c, DcDco1Pw, 107, 108, 23, 104, "PW");
  static const char* pwSrc[3] = {"LFO", "MAN", "ENV"};
  flk::sw(c, DcDco1PwSrc, 138, 138, 37, 45, pwSrc, 3);
  waveButtons(c, 179, 144, DcDco1Pulse, DcDco1Saw, DcDco1Sub);
  {
    ImVec2 p0 = F.p(179, 96), p1 = F.p(284, 136);
    skin::graphWell(dl, p0, p1, K);
    const bool pu = val(st, DcDco1Pulse) >= 0.5f, sa = val(st, DcDco1Saw) >= 0.5f, sb = val(st, DcDco1Sub) >= 0.5f;
    const double pw = 0.5 - 0.45 * val(st, DcDco1Pw);
    const double sl = val(st, DcDco1SubLevel);
    cycleGraph(dl, p0, ImVec2(p1.x - p0.x, p1.y - p0.y),
               [&](double ph) {
                 double v = 0;
                 const double q = std::fmod(ph * 2.0, 1.0);
                 if (sa) v += 2 * q - 1;
                 if (pu) v += q < pw ? 1 : -1;
                 if (sb) v += (ph < 0.5 ? 1 : -1) * sl;
                 return v / std::max(1, static_cast<int>(pu) + static_cast<int>(sa) + static_cast<int>(sb));
               },
               K.graphCurve, 1.3f);
  }
  flk::vsl(c, DcDco1SubLevel, 292, 108, 23, 104, "SUB");
  flk::vsl(c, DcDco1SubPhase, 321, 108, 23, 104, "PHASE");

  // ---- cross mod
  flk::section(c, 355, 52, 178, 176, "CROSS MOD");
  static const char* xs[6] = {"SIN", "TRI", "RMP+", "RMP-", "SQR", "OUT"};
  flk::vsl(c, DcXmod12, 364, 108, 23, 104, "1>2");
  flk::sw(c, DcXmod12Shape, 395, 115, 46, 90, xs, 6);
  flk::vsl(c, DcXmod21, 446, 108, 23, 104, "2>1");
  flk::sw(c, DcXmod21Shape, 478, 115, 46, 90, xs, 6);

  // ---- DCO 2
  flk::section(c, 537, 52, 400, 176, "DCO 2");
  flk::chk(c, DcDco2On, 600, 55, 50, 14, "ON");
  flk::chk(c, DcLfo1ToDco2Wide, 546, 76, 14, 13, "");
  flk::chk(c, DcLfo2ToDco2Wide, 572, 76, 14, 13, "");
  flk::text(c, 590, 78, "WIDE");
  flk::vsl(c, DcLfo1ToDco2, 543, 108, 23, 104, "LFO1");
  flk::vsl(c, DcLfo2ToDco2, 569, 108, 23, 104, "LFO2");
  flk::vsl(c, DcEnv2ToDco2, 595, 108, 23, 104, "ENV2");
  flk::vsl(c, DcDco2Pw, 621, 108, 23, 104, "PW");
  flk::sw(c, DcDco2PwSrc, 650, 138, 37, 45, pwSrc, 3);
  waveButtons(c, 691, 144, DcDco2Pulse, DcDco2Saw, DcDco2Sub);
  flk::vsl(c, DcDco2SubLevel, 803, 108, 23, 104, "SUB");
  flk::vsl(c, DcDco2SubPhase, 829, 108, 23, 104, "PHASE");
  flk::vsl(c, DcDco2Semi, 855, 108, 23, 104, "SEMI");
  flk::vsl(c, DcDco2Detune, 881, 108, 23, 104, "DETUNE");
  flk::chk(c, DcSync, 908, 122, 26, 13, "");
  flk::textC(c, 921, 140, "SYNC");
  {
    char b[32];
    std::snprintf(b, sizeof b, "%+.1f st", val(st, DcDco2Semi) * 96.0 - 48.0);
    flk::lcd(c, 691, 97, 105, 16, b);
  }

  // ---- mixer
  flk::section(c, 941, 52, 133, 176, "MIXER");
  flk::vsl(c, DcDco1Level, 950, 108, 23, 104, "DCO1");
  flk::vsl(c, DcDco2Level, 976, 108, 23, 104, "DCO2");
  flk::vsl(c, DcNoise, 1002, 108, 23, 104, "NOISE");
  flk::vsl(c, DcAm, 1027, 108, 23, 104, "AM");

  // ---- filter
  flk::section(c, 6, 232, 352, 168, "VCF");
  static const char* slope[4] = {"6", "12", "18", "24"};
  flk::sw(c, DcSlope, 26, 303, 44, 60, slope, 4);
  flk::text(c, 30, 290, "DB/OCT");
  flk::vsl(c, DcCutoff, 74, 279, 23, 104, "CUT");
  flk::vsl(c, DcReso, 99, 279, 23, 104, "RES");
  static const char* pol[2] = {"+", "-"};
  flk::sw(c, DcEnvPolarity, 128, 316, 20, 30, pol, 2);
  flk::vsl(c, DcEnv1ToCut, 152, 279, 23, 104, "ENV");
  flk::vsl(c, DcLfo1ToCut, 178, 279, 23, 104, "LFO1");
  flk::vsl(c, DcLfo2ToCut, 204, 279, 23, 104, "LFO2");
  flk::vsl(c, DcKeyFollow, 230, 279, 23, 104, "KEY");
  flk::sw(c, DcFilterFmShape, 260, 282, 58, 90, xs, 6);
  flk::vsl(c, DcFilterFm, 323, 279, 23, 104, "FM");
  flk::section(c, 362, 232, 46, 168, "HPF");
  flk::chk(c, DcHpfOn, 375, 249, 14, 13, "");
  flk::vsl(c, DcHpf, 371, 279, 23, 104, "FREQ");
  flk::section(c, 410, 232, 54, 168, "VCA");
  static const char* vca[3] = {"ENV1", "GATE", "ENV2"};
  flk::sw(c, DcVcaSrc, 414, 282, 44, 45, vca, 3);
  flk::wheel(c, DcVelVol, 422, 336, 24, "Vel", skin::WheelBigLine);
  flk::chk(c, DcVcaSmooth, 412, 377, 48, 13, "SMTH");

  // ---- envelopes
  static const char* adsr[4] = {"A", "D", "S", "R"};
  flk::section(c, 468, 232, 116, 168, "ENV 1");
  for (int i = 0; i < 4; ++i) flk::vsl(c, DcEnv1A + i, 475 + 26.f * i, 279, 23, 104, adsr[i]);
  flk::section(c, 588, 232, 116, 168, "ENV 2");
  for (int i = 0; i < 4; ++i) flk::vsl(c, DcEnv2A + i, 596 + 26.f * i, 279, 23, 104, adsr[i]);

  // ---- LFOs
  static const char* trig[2] = {"FREE", "KEY"};
  flk::section(c, 708, 232, 146, 168, "LFO 1");
  flk::chk(c, DcLfo1Sync, 716, 249, 40, 13, "SYNC");
  flk::vsl(c, DcLfo1Rate, 712, 279, 23, 104, "RATE");
  flk::vsl(c, DcLfo1Delay, 740, 279, 23, 104, "DELAY");
  flk::sw(c, DcLfo1Trig, 772, 316, 40, 30, trig, 2);
  flk::chk(c, DcLfo1PhaseOn, 812, 249, 40, 13, "PH");
  flk::vsl(c, DcLfo1Phase, 821, 279, 23, 104, "PHASE");
  flk::section(c, 858, 232, 216, 168, "LFO 2");
  flk::chk(c, DcLfo2Sync, 865, 249, 40, 13, "SYNC");
  flk::vsl(c, DcLfo2Rate, 862, 279, 23, 104, "RATE");
  flk::vsl(c, DcLfo2Delay, 890, 279, 23, 104, "DELAY");
  flk::sw(c, DcLfo2Trig, 924, 316, 40, 30, trig, 2);
  flk::chk(c, DcLfo2PhaseOn, 968, 249, 40, 13, "PH");
  flk::vsl(c, DcLfo2Phase, 975, 279, 23, 104, "PHASE");
  static const char* l2[6] = {"TRI", "SIN", "SQR", "RMP+", "RMP-", "RND"};
  flk::sw(c, DcLfo2Shape, 1006, 282, 50, 90, l2, 6);

  // ---- voice
  flk::section(c, 6, 404, 140, 166, "VOICE");
  flk::tog(c, DcLfoTrigger, 30, 424, 48, 21, "LFO TRG");
  static const char* pm[2] = {"EXP", "LIN"};
  flk::sw(c, DcPortaMode, 31, 461, 48, 30, pm, 2);
  flk::wheel(c, DcPorta, 31, 508, 34, "Porta");
  flk::chk(c, DcMono, 82, 465, 56, 13, "MONO");
  flk::chk(c, DcLegato, 82, 489, 56, 13, "LEGATO");
  flk::chk(c, DcRetrig, 82, 514, 56, 13, "RETRIG");
  flk::chk(c, DcRelease, 82, 539, 56, 13, "RELEASE");

  // ---- arpeggiator
  flk::section(c, 148, 404, 177, 166, "ARPEGGIATOR");
  flk::tog(c, DcArpOn, 151, 425, 33, 26, "ARP");
  static const char* rng[3] = {"1", "2", "3"};
  flk::sw(c, DcArpRange, 154, 470, 29, 45, rng, 3);
  flk::text(c, 151, 520, "RANGE");
  flk::menu(c, DcArpMode, 190, 430, 66, 18);
  flk::chk(c, DcArpSync, 262, 424, 52, 13, "SYNC");
  flk::vsl(c, DcArpRate, 263, 453, 23, 96, "RATE");
  flk::vsl(c, DcArpGate, 291, 453, 23, 96, "GATE");
  flk::chk(c, DcArpVariant, 190, 455, 66, 13, "CLASSIC");

  // ---- matrix (two pages of four rows)
  int& mxPage = pages()[t.key() ^ 0x4d58];
  flk::section(c, 329, 404, 220, 166, "MOD MATRIX");
  if (flk::btn(c, "##mxpage", 498, 406, 46, 13, mxPage ? "5-8" : "1-4", true)) mxPage ^= 1;
  for (int r = 0; r < 4; ++r) {
    const int row = mxPage * 4 + r;
    const float y = 455 + 26.f * r;
    ImGui::PushID(row);
    flk::menu(c, DcMxSrc1 + row, 333, y, 89, 21);
    flk::menu(c, DcMxDst1 + row, 425, y, 89, 21);
    skin::wheel(a, t, DcMxAmt1 + row, F.p(518, y - 1), 24 * F.s, K, skin::WheelBigLine | skin::WheelCentered);
    ImGui::PopID();
  }
  flk::text(c, 336, 437, "SOURCE");
  flk::text(c, 428, 437, "TARGET");

  // ---- chorus
  flk::section(c, 553, 404, 83, 166, "CHORUS");
  static const char* ch[4] = {"OFF", "I", "II", "I+II"};
  flk::sw(c, DcChorus, 558, 490, 72, 73, ch, 4);
  flk::wheel(c, DcChorusNoise, 560, 432, 30, "Noise");
  flk::chk(c, DcChorusGate, 597, 440, 36, 13, "GATE");

  // ---- effects (paged)
  const skin::Skin& KF = fxSkin();
  flk::Ctx cf{a, t, F, KF, dl};
  skin::well(dl, F.p(640, 398), F.p(1060, 562), KF, 4);
  int& page = pages()[t.key()];
  static const char* tabs[5] = {"SAT", "ENSEMBLE", "EQ", "DELAY", "REVERB"};
  static const int tabOn[5] = {DcSatOn, DcEnsOn, DcEqOn, DcDlyOn, DcRevOn};
  for (int i = 0; i < 5; ++i) {
    ImGui::PushID(i);
    if (flk::btn(cf, "##fxtab", 664 + 84.f * i, 400, 62, 19, tabs[i], page == i)) page = i;
    flk::chk(cf, tabOn[i], 644 + 84.f * i, 402, 16, 16, "");
    ImGui::PopID();
  }
  switch (page) {
    case 0: {
      flk::vsl(cf, DcSatAmount, 732, 453, 23, 96, "AMOUNT");
      static const char* pp[2] = {"PRE", "POST"};
      flk::sw(cf, DcSatPost, 684, 470, 37, 40, pp, 2);
      flk::chk(cf, DcSatHq, 684, 544, 40, 13, "HQ");
      ImVec2 p0 = F.p(766, 439), p1 = F.p(1042, 553);
      skin::graphWell(dl, p0, p1, KF);
      const double drive = 1.0 + val(st, DcSatAmount) * 15.0;
      const double mk = 1.0 / softClip(drive * 0.5) * 0.5;
      cycleGraph(dl, p0, ImVec2(p1.x - p0.x, p1.y - p0.y), [&](double x) { return softClip((2 * x - 1) * drive) * mk; },
                 KF.graphCurve, 1.5f);
      break;
    }
    case 1: {
      const int e[10] = {DcEnsDelay, DcEnsFb, DcEnsDepth, DcEnsRate, DcEnsMove, DcEnsMoveRate, DcEnsHighPass, DcEnsLowPass, DcEnsPhase, DcEnsMix};
      const char* n[10] = {"DELAY", "FDBK", "DEPTH", "RATE", "MOVE", "M.RATE", "HP", "LP", "PHASE", "MIX"};
      const float x[10] = {655, 692, 730, 767, 805, 843, 919, 955, 991, 1027};
      for (int i = 0; i < 10; ++i) flk::vsl(cf, e[i], x[i], 453, 23, 96, n[i]);
      static const char* ln[2] = {"2", "4"};
      flk::sw(cf, DcEnsLines, 880, 470, 30, 40, ln, 2);
      flk::text(cf, 876, 515, "LINES");
      break;
    }
    case 2: {
      const char* bands[3] = {"LOW", "MID", "HIGH"};
      for (int b = 0; b < 3; ++b) {
        const float x = 669 + 31.f * b;
        flk::textC(cf, x + 15, 442, bands[b]);
        skin::wheel(a, t, DcEqGainLo + b, F.p(x, 452), 30 * F.s, KF, skin::WheelBigLine | skin::WheelCentered);
        skin::wheel(a, t, DcEqFreqLo + b, F.p(x, 488), 30 * F.s, KF, skin::WheelBigLine);
        skin::wheel(a, t, DcEqQLo + b, F.p(x, 524), 30 * F.s, KF, skin::WheelBigLine);
      }
      ImVec2 p0 = F.p(766, 439), p1 = F.p(1039, 553);
      skin::graphWell(dl, p0, p1, KF);
      // Response: sum of the three bands (dB, ±18) over 20 Hz..20 kHz.
      auto bandDb = [&](double hz) {
        double db = 0;
        for (int b = 0; b < 3; ++b) {
          const double g = (val(st, DcEqGainLo + b) * 2.0 - 1.0) * 18.0;
          const double f0 = 20.0 * std::pow(1000.0, val(st, DcEqFreqLo + b));
          const double q = 0.3 * std::pow(20.0, val(st, DcEqQLo + b));
          const double r = std::log2(hz / f0);
          if (b == 0) db += g / (1.0 + std::exp(4.0 * q * r));
          else if (b == 2) db += g / (1.0 + std::exp(-4.0 * q * r));
          else db += g * std::exp(-r * r * q * 2.0);
        }
        return db;
      };
      cycleGraph(dl, p0, ImVec2(p1.x - p0.x, p1.y - p0.y), [&](double x) { return bandDb(20.0 * std::pow(1000.0, x)) / 18.0; },
                 KF.graphCurve, 1.5f);
      break;
    }
    case 3: {
      flk::chk(cf, DcDlySync, 751, 422, 50, 14, "SYNC");
      const int d[9] = {DcDlyInput, DcDlyTime, DcDlyRightTap, DcDlyFb, DcDlyFbCut, DcDlyStereo, DcDlyBlur, DcDlyTone, -1};
      const char* n[8] = {"INPUT", "TIME", "R TAP", "FDBK", "CUT", "STEREO", "BLUR", "TONE"};
      const float x[8] = {709, 748, 788, 825, 862, 947, 987, 1027};
      for (int i = 0; i < 8; ++i) flk::vsl(cf, d[i], x[i], 453, 23, 96, n[i]);
      static const char* ft[4] = {"LP", "HP", "BP", "OFF"};
      flk::sw(cf, DcDlyFbType, 895, 470, 44, 60, ft, 4);
      break;
    }
    default: {
      const char* n[12] = {"LOW", "HIGH", "PRE", "PRE FB", "SIZE", "DIFF", "DECAY", "DAMP", "WIDTH", "DRY", "ER", "WET"};
      const float x[12] = {670, 697, 725, 753, 807, 834, 862, 890, 943, 971, 999, 1027};
      for (int i = 0; i < 12; ++i) flk::vsl(cf, DcRevLowCut + i, x[i], 453, 23, 96, n[i]);
      break;
    }
  }
  F.end(kW, kH);
}

void drawPanelOne(App& a, const PluginTarget& t, float W) {
  const skin::Skin& K = dcoSkin();
  constexpr float kW = 1080, kH = 217;
  skin::Form F = skin::Form::fit(W, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  flk::Ctx c{a, t, F, K, dl};
  flk::tog(c, DcHold, 11, 18, 37, 19, "HOLD");
  static const char* oct[3] = {"-1", "0", "+1"};
  flk::sw(c, DcOctave, 85, 18, 60, 18, oct, 3, true);
  flk::wheel(c, DcVolume, 884, 16, 24, "Vol");
  skin::text(dl, F.p(930, 18), K.accent2C(), "DCO SYNTH", F.font(15));
  ImGui::SetCursorScreenPos(F.p(300, 8));
  liveScope(a, t, F.sz(480, 34));
  // Arpeggiator.
  flk::section(c, 6, 46, 162, 166, "ARP");
  flk::tog(c, DcArpOn, 17, 128, 33, 33, "ON");
  static const char* am[3] = {"UP", "DN", "U&D"};
  // The classic panel stores the three modes on the 0 / 4 / 8 positions.
  {
    const PluginState* sp = t.state(a.doc.song());
    const int cur = sp ? std::clamp(static_cast<int>(std::lround(val(*sp, DcArpMode) / 4.0)), 0, 2) : 0;
    for (int i = 0; i < 3; ++i) {
      ImGui::PushID(i);
      if (flk::btn(c, "##am", 60, 120 + 16.f * i, 34, 15, am[i], cur == i) && sp) {
        PluginState s = *sp;
        s.params[DcArpMode] = static_cast<float>(i * 4);
        setPluginState(a, t, s, "Arpeggio mode");
      }
      ImGui::PopID();
    }
  }
  static const char* rng[3] = {"1", "2", "3"};
  flk::sw(c, DcArpRange, 102, 120, 31, 48, rng, 3);
  flk::chk(c, DcArpSync, 139, 65, 26, 16, "");
  flk::vsl(c, DcArpRate, 139, 93, 23, 96, "RATE");
  // LFO.
  flk::section(c, 172, 46, 136, 166, "LFO");
  flk::chk(c, DcLfo1Sync, 180, 65, 18, 16, "");
  flk::vsl(c, DcLfo1Rate, 178, 93, 23, 96, "RATE");
  flk::vsl(c, DcLfo1Delay, 204, 93, 23, 96, "DELAY");
  static const char* trig[2] = {"AUTO", "MAN"};
  flk::sw(c, DcLfo1Trig, 236, 120, 37, 33, trig, 2);
  flk::tog(c, DcLfoTrigger, 237, 179, 33, 17, "TRG");
  flk::chk(c, DcLfo1PhaseOn, 281, 65, 18, 16, "");
  flk::vsl(c, DcLfo1Phase, 279, 93, 23, 96, "PH");
  // DCO.
  flk::section(c, 312, 46, 276, 166, "DCO");
  flk::vsl(c, DcLfo1ToDco1, 318, 93, 23, 96, "LFO");
  flk::vsl(c, DcDco1Pw, 345, 93, 23, 96, "PWM");
  static const char* pwSrc[3] = {"LFO", "MAN", "ENV"};
  flk::sw(c, DcDco1PwSrc, 375, 120, 37, 48, pwSrc, 3);
  waveButtons(c, 416, 128, DcDco1Pulse, DcDco1Saw, DcDco1Sub);
  flk::vsl(c, DcDco1SubLevel, 527, 93, 25, 96, "SUB");
  flk::vsl(c, DcNoise, 557, 93, 23, 96, "NOISE");
  // HPF / VCF.
  flk::section(c, 592, 46, 34, 166, "HPF");
  flk::vsl(c, DcHpf, 598, 93, 23, 96, "FREQ");
  flk::section(c, 630, 46, 182, 166, "VCF");
  flk::vsl(c, DcCutoff, 639, 93, 23, 96, "FREQ");
  flk::vsl(c, DcReso, 666, 93, 23, 96, "RES");
  static const char* pol[2] = {"+", "-"};
  flk::sw(c, DcEnvPolarity, 696, 120, 22, 33, pol, 2);
  flk::vsl(c, DcEnv1ToCut, 723, 93, 23, 96, "ENV");
  flk::vsl(c, DcLfo1ToCut, 750, 93, 23, 96, "LFO");
  flk::vsl(c, DcKeyFollow, 778, 93, 23, 96, "KYBD");
  // VCA + envelope.
  flk::section(c, 816, 46, 48, 166, "VCA");
  {
    const PluginState* sp = t.state(a.doc.song());
    const bool gate = sp && val(*sp, DcVcaSrc) >= 0.5f;
    if (flk::btn(c, "##vcaenv", 819, 120, 37, 16, "ENV", !gate) && sp && gate) {
      PluginState s = *sp;
      s.params[DcVcaSrc] = 0;
      setPluginState(a, t, s, "VCA source");
    }
    if (flk::btn(c, "##vcagate", 819, 137, 37, 16, "GATE", gate) && sp && !gate) {
      PluginState s = *sp;
      s.params[DcVcaSrc] = 2;
      setPluginState(a, t, s, "VCA source");
    }
  }
  flk::section(c, 866, 46, 118, 166, "ENV");
  static const char* adsr[4] = {"A", "D", "S", "R"};
  for (int i = 0; i < 4; ++i) flk::vsl(c, DcEnv1A + i, 869 + 27.7f * i, 93, 23, 96, adsr[i]);
  flk::section(c, 988, 46, 86, 166, "CHORUS");
  static const char* ch[4] = {"OFF", "I", "II", "I+II"};
  flk::sw(c, DcChorus, 993, 87, 72, 81, ch, 4);
  F.end(kW, kH);
}

}  // namespace

void drawDcoSynthEditor(App& a, const PluginTarget& t, float W) {
  const PluginState* sp = t.state(a.doc.song());
  if (!sp) return;
  const PluginState st = *sp;
  ImGui::PushID("gl.dcosynth");
  // Panel switch above the form.
  {
    const bool one = ival(st, DcPanel) == PanelOne;
    if (ImGui::SmallButton(one ? i18n::tr("Panel: one DCO (classic)") : i18n::tr("Panel: two DCO"))) {
      PluginState s = st;
      s.params[DcPanel] = one ? 0.f : 1.f;
      setPluginState(a, t, s, "DCO Synth panel");
    }
  }
  if (ival(st, DcPanel) == PanelOne) drawPanelOne(a, t, W);
  else drawPanelTwo(a, t, st, W);
  ImGui::PopID();
}

}  // namespace gl::packs::synths
