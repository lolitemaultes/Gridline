// Gridline — "models" pack: editor dispatch.
#include "../../app/App.h"
#include "../PacksUi.h"

namespace gl::packs::models {

void drawAdditive(App& a, const PluginTarget& t, float width);
void drawHarmor(App& a, const PluginTarget& t, float width);
void drawImageSynth(App& a, const PluginTarget& t, float width);
void drawStringModel(App& a, const PluginTarget& t, float width);
void drawDrumModel(App& a, const PluginTarget& t, float width);

bool drawEditor(App& a, const PluginTarget& t, float width) {
  const PluginState* st = t.state(a.doc.song());
  if (!st) return false;
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
