// Gridline - J-219: Track Receipt measurement. Finds every audio file a project
// refers to (plugin states and data, audio clips, mixer effects, paths and
// Tracklib-style names inside hosted plugins' saved chunks), then plays the
// project offline with the sample coverage tap (engine/SampleTap.h) on, so
// each built-in voice reports which parts of each file it reads.
//
// Not counted (listed as such): muted clips, clips on muted playlist tracks,
// muted channels and channels whose volume or mixer track is silent.
// Not measurable: files used inside third-party plugins (and built-ins that
// analyse a file instead of playing it: wavetables, impulse responses,
// resynthesis): counted as the whole file, marked estimated.
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <vector>
#include "../core/Song.h"
#include "../core/TrackReceipt.h"

namespace gl::receipt {

struct PassOptions {
  int sampleRate = 44100;
  std::function<void(double)> progress;  // 0..1 (rendering thread)
  const std::atomic<bool>* cancel = nullptr;
};

// One file reference of a plugin.
struct FileRef {
  std::string path;          // absolute path, or a bare file name found in a chunk
  ObjectId owner = kNoId;    // channel or mixer track
  bool channel = true;       // false: a mixer effect slot
  int slot = -1;             // effect slot (mixer) / note processor index
  bool hosted = false;       // inside a third-party plugin (its saved chunk)
  std::string plugin;        // plugin display name
};
// Every audio file the project refers to (static scan, no audio decoded).
std::vector<FileRef> fileRefs(const Song& song);
// Paths and Tracklib-style file names inside a hosted plugin's saved state
// ("<prefix>:<base64>[:<base64>]"; compressed blocks inside are inflated).
std::vector<std::string> chunkFileNames(const std::string& data);
bool isAudioName(const std::string& path);

// A render of the project was written to `path`: its sidecar names every file
// the project refers to (which parts went in is not known). False when none.
bool writeRenderOrigin(const Song& song, const std::string& path);

// The files only (no render): what the picker lists. `decode` reads each file's length.
Measurement scan(const Song& song, bool decode = false);
// The full measurement (renders the project; can take a while for long songs).
Measurement measure(const Song& song, const PassOptions& o = {});

}  // namespace gl::receipt
