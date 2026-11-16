// Gridline — plugin packs: editor entry points (see Packs.h).
#pragma once
#include "../app/PluginUI.h"

namespace gl::packs {

// Draws the custom editor for a pack type; false when no pack owns it.
bool drawEditor(App& a, const PluginTarget& t, float width);
// Per frame, after the plugin windows: windows a pack keeps open on its own
// (e.g. the audio editor's standalone window) and background work (recording).
void drawWindows(App& a);
// K-15: true when a pack shows this effect's editor in a panel of the main layout (Loupe
// pinned): the effect window is not drawn (and App::fxEditor is cleared).
bool claimEffectEditor(App& a, const PluginTarget& t);
// K-112 (R3): true when Loupe is pinned into the layout and `t` is a Loupe effect: the panel's
// session becomes that slot's, so a host that draws the panel's body shows this effect.
bool showInLoupePanel(App& a, const PluginTarget& t);
// K-112 (R3): the Loupe panel's content in w x h (a layout part's frame hosts it).
void drawLoupeBody(App& a, float w, float h);
// Dev checks (--dev-input): commands a pack answers ("loupe ..."); true when handled.
bool devCommand(App& a, const std::string& cmd, const std::vector<std::string>& args);
// Opens the audio editor (gl.edison) on a channel's sample in its own window.
bool openAudioEditor(App& a, const std::string& path, ObjectId channel);
// The audio editor's windows of their own close (the panel pinned into the layout stays).
void closeAudioEditorWindows(App& a);
// K-112B (R04-04): closes the audio editor's own windows on channel samples (the Tracks layout:
// the lower pane shows one editor, so another editor taking it closes the audio editor).
void closeAudioEditorSampleWindows();
// Opens an audio file on its own in the audio editor (the browser's "Edit in Loupe"): the
// file itself is never changed; edits stay in the editor to drag out, send or save.
bool openFileInLoupe(App& a, const std::string& path);
// Record into the audio editor (J-8 record filter): arms the gl.edison slot of
// a mixer track to record while the transport plays (one is inserted into the
// first free slot when the track has none). Returns the slot, -1 = no room.
int armAudioEditorRecording(App& a, ObjectId track);
// J-184: loads a picture file into a Pixel Tone (gl.pixeltone) target: decoded
// on a worker thread, applied on the UI thread as one undo step ("load
// picture"); a file that is not a picture leaves the state as it was and says
// why. False when the target is not a Pixel Tone.
bool loadPicture(App& a, const PluginTarget& t, const std::string& path);
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
