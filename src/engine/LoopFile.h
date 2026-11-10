// Gridline - what an audio file says about itself as a loop: sliced loop
// files (.rx2: slices, beats, tempo), tagged loops (.caf: beat count, key,
// tags, beat markers), AIFF loops (the "basc" chunk: beats, key, meter) and
// WAV loops (the "acid" chunk: beats, tempo, root note). Reads chunks only,
// never decodes audio, so the browser can ask for every file it lists.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gl::loopfile {

enum class Format { None, Sliced, Tagged, Aiff, Wav };

struct Slice {
  int64_t start = 0, length = 0;  // frames of the decoded audio
  double beat = 0;                // position in the loop (beats from its start)
  bool muted = false;
};

struct Info {
  Format format = Format::None;
  bool loop = false;      // plays in time: it states its beats (a one-shot does not)
  double bpm = 0;         // its own tempo (0 = unknown)
  double beats = 0;       // loop length in beats (0 = unknown)
  int sigNum = 4, sigDen = 4;
  int key = -1;           // pitch class, -1 = none
  bool minor = false;
  bool hasMode = false;   // the file states major or minor
  double sampleRate = 0;
  int64_t frames = 0;     // frames of audio (0 = unknown)
  int64_t loopStart = 0, loopEnd = 0;  // the part that loops (frames; end 0 = to the end)
  std::vector<Slice> slices;           // sliced loops; tagged loops: cut at their beat markers
  std::vector<int64_t> markers;        // tagged loops: beat markers (frames)
  std::vector<std::string> tags;       // lower case: format, loop or one-shot, the file's own tags
};

// From the file's bytes; false when the file says nothing about itself as a loop.
bool readBytes(const uint8_t* p, size_t n, Info& out);
// From a file (its first 64 MB at most).
bool read(const std::string& path, Info& out);

// The frames that loop: the file's loop region when it states one inside its `frames` frames of
// audio, else all of them.
int64_t loopFrames(const Info& li, int64_t frames);
// The same for a file by path, read once per path and kept (for the editors; thread safe).
int64_t loopFramesOf(const std::string& path, int64_t frames);

// Format words shown and searched ("sliced loop", "tagged loop", ...), "" for None.
const char* formatTag(Format f);

}  // namespace gl::loopfile
