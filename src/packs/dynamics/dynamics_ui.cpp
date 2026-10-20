// Gridline — "dynamics" pack: editors (dispatch; the editors live in
// masterfl_ui.cpp, dist_fl_ui.cpp, rack_ui.cpp and dist_ui.cpp).
#include "../PacksUi.h"
#include <string>
#include "../../app/App.h"

namespace gl::packs::dynamics {

void editorLoudnessFl(App& a, const PluginTarget& t, float width);  // dist_fl_ui.cpp (J-19)
void editorAmpRack(App& a, const PluginTarget& t, float width);
void editorEffector(App& a, const PluginTarget& t, float width);
void editorOverdriveFl(App& a, const PluginTarget& t, float width);  // dist_fl_ui.cpp (J-19)
void editorSqueezeFl(App& a, const PluginTarget& t, float width);  // dist_fl_ui.cpp (J-19)
void editorTransplit(App& a, const PluginTarget& t, float width);
void editorMasterFl(App& a, const PluginTarget& t, float width);   // masterfl_ui.cpp (κ2)
void editorEnhanceFl(App& a, const PluginTarget& t, float width);
void editorPocket(App& a, const PluginTarget& t, float width);  // pocket_ui.cpp (J-42)

bool drawEditor(App& a, const PluginTarget& t, float width) {
  const PluginInfo* info = t.info(a.doc.song());
  if (!info) return false;
  const std::string& ty = info->type;
  void (*fn)(App&, const PluginTarget&, float) = nullptr;
  if (ty == "gl.master") fn = editorMasterFl;
  else if (ty == "gl.emphasis") fn = editorLoudnessFl;
  else if (ty == "gl.enhance") fn = editorEnhanceFl;
  else if (ty == "gl.amprack") fn = editorAmpRack;
  else if (ty == "gl.effector") fn = editorEffector;
  else if (ty == "gl.overdrive") fn = editorOverdriveFl;
  else if (ty == "gl.squeeze") fn = editorSqueezeFl;
  else if (ty == "gl.transplit") fn = editorTransplit;
  else if (ty == "gl.pocketrack") fn = editorPocket;
  if (!fn) return false;
  fn(a, t, width);
  return true;
}

}  // namespace gl::packs::dynamics
