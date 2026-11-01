// Gridline — J-217: min / max summary of an audio waveform, built incrementally.
//
// The playlist draws audio clips from it: one (lo, hi) pair per bin of
// `binFrames()` frames of the mono mix (0.5 * (L + R), as the clip plays its
// file), plus a coarse level (one pair per kGroup bins) so a range query costs
// O(kGroup) whatever its length, i.e. drawing a zoomed-out hour-long take
// costs the same as a short one.
//
// add() extends the summary in any chunk sizes: the result is identical to
// one add() of the whole signal (block-size invariant), so a take summarised
// while it records equals the summary of its file. Memory is bounded: when the
// bin count reaches `maxBins`, neighbouring bins merge (the bin width doubles),
// deterministically from the frame count, so the live and the rescanned
// summaries still agree. No allocation beyond amortised vector growth.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace gl {

class PeakSummary {
 public:
  static constexpr size_t kBin = 128;                     // frames per bin (until the cap doubles it)
  static constexpr size_t kGroup = 64;                    // bins per coarse entry
  static constexpr size_t kDefaultMaxBins = size_t{1} << 21;  // 16 MB of pairs: ~93 min at 48 kHz before bins widen

  explicit PeakSummary(size_t maxBins = kDefaultMaxBins) : maxBins_(std::max<size_t>(2, maxBins & ~size_t{1})) {}

  void clear() {
    lo_.clear(), hi_.clear(), clo_.clear(), chi_.clear();
    frames_ = 0;
    inBin_ = 0;
    bin_ = kBin;
  }
  // Mono mix of L and R (R = L for mono material), `n` frames.
  void add(const float* L, const float* R, size_t n) {
    for (size_t i = 0; i < n; ++i) push(0.5f * (L[i] + R[i]));
  }
  void addSilence(size_t n) {
    for (size_t i = 0; i < n; ++i) push(0.0f);
  }
  size_t frames() const { return frames_; }
  size_t bins() const { return lo_.size(); }
  size_t binFrames() const { return bin_; }
  float lo(size_t b) const { return lo_[b]; }
  float hi(size_t b) const { return hi_[b]; }
  // Min / max over bins [b0, b1), folded into lo / hi (which the caller seeds).
  void range(size_t b0, size_t b1, float& lo, float& hi) const {
    b1 = std::min(b1, lo_.size());
    while (b0 < b1 && b0 % kGroup != 0) lo = std::min(lo, lo_[b0]), hi = std::max(hi, hi_[b0]), ++b0;
    while (b0 + kGroup <= b1) {
      const size_t g = b0 / kGroup;
      lo = std::min(lo, clo_[g]), hi = std::max(hi, chi_[g]);
      b0 += kGroup;
    }
    while (b0 < b1) lo = std::min(lo, lo_[b0]), hi = std::max(hi, hi_[b0]), ++b0;
  }
  // FNV-1a of every bin's bits (dev checks: two summaries drawn the same).
  uint64_t checksum() const {
    uint64_t h = 1469598103934665603ull;
    auto mixf = [&](float f) {
      uint32_t u;
      static_assert(sizeof u == sizeof f, "float");
      std::memcpy(&u, &f, sizeof u);
      for (int k = 0; k < 4; ++k) h = (h ^ ((u >> (8 * k)) & 0xFFu)) * 1099511628211ull;
    };
    for (size_t b = 0; b < lo_.size(); ++b) mixf(lo_[b]), mixf(hi_[b]);
    return h ^ static_cast<uint64_t>(bin_);
  }
  size_t memoryBytes() const { return (lo_.capacity() + hi_.capacity() + clo_.capacity() + chi_.capacity()) * sizeof(float); }

 private:
  std::vector<float> lo_, hi_;    // per bin
  std::vector<float> clo_, chi_;  // per kGroup bins
  size_t frames_ = 0;
  size_t inBin_ = 0;  // frames in the last bin (0: none open, the next frame starts one)
  size_t bin_ = kBin;
  size_t maxBins_;

  void push(float v) {
    if (inBin_ == 0) {  // a new bin starts
      if (lo_.size() >= maxBins_) widen();
      lo_.push_back(v), hi_.push_back(v);
      const size_t b = lo_.size() - 1;
      if (b % kGroup == 0) clo_.push_back(v), chi_.push_back(v);
    } else {
      float& l = lo_.back();
      float& h = hi_.back();
      l = std::min(l, v), h = std::max(h, v);
    }
    float& cl = clo_.back();
    float& ch = chi_.back();
    cl = std::min(cl, v), ch = std::max(ch, v);
    ++frames_;
    if (++inBin_ == bin_) inBin_ = 0;
  }
  // Every pair of bins becomes one (all bins are full here: a new one is about to start).
  void widen() {
    const size_t n = lo_.size() / 2;
    for (size_t k = 0; k < n; ++k) {
      lo_[k] = std::min(lo_[2 * k], lo_[2 * k + 1]);
      hi_[k] = std::max(hi_[2 * k], hi_[2 * k + 1]);
    }
    lo_.resize(n), hi_.resize(n);
    bin_ *= 2;  // (inBin_ is 0: the merged bins are all full)
    clo_.clear(), chi_.clear();
    for (size_t b = 0; b < n; ++b) {
      if (b % kGroup == 0) clo_.push_back(lo_[b]), chi_.push_back(hi_[b]);
      clo_.back() = std::min(clo_.back(), lo_[b]), chi_.back() = std::max(chi_.back(), hi_[b]);
    }
  }
};

}  // namespace gl
