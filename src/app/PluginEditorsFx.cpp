// Gridline — delay / reverb editor dispatch: the reference-model editors
// (PluginEditors{Delay,Time}Ref.cpp) plus the 0.5 legacy-model Delay editor.
#include "../engine/Dsp.h"
#include "PluginEditorsCommon.h"

namespace gl {

using theme::pal;
using namespace pp;

namespace {

using namespace ed;
constexpr double kPI = dsp::kPi;

// Panel helpers: a row of knobs of equal width.
void knobRow(App& a, const PluginTarget& t, std::initializer_list<std::pair<int, const char*>> ks, float width, int style = pui::KnobSmall) {
  float c = width / static_cast<float>(ks.size());
  bool first = true;
  for (auto [k, l] : ks) {
    if (!first) ImGui::SameLine(0, 0);
    first = false;
    pui::knob(a, t, k, l, style, c);
  }
}

// ======================================================================= Delay
void editorDelay(App& a, const PluginTarget& t, float width) {
  const Song& s = a.doc.song();
  const PluginState st = *t.state(s);
  const auto& P = pal();
  const float rowH = 172;
  // Input strip: tall send slider (and input pan under the output column).
  pui::beginPanel("IN", 56, rowH * 2 + 8);
  ImGui::SetCursorPosX(18);
  pui::vslider(a, t, DlyInput, "Send", ImVec2(18, rowH * 2 - 74));
  pui::endPanel();
  ImGui::SameLine(0, 8);
  ImGui::BeginGroup();
  float mainW = width - 56 - 8 - 92 - 8;
  // ---- top row: delay time / stereo | feedback / filter / lo-fi
  float wTime = mainW * 0.5f, wFb = mainW - wTime - 8;
  pui::beginPanel("DELAY", wTime, rowH);
  ImGui::BeginGroup();
  if (bval(st, DlySync, true)) {
    pui::knob(a, t, DlyTime, "Time", pui::KnobLarge, 82);
  } else {
    pui::knob(a, t, DlyTimeMs, "Time", pui::KnobLarge, 82);
  }
  ImGui::EndGroup();
  ImGui::SameLine(0, 2);
  ImGui::BeginGroup();
  pui::led(a, t, DlySync, "Tempo sync");
  pui::led(a, t, DlyKeepPitch, "Keep pitch");
  pui::hslider(a, t, DlyTimeSmooth, "Smooth", 110);
  ImGui::EndGroup();
  ImGui::SameLine(0, 6);
  ImGui::BeginGroup();
  pui::knob(a, t, DlyOffset, "R/L offset", pui::KnobSmall, 62);
  pui::knob(a, t, DlyStereoSpread, "Spread", pui::KnobSmall, 62);
  ImGui::EndGroup();
  ImGui::SameLine(0, 6);
  static const char* modes[5] = {"Stereo", "Ping-pong", "Invert", "Mono", "Off"};
  ImGui::BeginGroup();
  pui::caption("TYPE");
  pui::segmented(a, t, DlyMode, 84, modes, 1);
  ImGui::EndGroup();
  pui::endPanel();
  ImGui::SameLine(0, 8);
  pui::beginPanel("FEEDBACK", wFb, rowH);
  pui::knob(a, t, DlyFeedback, "Level", pui::KnobLarge, 78);
  ImGui::SameLine(0, 0);
  ImGui::BeginGroup();
  int ft = ival(st, DlyFilterType);
  pui::knob(a, t, DlyCutoff, ft == 2 ? "Cutoff" : "High cut", pui::KnobSmall, 60);
  pui::knob(a, t, ft == 0 ? DlyLowCut : DlyResonance, ft == 0 ? "Low cut" : "Reso", pui::KnobSmall, 60);
  ImGui::EndGroup();
  ImGui::SameLine(0, 4);
  static const char* fmodes[5] = {"Classic", "LP", "HP", "BP", "Off"};
  ImGui::BeginGroup();
  pui::caption("FILTER");
  pui::segmented(a, t, DlyFilterType, 62, fmodes, 1);
  ImGui::EndGroup();
  ImGui::SameLine(0, 8);
  ImGui::BeginGroup();
  float lc = (wFb - 78 - 60 - 62 - 44) / 2;
  pui::knob(a, t, DlyRateReduce, "Rate", pui::KnobSmall, lc);
  ImGui::SameLine(0, 0);
  pui::knob(a, t, DlyBits, "Bits", pui::KnobSmall, lc);
  pui::knob(a, t, DlySaturation, "Warmth", pui::KnobSmall, lc);
  ImGui::EndGroup();
  pui::endPanel();
  // ---- bottom row: modulation | diffusion | distortion (+ curve)
  float wMod = mainW * 0.30f, wDif = mainW * 0.22f, wDist = mainW - wMod - wDif - 16;
  pui::beginPanel("MODULATION", wMod, rowH);
  knobRow(a, t, {{DlyModRate, "Rate"}, {DlyModDepth, "Time"}, {DlyModCutoff, "Cutoff"}}, wMod - 20, pui::KnobMedium);
  // LFO preview.
  {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float w = wMod - 22, h = 26;
    pui::displayFrame(dl, p, ImVec2(p.x + w, p.y + h));
    ImVec2 pts[60];
    double depth = val(st, DlyModDepth) + val(st, DlyModCutoff);
    for (int i = 0; i < 60; ++i) pts[i] = ImVec2(p.x + 4 + (w - 8) * i / 59.0f, p.y + h * 0.5f - static_cast<float>(std::sin(i / 59.0 * 4 * kPI) * depth) * h * 0.4f);
    dl->AddPolyline(pts, 60, depth > 0 ? P.accent : P.textFaint, 0, 1.3f);
    ImGui::Dummy(ImVec2(w, h));
  }
  pui::endPanel();
  ImGui::SameLine(0, 8);
  pui::beginPanel("DIFFUSION", wDif, rowH);
  knobRow(a, t, {{DlyDiffusion, "Level"}, {DlyDiffSpread, "Spread"}}, wDif - 20, pui::KnobMedium);
  pui::endPanel();
  ImGui::SameLine(0, 8);
  pui::beginPanel("DISTORTION", wDist, rowH);
  static const char* dmodes[3] = {"Off", "Limit", "Sat"};
  ImGui::BeginGroup();
  pui::segmented(a, t, DlyDistType, 52, dmodes, 1);
  ImGui::EndGroup();
  ImGui::SameLine(0, 6);
  float curveW = 92;
  knobRow(a, t, {{DlyDistThresh, "Thresh"}, {DlyDistHardness, "Hard"}, {DlyDistSymmetry, "Sym"}}, wDist - 20 - 58 - curveW - 8);
  ImGui::SameLine(0, 4);
  {  // transfer curve of the loop distortion
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float sz = curveW;
    pui::displayFrame(dl, p, ImVec2(p.x + sz, p.y + sz));
    dl->AddLine(ImVec2(p.x + sz / 2, p.y + 4), ImVec2(p.x + sz / 2, p.y + sz - 4), P.gridStep);
    dl->AddLine(ImVec2(p.x + 4, p.y + sz / 2), ImVec2(p.x + sz - 4, p.y + sz / 2), P.gridStep);
    int type = ival(st, DlyDistType);
    double T = dsp::dbToGain(val(st, DlyDistThresh)), hard = val(st, DlyDistHardness), sym = val(st, DlyDistSymmetry);
    ImVec2 pts[81];
    for (int i = 0; i <= 80; ++i) {
      double x = -1.2 + 2.4 * i / 80.0, y = x;
      if (type == 1) {
        y = std::clamp(x, -T, T);
      } else if (type == 2) {
        double top = T * (x >= 0 ? 1 + sym : 1 - sym), ax = std::fabs(x), lin = hard * top, k = (1 - hard) * top;
        double yy = ax <= lin ? ax : (k > 1e-9 && ax < lin + 2 * k ? ax - (ax - lin) * (ax - lin) / (4 * k) : top);
        y = x < 0 ? -yy : yy;
      }
      pts[i] = ImVec2(p.x + 4 + (sz - 8) * static_cast<float>((x + 1.2) / 2.4), p.y + sz / 2 - static_cast<float>(std::clamp(y, -1.2, 1.2) / 1.2) * (sz / 2 - 4));
    }
    dl->AddPolyline(pts, 81, type ? P.paper : P.textFaint, 0, 1.6f);
    ImGui::Dummy(ImVec2(sz, sz));
  }
  pui::endPanel();
  ImGui::EndGroup();
  ImGui::SameLine(0, 8);
  // Output column.
  pui::beginPanel("OUT", 92, rowH * 2 + 8);
  pui::knob(a, t, DlyWet, "Wet", pui::KnobMedium, 72);
  pui::knob(a, t, DlyTone, "Tone", pui::KnobMedium, 72);
  pui::knob(a, t, DlyDry, "Dry", pui::KnobMedium, 72);
  pui::knob(a, t, DlyInputPan, "In pan", pui::KnobSmall, 72);
  pui::endPanel();
}

}  // namespace

void editorReverbRef(App& a, const PluginTarget& t, float width);  // PluginEditorsTimeRef.cpp
void editorDelayRef(App& a, const PluginTarget& t, float width);   // PluginEditorsDelayRef.cpp

bool drawFxEditor(App& a, const PluginTarget& t, float width) {
  const PluginState* st = t.state(a.doc.song());
  if (!st) return false;
  const std::string& ty = st->type;
  if (ty == "gl.delay") {
    if (bval(*t.state(a.doc.song()), DlyModel)) editorDelayRef(a, t, width);
    else editorDelay(a, t, width);
  }
  else if (ty == "gl.reverb") editorReverbRef(a, t, width);
  else return false;
  return true;
}

}  // namespace gl
