// Gridline - "tracker" pack, K-106 cp18 (K-316 plan 8.2): the waveform's peaks for the Tracker's
// Sample editor, made so the editor draws a sample of any length at little cost.
//
// * PeakPyramid: per sample file, the lowest and highest value of every block of 64 frames
//   (level 0), of every 128 (level 1), and so on while a level has 2048 entries or more. Built once
//   when the file first shows (about n / 32 floats). range() gives the exact lowest and highest
//   value of any span of frames: whole blocks from the deepest level that fits, raw frames only at
//   the ragged ends (less than 64 at each end).
// * ColumnCache: the lowest and highest value under each pixel column of the editor's view, rebuilt
//   only when what it shows changes (the file, the view, the width, 8-bit play); idle frames reuse it.
// Stereo files show the middle of the two channels, as the plugin editor's waveform does. No drawing
// and no App here (test_tracker_peaks checks both against a brute-force scan).
#pragma once
#include <cstdint>
#include <memory>
#include <vector>

namespace gl {
struct Sample;
}

namespace gl::packs::trackerpack {

class PeakPyramid {
 public:
  static constexpr int kBlock = 64;  // frames per level-0 entry
  // `R` null: mono.
  void build(const float* L, const float* R, int32_t frames);
  int32_t frames() const { return frames_; }
  int levels() const { return static_cast<int>(mn_.size()); }
  size_t entries(int level) const { return mn_[static_cast<size_t>(level)].size(); }
  // The lowest and highest value of frames [a, b) (clamped to the file). False when the span is empty.
  bool range(const float* L, const float* R, int64_t a, int64_t b, float* lo, float* hi) const;
  // How many raw frames the last range() calls read and how many blocks (the test's cost check).
  mutable long long rawReads = 0, blockReads = 0;

 private:
  int32_t frames_ = 0;
  std::vector<std::vector<float>> mn_, mx_;
};

// The pyramid of a loaded sample file, made the first time it is asked for and kept while the file
// is (UI thread).
const PeakPyramid& peaksFor(const std::shared_ptr<const Sample>& sp);

// One column per pixel of a view of `frames` frames starting at `start` (file frames, fractional),
// `len` frames wide over `width` pixels. Rebuilt only when the key changes.
struct ColumnKey {
  const void* file = nullptr;
  double start = 0, len = 0;
  int width = 0;
  bool eightBit = false;
  bool operator==(const ColumnKey& o) const {
    return file == o.file && start == o.start && len == o.len && width == o.width && eightBit == o.eightBit;
  }
};
struct ColumnCache {
  ColumnKey key;
  std::vector<float> lo, hi;  // per column; lo > hi: no frame under it
  long long builds = 0;
  // True when it was rebuilt.
  bool update(const PeakPyramid& p, const float* L, const float* R, const ColumnKey& k);
};

}  // namespace gl::packs::trackerpack
