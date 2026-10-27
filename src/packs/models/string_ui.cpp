// Gridline — String Model editor (J-19) on the reference physical string
// synth's 855 × 452 grid (the reference's mod-matrix row is dropped) and
// palette: exciter LCD, voices, volume and level on top; the exciter /
// filter wheel column; the strings block (decay, damping, stiffness, pickup,
// saturation, the live string diagram, second string); body / amp /
// envelope column; body resonance view and strummer; effects row.
#include "../flc_kit.h"
#include "../../engine/Dsp.h"
#include "models_widgets.h"

namespace gl::packs::models {

using namespace ui2;

namespace {

// String diagram: the string with its strike / pickup points (draggable) and
// the decay time across frequency below it.
void stringView(App& a, const PluginTarget& t, const PluginState& st, ImVec2 size) {
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p = graphFrame(size, "##string", "STRING", a);
  bool act = ImGui::IsItemActive(), hov = ImGui::IsItemHovered();
  static int dragging = 0;  // 1 = strike, 2 = pickup
  const float x0 = p.x + 20, x1 = p.x + size.x - 20, sy = p.y + size.y * 0.30f;
  float pos = val(st, StExcPos, 0.15f);
  float pickFrac = 0.04f + 0.46f * val(st, StPickup, 0.3f);
  float sx = x0 + (x1 - x0) * pos, px = x0 + (x1 - x0) * pickFrac;
  // Bridges and the string with its first modes.
  dl->AddRectFilled(ImVec2(x0 - 4, sy - 16), ImVec2(x0, sy + 16), P.textDim, 1);
  dl->AddRectFilled(ImVec2(x1, sy - 16), ImVec2(x1 + 4, sy + 16), P.textDim, 1);
  for (int m = 1; m <= 3; ++m) {
    ImVec2 prev(x0, sy);
    for (int i = 1; i <= 64; ++i) {
      float u = i / 64.0f;
      float y = sy - std::sin(3.14159265f * m * u) * 14.0f / m * std::sin(3.14159265f * m * pos) * 1.2f;
      ImVec2 q(x0 + (x1 - x0) * u, y);
      dl->AddLine(prev, q, theme::withAlpha(P.textFaint, 0.7f / m), 1.0f);
      prev = q;
    }
  }
  dl->AddLine(ImVec2(x0, sy), ImVec2(x1, sy), P.text, 1.5f);
  dl->AddTriangleFilled(ImVec2(sx, sy - 4), ImVec2(sx - 6, sy - 16), ImVec2(sx + 6, sy - 16), P.accent);
  dl->AddRectFilled(ImVec2(px - 7, sy + 5), ImVec2(px + 7, sy + 13), P.paper, 2);
  ImGui::PushFont(a.fontMono, 9.5f);
  dl->AddText(ImVec2(sx + 8, sy - 22), P.accent, "strike");
  dl->AddText(ImVec2(px + 9, sy + 4), P.textDim, "pickup");
  ImGui::PopFont();
  // Decay (T60) across frequency for the current settings at C3.
  const float gy0 = p.y + size.y * 0.52f, gy1 = p.y + size.y - 18;
  dl->AddText(ImVec2(p.x + 8, gy0 - 4), P.textFaint, "");
  ImGui::PushFont(a.fontMono, 9.5f);
  dl->AddText(ImVec2(p.x + 8, gy0 - 14), P.textFaint, "DECAY BY FREQUENCY");
  ImGui::PopFont();
  double decay = val(st, StDecay, 4), damp = val(st, StDamp, 0.35f);
  double f0 = 130.81, sr = 44100;
  double dC = std::clamp(damp * 0.92 * (0.25 + 0.75 * std::min(1.0, 300.0 / f0)), 0.0, 0.95);
  double hlp0 = (1 - dC) / std::sqrt(1 - 2 * dC * std::cos(dsp::kTwoPi * f0 / sr) + dC * dC);
  double gLoop = std::min(0.99995, std::pow(10.0, -3.0 / (decay * f0)) / hlp0);
  ImVec2 prev;
  for (int i = 0; i <= 120; ++i) {
    double hz = 50 * std::pow(20000.0 / 50, i / 120.0);
    double w = dsp::kTwoPi * hz / sr;
    double mag = (1 - dC) / std::sqrt(1 - 2 * dC * std::cos(w) + dC * dC) * gLoop;  // loop gain at hz
    double t60 = mag < 1 ? -3.0 / (std::log10(mag) * f0) : 30.0;
    float y = gy1 - static_cast<float>(std::clamp(std::log(std::max(0.01, t60) / 0.01) / std::log(30.0 / 0.01), 0.0, 1.0)) * (gy1 - gy0);
    ImVec2 q(p.x + 8 + (size.x - 16) * i / 120.0f, y);
    if (i) dl->AddLine(prev, q, P.accent, 1.8f);
    prev = q;
  }
  freqLabels(a, dl, ImVec2(p.x + 8, gy0), ImVec2(size.x - 16, gy1 - gy0 + 16), 50, 20000);
  // Drag the strike (top half, near the arrow) or pickup marker.
  ImVec2 m = ImGui::GetIO().MousePos;
  if (ImGui::IsItemActivated()) dragging = std::fabs(m.x - sx) <= std::fabs(m.x - px) ? 1 : 2;
  if (act && dragging) {
    float u = std::clamp((m.x - x0) / (x1 - x0), 0.0f, 1.0f);
    if (dragging == 1)
      a.doc.setParam(t.pid(StExcPos), std::clamp(u, 0.02f, 0.5f));
    else
      a.doc.setParam(t.pid(StPickup), std::clamp((u - 0.04f) / 0.46f, 0.0f, 1.0f));
  }
  if (ImGui::IsItemDeactivated()) {
    a.doc.endGesture();
    dragging = 0;
  }
  if (hov) a.setHint("Drag the strike point (arrow) or the pickup (bar) along the string · lower curve: how long each frequency rings");
}

void bodyView(App& a, const PluginTarget& t, const PluginState& st, ImVec2 size) {
  (void)t;
  const auto& P = pal();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p = graphFrame(size, "##body", nullptr, a);
  int body = ival(st, StBody, 1);
  float bsize = val(st, StBodySize, 1);
  const BodyMode* bm = bodyModes(body);
  dsp::Biquad f[kBodyModes];
  for (int k = 0; k < kBodyModes; ++k) f[k].set(dsp::Biquad::BandPass, 44100, bm[k].hz / bsize, bm[k].q, 0);
  freqLabels(a, dl, p, size, 50, 10000);
  ImVec2 prev;
  for (int i = 0; i <= 100; ++i) {
    double hz = 50 * std::pow(200.0, i / 100.0), mag = 0;
    for (int k = 0; k < kBodyModes; ++k) mag += f[k].magnitude(hz, 44100) * bm[k].gain;
    float y = p.y + size.y - 16 - static_cast<float>(std::min(1.0, mag / 1.3)) * (size.y - 24);
    ImVec2 q(p.x + size.x * i / 100.0f, y);
    if (i && body) dl->AddLine(prev, q, P.paper, 1.5f);
    prev = q;
  }
  if (!body) {
    ImGui::PushFont(a.fontMono, 10.0f);
    dl->AddText(ImVec2(p.x + 8, p.y + 6), P.textFaint, "NO BODY");
    ImGui::PopFont();
  }
}

const skin::Skin& stringSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x3B3836, 0x45413E, 0x393A3E, 0xD2B3A7, 0x958269, 0xCFC5A9);
    s.handle = skin::hex(0x212121);
    s.select = skin::hex(0xD2B3A7);
    s.accent2 = skin::hex(0x7FB2D0);
    s.lcdBg = skin::hex(0x262426);
    s.lcdText = skin::hex(0xD3D2B5);
    s.pillBg = skin::hex(0x2D383E);
    s.pillText = skin::hex(0xB8BDC1);
    s.graphBg = skin::hex(0x262426);
    s.graphGrid = skin::hex(0x343133);
    s.graphGridStrong = skin::hex(0x46423F);
    s.graphCurve = skin::hex(0xE6CBBE);
    return s;
  }();
  return k;
}

}  // namespace

void drawStringModel(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  const skin::Skin& K = stringSkin();
  constexpr float kW = 855, kH = 452;
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  flk::Ctx c{a, t, F, K, dl};
  ImGui::PushID("gl.string");
  // ---- top: name, exciter LCD menu, voices, volume, level meter
  if (!pui::titleWordmark(dl, "gl.string", F.p(16, 22), F.font(13)))  // J-136: the wordmark
    skin::text(dl, F.p(16, 22), K.select, "STRING MODEL", F.font(13));
  flk::text(c, 210, 18, "EXCITER");
  flk::menu(c, StExciter, 274, 17, 207, 22);
  flk::text(c, 520, 12, "VOICES");
  flk::digi(c, StVoices, 520, 26, 46, 18);
  flk::wheel(c, StVolume, 695, 10, 36, "Volume");
  {
    float pk = 0;
    if (const ScopeTap* tap = t.tap(a)) {
      static float buf[512];
      if (tap->read(buf, 512))
        for (float v : buf) pk = std::max(pk, std::fabs(v));
    }
    float db = pk > 1e-6f ? 20.0f * std::log10(pk) : -120.0f;
    ImVec2 p0 = F.p(767, 17), p1 = F.p(836, 54);
    skin::graphWell(dl, p0, p1, K);
    float f = std::clamp((db + 48) / 48, 0.0f, 1.0f);
    dl->AddRectFilled(ImVec2(p0.x + 3, p1.y - 10 * F.s), ImVec2(p0.x + 3 + (p1.x - p0.x - 6) * f, p1.y - 4 * F.s), K.fill, 2);
    flk::text(c, 772, 22, "LEVEL", K.graphText, 7);
  }
  // ---- exciter + filter column
  flk::section(c, 4, 64, 196, 290, "EXCITE / FILTER");
  flk::wheel(c, StExcPos, 86 - 60, 86, 45, "Position");
  flk::wheel(c, StExcHard, 86, 86, 45, "Hardness");
  flk::wheel(c, StExcNoise, 146, 86, 45, "Noise");
  flk::wheel(c, StBowForce, 26, 156, 45, "Force");
  flk::wheel(c, StLoCut, 86, 156, 45, "Lo cut");
  flk::wheel(c, StHiCut, 146, 156, 45, "Hi cut");
  flk::wheel(c, StHiCutEnv, 56, 226, 45, "Env amt", skin::WheelBigLine | skin::WheelCentered);
  flk::wheel(c, StHiCutDec, 116, 226, 45, "Env dec");
  flk::text(c, 12, 300, "MOTION");
  flk::wheel(c, StVibrato, 26, 298 + 14, 26, "Vibrato");
  flk::wheel(c, StVibRate, 86, 312, 26, "Rate");
  flk::wheel(c, StSpread, 146, 312, 26, "Spread");
  // ---- strings
  flk::section(c, 206, 64, 342, 290, "STRINGS");
  flk::wheel(c, StDecay, 224, 84, 38, "Decay");
  flk::wheel(c, StDamp, 284, 84, 38, "Damping");
  flk::wheel(c, StStiffness, 349, 92, 28, "Stiff");
  flk::wheel(c, StPickup, 404, 92, 28, "Pickup");
  flk::wheel(c, StSaturation, 494, 84, 38, "Saturate");
  ImGui::SetCursorScreenPos(F.p(214, 140));
  stringView(a, t, st, F.sz(326, 138));
  flk::text(c, 216, 292, "2ND STRING");
  flk::menu(c, St2Ratio, 216, 306, 100, 18);
  flk::wheel(c, St2Level, 352, 290, 34, "Level");
  flk::wheel(c, StDetune, 420, 290, 34, "Detune", skin::WheelBigLine | skin::WheelCentered);
  // ---- body / amp / envelope
  flk::section(c, 554, 64, 140, 290, "BODY / AMP");
  flk::menu(c, StBody, 560, 82, 128, 18);
  flk::wheel(c, StBodyMix, 566, 110, 45, "Mix");
  flk::wheel(c, StBodySize, 626, 110, 45, "Size");
  flk::wheel(c, StDrive, 566, 180, 45, "Drive");
  flk::wheel(c, StTone, 626, 180, 45, "Tone");
  flk::tog(c, StCab, 566, 250, 116, 18, "CABINET");
  flk::text(c, 566, 282, "ENVELOPE");
  flk::wheel(c, StAttack, 562, 296, 30, "Att");
  flk::wheel(c, StSustain, 606, 296, 30, "Sus");
  flk::wheel(c, StRelease, 650, 296, 30, "Rel");
  // ---- body response + strummer
  flk::section(c, 698, 64, 153, 290, "RESONANCE");
  ImGui::SetCursorScreenPos(F.p(704, 82));
  bodyView(a, t, st, F.sz(141, 120));
  flk::text(c, 704, 214, "STRUMMER");
  flk::menu(c, StStrum, 704, 228, 141, 18);
  static const char* dirs[3] = {"DOWN", "UP", "ALT"};
  auto dd = describeParam(a.doc.song(), t.pid(StStrumDir));
  int nDir = dd && !dd->choices.empty() ? std::min(3, static_cast<int>(dd->choices.size())) : 2;
  flk::sw(c, StStrumDir, 704, 252, 141, 18, dirs, nDir, true);
  flk::wheel(c, StStrumTime, 758, 280, 34, "Spacing");
  // ---- effects row
  flk::section(c, 4, 360, 847, 88, "EFFECTS");
  skin::groupCaption(dl, F, 20, 250, 376, K, "DELAY");
  skin::groupCaption(dl, F, 270, 390, 376, K, "CHORUS");
  skin::groupCaption(dl, F, 410, 600, 376, K, "REVERB");
  flk::wheel(c, StFx + FxDelay, 30, 390, 40, "Wet");
  flk::wheel(c, StFx + FxDelayTime, 110, 390, 40, "Time");
  flk::wheel(c, StFx + FxDelayFeedback, 190, 390, 40, "Fdbk");
  flk::wheel(c, StFx + FxChorus, 310, 390, 40, "Wet");
  flk::wheel(c, StFx + FxReverb, 440, 390, 40, "Wet");
  flk::wheel(c, StFx + FxRoomSize, 530, 390, 40, "Room");
  F.end(kW, kH);
  ImGui::PopID();
}

}  // namespace gl::packs::models
