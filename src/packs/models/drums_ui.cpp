// Gridline — "models" pack, J-22 group "drums": editors on the reference
// forms' grids and palettes (slate-brown panel, orange-filled small wheels,
// blue-filled big wheels, dark combos with cream text).
//   Membrane Pad  — 794 × 225: header (preset name, mallet / membrane / bands /
//                   output captions), mallet column, membrane 2 × 3 block, LOW +
//                   sine column, MID band with its repeat switch, HIGH band,
//                   hold / lo-fi / pitch, four velocity routes.
//   Membrane Kit  — 762 × 614: channel pads (8 per bank: volume / pan wheels,
//                   name, key, mute / solo, play tile), master level + bank,
//                   the selected channel's drum model (same grid as the pad),
//                   the step sequencer (pattern, trigger key, length, 16 steps
//                   per page, velocity strip) and the 8-band EQ / limiter row.
//   Kick Sculptor — 389 × 420: duration / output / sampling wheels, the pitch
//                   arc (base, peak, slide), sub / drive / attack, variation arc,
//                   noise block (sample selector, A / D / S / start, pitch, mix,
//                   filter) and the click arc.
//   Sweep Kick    — 380 × 80: max / min / decay group, amp decay, click, dist.
#include "../../app/KeyLight.h"
#include "../flc_kit.h"
#include <map>
#include "drums.h"
#include "drums_voice.h"
#include "models_widgets.h"

namespace gl::packs::models {

using namespace dr;

namespace {

// --------------------------------------------------------------- palettes
const skin::Skin& padSkin() {  // small wheels: light rail, orange fill, grey cap
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x3B3836, 0x2C3338, 0xE0E0E0, 0xE8AC13, 0x626465, 0xD3D2B5);
    s.fillPressed = skin::hex(0xC79510);
    s.handle = skin::hex(0x212121);
    s.select = skin::hex(0xE8AC13);
    s.accent2 = skin::hex(0x18A0C8);
    s.lcdBg = skin::hex(0x171717);
    s.lcdText = skin::hex(0xDAD0B2);
    s.pillBg = skin::hex(0x171717);
    s.pillText = skin::hex(0xD3D2B5);
    s.graphBg = skin::hex(0x171717);
    s.graphGrid = skin::hex(0x262626);
    s.graphCurve = skin::hex(0xE8AC13);
    return s;
  }();
  return k;
}
const skin::Skin& padBig() {  // big wheels: dark rail ring, blue fill, light cap
  static const skin::Skin k = [] {
    skin::Skin s = padSkin();
    s.rail = skin::hex(0x533E4A);
    s.fill = skin::hex(0x1D6E94);
    s.fillPressed = skin::hex(0x2489B9);
    s.cap = skin::hex(0xD5D5D5);
    return s;
  }();
  return k;
}
const skin::Skin& kitChannel() {  // channel wheels: black rail, silver fill
  static const skin::Skin k = [] {
    skin::Skin s = padSkin();
    s.rail = skin::hex(0x0F0F0F);
    s.fill = skin::hex(0xC1C1C1);
    s.fillPressed = skin::hex(0xE0E0E0);
    s.cap = skin::hex(0x9EA0A0);
    return s;
  }();
  return k;
}

std::string noteName(int n) {
  static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  char b[32];
  std::snprintf(b, sizeof b, "%s%d", names[((n % 12) + 12) % 12], n / 12 - 1);
  return b;
}

// The drum model's controls on the pad form's grid, offset by (dx, dy) and
// parameter base `base` (pad: 0, kit channel: its model block).
void drawModel(App& a, const PluginTarget& t, const skin::Form& F, ImDrawList* dl, int base, float dx, float dy) {
  flk::Ctx c{a, t, F, padSkin(), dl};
  flk::Ctx cB{a, t, F, padBig(), dl};
  auto X = [&](float x) { return x + dx; };
  auto Y = [&](float y) { return y + dy; };
  // Section captions (reference: MALLET, MEMBRANE, MATERIAL, MIDDLE, EQ FREQ / LEVEL, LO FI, PARAMETER / VALUE).
  struct Cap { float x, w; const char* s; };
  const Cap caps[] = {{38, 104, "Mallet"}, {150, 106, "Membrane"}, {262, 50, "Low cut"},     {316, 48, "Sine"},
                      {368, 126, "Middle"}, {506, 52, "High"},      {562, 50, "Output"}, {618, 140, "Velocity · parameter / value"}};
  for (const auto& k : caps) flk::section(c, X(k.x), Y(42), k.w, 162, k.s);
  flk::chk(c, base + MdMalletRetrig, X(48), Y(166), 70, 20, "RETRIG");
  // J-128: three rows across every section, each on one centre line and one
  // label line (the 30 and 32 unit wheels used to sit 1 to 2 units apart).
  {
    const flk::Row row(c, Y(61), 32);
    flk::wheel(c, base + MdMalletAmp, X(47), Y(63), 30, "Level");
    flk::wheel(c, base + MdMalletDecay, X(99), Y(63), 30, "Decay");
    flk::wheel(cB, base + MdMemDecay, X(159), Y(61), 32, "Decay");
    flk::wheel(cB, base + MdMemCutoff, X(214), Y(61), 32, "Cutoff");
    flk::wheel(c, base + MdLowFreq, X(272), Y(63), 30, "Freq");
    flk::wheel(c, base + MdSofFreq, X(324), Y(63), 30, "Freq");
    flk::wheel(c, base + MdMidFreq, X(376), Y(63), 30, "Freq");
    flk::wheel(c, base + MdMidFreqMod, X(427), Y(63), 30, "Frq mod", skin::WheelBigLine | skin::WheelCentered);
    flk::wheel(c, base + MdHighFreq, X(516), Y(63), 30, "Freq");
    flk::wheel(cB, base + MdHold, X(570), Y(61), 32, "Hold");
  }
  {
    const flk::Row row(c, Y(110), 32);
    flk::wheel(c, base + MdMalletNoise, X(47), Y(113), 30, "Noise");
    flk::wheel(c, base + MdMalletNoiseRp, X(99), Y(113), 30, "Pattern");
    flk::wheel(cB, base + MdTension, X(159), Y(110), 32, "Tension");
    flk::wheel(cB, base + MdPhase, X(214), Y(110), 32, "Phase");
    flk::wheel(c, base + MdLowReso, X(272), Y(113), 30, "Reso");
    flk::wheel(c, base + MdSofDecay, X(324), Y(113), 30, "Decay");
    flk::wheel(c, base + MdMidLevel, X(376), Y(113), 30, "Level");
    flk::wheel(c, base + MdMidLevelMod, X(427), Y(113), 30, "Lvl mod", skin::WheelBigLine | skin::WheelCentered);
    flk::wheel(c, base + MdHighLevel, X(516), Y(113), 30, "Level");
    flk::wheel(cB, base + MdLofi, X(570), Y(110), 32, "Lo-fi");
  }
  {
    const flk::Row row(c, Y(160), 32);
    flk::wheel(cB, base + MdMaterial, X(159), Y(160), 32, "Material");
    flk::wheel(cB, base + MdSize, X(214), Y(160), 32, "Size");
    flk::wheel(cB, base + MdShape, X(271), Y(160), 32, "Shape");
    flk::wheel(c, base + MdSofLevel, X(324), Y(160), 30, "Level");
    flk::wheel(c, base + MdMidRes, X(376), Y(160), 30, "Reso");
    flk::wheel(c, base + MdMidAttack, X(427), Y(160), 30, "Attack");
    flk::wheel(c, base + MdHighRes, X(516), Y(160), 30, "Reso");
    flk::wheel(cB, base + MdPitch, X(570), Y(160), 32, "Pitch", skin::WheelBigLine | skin::WheelCentered);
  }
  static const char* rep[] = {"-", "2", "3", "4"};
  flk::sw(c, base + MdRetrig, X(467), Y(110), 22, 66, rep, 4);
  for (int r = 0; r < 4; ++r) {
    flk::menu(c, base + MdVelSrc1 + r, X(625), Y(70 + 34 * r), 88, 20);
    skin::wheel(a, t, base + MdVelAmt1 + r, F.p(X(723), Y(66 + 34 * r)), 26 * F.s, padSkin(), skin::WheelBigLine);
  }
}

// ---------------------------------------------------------------- Membrane Pad
void drawPad(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  constexpr float kW = 794, kH = 225;
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const skin::Skin& K = padSkin();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  flk::Ctx c{a, t, F, K, dl};
  ImGui::PushID("gl.membranepad");
  // Header: name LCD, fundamental readout, output wheel, audition tile.
  if (!pui::titleWordmark(c.dl, "gl.membranepad", c.F.p(16, 14), c.F.font(10), c.K.text))  // J-136: the wordmark
    flk::text(c, 16, 14, "MEMBRANE PAD", K.text, 10);
  flk::lcd(c, 150, 10, 220, 20, st.data.empty() ? "<user patch>" : st.data.c_str());
  float p[MdCount];
  for (int k = 0; k < MdCount; ++k) p[k] = ui2::val(st, PdModel + k);
  char b[48];
  std::snprintf(b, sizeof b, "%.0f Hz · %.2f s", mdFundamental(p), mdRingSeconds(p));
  flk::lcd(c, 380, 10, 150, 20, b);
  flk::text(c, 548, 16, "OUTPUT");
  skin::wheel(a, t, PdOutput, F.p(600, 8), 26 * F.s, padBig(), skin::WheelBigLine);
  skin::button("##audition", F.p(650, 8), F.sz(80, 26), K, "STRIKE", false, F.font(8));
  if (t.isChannel())  // J-77: flashes on every strike (any key: one membrane)
    keylight::drawFlash(dl, ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), theme::pal().hot, keylight::flashAny(a, t.owner), 3 * F.s);
  if (ImGui::IsItemActivated()) a.engine.noteOn(t.owner, 60, 0.9f);
  if (ImGui::IsItemDeactivated()) a.engine.noteOff(t.owner, 60);
  drawModel(a, t, F, dl, PdModel, 0, 0);
  F.end(kW, kH);
  ImGui::PopID();
}

// ---------------------------------------------------------------- Membrane Kit
struct KitUi {
  int sel = 0, bank = 0, pattern = 0, page = 0;
};
std::map<uint64_t, KitUi>& kitUi() {
  static std::map<uint64_t, KitUi> m;
  return m;
}

void drawKit(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  KitData d;
  decodeKitData(st.data, d);
  constexpr float kW = 762, kH = 614;
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const skin::Skin& K = padSkin();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  flk::Ctx c{a, t, F, K, dl};
  flk::Ctx cB{a, t, F, padBig(), dl};
  ImGui::PushID("gl.membranekit");
  KitUi& u = kitUi()[t.key()];
  u.sel = std::clamp(u.sel, 0, kKitPads - 1);
  u.bank = std::clamp(u.bank, 0, 1);
  u.pattern = std::clamp(u.pattern, 0, kKitPatterns - 1);
  u.page = std::clamp(u.page, 0, 3);
  if (!pui::titleWordmark(c.dl, "gl.membranekit", c.F.p(30, 30), c.F.font(10), c.K.text))  // J-136: the wordmark
    flk::text(c, 30, 30, "MEMBRANE KIT", K.text, 10);
  flk::lcd(c, 150, 26, 300, 22, d.kitName.empty() ? "<user kit>" : d.kitName.c_str());
  // ---- channel pads
  skin::well(dl, F.p(29, 57), F.p(609, 287), K, 4 * F.s);
  for (int i = 0; i < 8; ++i) {
    const int ch = u.bank * 8 + i;
    const float x = 29 + (i % 4) * 145, y = 57 + (i / 4) * 115;
    ImGui::PushID(ch);
    const std::string nm = d.padName[ch].empty() || d.padName[ch] == "..." ? "Channel " + std::to_string(ch + 1) : d.padName[ch];
    flk::lcd(c, x + 13, y + 2, 92, 15, nm.c_str());
    const int key = ui2::ival(st, kitPad(ch, KpKey), 60 + ch);
    flk::lcd(c, x + 108, y + 2, 30, 14, noteName(key).c_str());
    skin::wheel(a, t, kitPad(ch, KpVolume), F.p(x + 6, y + 22), 36 * F.s, kitChannel(), skin::WheelBigLine);
    skin::wheel(a, t, kitPad(ch, KpPan), F.p(x + 10, y + 64), 28 * F.s, kitChannel(), skin::WheelBigLine | skin::WheelCentered);
    flk::chk(c, kitPad(ch, KpMute), x + 106, y + 28, 36, 18, "M");
    flk::chk(c, kitPad(ch, KpSolo), x + 106, y + 52, 36, 18, "S");
    char num[32];
    std::snprintf(num, sizeof num, "%d", ch + 1);
    skin::button("##play", F.p(x + 54, y + 26), F.sz(45, 45), K, num, u.sel == ch, F.font(10));
    // J-77: the channel's pad flashes on every hit of its key (playback, MIDI, keys, clicks).
    if (t.isChannel())
      keylight::drawFlash(dl, ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), theme::pal().hot, keylight::flash(a, t.owner, key), 4 * F.s);
    if (ImGui::IsItemActivated()) {
      u.sel = ch;
      a.engine.noteOn(t.owner, key, 0.9f);
    }
    if (ImGui::IsItemDeactivated()) a.engine.noteOff(t.owner, key);
    ImGui::PopID();
  }
  // ---- master column
  flk::text(c, 618, 26, "MASTER");
  flk::wheel(cB, KmLevel, 701, 18, 32, "Level");
  if (flk::btn(c, "##b1", 611, 79, 55, 52, "1-8", u.bank == 0)) u.bank = 0;
  if (flk::btn(c, "##b2", 671, 79, 55, 52, "9-16", u.bank == 1)) u.bank = 1;
  flk::text(c, 614, 146, "CHANNEL KEY");
  flk::digi(c, kitPad(u.sel, KpKey), 614, 160, 60, 18);
  {
    char b[32];
    std::snprintf(b, sizeof b, "CH %d", u.sel + 1);
    flk::lcd(c, 680, 160, 50, 18, b);
  }
  // ---- selected channel's drum model (pad grid shifted as on the reference)
  drawModel(a, t, F, dl, kitPad(u.sel, KpModel), -16, 255);
  // ---- sequencer
  KitPattern& P = d.pattern[u.pattern];
  skin::well(dl, F.p(6, 462), F.p(756, 552), K, 4 * F.s);
  flk::text(c, 14, 470, "SEQUENCER");
  for (int k = 0; k < kKitPatterns; ++k) {
    char b[8];
    std::snprintf(b, sizeof b, "%d", k + 1);
    if (flk::btn(c, (std::string("##pat") + b).c_str(), 100 + k * 21, 468, 19, 18, b, u.pattern == k)) u.pattern = k;
  }
  bool changed = false;
  flk::text(c, 444, 472, "KEY");
  {
    const std::string kn = P.key < 0 ? "--" : noteName(P.key);
    if (flk::btn(c, "##keydn", 466, 468, 14, 18, GL_ICON_LABEL("chevron-left", "<"), false)) {
      P.key = P.key <= 0 ? -1 : P.key - 1;
      changed = true;
    }
    flk::lcd(c, 482, 468, 34, 18, kn.c_str());
    if (flk::btn(c, "##keyup", 518, 468, 14, 18, GL_ICON_LABEL("chevron-right", ">"), false)) {
      P.key = std::min(127, P.key < 0 ? 60 : P.key + 1);
      changed = true;
    }
  }
  flk::text(c, 540, 472, "LEN");
  {
    char b[8];
    std::snprintf(b, sizeof b, "%d", P.length);
    if (flk::btn(c, "##lendn", 560, 468, 14, 18, GL_ICON_LABEL("chevron-left", "<"), false)) {
      P.length = std::max(0, P.length - 1);
      changed = true;
    }
    flk::lcd(c, 576, 468, 28, 18, b);
    if (flk::btn(c, "##lenup", 606, 468, 14, 18, GL_ICON_LABEL("chevron-right", ">"), false)) {
      P.length = std::min(kKitSteps, P.length + 1);
      changed = true;
    }
  }
  for (int pg = 0; pg < 4; ++pg) {
    char b[8];
    std::snprintf(b, sizeof b, "%d", pg + 1);
    if (flk::btn(c, (std::string("##pg") + b).c_str(), 630 + pg * 31, 468, 28, 18, b, u.page == pg)) u.page = pg;
  }
  {  // J-68: running step of the kit's sequencer (published by the engine)
    int playing = -1;
    if (t.isChannel())
      if (auto rt = a.builder.channelRuntime(t.owner); rt && rt->gen) playing = rt->gen->displayStep();
    if (playing >= u.page * 16 && playing < u.page * 16 + 16) {
      const float x = 60 + (playing - u.page * 16) * 40;
      dl->AddRect(F.p(x - 3, 493), F.p(x + 30, 544), theme::withAlpha(K.select, 0.9f), 3 * F.s, 0, 1.5f);
    }
  }
  for (int s = 0; s < 16; ++s) {
    const int step = u.page * 16 + s;
    uint8_t& cell = P.cell[u.sel][step];
    const bool on = (cell & 0x80) != 0;
    const float x = 60 + s * 40;
    char id[16];
    std::snprintf(id, sizeof id, "##st%d", s);
    char lbl[32];
    std::snprintf(lbl, sizeof lbl, "%d", step + 1);
    if (flk::btn(c, id, x, 496, 27, 27, lbl, on)) {
      cell = static_cast<uint8_t>(on ? (cell & 0x7F) : (cell | 0x80));
      changed = true;
    }
    const float vh = 14.0f * static_cast<float>(cell & 0x7F) / 127.0f;
    dl->AddRectFilled(F.p(x, 540 - vh), F.p(x + 27, 540), step < P.length ? K.fill : K.textDim);
  }
  if (changed) {
    PluginState ns = st;
    ns.data = encodeKitData(d);
    setPluginState(a, t, ns, "edit drum pattern");
  }
  flk::text(c, 14, 500, "CH");
  {
    char b[32];
    std::snprintf(b, sizeof b, "%d", u.sel + 1);
    flk::lcd(c, 14, 512, 34, 16, b);
  }
  flk::wheel(cB, KmSwing, 712, 500, 30, "Swing");
  // ---- EQ / limiter
  skin::well(dl, F.p(6, 556), F.p(756, 610), K, 4 * F.s);
  static const char* bands[8] = {"60", "150", "400", "1k", "2.4k", "5k", "9k", "14k"};
  for (int k = 0; k < 8; ++k) flk::wheel(cB, KmEq1 + k, 36 + k * 57, 558, 30, bands[k], skin::WheelBigLine | skin::WheelCentered);
  flk::tog(c, KmEqOn, 495, 565, 50, 22, "EQ");
  flk::tog(c, KmLimiter, 560, 565, 60, 22, "LIMIT");
  flk::wheel(cB, KmLimGain, 636, 558, 30, "Gain");
  flk::wheel(cB, KmLimRelease, 693, 558, 30, "Release");
  F.end(kW, kH);
  ImGui::PopID();
}

// ---------------------------------------------------------------- Kick Sculptor
const skin::Skin& kickSkin() {  // big wheels: grey rail, dark fill
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x3B3836, 0x2C2A28, 0xB1B3B4, 0x353535, 0x616263, 0xD3D2B5);
    s.fillPressed = skin::hex(0x2D2D2D);
    s.lcdBg = skin::hex(0x171717);
    s.lcdText = skin::hex(0xC79510);
    s.pillBg = skin::hex(0x171717);
    s.pillText = skin::hex(0xC79510);
    s.select = skin::hex(0xC79510);
    return s;
  }();
  return k;
}
const skin::Skin& kickMid() {  // arc wheels: light cap, dark fill
  static const skin::Skin k = [] {
    skin::Skin s = kickSkin();
    s.rail = skin::hex(0x533E4A);
    s.fill = skin::hex(0xA0A8AB);
    s.fillPressed = skin::hex(0xC0C8CB);
    s.cap = skin::hex(0xA0A8AB);
    return s;
  }();
  return k;
}
const skin::Skin& kickNoise() {  // small amber noise wheels
  static const skin::Skin k = [] {
    skin::Skin s = kickSkin();
    s.rail = skin::hex(0x8F5D1B);
    s.fill = skin::hex(0xE6AC13);
    s.fillPressed = skin::hex(0xC79510);
    s.cap = skin::hex(0xC79510);
    return s;
  }();
  return k;
}

void drawKick(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  constexpr float kW = 389, kH = 420;
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const skin::Skin& K = kickSkin();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  flk::Ctx c{a, t, F, K, dl};
  flk::Ctx cM{a, t, F, kickMid(), dl};
  flk::Ctx cN{a, t, F, kickNoise(), dl};
  ImGui::PushID("gl.kicksculpt");
  if (!pui::titleWordmark(c.dl, "gl.kicksculpt", c.F.p(10, 6), c.F.font(9), c.K.text))  // J-136: the wordmark
    flk::text(c, 10, 6, "KICK SCULPTOR", K.text, 9);
  if (!st.data.empty()) flk::lcd(c, 120, 4, 160, 14, st.data.c_str());
  // Arcs (decoration): the pitch arc and the click arc the wheels sit on.
  dl->PathArcTo(F.p(150, 260), 120 * F.s, 3.3f, 4.6f, 32);
  dl->PathStroke(skin::hex(0x2A2725), 0, 10 * F.s);
  flk::wheel(c, KsDuration, 36, 47, 46, "Duration");
  flk::wheel(c, KsLevel, 172, 47, 46, "Output");
  flk::wheel(c, KsSampling, 307, 47, 46, "Sampling");
  flk::wheel(cM, KsBase, 123, 100, 36, "Base");
  flk::wheel(cM, KsPeak, 77, 128, 36, "Peak");
  flk::wheel(cM, KsSlide, 36, 170, 36, "Slide");
  flk::wheel(cM, KsDrive, 233, 100, 36, "Drive");
  flk::wheel(cM, KsSub, 278, 128, 36, "Sub");
  flk::wheel(cM, KsAttack, 316, 170, 36, "Attack");
  flk::wheel(cM, KsVarBase, 30, 228, 36, "Vary base");
  flk::wheel(cM, KsVarPeak, 44, 278, 36, "Vary peak");
  flk::wheel(cM, KsVarSlide, 80, 326, 36, "Vary slide");
  flk::wheel(cM, KsVarPhase, 126, 368, 36, "Vary phase");
  flk::wheel(cM, KsClickLevel, 316, 278, 36, "Click");
  flk::wheel(cM, KsClickCut, 276, 326, 36, "Click cut");
  flk::wheel(cM, KsClickFreq, 232, 368, 36, "Click tone");
  // Noise block.
  flk::section(c, 112, 200, 160, 120, "Noise");
  flk::menu(c, KsNoiseSample, 140, 218, 106, 18);
  flk::wheel(cN, KsNoiseA, 124, 244, 20, "A");
  flk::wheel(cN, KsNoiseD, 154, 244, 20, "D");
  flk::wheel(cN, KsNoiseS, 184, 244, 20, "S");
  flk::wheel(cN, KsNoiseStart, 220, 244, 20, "Start");
  flk::wheel(cN, KsNoisePitch, 134, 284, 20, "Pitch");
  flk::wheel(cN, KsNoiseFilter, 182, 284, 20, "Filter");
  flk::wheel(cN, KsNoiseMix, 228, 284, 20, "Mix");
  skin::button("##aud", F.p(300, 6), F.sz(80, 18), K, "KICK", false, F.font(8));
  if (ImGui::IsItemActivated()) a.engine.noteOn(t.owner, 60, 0.9f);
  if (ImGui::IsItemDeactivated()) a.engine.noteOff(t.owner, 60);
  F.end(kW, kH);
  ImGui::PopID();
}

// ---------------------------------------------------------------- Sweep Kick
void drawSweep(App& a, const PluginTarget& t, float width) {
  constexpr float kW = 380, kH = 80;
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  static const skin::Skin K = [] {
    skin::Skin s = skin::Skin::fl(0x454E54, 0x3A4248, 0x3F484D, 0xFFA830, 0x3F484D, 0xB9BFC3);
    s.fillPressed = skin::hex(0xFFA830);
    s.panelHi = skin::hex(0x5B656B);
    return s;
  }();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  flk::Ctx c{a, t, F, K, dl};
  ImGui::PushID("gl.sweepkick");
  skin::bevel(dl, F.p(5, 5), F.p(183, 75), K);
  skin::bevel(dl, F.p(188, 5), F.p(247, 75), K);
  skin::bevel(dl, F.p(252, 5), F.p(311, 75), K);
  skin::bevel(dl, F.p(316, 5), F.p(375, 75), K);
  flk::wheel(c, SkMaxFreq, 17, 13, 38, "Max");
  flk::wheel(c, SkMinFreq, 74, 13, 38, "Min", skin::WheelBigLine | skin::WheelCentered);
  flk::wheel(c, SkFreqDecay, 132, 13, 38, "Decay");
  flk::wheel(c, SkAmpDecay, 198, 13, 38, "Amp decay");
  flk::wheel(c, SkClick, 261, 13, 38, "Click");
  flk::wheel(c, SkDist, 325, 13, 38, "Dist");
  F.end(kW, kH);
  ImGui::PopID();
}

}  // namespace

bool drawDrumsEditors(App& a, const PluginTarget& t, float width) {
  const PluginState* st = t.state(a.doc.song());
  if (!st) return false;
  if (st->type == "gl.membranepad") drawPad(a, t, width);
  else if (st->type == "gl.membranekit") drawKit(a, t, width);
  else if (st->type == "gl.kicksculpt") drawKick(a, t, width);
  else if (st->type == "gl.sweepkick") drawSweep(a, t, width);
  else return false;
  return true;
}

}  // namespace gl::packs::models
