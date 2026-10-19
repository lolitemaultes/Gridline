// Gridline — "models" pack, J-22 group "strings": editor dispatch (Twin
// String in twinstring_ui.cpp, Plucker in plucker_ui.cpp, Step Bass in
// stepbass_ui.cpp).
#include "../../app/App.h"
#include "../PacksUi.h"

namespace gl::packs::models {

void drawTwinString(App& a, const PluginTarget& t, float width);  // twinstring_ui.cpp
void drawPlucker(App& a, const PluginTarget& t, float width);     // plucker_ui.cpp
void drawStepBass(App& a, const PluginTarget& t, float width);    // stepbass_ui.cpp

bool drawStringsEditors(App& a, const PluginTarget& t, float width) {
  const PluginState* st = t.state(a.doc.song());
  if (!st) return false;
  if (st->type == "gl.twinstring")
    drawTwinString(a, t, width);
  else if (st->type == "gl.plucker")
    drawPlucker(a, t, width);
  else if (st->type == "gl.stepbass")
    drawStepBass(a, t, width);
  else
    return false;
  return true;
}

}  // namespace gl::packs::models
