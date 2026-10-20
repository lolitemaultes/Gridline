// Gridline — export & interop UI (J-20): the render dialog (scope, formats,
// quality, splitting, destination) with its progress / summary window, MIDI
// import options, project data files / bones / zipped project, the missing
// samples window, and "export as WAV" for samples and audio clips.
#pragma once
#include <string>
#include "../core/Types.h"

namespace gl {

class App;

namespace exportui {

// What the render dialog opens with.
enum class Preset {
  Audio = 0,       // the song to files
  Stems,           // split mixer tracks
  PlaylistTracks,  // split playlist tracks
  Midi,            // MIDI only
  PatternFile,     // the current pattern to files
  AudioClip,       // the current pattern as an audio clip
  PatternReplace,  // the current pattern as audio clips replacing its pattern clips
  TrackGroups,     // J-31: split playlist track groups
};
void openRender(App& a, Preset p = Preset::Audio);
// Opens the render dialog with the settings of the last render (formats, scope, splitting).
void openRenderAgain(App& a);
// Starts the render with the dialog's settings into `folder` (dev checks / scripting).
void startRenderTo(App& a, const std::string& folder);
// Per frame (from drawDialogs): render dialog, progress, MIDI import, missing samples.
void draw(App& a);
// Body of a progress window (status line, bar, Cancel): the render progress
// window and assistant renders (J-61) share it. True when Cancel was pressed.
bool progressBody(const std::string& status, double progress, bool escapeCancels = true);

// MIDI.
void importMidiFile(App& a, const std::string& path);        // options dialog, then a new pattern + channels
void importMidiAtCursor(App& a, const std::string& path);    // into the piano roll's channel at the view start
void exportMidiDialog(App& a, int scope);                    // 0 song, 1 pattern, 2 time selection

// Project interop.
void exportDataFilesDialog(App& a);
void exportBonesDialog(App& a);
void exportZipDialog(App& a);
// File > Export > FL Studio project (.flp) (J-34) / FL Studio zipped loop
// package (.zip: the .flp and the files it uses, J-52).
void exportFlpDialog(App& a);
void exportFlpZipDialog(App& a);
// Writes the open song as an .flp / FL zipped loop package (Save as, the
// export dialogs). Failures and approximated plugins are notified and logged.
bool writeFlpFile(App& a, const std::string& path);
bool writeFlpZip(App& a, const std::string& path);
void openMissingSamples(App& a);
// Imports MIDI files (new pattern) and audio files (playlist clip) into the
// open project; returns false for anything else (projects open through
// App::openProject, J-52).
bool openAny(App& a, const std::string& path);
void importAudioAsClip(App& a, const std::string& path);

// Samples / clips as WAV.
void exportChannelSample(App& a, ObjectId channel);
void exportClipAudio(App& a, ObjectId clip);

}  // namespace exportui
}  // namespace gl
