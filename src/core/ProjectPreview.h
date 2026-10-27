// Gridline - J-141: project sketches for the welcome launcher. A sketch is a small,
// resolution-free description of a song: its arrangement as rows of coloured blocks on a
// normalised time axis (or, with a near-empty playlist, the first pattern's steps / notes;
// without either, the channel rack or the named mixer inserts),
// plus the facts a tile shows (title, tempo, metre, length, counts). The launcher draws it
// as vectors, so it is crisp at every interface scale and follows the theme.
//
// Sketches are cached as small JSON files, `<cacheDir>/<fnv64 of the path>.json`, stamped
// with the project file's size and modification time: a changed file makes its sketch stale.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "Json.h"
#include "Song.h"

namespace gl::preview {

inline constexpr int kVersion = 2;
inline constexpr int kMaxRows = 10;     // arrangement rows drawn (later tracks fold into the last)
inline constexpr size_t kMaxBlocks = 320;
inline constexpr int kMaxStepCols = 64;

struct Block {
  int row = 0;
  float x0 = 0, x1 = 0;  // 0..1 of the song length (steps: of the pattern)
  uint32_t rgb = 0;      // content colour 0xRRGGBB
  bool muted = false;
};

// Arrangement: clips by track; Pattern: the first pattern's steps (cols > 0) or notes;
// Rack: channels with nothing to play yet (a row per channel); Mixer: named inserts only
// (a strip per insert, `row` = strip index).
enum class Kind { Empty, Arrangement, Pattern, Rack, Mixer };

struct Sketch {
  Kind kind = Kind::Empty;
  std::string title;
  double tempo = 0;
  int num = 4, den = 4;
  int bars = 0;           // song (arrangement) or pattern length in bars
  double seconds = 0;     // song length at the song tempo
  int channels = 0, patterns = 0, clips = 0;
  int rows = 0;           // arrangement / pattern rows in use
  int cols = 0;           // Pattern: step columns
  std::vector<Block> blocks;
  // Source stamp (cache validation).
  uint64_t fileSize = 0;
  int64_t fileTime = 0;
};

Sketch make(const Song& s);
Json toJson(const Sketch& k);
bool fromJson(const Json& j, Sketch& out);

// 64-bit FNV-1a of the path, as 16 hex digits (cache file names).
std::string pathKey(const std::string& path);
std::string cacheFile(const std::string& cacheDir, const std::string& projectPath);
// The file's current size and time (false when it is missing).
bool fileStamp(const std::string& path, uint64_t& size, int64_t& time);
// A cached sketch whose stamp still matches the project file.
bool loadCached(const std::string& cacheDir, const std::string& projectPath, Sketch& out);
// Stamps `k` with the project file and writes it. False when the file is missing or unwritable.
bool storeCached(const std::string& cacheDir, const std::string& projectPath, Sketch k);

}  // namespace gl::preview
