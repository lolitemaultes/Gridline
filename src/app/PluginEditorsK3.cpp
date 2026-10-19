// Gridline — κ3 editors: the stereo / creative effects drawn at the reference
// form geometry with the skin kit (SkinKit.h + SkinForms.h): Balance (gl.utility),
// Auto Pan (gl.autopan), Stereo Matrix (gl.stereomatrix), Stereo Shaper
// (gl.stereo), Waveshaper, Clip Dist (gl.fastdist), Flangus (gl.swirl),
// Chorus, Shaper / Love Filter, Time Gate.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include "../engine/Dsp.h"
#include "../engine/Effects.h"
#include "../engine/Program.h"
#include "../engine/WaveCurve.h"
#include <map>
#include "EnvEditor.h"
#include "../packs/dynamics/dynamics.h"
#include "../packs/spacefx/spacefx.h"
#include "../packs/utility/utility.h"
#include "PluginEditorsCommon.h"
#include "PluginEditorsK3.h"
#include "SkinForms.h"
#include "PluginSkin.h"

namespace gl {

namespace {

using namespace ed;
namespace sk = skin;
namespace fm = skin::forms;

float effectBand(App& a, const PluginTarget& t, int k, float dflt = -120.f) {
  if (t.isChannel()) return dflt;
  if (auto rt = a.builder.effectRuntime(t.owner, t.slot)) return rt->bands[k].load();
  return dflt;
}

// Standard parameter interaction (pui::dragParamItem) on the last item.
bool dragBound(App& a, const PluginTarget& t, int param, float pixels) {
  ParamId pid = t.pid(param);
  auto d = describeParam(a.doc.song(), pid);
  auto v = getParam(a.doc.song(), pid);
  if (!d || !v) return false;
  double val = *v;
  return pui::dragParamItem(a, pid, *d, val, pixels, false);
}

std::string fmtParam(App& a, const PluginTarget& t, int param) {
  auto d = describeParam(a.doc.song(), t.pid(param));
  auto v = getParam(a.doc.song(), t.pid(param));
  return d && v ? d->format(*v) : std::string();
}

// Vertical stereo peak meter (two bars) in dBFS (-60..+6).
void stereoMeter(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float dbL, float dbR, const sk::Palette& pal) {
  dl->AddRectFilled(p0, p1, pal.lcdBg, 3);
  float w = (p1.x - p0.x - 9) / 2;
  float db[2] = {dbL, dbR};
  for (int c = 0; c < 2; ++c) {
    ImVec2 a(p0.x + 3 + c * (w + 3), p0.y + 3), b(a.x + w, p1.y - 3);
    dl->AddRectFilled(a, b, theme::withAlpha(pal.rail, 0.6f), 2);
    float n = std::clamp((db[c] + 60.f) / 66.f, 0.f, 1.f);
    float y = b.y - n * (b.y - a.y);
    float y0 = b.y - (60.f / 66.f) * (b.y - a.y);  // 0 dB line
    if (n > 0) {
      dl->AddRectFilled(ImVec2(a.x, std::max(y, y0)), b, pal.fill, 2);
      if (y < y0) dl->AddRectFilled(ImVec2(a.x, y), ImVec2(b.x, y0), IM_COL32(0xE5, 0x48, 0x4D, 255), 2);
    }
    dl->AddLine(ImVec2(a.x, y0), ImVec2(b.x, y0), theme::withAlpha(pal.text, 0.5f), 1);
  }
}

// ======================================================================= Balance
void editorBalance(App& a, const PluginTarget& t, float width) {
  using namespace pp;
  sk::Palette pal = sk::paletteFrom(fm::kBalance);
  pal.text = IM_COL32(0xF4, 0xF6, 0xF8, 255);
  pal.textDim = IM_COL32(0x2A, 0x2E, 0x31, 255);
  sk::View v = sk::begin(fm::kBalance, width, 2.6f, pal);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  // Value readouts above the knobs.
  std::string ps = fmtParam(a, t, UtlPan), vs = fmtParam(a, t, UtlGain);
  sk::textAt(dl, v.p0(v.c("PanWheelLabel")), v.p1(v.c("PanWheelLabel")), pal.text, ps.c_str(), v.px(11), 0, true);
  sk::textAt(dl, v.p0(v.c("VolWheelLabel")), v.p1(v.c("VolWheelLabel")), pal.text, vs.c_str(), v.px(11), 0, true);
  sk::wheel(a, t, v, "PanWheel", UtlPan, pal);
  sk::wheel(a, t, v, "VolWheel", UtlGain, pal);
  sk::label(v, "PanLabel", pal, "BALANCE");
  sk::label(v, "VolLabel", pal, "VOLUME");
  stereoMeter(dl, v.p0(v.c("PeakMeter")), v.p1(v.c("PeakMeter")), effectBand(a, t, 0), effectBand(a, t, 1), pal);
  sk::end(v);
  // Gridline additions (width, polarity, mute, DC) in a strip under the form.
  ImGui::Dummy(ImVec2(0, 4));
  float w = v.f->w * v.s;
  pui::beginPanel("MORE", w);
  float c = (w - 20) / 4;
  pui::knob(a, t, UtlWidth, "Width", pui::KnobSmall, c);
  ImGui::SameLine(0, 0);
  ImGui::BeginGroup();
  pui::led(a, t, UtlMono, "Mono");
  pui::led(a, t, UtlSwap, "Swap L/R");
  pui::led(a, t, UtlDcRemove, "Remove DC");
  ImGui::EndGroup();
  ImGui::SameLine(0, 12);
  ImGui::BeginGroup();
  pui::led(a, t, UtlInvert, "Invert");
  static const char* which[3] = {"Both", "L", "R"};
  pui::segmented(a, t, UtlInvertMode, c, which);
  ImGui::EndGroup();
  ImGui::SameLine(0, 12);
  ImGui::BeginGroup();
  pui::led(a, t, UtlMute, "Mute");
  pui::segmented(a, t, UtlMuteChannel, c, which);
  static const char* law[3] = {"Balance", "Power", "Folded"};
  pui::segmented(a, t, UtlPanLaw, c, law);
  ImGui::EndGroup();
  pui::endPanel();
}

// Vertical background gradient (lighter top) over a rect.
void gradient(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 base, float lift = 0.12f) {
  ImU32 top = theme::mix(base, IM_COL32(255, 255, 255, 255), lift), bot = theme::mix(base, IM_COL32(0, 0, 0, 255), lift * 0.6f);
  dl->AddRectFilledMultiColor(p0, p1, top, top, bot, bot);
}

// ======================================================================= Flangus (gl.swirl)
void editorFlangus(App& a, const PluginTarget& t, float width) {
  namespace sf = packs::spacefx;
  sk::Palette pal = sk::paletteFrom(fm::kFlangus);
  pal.text = IM_COL32(0xE6, 0xE9, 0xEC, 255);
  sk::View v = sk::begin(fm::kFlangus, width, 2.4f, pal);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  gradient(dl, v.o, v.p(static_cast<float>(v.f->w), static_cast<float>(v.f->h)), pal.panel, 0.16f);
  struct Col {
    const char* ctl;
    const char* lbl;
    const char* lblCtl;
    int param;
  };
  const bool ref = pui::paramValue(a, t, sf::SwModel) >= 0.5;
  const Col cols[7] = {{"ChDepthWheel", "DEPTH", "ChDepthWheelLabel", ref ? sf::SwDepthK : sf::SwDepth},
                       {"ChSpeedWheel", "SPEED", "ChSpeedWheelLabel", ref ? sf::SwSpeedK : sf::SwSpeed},
                       {"ChDelayWheel", "DELAY", "ChDelayWheelLabel", ref ? sf::SwDelayK : sf::SwDelay},
                       {"ChSpreadWheel", "SPREAD", "ChSpreadWheelLabel", ref ? sf::SwSpreadK : sf::SwSpread},
                       {"ChCrossWheel", "CROSS", "ChCrossWheelLabel", ref ? sf::SwCrossK : sf::SwCross},
                       {"ChDryWheel", "DRY", "ChDryWheelLabel", ref ? sf::SwDryK : sf::SwDry},
                       {"ChWetWheel", "WET", "ChWetWheelLabel", ref ? sf::SwWetK : sf::SwWet}};
  for (const Col& c : cols) {
    sk::slider(a, t, v, c.ctl, c.param, pal);
    const sk::FormCtl& l = v.c(c.lblCtl);
    sk::textAt(dl, v.p(static_cast<float>(l.x - 6), static_cast<float>(l.y - 2)), v.p(static_cast<float>(l.x + l.w + 6), static_cast<float>(l.y + l.h + 4)),
               pal.text, c.lbl, std::clamp(v.px(7.5f), 10.f, 13.f));
  }
  // Order: number stepper (drag / wheel), voices 1..9.
  {
    const sk::FormCtl& oc = v.c("ChOrderWheel");
    ImVec2 p0 = v.p0(oc), p1 = v.p1(oc);
    ImGui::SetCursorScreenPos(p0);
    ImGui::PushID("order");
    ImGui::InvisibleButton("##o", ImVec2(p1.x - p0.x, p1.y - p0.y));
    dragBound(a, t, sf::SwOrder, 90.f);
    ImGui::PopID();
    int ord = static_cast<int>(std::lround(pui::paramValue(a, t, sf::SwOrder)));
    dl->AddRectFilled(p0, p1, pal.lcdBg, v.px(3));
    dl->AddRect(p0, p1, theme::mix(pal.panel, IM_COL32(0, 0, 0, 255), 0.5f), v.px(3));
    char b[8];
    std::snprintf(b, sizeof b, "%d", ord);
    sk::textAt(dl, ImVec2(p0.x, p0.y + v.px(16)), ImVec2(p1.x, p1.y - v.px(16)), pal.fill, b, v.px(16), 0, true);
    float cx = (p0.x + p1.x) * 0.5f, aw = v.px(5);
    dl->AddTriangleFilled(ImVec2(cx - aw, p0.y + v.px(11)), ImVec2(cx + aw, p0.y + v.px(11)), ImVec2(cx, p0.y + v.px(5)), pal.textDim);
    dl->AddTriangleFilled(ImVec2(cx - aw, p1.y - v.px(11)), ImVec2(cx + aw, p1.y - v.px(11)), ImVec2(cx, p1.y - v.px(5)), pal.textDim);
    const sk::FormCtl& l = v.c("ChOrderWheelLabel");
    sk::textAt(dl, v.p(static_cast<float>(l.x - 4), static_cast<float>(l.y - 2)), v.p(static_cast<float>(l.x + l.w + 4), static_cast<float>(l.y + l.h + 4)),
               pal.text, "VOICES", std::clamp(v.px(7.f), 10.f, 13.f));
  }
  // Space above the order stepper: a small picture of the voice spread.
  {
    ImVec2 q0 = v.p(5, 10), q1 = v.p(36, 66);
    dl->AddRectFilled(q0, q1, theme::withAlpha(pal.lcdBg, 0.7f), v.px(3));
    int ord = static_cast<int>(std::lround(pui::paramValue(a, t, sf::SwOrder)));
    double spread = pui::paramValue(a, t, ref ? sf::SwSpreadK : sf::SwSpread);
    for (int k = 0; k < ord; ++k) {
      float f = ord > 1 ? static_cast<float>(k) / (ord - 1) : 0.5f;
      float x = q0.x + (q1.x - q0.x) * (0.5f + (f - 0.5f) * static_cast<float>(spread) * 0.8f);
      float y = q0.y + (q1.y - q0.y) * (0.15f + 0.7f * (k + 0.5f) / std::max(1, ord));
      dl->AddCircleFilled(ImVec2(x, y), v.px(2.2f), pal.fill);
    }
  }
  sk::end(v);
}

// ======================================================================= Stereo Matrix (Stereo Shaper reference)
void editorStereoMatrix(App& a, const PluginTarget& t, float width) {
  namespace ut = packs::utility;
  sk::Palette pal = sk::paletteFrom(fm::kStereoshaper);
  pal.text = IM_COL32(0xE4, 0xE8, 0xEB, 255);
  pal.fill = sk::col(0xFCC202);
  sk::View v = sk::begin(fm::kStereoshaper, width, 2.4f, pal);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  gradient(dl, v.o, v.p(static_cast<float>(v.f->w), static_cast<float>(v.f->h)), pal.panel, 0.10f);
  // Section frames (matrix | effects).
  dl->AddRect(v.p(3, 20), v.p(176, 309), theme::withAlpha(pal.text, 0.18f), v.px(4));
  dl->AddRect(v.p(183, 20), v.p(268, 309), theme::withAlpha(pal.text, 0.18f), v.px(4));
  sk::textAt(dl, v.p0(v.c("LogoLabel")), v.p1(v.c("LogoLabel")), pal.text, "MATRIX", v.px(10), -1);
  sk::textAt(dl, v.p0(v.c("QuickLabel1")), v.p1(v.c("QuickLabel1")), pal.text, "EFFECTS", v.px(10), -1);
  struct Sl {
    const char* ctl;
    int param;
    const char* top;
    const char* bottom;
  };
  static const Sl sl[4] = {{"LRVolWheel", ut::SmRL, "R", "> L"}, {"LVolWheel", ut::SmLL, "L", "> L"}, {"RVolWheel", ut::SmRR, "R", "> R"},
                           {"RLVolWheel", ut::SmLR, "L", "> R"}};
  for (const Sl& s : sl) {
    sk::slider(a, t, v, s.ctl, s.param, pal);
    const sk::FormCtl& c = v.c(s.ctl);
    char lb[16];
    std::snprintf(lb, sizeof lb, "%s%s", s.top, s.bottom);
    sk::textAt(dl, v.p(static_cast<float>(c.x - 4), 288), v.p(static_cast<float>(c.x + c.w + 4), 302), pal.text, lb, std::clamp(v.px(8.f), 9.f, 13.f));
    std::string val = fmtParam(a, t, s.param);
    sk::textAt(dl, v.p(static_cast<float>(c.x - 6), 40), v.p(static_cast<float>(c.x + c.w + 6), 56), pal.textDim, val.c_str(),
               std::clamp(v.px(7.f), 9.f, 12.f), 0, true);
  }
  static const char* pos[2] = {"PRE", "POST"};
  sk::textSelector(a, t, v, "FXPosSelect", ut::SmFxPos, pal, pos, 2);
  const bool classic = pui::paramValue(a, t, ut::SmClassic) >= 0.5;
  sk::wheel(a, t, v, "DelWheel", classic ? ut::SmDelayK : ut::SmDelay, pal);
  sk::wheel(a, t, v, "APFreqWheel", classic ? ut::SmDephaseK : ut::SmDephase, pal);
  sk::textAt(dl, v.p(187, 132), v.p(266, 146), pal.text, "DELAY", std::clamp(v.px(8.f), 9.f, 13.f));
  sk::textAt(dl, v.p(187, 225), v.p(266, 239), pal.text, "DEPHASE", std::clamp(v.px(8.f), 9.f, 13.f));
  std::string dv = fmtParam(a, t, ut::SmDelay), pv = fmtParam(a, t, ut::SmDephase);
  if (classic) {
    double dk = pui::paramValue(a, t, ut::SmDelayK), pk = pui::paramValue(a, t, ut::SmDephaseK);
    char b[40];
    std::snprintf(b, sizeof b, "%.1f ms %s", (std::pow(1001.0, std::fabs(dk)) - 1.0) * 0.5, dk < 0 ? "L" : dk > 0 ? "R" : "");
    dv = b;
    if (std::fabs(pk) < 1e-6) pv = "off";
    else {
      std::snprintf(b, sizeof b, "%.0f Hz %s", (std::pow(5001.0, 1.0 - std::fabs(pk)) - 1.0) * 4.398 + 10.0, pk < 0 ? "L" : "R");
      pv = b;
    }
  }
  sk::textAt(dl, v.p(187, 144), v.p(266, 156), pal.textDim, dv.c_str(), std::clamp(v.px(7.f), 9.f, 12.f), 0, true);
  sk::textAt(dl, v.p(187, 237), v.p(266, 249), pal.textDim, pv.c_str(), std::clamp(v.px(7.f), 9.f, 12.f), 0, true);
  // Difference output (not available in Gridline: shown inactive).
  const sk::FormCtl& sc = v.c("SideChainSelect");
  sk::lcd(dl, v.p0(sc), v.p1(sc), pal, "--");
  sk::textAt(dl, v.p(186, 257), v.p(230, 279), pal.textDim, "DIFF", std::clamp(v.px(7.f), 9.f, 12.f), 1);
  sk::end(v);
}

// ======================================================================= Auto Pan (PanOMatic reference)
// Reference controls: static pan / volume, LFO shape (sine / triangle / square), target (off / pan / volume),
// amount and speed, and the pan x volume pad. Gridline keeps separate pan and volume depths: the target
// selector moves the amount between them.
void editorPanOMatic(App& a, const PluginTarget& t, float width) {
  using namespace pp;
  const PluginState st = *t.state(a.doc.song());
  sk::Palette pal = sk::paletteFrom(fm::kPanomatic);
  pal.text = IM_COL32(0xE4, 0xE8, 0xEB, 255);
  pal.fill = sk::col(0xFFA830);
  sk::View v = sk::begin(fm::kPanomatic, width, 2.4f, pal);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  sk::panel(v, "VectorPanel1", pal);
  sk::panel(v, "TransientsPanel", pal, "LFO");
  sk::wheel(a, t, v, "PanWheel", ApStaticPan, pal);
  sk::wheel(a, t, v, "VolWheel", ApStaticVol, pal);
  sk::label(v, "PanWheelLabel", pal, "PAN");
  sk::label(v, "VolWheelLabel", pal, "VOL");
  {  // static value readouts right of the knobs
    std::string ps = fmtParam(a, t, ApStaticPan), vs = fmtParam(a, t, ApStaticVol);
    sk::textAt(dl, v.p(94, 10), v.p(138, 28), pal.textDim, ps.c_str(), std::clamp(v.px(6.5f), 9.f, 11.f), -1, true);
    sk::textAt(dl, v.p(94, 30), v.p(138, 48), pal.textDim, vs.c_str(), std::clamp(v.px(6.5f), 9.f, 11.f), -1, true);
  }
  sk::selector(
      a, t, v, "LFOShapeSelect", ApShape, pal,
      [&](ImDrawList* d, int k, ImVec2 p0, ImVec2 p1, bool sel) {
        static const int g[3] = {0, 1, 2};
        sk::glyphWave(d, g[k], ImVec2(p0.x + v.px(3), p0.y + v.px(4)), ImVec2(p1.x - v.px(3), p1.y - v.px(4)), sel ? pal.text : pal.textDim,
                      std::max(1.f, v.px(1.2f)));
      },
      3);
  // Target: off / pan / volume. Reference engine: one target + amount; morph engine: the selector
  // moves the amount between the separate pan and volume depths.
  const bool classic = ed::bval(st, ApClassic, true);
  const double pa = ed::val(st, ApPan, 0.7f), va = ed::val(st, ApVolume);
  const int target = classic ? ed::ival(st, ApTarget) : pa > 1e-6 ? 1 : va > 1e-6 ? 2 : 0;
  if (classic) {
    static const char* names[3] = {"OFF", "PAN", "VOL"};
    sk::textSelector(a, t, v, "LFODestSelect", ApTarget, pal, names, 3);
  } else {
    static const char* names[3] = {"OFF", "PAN", "VOL"};
    const sk::FormCtl& c = v.c("LFODestSelect");
    ImVec2 p0 = v.p0(c), p1 = v.p1(c);
    float ch = (p1.y - p0.y) / 3;
    dl->AddRectFilled(p0, p1, pal.well, v.px(2));
    for (int k = 0; k < 3; ++k) {
      ImVec2 a0(p0.x, p0.y + k * ch), a1(p1.x, a0.y + ch);
      ImGui::SetCursorScreenPos(a0);
      ImGui::PushID(900 + k);
      if (ImGui::InvisibleButton("##tg", ImVec2(p1.x - p0.x, ch)) && k != target) {
        PluginState ns = st;
        double amt = std::max(pa, va);
        if (amt <= 1e-6) amt = 0.5;
        ns.params[static_cast<size_t>(ApPan)] = k == 1 ? static_cast<float>(amt) : 0.f;
        ns.params[static_cast<size_t>(ApVolume)] = k == 2 ? static_cast<float>(amt) : 0.f;
        setPluginState(a, t, ns, "LFO target");
      }
      if (ImGui::IsItemHovered()) a.setHint("LFO target");
      ImGui::PopID();
      if (k == target) dl->AddRectFilled(ImVec2(a0.x + 1, a0.y + 1), ImVec2(a1.x - 1, a1.y - 1), theme::withAlpha(pal.fill, 0.35f), v.px(2));
      sk::textAt(dl, a0, a1, k == target ? pal.text : pal.textDim, names[k], std::clamp(v.px(7.5f), 9.f, 12.f));
    }
  }
  const bool sync = ed::bval(st, ApSync, true);
  sk::wheel(a, t, v, "LFOAmtWheel", classic ? ApAmount : target == 2 ? ApVolume : ApPan, pal);
  sk::wheel(a, t, v, "LFOSpdWheel", classic ? ApSpeedK : sync ? ApRate : ApRateHz, pal);
  sk::label(v, "LFOAmtWheelLabel", pal, "AMT");
  sk::label(v, "LFOSpdWheelLabel", pal, "SPD");
  if (!classic) {  // tempo sync (morph engine) in the free space of the LFO title row
    ImVec2 p0 = v.p(6, 88), p1 = v.p(54, 101);
    ImGui::SetCursorScreenPos(p0);
    ImGui::PushID("sync");
    if (ImGui::InvisibleButton("##sy", ImVec2(p1.x - p0.x, p1.y - p0.y))) a.doc.setParam(t.pid(ApSync), sync ? 0 : 1, false);
    if (ImGui::IsItemHovered()) a.setHint("Tempo sync: the speed knob steps through note lengths");
    ImGui::PopID();
    float led = v.px(7);
    ImVec2 l0(p0.x, (p0.y + p1.y - led) * 0.5f);
    dl->AddRectFilled(l0, ImVec2(l0.x + led, l0.y + led), sync ? pal.fill : pal.well, v.px(1.5f));
    sk::textAt(dl, ImVec2(l0.x + led + v.px(2), p0.y), p1, sync ? pal.text : pal.textDim, "SYNC", std::clamp(v.px(6.5f), 8.f, 11.f), -1);
  }
  {  // pan x volume pad: drag sets both static values; the moving dot shows the LFO
    const sk::FormCtl& c = v.c("GridSelect");
    ImVec2 p0 = v.p0(c), p1 = v.p1(c);
    dl->AddRectFilled(p0, p1, sk::col(0x2A2E3A), v.px(3));
    for (int k = 1; k < 8; ++k) {
      float x = p0.x + (p1.x - p0.x) * k / 8, y = p0.y + (p1.y - p0.y) * k / 8;
      ImU32 gc = k == 4 ? IM_COL32(255, 255, 255, 40) : IM_COL32(255, 255, 255, 14);
      dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), gc);
      dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), gc);
    }
    const float vmax = 1.25f;
    auto toScr = [&](double pan, double vol) {
      return ImVec2(p0.x + static_cast<float>((pan + 1) * 0.5) * (p1.x - p0.x), p1.y - static_cast<float>(vol / vmax) * (p1.y - p0.y));
    };
    ImGui::SetCursorScreenPos(p0);
    ImGui::PushID("pad");
    ImGui::InvisibleButton("##pad", ImVec2(p1.x - p0.x, p1.y - p0.y));
    if (ImGui::IsItemActive()) {
      ImVec2 m = ImGui::GetIO().MousePos;
      double pan = std::clamp((m.x - p0.x) / (p1.x - p0.x) * 2 - 1, -1.f, 1.f), vol = std::clamp((p1.y - m.y) / (p1.y - p0.y), 0.f, 1.f) * vmax;
      a.doc.setParam(t.pid(ApStaticPan), pan, true);
      a.doc.setParam(t.pid(ApStaticVol), vol, true);
    }
    if (ImGui::IsItemDeactivated()) a.doc.endGesture();
    if (ImGui::IsItemHovered()) a.setHint("Drag: panning (left / right) and volume (up / down)");
    ImGui::PopID();
    ImVec2 sp = toScr(ed::val(st, ApStaticPan), ed::val(st, ApStaticVol, 1));
    dl->AddCircle(sp, v.px(5), pal.text, 16, std::max(1.f, v.px(1.2f)));
    float live[2] = {static_cast<float>(ed::val(st, ApStaticPan)), static_cast<float>(ed::val(st, ApStaticVol, 1))};
    if (!t.isChannel())
      if (auto rt = a.builder.effectRuntime(t.owner, t.slot)) live[0] = rt->bands[0].load(), live[1] = rt->bands[1].load();
    dl->AddCircleFilled(toScr(live[0], live[1]), v.px(4), pal.fill, 16);
  }
  sk::end(v);
}

// ======================================================================= Clip Dist (Fast Dist reference)
void editorFastDist(App& a, const PluginTarget& t, float width) {
  namespace dy = packs::dynamics;
  const PluginState st = *t.state(a.doc.song());
  sk::Palette pal = sk::paletteFrom(fm::kFastdist);
  pal.text = IM_COL32(0xEE, 0xF0, 0xF2, 255);
  pal.fill = IM_COL32(0xFF, 0xFF, 0xFF, 255);
  sk::View v = sk::begin(fm::kFastdist, width, 2.6f, pal);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  gradient(dl, v.o, v.p(260, 120), pal.panel, 0.14f);
  struct K {
    const char* ctl;
    int param;
    const char* label;
  };
  const bool classic = ed::bval(st, dy::FdClassic, true);
  const K ks[4] = {{"PreAmpWheel", classic ? dy::FdPreK : dy::FdPreAmp, "PRE"}, {"ThresWheel", classic ? dy::FdDrive : dy::FdThreshold, "THRES"},
                   {"MixWheel", classic ? dy::FdMixK : dy::FdMix, "MIX"}, {"PostGainWheel", classic ? dy::FdPostK : dy::FdPost, "POST"}};
  for (const K& k : ks) {
    sk::wheel(a, t, v, k.ctl, k.param, pal);
    const sk::FormCtl& c = v.c(k.ctl);
    sk::textAt(dl, v.p(static_cast<float>(c.x - 4), 70), v.p(static_cast<float>(c.x + c.w + 4), 84), pal.text, k.label, std::clamp(v.px(8.f), 9.f, 13.f));
    std::string val = fmtParam(a, t, k.param);
    sk::textAt(dl, v.p(static_cast<float>(c.x - 6), 86), v.p(static_cast<float>(c.x + c.w + 6), 98), theme::withAlpha(pal.text, 0.6f), val.c_str(),
               std::clamp(v.px(6.5f), 8.f, 11.f), 0, true);
  }
  {  // mode A / B
    int type = ed::ival(st, dy::FdType);
    if (sk::button(v, "ModeSelect", pal, true, type ? "B" : "A", {}, "Distortion type (click to switch)"))
      a.doc.setParam(t.pid(dy::FdType), type ? 0 : 1, false);
    sk::textAt(dl, v.p(112, 70), v.p(156, 84), pal.text, "TYPE", std::clamp(v.px(8.f), 9.f, 13.f));
  }
  if (classic) {  // visual: transfer curve, input 0..1 → output (pre, drive, mix, post applied)
    const sk::FormCtl& c = v.c("Visual");
    ImVec2 p0 = v.p0(c), p1 = v.p1(c);
    dl->AddRectFilled(p0, p1, sk::col(0x1B1A1E));
    auto gk = [](double x) { return x == 1.0 ? 1.0 : 0.1 * (std::pow(11.0, x) - 1.0); };
    double pre = gk(ed::val(st, dy::FdPreK, 1)), post = gk(ed::val(st, dy::FdPostK, 1)), wet = gk(ed::val(st, dy::FdMixK, 1)) * post;
    double dry = (post - wet) * pre * post;
    int type = ed::ival(st, dy::FdType), drive = ed::ival(st, dy::FdDrive, 10);
    ImVec2 pts[121];
    for (int i = 0; i <= 120; ++i) {
      double x = i / 120.0;
      double y = dy::fastDistRef(std::min(1.0, x * pre), type, drive) * wet + x * dry;
      pts[i] = ImVec2(p0.x + (p1.x - p0.x) * static_cast<float>(x), p1.y - static_cast<float>(std::clamp(y, 0.0, 1.0)) * (p1.y - p0.y));
    }
    dl->AddPolyline(pts, 121, IM_COL32(0xFF, 0xFF, 0xFF, 230), 0, std::max(1.5f, v.px(1.2f)));
  } else {  // legacy engine: one cycle of a sine through the distortion
    const sk::FormCtl& c = v.c("Visual");
    ImVec2 p0 = v.p0(c), p1 = v.p1(c);
    dl->AddRectFilled(p0, p1, sk::col(0x1B1A1E));
    dl->AddLine(ImVec2(p0.x, (p0.y + p1.y) * 0.5f), ImVec2(p1.x, (p0.y + p1.y) * 0.5f), IM_COL32(255, 255, 255, 30));
    double pre = std::pow(10.0, ed::val(st, dy::FdPreAmp) / 20.0), thr = std::clamp(static_cast<double>(ed::val(st, dy::FdThreshold, 1)), 0.01, 1.0);
    double mix = ed::val(st, dy::FdMix, 1), post = std::pow(10.0, ed::val(st, dy::FdPost) / 20.0);
    int type = ed::ival(st, dy::FdType);
    ImVec2 pts[121];
    for (int i = 0; i <= 120; ++i) {
      double x = std::sin(2 * 3.14159265358979 * i / 120.0);
      double y = (x + (dy::fastDistCurve(x * pre, thr, type) - x) * mix) * post;
      pts[i] = ImVec2(p0.x + (p1.x - p0.x) * i / 120.0f, (p0.y + p1.y) * 0.5f - static_cast<float>(std::clamp(y, -1.2, 1.2)) * (p1.y - p0.y) * 0.4f);
    }
    dl->AddPolyline(pts, 121, IM_COL32(0xFF, 0xFF, 0xFF, 230), 0, std::max(1.5f, v.px(1.2f)));
  }
  sk::end(v);
}

// ======================================================================= Stereo Shaper (Stereo Enhancer reference)
void editorStereoEnhancer(App& a, const PluginTarget& t, float width) {
  using namespace pp;
  sk::Palette pal = sk::paletteFrom(fm::kStereoenhancer);
  pal.text = IM_COL32(0xF6, 0xF8, 0xFA, 255);
  pal.textDim = IM_COL32(0x2C, 0x33, 0x38, 255);
  pal.fill = IM_COL32(0xFF, 0xFF, 0xFF, 255);
  sk::View v = sk::begin(fm::kStereoenhancer, width, 2.6f, pal);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  gradient(dl, v.o, v.p(static_cast<float>(v.f->w), static_cast<float>(v.f->h)), pal.panel, 0.12f);
  sk::bevel(v, "VectorBevel1", pal);
  const float fs = std::clamp(v.px(8.f), 9.f, 13.f);
  sk::textAt(dl, v.p0(v.c("SepWheelLabel")), v.p1(v.c("SepWheelLabel")), pal.text, "SEPARATION", fs);
  sk::wheel(a, t, v, "SepWheel", StSep, pal);
  {  // wider / merged glyphs under the separation knob
    ImVec2 a0 = v.p0(v.c("SepWheelPlusLabel")), a1 = v.p1(v.c("SepWheelPlusLabel"));
    float m = (a0.y + a1.y) * 0.5f, cx = (a0.x + a1.x) * 0.5f, r = v.px(3);
    dl->AddCircle(ImVec2(cx - r * 1.4f, m), r, pal.text, 10, 1.2f), dl->AddCircle(ImVec2(cx + r * 1.4f, m), r, pal.text, 10, 1.2f);
    ImVec2 b0 = v.p0(v.c("SepWheelMinLabel")), b1 = v.p1(v.c("SepWheelMinLabel"));
    float bx = (b0.x + b1.x) * 0.5f, bm = (b0.y + b1.y) * 0.5f;
    dl->AddCircle(ImVec2(bx - r * 0.5f, bm), r, pal.text, 10, 1.2f), dl->AddCircle(ImVec2(bx + r * 0.5f, bm), r, pal.text, 10, 1.2f);
  }
  static const char* pos[2] = {"PRE", "POST"};
  sk::textSelector(a, t, v, "PosSelect", StFxPos, pal, pos, 2);
  const bool classic = ed::bval(*t.state(a.doc.song()), StClassic, true);
  sk::wheel(a, t, v, "PhaseWheel", classic ? StPhaseK : StHaas, pal);
  sk::textAt(dl, v.p0(v.c("PhaseWheelLLabel")), v.p1(v.c("PhaseWheelLLabel")), pal.text, "L", fs);
  sk::textAt(dl, v.p0(v.c("PhaseWheelRLabel")), v.p1(v.c("PhaseWheelRLabel")), pal.text, "R", fs);
  static const char* inv[3] = {"NONE", "LEFT", "RIGHT"};
  sk::textSelector(a, t, v, "InvSelect", StInvert, pal, inv, 3);
  sk::label(v, "PosSelectLabel", pal, "PHASE", -1, true);
  sk::label(v, "InvSelectLabel", pal, "INVERT", 1, true);
  sk::textAt(dl, v.p0(v.c("LogoLabel")), v.p1(v.c("LogoLabel")), pal.text, "STEREO SHAPER", fs, 0);
  sk::wheel(a, t, v, "PanWheel", StPan, pal);
  sk::wheel(a, t, v, "VolWheel", classic ? StVolK : StVolume, pal);
  sk::textAt(dl, v.p0(v.c("PanWheelLabel")), v.p1(v.c("PanWheelLabel")), pal.text, "PAN", fs);
  sk::textAt(dl, v.p0(v.c("VolWheelLabel")), v.p1(v.c("VolWheelLabel")), pal.text, "VOL", fs);
  {
    std::string hv;
    if (classic) {
      double pk = pui::paramValue(a, t, StPhaseK), ms = (std::pow(1001.0, std::fabs(pk)) - 1.0) * 0.5;
      char b[32];
      std::snprintf(b, sizeof b, "%.1f ms %s", ms, pk < 0 ? "L" : pk > 0 ? "R" : "");
      hv = b;
    } else {
      hv = fmtParam(a, t, StHaas);
    }
    sk::textAt(dl, v.p(128, 92), v.p(183, 107), pal.text, hv.c_str(), std::clamp(v.px(7.f), 8.f, 11.f), 1, true);
  }
  sk::end(v);
  // Gridline additions: width, mid / side levels, mono bass, the L/R matrix.
  ImGui::Dummy(ImVec2(0, 4));
  float w = v.f->w * v.s;
  pui::beginPanel("MORE", w);
  pui::led(a, t, StClassic, "Reference engine");
  if (classic) {
    pui::endPanel();
    return;
  }
  float c = (w - 20) / 9;
  const int ks[9] = {StWidth, StMid, StSide, StMonoBelow, StDephase, StLL, StRL, StLR, StRR};
  const char* ls[9] = {"Width", "Mid", "Side", "Mono <", "Dephase", "L>L", "R>L", "L>R", "R>R"};
  for (int k = 0; k < 9; ++k) {
    if (k) ImGui::SameLine(0, 0);
    pui::knob(a, t, ks[k], ls[k], pui::KnobSmall, c);
  }
  pui::endPanel();
}

// ======================================================================= Chorus (reference: 3 x 4 knob cells)
void editorChorus(App& a, const PluginTarget& t, float width) {
  using namespace pp;
  const PluginState st = *t.state(a.doc.song());
  sk::Palette pal = sk::paletteFrom(fm::kChorus);
  pal.text = IM_COL32(0xE8, 0xEC, 0xEE, 255);
  pal.fill = sk::col(0xB3E5E5);
  sk::View v = sk::begin(fm::kChorus, width, 2.2f, pal);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  struct Cell {
    const char* bevel;
    const char* wheel;
    const char* label;
    const char* value;
    int param;
    const char* caption;
  };
  static const Cell cells[12] = {
      {"DelayBevel", "DelayWheel", "DelayLabel", "DelayValueLabel", ChoDelay, "DELAY"},
      {"DepthBevel", "DepthWheel", "DepthLabel", "DepthValueLabel", ChoDepthMs, "DEPTH"},
      {"StereoBevel", "StereoWheel", "StereoLabel", "StereoValueLabel", ChoStereo, "STEREO"},
      {"LFO1FreqBevel", "LFO1FreqWheel", "LFO1FreqLabel", "LFO1FreqValueLabel", ChoRate, "LFO 1 RATE"},
      {"Lfo2FreqBevel", "LFO2FreqWheel", "LFO2FreqLabel", "LFO2FreqValueLabel", ChoRate2, "LFO 2 RATE"},
      {"Lfo3FreqBevel", "LFO3FreqWheel", "LFO3FreqLabel", "LFO3FreqValueLabel", ChoRate3, "LFO 3 RATE"},
      {"Lfo1WaveBevel", "LFO1WaveWheel", "LFO1WaveLabel", "LFO1WaveValueLabel", ChoWave1, "LFO 1 SHAPE"},
      {"Lfo2WaveBevel", "LFO2WaveWheel", "LFO2WaveLabel", "LFO2WaveValueLabel", ChoWave2, "LFO 2 SHAPE"},
      {"Lfo3WaveBevel", "LFO3WaveWheel", "LFO3WaveLabel", "LFO3WaveValueLabel", ChoWave3, "LFO 3 SHAPE"},
      {"CrossTypeBevel", "CrossTypeWheel", "CrossTypeLabel", "CrossTypeValueLabel", ChoCrossMode, "SPLIT"},
      {"CrossCutoffBevel", "CrossCutoffWheel", "CrossCutoffLabel", "CrossCutoffValueLabel", ChoCrossFreq, "SPLIT FREQ"},
      {"WetOnlyBevel", "WetOnlyWheel", "WetOnlyLabel", "WetOnlyValueLabel", ChoWetOnly, "WET ONLY"},
  };
  const float fs = std::clamp(v.px(7.5f), 9.f, 13.f);
  for (const Cell& c : cells) {
    sk::bevel(v, c.bevel, pal);
    sk::wheel(a, t, v, c.wheel, c.param, pal);
    sk::label(v, c.label, pal, c.caption, 0, true);
    std::string val = fmtParam(a, t, c.param);
    if (c.param == ChoStereo) {
      char b[24];
      std::snprintf(b, sizeof b, "%d degrees", static_cast<int>(180.0 * ed::val(st, ChoStereo, 1.f / 3)));
      val = b;
    }
    const sk::FormCtl& vl = v.c(c.value);
    sk::textAt(dl, v.p0(vl), v.p1(vl), theme::withAlpha(pal.text, 0.8f), val.c_str(), fs, 0, true);
  }
  sk::end(v);
  // Gridline additions: the morphing engine (used when the reference engine is off).
  ImGui::Dummy(ImVec2(0, 4));
  float w = v.f->w * v.s;
  pui::beginPanel("MORE", w);
  pui::led(a, t, ChoClassic, "Reference engine");
  if (!ed::bval(st, ChoClassic, true)) {
    float c = (w - 20) / 5;
    pui::knob(a, t, ChoDepth, "Depth", pui::KnobSmall, c);
    ImGui::SameLine(0, 0);
    pui::knob(a, t, ChoMix, "Mix", pui::KnobSmall, c);
    ImGui::SameLine(0, 0);
    pui::knob(a, t, ChoFeedback, "Feedback", pui::KnobSmall, c);
    ImGui::SameLine(0, 0);
    pui::knob(a, t, ChoShape, "Shape", pui::KnobSmall, c);
    ImGui::SameLine(0, 0);
    ImGui::BeginGroup();
    pui::caption("VOICES");
    pui::segmented(a, t, ChoVoices, c - 8);
    ImGui::EndGroup();
  }
  pui::endPanel();
}

// ======================================================================= Waveshaper (reference: curve editor + settings strip)
struct WsUi {
  envui::State env;
  bool freeze = false, step = false, snap = false, settings = true;
};
std::map<uint64_t, WsUi> wsUi;

void editorWaveShaper(App& a, const PluginTarget& t, float width) {
  using namespace pp;
  const PluginState st = *t.state(a.doc.song());
  WsUi& ui = wsUi[t.key()];
  sk::Palette pal = sk::paletteFrom(fm::kWaveshaper);
  pal.text = IM_COL32(0xE4, 0xE8, 0xEB, 255);
  pal.fill = sk::col(0xD8DDE0);
  sk::View v = sk::begin(fm::kWaveshaper, width, 1.9f, pal);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const bool unipolar = ed::bval(st, WsBipolar, true);
  // Curve (free points; 0.5 projects show their 17 points until edited).
  wscurve::Curve c;
  if (!wscurve::parse(st.data.c_str(), c)) {
    float pts[kShapePoints];
    for (int k = 0; k < kShapePoints; ++k) pts[k] = ed::val(st, k);
    wscurve::fromPoints(pts, kShapePoints, unipolar, c);
  }
  {
    const sk::FormCtl& ec = v.c("MEnvEditor");
    ImVec2 e0 = v.p0(ec), e1 = v.p1(ec);
    if (!ui.settings) e1.y = v.p(0, 345).y;
    envui::Style es;
    es.bg = sk::col(0x2A3238);
    es.grid = IM_COL32(255, 255, 255, 14);
    es.gridStrong = IM_COL32(255, 255, 255, 40);
    es.line = sk::col(0xCAB0E8);
    es.fill = theme::withAlpha(es.line, 0.12f);
    es.point = IM_COL32(0xF0, 0xF0, 0xF0, 255);
    es.pointHot = sk::col(0x49B5F5);
    es.handle = IM_COL32(0xF0, 0xF0, 0xF0, 170);
    es.text = es.point;
    envui::Opts o;
    o.xMax = 1;
    o.gridDiv = 8, o.gridEvery = unipolar ? 8 : 4, o.yLines = 8;
    o.snapDiv = ui.snap ? 16 : 0;
    o.freeze = ui.freeze, o.step = ui.step;
    o.lockEnds = true;
    o.centreLine = !unipolar;
    int r = envui::edit("##wsenv", e0, e1, c, ui.env, o, es);
    if (r & envui::Changed) {
      PluginState ns = st;
      ns.data = wscurve::serialize(c);
      setPluginState(a, t, ns, "edit waveshaper curve", 0x5753000000000000ull ^ t.key());
    }
    if (r & envui::Released) a.doc.endGesture();
    // Input level marker (after the pre amp).
    if (!t.isChannel())
      if (auto rt = a.builder.effectRuntime(t.owner, t.slot)) {
        float pk = std::min(1.f, rt->bands[0].load());
        float xm = unipolar ? pk : pk * 0.5f + 0.5f;
        if (pk > 1e-4f) dl->AddLine(ImVec2(e0.x + xm * (e1.x - e0.x), e0.y), ImVec2(e0.x + xm * (e1.x - e0.x), e1.y), theme::withAlpha(es.pointHot, 0.7f));
      }
  }
  if (sk::button(v, "SettingsBtn", pal, ui.settings, nullptr, sk::glyphMenu, "Show / hide settings")) ui.settings = !ui.settings;
  if (ui.settings) {
    sk::panel(v, "SettingsPanel", pal);
    sk::button(v, "WaveLabel", pal, false, "WAVE");
    sk::textAt(dl, v.p(2, 276), v.p(60, 292), pal.textDim, "SHAPER", std::clamp(v.px(7.f), 9.f, 12.f), -1);
    if (sk::button(v, "PolarBtn", pal, !unipolar, nullptr,
                   [unipolar](ImDrawList* d, ImVec2 p0, ImVec2 p1, ImU32 col) {
                     float m = (p0.y + p1.y) * 0.5f;
                     if (unipolar) {
                       d->AddLine(ImVec2(p0.x, p1.y), ImVec2((p0.x + p1.x) * 0.5f, m), col, 1.5f);
                       d->AddLine(ImVec2((p0.x + p1.x) * 0.5f, m), ImVec2(p1.x, p1.y), col, 1.5f);
                     } else {
                       d->AddLine(ImVec2(p0.x, p1.y), ImVec2(p1.x, p0.y), col, 1.5f);
                     }
                   },
                   unipolar ? "Unipolar (click: bipolar)" : "Bipolar (click: unipolar)")) {
      PluginState ns = st;
      wscurve::Curve cc = c;
      if (unipolar) wscurve::toBipolar(cc);
      else wscurve::toUnipolar(cc);
      ns.data = wscurve::serialize(cc);
      ns.params[static_cast<size_t>(WsBipolar)] = unipolar ? 0.f : 1.f;
      setPluginState(a, t, ns, "waveshaper polarity");
    }
    const bool classic = ed::bval(st, WsClassic, true);
    sk::wheel(a, t, v, "PreAmpWheel", classic ? WsPreAmp : WsPreGain, pal);
    sk::wheel(a, t, v, "MixWheel", WsMix, pal);
    sk::wheel(a, t, v, "PostGainWheel", classic ? WsPost : WsPostGain, pal);
    const float fs = std::clamp(v.px(7.f), 9.f, 12.f);
    sk::textAt(dl, v.p(138, 297), v.p(186, 309), pal.text, "PRE", fs);
    sk::textAt(dl, v.p(180, 297), v.p(228, 309), pal.text, "MIX", fs);
    sk::textAt(dl, v.p(223, 297), v.p(271, 309), pal.text, "POST", fs);
    if (sk::button(v, "MenuBtn", pal, false, nullptr, sk::glyphMenu, "Curve options")) ImGui::OpenPopup("##wsmenu");
    if (ImGui::BeginPopup("##wsmenu")) {
      auto setCurve = [&](const wscurve::Curve& nc, const char* what) {
        PluginState ns = st;
        ns.data = wscurve::serialize(nc);
        setPluginState(a, t, ns, what);
      };
      if (ImGui::MenuItem("Reset (straight line)")) {
        wscurve::Curve nc;
        nc.add(0, 0.f), nc.add(1, 1.f), nc.prepare();
        setCurve(nc, "reset curve");
      }
      if (ImGui::MenuItem("Flip vertically")) {
        wscurve::Curve nc = c;
        for (int i = 0; i < nc.n; ++i) nc.y[i] = 1.f - nc.y[i], nc.t[i] = -nc.t[i];
        nc.prepare();
        setCurve(nc, "flip curve");
      }
      ImGui::Separator();
      bool cl = ed::bval(st, WsClassic, true);
      if (ImGui::MenuItem("Reference gain staging", nullptr, cl)) a.doc.setParam(t.pid(WsClassic), cl ? 0 : 1, false);
      ImGui::EndPopup();
    }
    if (sk::checkFree(v, "FreezeBtn", pal, ui.freeze, "FRZ", "Freeze editing")) ui.freeze = !ui.freeze;
    if (sk::checkFree(v, "StepBtn", pal, ui.step, "STEP", "Step editing")) ui.step = !ui.step;
    if (sk::checkFree(v, "SnapBtn", pal, ui.snap, "SNAP", "Snap to grid")) ui.snap = !ui.snap;
    sk::check(a, t, v, "HqCheckBox", WsHq, pal, "HQ");
    sk::check(a, t, v, "SmoothCheckBox", WsSmooth, pal, "SMTH");
    sk::check(a, t, v, "CenterBtn", WsCenter, pal, "CENTER");
    sk::digi(a, t, v, "OverSelect", WsOversample, pal, [](double x) {
      static const char* n[5] = {"0", "2x", "4x", "8x", "16"};
      return std::string(n[std::clamp(static_cast<int>(std::lround(x)), 0, 4)]);
    });
  }
  sk::end(v);
}

}  // namespace

void editorGrossBeat(App& a, const PluginTarget& t, float width);  // PluginEditorsK3Gb.cpp

bool drawK3Editor(App& a, const PluginTarget& t, float width) {
  const PluginState* st = t.state(a.doc.song());
  if (!st) return false;
  const std::string& ty = st->type;
  if (ty == "gl.utility") editorBalance(a, t, width);
  else if (ty == "gl.swirl") editorFlangus(a, t, width);
  else if (ty == "gl.timegate") editorGrossBeat(a, t, width);
  else if (ty == "gl.stereomatrix") editorStereoMatrix(a, t, width);
  else if (ty == "gl.autopan") editorPanOMatic(a, t, width);
  else if (ty == "gl.fastdist") editorFastDist(a, t, width);
  else if (ty == "gl.stereo") editorStereoEnhancer(a, t, width);
  else if (ty == "gl.chorus") editorChorus(a, t, width);
  else if (ty == "gl.waveshaper") editorWaveShaper(a, t, width);
  else return false;
  return true;
}

}  // namespace gl
