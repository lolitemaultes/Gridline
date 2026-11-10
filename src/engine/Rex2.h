// Gridline - sliced loop files (.rx2): the chunk container, the slice list,
// the loop's tempo and length, and the losslessly compressed audio, decoded
// sample for sample. Written from the public-domain format description of
// the VelociLoops project (no third-party code). Older AIFF based loop files
// (.rex, .rcy) are plain AIFF audio and decode through the AIFF reader.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "WavPack.h"

namespace gl::rex {

// One entry of the slice list as the file stores it.
struct SliceEntry {
  uint32_t start = 0;   // first frame in the decoded audio
  uint32_t length = 0;  // frames; 1 or less: an analysis marker, not a slice
  uint16_t points = 0;  // analysis strength (0x7fff = a slice the user placed)
  uint8_t flags = 0;    // bit 0 muted, 1 locked, 2 selected
  bool marker() const { return length <= 1; }
  bool muted() const { return (flags & 1) != 0; }
};

struct Info {
  // Audio (SINF).
  int channels = 0;           // 1 or 2
  int formatCode = 0;         // 3 = 16-bit, 5 = 24-bit
  int bits = 16;
  uint32_t sampleRate = 0;
  uint32_t frames = 0;        // decoded frames
  uint32_t loopStart = 0, loopEnd = 0;  // frames, end exclusive (end <= start: the whole audio)
  // Loop settings (GLOB).
  uint32_t entryCount = 0;
  int bars = 0, beats = 0, sigNum = 4, sigDen = 4;
  int sensitivity = 0, gateSensitivity = 0, gain = 0;
  uint32_t tempoMilli = 0;          // preview tempo, BPM x 1000
  std::vector<SliceEntry> entries;  // the stored list, in file order
  // Compressed audio (SDAT) within the file.
  size_t audioOffset = 0, audioSize = 0;
  bool hasAudio = false;
};

// A sliced loop file: a "CAT " container of type "REX2".
bool looksLikeRex2(const uint8_t* p, size_t n);
// Reads the chunks (not the audio). False + err for files that are not
// sliced loop files or whose header chunks are damaged.
bool parse(const uint8_t* p, size_t n, Info& out, std::string* err);

// The compressed audio decoded to integer samples at the file's bit depth,
// interleaved (left, right) for stereo. Stops after `frames` frames; data
// that ends early leaves the rest silent and returns false.
bool decodePcm(const uint8_t* data, size_t size, int channels, int bits, uint32_t frames, std::vector<int32_t>& out, std::string* err);

// The whole file decoded: audio as floats (-1..1) and the chunk info.
bool decode(const uint8_t* p, size_t n, DecodedAudio& out, Info* info, std::string* err);

// The loop region (start, end) in frames, the whole audio when the file has none.
void loopRegion(const Info& in, uint32_t& start, uint32_t& end);
// Loop length in beats of the time signature's note value (bars and beats of
// the settings; else from the tempo and the loop's length). 0 when unknown.
double loopBeats(const Info& in);
// The audio's own tempo in BPM: the loop's beats (in quarter notes) over the
// loop region's length; the stored preview tempo when that is unknown. 0 when
// neither is known.
double tempo(const Info& in);
// The tempo the loop was last auditioned at (stored), 0 when none.
double previewTempo(const Info& in);

// The slices to play: starts (frames, ascending) of the stored slices inside
// the loop region (analysis markers are not slices), a leading slice from the
// loop start when the first stored one starts later, lengths (frames until
// the slice's audio ends: the next start, or less when the file stores a
// shorter one) and the muted flags.
struct Slice {
  uint32_t start = 0, length = 0;
  bool muted = false;
};
std::vector<Slice> slices(const Info& in);

// Position of a frame in the loop, in beats from the loop start.
double beatOf(const Info& in, uint32_t frame);

}  // namespace gl::rex
