// Gridline — "synths" pack, J-22 group "va": editors (Twin VA in
// twinva_ui.cpp, Saw Swarm in sawswarm_ui.cpp, Finger Bass in
// fingerbass_ui.cpp).
#include "../../app/App.h"
#include "../PacksUi.h"
#include "va_ui.h"

namespace gl::packs::synths {

bool drawVaEditors(App& a, const PluginTarget& t, float width) {
  const PluginState* st = t.state(a.doc.song());
  if (!st) return false;
  if (st->type == "gl.twinva") {
    va::drawTwinVaEditor(a, t, width);
    return true;
  }
  if (st->type == "gl.sawswarm") {
    va::drawSawSwarmEditor(a, t, width);
    return true;
  }
  if (st->type == "gl.fingerbass") {
    va::drawFingerBassEditor(a, t, width);
    return true;
  }
  return false;
}

}  // namespace gl::packs::synths
