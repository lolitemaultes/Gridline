// Gridline - K-245: macro knobs in the plugin windows (core/Macros.h).
//
// - linkMenu: the "Link to macro" submenu on every parameter's right-click menu (built-in knobs,
//   installed plugins' parameter rows, Tools > Last moved control for a plugin's own interface).
// - drawPanel: the eight macro knobs of a channel (its window's Misc page) or of a mixer track (its
//   effect windows): rename, Map (learn from the next moved control or pick from a list), and the
//   selected macro's targets with their range, curve and a remove button.
#pragma once
#include "../core/Params.h"

namespace gl {
class App;
}

namespace gl::macroui {

// The "Link to macro" submenu for `pid` (nothing for song parameters or a macro of its own owner).
void linkMenu(App& a, ParamId pid);
// 0.7.8: its parts, for the knob menu's Modulate > Macro: whether it has anything for `pid`, its body,
// and whether a macro drives `pid` now.
bool linkMenuAvailable(App& a, ParamId pid);
void linkMenuItems(App& a, ParamId pid);
bool linkedToMacro(App& a, ParamId pid);
// The macro panel of `owner` (a channel or a mixer track), `width` wide.
void drawPanel(App& a, ObjectId owner, float width);
// The effect window's macro strip for the slot's mixer track: one quiet line that opens the panel
// (open from the start while the track has an assigned macro).
void drawEffectStrip(App& a, ObjectId mixerTrack, float width);
// Once a frame (main loop): a macro learning a target takes the next control moved anywhere, also in
// a plugin's own interface while no macro panel is drawn.
void frame(App& a);

// Dev / test hooks: the last panel drawn (owner, knob centres, the selected macro).
struct DevState {
  ObjectId owner = 0;
  int frame = -1;
  int selected = 0;
  int learning = -1;
  float knobX[8] = {}, knobY[8] = {};
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};
const DevState& devState();
// Starts or stops learning for macro k of `owner` (-1: stop), as the Map menu's Learn does.
void devLearn(App& a, ObjectId owner, int k);

}  // namespace gl::macroui
