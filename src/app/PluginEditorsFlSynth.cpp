// Gridline — J-19 reference-family editors for the built-in instruments, on
// their reference panels' grids and palettes (PluginSkin kit).
//   Kick            → kick synth panel (380 × 80 row of pitch / amp / click / dist bevels, extended)
//   Snare/Hat/Clap  → drum-synth panel (580 × 358: wave · trigger · osc 1 · osc 2 · main out · keyboard)
//   Pluck           → string panel (360 × 156: two big wheels + switch pads, extended)
//   Acid Bass       → bass-line panel (700 wide: main wheel row, tweaks / distortion, 16-step sequencer)
// FM Synth, Sampler, Granular and MIDI Out live in PluginEditorsFlSynth2.cpp.
#include "PluginEditorsFlSynth.h"

namespace gl {

using namespace pp;

bool drawFlSynthEditor2(App& a, const PluginTarget& t, float width);  // PluginEditorsFlSynth2.cpp

namespace {

using namespace ed;
using namespace flsyn;

// ======================================================================= Kick
// Reference: one 380 × 80 strip — a pitch bevel (max / min / decay) and three
// single-wheel bevels (amp decay, click, distortion). The extra controls sit
// in two more rows of the same bevels underneath.
const skin::Skin& kickSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x454E54, 0x3B4348, 0x3F484D, 0xFFA830, 0x3F484D, 0xB9BFC3);
    s.graphCurve = skin::hex(0xFFC56B);
    return s;
  }();
  return k;
}

void editorKickFl(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  const skin::Skin& K = kickSkin();
  constexpr float kW = 380, kH = 230;
  skin::Form F = fitForm(width, kW, kH, 440);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  // Bevels: the pitch block on the left, three single-wheel columns on the right.
  for (int r = 0; r < 3; ++r) {
    float y = 5 + r * 75.0f;
    skin::well(dl, F.p(5, y), F.p(183, y + 70), K, 3 * F.s);
    for (float x : {188.0f, 252.0f, 316.0f}) skin::well(dl, F.p(x, y), F.p(x + 59, y + 70), K, 3 * F.s);
  }
  auto w1 = [&](int p, float x, float y, const char* l) { wheelL(a, t, p, F, x, y, 40, K, K, l); };
  // Row 1 (reference strip).
  w1(KickPitch, 17, 13, "MAX");
  w1(KickEndPitch, 74, 13, "MIN");
  w1(KickSweep, 132, 13, "DECAY");
  w1(KickDecay, 198, 12, "AMP DECAY");
  w1(KickClick, 261, 12, "CLICK");
  w1(KickDrive, 325, 12, "DIST");
  // Row 2: sweep curve, sub, attack · noise · click tone · distortion tone.
  w1(KickSweepCurve, 17, 88, "CURVE");
  w1(KickSub, 74, 88, "SUB");
  w1(KickAttack, 132, 88, "ATTACK");
  w1(KickNoise, 198, 87, "NOISE");
  w1(KickClickTone, 261, 87, "CLK TONE");
  w1(KickTone, 325, 87, "TONE");
  // Row 3: variation + key tracking + preview · noise decay · click length · volume.
  w1(KickVariation, 12, 163, "VARIATION");
  skin::check(a, t, KickKeyTrack, F.p(60, 160), F.sz(60, 12), K, "KEY TRK");
  waveWell(dl, F.p(60, 175), F.p(178, 220), K, renderPreview(t, st, std::min(1.2, val(st, KickDecay, 0.45f) * 2.2 + 0.05)));
  w1(KickNoiseDecay, 198, 162, "N. DECAY");
  w1(KickClickLen, 261, 162, "CLK LEN");
  w1(KickVolume, 325, 162, "VOLUME");
  endForm(F, width, kH);
}

// ======================================================================= Drum synth panel (Snare / Hat / Clap)
// Reference: 580 × 358 — WAVE (top left), TRIG (left), OSC 1 / OSC 2 strips
// (centre: two rail sliders + a wheel row each), MAIN OUT column (right) and
// a keyboard strip along the bottom. Light-capped wheels with a dark red ring.
const skin::Skin& drumSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x404748, 0x4A5253, 0x3F484D, 0xDFDFDF, 0x3F484D, 0xB9BFC3);
    s.pillBg = skin::hex(0x404847);
    s.pillText = skin::hex(0xA2B0B7);
    s.graphCurve = skin::hex(0xD8DCDD);
    s.handle = skin::hex(0x616369);
    return s;
  }();
  return k;
}
const skin::Skin& drumWheelSkin() {  // light cap, white rail, dark red value
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x404748, 0x4A5253, 0xD6D8D9, 0x9A2A2A, 0xC6C7C8, 0x2A2F31);
    s.fillPressed = skin::hex(0xAE0000);
    return s;
  }();
  return k;
}

struct DrumSlider {
  int param;
  const char* label;
};
struct DrumWheel {
  int param;
  const char* label;
  float x;
};

// Osc strip: two labelled rail sliders and a row of wheels.
void drumOsc(App& a, const PluginTarget& t, const skin::Form& F, float y, float h, const char* caption, DrumSlider s1, DrumSlider s2,
             std::initializer_list<DrumWheel> wheels, float wheelY) {
  const skin::Skin& K = drumSkin();
  const skin::Skin& Kw = drumWheelSkin();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::well(dl, F.p(149, y), F.p(465, y + h), K, 4 * F.s);
  pillAt(dl, F, 405, y + 3, 55, 13, K, caption);
  const DrumSlider sl[2] = {s1, s2};
  for (int i = 0; i < 2; ++i) {
    if (sl[i].param < 0) continue;
    float sy = y + 20 + i * 20;
    skin::text(dl, F.p(154, sy + 1), K.textDim, sl[i].label, lf(F, 7.0f));
    dl->AddRectFilled(F.p(185, sy - 1), F.p(404, sy + 16), skin::darken(K.panel, 0.7f), 2);
    skin::hslider(a, t, sl[i].param, F.p(186, sy), F.sz(217, 14), K, skin::hex(0x9A2A2A));
    if (auto d = describeParam(a.doc.song(), t.pid(sl[i].param))) {
      std::string v = d->format(pui::paramValue(a, t, sl[i].param));
      skin::lcd(dl, F.p(407, sy - 1), F.p(460, sy + 16), K, v.c_str(), lf(F, 7.0f));
    }
  }
  for (const DrumWheel& w : wheels) wheelL(a, t, w.param, F, w.x, wheelY, 38, K, Kw, w.label);
}

// Common frame: panel, bevels, main out (pan / volume), preview + keyboard.
// `body` draws the regions that differ per drum.
void drumFrame(App& a, const PluginTarget& t, float width, int pan, int vol, double previewSec, const char* waveCap, const char* trigCap,
               const std::function<void(const skin::Form&)>& body) {
  const PluginState st = *t.state(a.doc.song());
  const skin::Skin& K = drumSkin();
  const skin::Skin& Kw = drumWheelSkin();
  constexpr float kW = 580, kH = 358;
  skin::Form F = fitForm(width, kW, kH, 430);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  skin::well(dl, F.p(5, 5), F.p(145, 105), K, 4 * F.s);
  skin::well(dl, F.p(5, 109), F.p(145, 260), K, 4 * F.s);
  skin::well(dl, F.p(469, 5), F.p(575, 260), K, 4 * F.s);
  skin::well(dl, F.p(5, 264), F.p(575, 354), K, 4 * F.s);
  pillAt(dl, F, 90, 8, 52, 13, K, waveCap);
  pillAt(dl, F, 90, 112, 52, 13, K, trigCap);
  pillAt(dl, F, 474, 8, 96, 13, K, "MAIN OUT");
  wheelL(a, t, pan, F, 475, 37, 40, K, Kw, "PAN", skin::WheelBigLine | skin::WheelCentered);
  wheelL(a, t, vol, F, 530, 37, 40, K, Kw, "VOL");
  pillAt(dl, F, 474, 96, 96, 13, K, "PREVIEW");
  waveWell(dl, F.p(474, 113), F.p(570, 255), K, renderPreview(t, st, previewSec));
  body(F);
  auditionKeyboard(a, t, F, 10, 269, 560, 80, 36, 28, K);
  endForm(F, width, kH);
}

void editorSnareFl(App& a, const PluginTarget& t, float width) {
  drumFrame(a, t, width, SnPan, SnVolume, 0.5, "CLICK", "VELOCITY", [&](const skin::Form& F) {
    const skin::Skin& K = drumSkin();
    const skin::Skin& Kw = drumWheelSkin();
    wheelL(a, t, SnClick, F, 20, 40, 38, K, Kw, "CLICK");
    wheelL(a, t, SnTilt, F, 84, 40, 38, K, Kw, "TILT", skin::WheelBigLine | skin::WheelCentered);
    wheelL(a, t, SnVelTone, F, 56, 150, 38, K, Kw, "VEL > TONE");
    drumOsc(a, t, F, 5, 128, "BODY", {SnTune, "TUNE"}, {SnSweep, "SWEEP"},
            {{SnHarmonics, "HARM", 198}, {SnDecay, "DEC", 243}, {SnSweepTime, "SWP T", 288}, {SnTone, "TONE", 377}}, 79);
    drumOsc(a, t, F, 136, 124, "WIRES", {SnNoiseBand, "BAND"}, {SnNoiseWidth, "WIDTH"},
            {{SnRing, "RING", 154}, {SnNoiseDecay, "DEC", 243}, {SnSnappy, "SNAPPY", 422}}, 202);
  });
}

void editorHatFl(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  drumFrame(a, t, width, HatPan, HatVolume, bval(st, HatOpen) ? 0.8 : 0.3, "MODE", "CHOKE", [&](const skin::Form& F) {
    const skin::Skin& K = drumSkin();
    skin::check(a, t, HatOpen, F.p(14, 34), F.sz(80, 16), K, "OPEN HAT");
    skin::text(ImGui::GetWindowDrawList(), F.p(14, 62), K.textDim, "Open hats ring;", lf(F, 7.0f));
    skin::text(ImGui::GetWindowDrawList(), F.p(14, 74), K.textDim, "closed are short.", lf(F, 7.0f));
    static const char* ch[2] = {"NO CHOKE", "CHOKE"};
    skin::switchSel(a, t, HatChoke, F.p(14, 140), F.sz(122, 40), K, ch, 2);
    drumOsc(a, t, F, 5, 128, "METAL", {HatBase, "BASE"}, {HatMetal, "METAL"},
            {{HatClick, "CLICK", 198}, {HatAttack, "ATT", 243}, {HatDecay, "DEC", 288}}, 79);
    drumOsc(a, t, F, 136, 124, "FILTER", {HatTone, "TONE"}, {HatWidth, "WIDTH"}, {}, 0);
  });
}

void editorClapFl(App& a, const PluginTarget& t, float width) {
  drumFrame(a, t, width, ClapPan, ClapVolume, 0.6, "FEEL", "SPACE", [&](const skin::Form& F) {
    const skin::Skin& K = drumSkin();
    const skin::Skin& Kw = drumWheelSkin();
    wheelL(a, t, ClapHumanize, F, 56, 40, 38, K, Kw, "HUMANIZE");
    wheelL(a, t, ClapStereo, F, 56, 150, 38, K, Kw, "STEREO");
    drumOsc(a, t, F, 5, 128, "HITS", {ClapHits, "HITS"}, {ClapSpread, "SPACE"},
            {{ClapHitDecay, "HIT DEC", 198}, {ClapFalloff, "FALLOFF", 243}}, 79);
    drumOsc(a, t, F, 136, 124, "TAIL", {ClapTone, "TONE"}, {ClapWidth, "WIDTH"}, {{ClapDecay, "DEC", 243}}, 202);
  });
}

// ======================================================================= Pluck
// Reference: 360 × 156 warm panel — DECAY and COLOR big wheels, NORM / GATE
// pads top right, WIDEN bottom right. Extra controls in a strip underneath.
const skin::Skin& pluckSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0xF9B352, 0xEFA33E, 0x3F484D, 0xFFFFFF, 0x3F484D, 0x6E4300);
    s.textDim = skin::hex(0x845100);
    s.pillBg = skin::hex(0xFFCE83);
    s.pillText = skin::hex(0x845100);
    s.lcdBg = skin::hex(0x3F484D);
    s.lcdText = skin::hex(0xFFE2B0);
    s.graphBg = skin::hex(0x3F484D);
    s.graphGrid = skin::hex(0x56606A);
    s.graphCurve = skin::hex(0xFFD08A);
    s.select = skin::hex(0x3F484D);
    s.panelLo = skin::hex(0xE39A3A);
    return s;
  }();
  return k;
}
const skin::Skin& pluckWheelSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = pluckSkin();
    s.text = skin::hex(0xE8ECEE);  // light indicator line on the dark cap
    s.fillPressed = skin::hex(0xFFA830);
    return s;
  }();
  return k;
}

// Reference "pad" switch on the warm panel: lit (deeper orange, dark text) when on.
void padToggle(App& a, const PluginTarget& t, int param, ImVec2 tl, ImVec2 size, const char* label, float font) {
  const bool on = pui::paramValue(a, t, param) >= 0.5;
  ImGui::PushID(param + 88000);
  ImGui::SetCursorScreenPos(tl);
  const bool pressed = ImGui::InvisibleButton("##pad", size);
  const bool hov = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p1(tl.x + size.x, tl.y + size.y);
  ImU32 bg = on ? skin::hex(0xE8892A) : skin::hex(0xFFCE83);
  if (hov) bg = skin::lighten(bg, 0.08f);
  dl->AddRectFilled(ImVec2(tl.x, tl.y + 1), ImVec2(p1.x, p1.y + 1), skin::hex(0xC77A1E), 4);
  dl->AddRectFilled(tl, p1, bg, 4);
  if (on) dl->AddRectFilled(ImVec2(tl.x + 5, (tl.y + p1.y) * 0.5f - 2), ImVec2(tl.x + 9, (tl.y + p1.y) * 0.5f + 2), skin::hex(0xFFF3D6), 1);
  ImGui::PushFont(a.fontMonoBold, font);
  ImVec2 ts = ImGui::CalcTextSize(label);
  dl->AddText(ImVec2(std::round((tl.x + p1.x - ts.x) * 0.5f), std::round((tl.y + p1.y - ts.y) * 0.5f)), on ? skin::hex(0x3A2300) : skin::hex(0x845100),
              label);
  ImGui::PopFont();
  if (hov) {
    auto d = describeParam(a.doc.song(), t.pid(param));
    if (d) a.setHint(d->name + ": " + (on ? "on" : "off") + "   (click to toggle)");
  }
  if (pressed) a.doc.setParam(t.pid(param), on ? 0 : 1, false);
  ImGui::PopID();
}

void editorPluckFl(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  const skin::Skin& K = pluckSkin();
  const skin::Skin& Kw = pluckWheelSkin();
  constexpr float kW = 360, kH = 236;
  skin::Form F = fitForm(width, kW, kH, 420);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  pillAt(dl, F, 4, 13, 60, 16, K, "DECAY");
  pillAt(dl, F, 143, 34, 50, 16, K, "COLOR");
  skin::wheel(a, t, PlkDecay, F.p(58, 25), 88 * F.s, Kw, skin::WheelBigLine);
  skin::wheel(a, t, PlkBrightness, F.p(178, 45), 88 * F.s, Kw, skin::WheelBigLine);
  {
    std::string d = describeParam(a.doc.song(), t.pid(PlkDecay))->format(val(st, PlkDecay, 2.5f));
    skin::lcd(dl, F.p(64, 116), F.p(140, 130), K, d.c_str(), lf(F, 8));
  }
  padToggle(a, t, PlkNormalize, F.p(301, 5), F.sz(55, 25), "NORM", lf(F, 8.5f));
  padToggle(a, t, PlkGate, F.p(301, 32), F.sz(55, 25), "GATE", lf(F, 8.5f));
  pillAt(dl, F, 301, 127, 55, 22, K, "WIDEN");
  skin::wheel(a, t, PlkWidth, F.p(306, 72), 46 * F.s, Kw, skin::WheelBigLine);
  // Extension strip: string details + preview.
  skin::well(dl, F.p(4, 160), F.p(356, 232), K, 4 * F.s);
  auto w = [&](int p, float x, const char* l) { wheelL(a, t, p, F, x, 166, 40, K, Kw, l); };
  w(PlkDamping, 12, "DAMPING");
  w(PlkPosition, 60, "PICK POS");
  w(PlkRelease, 108, "RELEASE");
  w(PlkVolume, 156, "VOLUME");
  waveWell(dl, F.p(206, 166), F.p(350, 226), K, renderPreview(t, st, std::min(3.0, static_cast<double>(val(st, PlkDecay, 2.5f)))));
  endForm(F, width, kH);
}

// ======================================================================= Acid Bass
// Reference: 700 wide blue-grey panel — main wheel row (tuning, wave switch,
// waveform, cutoff, reso, env mod, decay, accent, big volume), tweaks row and
// distortion block, then the 16-step sequencer (pitch digits, gate / slide /
// accent buttons, step LEDs). The reference's delay / reverb rows have no
// counterpart here and are left out.
const skin::Skin& acidSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x436370, 0x3B5865, 0xCCC8CA, 0x676366, 0xDFDFE6, 0xD3D8DA);
    s.textDim = skin::hex(0xC0CBD0);
    s.pillBg = skin::hex(0x31373B);
    s.pillText = skin::hex(0xCACBBC);
    s.lcdBg = skin::hex(0x26343B);
    s.lcdText = skin::hex(0xE8EEF0);
    s.select = skin::hex(0xF0856A);
    s.handle = skin::hex(0xB9B9B9);
    return s;
  }();
  return k;
}
const skin::Skin& acidDigiSkin() {  // light digit wells with dark digits
  static const skin::Skin k = [] {
    skin::Skin s = acidSkin();
    s.lcdBg = skin::hex(0xC5D0D6);
    s.lcdText = skin::hex(0x1F282D);
    return s;
  }();
  return k;
}
const skin::Skin& acidWheelSkin() {  // light caps, dark indicator
  static const skin::Skin k = [] {
    skin::Skin s = acidSkin();
    s.text = skin::hex(0x2B3034);
    s.fill = skin::hex(0x5E5A5D);
    s.fillPressed = skin::hex(0x8E8A8D);
    return s;
  }();
  return k;
}

void editorAcidFl(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  const skin::Skin& K = acidSkin();
  const skin::Skin& Kw = acidWheelSkin();
  const skin::Skin& Kd = acidDigiSkin();
  constexpr float kW = 700, kH = 432;
  skin::Form F = fitForm(width, kW, kH, 460);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  // ---- main row
  skin::well(dl, F.p(8, 8), F.p(692, 150), K, 4 * F.s);
  auto big = [&](int p, float x, const char* l) { wheelL(a, t, p, F, x, 40, 56, K, Kw, l); };
  big(AcTuning, 24, "TUNING");
  {
    static const char* wv[2] = {"SAW", "SQR"};
    skin::switchSel(a, t, AcWave, F.p(84, 40), F.sz(40, 56), K, wv, 2);
  }
  big(AcBlend, 128, "WAVEFORM");
  big(AcCutoff, 200, "CUTOFF");
  big(AcReso, 272, "RESO");
  big(AcEnvMod, 343, "ENV MOD");
  big(AcDecay, 414, "DECAY");
  big(AcAccent, 486, "ACCENT");
  wheelL(a, t, AcVolume, F, 598, 34, 70, K, Kw, "VOLUME");
  skin::check(a, t, AcVintage, F.p(20, 122), F.sz(110, 14), K, "VINTAGE SQUARE");
  pillAt(dl, F, 14, 13, 70, 14, K, "BASS LINE");
  // ---- tweaks · distortion · slide
  skin::well(dl, F.p(8, 156), F.p(398, 248), K, 4 * F.s);
  pillAt(dl, F, 14, 160, 60, 13, K, "TWEAKS");
  auto tw = [&](int p, float x, const char* l) { wheelL(a, t, p, F, x, 178, 40, K, Kw, l); };
  tw(AcHighPass, 18, "HP");
  tw(AcAccentDecay, 72, "ACC DEC");
  tw(AcKeyFollow, 126, "KEY FLW");
  tw(AcPwmDepth, 180, "PWM");
  tw(AcPwmRate, 234, "PWM RATE");
  tw(AcPulseWidth, 288, "PW");
  tw(AcClicks, 342, "CLICK");
  skin::well(dl, F.p(404, 156), F.p(548, 248), K, 4 * F.s);
  pillAt(dl, F, 410, 160, 80, 13, K, "DISTORTION");
  tw(AcDrive, 422, "DRIVE");
  tw(AcDriveTone, 486, "TONE");
  skin::well(dl, F.p(554, 156), F.p(692, 248), K, 4 * F.s);
  pillAt(dl, F, 560, 160, 60, 13, K, "GLIDE");
  tw(AcSlide, 568, "SLIDE");
  tw(AcAccentThresh, 630, "ACC AT");
  // ---- sequencer
  skin::well(dl, F.p(8, 254), F.p(692, 426), K, 4 * F.s);
  skin::check(a, t, AcSeqOn, F.p(17, 262), F.sz(80, 16), K, "SEQUENCER");
  skin::text(dl, F.p(220, 264), K.textDim, "STEPS", lf(F, 7.5f));
  skin::digi(a, t, AcSeqLength, F.p(256, 260), F.sz(34, 20), Kd);
  skin::text(dl, F.p(306, 264), K.textDim, "SHUFFLE", lf(F, 7.5f));
  skin::digi(a, t, AcSeqShuffle, F.p(352, 260), F.sz(50, 20), Kd);
  skin::text(dl, F.p(420, 264), K.textDim, "GATE", lf(F, 7.5f));
  skin::digi(a, t, AcSeqGate, F.p(450, 260), F.sz(50, 20), Kd);
  if (skin::button("##acclr", F.p(600, 260), F.sz(40, 20), K, "CLEAR", false, lf(F, 7.5f))) {
    PluginState ns = st;
    for (int k = 0; k < kAcSteps; ++k) {
      ns.params[static_cast<size_t>(acStep(k, AcStepGate))] = 0;
      ns.params[static_cast<size_t>(acStep(k, AcStepAccent))] = 0;
      ns.params[static_cast<size_t>(acStep(k, AcStepSlide))] = 0;
    }
    setPluginState(a, t, ns, "clear sequence");
  }
  if (ImGui::IsItemHovered()) a.setHint("Clear all gates, accents and slides");
  if (skin::button("##acrnd", F.p(644, 260), F.sz(40, 20), K, "RAND", false, lf(F, 7.5f))) {
    PluginState ns = st;
    uint32_t r = static_cast<uint32_t>(ImGui::GetFrameCount()) * 2654435761u + 7;
    auto rnd = [&r](int m) {
      r ^= r << 13, r ^= r >> 17, r ^= r << 5;
      return static_cast<int>(r % static_cast<uint32_t>(m));
    };
    static const int scale[7] = {0, 3, 5, 7, 10, 12, 0};
    for (int k = 0; k < kAcSteps; ++k) {
      ns.params[static_cast<size_t>(acStep(k, AcStepNote))] = static_cast<float>(scale[rnd(7)]);
      ns.params[static_cast<size_t>(acStep(k, AcStepOctave))] = static_cast<float>(rnd(5) == 0 ? 2 : rnd(6) == 0 ? 0 : 1);
      ns.params[static_cast<size_t>(acStep(k, AcStepGate))] = static_cast<float>(rnd(4) == 0 ? 0 : rnd(6) == 0 ? 2 : 1);
      ns.params[static_cast<size_t>(acStep(k, AcStepAccent))] = rnd(4) == 0 ? 1.f : 0.f;
      ns.params[static_cast<size_t>(acStep(k, AcStepSlide))] = rnd(5) == 0 ? 1.f : 0.f;
    }
    setPluginState(a, t, ns, "randomize sequence");
  }
  if (ImGui::IsItemHovered()) a.setHint("Randomize the sequence");
  // Step grid: rows PITCH (digits), OCT, GATE, SLIDE, ACCENT, then the step LEDs.
  const char* rows[5] = {"PITCH", "OCT", "GATE", "SLIDE", "ACCENT"};
  const float rowY[5] = {290, 311, 332, 353, 374};
  const float rowH[5] = {18, 18, 19, 19, 19};
  for (int r = 0; r < 5; ++r) skin::text(dl, F.p(14, rowY[r] + 3), K.pillText, rows[r], lf(F, 7.0f));
  const int len = ival(st, AcSeqLength, 16);
  int playStep = -1;
  if (a.engine.playing() && bval(st, AcSeqOn))
    playStep = static_cast<int>(std::fmod(a.engine.position() / std::max(1, a.doc.song().ppq) * 4.0, len));
  static const char* notes[13] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B", "C+"};
  static const char* octs[3] = {"-", "0", "+"};
  const ImU32 btnOff = skin::hex(0x455765), btnOn = skin::hex(0xD0D8DC), ledOn = skin::hex(0xF0856A);
  for (int k = 0; k < kAcSteps; ++k) {
    const float x = 61 + k * 36.0f;
    const bool inLen = k < len;
    ImGui::PushID(k + 700);
    for (int r = 0; r < 5; ++r) {
      ImVec2 q = F.p(x, rowY[r]), q1 = F.p(x + 31, rowY[r] + rowH[r]);
      ImGui::SetCursorScreenPos(q);
      ImGui::PushID(r);
      const int param = r == 0 ? acStep(k, AcStepNote) : r == 1 ? acStep(k, AcStepOctave) : r == 2 ? acStep(k, AcStepGate)
                        : r == 3                       ? acStep(k, AcStepSlide)
                                                       : acStep(k, AcStepAccent);
      const int v = ival(st, param);
      const bool click = ImGui::InvisibleButton("c", ImVec2(q1.x - q.x, q1.y - q.y));
      const bool hov = ImGui::IsItemHovered();
      ImGuiIO& io = ImGui::GetIO();
      if (r <= 1) {
        dl->AddRectFilled(q, q1, inLen ? (hov ? skin::lighten(Kd.lcdBg, 0.15f) : Kd.lcdBg) : skin::darken(Kd.lcdBg, 0.35f), 2);
        const char* txt = r == 0 ? notes[std::clamp(v, 0, 12)] : octs[std::clamp(v, 0, 2)];
        ImGui::PushFont(a.fontMonoBold, lf(F, 8.0f));
        ImVec2 ts = ImGui::CalcTextSize(txt);
        dl->AddText(ImVec2((q.x + q1.x - ts.x) * 0.5f, (q.y + q1.y - ts.y) * 0.5f), Kd.lcdText, txt);
        ImGui::PopFont();
      } else {
        bool on = r == 2 ? v >= 1 : v != 0;
        ImU32 bg = on ? (r == 2 && v == 2 ? skin::lighten(btnOff, 0.35f) : btnOn) : btnOff;
        if (!inLen) bg = skin::darken(bg, 0.3f);
        if (hov) bg = skin::lighten(bg, 0.1f);
        dl->AddRectFilled(q, q1, bg, 3);
        if (r == 2 && v == 2) {
          ImGui::PushFont(a.fontMonoBold, lf(F, 7.0f));
          ImVec2 ts = ImGui::CalcTextSize("TIE");
          dl->AddText(ImVec2((q.x + q1.x - ts.x) * 0.5f, (q.y + q1.y - ts.y) * 0.5f), skin::hex(0x1F282D), "TIE");
          ImGui::PopFont();
        }
      }
      if (click || (hov && io.MouseWheel != 0)) {
        int dir = io.MouseWheel < 0 ? -1 : 1;
        int nv = r == 0 ? (v + dir + 13) % 13 : r == 1 ? (v + dir + 3) % 3 : r == 2 ? (v + 1) % 3 : 1 - v;
        a.doc.setParam(t.pid(param), nv, false);
      }
      if (hov) a.setHint(std::string("Step ") + std::to_string(k + 1) + " " + rows[r] + "   (click / wheel to change)");
      ImGui::PopID();
    }
    skin::ledDot(dl, F.p(x + 15.5f, 404), 5 * F.s, k == playStep, K, ledOn);
    if (!inLen) dl->AddLine(F.p(x + 10, 404), F.p(x + 21, 404), K.textDim, 1.0f);
    ImGui::PopID();
  }
  endForm(F, width, kH);
}

}  // namespace

// Entry point (called before the older built-in editors).
bool drawFlSynthEditor(App& a, const PluginTarget& t, float width) {
  const PluginState* st = t.state(a.doc.song());
  if (!st) return false;
  const std::string& ty = st->type;
  if (ty == "gl.kick") editorKickFl(a, t, width);
  else if (ty == "gl.snare") editorSnareFl(a, t, width);
  else if (ty == "gl.hat") editorHatFl(a, t, width);
  else if (ty == "gl.clap") editorClapFl(a, t, width);
  else if (ty == "gl.pluck") editorPluckFl(a, t, width);
  else if (ty == "gl.acid") editorAcidFl(a, t, width);
  else return drawFlSynthEditor2(a, t, width);
  return true;
}

}  // namespace gl
