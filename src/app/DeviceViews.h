// Gridline - compact device views (K-112c): the picture each major built-in device shows beside its
// main knobs in a device row (the Live layout's device band): the EQ's curve, a filter's response,
// the dynamics' transfer curves, a distortion's shape, a bitcrusher's steps, a reverb's decay, a
// delay's echoes, a modulation sweep and an instrument's envelope.
//
// The pictures come from the same models and laws the editors and engines use (engine/EqDesign.h,
// engine/GraphModels.h, and the transfer functions and previews the editors draw, exported below),
// so a compact view and the full editor always agree. Drawn with theme roles only.
#pragma once
#include <string>
#include <vector>
#include "../../third_party/imgui/imgui.h"
#include "../core/Types.h"

namespace gl {
class App;
struct PluginTarget;
struct PluginState;

namespace devview {

// Transfer curves and previews the editors draw (defined beside their editors).
double distTransfer(const PluginState& st, double x);     // gl.dist, linear in -> out (PluginEditorsFl.cpp)
double clipperTransfer(const PluginState& st, double x);  // gl.clipper, linear in -> out (PluginEditorsDynFl.cpp)
void crushView(App& a, const PluginTarget& t, ImVec2 p0, ImVec2 p1);  // gl.bitcrush preview, no captions (PluginEditorsFl.cpp)
// The band the EQ's editor has selected (-1 none), shared with the compact view (PluginEditorsEqRef.cpp).
int eqSelectedBand(const PluginTarget& t);
void eqSelectBand(const PluginTarget& t, int band);

// The width (px at 100 %) of the device's compact view, 0 when it has none (it shows knobs only).
float width(App& a, const PluginTarget& t);
// Draws the compact view in [p0, p1]. Hovering it names what it shows. The picture can be dragged:
// the EQ's dots (frequency and gain; with Alt, the band's width), the filter's cutoff and resonance,
// a dynamics device's threshold, the distortion's drive, the delay's time and feedback, the reverb's
// decay; each drag is one undo step. Returns true when it was double-clicked where no handle took
// it (the caller opens the device's whole editor).
bool draw(App& a, const PluginTarget& t, ImVec2 p0, ImVec2 p1);

// The device's main controls for a device row, in order: the ones a person reaches for first (the
// EQ's selected band, a synth's filter, a delay's time and feedback in the model in use). Empty:
// none chosen, the caller takes the first parameters. `label` empty: the parameter's own name.
struct Main {
  int param = 0;
  std::string label;
};
std::vector<Main> mainParams(App& a, const PluginTarget& t);

}  // namespace devview
}  // namespace gl
