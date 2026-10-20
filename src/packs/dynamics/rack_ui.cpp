// Gridline — "dynamics" pack: editors for Amp Rack (gl.amprack) and
// Multi FX (gl.effector), J-19 skin. Multi FX sits on its reference panel's
// 430 × 418 grid (in/out wheels and meters on top, X/Y wheels left of the
// modulation pad, rate column and modulation wheels right of it, effect
// selector along the bottom). Amp Rack has no reference form: it is drawn as
// a pedal board in the reference family's style (dark panel, amber values).
#include "../flc_kit.h"
#include "dyn_widgets.h"
#include "effector_ref.h"

namespace gl::packs::dynamics {

using namespace ui;

namespace {

// New state with slot `s` switched to module `type` (knobs at that module's defaults).
PluginState withModule(const PluginState& st, int s, int type) {
  PluginState n = st;
  normalizePluginState(n);
  auto put = [&](int k, double v) { n.params[static_cast<size_t>(rkParam(s, k))] = static_cast<float>(v); };
  put(RsType, type);
  put(RsOn, type != RmEmpty ? 1 : 0);
  for (int k = 0; k < kRackKnobs; ++k) put(RsK0 + k, rackKnobDefault(type, k));
  put(RsMix, type == RmReverb ? 0.35 : (type == RmDelay ? 0.5 : 1.0));
  return n;
}
PluginState swapped(const PluginState& st, int s1, int s2) {
  PluginState n = st;
  normalizePluginState(n);
  for (int k = 0; k < RsStride; ++k) std::swap(n.params[static_cast<size_t>(rkParam(s1, k))], n.params[static_cast<size_t>(rkParam(s2, k))]);
  return n;
}

const skin::Skin& rackSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x2B2E30, 0x3A3F42, 0x23272A, 0xF5A623, 0x4B5155, 0xC8CDD0);
    s.select = skin::hex(0xF5A623);
    s.lcdText = skin::hex(0xFFC766);
    s.pillBg = skin::hex(0x23272A);
    return s;
  }();
  return k;
}

const skin::Skin& effectorSkin() {
  static const skin::Skin k = [] {
    skin::Skin s = skin::Skin::fl(0x3B3836, 0x332F2D, 0x8B8D8F, 0xF99801, 0x4B5155, 0xC9C4BE);
    s.select = skin::hex(0xFFA830);
    s.lcdBg = skin::hex(0x21272B);
    s.lcdText = skin::hex(0xFFA830);
    s.graphBg = skin::hex(0x31373B);
    s.graphGrid = skin::hex(0x3B4246);
    s.graphGridStrong = skin::hex(0x4A5357);
    s.graphCurve = skin::hex(0xDC9D02);
    s.fill = skin::hex(0xF99801);
    s.accent2 = skin::hex(0xDC9D02);
    return s;
  }();
  return k;
}

// One pedal: well, slot number, module box (menu), power LED, six wheels, mix, move arrows.
void pedal(App& a, const PluginTarget& t, const PluginState& st, const flk::Ctx& c, int s, float x, float y, float w, float h) {
  const skin::Skin& K = c.K;
  const skin::Form& F = c.F;
  ImDrawList* dl = c.dl;
  int type = std::clamp(ival(st, rkParam(s, RsType)), 0, RmCount - 1);
  bool on = val(st, rkParam(s, RsOn)) >= 0.5f && type != RmEmpty;
  ImGui::PushID(s);
  skin::well(dl, F.p(x, y), F.p(x + w, y + h), K, 5 * F.s);
  if (!on) dl->AddRectFilled(F.p(x, y), F.p(x + w, y + h), theme::withAlpha(skin::darken(K.panel, 0.5f), 0.35f), 5 * F.s);
  char num[8];
  std::snprintf(num, sizeof num, "%d", s + 1);
  flk::text(c, x + 6, y + 7, num);
  // Module box (opens the module list).
  {
    ImVec2 p0 = F.p(x + 18, y + 4), p1 = F.p(x + w - 26, y + 20);
    ImGui::SetCursorScreenPos(p0);
    if (ImGui::InvisibleButton("##type", ImVec2(p1.x - p0.x, p1.y - p0.y))) ImGui::OpenPopup("##modules");
    bool hov = ImGui::IsItemHovered();
    if (hov) a.setHint("Module of slot " + std::to_string(s + 1) + ": " + rackModuleName(type) + "   (click to change)");
    dl->AddRectFilled(p0, p1, hov ? skin::lighten(K.lcdBg, 0.06f) : K.lcdBg, 2);
    std::string nm = flk::upper(rackModuleName(type));
    skin::label(dl, (p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f - F.font(8) * 0.6f, nm.c_str(), on ? K.lcdText : K.textDim, F.font(8));
    if (ImGui::BeginPopup("##modules")) {
      for (int k = 0; k < RmCount; ++k)
        if (ImGui::MenuItem(rackModuleName(k), nullptr, k == type) && k != type) setPluginState(a, t, withModule(st, s, k), "Change rack module");
      ImGui::EndPopup();
    }
  }
  // Power LED.
  {
    ImVec2 ctr = F.p(x + w - 12, y + 12);
    ImGui::SetCursorScreenPos(ImVec2(ctr.x - 9 * F.s, ctr.y - 9 * F.s));
    if (ImGui::InvisibleButton("##power", F.sz(18, 18)) && type != RmEmpty) setParam(a, t, rkParam(s, RsOn), on ? 0 : 1);
    if (ImGui::IsItemHovered()) a.setHint(on ? "Module on (click to bypass)" : "Module bypassed (click to enable)");
    skin::ledDot(dl, ctr, 4.5f * F.s, on, K);
  }
  if (type == RmEmpty) {
    for (float yy = y + 30; yy < y + h - 70; yy += 8) dl->AddLine(F.p(x + 8, yy), F.p(x + w - 8, yy), theme::withAlpha(K.textDim, 0.12f));
    flk::textC(c, x + w * 0.5f, y + h * 0.42f, "EMPTY SLOT");
  } else {
    const float cw = (w - 8) / 2, ws = 34;
    for (int k = 0; k < kRackKnobs; ++k) {
      const char* lbl = rackKnobLabel(type, k);
      if (!lbl) continue;
      float cx = x + 4 + (k % 2) * cw + cw * 0.5f, cy = y + 28 + (k / 2) * 66;
      skin::wheel(a, t, rkParam(s, RsK0 + k), F.p(cx - ws * 0.5f, cy), ws * F.s, K, skin::WheelBigLine);
      char txt[32];
      rackKnobText(type, k, val(st, rkParam(s, RsK0 + k)), txt, sizeof txt);
      std::string L = flk::upper(lbl);
      flk::textC(c, cx, cy + ws + 1, L.c_str());
      skin::valueStrip(dl, F.p(cx - cw * 0.5f + 4, cy + ws + 12), F.p(cx + cw * 0.5f - 4, cy + ws + 23), K, txt, F.font(7.5f));
    }
  }
  // Footer: mix + move arrows.
  float fy = y + h - 58;
  dl->AddLine(F.p(x + 6, fy), F.p(x + w - 6, fy), skin::darken(K.wellC(), 0.3f));
  if (type != RmEmpty) flk::wheel(c, rkParam(s, RsMix), x + 10, fy + 8, 30, "Mix");
  if (flk::btn(c, "##left", x + w - 58, fy + 16, 22, 20, "<", false) && s > 0) setPluginState(a, t, swapped(st, s, s - 1), "Move rack module");
  if (ImGui::IsItemHovered()) a.setHint("Move module left");
  if (flk::btn(c, "##right", x + w - 32, fy + 16, 22, 20, ">", false) && s + 1 < kRackSlots)
    setPluginState(a, t, swapped(st, s, s + 1), "Move rack module");
  if (ImGui::IsItemHovered()) a.setHint("Move module right");
  ImGui::PopID();
}

}  // namespace

// ===================================================================== gl.amprack
void editorAmpRack(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  const skin::Skin& K = rackSkin();
  constexpr float kW = 1000, kH = 372;
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  flk::Ctx c{a, t, F, K, dl};
  float in = -120, out = -120;
  if (auto rt = runtime(a, t)) {
    in = rt->input.load();
    out = rt->meter.load();
  }
  skin::meter(dl, F.p(6, 6), F.p(20, 306), K, in, 48);
  skin::meter(dl, F.p(kW - 20, 6), F.p(kW - 6, 306), K, out, 48);
  const float x0 = 26, gap = 6, pw = (kW - 2 * x0 - (kRackSlots - 1) * gap) / kRackSlots;
  for (int s = 0; s < kRackSlots; ++s) pedal(a, t, st, c, s, x0 + s * (pw + gap), 6, pw, 300);
  // Signal-flow arrows between pedals.
  for (int s = 0; s + 1 < kRackSlots; ++s) {
    ImVec2 m = F.p(x0 + (s + 1) * (pw + gap) - gap * 0.5f, 150);
    dl->AddTriangleFilled(ImVec2(m.x - 2.5f * F.s, m.y - 4 * F.s), ImVec2(m.x - 2.5f * F.s, m.y + 4 * F.s), ImVec2(m.x + 3 * F.s, m.y), K.textDim);
  }
  // Rack strip.
  skin::well(dl, F.p(6, 312), F.p(kW - 6, kH - 6), K, 4 * F.s);
  flk::wheel(c, RkInput, 20, 316, 34, "Input");
  flk::wheel(c, RkOutput, 74, 316, 34, "Output");
  flk::tog(c, RkSoftClip, 130, 326, 110, 22, "SOFT CLIP OUT");
  flk::text(c, 262, 330, "OVERSAMPLING");
  static const char* os[3] = {"OFF", "2X", "4X"};
  flk::sw(c, RkOversample, 350, 326, 150, 22, os, 3, true);
  flk::text(c, 520, 330, "SIGNAL FLOWS LEFT TO RIGHT · ARROWS REORDER MODULES", K.textDim, 7.5f);
  F.end(kW, kH);
}

namespace {
void shapeIcon(ImDrawList* dl, int i, ImVec2 c, float w, float h, ImU32 col) {
  ImVec2 pts[33];
  const double ra[5] = {0.6, -0.4, 0.9, -0.7, 0.2};
  for (int k = 0; k <= 32; ++k) {
    double ph = k / 32.0;
    int seg = std::min(4, static_cast<int>(ph * 5));
    double y = i < 4 ? effectorShape(i, ph, 0, 0) : effectorShape(i, ph * 5 - seg, ra[seg], ra[(seg + 1) % 5]);
    pts[k] = ImVec2(c.x - w * 0.5f + w * static_cast<float>(ph), c.y - static_cast<float>(y) * h * 0.5f);
  }
  dl->AddPolyline(pts, 33, col, 0, 1.4f);
}
}  // namespace

// ===================================================================== gl.effector
namespace {
// J-26 reference engine: the same panel driven by the twelve stored 0..1 values.
const char* kEfRefNames[12] = {"Distort", "Crusher", "Flanger", "Phaser", "Filter", "Echo",
                               "Reverb",  "Panner",  "Trance", "Stutter", "Vowel",  "Ring"};
const char* kEfRefAxes[12][2] = {{"Drive", "Band"},  {"Hold", "Bits"},  {"Depth", "Feedback"}, {"Frequency", "Feedback"},
                                 {"Cutoff", "Resonance"}, {"Feedback", "Tone"}, {"Size", "Damping"}, {"Pan", "Width"},
                                 {"Curve", "Floor"},  {"-", "-"},       {"Vowel", "Formant"},  {"Frequency", "-"}};
void editorEffectorRef(App& a, const PluginTarget& t, float width, const skin::Skin& K) {
  const PluginState st = *t.state(a.doc.song());
  constexpr float kW = 430, kH = 418;
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  flk::Ctx c{a, t, F, K, dl};
  auto rt = runtime(a, t);
  float in = rt ? rt->input.load() : -120.f;
  float outPk = tapPeak(t.tap(a), 800);
  float liveX = val(st, EfRawX), liveY = val(st, EfRawY);
  if (rt) {
    liveX = rt->bands[0].load();
    liveY = rt->bands[1].load();
  }
  const int kind = efref::kindOf(val(st, EfRawKind));
  skin::Skin io = K;
  io.rail = skin::hex(0xC0C1C2);
  io.fill = skin::hex(0x929598);
  io.cap = skin::hex(0x414146);
  skin::wheel(a, t, EfRawIn, F.p(19, 9), 34 * F.s, io, skin::WheelBigLine);
  skin::wheel(a, t, EfRawOut, F.p(378, 9), 34 * F.s, io, skin::WheelBigLine | skin::WheelCentered);
  flk::textC(c, 36, 45, "IN");
  flk::textC(c, 395, 45, "OUT");
  levelBar(dl, F.p(65, 20), F.p(162, 38), in, -48, true);
  levelBar(dl, F.p(270, 20), F.p(367, 38), toDb(outPk), -48, true);
  {
    std::string nm = flk::upper(kEfRefNames[kind]);
    skin::label(dl, F.p(216, 0).x, F.p(0, 12).y, nm.c_str(), K.lcdText, F.font(10));
    static const char* eng[2] = {"LEGACY", "REF"};
    flk::sw(c, EfModel, 184, 32, 64, 14, eng, 2, true);
  }
  flk::wheel(c, EfRawX, 16, 68, 40, kEfRefAxes[kind][0]);
  flk::wheel(c, EfRawY, 16, 136, 40, kEfRefAxes[kind][1]);
  {
    bool by = val(st, EfRawBypass) != 0;
    ImGui::PushID("bypass");
    if (skin::button("##by", F.p(24, 204), F.sz(24, 24), K, "", by)) setParam(a, t, EfRawBypass, by ? 0 : 1);
    if (ImGui::IsItemHovered()) a.setHint("Bypass the effect");
    ImGui::PopID();
    skin::ledDot(dl, F.p(36, 216), 5 * F.s, by, K, skin::hex(0xFF6C00));
    flk::textC(c, 36, 231, "BYPASS");
  }
  flk::wheel(c, EfRawMix, 16, 266, 40, "Wet");
  {
    ImVec2 p0 = F.p(76, 65), p1 = F.p(322, 311);
    const float pad = p1.x - p0.x;
    skin::xyPad(a, t, EfRawX, EfRawY, p0, ImVec2(pad, p1.y - p0.y), K);
    if (ImGui::IsItemHovered()) a.setHint("X / Y: drag to set both parameters; the modulation path is drawn around the point");
    auto at = [&](double x, double y) { return ImVec2(p0.x + static_cast<float>(x) * pad, p1.y - static_cast<float>(y) * pad); };
    double X = val(st, EfRawX), Y = val(st, EfRawY), ax = efref::modAmount(val(st, EfRawModX)), ay = efref::modAmount(val(st, EfRawModY));
    int shape = efref::shapeOf(val(st, EfRawShape));
    if (ax != 0 || ay != 0) {  // one LFO drives both axes: the path is a line segment
      ImVec2 path[97];
      for (int i = 0; i <= 96; ++i) {
        double ph = i / 96.0;
        double m = shape == 0 ? std::sin(2 * 3.14159265358979 * ph) : shape == 1 ? 1 - 2 * ph : (ph <= 0.5 ? 1.0 : -1.0);
        path[i] = at(std::clamp(X + m * ax, 0.0, 1.0), std::clamp(Y + m * ay, 0.0, 1.0));
      }
      dl->AddPolyline(path, 97, theme::withAlpha(K.graphCurve, 0.7f), 0, 1.5f);
    }
    dl->AddCircleFilled(at(liveX, liveY), 4 * F.s, K.text);
    skin::text(dl, ImVec2(p0.x + 6, p1.y - F.font(8) - 5), K.graphText, kEfRefAxes[kind][0], F.font(8));
    skin::text(dl, ImVec2(p0.x + 6, p0.y + 5), K.graphText, kEfRefAxes[kind][1], F.font(8));
  }
  {  // rate column: 9 lengths in beats, longest at the top
    static const char* rates[9] = {"8", "4", "2", "1", "3/4", "1/2", "3/8", "1/4", "1/8"};
    const int cur = efref::rateOf(val(st, EfRawRate));
    for (int k = 0; k < 9; ++k) {
      int choice = 8 - k;
      ImGui::PushID(600 + k);
      if (skin::button("##rate", F.p(335, 65 + k * 27.3f), F.sz(24, 26), K, rates[k], cur == choice, F.font(7)))
        setParam(a, t, EfRawRate, choice / 8.0);
      if (ImGui::IsItemHovered()) a.setHint(std::string("Modulation cycle: ") + rates[k] + " beat(s)");
      ImGui::PopID();
    }
  }
  flk::wheel(c, EfRawModX, 375, 64, 36, "Mod X", skin::WheelBigLine | skin::WheelCentered);
  flk::wheel(c, EfRawModY, 375, 122, 36, "Mod Y", skin::WheelBigLine | skin::WheelCentered);
  {
    static const char* shp[3] = {"SIN", "SAW", "SQR"};
    const int cur = efref::shapeOf(val(st, EfRawShape));
    for (int k = 0; k < 3; ++k) {
      ImGui::PushID(650 + k);
      if (skin::button("##shape", F.p(375, 180 + k * 22.0f), F.sz(40, 20), K, shp[k], cur == k, F.font(7))) setParam(a, t, EfRawShape, k / 2.0);
      ImGui::PopID();
    }
  }
  flk::wheel(c, EfRawTempoRed, 375, 252, 36, "Reduce");
  skin::well(dl, F.p(10, 328), F.p(420, 408), K, 4 * F.s);
  for (int k = 0; k < 12; ++k) {
    float bx = 14 + (k % 6) * 67.3f, by = 332 + (k / 6) * 37.5f;
    ImGui::PushID(700 + k);
    std::string nm = flk::upper(kEfRefNames[k]);
    if (skin::button("##kind", F.p(bx, by), F.sz(64, 34), K, nm.c_str(), kind == k, F.font(7.5f))) setParam(a, t, EfRawKind, k / 11.0);
    ImGui::PopID();
  }
  F.end(kW, kH);
}
}  // namespace

void editorEffector(App& a, const PluginTarget& t, float width) {
  const PluginState st = *t.state(a.doc.song());
  if (val(st, EfModel) >= 0.5f) {
    editorEffectorRef(a, t, width, effectorSkin());
    return;
  }
  const skin::Skin& K = effectorSkin();
  constexpr float kW = 430, kH = 418;
  skin::Form F = skin::Form::fit(width, kW);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::panel(dl, F.o, F.p(kW, kH), K, 4);
  skin::Scope scope(K);
  flk::Ctx c{a, t, F, K, dl};
  auto rt = runtime(a, t);
  float in = rt ? rt->input.load() : -120.f;
  float outPk = tapPeak(t.tap(a), 800);
  float liveX = val(st, EfX), liveY = val(st, EfY);
  if (rt) {
    liveX = rt->bands[0].load();
    liveY = rt->bands[1].load();
  }
  const int kind = std::clamp(ival(st, EfKind), 0, kEfKinds - 1);
  // ---- top: in / out wheels and meters, effect name
  skin::Skin io = K;
  io.rail = skin::hex(0xC0C1C2);
  io.fill = skin::hex(0x929598);
  io.cap = skin::hex(0x414146);
  skin::wheel(a, t, EfInput, F.p(19, 9), 34 * F.s, io, skin::WheelBigLine);
  skin::wheel(a, t, EfOutput, F.p(378, 9), 34 * F.s, io, skin::WheelBigLine);
  flk::textC(c, 36, 45, "IN");
  flk::textC(c, 395, 45, "OUT");
  levelBar(dl, F.p(65, 20), F.p(162, 38), in, -48, true);
  levelBar(dl, F.p(270, 20), F.p(367, 38), toDb(outPk), -48, true);
  {
    std::string nm = flk::upper(effectorKindName(kind));
    skin::label(dl, F.p(216, 0).x, F.p(0, 12).y, nm.c_str(), K.lcdText, F.font(10));
    static const char* eng[2] = {"LEGACY", "REF"};
    flk::sw(c, EfModel, 184, 32, 64, 14, eng, 2, true);
  }
  // ---- left: X / Y / bypass / wet
  flk::wheel(c, EfX, 16, 68, 40, effectorAxisLabel(kind, 0));
  flk::wheel(c, EfY, 16, 136, 40, effectorAxisLabel(kind, 1));
  {
    bool by = val(st, EfBypass) >= 0.5f;
    ImGui::PushID("bypass");
    if (skin::button("##by", F.p(24, 204), F.sz(24, 24), K, "", by)) setParam(a, t, EfBypass, by ? 0 : 1);
    if (ImGui::IsItemHovered()) a.setHint("Bypass the effect");
    ImGui::PopID();
    skin::ledDot(dl, F.p(36, 216), 5 * F.s, by, K, skin::hex(0xFF6C00));
    flk::textC(c, 36, 231, "BYPASS");
  }
  flk::wheel(c, EfMix, 16, 266, 40, "Wet");
  // ---- X / Y pad with the modulation path and the live position
  {
    ImVec2 p0 = F.p(76, 65), p1 = F.p(322, 311);
    const float pad = p1.x - p0.x;
    skin::xyPad(a, t, EfX, EfY, p0, ImVec2(pad, p1.y - p0.y), K);
    if (ImGui::IsItemHovered()) a.setHint("X / Y: drag to set both parameters; the modulation path is drawn around the point");
    auto at = [&](double x, double y) { return ImVec2(p0.x + static_cast<float>(x) * pad, p1.y - static_cast<float>(y) * pad); };
    double X = val(st, EfX), Y = val(st, EfY), mx = val(st, EfModX), my = val(st, EfModY);
    int shape = ival(st, EfModShape);
    if (mx != 0 || my != 0) {
      if (shape >= 4) {
        ImVec2 a0 = at(std::clamp(X - std::fabs(mx) * 0.5, 0.0, 1.0), std::clamp(Y - std::fabs(my) * 0.5, 0.0, 1.0));
        ImVec2 a1 = at(std::clamp(X + std::fabs(mx) * 0.5, 0.0, 1.0), std::clamp(Y + std::fabs(my) * 0.5, 0.0, 1.0));
        dl->AddRectFilled(ImVec2(a0.x, a1.y), ImVec2(a1.x, a0.y), theme::withAlpha(K.graphCurve, 0.14f));
      } else {
        ImVec2 path[97];
        for (int i = 0; i <= 96; ++i) {
          double ph = i / 96.0, q = ph + 0.25;
          q -= std::floor(q);
          path[i] = at(std::clamp(X + mx * 0.5 * effectorShape(shape, ph, 0, 0), 0.0, 1.0), std::clamp(Y + my * 0.5 * effectorShape(shape, q, 0, 0), 0.0, 1.0));
        }
        dl->AddPolyline(path, 97, theme::withAlpha(K.graphCurve, 0.7f), 0, 1.5f);
      }
    }
    dl->AddCircleFilled(at(liveX, liveY), 4 * F.s, K.text);
    skin::text(dl, ImVec2(p0.x + 6, p1.y - F.font(8) - 5), K.graphText, effectorAxisLabel(kind, 0), F.font(8));
    skin::text(dl, ImVec2(p0.x + 6, p0.y + 5), K.graphText, effectorAxisLabel(kind, 1), F.font(8));
  }
  // ---- rate column (8 cells, slowest at the bottom → reversed labels top to bottom)
  {
    static const char* rates[8] = {"8", "4", "2", "1", "1/2", "1/4", "1/8", "1/16"};
    const int cur = ival(st, EfModRate);
    for (int k = 0; k < 8; ++k) {
      int choice = 7 - k;
      ImGui::PushID(600 + k);
      if (skin::button("##rate", F.p(335, 65 + k * 30.75f), F.sz(24, 29), K, rates[k], cur == choice, F.font(7)))
        setParam(a, t, EfModRate, choice);
      if (ImGui::IsItemHovered()) a.setHint(std::string("Modulation rate: ") + (choice >= 4 ? rates[k] + std::string(" bar(s)") : rates[k]));
      ImGui::PopID();
    }
  }
  // ---- modulation wheels + shape
  flk::wheel(c, EfModX, 375, 68, 40, "Mod X", skin::WheelBigLine | skin::WheelCentered);
  flk::wheel(c, EfModY, 375, 136, 40, "Mod Y", skin::WheelBigLine | skin::WheelCentered);
  skin::iconSel(a, t, EfModShape, F.p(375, 196), F.sz(40, 96), K, 6, shapeIcon, false);
  // ---- effect selector (6 × 2)
  skin::well(dl, F.p(10, 328), F.p(420, 408), K, 4 * F.s);
  for (int k = 0; k < kEfKinds; ++k) {
    float bx = 14 + (k % 6) * 67.3f, by = 332 + (k / 6) * 37.5f;
    ImGui::PushID(700 + k);
    std::string nm = flk::upper(effectorKindName(k));
    if (skin::button("##kind", F.p(bx, by), F.sz(64, 34), K, nm.c_str(), kind == k, F.font(7.5f))) setParam(a, t, EfKind, k);
    ImGui::PopID();
  }
  F.end(kW, kH);
}

}  // namespace gl::packs::dynamics
