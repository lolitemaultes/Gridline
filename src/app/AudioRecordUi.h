// Gridline — audio input & recording UI (J-8): mixer input selector / monitor /
// arm, the record filter, Settings → Audio input, the per-frame recording
// session, and the audio clip workflow (clip menu + properties window).
#pragma once
#include <string>
#include "../../third_party/imgui/imgui.h"
#include "App.h"

namespace gl {

std::string recordedDir(const App& a);  // "<project>/Recorded" (unsaved: config folder)
std::string editedDir(const App& a);    // "<project>/Edited audio": the audio editor's saved edits
std::string seltmannLoopsDir(const App& a);  // "<project>/Seltmann loops": loops rendered by Seltmann (J-101)
std::string renderedDir(const App& a);  // "<project>/Rendered"
// "In 1", "In 1 - In 2", "(none)".
std::string inputName(int first, bool stereo);
// K-357: shared input choice list for all layouts' track input menus (adds JACK peer names when active).
struct InputChoice {
  int first = -1;
  bool stereo = true;
  std::string label;     // short label ("In 1 - In 2", "In 1")
  std::string peers;     // connected external JACK ports, empty if none or non-JACK
  bool jackPort = false;
};
std::vector<InputChoice> inputChoices(const App& a, bool padToStereo = false);
std::string inputChoiceMenuLabel(const InputChoice& c);

// Record button menu: what to record (audio into the playlist / the audio
// editor, notes, automation), punch, file format.
void audioRecordFilterMenu(App& a);
void audioEditorRecordItem(App& a);  // K-314: the "Audio, into Loupe" switch (a menu row)
// K-123: the audio file format submenu.
void audioFormatMenu(App& a);
// K-123: a mixer track's input peaks this frame (linear; read once a frame, shared by every view).
void inputPeaks(App& a, ObjectId track, float& l, float& r);
// Mixer inspector: input selector, monitoring, arm, input meter.
void mixerInputPanel(App& a, const MixerTrack& t, float width);
// Mixer strip: the disk-record arm button. Returns true when clicked.
bool stripArmButton(App& a, const MixerTrack& t, ImVec2 pos);
// Arms / disarms a track; a track without an input gets the first input pair.
void toggleArm(App& a, ObjectId track);
// GitHub #89: once a frame. A track armed here (any arm button, menu, shortcut or script) while no input
// device is chosen gets the computer's default input device opened, so the armed track records. The
// choice is kept in Settings > Audio like any other; projects opened with tracks armed choose nothing.
void armInputCheck(App& a);
// K-123: record safe (the track cannot be armed); disarms it when turned on.
void setRecordSafe(App& a, ObjectId track, bool on);
// Playlist audio clip menu items and the clip properties window.
void audioClipMenu(App& a, ObjectId clip, bool withEdit = true);  // K-456: withEdit false: no "Edit clip..." row (the caller has it)
// Checkpoint 49: an audio clip's menu in the Clips layout, as the reference's: the edit block (not from a
// grid slot, which has its own), Rename, Colour, Deactivate, freeze, Consolidate, Split, Reverse, stems.
void clipsAudioClipMenu(App& a, ObjectId clip);
void audioClipTempoItems(App& a, ObjectId clip);  // K-158: fit to tempo, normalize (the clip menu's Audio submenu)
// Settings → Audio: input device, channels, recording offset and format.
void audioInputSettings(App& a, float fieldW);

}  // namespace gl
