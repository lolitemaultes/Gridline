// Gridline - K-123 (0.7.7): capture while armed. Every armed mixer track that records its input
// keeps the input it heard in a ring, whether or not Record is on, with the song position of each
// frame (latency compensated, as recorded takes are). So a pass played without Record can be
// recovered afterwards, and a punch can reach a little before its punch in (the punch crossfade).
//
// The ring is allocated on the interface thread (ProgramBuilder) and never resized: a new length
// is a new ring in the next Program. The audio thread writes frames and positions (write); the
// interface thread reads what is still held. Positions are kept as runs, not per frame: a mark
// starts a run of evenly spaced positions, and a mark with `jump` set starts a new continuous
// stretch (the transport started, looped, jumped or stopped).
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gl {

class ArmedRing {
 public:
  explicit ArmedRing(size_t frames);  // interface thread
  size_t capacity() const { return cap_; }
  // The capacity a ring asked for `frames` gets (at least the short ring a punch reaches into).
  static size_t capacityFor(size_t frames);

  // ---- audio thread ----
  // n frames of the track's input; ticks[i] is the song position heard when frame i was played
  // (-1: the transport was not playing).
  void write(const float* L, const float* R, const double* ticks, int n);
  // Frames written so far (absolute index of the next frame).
  size_t written() const { return pos_.load(std::memory_order_acquire); }

  // ---- interface thread ----
  // The oldest frame still held.
  size_t oldest() const;
  // Copies frames [from, from + n) if they are all still held; returns the frames copied (0 when
  // some were overwritten meanwhile or are not written yet).
  size_t read(size_t from, size_t n, float* L, float* R) const;
  // The song position of a held frame (false: not held, or the transport was not playing).
  bool tickAt(size_t frame, double& tick) const;
  // The continuous stretch that holds `frame`: [first, end) frames, all with positions that run on
  // without a jump. False when the frame is not held or has no position. `end` stops at written().
  bool runOf(size_t frame, size_t& first, size_t& end) const;
  // The newest continuous stretch of at least `minFrames` frames (the last pass played).
  bool lastRun(size_t minFrames, size_t& first, size_t& end) const;

 private:
  struct Mark {
    size_t frame = 0;
    double tick = -1;  // position of `frame` (-1: no position until the next mark)
    double step = 0;   // ticks per frame after it
    bool jump = true;  // starts a new continuous stretch
  };
  static constexpr size_t kMarks = size_t{1} << 15;
  static constexpr size_t kMarkMargin = 64;  // marks the reader leaves alone (the writer may be on them)
  // Marks still readable: [lo, hi) mark numbers.
  void markRange(size_t& lo, size_t& hi) const;
  // The newest mark at or before `frame` (false: none held).
  bool markFor(size_t frame, size_t lo, size_t hi, size_t& k) const;
  size_t cap_;
  std::vector<float> L_, R_;
  std::vector<Mark> marks_;
  std::atomic<size_t> pos_{0};
  std::atomic<size_t> markHead_{0};
  // (audio thread) the run being written
  double expect_ = -1, step_ = 0, last_ = -1;
};

}  // namespace gl
