// Gridline — live frame statistics (dev flag `--frame-stats`, J-66): once a
// second, stderr gets the main loop's frame rate, frame time and where it
// went (event pump, tick, interface, presenting, plugin windows, waiting),
// the process CPU load and how often hosted plugin interfaces drew.
// Measures a normal interactive run (vsync on), unlike --profile-frames.
// Audio blocks that missed their deadline are counted too.
//
// Related dev switches (environment):
//   GRIDLINE_DEV_DRAG="x,y,seconds"  turns a knob of the first plugin window's
//                                    interface (posted mouse drag at x,y) ~4 s in
//   GRIDLINE_DRAW_STATS_LOG=1        lists the interface's view classes
//   GRIDLINE_VSYNC_SWAP=1            old pacing: blocking vsync swap instead of
//                                    the run-loop frame clock (A/B comparisons)
#pragma once
#include <atomic>
#include <cstdint>

namespace gl::fstats {

void enable(bool on);
bool enabled();

enum Part { Events, Tick, Ui, Present, PluginWindows, Wait, kParts };
const char* partName(Part p);

void add(Part p, double ms);
// Time stamp (0 when disabled) and the time since it added to a part.
uint64_t stamp();
void since(Part p, uint64_t t0);
// Wall time of a scope added to a part (one branch when disabled).
class Timer {
 public:
  explicit Timer(Part p);
  ~Timer();
  Timer(const Timer&) = delete;
  Timer& operator=(const Timer&) = delete;

 private:
  Part p_;
  uint64_t t0_ = 0;
};
// Closes a frame; prints the last second's summary when one has passed.
void endFrame();
// Audio thread (lock-free): one output block took `busy` seconds of a
// `period`-second budget; late blocks (busy > period) are counted as dropouts.
void audioBlock(double busy, double period);

}  // namespace gl::fstats
