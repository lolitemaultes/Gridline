// Gridline — hidden, narrow and folded mixer strips in the mixer views (GitHub #58): the track
// menu's items, a folded group's menu, the "N hidden" list behind each view's toolbar button,
// and the slim strip the Studio mixer draws for a narrow track or a folded group. The rules and
// edits live in core/MixerHide.h.
#pragma once
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../core/MixerHide.h"

namespace gl {
class App;

namespace mixhideui {

// Width of a slim strip (narrow track or folded group) in the Studio mixer.
inline constexpr float kSlimW = 30.0f;

// Hide, Narrow (checked while on) for the selected tracks, `id` among them. Not for the master.
void trackItems(App& a, ObjectId id, const std::vector<ObjectId>& selIds);
// Collapse group / Expand group for the track's group (inside the track menu's Group submenu).
void groupItem(App& a, const std::string& group);
// A folded group's menu (its strip's right-click): expand, mute, solo, the members to show one by one.
void groupMenu(App& a, const std::string& group);
// The toolbar button's text ("3 hidden"), "" when nothing is hidden.
std::string hiddenLabel(const Song& s);
// The list behind it: each hidden track and folded group (a click shows it), then Show all.
void hiddenMenu(App& a);
// Text reading upwards from `o` (a slim strip's name), at most `maxLen` long (ellipsised).
void upwardText(ImDrawList* dl, ImFont* f, float fs, ImVec2 o, ImU32 col, const std::string& text, float maxLen);
// Dev: the columns a mixer view other than Studio's drew this frame (mixer index, x range), printed
// by "mixhide state" as "  cols i:width ...".
void beginColumns();
// Dev: "mixhide smoothview N" asks Smooth's mixer for view N (0 single, 1 tracks, 2 all); -1 when not asked.
int devSmoothView();
void noteColumn(size_t index, float x0, float x1);
// Dev input "mixhide ...": hide|show|narrow|wide <index...>, group <name> <index...>, fold|unfold <name>,
// showall, state (prints "mixhide frame N: hidden C undo U" and one line per track).
void devCommand(App& a, const std::vector<std::string>& args, int frame);

}  // namespace mixhideui
}  // namespace gl
