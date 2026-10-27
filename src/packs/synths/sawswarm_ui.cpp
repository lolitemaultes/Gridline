// Gridline — "synths" pack, J-22: Saw Swarm editor on the reference super-saw
// synth's 897 × 622 grid and palette (warm grey panel, silver / white wheels,
// slate switch wells, near-black combos, olive digit boxes): master row
// (level, pan, amp ADSR, transpose / fine, glide switches + time, mono,
// octaver, unison restart / voices / detune / spread); oscillator + filter +
// LFO row; sub-saw + filter envelope + LFO switches row; effects row (chorus,
// phaser, delay, reverb); the 6-slot MIDI matrix and the arpeggiator.
#include <cstdio>
#include <string>
#include "../../app/App.h"
#include "../../app/InstrumentSkin.h"
#include "va.h"
#include "va_ui.h"

namespace gl::packs::synths::va {

namespace {

using iskin::rgb;
constexpr float kW = 897, kH = 622;

iskin::Palette swarmPalette() {
  iskin::Palette p;
  p.bgTop = rgb(0x45413E);
  p.bgBottom = rgb(0x3B3836);
  p.panel = rgb(0x272C2E);
  p.panelLine = rgb(0x1B1918);
  p.rail = rgb(0x5D5955);
  p.fill = rgb(0xE0E0E0);
  p.fill2 = rgb(0xC7C7C7);
  p.fill3 = rgb(0x8D989E);
  p.cap = rgb(0x4A4643);
  p.handle = rgb(0x212121);
  p.label = rgb(0xB9B4AE);
  p.text = rgb(0xEDE9E4);
  p.textDim = rgb(0x948F89);
  p.lcdBg = rgb(0x585135);
  p.lcdText = rgb(0xF1E7B4);
  p.led = rgb(0xF2F2F2);
  p.ledOff = rgb(0x2A2826);
  return p;
}

// Two-position switch well (toggle): the lit half shows the state, the
// state names show in the hint bar.
void sw2(App& a, const PluginTarget& t, const iskin::Skin& k, int p, float x, float y, const char* on, const char* off, const char* cap) {
  const ImU32 lit = k.c.led, well = k.c.panel, edge = k.c.panelLine;
  iskin::switchBtn(a, t, k, p, x, y, 19, 54, [=](ImDrawList* dl, ImVec2 p0, ImVec2 p1, bool v) {
    dl->AddRectFilled(p0, p1, well, 3);
    dl->AddRect(p0, p1, edge, 3);
    const float mid = (p0.y + p1.y) * 0.5f, in = (p1.x - p0.x) * 0.2f;
    if (v)
      dl->AddRectFilled(ImVec2(p0.x + in, p0.y + in), ImVec2(p1.x - in, mid - 1), lit, 2);
    else
      dl->AddRectFilled(ImVec2(p0.x + in, mid + 1), ImVec2(p1.x - in, p1.y - in), iskin::lerp(lit, well, 0.55f), 2);
  });
  caption(k, x - 16, y - 12, 51, cap);
  if (ImGui::IsItemHovered()) a.setHint(std::string(cap) + ": " + on + " / " + off);
}

void combo(App& a, const PluginTarget& t, const iskin::Skin& k, int p, float x, float y, float w, float h) {
  skin::menuBox(a, t, p, k.at(x, y), ImVec2(w * k.s, h * k.s), k.shared(), 9.0f * std::max(1.0f, k.s * 0.9f));
}

}  // namespace

void drawSawSwarmEditor(App& a, const PluginTarget& t, float width) {
  const PluginState* sp = t.state(a.doc.song());
  if (!sp) return;
  const PluginState st = *sp;
  iskin::Skin k = iskin::begin(width, kW, kH, swarmPalette());
  ImGui::PushID("gl.sawswarm");
  iskin::background(k, 0, 0, kW, kH, k.c.bgTop, k.c.bgBottom, 4);
  const ImU32 F = k.c.fill, F2 = k.c.fill2;
  auto W = [&](int p, float x, float y, float s, const char* cap, bool c = false) { wheelCap(a, t, k, p, x, y, s, F2, cap, c); };
  auto S = [&](int p, float x, float y, float h, const char* cap, bool c = false) { sliderCap(a, t, k, p, x, y, 22, h, F, cap, c); };
  auto C = [&](int p, float x, float y, const char* cap) { toggleCap(a, t, k, p, x, y, 30, 28, cap); };

  // Title / top strip.
  if (!pui::titleWordmarkIn(k.dl(), "gl.sawswarm", k.at(26, 10), k.at(26 + 200, 10 + 26), k.px(17), 0, k.c.text))  // J-136: the wordmark
    iskin::text(k, 26, 10, 200, 26, "SAW SWARM", 17, k.c.text, 0, false, true);
  {
    std::string name = dataLine(st.data, "name");
    iskin::bevel(k, 230, 12, 300, 22, rgb(0x171717), k.c.panelLine, 3);
    iskin::text(k, 238, 12, 290, 22, name.empty() ? "super-saw synthesizer" : name.c_str(), 11, k.c.textDim);
  }
  liveScopeAt(a, t, k, 545, 10, 196, 26);
  iskin::text(k, 750, 2, 70, 12, "POLY", 8, k.c.label, 1);
  combo(a, t, k, SwPoly, 775, 15, 47, 19);
  iskin::text(k, 820, 2, 60, 12, "BEND", 8, k.c.label, 1);
  combo(a, t, k, SwBendRange, 825, 15, 47, 19);

  // ---- master row
  panel(k, 20, 48, 860, 116, "MASTER");
  W(SwLevel, 35, 81, 52, "LEVEL");
  W(SwPan, 99, 81, 52, "PAN", true);
  S(SwAmpA, 175, 67, 86, "A");
  S(SwAmpD, 205, 67, 86, "D");
  S(SwAmpS, 235, 67, 86, "S");
  S(SwAmpR, 265, 67, 86, "R");
  sliderCap(a, t, k, SwTranspose, 322, 67, 19, 86, F, "TRANSP", true);
  sliderCap(a, t, k, SwFine, 368, 67, 19, 86, F, "FINE", true);
  sw2(a, t, k, SwGlideStatic, 413, 76, "STAT", "VARI", "TIME");
  sw2(a, t, k, SwGlideHeld, 450, 76, "HELD", "BEND", "KEYS");
  W(SwGlide, 490, 81, 52, "GLIDE");
  sw2(a, t, k, SwMono, 564, 76, "ON", "OFF", "MONO");
  sw2(a, t, k, SwOctaver, 600, 76, "ON", "OFF", "OCTAVE");
  sw2(a, t, k, SwUniRetrig, 637, 76, "ON", "OFF", "RETRIG");
  W(SwUnison, 692, 85, 46, "UNISON");
  W(SwUniDetune, 750, 81, 52, "DETUNE");
  W(SwUniPan, 811, 81, 52, "PAN");

  // ---- oscillator / filter / LFO
  panel(k, 20, 170, 268, 92, "OSCILLATOR");
  W(SwNoise, 35, 197, 52, "NOISE");
  W(SwSyncFreq, 99, 197, 52, "SYNC FREQ");
  C(SwSyncOn, 156, 193, "SYNC");  // J-128: clear of the SYNC FREQ wheel
  sw2(a, t, k, SwSubRetrig, 195, 207, "ON", "OFF", "RETRIG");
  sw2(a, t, k, SwSubInvert, 233, 207, "ON", "OFF", "INVERT");
  sw2(a, t, k, SwSubSquare, 268, 207, "ON", "OFF", "SQUARE");
  panel(k, 296, 170, 316, 92, "FILTER");
  W(SwCutoff, 308, 197, 52, "CUTOFF");
  W(SwReso, 368, 197, 52, "RESO");
  W(SwKeyTrk, 429, 197, 52, "KBD TRK");
  W(SwVelTrk, 490, 197, 52, "VEL TRK");
  W(SwFltMode, 562, 201, 44, "MODE");
  {
    static const char* fm[4] = {"LP24", "LP12", "BP", "HP"};
    iskin::text(k, 556, 182, 56, 12, fm[std::clamp(static_cast<int>(st.params.size() > SwFltMode ? st.params[SwFltMode] : 0), 0, 3)], 9,
                k.c.text, 1, true);
  }
  panel(k, 620, 170, 260, 92, "LFO");
  W(SwLfoSpeed, 629, 197, 52, "SPEED");
  W(SwLfoAttack, 689, 197, 52, "ATTACK");
  W(SwLfoRelease, 750, 197, 52, "RELEASE");
  W(SwLfoAmt, 811, 197, 52, "AMOUNT");

  // ---- sub-saw / filter envelope / LFO options
  panel(k, 20, 270, 274, 106, "SUB-SAW");
  W(SwSubLevel, 36, 294, 52, "LEVEL");
  W(SwSubPhase, 99, 294, 52, "PHASE");
  W(SwSubOct, 175, 301, 44, "OCTAVE", true);
  W(SwSubDetune, 239, 294, 52, "DETUNE");
  panel(k, 302, 270, 304, 106, "FILTER ENVELOPE");
  S(SwEgA, 313, 289, 76, "A");
  S(SwEgD, 345, 289, 76, "D");
  S(SwEgS, 378, 289, 76, "S");
  S(SwEgR, 411, 289, 76, "R");
  S(SwEgAmt, 454, 289, 76, "AMOUNT");
  W(SwEgDest, 530, 303, 44, "TARGET");
  {
    static const char* ed[4] = {"CUTOFF", "SYNC", "SUB PH", "PITCH"};
    iskin::text(k, 506, 286, 92, 12, ed[std::clamp(static_cast<int>(st.params.size() > SwEgDest ? st.params[SwEgDest] : 0), 0, 3)], 8,
                k.c.text, 1, true);
  }
  panel(k, 620, 270, 260, 106, "");
  sw2(a, t, k, SwLfoRetrig, 633, 291, "ON", "OFF", "RETRIG");
  sw2(a, t, k, SwLfoSync, 669, 291, "ON", "OFF", "SYNC");
  C(SwLfoInvert, 697, 270, "INV");
  W(SwLfoShape, 717, 303, 44, "SHAPE");
  W(SwLfoDest, 805, 303, 44, "TARGET");
  {
    static const char* sh[4] = {"TRI", "SAW", "SQR", "RND"};
    static const char* ld[7] = {"CUTOFF", "SYNC", "SUB PH", "PITCH", "NOISE", "LEVEL", "PAN"};
    const int s = std::clamp(static_cast<int>(st.params.size() > SwLfoShape ? st.params[SwLfoShape] : 0), 0, 3);
    const int d = std::clamp(static_cast<int>(st.params.size() > SwLfoDest ? st.params[SwLfoDest] : 0), 0, 6);
    iskin::text(k, 760, 290, 44, 12, sh[s], 8, k.c.text, 1, true);
    iskin::text(k, 800, 360, 60, 12, ld[d], 8, k.c.text, 1, true);
  }

  // ---- effects
  panel(k, 20, 412, 860, 104, "EFFECTS");
  C(SwChoOn, 22, 424, "CHORUS");
  const int ch[3] = {SwChoDepth, SwChoRate, SwChoMix};
  const char* chn[3] = {"DEPTH", "RATE", "MIX"};
  for (int i = 0; i < 3; ++i) W(ch[i], 32.f + 42 * i, 465, 36, chn[i]);
  C(SwPhOn, 162, 424, "PHASER");
  const int ph[5] = {SwPhCenter, SwPhRate, SwPhDepth, SwPhFb, SwPhMix};
  const char* phn[5] = {"CENTER", "RATE", "DEPTH", "FDBK", "MIX"};
  for (int i = 0; i < 5; ++i) W(ph[i], 172.f + 42 * i, 465, 36, phn[i]);
  C(SwDlyOn, 384, 424, "DELAY");
  const int dl[7] = {SwDlyL, SwDlyR, SwDlyFb, SwDlyDepth, SwDlyLoCut, SwDlyHiCut, SwDlyMix};
  const char* dln[7] = {"LEFT", "RIGHT", "FDBK", "DEPTH", "LO-CUT", "HI-CUT", "MIX"};
  for (int i = 0; i < 7; ++i) W(dl[i], 396.f + 42.2f * i, 465, 36, dln[i]);
  C(SwRevOn, 694, 424, "REVERB");
  const int rv[4] = {SwRevDecay, SwRevHiCut, SwRevDamp, SwRevMix};
  const char* rvn[4] = {"DECAY", "HI-CUT", "DAMP", "MIX"};
  for (int i = 0; i < 4; ++i) W(rv[i], 705.f + 42.3f * i, 465, 36, rvn[i]);

  // ---- MIDI modulation matrix + arpeggiator
  panel(k, 20, 520, 640, 80, "MIDI MODULATION MATRIX");
  for (int m = 0; m < kSwModSlots; ++m) {
    const float x0 = (m / 2 == 0) ? 34.f : (m / 2 == 1) ? 246.f : 457.f;
    const float y0 = (m % 2 == 0) ? 536.f : 566.f;
    ImGui::PushID(m);
    combo(a, t, k, SwModSrc1 + m, x0, y0, 60, 22);
    iskin::wheel(a, t, k, SwModAmt1 + m, x0 + 68, y0 - 3, 28, k.c.fill3, iskin::WheelCentered);  // J-128: centred on the combos, rows 30 apart
    combo(a, t, k, SwModDest1 + m, x0 + 104, y0, 90, 22);
    ImGui::PopID();
  }
  panel(k, 668, 520, 212, 80, "ARPEGGIATOR");
  W(SwArpMode, 682, 547, 44, "MODE");
  C(SwArpOn, 758, 525, "PLAY");
  combo(a, t, k, SwArpSync, 750, 566, 49, 22);
  W(SwArpRange, 814, 547, 44, "RANGE");
  iskin::slider(a, t, k, SwModWheel, 868, 530, 9, 64, F, false);
  iskin::end(k, kH);
  ImGui::PopID();
}

}  // namespace gl::packs::synths::va
