// Gridline: J-142, clip parameter fields that edit a whole selection relatively
// (core/ClipMultiEdit.h has the rules per parameter).
#pragma once
#include <cstdint>
#include "../core/ClipMultiEdit.h"

namespace gl {
class App;
}

namespace gl::clipui {

// A drag field (ImGui::DragFloat with these arguments) for parameter `p` of
// clip `id`, shown in UI units (`uiValue` = the clip's value, one parameter
// unit = `uiPerUnit` UI units: ms per second, ...). Only when the clip belongs
// to a multi-selection: it draws the field and every selected clip moves by
// the amount the field moves from where the drag began, each clamped at its
// own limits (one undo step per drag). A value typed into the field (Ctrl+click
// or double-click) is an offset too: the clicked clip gets the typed value and
// every other clip moves by the same difference. Returns false (nothing drawn)
// for a single clip: the caller draws its usual field.
bool multiDrag(App& a, ObjectId id, clipmulti::Param p, const char* label, float uiValue, float speed, float lo, float hi,
               const char* fmt, double uiPerUnit, const char* undoLabel, uint64_t mergeKey);

// The clips an edit of `id` reaches with the playlist's current selection.
std::vector<ObjectId> targets(const App& a, ObjectId id, clipmulti::Param p);

}  // namespace gl::clipui
