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
// J-150: true when a pack takes this device's MIDI for itself (a DJ deck bound to
// the controller, or learning a control): the app's links and templates skip it.
bool claimMidi(App& a, const std::string& device);
// J-175: true when a pack records while the record button is armed and the transport rolls
// (a deck setup armed with a track on a deck: its performance passes). Song mode without a
// time selection then plays on past the song end, as for audio recording.
bool recordsWithTransport(App& a);

}  // namespace gl::packs

#define GL_DECLARE_PACK_UI(P)                                       \
  namespace gl::packs::P {                                          \
  bool drawEditor(App& a, const PluginTarget& t, float width);      \
  }
