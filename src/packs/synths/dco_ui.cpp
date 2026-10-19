// Gridline — "synths" pack, J-22 group "dco": editors (DCO Synth in
// dcosynth_ui.cpp, Warm Trio in warmsynth_ui.cpp, FM Duo in fmduo_ui.cpp).
#include "../../app/App.h"
#include "../PacksUi.h"

namespace gl::packs::synths {

void drawDcoSynthEditor(App& a, const PluginTarget& t, float width);   // dcosynth_ui.cpp
void drawWarmSynthEditor(App& a, const PluginTarget& t, float width);  // warmsynth_ui.cpp
void drawFmDuoEditor(App& a, const PluginTarget& t, float width);      // fmduo_ui.cpp

bool drawDcoEditors(App& a, const PluginTarget& t, float width) {
  const PluginState* st = t.state(a.doc.song());
  if (!st) return false;
  if (st->type == "gl.dcosynth") drawDcoSynthEditor(a, t, width);
  else if (st->type == "gl.warmsynth") drawWarmSynthEditor(a, t, width);
  else if (st->type == "gl.fmduo") drawFmDuoEditor(a, t, width);
  else return false;
  return true;
}

}  // namespace gl::packs::synths
