// Gridline — plugin packs: editor entry points (see Packs.h).
#pragma once
#include "../app/PluginUI.h"

namespace gl::packs {

// Draws the custom editor for a pack type; false when no pack owns it.
bool drawEditor(App& a, const PluginTarget& t, float width);
// Per frame, after the plugin windows: windows a pack keeps open on its own
// (e.g. the audio editor's standalone window) and background work (recording).
void drawWindows(App& a);
// Opens the audio editor (gl.edison) on a channel's sample in its own window.
bool openAudioEditor(App& a, const std::string& path, ObjectId channel);
// Record into the audio editor (J-8 record filter): arms the gl.edison slot of
// a mixer track to record while the transport plays (one is inserted into the
// first free slot when the track has none). Returns the slot, -1 = no room.
int armAudioEditorRecording(App& a, ObjectId track);

}  // namespace gl::packs

#define GL_DECLARE_PACK_UI(P)                                       \
  namespace gl::packs::P {                                          \
  bool drawEditor(App& a, const PluginTarget& t, float width);      \
  }
