// Gridline: J-142, clip parameter fields that edit a whole selection relatively (ClipMultiUi.h).
#include "ClipMultiUi.h"
#include "../../third_party/imgui/imgui.h"
#include "../core/I18n.h"
#include "App.h"

namespace gl::clipui {

namespace {
struct Active {
  ImGuiID item = 0;
  ObjectId clip = kNoId;
  clipmulti::Snapshot snap;
  float uiBase = 0, v = 0;
};
Active& active() {
  static Active act;
  return act;
}
}  // namespace

std::vector<ObjectId> targets(const App& a, ObjectId id, clipmulti::Param p) {
  return clipmulti::targets(a.doc.song(), a.playlist.selected, id, p);
}

bool multiDrag(App& a, ObjectId id, clipmulti::Param p, const char* label, float uiValue, float speed, float lo, float hi,
               const char* fmt, double uiPerUnit, const char* undoLabel, uint64_t mergeKey) {
  const std::vector<ObjectId> ids = targets(a, id, p);
  if (ids.size() < 2 || uiPerUnit == 0) return false;
  Active& act = active();
  const ImGuiID wid = ImGui::GetID(label);
  bool mine = act.item == wid && act.clip == id;
  float v = mine ? act.v : uiValue;
  float vlo = lo, vhi = hi;
  if (mine) {  // the field's travel: as far as at least one clip still moves
    vlo = act.uiBase + static_cast<float>(act.snap.minDelta() * uiPerUnit);
    vhi = act.uiBase + static_cast<float>(act.snap.maxDelta() * uiPerUnit);
  }
  const bool changed = ImGui::DragFloat(label, &v, speed, vlo, vhi, fmt);
  if (ImGui::IsItemActivated() || (changed && !mine)) {
    act.item = wid, act.clip = id, act.uiBase = uiValue, act.v = uiValue;
    act.snap = clipmulti::capture(a.doc.song(), ids, p, id);
    mine = true;
  }
  if (changed && mine) {
    act.v = v;
    clipmulti::offset(a.doc, undoLabel, act.snap, static_cast<double>(v - act.uiBase) / uiPerUnit, mergeKey);
  }
  if (ImGui::IsItemDeactivated() && act.item == wid) {
    act.item = 0, act.clip = kNoId;
    a.doc.endGesture();
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip(i18n::tr("%d selected clips: each moves by the same amount"), static_cast<int>(ids.size()));
  return true;
}

}  // namespace gl::clipui
