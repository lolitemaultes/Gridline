// Gridline — "dynamics" pack, J-42: Pocket Rack editor (gl.pocketrack).
// One strip per slot: module picker (loads the module's defaults), its
// controls as knobs, then post gain / mix / wet balance.
#include <algorithm>
#include <string>
#include "../../app/App.h"
#include "../../app/PluginUI.h"
#include "../PacksUi.h"
#include "dynamics.h"

namespace gl::packs::dynamics {

void editorPocket(App& a, const PluginTarget& t, float width) {
  const PluginState* st = t.state(a.doc.song());
  if (!st) return;
  const PluginState s = *st;
  auto val = [&](int k) { return k < static_cast<int>(s.params.size()) ? s.params[static_cast<size_t>(k)] : 0.f; };
  const float cell = 64.f;
  for (int slot = 0; slot < kPkSlots; ++slot) {
    ImGui::PushID(slot);
    const int type = std::clamp(static_cast<int>(std::lround(val(pkParam(slot, PkType)))), 0, kPkTypes - 1);
    std::string title = "Slot " + std::to_string(slot + 1) + " \xE2\x80\x94 " + pocketModuleName(type);
    pui::beginSection(title.c_str(), width - 16);
    // Module picker: switching loads the new module's defaults.
    ImGui::SetNextItemWidth(180);
    if (ImGui::BeginCombo("##type", pocketModuleName(type))) {
      for (int ty = 0; ty < kPkTypes; ++ty) {
        const char* n = pocketModuleName(ty);
        if (!*n) continue;
        if (ImGui::Selectable(n, ty == type) && ty != type) {
          PluginState ns = s;
          float d[kPkParams];
          pocketModuleDefaults(ty, d);
          ns.params[static_cast<size_t>(pkParam(slot, PkType))] = static_cast<float>(ty);
          for (int i = 0; i < kPkParams; ++i) ns.params[static_cast<size_t>(pkParam(slot, PkP0 + i))] = d[i];
          setPluginState(a, t, ns, "change module");
        }
      }
      ImGui::EndCombo();
    }
    if (type != 0) {
      ImGui::SameLine();
      pui::toggleButton(a, t, pkParam(slot, PkP0), "On");
      const float avail = width - 40;
      const int perRow = std::max(1, static_cast<int>(avail / cell));
      int col = 0;
      auto knob = [&](int k, const char* label) {
        if (col > 0 && col < perRow) ImGui::SameLine();
        if (col >= perRow) col = 0;
        pui::knobCell(a, t, k, label, cell, 15);
        ++col;
      };
      for (int i = 1; i < kPkParams; ++i)
        if (const char* n = pocketParamName(type, i)) knob(pkParam(slot, PkP0 + i), n);
      col = perRow;  // the wrapper controls start a new row
      knob(pkParam(slot, PkPost), "Out");
      knob(pkParam(slot, PkMix), "Dry/wet");
      knob(pkParam(slot, PkPan), "Wet bal.");
    }
    pui::endSection();
    ImGui::PopID();
  }
}

}  // namespace gl::packs::dynamics
