// Gridline — minimal PCM WAV writer (16/24-bit, interleaved).
#pragma once
#include <string>
#include "AudioBuffer.h"

namespace gl {

// Writes an interleaved RIFF/WAVE file. Returns false on I/O error.
bool writeWav(const std::string& path, const AudioBuffer& buf, int sampleRate,
              int bitsPerSample = 16);

}  // namespace gl
