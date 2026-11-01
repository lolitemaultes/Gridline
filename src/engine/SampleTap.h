// Gridline - J-219: sample coverage tap (Track Receipt).
//
// While a tap Pass is active (an offline receipt render), every voice that
// plays an audio file reports which source frames it reads. The pass keeps,
// per watched file, an exact bitmap of the file's milliseconds that were read
// and, per (file, owner) pair (owner = the channel or audio clip that played
// it), the merged source ranges, how often it started and where in the song.
//
// Audio thread: no allocation, no locks. Everything is preallocated when the
// Pass is built (UI / worker thread); the audio side claims pair slots with a
// compare-and-swap and sets bits with fetch_or. When no pass is active, every
// call is a single relaxed load and returns at once.
//
// Voices use it like this (per render block):
//   tap::Run run(sample);          // finds the slot of (sample, current owner)
//   for (...) { run.at(framePos); ... }   // the source frame read for this output frame
//   // (flushes when it goes out of scope; jumps start a new range)
// and at a note start: tap::play(sample).
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "../core/Types.h"

namespace gl {
struct Sample;
}

namespace gl::tap {

struct Range {
  double a = 0, b = 0;  // seconds of the file, [a, b)
};

struct Slot;
struct PassData;

// True while a pass is active (any thread).
bool active();

// The slot of (sample's file, the current owner); nullptr when no pass is
// active or the sample's file is not watched.
Slot* slotFor(const Sample* s);
// Counts a start (a note, a clip) of `s` for the current owner.
void play(const Sample* s);
void play(Slot* slot);
// Marks seconds [a, b) of the slot's file as read.
void mark(Slot* slot, double a, double b);
// Marks the share [f0, f1) (0..1 of the file's length) as read: for plugins
// that play a processed copy whose positions map to the file by share.
void markShare(Slot* slot, double f0, double f1);
// Marks source frames f0..f1 (inclusive, fractional) of a sample as read.
void markFrames(const Sample* s, double f0, double f1);

// The owner (Channel::id or Clip::id) and song tick of the code running on
// this thread. The engine sets it around each channel's processing and each
// audio clip; nested scopes restore the outer owner.
class OwnerScope {
 public:
  OwnerScope(ObjectId owner, double tick);
  ~OwnerScope();
  OwnerScope(const OwnerScope&) = delete;
  OwnerScope& operator=(const OwnerScope&) = delete;

 private:
  ObjectId prevOwner_;
  double prevTick_;
  bool on_;
};

// A run of consecutive reads of one sample (one voice, one block): frames
// that follow each other closely extend the run; a jump flushes it.
class Run {
 public:
  Run() = default;
  explicit Run(const Sample* s) { begin(s); }
  ~Run() { flush(); }
  Run(const Run&) = delete;
  Run& operator=(const Run&) = delete;
  void begin(const Sample* s);
  bool on() const { return slot_ != nullptr; }
  void at(double frame) {
    if (!slot_) return;
    if (!open_) {
      lo_ = hi_ = last_ = frame;
      open_ = true;
      return;
    }
    const double d = frame - last_;
    if (d > kGap || d < -kGap) {
      flushOpen();
      lo_ = hi_ = frame;
      open_ = true;
    } else {
      lo_ = frame < lo_ ? frame : lo_;
      hi_ = frame > hi_ ? frame : hi_;
    }
    last_ = frame;
  }
  void flush() {
    if (open_) flushOpen();
  }

 private:
  static constexpr double kGap = 512;  // frames: a bigger step is a jump
  void flushOpen();
  Slot* slot_ = nullptr;
  double toSec_ = 0;
  double lo_ = 0, hi_ = 0, last_ = 0;
  bool open_ = false;
};

// A copyable run kept inside a voice that is processed one frame at a time
// (voices in the inner loop): flushes on a jump and every kEvery frames, so a
// voice that ends loses at most kEvery frames of coverage (a few ms).
struct Cover {
  static constexpr int kEvery = 256;
  double lo = 0, hi = 0, last = 0;
  int count = 0;
  bool open = false;
  void at(const Sample* s, double frame) {
    if (!active()) return;
    if (open && (frame - last > 512 || last - frame > 512)) flush(s);
    if (!open) {
      lo = hi = last = frame;
      open = true;
      count = 0;
      return;
    }
    lo = frame < lo ? frame : lo;
    hi = frame > hi ? frame : hi;
    last = frame;
    if (++count >= kEvery) flush(s);
  }
  void flush(const Sample* s) {
    if (!open) return;
    open = false;
    markFrames(s, lo, hi);
  }
};

// ---- the pass (UI / worker thread) ----

struct WatchFile {
  std::string path;
  double seconds = 0;  // length of the file (decoded)
};

struct Use {
  int file = -1;               // index into the watch list
  ObjectId owner = kNoId;
  int64_t plays = 0;           // starts counted with play()
  double tick0 = 0, tick1 = 0; // first / last song tick it was heard (tick1 < tick0: never)
  std::vector<Range> ranges;   // merged source ranges (seconds of the file)
  bool overflow = false;       // more ranges than kept: neighbours were merged
};

class Pass {
 public:
  static constexpr int kMaxPairs = 8192;
  static constexpr int kMaxRanges = 96;
  static constexpr double kGrain = 0.001;  // seconds per bitmap bit

  explicit Pass(std::vector<WatchFile> files);
  ~Pass();
  Pass(const Pass&) = delete;
  Pass& operator=(const Pass&) = delete;

  // Makes this the active pass (one at a time machine-wide; false when another is active).
  bool begin();
  void end();

  const std::vector<WatchFile>& files() const;
  // Exact union of everything read from file i (bitmap, kGrain resolution).
  std::vector<Range> fileRanges(int i) const;
  std::vector<Use> uses() const;
  bool pairsFull() const;

 private:
  std::unique_ptr<PassData> d_;
};

}  // namespace gl::tap
