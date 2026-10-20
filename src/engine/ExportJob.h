// Gridline — a complete export ("render to file", RN-01..05, RN-07): one
// render (or one per mixer insert / playlist track) written to every chosen
// format, plus MIDI. Shared by the export dialog (worker thread) and the CLI.
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <vector>
#include "../core/MidiFile.h"
#include "../core/Song.h"
#include "Export.h"
#include "Render.h"

namespace gl {

struct ExportSettings {
  RenderJob render;                  // scope, loops, tail, switches, quality, sample rate
  std::vector<ExportFormat> formats = {ExportFormat::Wav};
  bool midi = false;                 // also write a .mid of the scope
  int wavBits = 24;                  // 16 / 24 / 32 (float)
  int flacBits = 24;                 // 16 / 24
  DitherMode dither = DitherMode::Tpdf;
  int mp3Bitrate = 320, oggBitrate = 256, m4aBitrate = 256;
  ChannelMode channels = ChannelMode::Stereo;
  bool normalize = false;
  double normalizeDb = 0.0;
  bool tempoInfo = true;             // WAV: tempo / beats ('acid')
  bool markers = true;               // WAV: song markers inside the scope as cue points
  // Splitting (both off: one mix).
  bool splitMixer = false;           // one file per insert ("split mixer tracks")
  bool stemsMasterFx = false;        // stems go through the master effects
  bool stemsIncludeMaster = true;    // with split: also the full mix (the master's own file)
  std::vector<ObjectId> stemTracks;  // inserts to split (empty = every insert that carries sound)
  bool splitPlaylist = false;        // one file per playlist track
  bool splitPlaylistGroups = false;  // J-31: one file per playlist track group
  // Files: <folder>/<baseName>.<ext>; split parts use `partPattern` (see expandNamePattern).
  std::string folder;
  std::string baseName = "Untitled";
  std::string partPattern = "%P - %N %T";
  std::string projectTitle;
};

struct ExportOutcome {
  std::vector<std::string> files;   // written
  std::vector<std::string> errors;
  bool cancelled = false;
  int64_t frames = 0;               // frames of the (first) mix
  float peak = 0;                   // before normalising (first mix)
};

// Writes everything. `progress` gets 0..1 and a short status line.
ExportOutcome runExport(const Song& song, const ExportSettings& s,
                        const std::function<void(double, const std::string&)>& progress = {},
                        const std::atomic<bool>* cancel = nullptr);

// MIDI for an export scope (song / pattern / range).
MidiFileData exportScopeMidi(const Song& song, const RenderJob& job);
// WAV markers of the song markers inside the render scope (frame positions).
std::vector<WavMarker> scopeMarkers(const Song& song, const RenderJob& job, Tick start, Tick end);

}  // namespace gl
