// Gridline — Alloy editor on the reference metallic timbre generator's 560 ×
// 640 form (its test keyboard row replaced by an options row): master level /
// coarse / fine rails, the X / Y modulation pad, magic dust and the patch
// notes on top; the timbre rails (richness, pre-decay, decay, release,
// fullness, morph), the two seeds, unison rails and the filter block; the
// articulator / timbre EQ editor (target + part selectors, curve editor and
// its envelope / LFO knobs) with the harmonic comb and the 3-band mix EQ; the
// chorus, delay and reverb rails; then portamento / key / velocity / quality.
#include "../../app/App.h"
#include "metal_widgets.h"

namespace gl::packs::models::metal {

using namespace ui;

namespace {

constexpr float kW = 560, kH = 604;

const skin::Skin& alSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(bgr(5327684), bgr(4077875), bgr(5064767), bgr(9536893), bgr(5064767), bgr(11117983));
    s.select = skin::hex(bgr(3188991));  // orange chorus / selection fill
    s.accent2 = skin::hex(bgr(3188991));
    s.handle = skin::hex(bgr(5393737));
    s.lcdBg = skin::hex(bgr(8810862));
    s.lcdText = skin::hex(0xEFF3F7);
    s.pillBg = skin::hex(bgr(3946805));
    s.graphBg = skin::hex(0x2E3238);
    s.graphGrid = skin::hex(0x3A3F46);
    s.graphGridStrong = skin::hex(0x4A5059);
    s.graphCurve = skin::hex(bgr(10782331));
    return s;
  }();
  return k;
}

struct AlUi {
  int target = AaVolume;  // 0..12 articulators, 13..16 timbre EQ curves
  int part = 0;           // 0 envelope, 1 LFO, 2..5 key / velocity / X / Y maps
  int sel = -1;
};

void commit(App& a, const PluginTarget& t, const PluginState& st, const AlloyData& d, const char* label) {
  PluginState ns = st;
  ns.data = serializeAlloy(d);
  if (!(ns == st)) setPluginState(a, t, ns, label, (t.key() * 1099511628211ull) ^ 0x416C6C6Full);
}

// Small selector box cycling through `items` (click: next, right-click: previous).
bool cycleBox(const flk::Ctx& c, const char* id, float x, float y, float w, float h, const char* const* items, int n, int& v) {
  const ImVec2 p0 = c.F.p(x, y), p1 = c.F.p(x + w, y + h);
  skin::lcd(c.dl, p0, p1, c.K, items[std::clamp(v, 0, n - 1)], c.F.font(8.5f));
  ImGui::SetCursorScreenPos(p0);
  ImGui::InvisibleButton(id, ImVec2(std::max(1.0f, p1.x - p0.x), std::max(1.0f, p1.y - p0.y)),
                         ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  bool ch = false;
  if (ImGui::IsItemClicked(0)) v = (v + 1) % n, ch = true;
  if (ImGui::IsItemClicked(1)) v = (v + n - 1) % n, ch = true;
  if (ImGui::IsItemHovered()) c.a.setHint("Click: next · right-click: previous");
  return ch;
}

}  // namespace

void drawAlloy(App& a, const PluginTarget& t, float width) {
  const PluginState* sp = t.state(a.doc.song());
  if (!sp) return;
  const PluginState st = *sp;
  const skin::Skin& K = alSkin();
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  flk::Ctx c{a, t, F, K, dl};
  AlUi& u = stateFor<AlUi>(t);
  AlloyData d;
  if (!parseAlloy(st.data, d)) d = AlloyData{};
  ImGui::PushID("gl.alloy");
  // ---- master + modulation + notes
  flk::vsl(c, AlVolume, 11, 4, 20, 64, "Vol");
  flk::vsl(c, AlCoarse, 44, 4, 20, 64, "Crs");
  flk::vsl(c, AlFine, 67, 4, 20, 64, "Fine");
  skin::xyPad(a, t, AlModX, AlModY, F.p(99, 8), F.sz(77, 77), K, "X / Y");
  flk::text(c, 184, 12, "MODULATION");
  flk::wheel(c, AlModX, 184, 26, 22, nullptr);
  flk::wheel(c, AlModY, 184, 56, 22, nullptr);
  flk::text(c, 210, 32, "X");
  flk::text(c, 210, 62, "Y");
  flk::chk(c, AlModSmooth, 222, 70, 70, 14, "SMOOTH");
  flk::wheel(c, AlDust, 262, 14, 28, "Dust");
  {
    const ImVec2 i0 = F.p(320, 8), i1 = F.p(527, 84);
    dl->AddRectFilled(i0, i1, skin::hex(bgr(3419686)), 3);
    std::string info = d.info.empty() ? std::string("(no notes)") : d.info;
    dl->PushClipRect(i0, i1, true);
    float y = 12;
    size_t at = 0;
    for (int line = 0; line < 6 && at <= info.size(); ++line) {
      size_t nl = info.find('\n', at);
      if (nl == std::string::npos) nl = info.size();
      skin::text(dl, F.p(326, y), K.textDim, info.substr(at, std::min<size_t>(nl - at, 48)).c_str(), F.font(8));
      y += 11;
      at = nl + 1;
    }
    dl->PopClipRect();
  }
  // ---- timbre
  flk::text(c, 12, 100, "TIMBRE");
  flk::chk(c, AlDeclick, 95, 100, 61, 13, "DECLICK");
  flk::chk(c, AlAltDecay, 160, 100, 72, 13, "ALT DECAY");
  flk::vsl(c, AlRichness, 12, 118, 20, 66, "Rich");
  flk::vsl(c, AlPreDecay, 39, 118, 20, 66, "Pre");
  flk::vsl(c, AlDecay, 66, 118, 20, 66, "Dec");
  flk::vsl(c, AlRelease, 93, 118, 20, 66, "Rel");
  flk::vsl(c, AlFullness, 120, 118, 20, 66, "Full");
  flk::vsl(c, AlMorph, 154, 118, 20, 66, "Morph");
  flk::text(c, 188, 122, "SEED 2");
  flk::digi(c, AlSeed2, 187, 134, 42, 18);
  flk::text(c, 188, 160, "SEED 1");
  flk::digi(c, AlSeed1, 187, 172, 42, 18);
  // ---- unison
  flk::text(c, 250, 100, "UNISON");
  flk::text(c, 247, 160, "ORD");
  flk::digi(c, AlUniOrder, 247, 172, 28, 18);
  flk::vsl(c, AlUniPan, 285, 118, 20, 66, "Pan");
  flk::vsl(c, AlUniVol, 312, 118, 20, 66, "Vol");
  flk::vsl(c, AlUniPitch, 339, 118, 20, 66, "Pitch");
  flk::vsl(c, AlUniPhase, 366, 118, 20, 66, "Phase");
  // ---- filter
  flk::section(c, 400, 92, 156, 122, "Filter");
  flk::menu(c, AlFltType, 464, 98, 88, 17);
  flk::menu(c, AlFltMode, 464, 118, 88, 17);
  flk::wheel(c, AlCutoff, 462, 142, 42, "Freq");
  flk::wheel(c, AlReso, 515, 150, 34, "Res");
  flk::chk(c, AlFltFlat, 408, 184, 46, 14, "FLAT");
  // ---- articulator / timbre EQ editor
  static const char* const targets[AaCount + AeCount] = {
      "Volume",        "Filter freq",   "Filter reso",  "Pitch",       "Unison pitch", "Unison phase",  "Pre-decay",   "Decay time",
      "Release time",  "Fullness",      "Morphing",     "Rotation",    "EQ shifting",  "Forced harm.",  "Impulse EQ",  "Decay EQ",
      "Release EQ"};
  static const char* const parts[6] = {"Envelope", "LFO", "Key map", "Velocity map", "Mod X map", "Mod Y map"};
  if (cycleBox(c, "##tgt", 10, 222, 164, 18, targets, AaCount + AeCount, u.target)) u.sel = -1;
  const bool isEq = u.target >= AaCount;
  const bool hasEnv = !isEq && u.target < kAlloyEnvArts;
  if (!isEq) {
    if (!hasEnv && u.part < 2) u.part = 2;
    int pv = u.part;
    if (cycleBox(c, "##part", 206, 222, 164, 18, parts, 6, pv)) {
      if (!hasEnv && pv < 2) pv = pv == 1 ? 5 : 2;
      u.part = pv, u.sel = -1;
    }
  }
  flk::text(c, 378, 226, "COMB");
  flk::menu(c, AlComb, 404, 222, 46, 18);
  {
    hm::Curve* cv;
    float neutral;
    bool timeAxis = false;
    if (isEq) {
      cv = &d.eq[static_cast<size_t>(u.target - AaCount)];
      neutral = alloyEqNeutral(u.target - AaCount);
    } else {
      AlloyArtData& A = d.arts[static_cast<size_t>(u.target)];
      neutral = A.bipolar ? 0.5f : 1.0f;
      if (u.part == 0) cv = &A.env, timeAxis = true;
      else if (u.part == 1) cv = &A.lfo;
      else cv = &A.maps[static_cast<size_t>(u.part - 2)];
    }
    if (curveEdit(c, "##curve", 9, 245, 429, 140, *cv, timeAxis, neutral, u.sel)) {
      if (!isEq && u.part <= 1) cv->enabled = true;
      commit(a, t, st, d, "edit curve");
    }
    if (!isEq && u.part <= 1) {
      bool on = cv->enabled;
      if (flk::btn(c, "##on", 12, 390, 34, 16, on ? "ON" : "OFF", on)) {
        cv->enabled = !on;
        commit(a, t, st, d, on ? "disable curve" : "enable curve");
      }
    }
    if (hasEnv) {
      const int base = u.part == 1 ? AkLfoSpeed : AkAttack;
      static const char* envL[4] = {"Att", "Dec", "Sus", "Rel"};
      static const char* lfoL[4] = {"Speed", "Tens", "Skew", "Width"};
      for (int k = 0; k < 4; ++k)
        flk::wheel(c, alKnob(u.target, base + k), 70 + k * 56, 389, 22, u.part == 1 ? lfoL[k] : envL[k],
                   skin::WheelBigLine | (base + k == AkSustain || base + k >= AkLfoTension ? skin::WheelCentered : 0));
    }
  }
  // ---- mix EQ
  flk::text(c, 446, 258, "EQ");
  for (int k = 0; k < 3; ++k) {
    const float x = 464 + k * 32;
    static const char* const eqT[3] = {"LS", "PK", "HS"};
    flk::digi(c, AlEqType1 + k, x, 240, 24, 14, eqT[std::clamp(prmi(st, AlEqType1 + k, k), 0, 2)]);
    flk::vsl(c, AlEqLevel1 + k, x, 258, 20, 82, nullptr);
    flk::wheel(c, AlEqFreq1 + k, x - 5, 352, 28, nullptr);
    flk::wheel(c, AlEqWidth1 + k, x - 5, 386, 28, nullptr);
  }
  flk::text(c, 446, 362, "F");
  flk::text(c, 446, 396, "W");
  // ---- chorus / delay / reverb
  flk::section(c, 4, 428, 168, 122, "Chorus");
  flk::text(c, 6, 474, "ORD");
  flk::digi(c, AlChOrder, 7, 486, 24, 18);
  static const char* chL[6] = {"Dep", "Spd", "Dly", "Spr", "X", "Mix"};
  for (int k = 0; k < 6; ++k) flk::vsl(c, AlChDepth + k, 36 + k * 22, 448, 18, 82, chL[k], skin::hex(bgr(3188991)));
  flk::section(c, 178, 428, 160, 122, "Delay");
  flk::tog(c, AlDlOn, 248, 431, 32, 14, "ON");
  flk::tog(c, AlDlTempo, 284, 431, 50, 14, "TEMPO");
  static const char* dmodes[4] = {"NORM", "PING", "INV", "INV PP"};
  flk::sw(c, AlDlMode, 184, 452, 54, 72, dmodes, 4);
  static const char* dlL[4] = {"Fb", "Time", "Ofs", "Vol"};
  for (int k = 0; k < 4; ++k) flk::vsl(c, AlDlFeedback + k, 245 + k * 22, 452, 18, 78, dlL[k]);
  flk::section(c, 344, 428, 212, 122, "Reverb");
  flk::tog(c, AlRvOn, 452, 431, 32, 14, "ON");
  flk::tog(c, AlRvTempo, 488, 431, 50, 14, "TEMPO");
  flk::text(c, 348, 474, "COL");
  flk::digi(c, AlRvColor, 348, 486, 24, 18);
  static const char* rvL[9] = {"LC", "HC", "Pre", "Room", "Diff", "Dec", "HD", "", "Wet"};
  for (int k = 0; k < 9; ++k) {
    if (k == 7) continue;  // colour has its own digit box
    const int prm = AlRvLowCut + k;
    flk::vsl(c, prm, 379 + (k > 7 ? 7 : k) * 22, 452, 18, 78, rvL[k]);
  }
  // ---- options row (reference: options menu)
  flk::section(c, 4, 556, 552, 44, "Voice");
  flk::tog(c, AlPorta, 70, 562, 44, 15, "PORTA");
  flk::tog(c, AlPortaVar, 118, 562, 44, 15, "VAR");
  flk::menu(c, AlPortaLimit, 166, 561, 70, 17);
  flk::tog(c, AlKeyPitch, 244, 562, 54, 15, "KEY>PIT");
  flk::tog(c, AlKeySeed, 302, 562, 58, 15, "KEY>SEED");
  flk::tog(c, AlOneVoicePerKey, 364, 562, 54, 15, "1/KEY");
  flk::menu(c, AlQuality, 424, 561, 70, 17);
  flk::tog(c, AlAutoLoud, 498, 562, 54, 15, "AUTO LVL");
  flk::tog(c, AlVelVol, 70, 581, 54, 15, "VEL>VOL");
  flk::tog(c, AlVelAttTime, 128, 581, 62, 15, "VEL>ATT");
  flk::tog(c, AlVelAttLen, 194, 581, 62, 15, "VEL>LEN");
  flk::tog(c, AlRelLink, 260, 581, 62, 15, "REL LINK");
  flk::tog(c, AlNatScale, 326, 581, 62, 15, "NAT SCALE");
  flk::tog(c, AlNatWeight, 392, 581, 62, 15, "NAT WGT");
  ImGui::PopID();
  F.end(kW, kH);
}

}  // namespace gl::packs::models::metal
