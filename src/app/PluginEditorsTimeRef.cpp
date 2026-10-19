// Gridline — Reverb and Delay editors on the reference panels' grids
// (reverb 530 × 160, delay 747 × 217 panel units), drawn with per-plugin
// rack skins. Positions are panel units scaled to the window width.
#include <map>
#include "../engine/Dsp.h"
#include "PluginEditorsCommon.h"
#include "PluginSkin.h"

namespace gl {

using theme::pal;
using namespace pp;

namespace {

using namespace ed;

// Wheel with a small caption under it (centred on the wheel).
void wheelL(App& a, const PluginTarget& t, int param, const skin::Form& F, float x, float y, float size, const skin::Skin& k, const char* label,
            int flags = 0, ImU32 fill = 0) {
  skin::wheel(a, t, param, F.p(x, y), size * F.s, k, flags, fill);
  if (label) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    skin::label(dl, F.p(x + size * 0.5f, 0).x, F.p(0, y + size + 1.5f).y, label, k.textDim, std::clamp(7.5f * F.s, 10.0f, 13.0f));
  }
}

// ======================================================================= Reverb
const skin::Skin& reverbSkin() {
  static skin::Skin k = [] {
    skin::Skin s = skin::Skin::make(0x434B4D, 0x3F484D, 0x9B575D, 0xB06A70, 0x2E3538, 0x959D9F);
    s.panelHi = skin::hex(0x4A5355);
    s.panelLo = skin::hex(0x212526);
    s.graphBg = skin::hex(0x1B1F21);
    s.graphGrid = skin::hex(0x2B3133);
    s.graphGridStrong = skin::hex(0x3A4245);
    s.graphText = skin::hex(0x959D9F);
    s.lcdBg = skin::hex(0x212526);
    s.lcdText = skin::hex(0xD6A2A6);
    s.handle = skin::hex(0x93969F);
    s.select = skin::hex(0xC77A80);
    return s;
  }();
  return k;
}

// Room visual (the reference's left box): a slowly turning wireframe prism —
// more sides with more diffusion, wider with a bigger room, glowing brighter
// with more wet signal — over the plugin's fill colour.
void roomVisual(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const PluginState& st, const skin::Skin& k) {
  const float w = p1.x - p0.x, h = p1.y - p0.y;
  dl->AddRectFilled(p0, p1, skin::darken(k.fill, 0.55f), 3);
  dl->AddRectFilledMultiColor(p0, ImVec2(p1.x, p0.y + h * 0.5f), theme::withAlpha(skin::lighten(k.fill, 0.1f), 0.35f),
                              theme::withAlpha(skin::lighten(k.fill, 0.1f), 0.35f), 0, 0);
  const double diff = val(st, RvbDiffusion, 1), size = val(st, RvbSize, 0.5f), wet = std::clamp(val(st, RvbWet, 0.5f), 0.0f, 1.0f);
  const int sides = std::clamp(3 + static_cast<int>(std::lround(std::exp(diff * std::log(26.0)) - 1.0)), 3, 28);
  const double radius = size * 10.9 + 2.0, ringGap = 45.0 / radius + 5.0;
  const double t0 = ImGui::GetTime() * 0.35;
  const double cx = (p0.x + p1.x) * 0.5, cy = (p0.y + p1.y) * 0.5, scale = std::min(w, h) * 3.125 / 3.0;
  ImVec2 v[2][28];
  float depth[2][28];
  for (int ring = 0; ring < 2; ++ring)
    for (int i = 0; i < sides; ++i) {
      double an = t0 + i * 2.0 * dsp::kPi / sides + ring * diff * 0.785;
      // prism axis along z, spun about it, then tilted towards the viewer
      double x = std::cos(an) * radius, y = std::sin(an) * radius, z = (ring - 0.5) * ringGap;
      const double tilt = 1.05;
      double Y = y * std::cos(tilt) - z * std::sin(tilt), Z = y * std::sin(tilt) + z * std::cos(tilt);
      double pz = Z + 33.333;
      v[ring][i] = ImVec2(static_cast<float>(cx + x * scale / pz), static_cast<float>(cy + Y * scale / pz));
      depth[ring][i] = static_cast<float>(std::clamp(0.6 - Z / (2.0 * radius + ringGap), 0.15, 1.0));
    }
  const ImU32 glow = skin::lighten(k.fill, 0.75f);
  auto edge = [&](ImVec2 a, ImVec2 b, float d) {
    float al = static_cast<float>(0.25 + 0.75 * wet) * d;
    dl->AddLine(a, b, theme::withAlpha(glow, al * 0.35f), 4.0f);
    dl->AddLine(a, b, theme::withAlpha(glow, al), 1.4f);
  };
  for (int ring = 0; ring < 2; ++ring)
    for (int i = 0; i < sides; ++i) edge(v[ring][i], v[ring][(i + 1) % sides], 0.5f * (depth[ring][i] + depth[ring][(i + 1) % sides]));
  for (int i = 0; i < sides; ++i) edge(v[0][i], v[1][i], 0.5f * (depth[0][i] + depth[1][i]));
  dl->AddRect(p0, p1, skin::darken(k.panel, 0.6f), 3, 0, 1.0f);
  char buf[48];
  std::snprintf(buf, sizeof buf, "%.1f s", val(st, RvbDecayTime, 1.5f));
  monoText(dl, ImVec2(p0.x + 5, p1.y - h * 0.12f), theme::withAlpha(glow, 0.85f), buf, std::clamp(h * 0.07f, 9.0f, 12.0f));
  if (ival(st, RvbFreeze)) monoText(dl, ImVec2(p1.x - h * 0.42f, p1.y - h * 0.12f), glow, ival(st, RvbFreeze) == 1 ? "FREEZE" : "SUSTAIN", std::clamp(h * 0.07f, 9.0f, 12.0f));
}

// Decay visual: pre-delay gap, early reflections, the tail's envelope (low
// band with the bass multiplier, high band with the damping), modulation.
void reverbVisual(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const PluginState& st, const skin::Skin& k, double tempo) {
  skin::graphWell(dl, p0, p1, k);
  const float h = p1.y - p0.y;
  double preMs = val(st, RvbPreDelay, 10);
  double pre = bval(st, RvbPreSync) ? preMs * 0.384 / 192.0 * 60.0 / std::max(20.0, tempo) : preMs * 0.001;
  double rt60 = val(st, RvbDecayTime, 2.3f), bass = val(st, RvbBassMult, 1), size = val(st, RvbSize, 0.6f);
  double early = val(st, RvbEarly, 0.3f), wet = val(st, RvbWet, 0.25f), dampHz = val(st, RvbDampHz, 22100);
  double span = std::max(0.5, pre + std::max(rt60, rt60 * bass) * 1.05);
  const float lx = p0.x + 6, rx = p1.x - 6, top = p0.y + h * 0.14f, base = p1.y - h * 0.16f;
  auto X = [&](double sec) { return lx + static_cast<float>(std::clamp(sec / span, 0.0, 1.0)) * (rx - lx); };
  // time grid (seconds)
  for (int sgrid = 1; sgrid < 40; ++sgrid) {
    if (sgrid > span) break;
    dl->AddLine(ImVec2(X(sgrid), p0.y + 2), ImVec2(X(sgrid), p1.y - 2), k.graphGrid);
  }
  dl->AddLine(ImVec2(lx, base), ImVec2(rx, base), k.graphGridStrong);
  // tails: low band (bass multiplier) and high band (damping shortens it)
  double hiRt = rt60 * (dampHz < 22000 ? std::clamp(std::log2(dampHz / 500.0) / std::log2(22000.0 / 500.0), 0.15, 1.0) : 1.0);
  const int N = 72;
  ImVec2 lo[N + 2], hi[N + 2];
  for (int i = 0; i <= N; ++i) {
    double tt = pre + (span - pre) * i / N;
    double aL = std::pow(10.0, -3.0 * (tt - pre) / std::max(0.05, rt60 * bass)), aH = std::pow(10.0, -3.0 * (tt - pre) / std::max(0.05, hiRt));
    float amp = static_cast<float>(std::clamp(0.35 + 0.65 * wet, 0.0, 1.0));
    lo[i] = ImVec2(X(tt), base - static_cast<float>(std::sqrt(aL)) * amp * (base - top));
    hi[i] = ImVec2(X(tt), base - static_cast<float>(std::sqrt(aH)) * amp * (base - top) * 0.85f);
  }
  for (int i = 0; i < N; ++i) {
    ImVec2 q[4] = {lo[i], lo[i + 1], ImVec2(lo[i + 1].x, base), ImVec2(lo[i].x, base)};
    dl->AddConvexPolyFilled(q, 4, theme::withAlpha(k.fill, 0.28f));
  }
  dl->AddPolyline(lo, N + 1, k.fill, 0, 1.5f);
  dl->AddPolyline(hi, N + 1, skin::lighten(k.fill, 0.45f), 0, 1.0f);
  // early reflections (spacing follows the room size)
  static const double erMs[6] = {7.1, 11.3, 17.9, 23.3, 31.7, 41.9};
  static const double erGain[6] = {0.82, 0.71, 0.58, 0.49, 0.38, 0.29};
  for (int e = 0; e < 6; ++e) {
    double tt = pre + erMs[e] * (0.4 + size) * 0.001;
    dl->AddLine(ImVec2(X(tt), base), ImVec2(X(tt), base - static_cast<float>(erGain[e] * std::min(1.0, early)) * (base - top)), k.text, 1.5f);
  }
  dl->AddLine(ImVec2(X(0), base), ImVec2(X(0), top), k.graphText, 2.0f);  // direct sound
  char buf[48];
  std::snprintf(buf, sizeof buf, "%.1f s", rt60);
  monoText(dl, ImVec2(p0.x + 5, p1.y - h * 0.13f), k.graphText, buf, std::clamp(h * 0.07f, 9.0f, 12.0f));
  if (ival(st, RvbFreeze)) monoText(dl, ImVec2(p1.x - h * 0.42f, p1.y - h * 0.13f), k.select, ival(st, RvbFreeze) == 1 ? "FREEZE" : "SUSTAIN", std::clamp(h * 0.07f, 9.0f, 12.0f));
}

}  // namespace

void editorReverbRef(App& a, const PluginTarget& t, float width) {
  const PluginState* sp = t.state(a.doc.song());
  if (!sp) return;
  const PluginState st = *sp;
  const skin::Skin& K = reverbSkin();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  constexpr float kW = 530, kH = 160;
  skin::Form F{ImGui::GetCursorScreenPos(), width / kW};
  const float s = F.s;
  skin::panel(dl, F.o, ImVec2(F.o.x + width, F.o.y + kH * s), K, 4);
  // Visual (0,0) 160×160; main panel (160,0) 370×160.
  roomVisual(dl, F.p(4, 4), F.p(156, 108), st, K);
  reverbVisual(dl, F.p(4, 112), F.p(156, 156), st, K, a.doc.song().tempo);
  {  // J-26: network model (hall / plate reference) in the room view's corner
    static const char* md[2] = {"HALL", "PLATE"};
    skin::switchSel(a, t, RvbModel, F.p(104, 6), ImVec2(50 * s, 14 * s), K, md, 2);
  }
  dl->AddRectFilled(F.p(160, 0), F.p(530, 160), skin::hex(0x434B4D), 0);
  dl->AddLine(F.p(160, 0), F.p(160, 160), skin::darken(K.panel, 0.5f), 1.0f);
  const float fsL = std::clamp(7.5f * s, 10.0f, 13.0f);
  // Mid / side input selector (169,18) 45×30.
  {
    skin::label(dl, F.p(169 + 22.5f, 0).x, F.p(0, 6).y, "INPUT", K.textDim, fsL);
    static const char* io[2] = {"MID", "M+S"};
    skin::switchSel(a, t, RvbInputMode, F.p(169, 18), ImVec2(45 * s, 30 * s), K, io, 2);
  }
  wheelL(a, t, RvbHighCut, F, 172, 57, 36, K, "H.CUT");
  wheelL(a, t, RvbLowCut, F, 172, 108, 36, K, "L.CUT");
  wheelL(a, t, RvbPreDelay, F, 231, 5, 36, K, "PRE");
  // Tempo-based pre-delay check (287,27) + the pre-delay read-out in steps:ticks when on.
  skin::check(a, t, RvbPreSync, F.p(281, 24), ImVec2(30 * s, 14 * s), K, "TEMPO");
  if (bval(st, RvbPreSync)) {
    int ticks = static_cast<int>(std::lround(val(st, RvbPreDelay) * 0.384));
    char buf[16];
    std::snprintf(buf, sizeof buf, "%d:%02d", ticks / 48, ticks % 48);
    skin::lcd(dl, F.p(274, 7), F.p(316, 20), K, buf, fsL);
  }
  wheelL(a, t, RvbSize, F, 231, 57, 36, K, "SIZE");
  wheelL(a, t, RvbDiffusion, F, 231, 108, 36, K, "DIFF");
  wheelL(a, t, RvbModDepth, F, 275, 57, 36, K, "DEPTH");
  wheelL(a, t, RvbModRate, F, 275, 108, 36, K, "SPEED");
  wheelL(a, t, RvbBassMult, F, 334, 5, 36, K, "BASS");
  wheelL(a, t, RvbCrossover, F, 334, 57, 36, K, "CROSS");
  wheelL(a, t, RvbDecayTime, F, 378, 5, 36, K, "DEC");
  wheelL(a, t, RvbDampHz, F, 377, 57, 36, K, "DAMP");
  // Gridline additions in the free cell under the decay group.
  wheelL(a, t, RvbColor, F, 334, 108, 36, K, "COLOR", skin::WheelCentered);
  {
    skin::label(dl, F.p(378 + 18, 0).x, F.p(0, 103).y, "FREEZE", K.textDim, fsL);
    static const char* fz[3] = {"OFF", "FRZ", "SUS"};
    skin::switchSel(a, t, RvbFreeze, F.p(378, 113), ImVec2(36 * s, 36 * s), K, fz, 3);
  }
  // Level sliders (434/466/497, 1) 24×90 and separation wheel (460,108).
  const int lv[3] = {RvbDry, RvbEarly, RvbWet};
  const float lx[3] = {434, 466, 497};
  const char* ln[3] = {"DRY", "ER", "WET"};
  for (int i = 0; i < 3; ++i) {
    skin::vslider(a, t, lv[i], F.p(lx[i], 1), ImVec2(24 * s, 90 * s), K);
    skin::label(dl, F.p(lx[i] + 12, 0).x, F.p(0, 92).y, ln[i], K.textDim, fsL);
  }
  wheelL(a, t, RvbWidth, F, 460, 108, 36, K, "SEP", skin::WheelCentered);
  ImGui::SetCursorScreenPos(F.o);
  ImGui::Dummy(ImVec2(width, kH * s));
}

}  // namespace gl
