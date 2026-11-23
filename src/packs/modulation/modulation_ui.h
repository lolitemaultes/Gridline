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

// The knob context menu's Modulate submenu (0.7.8: every source in one place: the LFO board's LFO,
// Chaos and Random, macros, controller plugins; the settings of what moves it now). Call inside
// an open popup; false when the parameter takes no modulation (nothing drawn).
bool drawLinkMenu(App& a, ParamId pid);
// Modulate's "Remove all modulation": every source's link to `pid`, one undo step.
void removeAllModulation(App& a, ParamId pid);
// The "<source> settings" submenu of the one macro or controller link `link` driving `pid`.
void drawLinkSettings(App& a, ParamId pid, const ModLink& link);
// Draws the live (modulated) value of a linked knob as a marker on its arc.
// `center`/`radius` describe the knob; does nothing for unlinked parameters.
void drawModulationMarker(App& a, ParamId pid, ImVec2 center, float radius);

}  // namespace gl::packs::modulation
