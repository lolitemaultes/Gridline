// Gridline — audio export (RN-02, RN-03, RN-05): output formats, dithering,
// channel modes, normalisation, WAV metadata (markers, tempo), external
// encoders, file naming patterns.
//
// Encoders: WAV is written here; FLAC uses the audio editor's own encoder
// (packs/edison). MP3, Ogg Vorbis and AAC (.m4a) have no permissively
// licensed encoder Gridline can vendor, so they go through an encoder program
// installed on the computer (ffmpeg, lame, oggenc), run as a separate process;
// encoderStatus() reports what is available.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "../core/AudioBuffer.h"

namespace gl {

enum class ExportFormat : int { Wav = 0, Flac, Mp3, Ogg, M4a, Count };
const char* exportFormatName(ExportFormat f);
const char* exportFormatExt(ExportFormat f);  // ".wav", …
bool exportFormatExternal(ExportFormat f);    // needs an encoder program

enum class DitherMode : int {
  None = 0,     // plain rounding
  Tpdf,         // triangular dither, ±1 LSB (flat noise)
  NoiseShaped,  // triangular dither + error feedback that moves the noise to high frequencies
  Count
};
const char* ditherName(DitherMode d);

enum class ChannelMode : int { Stereo = 0, MonoMerged, MonoLeft, MonoRight, Count };
const char* channelModeName(ChannelMode m);

// Stereo → the requested channel layout (mono modes give one channel).
AudioBuffer applyChannelMode(const AudioBuffer& in, ChannelMode m);
// Scales to `targetDb` dBFS peak; returns the gain applied (1 for silence).
double normalizeBuffer(AudioBuffer& buf, double targetDb = 0.0);

// Quantises to `bits` (16 or 24) in place: every sample becomes an exact
// multiple of 1 / 2^(bits-1) (clamped to the integer range), with dither.
// Deterministic for a given seed.
void quantizeBuffer(AudioBuffer& buf, int bits, DitherMode dither, uint32_t seed = 0x2545F491u);
// Integer sample value of a quantised float (inverse of the scaling above).
inline int32_t quantizedInt(float x, int bits) {
  const double full = static_cast<double>(int64_t{1} << (bits - 1));
  double v = static_cast<double>(x) * full;
  v = v < -full ? -full : v > full - 1 ? full - 1 : v;
  return static_cast<int32_t>(v >= 0 ? v + 0.5 : v - 0.5);
}

// WAV metadata.
struct WavMarker {
  int64_t frame = 0;
  std::string name;
};
struct WavMeta {
  std::vector<WavMarker> markers;  // cue points + labels
  double tempo = 0;                // > 0: 'acid' chunk (tempo, beats, meter)
  int beats = 0;
  int meterNum = 4, meterDen = 4;
  bool loop = false;               // acid "loop" flag (one-shot otherwise)
  int64_t loopStart = -1, loopEnd = -1;  // sampler loop ('smpl')
};
// 16 / 24-bit integer or 32-bit float. Integer data must already be
// quantised (quantizeBuffer) for dither to take effect; otherwise it is rounded.
bool writeWavFile(const std::string& path, const AudioBuffer& buf, int sampleRate, int bits, const WavMeta& meta = {},
                  std::string* err = nullptr);

struct AudioFileOptions {
  ExportFormat format = ExportFormat::Wav;
  int bits = 24;                 // WAV 16 / 24 / 32 (float); FLAC 16 / 24
  DitherMode dither = DitherMode::Tpdf;  // integer formats
  int bitrate = 320;             // MP3 / Ogg / AAC, kbit/s
  WavMeta meta;                  // WAV only
  uint32_t seed = 0x2545F491u;   // dither seed
};
// Writes one audio file (the buffer is the final channel layout). Integer
// formats are dithered here.
bool writeAudioFile(const std::string& path, const AudioBuffer& buf, int sampleRate, const AudioFileOptions& o,
                    std::string* err = nullptr);

// External encoders: the program used for a format ("" = none found).
std::string encoderFor(ExportFormat f);
// One line for the UI: "ffmpeg (/usr/local/bin/ffmpeg)" or "not installed: …".
std::string encoderStatus(ExportFormat f);
// Overrides the search (tests): program name → path ("" = pretend missing).
void setEncoderOverride(const std::string& program, const std::string& path);

// File naming patterns. Tokens: %P project title, %S scope (Song / Pattern /
// Range), %T part name (insert or playlist track), %N part number (2 digits),
// %F format, %R sample rate, %D date (YYYY-MM-DD), %% percent. Characters
// that are not allowed in file names are replaced.
std::string expandNamePattern(const std::string& pattern, const std::map<char, std::string>& tokens);
std::string safeFileName(const std::string& s);

}  // namespace gl
