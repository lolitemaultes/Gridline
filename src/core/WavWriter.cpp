#include "WavWriter.h"
#include <cmath>
#include <cstdint>
#include <fstream>

namespace gl {
namespace {
void putU32(std::ostream& os, uint32_t v) {
  char b[4] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF),
               static_cast<char>((v >> 16) & 0xFF), static_cast<char>((v >> 24) & 0xFF)};
  os.write(b, 4);
}
void putU16(std::ostream& os, uint16_t v) {
  char b[2] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF)};
  os.write(b, 2);
}
}  // namespace

bool writeWav(const std::string& path, const AudioBuffer& buf, int sampleRate,
              int bitsPerSample) {
  if (buf.channels == 0 || (bitsPerSample != 16 && bitsPerSample != 24)) return false;
  std::ofstream os(path, std::ios::binary);
  if (!os) return false;

  const uint16_t ch = static_cast<uint16_t>(buf.channels);
  const uint16_t bps = static_cast<uint16_t>(bitsPerSample);
  const uint16_t blockAlign = static_cast<uint16_t>(ch * (bps / 8));
  const uint32_t byteRate = static_cast<uint32_t>(sampleRate) * blockAlign;
  const uint32_t dataSize = static_cast<uint32_t>(buf.frames) * blockAlign;

  os.write("RIFF", 4);
  putU32(os, 36 + dataSize);
  os.write("WAVE", 4);
  os.write("fmt ", 4);
  putU32(os, 16);          // PCM fmt chunk size
  putU16(os, 1);           // PCM
  putU16(os, ch);
  putU32(os, static_cast<uint32_t>(sampleRate));
  putU32(os, byteRate);
  putU16(os, blockAlign);
  putU16(os, bps);
  os.write("data", 4);
  putU32(os, dataSize);

  for (size_t i = 0; i < buf.frames; ++i) {
    for (size_t c = 0; c < buf.channels; ++c) {
      float v = buf.at(c, i);
      v = std::fmax(-1.0f, std::fmin(1.0f, v));  // hard clip guard
      if (bps == 16) {
        int32_t s = static_cast<int32_t>(std::lrintf(v * 32767.0f));
        putU16(os, static_cast<uint16_t>(s & 0xFFFF));
      } else {
        int32_t s = static_cast<int32_t>(std::lrintf(v * 8388607.0f));
        char b[3] = {static_cast<char>(s & 0xFF), static_cast<char>((s >> 8) & 0xFF),
                     static_cast<char>((s >> 16) & 0xFF)};
        os.write(b, 3);
      }
    }
  }
  return static_cast<bool>(os);
}

}  // namespace gl
