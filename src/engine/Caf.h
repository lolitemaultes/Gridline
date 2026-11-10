// Gridline - Core Audio Format files (.caf), as tagged loops use them: the
// chunk container, linear PCM decoded here, compressed audio (AAC, Apple
// Lossless) through the computer's decoder (engine/AudioCodecsAac.cpp, with
// a raw AAC stream rebuilt in memory for decoders that cannot open the
// container), encoder priming and padding trimmed from the packet table, and
// the loop metadata: beat count, time signature, key, tags and beat markers.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "WavPack.h"

namespace gl::caf {

struct Info {
  // Audio description ("desc").
  double sampleRate = 0;
  char format[5] = {0, 0, 0, 0, 0};  // "lpcm", "aac ", "alac", ...
  uint32_t formatFlags = 0;
  uint32_t bytesPerPacket = 0, framesPerPacket = 0, channels = 0, bits = 0;
  // Audio data ("data", after its edit count) within the file.
  size_t dataOffset = 0, dataSize = 0;
  bool hasData = false;
  // Packet table ("pakt"): packets, frames that play, priming and padding frames.
  bool hasPakt = false;
  int64_t packets = 0, validFrames = 0;
  int32_t priming = 0, remainder = 0;
  std::vector<uint32_t> packetSizes;  // bytes of each packet (variable-size formats)
  // Codec cookie ("kuki").
  std::vector<uint8_t> cookie;
  // Loop metadata.
  bool hasLoop = false;                              // the loop metadata chunk is there
  std::vector<std::pair<std::string, std::string>> meta;  // its key / value pairs, as stored
  std::vector<std::pair<std::string, std::string>> info;  // the "info" chunk's pairs
  std::vector<int64_t> markers;                     // beat markers (frames)
  bool lpcm() const { return format[0] == 'l' && format[1] == 'p' && format[2] == 'c' && format[3] == 'm'; }
  bool aac() const { return format[0] == 'a' && format[1] == 'a' && format[2] == 'c' && format[3] == ' '; }
  // Frames that play (packet table, else from the data size for PCM), 0 when unknown.
  int64_t frames() const;
  std::string value(const std::string& key) const;  // loop metadata value, "" when missing
};

bool looksLikeCaf(const uint8_t* p, size_t n);
// Reads the chunks (not the audio). False + err when the file is not a CAF
// file or its chunk list is damaged.
bool parse(const uint8_t* p, size_t n, Info& out, std::string* err);

// Decodes the file at `path` whose bytes are `p`/`n`.
bool decode(const std::string& path, const uint8_t* p, size_t n, DecodedAudio& out, std::string* err);

// The AAC packets as a raw AAC (ADTS) stream: one 7-byte header per packet
// from the cookie's audio configuration. Empty when the file is not AAC or
// its cookie or packet table cannot be read.
std::vector<uint8_t> toAdts(const uint8_t* p, size_t n, const Info& in);

// Raw decoder output (priming and padding included, or already trimmed by the
// decoder) cut to the frames the packet table says play.
void trimToPacketTable(DecodedAudio& d, const Info& in);

// Loop facts from the metadata: beat count (0 = a one-shot), time signature,
// key (pitch class, -1 = none) and mode, and tags (category, subcategory,
// genre, descriptors; lower case, no duplicates).
struct Loop {
  double beats = 0;
  int sigNum = 4, sigDen = 4;
  int key = -1;
  bool minor = false;
  std::vector<std::string> tags;
};
Loop loopOf(const Info& in);

}  // namespace gl::caf
