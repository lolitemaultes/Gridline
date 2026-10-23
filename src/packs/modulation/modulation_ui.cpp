// Gridline — "modulation" pack: editor dispatch, the knob "link to
// controller" menu and the live modulation marker. The editors live in
// modulation_ctl_ui.cpp (controllers), modulation_route_ui.cpp (surface,
// routing, sidechain) and modulation_patcher_ui.cpp (patch host).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "../../core/I18n.h"
#include "../../core/PluginParams.h"
#include "../PacksUi.h"
#include "modulation.h"
#include "modulation_dsp.h"
#include "modulation_editors.h"
#include "modulation_ui.h"
#include "modulation_widgets.h"

namespace gl::packs::modulation {

using theme::pal;

void drawLinkMenu(App& a, ParamId pid) {
  const Song& s = a.doc.song();
  const ModLink* cur = findModLink(s, pid.owner, pid.index);
  if (ImGui::BeginMenu(i18n::tr("Link to controller"))) {
    auto outs = listControllerOutputs(s);
    if (outs.empty()) {
      ImGui::TextDisabled("%s", i18n::tr("No controllers in this project."));
      ImGui::TextDisabled("%s", i18n::tr("Add Peak Follower, X-Y Pad, Formula Mod,"));
      ImGui::TextDisabled("%s", i18n::tr("Envelope Mod or Keyboard Mod first."));
    }
    for (const auto& o : outs) {
      bool sel = cur && cur->sourceOwner == o.owner && cur->sourceSlot == o.slot && cur->sourceOutput == o.output;
      if (ImGui::MenuItem(o.label.c_str(), nullptr, sel)) {
        ModLink l = cur ? *cur : ModLink{};
        l.sourceOwner = o.owner;
        l.sourceSlot = o.slot;
        l.sourceOutput = o.output;
        l.owner = pid.owner;
        l.index = pid.index;
        if (!a.doc.setModLink(l)) a.notify("Cannot link this parameter", true);
      }
    }
    ImGui::EndMenu();
  }
  if (!cur) return;
  if (ImGui::BeginMenu(i18n::tr("Controller link settings"))) {
    ModLink l = *cur;
    ImGui::TextDisabled("%s", controllerOutputName(s, l.sourceOwner, l.sourceSlot, l.sourceOutput).c_str());
    uint64_t key = pid.packed() ^ 0x4C494E4B00000000ull;
    bool en = l.enabled;
    if (ImGui::Checkbox(i18n::tr("Enabled"), &en)) a.doc.editModLink(pid, "toggle controller link", [&](ModLink& x) { x.enabled = en; });
    int curve = static_cast<int>(l.curve);
    ImGui::SetNextItemWidth(200);
    if (ImGui::BeginCombo("Mapping", modCurveName(l.curve))) {
      for (int c = 0; c < static_cast<int>(ModCurve::Count); ++c)
        if (ImGui::Selectable(modCurveName(static_cast<ModCurve>(c)), c == curve))
          a.doc.editModLink(pid, "change link mapping", [&](ModLink& x) { x.curve = static_cast<ModCurve>(c); });
      ImGui::EndCombo();
    }
    float mn = l.min * 100, mx = l.max * 100;
    ImGui::SetNextItemWidth(200);
    if (ImGui::SliderFloat(i18n::tr("Range start"), &mn, 0, 100, "%.0f%%")) a.doc.editModLink(pid, "change link range", [&](ModLink& x) { x.min = mn / 100; }, key ^ 1);
    ImGui::SetNextItemWidth(200);
    if (ImGui::SliderFloat(i18n::tr("Range end"), &mx, 0, 100, "%.0f%%")) a.doc.editModLink(pid, "change link range", [&](ModLink& x) { x.max = mx / 100; }, key ^ 2);
    float sm = l.smoothMs;
    ImGui::SetNextItemWidth(200);
    if (ImGui::SliderFloat(i18n::tr("Smoothing"), &sm, 0, 2000, "%.0f ms", ImGuiSliderFlags_Logarithmic))
      a.doc.editModLink(pid, "change link smoothing", [&](ModLink& x) { x.smoothMs = sm; }, key ^ 3);
    if (ImGui::IsItemDeactivated()) a.doc.endGesture();
    if (l.curve == ModCurve::Formula) {
      static char buf[256];
      static uint64_t bufFor = 0;
      if (bufFor != pid.packed()) {
        std::snprintf(buf, sizeof buf, "%s", l.formula.empty() ? "input" : l.formula.c_str());
        bufFor = pid.packed();
      }
      ImGui::SetNextItemWidth(260);
      ImGui::InputText(i18n::tr("Formula"), buf, sizeof buf);
      Expr ex;
      std::string err;
      bool ok = ex.compile(buf, {"input", "base", "t", "beat"}, &err);
      if (!ok) ImGui::TextColored(theme::vec(pal().danger), "%s", err.c_str());
      else ImGui::TextDisabled("%s", i18n::tr("input = controller (0..1), base = knob value, t, beat"));
      if (ok && ImGui::Button(i18n::tr("Apply formula"))) {
        std::string f = buf;
        a.doc.editModLink(pid, "change link formula", [&](ModLink& x) { x.formula = f; });
      }
    }
    ImGui::Separator();
    if (ImGui::MenuItem(i18n::tr("Remove controller link"))) a.doc.removeModLink(pid);
    ImGui::EndMenu();
  }
}

void drawModulationMarker(App& a, ParamId pid, ImVec2 center, float radius) {
  if (!isModulated(a.doc.song(), pid)) return;
  auto d = describeParam(a.doc.song(), pid);
  double v = 0;
  if (!d || !a.builder.modulatedValue(pid, v)) return;
  constexpr float kPi = 3.14159265f;
  float a0 = kPi * 0.75f, a1 = kPi * 2.25f;
  float an = a0 + static_cast<float>(d->toNormalized(v)) * (a1 - a0);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddCircleFilled(ImVec2(center.x + std::cos(an) * (radius + 1), center.y + std::sin(an) * (radius + 1)), 3.0f, pal().accent, 12);
}

// ------------------------------------------------------------- dispatch

bool drawEditor(App& a, const PluginTarget& t, float width) {
  const PluginInfo* info = t.info(a.doc.song());
  if (!info) return false;
  const std::string& ty = info->type;
  EditTarget e = targetFor(a, t, width);
  ImGui::PushID("modpack");
  bool handled = true;
  if (ty == "gl.peakctl") peakEditor(e);
  else if (ty == "gl.xyctl") xyEditor(e);
  else if (ty == "gl.xyzctl") xyzEditor(e);
  else if (ty == "gl.formulactl") formulaEditor(e);
  else if (ty == "gl.envctl") envEditor(e);
  else if (ty == "gl.keyctl") keyEditor(e);
  else if (ty == "gl.cvctl") cvEditor(e);
  else if (ty == "gl.surface") surfaceEditor(e);
  else if (ty == "gl.send") sendEditor(e);
  else if (ty == "gl.bussend") busEditor(e, true);
  else if (ty == "gl.busreturn") busEditor(e, false);
  else if (ty == "gl.scduck") duckEditor(e);
  else if (ty == "gl.patcher" || ty == "gl.patcherfx") drawPatchHostEditor(e);
  else handled = false;
  ImGui::PopID();
  return handled;
}

}  // namespace gl::packs::modulation
