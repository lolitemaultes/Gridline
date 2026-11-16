// Gridline — delay / reverb editor dispatch: the reference-model editors
// (PluginEditors{Delay,Time}Ref.cpp). K-261: Tracer has one view for every engine.
#include "PluginEditorsCommon.h"

namespace gl {

void editorReverbRef(App& a, const PluginTarget& t, float width);  // PluginEditorsTimeRef.cpp
void editorDelayRef(App& a, const PluginTarget& t, float width);   // PluginEditorsDelayRef.cpp

bool drawFxEditor(App& a, const PluginTarget& t, float width) {
  const PluginState* st = t.state(a.doc.song());
  if (!st) return false;
  const std::string& ty = st->type;
  if (ty == "gl.delay") {
    editorDelayRef(a, t, width);  // K-261: one view for every engine
  }
  else if (ty == "gl.reverb") editorReverbRef(a, t, width);
  else return false;
  return true;
}

}  // namespace gl
