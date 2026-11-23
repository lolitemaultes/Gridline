// Gridline: "modulation" pack: editor dispatch, the knob menu's Modulate
// submenu and the live modulation marker. The editors live in
// modulation_ctl_ui.cpp (controllers), modulation_route_ui.cpp (surface,
// routing, sidechain) and modulation_patcher_ui.cpp (patch host).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "../../core/I18n.h"
#include "../../core/PluginParams.h"
#include "../PacksUi.h"
#include "../../app/MacroUi.h"
#include "../../app/LfoUi.h"
#include "modulation.h"
#include "modulation_dsp.h"
#include "modulation_editors.h"
#include "modulation_ui.h"
#include "modulation_widgets.h"

namespace gl::packs::modulation {

using theme::pal;

namespace {
// The controller outputs submenu body (Modulate > Controller plugin).
void controllerItems(App& a, ParamId pid, const ModLink* cur) {
  const Song& s = a.doc.song();
  for (const auto& o : listControllerOutputs(s)) {
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
}
}  // namespace

// 0.7.8: every way to move a knob from another source, in one Modulate submenu, in one order:
//   [what it modulates]
//   LFO >  Chaos >  Random >      the LFO board's sources
//   ---
//   Macro >  Controller plugin >     the channel's or track's macros, a controller plugin's output
//   ---
//   <source> settings (amount) >     one per source moving it now
//   Remove all modulation
//   ---
//   Open the LFO board        Alt+F9
// A source kind with nothing for this control is left out (a macro knob's own set, a song parameter).
void removeAllModulation(App& a, ParamId pid) {
  a.doc.beginGroup("remove all modulation");
  lfoui::removeLinks(a, pid);
  if (findModLink(a.doc.song(), pid.owner, pid.index)) a.doc.removeModLink(pid);
  a.doc.endGroup(false);
}

bool drawLinkMenu(App& a, ParamId pid) {
  const Song& s = a.doc.song();
  if (pid.owner == kNoId || pid.owner == kSongId) return false;
  const bool board = lfoui::canModulate(a, pid);
  const bool macro = macroui::linkMenuAvailable(a, pid);
  if (!board && !macro && !describeParam(s, pid)) return false;
  const ModLink* cur = findModLink(s, pid.owner, pid.index);  // the one macro or controller link
  const bool anyBoard = !lfoLinksOn(s, pid.owner, pid.index).empty();
  if (!ImGui::BeginMenu(i18n::tr("Modulate"))) return true;
  {
    // What it modulates, a quiet caption (fitted: a long parameter name never widens the menu).
    const ImVec4 dim = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
    std::string head = paramDisplayName(s, pid);
    if (size_t at = head.rfind(" \xE2\x80\xBA "); at != std::string::npos) head = head.substr(at + 5);  // the control's own name
    ImGui::TextColored(dim, "%s", theme::fitText(head.c_str(), 240));
    ImGui::Separator();
  }
  if (board) {
    for (LfoKind k : {LfoKind::Lfo, LfoKind::Chaos, LfoKind::Random}) {
      ImGui::PushID(static_cast<int>(k));
      // A source of this kind moving it: the submenu's name says so ("LFO" with a dot after it).
      const std::string lb = std::string(i18n::tr(lfoui::sourceMenuName(k))) + (lfoui::movedBy(a, pid, k) ? "  \xE2\x80\xA2" : "");
      if (ImGui::BeginMenu((lb + "###src").c_str())) {
        lfoui::sourceMenu(a, pid, k);
        ImGui::EndMenu();
      }
      ImGui::PopID();
    }
    ImGui::Separator();
  }
  if (macro) {
    const std::string lb = std::string(i18n::tr("Macro")) + (macroui::linkedToMacro(a, pid) ? "  \xE2\x80\xA2" : "");
    if (ImGui::BeginMenu((lb + "###macro").c_str())) {
      macroui::linkMenuItems(a, pid);
      ImGui::EndMenu();
    }
  }
  {
    const bool any = !listControllerOutputs(s).empty();
    const bool ctl = cur && cur->sourceSlot != kLayerMacroSlot;
    const std::string lb = std::string(i18n::tr("Controller plugin")) + (ctl ? "  \xE2\x80\xA2" : "");
    if (ImGui::BeginMenu((lb + "###ctl").c_str(), any)) {
      controllerItems(a, pid, cur);
      ImGui::EndMenu();
    }
    if (!any && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ui::Hint("Add a controller plugin first (Peak Follower, X-Y Pad, Formula Mod, Envelope Mod or Keyboard Mod)");
  }
  // What moves it now: each source's settings, and a way to clear them all in one step.
  if (anyBoard || cur) {
    ImGui::Separator();
    if (anyBoard) lfoui::linkedSettingsItems(a, pid);
    if (cur) drawLinkSettings(a, pid, *cur);
    if (ImGui::MenuItem(i18n::tr("Remove all modulation"))) removeAllModulation(a, pid);
  }
  if (board) {
    ImGui::Separator();
    lfoui::openBoardItem(a);
  }
  ImGui::EndMenu();
  return true;
}

void drawLinkSettings(App& a, ParamId pid, const ModLink& link) {
  const Song& s = a.doc.song();
  const ModLink* cur = &link;
  std::string lb = controllerOutputName(s, cur->sourceOwner, cur->sourceSlot, cur->sourceOutput);
  lb = std::string(theme::fitText(lb.c_str(), 200)) + " " + i18n::tr("settings") + "###ctlset";
  if (ImGui::BeginMenu(lb.c_str())) {
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
    if (ImGui::MenuItem(cur->sourceSlot == kLayerMacroSlot ? i18n::tr("Unlink from macro") : i18n::tr("Remove controller link"))) a.doc.removeModLink(pid);
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
  else if (ty == "gl.cvin") cvInEditor(e);
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
