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

}  // namespace gl
