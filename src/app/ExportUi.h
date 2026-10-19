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
};
void openRender(App& a, Preset p = Preset::Audio);
// Starts the render with the dialog's settings into `folder` (dev checks / scripting).
void startRenderTo(App& a, const std::string& folder);
// Per frame (from drawDialogs): render dialog, progress, MIDI import, missing samples.
void draw(App& a);

// MIDI.
void importMidiFile(App& a, const std::string& path);        // options dialog, then a new pattern + channels
void importMidiAtCursor(App& a, const std::string& path);    // into the piano roll's channel at the view start
void exportMidiDialog(App& a, int scope);                    // 0 song, 1 pattern, 2 time selection

// Project interop.
void exportDataFilesDialog(App& a);
void exportBonesDialog(App& a);
void exportZipDialog(App& a);
void openMissingSamples(App& a);
// Opens anything Gridline reads: projects, FL projects, zipped projects, MIDI
// files (new pattern), audio files (playlist clip). Returns true when handled.
bool openAny(App& a, const std::string& path);
void importAudioAsClip(App& a, const std::string& path);

// Samples / clips as WAV.
void exportChannelSample(App& a, ObjectId channel);
void exportClipAudio(App& a, ObjectId clip);

}  // namespace exportui
}  // namespace gl
