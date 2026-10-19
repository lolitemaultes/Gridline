// Gridline — "models" pack, J-22 group "metal": editor dispatch (Alloy,
// Spectra Morph, Grain Scanner, Spline Scratcher).
#include "../../app/App.h"
#include "../PacksUi.h"

namespace gl::packs::models {

namespace metal {
void drawAlloy(App& a, const PluginTarget& t, float width);          // alloy_ui.cpp
void drawSpectraMorph(App& a, const PluginTarget& t, float width);   // spectramorph_ui.cpp
void drawGrainScan(App& a, const PluginTarget& t, float width);      // grainscan_ui.cpp
void drawSplineScratch(App& a, const PluginTarget& t, float width);  // splinescratch_ui.cpp
}  // namespace metal

bool drawMetalEditors(App& a, const PluginTarget& t, float width) {
  const PluginState* st = t.state(a.doc.song());
  if (!st) return false;
  if (st->type == "gl.alloy") metal::drawAlloy(a, t, width);
  else if (st->type == "gl.spectramorph") metal::drawSpectraMorph(a, t, width);
  else if (st->type == "gl.grainscan") metal::drawGrainScan(a, t, width);
  else if (st->type == "gl.splinescratch") metal::drawSplineScratch(a, t, width);
  else return false;
  return true;
}

}  // namespace gl::packs::models
