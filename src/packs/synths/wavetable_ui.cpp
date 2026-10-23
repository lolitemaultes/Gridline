// Gridline — "synths" pack: Wavetable editor (J-19). No reference
// counterpart: drawn in the reference family's style on a 1120 × 646 grid
// (dark panel, teal values, rail sliders, wheels, LCD menus) — voice / master
// / macros strip with scope on top, the two wavetable oscillators with their
// stacked-frame displays and sub / noise, the dual filter with both
// responses, paged envelope and LFO editors, the modulation matrix and the
// effects block.
#include "../../core/PluginBrand.h"
#include "../flc_kit.h"
#include "synths_widgets.h"

namespace gl::packs::synths {

using namespace ui2;

namespace {

struct WtUi {
  int env = 0, lfo = 0, page = 0;
};
std::map<uint64_t, WtUi> uiState;

double warpPhase(int warp, double a, double p) {
  switch (warp) {
    case WarpBendPlus:
    case WarpBendMinus: {
      double mid = warp == WarpBendPlus ? 0.5 - 0.45 * a : 0.5 + 0.45 * a;
      return p < mid ? 0.5 * p / mid : 0.5 + 0.5 * (p - mid) / (1.0 - mid);
    }
    case WarpSync: return std::fmod(p * (1.0 + 7.0 * a), 1.0);
    default: return p;
  }
}

double sampleFrame(const float* f, double p) {
  double x = std::clamp(p, 0.0, 0.999999) * kWtSize;
  int i = static_cast<int>(x);
  double t = x - i;
  return f[i] + (f[(i + 1) & (kWtSize - 1)] - f[i]) * t;
}

// Stacked frames with the current (warped) position on top.
void tableDisplay(App& a, const PluginTarget& t, int k, ImVec2 size) {
  const PluginState& st = *t.state(a.doc.song());
  const auto& P = pal();
  ImVec2 p = well(size, "##table");
  ImDrawList* dl = ImGui::GetWindowDrawList();
  int table = ival(st, wtOsc(k, WoTable)), warp = ival(st, wtOsc(k, WoWarp));
  double pos = val(st, wtOsc(k, WoPos)), wa = val(st, wtOsc(k, WoWarpAmt));
  bool on = val(st, wtOsc(k, WoOn)) >= 0.5f;
  const float dx = 7, dy = 5, w = size.x - 16 - dx * (kWtFrames - 1), h = size.y - 14 - dy * (kWtFrames - 1);
  const int n = 96;
  ImVec2 pts[n];
  for (int f = kWtFrames - 1; f >= 0; --f) {
    const float* fr = wavetableFrame(table, f, 3);
    float ox = p.x + 8 + dx * f, oy = p.y + 7 + dy * (kWtFrames - 1 - f);
    for (int i = 0; i < n; ++i) {
      double ph = static_cast<double>(i) / (n - 1);
      pts[i] = ImVec2(ox + w * static_cast<float>(ph), oy + h * 0.5f - static_cast<float>(sampleFrame(fr, ph)) * h * 0.45f);
    }
    dl->AddPolyline(pts, n, theme::withAlpha(P.text, 0.16f), 0, 1.0f);
  }
  // Current position, warped, drawn on the front plane.
  double fpos = pos * (kWtFrames - 1);
  int f0 = std::min(kWtFrames - 2, static_cast<int>(fpos));
  double mix = fpos - f0;
  const float* a0 = wavetableFrame(table, f0, 3);
  const float* a1 = wavetableFrame(table, f0 + 1, 3);
  float ox = p.x + 8 + dx * static_cast<float>(fpos), oy = p.y + 7 + dy * static_cast<float>(kWtFrames - 1 - fpos);
  for (int i = 0; i < n; ++i) {
    double ph = static_cast<double>(i) / (n - 1);
    double y;
    if (warp == WarpMirror) {
      double q = ph < 0.5 ? 2 * ph : 2 - 2 * ph;
      auto rd = [&](double x) { return sampleFrame(a0, x) * (1 - mix) + sampleFrame(a1, x) * mix; };
      y = (1 - wa) * rd(ph) + wa * rd(q);
    } else {
      double q = warpPhase(warp, wa, ph);
      y = sampleFrame(a0, q) * (1 - mix) + sampleFrame(a1, q) * mix;
      if (warp == WarpCrush) {
        double steps = std::pow(2.0, 1.0 + (1.0 - wa) * 11.0);
        y = std::round(y * steps) / steps;
      }
    }
    pts[i] = ImVec2(ox + w * static_cast<float>(ph), oy + h * 0.5f - static_cast<float>(y) * h * 0.45f);
  }
  dl->AddPolyline(pts, n, on ? P.paper : P.textFaint, 0, 1.8f);
  char buf[48];
  std::snprintf(buf, sizeof buf, "frame %.2f / %d", fpos + 1, kWtFrames);
  ImGui::PushFont(a.fontMono, 10.0f);
  dl->AddText(ImVec2(p.x + size.x - ImGui::CalcTextSize(buf).x - 6, p.y + 4), P.textFaint, buf);
  ImGui::PopFont();
  if (ImGui::IsItemActive()) {  // drag horizontally to scan the table
    PluginState ns = st;
    double np = std::clamp(static_cast<double>((ImGui::GetIO().MousePos.x - p.x) / size.x), 0.0, 1.0);
    ns.params[static_cast<size_t>(wtOsc(k, WoPos))] = static_cast<float>(np);
    setPluginState(a, t, ns, "scan wavetable", 0x5754505300000000ull ^ t.key() ^ static_cast<uint64_t>(k));
  }
  if (ImGui::IsItemDeactivated()) a.doc.endGesture();
  if (ImGui::IsItemHovered()) a.setHint("Drag left/right to scan the wavetable position");
}

const skin::Skin& wavetableSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x24282B, 0x2E3337, 0x3E454A, 0x5FD4C4, 0x3A4146, 0xB9BFC3);
    s.handle = skin::hex(0x32383D);
    s.select = skin::hex(0x5FD4C4);
    s.accent2 = skin::hex(0xE8C77A);
    s.lcdBg = skin::hex(0x171A1C);
    s.lcdText = skin::hex(0x8FE8DC);
    s.pillBg = skin::hex(0x3A4045);
    s.graphBg = skin::hex(0x171A1C);
    s.graphGrid = skin::hex(0x262B2E);
    s.graphGridStrong = skin::hex(0x343A3E);
    s.graphCurve = skin::hex(0x8FE8DC);
    return s;
  }();
  return k;
}

void lfoGlyphW(ImDrawList* dl, int i, ImVec2 c, float w, float h, ImU32 col) {
  static const int g[7] = {0, 1, 5, 6, 3, 7, 8};
  glyph(dl, g[std::clamp(i, 0, 6)], c, w, h, col);
}

void oscSection(const flk::Ctx& c, const PluginState& st, int k, float x0) {
  flk::section(c, x0, 52, 442, 210, k == 0 ? "OSC A" : "OSC B");
  flk::chk(c, wtOsc(k, WoOn), x0 + 60, 55, 46, 12, "ON");
  (void)st;
  flk::text(c, x0 + 116, 57, "TABLE");
  flk::menu(c, wtOsc(k, WoTable), x0 + 152, 54, 120, 17);
  flk::text(c, x0 + 284, 57, "WARP");
  flk::menu(c, wtOsc(k, WoWarp), x0 + 314, 54, 120, 17);
  ImGui::SetCursorScreenPos(c.F.p(x0 + 8, 76));
  tableDisplay(c.a, c.t, k, c.F.sz(426, 82));
  const int r1[6] = {WoPos, WoWarpAmt, WoCoarse, WoFine, WoLevel, WoPan};
  const char* n1[6] = {"Position", "Warp", "Coarse", "Fine", "Level", "Pan"};
  for (int i = 0; i < 6; ++i) flk::wheel(c, wtOsc(k, r1[i]), x0 + 18 + i * 70, 164, 30, n1[i]);
  const int r2[5] = {WoUni, WoDetune, WoBlend, WoWidth, WoPhase};
  const char* n2[5] = {"Unison", "Detune", "Blend", "Width", "Phase"};
  for (int i = 0; i < 5; ++i) flk::wheel(c, wtOsc(k, r2[i]), x0 + 18 + i * 70, 210, 30, n2[i]);
  flk::tog(c, wtOsc(k, WoRandPhase), x0 + 362, 216, 70, 20, "RANDOM");
}

}  // namespace

void drawWavetableEditor(App& a, const PluginTarget& t, float W) {
  const PluginState* sp = t.state(a.doc.song());
  if (!sp) return;
  const PluginState st = *sp;
  WtUi& u = uiState[t.key()];
  const skin::Skin& K = wavetableSkin();
  constexpr float kW = 1120, kH = 646;
  ImGui::PushID("gl.wavetable");
  skin::Form F = skin::Form::fit(W, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  flk::Ctx c{a, t, F, K, dl};

  // ---- top strip: name, voice, master, macros, scope
  skin::text(dl, F.p(12, 14), K.select, pluginTitle("gl.wavetable").c_str(), F.font(13));
  static const char* modes[3] = {"POLY", "MONO", "LEGATO"};
  flk::sw(c, WtPlayMode, 120, 14, 150, 18, modes, 3, true);
  flk::text(c, 282, 8, "VOICES");
  flk::digi(c, WtVoices, 282, 22, 44, 16);
  flk::wheel(c, WtGlide, 340, 6, 26, "Glide");
  flk::wheel(c, WtVelAmp, 384, 6, 26, "Vel>Vol");
  flk::wheel(c, WtTranspose, 428, 6, 26, "Trans", skin::WheelBigLine | skin::WheelCentered);
  flk::wheel(c, WtVolume, 472, 6, 26, "Vol");
  flk::text(c, 530, 18, "MACROS");
  for (int m = 0; m < 4; ++m) {
    char lbl[12];
    std::snprintf(lbl, sizeof lbl, "Macro %d", m + 1);
    skin::Skin mk = K;
    mk.fill = K.accent2C();
    bool ch = skin::wheel(a, t, WtMacro1 + m, F.p(584 + m * 46, 6), 26 * F.s, mk, skin::WheelBigLine);
    (void)ch;
    flk::textC(c, 597 + m * 46, 34, lbl);
  }
  ImGui::SetCursorScreenPos(F.p(780, 4));
  liveScope(a, t, F.sz(336, 42));

  // ---- oscillators, sub / noise
  oscSection(c, st, 0, 4);
  oscSection(c, st, 1, 450);
  flk::section(c, 896, 52, 220, 210, "SUB + NOISE");
  static const char* oct[2] = {"-1 OCT", "-2 OCT"};
  static const char* wv[3] = {"SIN", "TRI", "SQR"};
  flk::sw(c, WtSubOct, 906, 76, 200, 18, oct, 2, true);
  flk::sw(c, WtSubWave, 906, 100, 200, 18, wv, 3, true);
  flk::wheel(c, WtSubLevel, 986, 128, 40, "Sub");
  flk::wheel(c, WtNoiseLevel, 930, 196, 34, "Noise");
  flk::wheel(c, WtNoiseColor, 1040, 196, 34, "Colour");

  // ---- filters
  flk::section(c, 4, 266, 600, 200, "FILTERS");
  flk::text(c, 100, 270, "ROUTING");
  static const char* routes[2] = {"SERIAL", "PARALLEL"};
  flk::sw(c, WtRouting, 154, 268, 150, 16, routes, 2, true);
  {
    ImVec2 p0 = F.p(10, 290), p1 = F.p(598, 360);
    ImVec2 sz(p1.x - p0.x, p1.y - p0.y);
    ImGui::SetCursorScreenPos(p0);
    ImGui::InvisibleButton("##flt", sz);
    filterDrag(a, t, p0, sz, wtFlt(0, WfCutoff), wtFlt(0, WfReso), 2);
    skin::graphWell(dl, p0, p1, K);
    pui::drawFreqGrid(dl, p0, p1, 24, false);
    for (int k = 1; k >= 0; --k)
      if (val(st, wtFlt(k, WfOn)) >= 0.5f)
        filterCurve(dl, p0, sz, ival(st, wtFlt(k, WfType)), val(st, wtFlt(k, WfCutoff)), val(st, wtFlt(k, WfReso)),
                    k ? K.accent2C() : K.graphCurve, false);
  }
  for (int k = 0; k < 2; ++k) {
    float y = 368 + k * 48;
    ImGui::PushID(k);
    flk::chk(c, wtFlt(k, WfOn), 12, y + 10, 36, 14, k ? "F2" : "F1");
    flk::menu(c, wtFlt(k, WfType), 54, y + 8, 110, 18);
    const int fp[4] = {WfCutoff, WfReso, WfDrive, WfKeyTrack};
    const char* fn[4] = {"Cutoff", "Reso", "Drive", "Key"};
    for (int i = 0; i < 4; ++i) flk::wheel(c, wtFlt(k, fp[i]), 190 + i * 100, y, 28, fn[i]);
    ImGui::PopID();
  }

  // ---- envelopes (paged)
  flk::section(c, 608, 266, 252, 200, "ENVELOPES");
  {
    static const char* names[4] = {"AMP", "ENV 2", "ENV 3", "ENV 4"};
    for (int e = 0; e < 4; ++e) {
      ImGui::PushID(800 + e);
      if (flk::btn(c, "##envtab", 700 + e * 39, 268, 37, 14, names[e], u.env == e)) u.env = e;
      ImGui::PopID();
    }
    int A = wtEnv(u.env, 0);
    skin::EnvParams ep;
    ep.att = A, ep.dec = A + 1, ep.sus = A + 2, ep.rel = A + 3;
    skin::envelope(a, t, ep, F.p(616, 288), F.sz(236, 76), K);
    static const char* n[4] = {"A", "D", "S", "R"};
    for (int i = 0; i < 4; ++i) flk::vsl(c, A + i, 640 + i * 52, 370, 23, 76, n[i]);
  }

  // ---- LFOs (paged)
  flk::section(c, 864, 266, 252, 200, "LFOS");
  {
    static const char* names[4] = {"LFO 1", "LFO 2", "LFO 3", "LFO 4"};
    for (int l = 0; l < 4; ++l) {
      ImGui::PushID(900 + l);
      if (flk::btn(c, "##lfotab", 924 + l * 47, 268, 45, 14, names[l], u.lfo == l)) u.lfo = l;
      ImGui::PopID();
    }
    int l = u.lfo;
    skin::iconSel(a, t, wtLfo(l, WlShape), F.p(872, 290), F.sz(236, 22), K, 7, lfoGlyphW);
    int shape = ival(st, wtLfo(l, WlShape));
    ImVec2 p0 = F.p(872, 318), p1 = F.p(1108, 360);
    skin::graphWell(dl, p0, p1, K);
    cycleGraph(dl, p0, ImVec2(p1.x - p0.x, p1.y - p0.y), [&](double ph) { return lfoValue(shape, std::fmod(ph * 2, 1.0), ph < 0.5 ? 0 : 1, 7) * 0.9; },
               K.graphCurve, 1.3f);
    bool sync = val(st, wtLfo(l, WlSync)) >= 0.5f;
    flk::wheel(c, sync ? wtLfo(l, WlDiv) : wtLfo(l, WlRate), 884, 372, 40, sync ? "Division" : "Rate");
    flk::chk(c, wtLfo(l, WlSync), 950, 374, 120, 14, "TEMPO SYNC");
    flk::chk(c, wtLfo(l, WlRetrig), 950, 394, 120, 14, "RETRIGGER");
    int used = 0;
    for (int r = 0; r < kWtMatrixRows; ++r)
      if (ival(st, wtMx(r, 0)) == WsLfo1 + l && std::fabs(val(st, wtMx(r, 2))) > 1e-4f) ++used;
    char buf[64];
    std::snprintf(buf, sizeof buf, used ? "ROUTED IN %d SLOT%s" : "NOT ROUTED: USE THE MATRIX", used, used == 1 ? "" : "S");
    flk::text(c, 950, 420, buf, K.textDim, 7.5f);
  }

  // ---- matrix, effects
  flk::section(c, 4, 470, 736, 172, "MODULATION MATRIX");
  for (int r = 0; r < kWtMatrixRows; ++r) {
    float x0 = 12 + (r / 4) * 364, y = 494 + (r % 4) * 36;
    ImGui::PushID(r);
    flk::menu(c, wtMx(r, 0), x0, y, 140, 20);
    flk::menu(c, wtMx(r, 1), x0 + 146, y, 160, 20);
    skin::wheel(a, t, wtMx(r, 2), F.p(x0 + 314, y - 4), 28 * F.s, K, skin::WheelBigLine | skin::WheelCentered);
    ImGui::PopID();
  }
  flk::section(c, 744, 470, 372, 172, "EFFECTS");
  flk::wheel(c, WtFxDrive, 754, 512, 38, "Drive");
  struct G {
    const char* name;
    int on, k1, k2, k3;
    const char *l1, *l2, *l3;
  };
  const G groups[3] = {{"CHORUS", FxChoOn, FxChoRate, FxChoDepth, FxChoMix, "RATE", "DEPTH", "MIX"},
                       {"DELAY", FxDlyOn, FxDlyDiv, FxDlyFb, FxDlyMix, "TIME", "FDBK", "MIX"},
                       {"REVERB", FxRevOn, FxRevSize, FxRevDamp, FxRevMix, "SIZE", "DAMP", "MIX"}};
  for (int g = 0; g < 3; ++g) {
    float x0 = 806 + g * 104;
    flk::chk(c, WtFxDrive + groups[g].on, x0, 490, 96, 14, groups[g].name);
    flk::vsl(c, WtFxDrive + groups[g].k1, x0, 512, 23, 104, groups[g].l1);
    flk::vsl(c, WtFxDrive + groups[g].k2, x0 + 32, 512, 23, 104, groups[g].l2);
    flk::vsl(c, WtFxDrive + groups[g].k3, x0 + 64, 512, 23, 104, groups[g].l3);
  }
  F.end(kW, kH);
  ImGui::PopID();
}

}  // namespace gl::packs::synths
