// Gridline — "synths" pack: editors (Analog in analog_ui.cpp, Wavetable in
// wavetable_ui.cpp).
#include "../PacksUi.h"
#include "../../app/App.h"

namespace gl::packs::synths {

void drawAnalogEditor(App& a, const PluginTarget& t, float width);     // analog_ui.cpp
void drawWavetableEditor(App& a, const PluginTarget& t, float width);  // wavetable_ui.cpp
void drawMatrixFmEditor(App& a, const PluginTarget& t, float width);   // matrixfm_ui.cpp
void drawGmsEditor(App& a, const PluginTarget& t, float width);        // gms_ui.cpp
bool drawVaEditors(App& a, const PluginTarget& t, float width);        // va_ui.cpp (J-22)
bool drawDcoEditors(App& a, const PluginTarget& t, float width);       // dco_ui.cpp (J-22)

bool drawEditor(App& a, const PluginTarget& t, float width) {
  const PluginInfo* info = t.info(a.doc.song());
  if (!info) return false;
  if (drawVaEditors(a, t, width) || drawDcoEditors(a, t, width)) return true;
  if (info->type == "gl.analog") {
    drawAnalogEditor(a, t, width);
    return true;
  }
  if (info->type == "gl.wavetable") {
    drawWavetableEditor(a, t, width);
    return true;
  }
  if (info->type == "gl.gms") {
    drawGmsEditor(a, t, width);
    return true;
  }
  if (info->type == "gl.matrixfm") {
    drawMatrixFmEditor(a, t, width);
    return true;
  }
  return false;
}

}  // namespace gl::packs::synths
