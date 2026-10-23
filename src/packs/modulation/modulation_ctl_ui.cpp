// Gridline — "modulation" pack: controller editors on the reference
// controller panels' grids (form units scaled to the window width) with the
// reference palette roles: peak follower, X-Y and X-Y-Z pads, formula,
// envelope, keyboard and voltage controllers.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include "../../app/DrawSmoothUi.h"
#include "../../app/XyPadMath.h"
#include "../../core/I18n.h"
#include "../../engine/LiveGraph.h"
#include "modulation.h"
#include "modulation_dsp.h"
#include "modulation_editors.h"
#include "modulation_skin.h"

namespace gl::packs::modulation {

namespace {

using sk::openForm;

// History graph with grid inside a graph well.
void monitor(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const skin::Skin& k, uint64_t key, std::initializer_list<std::pair<int, ImU32>> lanes) {
  skin::graphWell(dl, p0, p1, k);
  for (int g = 1; g < 4; ++g) dl->AddLine(ImVec2(p0.x + 1, p0.y + (p1.y - p0.y) * g / 4), ImVec2(p1.x - 1, p0.y + (p1.y - p0.y) * g / 4), k.graphGrid);
  ImVec2 in(p0.x + 4, p0.y + 4), sz(p1.x - p0.x - 8, p1.y - p0.y - 8);
  for (auto [lane, col] : lanes) w::drawHistory(dl, in, sz, key, lane, col, 1.8f);
}

void tensionPreview(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const skin::Skin& k, double tension) {
  skin::graphWell(dl, p0, p1, k);
  ImVec2 pts[24];
  for (int i = 0; i < 24; ++i) {
    double x = i / 23.0;
    pts[i] = ImVec2(p0.x + 3 + static_cast<float>(x) * (p1.x - p0.x - 6), p1.y - 3 - static_cast<float>(tensionCurve(x, tension)) * (p1.y - p0.y - 6));
  }
  dl->AddPolyline(pts, 24, k.graphCurve, 0, 1.5f);
}

const std::vector<std::string>& lfoShapeLabels() {
  static const std::vector<std::string> v = {"SIN", "TRI", "SQR", "SAW", "RND"};
  return v;
}

}  // namespace

// ------------------------------------------------------------ Peak Follower
// Reference panel 521 × 209: PEAK panel | LFO panel on top, the monitor sheet
// underneath, ramp / mute check boxes at the bottom right.
void peakEditor(EditTarget& e) {
  const skin::Skin& K = sk::controller();
  const skin::Skin& L = sk::controllerLfo();
  for (int k = 0; k < 3; ++k) w::pushHistory(e.key, k, e.output(k));
  constexpr float kW = 560, kH = 214;
  skin::Form F = openForm(e, K, kW, kH);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::Scope scope(K);
  sk::panelBox(dl, F, 7, 7, 234, 83, K, "PEAK");
  const bool ref = e.p(PkModel, 0) >= 0.5f;  // J-42 reference: the stored integers, no attack, tempo-locked LFO
  if (ref) {
    const char* pk[4] = {"BASE", "VOL", "TENSION", "DECAY"};
    const int pkp[4] = {PkRBase, PkRAmount, PkRTension, PkRDecay};
    for (int i = 0; i < 4; ++i) sk::wheelL(e, pkp[i], F, 13 + i * 57, 26, 36, K, pk[i]);
  } else {
    const char* pk[5] = {"BASE", "VOL", "TENSION", "ATTACK", "DECAY"};
    const int pkp[5] = {PkBase, PkAmount, PkTension, PkAttack, PkDecay};
    for (int i = 0; i < 5; ++i) sk::wheelL(e, pkp[i], F, 13 + i * 46, 26, 36, K, pk[i]);
  }
  sk::panelBox(dl, F, 249, 7, 304, 83, L, "LFO");
  sk::wheelL(e, ref ? LfRBase : LfBase, F, 255, 26, 36, L, "BASE");
  sk::wheelL(e, ref ? LfRAmount : LfAmount, F, 297, 26, 36, L, "VOL");
  sk::wheelL(e, ref ? LfRTension : LfTension, F, 339, 26, 36, L, "TENSION");
  sk::menuBox(e, LfShape, F.p(381, 32), F.sz(54, 24), L);
  sk::text(dl, F, 408, 64, L, "SHAPE", 7.5f, true);
  if (ref) {
    sk::wheelL(e, LfRSpeed, F, 452, 26, 36, L, "SPEED");
    sk::wheelL(e, LfRPhase, F, 509, 26, 36, L, "PHASE");
  } else {
    const bool sync = e.p(LfSync, 1) >= 0.5f;
    if (sync) {
      sk::menuBox(e, LfSyncRate, F.p(441, 32), F.sz(58, 24), L);
      sk::text(dl, F, 470, 64, L, "LENGTH", 7.5f, true);
    } else {
      sk::wheelL(e, LfRate, F, 452, 26, 36, L, "SPEED");
    }
    sk::check(e, LfSync, F.p(470, 9), F.sz(60, 14), L, "SYNC");
    sk::wheelL(e, LfPhase, F, 509, 26, 36, L, "PHASE");
  }
  // J-68: the tension wheels show their curve (the engine's own tension law,
  // input level → output) drawn over the cap, redrawn when tension moves.
  {
    const PluginState* ps = e.st;
    if (ps) {
      const PluginState& st = *ps;
      auto vis = [&](bool lfo, float x, const skin::Skin& sk) {
        const int prm = lfo ? (ref ? LfRTension : LfTension) : (ref ? PkRTension : PkTension);
        const uint64_t in = livegraph::Hasher().f(e.p(prm, 0)).f(e.p(PkModel, 0)).value();
        const livegraph::Curve& c = livegraph::memo<livegraph::Curve>(e.key, lfo ? 0x504C5456u : 0x504B5456u).get(in, [&](livegraph::Curve& o) {
          o.resize(17);
          for (int i = 0; i < 17; ++i) o[static_cast<size_t>(i)] = static_cast<float>(peakTensionLaw(st, lfo, i / 16.0));
        });
        const ImVec2 q0 = F.p(x + 10, 36), q1 = F.p(x + 26, 52);
        ImVec2 pts[17];
        for (int i = 0; i < 17; ++i)
          pts[i] = ImVec2(q0.x + (q1.x - q0.x) * i / 16.0f, q1.y - (q1.y - q0.y) * std::clamp(c[static_cast<size_t>(i)], 0.0f, 1.0f));
        dl->AddPolyline(pts, 17, theme::withAlpha(sk.fill, 0.95f), 0, 1.4f);
      };
      vis(false, ref ? 127.0f : 105.0f, K);
      vis(true, 339, L);
    }
  }
  // Monitor sheet.
  sk::panelBox(dl, F, 7, 98, 546, 83, K);
  monitor(dl, F.p(12, 103), F.p(548, 176), K, e.key, {{2, theme::withAlpha(L.fill, 0.75f)}, {0, K.text}, {1, K.fill}});
  sk::readout(dl, F.p(7, 189), F.p(103, 207), K, "PEAK", e.output(1));
  {
    skin::Skin lk = L;
    sk::readout(dl, F.p(108, 189), F.p(196, 207), lk, "LFO", e.output(2));
  }
  sk::readout(dl, F.p(201, 189), F.p(311, 207), K, "P + LFO", e.output(0));
  sk::check(e, PkRamp, F.p(380, 189), F.sz(76, 18), K, "SMOOTH");
  sk::check(e, PkMute, F.p(458, 189), F.sz(95, 18), K, "MUTE AUDIO");
  if (ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered())
    e.a->setHint("Link any knob to Peak + LFO, Peak or LFO: right-click the knob, Link to controller");
  F.end(0, kH);
}

// ----------------------------------------------------------- X-Y / X-Y-Z pads
namespace {

void padArea(EditTarget& e, int px, int py, ImVec2 p0, ImVec2 size, const skin::Skin& k, float outX, float outY, bool invertY) {
  ImGui::SetCursorScreenPos(p0);
  ImGui::InvisibleButton("##pad", size);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::graphWell(dl, p0, ImVec2(p0.x + size.x, p0.y + size.y), k);
  for (int g = 1; g < 8; ++g) {
    ImU32 c = g == 4 ? k.graphGridStrong : k.graphGrid;
    dl->AddLine(ImVec2(p0.x + size.x * g / 8, p0.y + 1), ImVec2(p0.x + size.x * g / 8, p0.y + size.y - 1), c);
    dl->AddLine(ImVec2(p0.x + 1, p0.y + size.y * g / 8), ImVec2(p0.x + size.x - 1, p0.y + size.y * g / 8), c);
  }
  // J-91: positions drawn and read through one mapping (app/XyPadMath.h), always inside the pad.
  const xypad::Rect pr{p0.x, p0.y, p0.x + size.x, p0.y + size.y};
  ImVec2 cp;
  xypad::toScreen(pr, e.p(px, 0.5f), e.p(py, 0.5f), 0.0f, cp.x, cp.y);
  dl->AddLine(ImVec2(cp.x, p0.y + 1), ImVec2(cp.x, p0.y + size.y - 1), theme::withAlpha(k.text, 0.35f));
  dl->AddLine(ImVec2(p0.x + 1, cp.y), ImVec2(p0.x + size.x - 1, cp.y), theme::withAlpha(k.text, 0.35f));
  float r = std::clamp(size.x * 0.04f, 6.0f, 11.0f);
  dl->AddCircle(cp, r, k.text, 24, 1.6f);
  ImVec2 op;
  xypad::toScreen(pr, outX, invertY ? 1.0f - outY : outY, 0.0f, op.x, op.y);
  dl->AddCircleFilled(op, r * 0.55f, k.accent2C(), 20);
  ImGuiIO& io = ImGui::GetIO();
  if (ImGui::IsItemActive() && e.st) {
    PluginState st = *e.st;
    xypad::fromScreen(pr, io.MousePos.x, io.MousePos.y, 0.0f, st.params[static_cast<size_t>(px)], st.params[static_cast<size_t>(py)]);
    if (io.KeyShift) {  // snap to eighths
      st.params[static_cast<size_t>(px)] = std::round(st.params[static_cast<size_t>(px)] * 8) / 8;
      st.params[static_cast<size_t>(py)] = std::round(st.params[static_cast<size_t>(py)] * 8) / 8;
    }
    e.set(st, "move pad", e.key ^ 0x58595041ull);
  }
  if (ImGui::IsItemDeactivated()) e.a->doc.endGesture();
  if (ImGui::IsItemHovered()) e.a->setHint("Drag to move (Shift snaps to the grid). Ring = position, dot = smoothed output.");
}

std::map<uint64_t, int>& xyAxis() {
  static std::map<uint64_t, int> m;
  return m;
}

}  // namespace

// Reference panel 239 × 352: the pad on top, the axis bevel (axis switch,
// level / min / max / tension) and motion row underneath.
void xyEditor(EditTarget& e) {
  const skin::Skin& K = sk::xyPad();
  constexpr float kW = 239, kH = 352;
  skin::Form F = openForm(e, K, kW, kH, 350);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::Scope scope(K);
  const bool inv = e.p(XyInvertY) >= 0.5f;
  padArea(e, XyX, XyY, F.p(5, 5), F.sz(229, 229), K, e.output(0), e.output(1), inv);
  // Bottom panel.
  dl->AddRectFilled(F.p(0, 240), F.p(kW, kH), K.panelLo, 4 * F.s, ImDrawFlags_RoundCornersBottom);
  dl->AddRectFilled(F.p(0, 240), F.p(kW, 295), K.wellC(), 4 * F.s, ImDrawFlags_RoundCornersBottom);
  int& axis = xyAxis()[e.key];
  if (sk::button("##axis", F.p(12, 250), F.sz(36, 32), K, axis ? "Y" : "X", true)) axis = 1 - axis;
  if (ImGui::IsItemHovered()) e.a->setHint("Axis shown by the row of wheels (click to switch X / Y)");
  const int lvl = axis ? XyY : XyX, mn = axis ? XyYMin : XyXMin, mx = axis ? XyYMax : XyXMax, ten = axis ? XyYTension : XyXTension;
  sk::wheelL(e, lvl, F, 58, 244, 36, K, "LEVEL");
  sk::wheelL(e, mn, F, 102, 244, 36, K, "MIN");
  sk::wheelL(e, mx, F, 146, 244, 36, K, "MAX");
  sk::wheelL(e, ten, F, 194, 244, 36, K, "TENSION");
  sk::wheelL(e, XySmooth, F, 12, 300, 36, K, "SMOOTH");
  tensionPreview(dl, F.p(56, 302), F.p(96, 334), K, e.p(ten));
  sk::check(e, XyInvertY, F.p(104, 302), F.sz(70, 16), K, "INV Y");
  sk::readout(dl, F.p(104, 322), F.p(168, 338), K, "X", e.output(0));
  sk::readout(dl, F.p(172, 322), F.p(234, 338), K, "Y", e.output(1));
  sk::text(dl, F, 178, 304, K, "OUTPUT", 7.0f);
  F.end(0, kH);
}

// Reference panel 425 × 417: the pad and its x / y / z sliders on the left,
// the settings box along the bottom; the ranges sit where the reference keeps
// its (hidden) envelope view.
void xyzEditor(EditTarget& e) {
  const skin::Skin& K = sk::xyzPad();
  constexpr float kW = 425, kH = 360;
  skin::Form F = openForm(e, K, kW, kH);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::Scope scope(K);
  padArea(e, XzX, XzY, F.p(8, 8), F.sz(240, 240), K, e.output(0), e.output(1), false);
  // Z strip.
  {
    ImVec2 p0 = F.p(256, 8), sz = F.sz(26, 240);
    ImGui::SetCursorScreenPos(p0);
    ImGui::InvisibleButton("##z", sz);
    skin::graphWell(dl, p0, ImVec2(p0.x + sz.x, p0.y + sz.y), K);
    float z = e.p(XzZ, 0.5f), yz = p0.y + (1 - z) * sz.y;
    dl->AddRectFilled(ImVec2(p0.x + 4, yz), ImVec2(p0.x + sz.x - 4, p0.y + sz.y - 3), theme::withAlpha(K.fill, 0.3f), 2);
    dl->AddLine(ImVec2(p0.x + 2, yz), ImVec2(p0.x + sz.x - 2, yz), K.fill, 2);
    dl->AddCircleFilled(ImVec2(p0.x + sz.x * 0.5f, p0.y + (1 - e.output(2)) * sz.y), 3.5f * F.s, K.accent2C());
    if (ImGui::IsItemActive()) e.setParam(XzZ, std::clamp(1.0f - (ImGui::GetIO().MousePos.y - p0.y) / sz.y, 0.0f, 1.0f));
    if (ImGui::IsItemDeactivated()) e.a->doc.endGesture();
    if (ImGui::IsItemHovered()) e.a->setHint("Z: drag up and down");
  }
  // Ranges.
  sk::panelBox(dl, F, 290, 8, 127, 240, K, "RANGES");
  const char* ax[3] = {"X", "Y", "Z"};
  for (int a = 0; a < 3; ++a) {
    float y = 34 + a * 72;
    sk::wheelL(e, XzXMin + 2 * a, F, 298, y, 34, K, "MIN");
    sk::wheelL(e, XzXMax + 2 * a, F, 344, y, 34, K, "MAX");
    sk::text(dl, F, 397, y + 9, K, ax[a], 11.0f, false, K.fill);
  }
  // x / y / z sliders under the pad.
  for (int a = 0; a < 3; ++a) {
    float x = 8 + a * 92;
    sk::text(dl, F, x, 258, K, ax[a], 9.0f);
    sk::hslider(e, XzX + a, F.p(x + 12, 255), F.sz(74, 21), K);
  }
  // Settings box.
  dl->AddRectFilledMultiColor(F.p(0, 290), F.p(kW, kH), K.wellC(), K.wellC(), K.panel, K.panel);
  dl->AddLine(F.p(0, 290), F.p(kW, 290), skin::darken(K.panel, 0.4f));
  sk::text(dl, F, 12, 304, K, "SMOOTHING", 8.0f);
  sk::hslider(e, XzSmooth, F.p(90, 300), F.sz(110, 21), K, K.accent2C());
  sk::digi(e, XzSmooth, F.p(206, 300), F.sz(60, 21), K);
  for (int a = 0; a < 3; ++a) sk::readout(dl, F.p(12 + a * 104, 330), F.p(110 + a * 104, 350), K, ax[a], e.output(a));
  F.end(0, kH);
}

// ------------------------------------------------------- Formula Controller
namespace {
struct FormulaUi {
  char buf[512] = {};
  std::string loadedFrom;
  bool init = false;
};
std::map<uint64_t, FormulaUi>& formulaUi() {
  static std::map<uint64_t, FormulaUi> m;
  return m;
}
}  // namespace

// Reference panel 521 × 370: INPUT panel (a / b / c wheels), FORMULA panel
// (edit box, status, help, compile) and the monitor sheet.
void formulaEditor(EditTarget& e) {
  const skin::Skin& K = sk::formula();
  w::pushHistory(e.key, 0, e.output(0));
  FormulaUi& u = formulaUi()[e.key];
  std::string current = e.st ? e.st->data : "";
  if (!u.init || (current != u.loadedFrom && !ImGui::IsAnyItemActive())) {
    std::snprintf(u.buf, sizeof u.buf, "%s", current.empty() ? kDefaultFormula : current.c_str());
    u.loadedFrom = current;
    u.init = true;
  }
  constexpr float kW = 521, kH = 370;
  skin::Form F = openForm(e, K, kW, kH);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::Scope scope(K);
  sk::panelBox(dl, F, 7, 7, 507, 84, K, "INPUT");
  sk::wheelL(e, FmSmooth, F, 30, 28, 40, K, "SMOOTH");
  const int abc[3] = {FmA, FmB, FmC};
  const uint32_t abcCol[3] = {0xCCFF66, 0x66CCFF, 0xFF6699};
  const char* abcName[3] = {"a", "b", "c"};
  for (int i = 0; i < 3; ++i) {
    float x = 160 + i * 112;
    sk::wheel(e, abc[i], F.p(x, 11), 66 * F.s, K);
    ImGui::PushFont(e.a->fontMonoBold, F.font(13.0f));
    ImVec2 ts = ImGui::CalcTextSize(abcName[i]);
    ImVec2 c = F.p(x + 33, 44);
    dl->AddText(ImVec2(std::round(c.x - ts.x * 0.5f), std::round(c.y - ts.y * 0.5f)), skin::hex(abcCol[i]), abcName[i]);
    ImGui::PopFont();
    char buf[16];
    std::snprintf(buf, sizeof buf, "%.3f", static_cast<double>(e.p(abc[i])));
    skin::lcd(dl, F.p(x + 66, 62), F.p(x + 108, 78), K, buf, F.font(8.0f));
  }
  // Formula panel.
  sk::panelBox(dl, F, 7, 99, 507, 95, K, "FORMULA");
  ImGui::PushFont(e.a->fontMono, F.font(10.5f));
  ImGui::PushStyleColor(ImGuiCol_FrameBg, theme::vec(K.graphBg));
  ImGui::PushStyleColor(ImGuiCol_Text, theme::vec(skin::hex(0xE9EFF3)));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8 * F.s, std::max(2.0f, (26 * F.s - ImGui::GetFontSize()) * 0.5f)));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3);
  ImGui::SetCursorScreenPos(F.p(14, 124));
  ImGui::SetNextItemWidth(493 * F.s);
  bool enter = ImGui::InputText("##formula", u.buf, sizeof u.buf, ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(2);
  ImGui::PopFont();
  Expr test;
  std::string err;
  bool ok = compileFormula(u.buf, test, &err);
  {
    bool same = std::string(u.buf) == u.loadedFrom || (u.loadedFrom.empty() && std::string(u.buf) == kDefaultFormula);
    const char* msg = ok ? (same ? "Compiled" : "Valid - press Enter or Apply") : err.c_str();
    dl->AddRectFilled(F.p(14, 159), F.p(318, 188), skin::darken(K.panel, 0.5f), 3);
    ImGui::PushFont(e.a->fontMono, F.font(8.5f));
    dl->PushClipRect(F.p(14, 159), F.p(318, 188), true);
    ImVec2 ts = ImGui::CalcTextSize(msg);
    dl->AddText(ImVec2(F.p(20, 0).x, std::round(F.p(0, 173.5f).y - ts.y * 0.5f)), ok ? skin::hex(0xA7B0B6) : skin::hex(0xFF6B6B), msg);
    dl->PopClipRect();
    ImGui::PopFont();
  }
  static const char* kExamples[][2] = {
      {"Knob a", "a"},
      {"a times b", "a * b"},
      {"Steps of a (count from c)", "quant(a, 1 + round(c * 15))"},
      {"Beat ramp", "saw(beat) * a"},
      {"Bar triangle", "tri(bar) * a + b * (1 - a)"},
      {"Sine wave, speed b", "0.5 + 0.5 * sin(t * 2 * pi * (b * 8)) * a"},
      {"Gate from input level", "in > a ? 1 : 0"},
      {"Random each beat", "rand(floor(beat)) * a"},
      {"Smooth crossfade", "smoothstep(0, 1, a)"},
  };
  if (sk::button("##ex", F.p(324, 159), F.sz(86, 29), K, "EXAMPLES")) ImGui::OpenPopup("##examples");
  if (ImGui::BeginPopup("##examples")) {
    for (const auto& ex : kExamples)
      if (ImGui::MenuItem(ex[0], ex[1])) {
        std::snprintf(u.buf, sizeof u.buf, "%s", ex[1]);
        enter = true;
      }
    ImGui::EndPopup();
  }
  if (sk::button("##help", F.p(414, 159), F.sz(26, 29), K, "?")) ImGui::OpenPopup("##fhelp");
  if (ImGui::BeginPopup("##fhelp")) {
    ImGui::PushFont(e.a->fontMono, 11.5f);
    ImGui::TextUnformatted(i18n::tr("Variables: a b c (knobs, 0..1)  t (seconds)  beat  bar  tempo"));
    ImGui::TextUnformatted(i18n::tr("           in (input level 0..1)  playing (0/1)"));
    ImGui::TextUnformatted(i18n::tr("Operators: + - * / % ^  < <= > >= == !=  && ||  !  c ? x : y"));
    ImGui::TextUnformatted(i18n::tr("Functions: sin cos tan asin acos atan atan2 sinh cosh tanh sqrt abs sign"));
    ImGui::TextUnformatted(i18n::tr("           exp log log2 log10 pow min max floor ceil round trunc frac"));
    ImGui::TextUnformatted(i18n::tr("           mod clamp lerp step smoothstep if saw tri sqr pulse wrap fold"));
    ImGui::TextUnformatted(i18n::tr("           quant rand() rand(seed) deg rad    constants: pi e"));
    ImGui::TextUnformatted(i18n::tr("Waves take a phase in cycles (saw(beat) ramps once per beat)."));
    ImGui::TextUnformatted(i18n::tr("The result is clamped to 0..1."));
    ImGui::PopFont();
    ImGui::EndPopup();
  }
  bool apply = sk::button("##apply", F.p(444, 159), F.sz(63, 29), K, "APPLY", false);
  if ((enter || apply) && ok && e.st) {
    PluginState st = *e.st;
    st.data = u.buf;
    e.set(st, "edit formula", 0);
    u.loadedFrom = st.data;
  }
  // Monitor sheet.
  sk::panelBox(dl, F, 7, 202, 507, 133, K);
  monitor(dl, F.p(12, 207), F.p(509, 330), K, e.key, {{0, K.graphCurve}});
  sk::readout(dl, F.p(7, 342), F.p(160, 364), K, "OUTPUT", e.output(0));
  F.end(0, kH);
}

// ------------------------------------------------------- Envelope Controller
namespace {

std::map<uint64_t, int>& selArt() {
  static std::map<uint64_t, int> m;
  return m;
}

void envGraph(EditTarget& e, int art, ImVec2 p0, ImVec2 size, const skin::Skin& k) {
  int np = std::clamp(e.ip(envParam(art, EaNumPoints), 4), 2, kEnvPoints);
  int sus = e.ip(envParam(art, EaSustain), 2);
  double times[kEnvPoints], levels[kEnvPoints], total = 0;
  for (int i = 0; i < np; ++i) {
    times[i] = e.p(envPointTime(art, i), 0.1f);
    levels[i] = e.p(envPointLevel(art, i));
    total += times[i];
  }
  double view = std::max(1.0, total * 1.15);
  ImGui::SetCursorScreenPos(p0);
  ImGui::InvisibleButton("##env", size);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::graphWell(dl, p0, ImVec2(p0.x + size.x, p0.y + size.y), k);
  const float pad = 10;
  float x0 = p0.x + pad, y0 = p0.y + pad, gw = size.x - 2 * pad, gh = size.y - 2 * pad;
  for (double s = 0; s <= view + 1e-9; s += view > 4 ? 1.0 : 0.25) {
    float x = x0 + static_cast<float>(s / view) * gw;
    dl->AddLine(ImVec2(x, y0), ImVec2(x, y0 + gh), std::fmod(s, 1.0) < 1e-9 ? k.graphGridStrong : k.graphGrid);
  }
  for (int g = 0; g <= 4; ++g) dl->AddLine(ImVec2(x0, y0 + gh * g / 4), ImVec2(x0 + gw, y0 + gh * g / 4), k.graphGrid);
  ImVec2 pts[kEnvPoints + 1];
  pts[0] = ImVec2(x0, y0 + gh);
  double acc = 0;
  for (int i = 0; i < np; ++i) {
    acc += times[i];
    pts[i + 1] = ImVec2(x0 + static_cast<float>(acc / view) * gw, y0 + gh - static_cast<float>(levels[i]) * gh);
  }
  for (int i = 0; i < np; ++i) {
    ImVec2 q[4] = {pts[i], pts[i + 1], ImVec2(pts[i + 1].x, y0 + gh), ImVec2(pts[i].x, y0 + gh)};
    if (pts[i + 1].x > pts[i].x + 0.5f) dl->AddConvexPolyFilled(q, 4, theme::withAlpha(k.graphCurve, 0.12f));
  }
  dl->AddPolyline(pts, np + 1, k.graphCurve, 0, 2.0f);
  if (sus > 0 && sus <= np) {
    float sx = pts[sus].x;
    for (float y = y0; y < y0 + gh; y += 8) dl->AddLine(ImVec2(sx, y), ImVec2(sx, std::min(y + 4, y0 + gh)), k.graphText);
    ImGui::PushFont(e.a->fontMono, 10.0f);
    dl->AddText(ImVec2(sx + 4, y0), k.graphText, e.p(envParam(art, EaLoop)) >= 0.5f ? "SUSTAIN / LOOP" : "SUSTAIN");
    ImGui::PopFont();
  }
  float lv = e.output(art);
  dl->AddLine(ImVec2(x0, y0 + gh - lv * gh), ImVec2(x0 + gw, y0 + gh - lv * gh), theme::withAlpha(k.accent2C(), 0.7f), 1.0f);
  ImGuiIO& io = ImGui::GetIO();
  static int drag = -1;
  static uint64_t dragKey = 0;
  int hover = -1;
  for (int i = 0; i < np; ++i)
    if (std::hypot(io.MousePos.x - pts[i + 1].x, io.MousePos.y - pts[i + 1].y) < 9) hover = i;
  if (ImGui::IsItemActivated()) {
    drag = hover;
    dragKey = e.key ^ static_cast<uint64_t>(art);
  }
  for (int i = 0; i < np; ++i) {
    bool hot = i == hover || (drag == i && dragKey == (e.key ^ static_cast<uint64_t>(art)));
    dl->AddCircleFilled(pts[i + 1], hot ? 6.0f : 4.5f, hot ? skin::lighten(k.fill, 0.4f) : k.graphBg);
    dl->AddCircle(pts[i + 1], hot ? 6.0f : 4.5f, k.graphCurve, 16, 1.6f);
  }
  if (ImGui::IsItemActive() && drag >= 0 && dragKey == (e.key ^ static_cast<uint64_t>(art)) && e.st) {
    PluginState st = *e.st;
    double prevT = 0;
    for (int i = 0; i < drag; ++i) prevT += times[i];
    double t = (io.MousePos.x - x0) / gw * view - prevT;
    st.params[static_cast<size_t>(envPointTime(art, drag))] = static_cast<float>(std::clamp(t, 0.001, 10.0));
    st.params[static_cast<size_t>(envPointLevel(art, drag))] = std::clamp((y0 + gh - io.MousePos.y) / gh, 0.0f, 1.0f);
    e.set(st, "edit envelope point", e.key ^ 0x454E5650ull ^ static_cast<uint64_t>(drag));
  }
  if (ImGui::IsItemDeactivated()) {
    drag = -1;
    e.a->doc.endGesture();
  }
  if (hover >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && ImGui::IsItemHovered()) e.setParam(envParam(art, EaSustain), hover + 1, false);
  if (ImGui::IsItemHovered()) {
    char buf[160];
    if (hover >= 0)
      std::snprintf(buf, sizeof buf, "Point %d: %.0f ms, %.0f%%   (drag; right-click = sustain here)", hover + 1, times[hover] * 1000.0,
                    levels[hover] * 100.0);
    else
      std::snprintf(buf, sizeof buf, "Envelope: %d points, %.2f s   (drag the points)", np, total);
    e.a->setHint(buf);
  }
}

}  // namespace

// Reference panel 737 × 395: output selector column, levels and mode, the
// key-range keyboard on top; the envelope editor underneath with its
// envelope / LFO settings rows.
void envEditor(EditTarget& e) {
  if (e.p(EcModel, 0) >= 0.5f) {  // J-41: articulator model
    envArtEditor(e);
    return;
  }
  const skin::Skin& K = sk::controller();
  int& art = selArt()[e.key];
  art = std::clamp(art, 0, kEnvArts - 1);
  for (int k = 0; k < kEnvArts; ++k) w::pushHistory(e.key, k, e.output(k));
  constexpr float kW = 737, kH = 400;
  skin::Form F = openForm(e, K, kW, kH);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::Scope scope(K);
  // Output scopes (A..D) in the dark bevel.
  dl->AddRectFilled(F.p(0, 0), F.p(126, 104), K.panelLo, 4 * F.s, ImDrawFlags_RoundCornersTopLeft);
  for (int k = 0; k < kEnvArts; ++k) {
    ImVec2 a0 = F.p(5, 5 + k * 24.5f), a1 = F.p(121, 26 + k * 24.5f);
    ImGui::SetCursorScreenPos(a0);
    ImGui::PushID(k);
    if (ImGui::InvisibleButton("##scope", ImVec2(a1.x - a0.x, a1.y - a0.y))) art = k;
    ImGui::PopID();
    skin::graphWell(dl, a0, a1, K);
    w::drawHistory(dl, ImVec2(a0.x + 16 * F.s, a0.y + 2), ImVec2(a1.x - a0.x - 18 * F.s, a1.y - a0.y - 4), e.key, k,
                   k == art ? K.fill : K.graphText, 1.3f);
    char nm[4] = {static_cast<char>('A' + k), 0, 0, 0};
    sk::text(dl, F, 9, 9 + k * 24.5f, K, nm, 8.5f, false, k == art ? K.fill : K.graphText);
  }
  // Articulator selector.
  {
    ImVec2 s0 = F.p(132, 6), s1 = F.p(174, 98);
    dl->AddRectFilled(s0, s1, K.panelLo, 3);
    for (int k = 0; k < kEnvArts; ++k) {
      ImVec2 c0 = F.p(134, 8 + k * 22.5f), c1 = F.p(172, 28 + k * 22.5f);
      ImGui::SetCursorScreenPos(c0);
      ImGui::PushID(k + 10);
      if (ImGui::InvisibleButton("##art", ImVec2(c1.x - c0.x, c1.y - c0.y))) art = k;
      bool hov = ImGui::IsItemHovered();
      ImGui::PopID();
      if (k == art) dl->AddRectFilled(c0, c1, K.wellC(), 2);
      char nm[8];
      std::snprintf(nm, sizeof nm, "ENV %c", 'A' + k);
      ImGui::PushFont(e.a->fontMonoBold, F.font(8.0f));
      ImVec2 ts = ImGui::CalcTextSize(nm);
      dl->AddText(ImVec2(std::round((c0.x + c1.x - ts.x) * 0.5f), std::round((c0.y + c1.y - ts.y) * 0.5f)),
                  k == art ? K.fill : hov ? K.text : K.textDim, nm);
      ImGui::PopFont();
    }
  }
  ImGui::PushID(art);
  sk::wheelL(e, envParam(art, EaBase), F, 182, 6, 30, K, "BASE");
  sk::wheelL(e, envParam(art, EaEnvAmt), F, 218, 6, 30, K, "ENV");
  sk::wheelL(e, envParam(art, EaLfoAmt), F, 254, 6, 30, K, "LFO");
  sk::wheelL(e, envParam(art, EaVelAmt), F, 290, 6, 30, K, "VEL");
  sk::check(e, envParam(art, EaRetrig), F.p(182, 50), F.sz(140, 18), K, "RETRIGGER");
  sk::selector(e, envParam(art, EaMode), F.p(179, 75), F.sz(145, 23), K);
  // Key range keyboard.
  sk::panelBox(dl, F, 331, 7, 399, 91, K);
  {
    int lo = e.ip(envParam(art, EaKeyLow)), hi = e.ip(envParam(art, EaKeyHigh), 127);
    ImVec2 kp = F.p(337, 12), ks = F.sz(387, 52);
    int key = skin::keyboard("##kb", kp, ks, 36, 95, K, nullptr, lo, hi, -1);
    if (key >= 0) e.setParam(envParam(art, EaKeyLow), key, false);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
      // Right-click: the highest key (same hit test as the strip).
      ImGuiIO& io = ImGui::GetIO();
      int whites = 0;
      static const bool black[12] = {false, true, false, true, false, false, true, false, true, false, true, false};
      for (int n = 36; n <= 95; ++n) whites += black[n % 12] ? 0 : 1;
      int wi = static_cast<int>((io.MousePos.x - kp.x) / (ks.x / whites)), n = 36;
      for (int c = 0; n <= 95; ++n)
        if (!black[n % 12] && c++ == wi) break;
      e.setParam(envParam(art, EaKeyHigh), std::min(n, 95), false);
    }
    if (ImGui::IsItemHovered()) e.a->setHint("Left-click: lowest key, right-click: highest key for this output");
    sk::text(dl, F, 337, 72, K, "LOW", 7.5f);
    sk::digi(e, envParam(art, EaKeyLow), F.p(360, 69), F.sz(54, 20), K, w::noteName(lo).c_str());
    sk::text(dl, F, 422, 72, K, "HIGH", 7.5f);
    sk::digi(e, envParam(art, EaKeyHigh), F.p(450, 69), F.sz(54, 20), K, w::noteName(hi).c_str());
    sk::check(e, EcContinuous, F.p(520, 70), F.sz(200, 18), K, "CONTINUOUS OUTPUT");
  }
  // Envelope editor.
  envGraph(e, art, F.p(4, 106), F.sz(729, 196), K);
  sk::panelBox(dl, F, 4, 308, 362, 88, K, "ENVELOPE");
  const int np = std::clamp(e.ip(envParam(art, EaNumPoints), 4), 2, kEnvPoints);
  sk::text(dl, F, 12, 334, K, "POINTS", 8.0f);
  if (sk::button("##less", F.p(64, 330), F.sz(22, 22), K, "-") && np > 2) e.setParam(envParam(art, EaNumPoints), np - 1, false);
  {
    char buf[8];
    std::snprintf(buf, sizeof buf, "%d", np);
    skin::lcd(dl, F.p(90, 330), F.p(118, 352), K, buf, F.font(10.0f));
  }
  if (sk::button("##more", F.p(122, 330), F.sz(22, 22), K, "+") && np < kEnvPoints) e.setParam(envParam(art, EaNumPoints), np + 1, false);
  sk::text(dl, F, 160, 334, K, "SUSTAIN", 8.0f);
  sk::menuBox(e, envParam(art, EaSustain), F.p(220, 330), F.sz(136, 22), K);
  sk::check(e, envParam(art, EaLoop), F.p(12, 364), F.sz(200, 20), K, "LOOP TO SUSTAIN");
  const skin::Skin& L = sk::controllerLfo();
  sk::panelBox(dl, F, 372, 308, 361, 88, L, "LFO");
  sk::selector(e, envParam(art, EaLfoShape), F.p(380, 328), F.sz(220, 24), L, &lfoShapeLabels());
  sk::check(e, envParam(art, EaLfoSync), F.p(612, 330), F.sz(110, 20), L, "TEMPO SYNC");
  sk::text(dl, F, 380, 368, L, "RATE", 8.0f);
  if (e.p(envParam(art, EaLfoSync), 1) >= 0.5f)
    sk::menuBox(e, envParam(art, EaLfoSyncRate), F.p(420, 362), F.sz(110, 24), L);
  else
    sk::digi(e, envParam(art, EaLfoRate), F.p(420, 362), F.sz(110, 24), L);
  sk::text(dl, F, 548, 368, L, "DELAY", 8.0f);
  sk::digi(e, envParam(art, EaLfoDelay), F.p(596, 362), F.sz(126, 24), L);
  ImGui::PopID();
  F.end(0, kH);
}

// ------------------------------------------------------ Keyboard Controller
// Reference panel 558 × 232: note-off and value rows on the left, attack /
// release sliders on the right, the keyboard along the bottom.
void keyEditor(EditTarget& e) {
  const skin::Skin& K = sk::controller();
  if (!e.st) return;
  std::vector<float> vals = parseKeyValues(e.st->data);
  const int lo = 24, hi = 107;
  constexpr float kW = 558, kH = 232;
  skin::Form F = openForm(e, K, kW, kH);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::Scope scope(K);
  const int playing = e.output(2) > 0.5f ? static_cast<int>(std::lround(e.output(3) * 127.0f)) : -1;
  // Key value graph (drawn by dragging), aligned over the keyboard below.
  {
    ImVec2 p0 = F.p(13, 6), sz = F.sz(532, 46);
    ImGui::SetCursorScreenPos(p0);
    ImGui::InvisibleButton("##vals", sz);
    skin::graphWell(dl, p0, ImVec2(p0.x + sz.x, p0.y + sz.y), K);
    float bw = sz.x / static_cast<float>(hi - lo + 1);
    for (int k = lo; k <= hi; ++k) {
      float v = vals[static_cast<size_t>(k)];
      float x = p0.x + (k - lo) * bw;
      int m = k % 12;
      bool black = m == 1 || m == 3 || m == 6 || m == 8 || m == 10;
      dl->AddRectFilled(ImVec2(x + 0.5f, p0.y + sz.y - 2 - v * (sz.y - 4)), ImVec2(x + bw - 0.5f, p0.y + sz.y - 2),
                        k == playing ? skin::lighten(K.fill, 0.4f) : theme::withAlpha(K.fill, black ? 0.55f : 0.9f));
    }
    ImGuiIO& io = ImGui::GetIO();
    {  // J-99: strokes fill every key between samples and are smoothed (Alt: raw); one undo step each.
      drawui::Spec spec;
      spec.surface = "keyctl";
      spec.p0 = p0;
      spec.p1 = ImVec2(p0.x + sz.x, p0.y + sz.y);
      if (const std::vector<float>* res = drawui::stroke(*e.a, ImGui::GetItemID(), ImGui::IsItemActive(), (io.MousePos.x - p0.x) / sz.x,
                                                         std::clamp((p0.y + sz.y - 2 - io.MousePos.y) / (sz.y - 4), 0.0f, 1.0f),
                                                         &vals[static_cast<size_t>(lo)], hi - lo + 1, spec)) {
        for (int k = lo; k <= hi; ++k) vals[static_cast<size_t>(k)] = (*res)[static_cast<size_t>(k - lo)];
        PluginState st = *e.st;
        st.data = formatKeyValues(vals);
        if (!(st == *e.st)) e.set(st, "draw key values", e.key ^ 0x4B455956ull);
      }
    }
    if (ImGui::IsItemDeactivated()) e.a->doc.endGesture();
    if (ImGui::IsItemHovered()) {
      int k = std::clamp(lo + static_cast<int>((io.MousePos.x - p0.x) / bw), lo, hi);
      char buf[96];
      std::snprintf(buf, sizeof buf, "%s: %.0f%%   (drag to draw values, Alt: raw)", w::noteName(k).c_str(), vals[static_cast<size_t>(k)] * 100.0);
      e.a->setHint(buf);
    }
    drawui::chip(*e.a, "keyctl", p0, ImVec2(p0.x + sz.x, p0.y + sz.y));
  }
  sk::check(e, KcNoteOff, F.p(10, 63), F.sz(110, 20), K, "NOTE OFF VALUE");
  sk::hslider(e, KcNoteOffValue, F.p(124, 61), F.sz(163, 24), K, skin::hex(0x77A9E6));
  sk::readout(dl, F.p(298, 63), F.p(414, 83), K, "OUT", e.output(0));
  auto mapWith = [&](const char* id, const char* label, float x, auto fn) {
    if (sk::button(id, F.p(x, 104), F.sz(60, 28), K, label)) {
      PluginState st = *e.st;
      std::vector<float> v = vals;
      fn(v);
      st.data = formatKeyValues(v);
      e.set(st, std::string("key values: ") + label, 0);
    }
  };
  static const bool black[12] = {false, true, false, true, false, false, true, false, true, false, true, false};
  sk::text(dl, F, 10, 92, K, "AUTO MAP", 7.5f);
  mapWith("##ramp", "RAMP", 10, [&](std::vector<float>& v) {
    for (int k = 0; k < 128; ++k) v[static_cast<size_t>(k)] = std::clamp((k - lo) / static_cast<float>(hi - lo), 0.0f, 1.0f);
  });
  mapWith("##white", "WHITE", 72, [&](std::vector<float>& v) {
    int whites = 0, idx = 0;
    for (int k = lo; k <= hi; ++k) whites += black[k % 12] ? 0 : 1;
    for (int k = lo; k <= hi; ++k) {
      if (!black[k % 12]) v[static_cast<size_t>(k)] = static_cast<float>(idx++) / static_cast<float>(std::max(1, whites - 1));
      else v[static_cast<size_t>(k)] = v[static_cast<size_t>(k - 1)];
    }
  });
  mapWith("##smooth", "SMOOTH", 134, [&](std::vector<float>& v) {
    std::vector<float> o = v;
    for (int k = 1; k < 127; ++k) o[static_cast<size_t>(k)] = (v[static_cast<size_t>(k - 1)] + 2 * v[static_cast<size_t>(k)] + v[static_cast<size_t>(k + 1)]) / 4;
    v = o;
  });
  mapWith("##invert", "INVERT", 196, [&](std::vector<float>& v) {
    for (auto& x : v) x = 1.0f - x;
  });
  mapWith("##clear", "CLEAR", 258, [&](std::vector<float>& v) { std::fill(v.begin(), v.end(), 0.0f); });
  sk::readout(dl, F.p(330, 108), F.p(414, 128), K, "VEL", e.output(1));
  // Smoothing sliders.
  sk::text(dl, F, 454, 58, K, "ATT", 8.0f, true);
  sk::text(dl, F, 525, 58, K, "REL", 8.0f, true);
  sk::vslider(e, KcAttack, F.p(427, 68), F.sz(54, 72), K);
  sk::vslider(e, KcRelease, F.p(498, 68), F.sz(54, 72), K);
  // Keyboard.
  sk::panelBox(dl, F, 7, 146, 544, 80, K);
  bool lit[128] = {};
  if (playing >= 0 && playing < 128) lit[playing] = true;
  skin::keyboard("##keys", F.p(13, 151), F.sz(532, 69), lo, hi, K, lit);
  F.end(0, kH);
}

// --------------------------------------------------------------- Voltage Out
// Reference panel 302 × 472: two mirrored columns (source, offset wheel,
// polarity, scale wheel) under the on switch and title strip.
void cvEditor(EditTarget& e) {
  const skin::Skin& K = sk::voltage();
  constexpr float kW = 302, kH = 472;
  skin::Form F = openForm(e, K, kW, kH, 480);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  skin::Scope scope(K);
  const bool on = e.p(CvEnable) >= 0.5f;
  sk::check(e, CvEnable, F.p(14, 6), F.sz(52, 20), K, "ON");
  sk::text(dl, F, 70, 10, K, on ? "DC OUT: DC-COUPLED INTERFACE ONLY" : "OUTPUT OFF", 7.0f, false, on ? skin::hex(0xFFC097) : K.textDim);
  skin::pill(dl, F.p(30, 32), F.p(272, 56), K, "VOLTAGE OUT", F.font(10.0f));
  sk::selector(e, CvPitchScale, F.p(18, 62), F.sz(128, 18), K);
  sk::selector(e, CvRange, F.p(152, 62), F.sz(132, 18), K);
  sk::text(dl, F, 18, 88, K, "CLOCK", 7.0f);
  sk::menuBox(e, CvClockPpq, F.p(48, 84), F.sz(62, 18), K);
  sk::text(dl, F, 116, 88, K, "PULSE", 7.0f);
  sk::digi(e, CvPulse, F.p(144, 84), F.sz(56, 18), K);
  sk::text(dl, F, 206, 88, K, "GLIDE", 7.0f);
  sk::digi(e, CvGlide, F.p(234, 84), F.sz(50, 18), K);
  for (int side = 0; side < 2; ++side) {
    ImGui::PushID(side);
    float x = side ? 145 : 0;
    sk::menuBox(e, side ? CvRightSrc : CvLeftSrc, F.p(x + 26, 110), F.sz(104, 23), K);
    sk::wheelL(e, side ? CvRightOfs : CvLeftOfs, F, x + 33, 146, 90, K, "OFFSET");
    const int bp = side ? CvRightBipolar : CvLeftBipolar;
    sk::toggle(e, bp, F.p(x + 52, 254), F.sz(52, 34), K, e.p(bp) >= 0.5f ? "+/-" : "+");
    sk::wheelL(e, side ? CvRightScale : CvLeftScale, F, x + 38, 300, 80, K, "SCALE");
    skin::pill(dl, F.p(x + 22, 400), F.p(x + 134, 418), K, side ? "RIGHT VOLTAGE" : "LEFT VOLTAGE", F.font(8.0f));
    ImGui::PopID();
  }
  sk::readout(dl, F.p(14, 438), F.p(102, 462), K, "PITCH", e.output(0));
  sk::readout(dl, F.p(107, 438), F.p(195, 462), K, "GATE", e.output(1));
  sk::readout(dl, F.p(200, 438), F.p(288, 462), K, "VEL", e.output(2));
  F.end(0, kH);
}

}  // namespace gl::packs::modulation
