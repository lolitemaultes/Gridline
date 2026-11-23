// Gridline: the Patch Player's editor (patchplayer_ui.cpp).
#pragma once
#include <string>
#include <vector>
#include "../../app/App.h"
#include "../../app/PluginUI.h"

namespace gl::packs::samplers {

void drawPatchPlayerEditor(App& a, const PluginTarget& t, float width);
// "000:048 Strings" for a channel's patch (empty without a file): the layouts show it.
std::string patchPlayerTitle(const PluginState& st);
// Dev input: "patchplayer tab|find|pick|family|place|sends|state ...".
bool patchPlayerDevCommand(App& a, const std::string& cmd, const std::vector<std::string>& args);

}  // namespace gl::packs::samplers
