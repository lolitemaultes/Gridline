// Gridline - K-311: type-in values on every plugin control.
//
// The shared controls (pui knobs, sliders and dials, ui::Knob / ParamKnob / ParamFader, skin knobs,
// the pack widgets with their own menus) already offer "Type a value..." on right-click. Every
// other control of a built-in editor (XY pads, envelope handles, steppers, filter drags, bar lanes,
// switches, choice boxes, the Controls view's bars) gets it here, in one place: while an editor
// body draws on the frame the right button is let go, the parameters each control asks for
// (PluginTarget::pid) and the items it adds are noted in order; when the right-click landed on an
// item that opened no menu of its own and changed nothing, a small menu opens with the parameter(s)
// that item asked for: "Reset to default" and "Type a value..." (one per parameter for a control
// of several, such as an XY pad). The type-in box is the shared one (core/ParamText.h).
// drawPluginBody (app and the VST3 / CLAP / AU frames) calls begin / end around every editor.
#pragma once
#include <vector>
#include "../core/Params.h"
#include "../core/TypeInPick.h"

namespace gl {
class App;
namespace typein {

// Around an editor body. Off (every frame but a right release) costs one branch per pid().
void begin(App& a);
void end(App& a);
// A control asked for parameter `pid` (PluginTarget::pid, through audit::bindTouch).
void touch(const ParamId& pid);
// For a control's own right-click menu (an EQ band's node): "Type a value..." for each parameter
// (under its name when there are several);
// the shared type-in box opens over the editor when one is chosen.
void menuItems(App& a, const std::vector<ParamId>& params);

}  // namespace typein
}  // namespace gl
