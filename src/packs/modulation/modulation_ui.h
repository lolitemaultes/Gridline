// Gridline — "modulation" pack: editor entry points used by the application
// (channel settings window, knob context menu) besides drawEditor().
#pragma once
#include "../../app/PluginUI.h"

namespace gl::packs::modulation {

// Channel settings: the channel's note processor chain (add, reorder, remove)
// with each processor's editor. `width` = content width.
void drawChannelNoteFx(App& a, ObjectId channel, float width = 600.0f);
// One note processor's editor (channel, index in Channel::noteFx).
void drawNoteFxEditor(App& a, ObjectId channel, int index, float width);

// Knob context menu entries: "Link to controller" (lists every controller
// output) and, when linked, the link's mapping settings. Call inside an open popup.
void drawLinkMenu(App& a, ParamId pid);
// Draws the live (modulated) value of a linked knob as a marker on its arc.
// `center`/`radius` describe the knob; does nothing for unlinked parameters.
void drawModulationMarker(App& a, ParamId pid, ImVec2 center, float radius);

}  // namespace gl::packs::modulation
