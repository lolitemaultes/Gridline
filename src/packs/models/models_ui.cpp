// Gridline — "models" pack: editor dispatch.
#include "../../app/App.h"
#include "../PacksUi.h"

namespace gl::packs::models {

void drawAdditive(App& a, const PluginTarget& t, float width);
void drawHarmor(App& a, const PluginTarget& t, float width);
void drawImageSynth(App& a, const PluginTarget& t, float width);
void drawStringModel(App& a, const PluginTarget& t, float width);
void drawDrumModel(App& a, const PluginTarget& t, float width);
bool drawDrumsEditors(App& a, const PluginTarget& t, float width);    // drums_ui.cpp (J-22)
bool drawStringsEditors(App& a, const PluginTarget& t, float width);  // strings_ui.cpp (J-22)
bool drawMetalEditors(App& a, const PluginTarget& t, float width);    // metal_ui.cpp (J-22)

bool drawEditor(App& a, const PluginTarget& t, float width) {
  const PluginState* st = t.state(a.doc.song());
  if (!st) return false;
  if (drawDrumsEditors(a, t, width) || drawStringsEditors(a, t, width) || drawMetalEditors(a, t, width)) return true;
  if (st->type == "gl.additive")
    drawAdditive(a, t, width);
  else if (st->type == "gl.harmor")
    drawHarmor(a, t, width);
  else if (st->type == "gl.imagesynth")
    drawImageSynth(a, t, width);
  else if (st->type == "gl.string")
    drawStringModel(a, t, width);
  else if (st->type == "gl.drumkit")
    drawDrumModel(a, t, width);
  else
    return false;
  return true;
}

}  // namespace gl::packs::models
