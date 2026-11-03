// Gridline — export & interop UI (J-20): the render dialog (scope, formats,
// quality, splitting, destination) with its progress / summary window, MIDI
// import options, project data files / bones / zipped project, the missing
// samples window, and "export as WAV" for samples and audio clips.
#pragma once
#include <functional>
#include <vector>
#include <string>
#include "../core/ProjectInterop.h"
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
// Dev checks and the stress run: a render with these choices, its cancel and its state in a line.
struct DevRender {
  int formats = 1;     // bit per format: WAV, FLAC, MP3, Ogg, AAC
  int scope = 0;       // 0 song, 1 pattern
  int dest = 0;        // 0 files, 1 audio clip, 2 audio editor
  int split = 0;       // 1 mixer tracks, 2 playlist tracks, 3 playlist track groups
  int sampleRate = 2;  // index in the dialog's rate list
  std::string folder;
};
void devStart(App& a, const DevRender& r);
void devCancel();
std::string devState();
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
// export dialogs) as a background job with progress and cancel (J-80,
// FlpExportUi.h); failures are notified as error cards (J-78), a success as
// one summary card. `done(ok)` runs when it has finished. False when it
// could not start.
bool writeFlpFile(App& a, const std::string& path, std::function<void(bool)> done = {});
bool writeFlpZip(App& a, const std::string& path, std::function<void(bool)> done = {});
// Gridline zipped project (J-78), K-47 as a background job with progress and Cancel: failures are
// notified as error cards. `done(files)` runs on the interface thread once the archive is written;
// `files` sums up the files for the caller's "Saved" message. False when it could not start.
bool writeGridlineZip(App& a, const std::string& path, std::function<void(const std::string& files)> done);
void openMissingSamples(App& a);
void openMissingSamples(App& a, std::vector<std::string> known);  // K-42: already checked (on a worker)
// K-47: file work that can take long, off the interface thread, with the same progress window and
// Cancel as a render. `work` runs on a worker with a copy of what it needs and returns what to do with
// the result on the interface thread. `uses` is a snapshot of the song it reads: the unpacked copies
// it refers to stay while the job runs, even if the project closes. The project stays editable
// meanwhile, as during a render. One job at a time: false (with a card) when one is running.
using FileJobDone = std::function<void(App&)>;
bool startFileJob(App& a, const char* title, const Song& uses, std::function<FileJobDone(const ExportControl&)> work);
bool fileJobRunning();
// Dev: waits for a job to end (its result follows on the next frame), the state on stdout.
void waitFileJob();
// Dev: filejob <what> <args>, see main.cpp.
void devFileJob(App& a, const std::vector<std::string>& args);
// J-103 app quit: a running audio export is cancelled and its worker joined (not left to
// the static Job's destructor during static teardown). Idempotent.
void shutdown();
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
