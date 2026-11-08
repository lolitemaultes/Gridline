// Gridline - tracker modules (K-76): reading FastTracker 2 (.xm) and ProTracker-family (.mod) files.
//
// The parser works on bytes in memory and never trusts a length or count in the file: a short,
// truncated or corrupt file gives an error or a module with what could be read, never a crash or
// a read past the end. After reading, the module is cleaned the way the FT2 player cleans what it
// loads (ranges of volumes, notes, loops, envelope points), so the importer and the instrument can
// rely on those ranges.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include "TrackerChannel.h"

namespace gl::tracker {

inline constexpr int kMaxChannels = 32;
inline constexpr int kMaxPatterns = 256;
inline constexpr int kMaxRows = 256;
inline constexpr int kMaxInstruments = 128;
inline constexpr int kMaxOrders = 256;

enum class Format { Xm, Mod };

struct Sample {
  std::string name;
  SampleDef def;
  bool sixteenBit = false;      // as stored (the data below is always 16-bit values)
  std::vector<int16_t> data;    // 8-bit samples are scaled by 256 (exact)
};

struct Instrument {
  std::string name;
  InstrumentDef def;
  std::array<Sample, kMaxSamples> samples;
  int sampleCount = 0;  // samples the file listed (0 = an empty instrument)
  bool hasAudio() const {
    for (const auto& s : samples)
      if (!s.data.empty()) return true;
    return false;
  }
};

struct Pattern {
  int rows = 64;
  std::vector<Cell> cells;  // rows * kMaxChannels (empty = an empty pattern)
  const Cell& at(int row, int ch) const {
    static const Cell none{};
    if (cells.empty() || row < 0 || row >= rows || ch < 0 || ch >= kMaxChannels) return none;
    return cells[static_cast<size_t>(row) * kMaxChannels + static_cast<size_t>(ch)];
  }
};

struct Module {
  Format format = Format::Xm;
  std::string title, tracker;
  int channels = 8;
  bool linear = true;  // linear frequency table (else Amiga periods)
  int speed = 6, bpm = 125;
  std::vector<uint8_t> orders;  // song order: pattern numbers
  int restart = 0;
  std::vector<Pattern> patterns;           // kMaxPatterns entries
  std::vector<Instrument> instruments;     // index 1..128 (0 unused)
  std::vector<std::string> warnings;       // what was dropped or fixed while reading
};

// True for file names the importer reads (.xm, .mod).
bool isModulePath(const std::string& path);
// Reads a module. false + `err` when the bytes are not a supported module.
bool parseModule(const uint8_t* data, size_t size, Module& out, std::string* err = nullptr);
bool parseModule(const std::vector<uint8_t>& bytes, Module& out, std::string* err = nullptr);

}  // namespace gl::tracker
