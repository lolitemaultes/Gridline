// Gridline — WavPack (.wv) decoder: lossless integer (mono, stereo, joint
// stereo, 8–24-bit and packed 32-bit PCM), 32-bit float, and hybrid/lossy
// streams decoded without their correction (.wvc) file. Written from the
// WavPack 4/5 block format description; no third-party code. DSD streams are
// reported as unsupported.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gl {

struct DecodedAudio {
  int channels = 0;
  int sampleRate = 0;
  int bitsPerSample = 0;
  std::vector<float> interleaved;  // frames × channels, -1..1
  size_t frames() const { return channels > 0 ? interleaved.size() / static_cast<size_t>(channels) : 0; }
};

// Decodes a complete .wv file held in memory. Every block's checksum is
// verified (for hybrid files: the checksum of the lossy reconstruction; for
// float / 32-bit data also the extra-bits checksum); false + err on corrupt or
// unsupported data.
bool decodeWavPack(const uint8_t* data, size_t size, DecodedAudio& out, std::string* err);

// Ogg Vorbis (whole stream in memory). Implemented in AudioCodecs.cpp.
bool decodeOggVorbis(const uint8_t* data, size_t size, DecodedAudio& out, std::string* err);

// K-51: MPEG-4 audio (.m4a / .mp4: AAC, HE-AAC, Apple Lossless) and raw AAC (ADTS .aac),
// decoded from the file at `path` by the computer's own decoder (macOS AudioToolbox, Windows
// Media Foundation), else by the FFmpeg program (Linux, the BSDs). AudioCodecsAac.cpp.
bool looksLikeAacFile(const uint8_t* data, size_t size);
bool isAacExtension(const std::string& lowerExt);  // ".m4a", ".aac", ...
bool decodeAacFile(const std::string& path, DecodedAudio& out, std::string* err);
enum class AacRoute { Auto, System, Program };  // tests: only the system's decoder, only FFmpeg
void setAacRouteForTests(AacRoute r);

// The encoder priming and the real length an MPEG-4 file states for its first audio track,
// from the iTunes gapless tag (iTunSMPB) or the track's edit list. Units: the track's own
// samples (the media timescale; iTunSMPB counts the AAC core rate, half the output rate for
// HE-AAC). Mp4Gapless::found is false for a file that says neither. AudioCodecsAac.cpp.
struct Mp4Gapless {
  bool found = false;
  bool fromTag = false;     // iTunSMPB, else the edit list
  uint64_t priming = 0;     // samples to drop at the start
  uint64_t length = 0;      // samples to keep after them
  uint64_t padding = 0;     // iTunSMPB only: samples after the end
  uint32_t timescale = 0;   // the audio track's media timescale (mdhd), 0 if unknown
};
Mp4Gapless readMp4Gapless(const std::string& path);

// J-51: both decoders give up (false, "cancelled") when the calling thread's
// DecodeCancelScope flag is set (Sample.h; defined in Sample.cpp).
bool decodeCancelled();

}  // namespace gl
