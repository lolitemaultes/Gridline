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
std::string renderedDir(const App& a);  // "<project>/Rendered"
// "In 1", "In 1 - In 2", "(none)".
std::string inputName(int first, bool stereo);

// Record button menu: what to record (audio into the playlist / the audio
// editor, notes, automation), punch, file format.
void audioRecordFilterMenu(App& a);
// Mixer inspector: input selector, monitoring, arm, input meter.
void mixerInputPanel(App& a, const MixerTrack& t, float width);
// Mixer strip: the disk-record arm button. Returns true when clicked.
bool stripArmButton(App& a, const MixerTrack& t, ImVec2 pos);
// Arms / disarms a track; a track without an input gets the first input pair.
void toggleArm(App& a, ObjectId track);
// Playlist audio clip menu items and the clip properties window.
void audioClipMenu(App& a, ObjectId clip);
void drawClipProperties(App& a);
// Settings → Audio: input device, channels, recording offset and format.
void audioInputSettings(App& a, float fieldW);

}  // namespace gl
