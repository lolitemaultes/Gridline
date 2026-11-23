// Gridline: when realtime playback shares a block of the mixer graph out over the worker threads
// (GitHub #66). Waking workers costs what they spend waiting for each other, so a graph that one
// thread finishes in good time runs on the audio thread alone and the workers sleep.
//
// The gate looks at the graph's measured work per block (the sum of its tasks' times) and its heavy
// tasks (the ones worth a thread of their own):
//  * it opens when the work is over `enter` of the block's time in two blocks in a row, or over
//    `urgent` in one block with at least two heavy tasks (a sudden load: no block late for it);
//  * one long block alone does not open it: the audio thread preempted inside a task (a busy
//    computer) stretches that one task, not the graph's work;
//  * it closes only after `calmBlocks` blocks in a row under `leave` (below `enter`, so it never
//    flaps on a load near the line).
// Pure logic, no clocks: tests feed it numbers.
#pragma once
#include <cstdint>

namespace gl {

class ParallelGate {
 public:
  static constexpr double kEnter = 0.3, kLeave = 0.2, kUrgent = 0.8;
  static constexpr int kCalmBlocks = 64;

  // One block: `workNs` of tasks, `heavy` of them heavy, in a block of `blockNs`. True: share the
  // next block out.
  bool update(uint64_t workNs, int heavy, double blockNs) {
    const double w = static_cast<double>(workNs);
    overRun_ = w > kEnter * blockNs ? overRun_ + 1 : 0;
    if (open_) {
      calm_ = w < kLeave * blockNs ? calm_ + 1 : 0;
      if (calm_ >= kCalmBlocks) open_ = false, calm_ = 0;
    } else if (overRun_ >= 2 || (w > kUrgent * blockNs && heavy >= 2)) {
      open_ = true, calm_ = 0;
    }
    return open_;
  }
  bool open() const { return open_; }
  void reset() { open_ = false, overRun_ = 0, calm_ = 0; }

 private:
  bool open_ = false;
  int overRun_ = 0, calm_ = 0;
};

}  // namespace gl
