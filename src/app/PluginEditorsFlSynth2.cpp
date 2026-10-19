// Gridline — J-19 reference-family instrument editors, part 2:
//   FM Synth  → 4-operator FM panel (operator columns with coarse / fine /
//               velocity wheels, envelope display, envelope wheels; misc
//               column; keyboard strip), extended to four operators + algorithm.
//   Sampler   → channel sampler pages (sample · envelope · misc) in the
//               reference family look (no plugin form exists for it).
//   Granular  → granulizer panel (392 wide: grains · effects · transients ·
//               time · voice · wave scope).
//   MIDI Out  → MIDI out panel (558 × 389: channel / patch / bank / port
//               selectors, 3 × 3 controller wheels, bottom bar).
#include "../engine/Dsp.h"
#include "PluginEditorsFlSynth.h"

namespace gl {

using namespace pp;

namespace {

using namespace ed;
using namespace flsyn;

// ======================================================================= FM Synth
const skin::Skin& fmSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x303E45, 0x5D6973, 0xF2F4F5, 0xFFFFFF, 0x3F484D, 0xB9BFC3);
    s.fillPressed = skin::hex(0xE02010);
    s.pillBg = skin::hex(0x4F5A63);
    s.graphBg = skin::hex(0x26313A);
    s.graphGrid = skin::hex(0x36434D);
    s.graphGridStrong = skin::hex(0x4A5862);
    s.graphCurve = skin::hex(0x87B9F6);
    s.select = skin::hex(0x87B9F6);
    s.lcdText = skin::hex(0xDDE6F0);
    s.textDim = skin::hex(0xCDD3D8);  // labels on the lighter operator panels
    s.lcdBg = skin::hex(0x26313A);
    return s;
  }();
  return k;
}
const skin::Skin& fmWheelSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = fmSkin();
    s.rail = skin::hex(0x8A949C);
    s.text = skin::hex(0xF4F6F7);
    return s;
  }();
  return k;
}

void drawAlgoSkin(ImDrawList* dl, int algo, ImVec2 p, ImVec2 sz, const skin::Skin& k, float s) {
  skin::graphWell(dl, p, ImVec2(p.x + sz.x, p.y + sz.y), k);
  static const uint8_t mods[8][4] = {{2, 4, 8, 0}, {2, 12, 0, 0}, {10, 4, 0, 0}, {6, 0, 8, 0}, {2, 0, 8, 0}, {8, 8, 8, 0}, {0, 0, 8, 0}, {0, 0, 0, 0}};
  algo = std::clamp(algo, 0, 7);
  int depth[4] = {0, 0, 0, 0};
  for (int it = 0; it < 4; ++it)
    for (int o = 0; o < 4; ++o)
      for (int m = 0; m < 4; ++m)
        if (mods[algo][o] & (1 << m)) depth[m] = std::max(depth[m], depth[o] + 1);
  int perDepth[4] = {0, 0, 0, 0}, slot[4];
  for (int o = 0; o < 4; ++o) slot[o] = perDepth[depth[o]]++;
  int maxDepth = 0;
  for (int d : depth) maxDepth = std::max(maxDepth, d);
  const float rowH = std::min(22.0f * s, (sz.y - 18 * s) / static_cast<float>(maxDepth + 1));
  ImVec2 pos[4];
  for (int o = 0; o < 4; ++o)
    pos[o] = ImVec2(p.x + sz.x * (slot[o] + 1.0f) / (perDepth[depth[o]] + 1.0f), p.y + sz.y - 14 * s - depth[o] * rowH);
  for (int o = 0; o < 4; ++o)
    for (int m = 0; m < 4; ++m)
      if (mods[algo][o] & (1 << m)) dl->AddLine(pos[m], pos[o], k.graphText, 1.5f);
  const float bw = 9 * s, bh = 7 * s;
  for (int o = 0; o < 4; ++o) {
    if (depth[o] == 0) dl->AddLine(pos[o], ImVec2(pos[o].x, p.y + sz.y - 3), k.graphCurve, 1.5f);
    dl->AddRectFilled(ImVec2(pos[o].x - bw, pos[o].y - bh), ImVec2(pos[o].x + bw, pos[o].y + bh), depth[o] == 0 ? k.graphCurve : k.wellC(), 3);
    char b[4];
    std::snprintf(b, sizeof b, "%d", o + 1);
    ImGui::PushFont(app().fontMonoBold, std::clamp(9.0f * s, 9.0f, 13.0f));
    ImVec2 ts = ImGui::CalcTextSize(b);
    dl->AddText(ImVec2(pos[o].x - ts.x * 0.5f, pos[o].y - ts.y * 0.5f), depth[o] == 0 ? k.graphBg : k.text, b);
    ImGui::PopFont();
  }
  dl->AddCircle(ImVec2(pos[3].x + bw + 5 * s, pos[3].y - bh), 3.5f * s, k.graphText, 12, 1.0f);  // op 4 feedback
}

void editorFmFl(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  const skin::Skin& K = fmSkin();
  const skin::Skin& Kw = fmWheelSkin();
  constexpr float kW = 760, kH = 462;
  skin::Form F = fitForm(width, kW, kH, 470);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  auto w36 = [&](int p, float x, float y, const char* l, int flags = skin::WheelBigLine) { wheelL(a, t, p, F, x, y, 34, K, Kw, l, flags); };
  // ---- top row: MISC · LFO · ALGORITHM
  panelBox(dl, F, 2, 2, 270, 86, K, "MISC");
  w36(FmOctave, 8, 26, "COARSE", skin::WheelBigLine | skin::WheelCentered);
  w36(FmFeedback, 52, 26, "FEEDBK");
  w36(FmPitchEnv, 96, 26, "P. ENV");
  w36(FmGlide, 140, 26, "GLIDE");
  w36(FmVoices, 184, 26, "VOICES");
  w36(FmVolume, 228, 26, "VOLUME");
  panelBox(dl, F, 276, 2, 140, 86, K, "LFO");
  w36(FmLfoRate, 282, 26, "LFO RATE");
  w36(FmVibrato, 328, 26, "VIB");
  w36(FmLfoIndex, 374, 26, "> INDEX");
  panelBox(dl, F, 420, 2, 338, 86, K, "ALGORITHM");
  {
    static const char* nums[8] = {"1", "2", "3", "4", "5", "6", "7", "8"};
    skin::switchSel(a, t, FmAlgorithm, F.p(426, 20), F.sz(190, 22), K, nums, 8, true);
    std::string nm = describeParam(a.doc.song(), t.pid(FmAlgorithm))->format(val(st, FmAlgorithm));
    skin::lcd(dl, F.p(426, 48), F.p(616, 64), K, nm.c_str(), lf(F, 8));
    drawAlgoSkin(dl, ival(st, FmAlgorithm), F.p(622, 8), F.sz(130, 76), K, F.s);
  }
  // ---- operator columns
  for (int o = 0; o < kFmOps; ++o) {
    const float x = 2 + o * 189.5f, y = 92;
    char title[16];
    std::snprintf(title, sizeof title, "OPERATOR %d", o + 1);
    ImGui::PushID(o + 4000);
    panelBox(dl, F, x, y, 186, 284, K, title);
    skin::check(a, t, fmExt(o, FmFixed), F.p(x + 120, y + 3), F.sz(60, 12), K, "FIXED");
    const float c0 = x + 6, dx = 45;
    w36(fmParam(o, FmRatio), c0, y + 20, "COARSE");
    w36(fmParam(o, FmFine), c0 + dx, y + 20, "FINE", skin::WheelBigLine | skin::WheelCentered);
    w36(fmParam(o, FmVel), c0 + dx * 2, y + 20, "VEL.SENS");
    w36(fmParam(o, FmLevel), c0 + dx * 3, y + 20, "LEVEL");
    skin::EnvParams e;
    e.att = fmParam(o, FmAttack), e.dec = fmParam(o, FmDecay), e.sus = fmParam(o, FmSustain), e.rel = fmParam(o, FmRelease);
    skin::envelope(a, t, e, F.p(x + 4, y + 70), F.sz(178, 46), K);
    w36(fmParam(o, FmAttack), c0, y + 122, "ATT");
    w36(fmParam(o, FmDecay), c0 + dx, y + 122, "DEC");
    w36(fmParam(o, FmSustain), c0 + dx * 2, y + 122, "SUS");
    w36(fmParam(o, FmRelease), c0 + dx * 3, y + 122, "REL");
    w36(fmExt(o, FmWave), c0, y + 172, "WAVE");
    w36(fmExt(o, FmThru), c0 + dx, y + 172, "THRU");
    w36(fmExt(o, FmOffset), c0 + dx * 2, y + 172, "OFFSET");
    w36(fmExt(o, FmPan), c0 + dx * 3, y + 172, "PAN", skin::WheelBigLine | skin::WheelCentered);
    w36(fmExt(o, FmKeyScale), c0, y + 222, "KEY SCL");
    w36(fmExt(o, FmPhase), c0 + dx, y + 222, "PHASE");
    w36(fmExt(o, FmInit), c0 + dx * 2, y + 222, "INIT");
    ImGui::PopID();
  }
  // ---- keyboard
  skin::well(dl, F.p(2, 380), F.p(758, 460), K, 3 * F.s);
  auditionKeyboard(a, t, F, 5, 383, 750, 74, 36, 35, K);
  endForm(F, width, kH);
}

// ======================================================================= Sampler
// The channel sampler has no plugin form; its pages follow the reference
// channel-settings family: tabs on top, a waveform with region / loop markers,
// then wells of wheels, checks and selectors.
const skin::Skin& samplerSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x3A4246, 0x454E53, 0x2C3337, 0xFFA830, 0x3F484D, 0xB9BFC3);
    s.graphCurve = skin::hex(0xC9D2D6);
    s.accent2 = skin::hex(0x7FF0AA);
    return s;
  }();
  return k;
}
struct SamplerUi {
  int tab = 0;
  int drag = -1;
};
std::map<uint64_t, SamplerUi> samplerUi;

void lfoIcon(ImDrawList* dl, int i, ImVec2 c, float w, float h, ImU32 col) {
  static const int shapes[3] = {0, 1, 3};
  waveIcon(dl, shapes[std::clamp(i, 0, 2)], c, w, h, col);
}

void editorSamplerFl(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  const skin::Skin& K = samplerSkin();
  SamplerUi& ui = samplerUi[t.key()];
  constexpr float kW = 680, kH = 300;
  skin::Form F = fitForm(width, kW, kH, 440);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  static const char* tabs[3] = {"SAMPLE", "ENVELOPE / FILTER", "MISC"};
  for (int k = 0; k < 3; ++k) {
    char id[16];
    std::snprintf(id, sizeof id, "##smptab%d", k);
    if (skin::button(id, F.p(6 + k * 140.0f, 5), F.sz(134, 20), K, tabs[k], ui.tab == k, lf(F, 8))) ui.tab = k;
  }
  auto w = [&](int p, float x, float y, const char* l, int flags = skin::WheelBigLine) { wheelL(a, t, p, F, x, y, 36, K, K, l, flags); };
  if (ui.tab == 0) {
    // Region / loop editor (drag the markers).
    SamplePtr smp = st.data.empty() ? nullptr : sampleCache().get(st.data);
    ImVec2 p0 = F.p(6, 31), p1 = F.p(674, 131);
    ImGui::SetCursorScreenPos(p0);
    ImGui::InvisibleButton("##swave", ImVec2(p1.x - p0.x, p1.y - p0.y));
    sampleWell(dl, p0, p1, K, smp.get());
    const float sw = p1.x - p0.x, sh = p1.y - p0.y;
    if (!smp) skin::text(dl, ImVec2(p0.x + 12, p0.y + sh * 0.5f - 6), K.graphText, "Load or drop a sample", lf(F, 8));
    struct Mk {
      int param;
      ImU32 col;
      const char* label;
    };
    const bool loop = bval(st, SmpLoop);
    const Mk mks[4] = {{SmpStart, K.graphCurve, "START"}, {SmpEnd, K.graphCurve, "END"}, {SmpLoopStart, K.fill, "LOOP"}, {SmpLoopEnd, K.fill, ""}};
    const float sx = p0.x + val(st, SmpStart) * sw, ex = p0.x + val(st, SmpEnd, 1) * sw;
    dl->AddRectFilled(p0, ImVec2(sx, p1.y), theme::withAlpha(K.graphBg, 0.7f));
    dl->AddRectFilled(ImVec2(ex, p0.y), p1, theme::withAlpha(K.graphBg, 0.7f));
    if (loop) {
      float lx0 = p0.x + val(st, SmpLoopStart) * sw, lx1 = p0.x + val(st, SmpLoopEnd, 1) * sw;
      dl->AddRectFilled(ImVec2(lx0, p1.y - 9), ImVec2(lx1, p1.y - 4), theme::withAlpha(K.fill, 0.6f), 2);
    }
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsItemActivated()) {
      ui.drag = -1;
      float best = 10;
      for (int k = 0; k < 4; ++k) {
        if (k >= 2 && !loop) continue;
        float x = p0.x + val(st, mks[k].param, k % 2 ? 1.f : 0.f) * sw;
        if (std::fabs(io.MousePos.x - x) < best) best = std::fabs(io.MousePos.x - x), ui.drag = k;
      }
    }
    for (int k = 0; k < 4; ++k) {
      if (k >= 2 && !loop) continue;
      float x = p0.x + val(st, mks[k].param, k % 2 ? 1.f : 0.f) * sw;
      dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), mks[k].col, ui.drag == k && ImGui::IsItemActive() ? 2.5f : 1.5f);
      if (mks[k].label[0]) skin::text(dl, ImVec2(k == 1 ? x - 30 : x + 3, p0.y + 3 + (k >= 2 ? 12 : 0)), mks[k].col, mks[k].label, lf(F, 7));
    }
    if (ImGui::IsItemActive() && ui.drag >= 0 && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1))
      a.doc.setParam(t.pid(mks[ui.drag].param), std::clamp((io.MousePos.x - p0.x) / sw, 0.0f, 1.0f), true);
    if (ImGui::IsItemDeactivated()) a.doc.endGesture();
    if (ImGui::IsItemHovered()) a.setHint("Drag the start / end and loop markers");
    // PLAYBACK
    panelBox(dl, F, 6, 137, 214, 157, K, "PLAYBACK");
    skin::check(a, t, SmpLoop, F.p(14, 158), F.sz(60, 17), K, "LOOP");
    skin::menuBox(a, t, SmpLoopMode, F.p(80, 156), F.sz(132, 18), K, lf(F, 8));
    skin::check(a, t, SmpReverse, F.p(14, 184), F.sz(80, 17), K, "REVERSE");
    skin::check(a, t, SmpOneShot, F.p(110, 184), F.sz(90, 17), K, "ONE SHOT");
    skin::text(dl, F.p(14, 213), K.textDim, "DECLICK", lf(F, 7.5f));
    skin::menuBox(a, t, SmpDeclick, F.p(80, 210), F.sz(132, 18), K, lf(F, 8));
    // TIME STRETCHING
    panelBox(dl, F, 226, 137, 200, 157, K, "TIME STRETCHING");
    {
      static const char* tm[2] = {"RESAMPLE", "STRETCH"};
      skin::switchSel(a, t, SmpStretchMode, F.p(234, 156), F.sz(184, 20), K, tm, 2, true);
    }
    w(SmpPitchShift, 256, 190, "PITCH", skin::WheelBigLine | skin::WheelCentered);
    w(SmpTimeStretch, 352, 190, "TIME");
    // PROCESSING
    panelBox(dl, F, 432, 137, 242, 157, K, "PROCESSING");
    skin::check(a, t, SmpNormalize, F.p(440, 158), F.sz(110, 17), K, "NORMALIZE");
    skin::check(a, t, SmpRemoveDc, F.p(556, 158), F.sz(110, 17), K, "REMOVE DC");
    skin::check(a, t, SmpPolarity, F.p(440, 178), F.sz(110, 17), K, "POLARITY");
    skin::check(a, t, SmpSwapStereo, F.p(556, 178), F.sz(110, 17), K, "SWAP STEREO");
    w(SmpFadeIn, 452, 210, "FADE IN");
    w(SmpFadeOut, 522, 210, "FADE OUT");
    w(SmpLoopXfade, 592, 210, "X-FADE");
  } else if (ui.tab == 1) {
    panelBox(dl, F, 6, 31, 290, 263, K, "VOLUME ENVELOPE");
    skin::EnvParams e;
    e.del = SmpEnvDelay, e.att = SmpAttack, e.hold = SmpEnvHold, e.dec = SmpDecay, e.sus = SmpSustain, e.rel = SmpRelease;
    skin::envelope(a, t, e, F.p(12, 50), F.sz(278, 110), K);
    w(SmpEnvDelay, 14, 168, "DEL");
    w(SmpAttack, 60, 168, "ATT");
    w(SmpEnvHold, 106, 168, "HOLD");
    w(SmpDecay, 152, 168, "DEC");
    w(SmpSustain, 198, 168, "SUS");
    w(SmpRelease, 244, 168, "REL");
    w(SmpVelVolume, 14, 226, "VEL");
    panelBox(dl, F, 302, 31, 170, 263, K, "LFO");
    {
      static const char* tg[5] = {"OFF", "VOL", "PAN", "CUT", "PITCH"};
      skin::switchSel(a, t, SmpLfoTarget, F.p(310, 50), F.sz(154, 20), K, tg, 5, true);
    }
    skin::iconSel(a, t, SmpLfoShape, F.p(310, 78), F.sz(154, 24), K, 3, lfoIcon, true);
    w(SmpLfoRate, 312, 114, "SPEED");
    w(SmpLfoAmount, 368, 114, "AMOUNT", skin::WheelBigLine | skin::WheelCentered);
    w(SmpLfoDelay, 424, 114, "DELAY");
    panelBox(dl, F, 478, 31, 196, 263, K, "FILTER");
    {
      static const char* ft[6] = {"OFF", "LP", "LP24", "BP", "HP", "NOTCH"};
      skin::switchSel(a, t, SmpFilterType, F.p(486, 50), F.sz(180, 20), K, ft, 6, true);
    }
    w(SmpCutoff, 490, 78, "CUT");
    w(SmpReso, 552, 78, "RES");
    w(SmpFiltEnv, 614, 78, "ENV", skin::WheelBigLine | skin::WheelCentered);
    skin::EnvParams fe;
    fe.att = SmpFiltAttack, fe.dec = SmpFiltDecay, fe.sus = SmpFiltSustain, fe.rel = SmpFiltRelease, fe.amount = SmpFiltEnv;
    skin::envelope(a, t, fe, F.p(486, 132), F.sz(180, 50), K);
    w(SmpFiltAttack, 486, 188, "A");
    w(SmpFiltDecay, 531, 188, "D");
    w(SmpFiltSustain, 576, 188, "S");
    w(SmpFiltRelease, 621, 188, "R");
    w(SmpVelCutoff, 510, 240, "VEL");
    w(SmpKeyCutoff, 590, 240, "KEY", skin::WheelBigLine | skin::WheelCentered);
  } else {
    panelBox(dl, F, 6, 31, 668, 263, K, "ROOT NOTE · VOICE");
    w(SmpRoot, 20, 54, "ROOT");
    w(SmpFine, 90, 54, "FINE", skin::WheelBigLine | skin::WheelCentered);
    w(SmpPan, 160, 54, "PAN", skin::WheelBigLine | skin::WheelCentered);
    w(SmpVoices, 230, 54, "VOICES");
    w(SmpGlide, 300, 54, "GLIDE");
    w(SmpVolume, 370, 54, "VOLUME");
    {
      std::string r = noteName(static_cast<int>(val(st, SmpRoot, 60) + 0.5f));
      skin::text(dl, F.p(450, 60), K.textDim, "ROOT NOTE", lf(F, 7.5f));
      skin::lcd(dl, F.p(450, 74), F.p(530, 92), K, r.c_str(), lf(F, 9));
    }
    auditionKeyboard(a, t, F, 12, 120, 656, 168, 24, 49, K, static_cast<int>(val(st, SmpRoot, 60) + 0.5f), SmpRoot);
  }
  endForm(F, width, kH);
}

// ======================================================================= Granular
const skin::Skin& granSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x454B4F, 0x4C5256, 0x3B4144, 0xB1E758, 0x62676A, 0xB9BFC3);
    s.textDim = skin::hex(0x8D989E);
    s.pillBg = skin::hex(0x31373B);
    s.pillText = skin::hex(0x8D989E);
    s.graphBg = skin::hex(0x21272B);
    s.graphCurve = skin::hex(0xBEC6CA);
    return s;
  }();
  return k;
}

void editorGranularFl(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  const skin::Skin& K = granSkin();
  constexpr float kW = 392, kH = 500;
  skin::Form F = fitForm(width, kW, kH, 560);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  // Wheel with the reference's pill label underneath (panel-relative coordinates).
  auto gw = [&](int p, float px, float py, float x, float y, const char* l, float size = 56) {
    skin::wheel(a, t, p, F.p(px + x, py + y), size * F.s, K, skin::WheelBigLine);
    ImGui::PushFont(a.fontMonoBold, lf(F, 7.0f));
    float tw = ImGui::CalcTextSize(l).x / F.s + 8;
    ImGui::PopFont();
    pillAt(dl, F, px + x + size * 0.5f - tw * 0.5f, py + y + size - 1, tw, 13, K, l);
  };
  // GRAINS
  panelBox(dl, F, 6, 6, 380, 99, K, "GRAINS");
  gw(GrSize, 6, 6, 19, 22, "SIZE");
  gw(GrShape, 6, 6, 83, 22, "SHAPE");
  gw(GrDensity, 6, 6, 149, 22, "GR. SPACING");
  gw(GrSpread, 6, 6, 218, 22, "SPREAD");
  {
    ImVec2 p0 = F.p(305, 27), p1 = F.p(376, 93);
    skin::graphWell(dl, p0, p1, K);
    const double edge = std::clamp(static_cast<double>(val(st, GrShape, 0.5f)), 0.02, 0.5);
    ImVec2 pts[40];
    for (int i = 0; i < 40; ++i) {
      double ph = i / 39.0;
      double v = ph < edge ? 0.5 - 0.5 * std::cos(dsp::kPi * ph / edge) : ph > 1 - edge ? 0.5 - 0.5 * std::cos(dsp::kPi * (1 - ph) / edge) : 1.0;
      pts[i] = ImVec2(p0.x + 4 + (p1.x - p0.x - 8) * static_cast<float>(ph), p1.y - 5 - static_cast<float>(v) * (p1.y - p0.y - 10));
    }
    dl->AddPolyline(pts, 40, K.fill, 0, 1.5f);
  }
  // EFFECTS
  panelBox(dl, F, 6, 110, 380, 99, K, "EFFECTS");
  gw(GrStereo, 6, 110, 19, 22, "PAN");
  gw(GrWobble, 6, 110, 83, 22, "DEPTH");
  gw(GrWobbleRate, 6, 110, 149, 22, "SPEED");
  gw(GrJitter, 6, 110, 218, 22, "RAND");
  gw(GrPitchRand, 6, 110, 299, 22, "PITCH RND");
  // TRANSIENTS · TIME
  panelBox(dl, F, 6, 214, 184, 99, K, "TRANSIENTS");
  gw(GrTransientHold, 6, 214, 19, 22, "HOLD");
  gw(GrSpeed, 6, 214, 100, 22, "PLAY SPD");
  panelBox(dl, F, 196, 214, 190, 99, K, "TIME");
  gw(GrPosition, 196, 214, 10, 22, "START");
  skin::check(a, t, GrLoop, F.p(238, 217), F.sz(48, 12), K, "LOOP");
  skin::check(a, t, GrTempoSync, F.p(290, 217), F.sz(94, 12), K, "TEMPO LOCK");
  skin::toggle(a, t, GrFreeze, F.p(306, 238), F.sz(72, 24), K, "HOLD", lf(F, 8));
  skin::text(dl, F.p(266, 272), K.textDim, "KEY TO", lf(F, 7.0f));
  skin::menuBox(a, t, GrKeyMode, F.p(306, 286), F.sz(72, 20), K, lf(F, 7.5f));
  // VOICE
  panelBox(dl, F, 6, 318, 380, 76, K, "VOICE");
  auto vw = [&](int p, float x, const char* l) { gw(p, 6, 318, x, 18, l, 40); };
  vw(GrBaseNote, 22, "BASE");
  vw(GrPitch, 94, "PITCH");
  vw(GrAttack, 166, "ATT");
  vw(GrRelease, 238, "REL");
  vw(GrVolume, 310, "VOL");
  // WAVE scope (drag sets the position; drop an audio file to load it)
  SamplePtr smp = st.data.empty() ? nullptr : sampleCache().get(st.data);
  ImVec2 p0 = F.p(6, 399), p1 = F.p(386, 496);
  ImGui::SetCursorScreenPos(p0);
  ImGui::InvisibleButton("##gwave", ImVec2(p1.x - p0.x, p1.y - p0.y));
  std::string dropped = acceptAudioDrop();
  if (!dropped.empty()) {
    PluginState ns = st;
    ns.data = dropped;
    setPluginState(a, t, ns, "load sample");
  }
  sampleWell(dl, p0, p1, K, smp.get());
  const float sw = p1.x - p0.x;
  const float pos = val(st, GrPosition, 0.3f), spread = val(st, GrSpread, 0.1f);
  const float x0 = p0.x + pos * sw, x1 = std::min(p1.x, x0 + spread * sw);
  dl->AddRectFilled(ImVec2(x0, p0.y), ImVec2(std::max(x0 + 2, x1), p1.y), theme::withAlpha(K.fill, 0.22f));
  dl->AddLine(ImVec2(x0, p0.y), ImVec2(x0, p1.y), K.fill, 2);
  if (!smp) skin::text(dl, ImVec2(p0.x + 12, (p0.y + p1.y) * 0.5f - 6), K.graphText, "Drop an audio file here", lf(F, 8));
  if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1))
    a.doc.setParam(t.pid(GrPosition), std::clamp((ImGui::GetIO().MousePos.x - p0.x) / sw, 0.0f, 1.0f), true);
  if (ImGui::IsItemDeactivated()) a.doc.endGesture();
  if (ImGui::IsItemHovered()) a.setHint("Drag to set the grain position · drop an audio file to load");
  if (skin::button("##gopen", F.p(362, 401), F.sz(22, 16), K, "...", false, lf(F, 8))) {
    PluginTarget tt = t;
    openSampleDialog(a, "Load sample", [&a, tt](const std::string& path) {
      const PluginState* cur = tt.state(a.doc.song());
      if (!cur) return;
      PluginState ns = *cur;
      ns.data = path;
      setPluginState(a, tt, ns, "load sample");
    });
  }
  if (ImGui::IsItemHovered()) a.setHint("Open an audio file");
  endForm(F, width, kH);
}

// ======================================================================= MIDI Out
const skin::Skin& midiSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x31373B, 0x363C40, 0x3B4144, 0xFFA830, 0x52575A, 0xB9BFC3);
    s.fillPressed = skin::hex(0xFF7E31);
    s.lcdBg = skin::hex(0x723C43);
    s.lcdText = skin::hex(0xFFEAAA);
    s.textDim = skin::hex(0x9AA1A6);
    return s;
  }();
  return k;
}

void editorMidiOutFl(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  const skin::Skin& K = midiSkin();
  constexpr float kW = 558, kH = 389;
  skin::Form F = fitForm(width, kW, kH, 440);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  const float fl = lf(F, 9.5f);
  // Header selectors.
  skin::text(dl, F.p(7, 16), K.text, "Channel", fl);
  skin::digi(a, t, MoChannel, F.p(58, 10), F.sz(32, 25), K);
  skin::text(dl, F.p(108, 16), K.text, "Patch", fl);
  skin::digi(a, t, MoProgram, F.p(144, 10), F.sz(60, 25), K);
  skin::text(dl, F.p(342, 16), K.text, "Bank", fl);
  skin::digi(a, t, MoBankMsb, F.p(376, 10), F.sz(40, 25), K);
  skin::digi(a, t, MoBankLsb, F.p(420, 10), F.sz(40, 25), K);
  skin::text(dl, F.p(482, 16), K.text, "Port", fl);
  skin::digi(a, t, MoPort, F.p(513, 10), F.sz(40, 25), K);
  if (a.midi) {
    const bool none = a.midi->output.empty();
    std::string s = none ? "no output set" : "→ " + a.midi->output;
    dl->PushClipRect(F.p(210, 10), F.p(338, 36), true);
    skin::text(dl, F.p(212, 16), none ? skin::hex(0xE5484D) : K.textDim, s.c_str(), lf(F, 7.0f));
    dl->PopClipRect();
  }
  // Controller page: 3 × 3 wheels (8 controllers + note velocity).
  skin::well(dl, F.p(0, 46), F.p(558, 349), K, 3 * F.s);
  static const char* types[5] = {"CC", "RPN", "NRPN", "AT", "Note"};
  for (int k = 0; k < 9; ++k) {
    const float x = 83 + (k % 3) * 152.0f, y = 51 + (k / 3) * 84.0f;
    ImGui::PushID(k + 1000);
    if (k < kMoKnobs) {
      skin::wheel(a, t, moKnob(k, MoKnobValue), F.p(x, y), 70 * F.s, K, skin::WheelBigLine);
      char lbl[24];
      std::snprintf(lbl, sizeof lbl, "%s %d", types[std::clamp(ival(st, moKnob(k, MoKnobType)), 0, 4)], ival(st, moKnob(k, MoKnobNumber)));
      if (skin::button("##cfg", F.p(x + 72, y + 4), F.sz(78, 20), K, lbl, false, lf(F, 8))) ImGui::OpenPopup("##knobcfg");
      if (ImGui::IsItemHovered()) a.setHint("Set up this controller (type, number, channel, range)");
      std::string v = describeParam(a.doc.song(), t.pid(moKnob(k, MoKnobValue)))->format(val(st, moKnob(k, MoKnobValue)));
      skin::text(dl, F.p(x + 74, y + 30), skin::hex(0x797F83), v.c_str(), lf(F, 8));
      if (ImGui::BeginPopup("##knobcfg")) {
        ImGui::TextDisabled("Controller %d", k + 1);
        pui::segmented(a, t, moKnob(k, MoKnobType), 260, types);
        pui::stepper(a, t, moKnob(k, MoKnobNumber), 110, "Number");
        pui::stepper(a, t, moKnob(k, MoKnobChannel), 110, "Channel (0 = plugin)");
        pui::stepper(a, t, moKnob(k, MoKnobMin), 110, "Min");
        pui::stepper(a, t, moKnob(k, MoKnobMax), 110, "Max");
        ImGui::EndPopup();
      }
    } else {
      skin::wheel(a, t, MoVelocity, F.p(x, y), 70 * F.s, K, skin::WheelBigLine);
      skin::text(dl, F.p(x + 74, y + 8), K.text, "Velocity", lf(F, 8));
      std::string v = describeParam(a.doc.song(), t.pid(MoVelocity))->format(val(st, MoVelocity));
      skin::text(dl, F.p(x + 74, y + 30), skin::hex(0x797F83), v.c_str(), lf(F, 8));
    }
    ImGui::PopID();
  }
  // Bottom bar: bend range + velocity curve.
  skin::text(dl, F.p(6, 362), K.text, "Bend", fl);
  skin::digi(a, t, MoBendRange, F.p(40, 356), F.sz(44, 25), K);
  skin::text(dl, F.p(100, 362), K.text, "Velocity curve", fl);
  static const char* vc[4] = {"LINEAR", "SOFT", "HARD", "FIXED"};
  skin::switchSel(a, t, MoVelCurve, F.p(210, 356), F.sz(300, 25), K, vc, 4, true);
  endForm(F, width, kH);
}

}  // namespace

bool drawFlSynthEditor2(App& a, const PluginTarget& t, float width) {
  const PluginState* st = t.state(a.doc.song());
  if (!st) return false;
  const std::string& ty = st->type;
  if (ty == "gl.fm") editorFmFl(a, t, width);
  else if (ty == "gl.sampler") editorSamplerFl(a, t, width);
  else if (ty == "gl.granular") editorGranularFl(a, t, width);
  else if (ty == "gl.midiout") editorMidiOutFl(a, t, width);
  else return false;
  return true;
}

}  // namespace gl
