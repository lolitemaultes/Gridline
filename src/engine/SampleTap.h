// Gridline - J-219: sample coverage tap (Track Receipt).
//
// While a tap Pass is bound on a thread (an offline receipt render binds it on
// every thread that renders for it: K-322), every voice that plays an audio
// file there reports which source frames it reads. The pass keeps,
// per watched file, an exact bitmap of the file's milliseconds that were read
// and, per (file, owner) pair (owner = the channel or audio clip that played
// it), the merged source ranges, how often it started and where in the song.
//
// Audio thread: no allocation, no locks. Everything is preallocated when the
// Pass is built (UI / worker thread); the audio side claims pair slots with a
// compare-and-swap and sets bits with fetch_or. When no pass is bound on the
// thread, every call is a single thread-local load and returns at once.
//
// K-322: a pass is bound per thread, never process-wide. The live audio engine
// binds none, so playing the project while a receipt measures it never marks
// into the receipt; several passes may run at once.
//
// Voices use it like this (per render block):
//   tap::Run run(sample);          // finds the slot of (sample, current owner)
//   for (...) { run.at(framePos); ... }   // the source frame read for this output frame
//   // (flushes when it goes out of scope; jumps start a new range)
// and at a note start: tap::play(sample) (and tap::pitch when the voice knows its pitch).
//
// K-321: besides the ranges, each pair counts the output seconds it sounded while reading
// (outSeconds: every voice and every repeat counts), the jumps inside a voice (chops) and the
// pitch / time ratio its voices reported. A run's first range that ends in a jump is flagged as
// a cue block (a deck cueing, a slice retrigger: what a voice reads in its block before it jumps
// to where it plays); a flagged range that touches another read loses the flag. The receipt
// leaves flagged ranges out when they are shorter than receipt::kMinHeard and keeps every other
// short read (a stutter, grains far apart).
// With Pass::Options::segments the pass also logs WHEN in the render's output each read
// happened: per pair, segments [outA, outB) -> [srcA, srcB] (seconds; srcB < srcA: read
// backwards), one per run; a run that continues the previous one at the same speed extends it.
// The output clock is the engine's (OwnerScope): a segment is exact to one render block.
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
  bool cue = false;     // K-321: only a cue block read it (see above)
};

// K-321: a stretch of the render's output and the source seconds read meanwhile.
struct Segment {
  double outA = 0, outB = 0;  // seconds of the render's output (engine frames / rate), [outA, outB)
  double srcA = 0, srcB = 0;  // seconds of the file, read from srcA to srcB (srcB < srcA: backwards)
};

struct Slot;
struct PassData;

class Pass;

// True while a pass is bound on this thread (tap::Bind).
bool active();

// The slot of (sample's file, the current owner); nullptr when no pass is
// active or the sample's file is not watched.
Slot* slotFor(const Sample* s);
// Counts a start (a note, a clip) of `s` for the current owner.
void play(const Sample* s);
void play(Slot* slot);
// Marks seconds [a, b) of the slot's file as read.
void mark(Slot* slot, double a, double b);
// K-321: the same, for code that knows the output frames of the block it covers: frames
// [outFrom, outTo) of this block (OwnerScope's clock) played the file from a to b
// (reverse: from b down to a). Counts the play seconds and logs the segment.
void mark(Slot* slot, double a, double b, int outFrom, int outTo, bool reverse = false);
// K-321: frames from the engine block's start to where the current render call starts (an
// instrument that renders its block in pieces between events sets it before each piece; the
// owner scope starts it at 0). Runs begun and marks made after it count from there.
void blockOffset(int frames);
// K-321: a voice of this pair plays at `semis` semitones from the file's own pitch and at
// `timeRatio` source seconds per output second (<= 0: not known; the segments tell). Called at
// a note start by voices that know both (a stretch engine changes one without the other).
void pitch(Slot* slot, double semis, double timeRatio);
void pitch(const Sample* s, double semis, double timeRatio);
// Marks the share [f0, f1) (0..1 of the file's length) as read: for plugins
// that play a processed copy whose positions map to the file by share.
void markShare(Slot* slot, double f0, double f1);
// Marks source frames f0..f1 (inclusive, fractional) of a sample as read.
void markFrames(const Sample* s, double f0, double f1);
// K-319: how many reads the pass bound on this thread has marked (0 when none is
// bound). A receipt render's tail runs while this keeps growing.
uint64_t marks();
// K-319: true while the bound pass only measures (nobody hears its sound): audio clips, whose
// reads are known before their sound is made, mark them and make no sound (no stretch render,
// no stretch voice). Off unless the pass asks for it (Pass::setReadsOnly).
bool readsOnly();

// K-322: binds `pass` on this thread for the guard's life (nullptr: nothing
// changes). Renderers bind their job's pass on the thread that renders and the
// engine binds it around every graph task its workers run; nested guards
// restore the outer binding.
class Bind {
 public:
  explicit Bind(const Pass* pass);
  ~Bind();
  Bind(const Bind&) = delete;
  Bind& operator=(const Bind&) = delete;

 private:
  PassData* prev_ = nullptr;
  bool on_ = false;
};

// The owner (Channel::id or Clip::id) and song tick of the code running on
// this thread. The engine sets it around each channel's processing and each
// audio clip; nested scopes restore the outer owner. K-321: the engine's scopes also set the
// output clock (the engine frame the block starts at, the engine's rate); a nested scope
// without them keeps the outer clock.
class OwnerScope {
 public:
  OwnerScope(ObjectId owner, double tick);
  OwnerScope(ObjectId owner, double tick, int64_t outFrame, double rate);
  ~OwnerScope();
  OwnerScope(const OwnerScope&) = delete;
  OwnerScope& operator=(const OwnerScope&) = delete;

 private:
  ObjectId prevOwner_;
  double prevTick_;
  int64_t prevOut_ = 0;
  int prevSub_ = 0;
  double prevRate_ = 0;
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
    ++n_;
    if (!open_) {
      lo_ = hi_ = last_ = first_ = frame;
      i0_ = n_ - 1;
      open_ = true;
      return;
    }
    const double d = frame - last_;
    if (d > kGap || d < -kGap) {
      flushOpen(true);
      lo_ = hi_ = first_ = frame;
      i0_ = n_ - 1;
      open_ = true;
    } else {
      lo_ = frame < lo_ ? frame : lo_;
      hi_ = frame > hi_ ? frame : hi_;
    }
    last_ = frame;
  }
  void flush() {
    if (open_) flushOpen(false);
  }

 private:
  static constexpr double kGap = 512;  // frames: a bigger step is a jump
  void flushOpen(bool jump);
  Slot* slot_ = nullptr;
  double toSec_ = 0;
  double lo_ = 0, hi_ = 0, last_ = 0, first_ = 0;
  int64_t n_ = 0, i0_ = 0;  // K-321: at() calls since begin (output frames), the open range's first one
  int flushed_ = 0;         // K-321: ranges flushed since begin (the first may be a cue block)
  int64_t base_ = 0;        // K-321: the output frame of the first at() (the clock at begin)
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
    if (open && (frame - last > 512 || last - frame > 512)) flush(s, true);
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
  void flush(const Sample* s, bool jump = false) {
    if (!open) return;
    open = false;
    covered(s, lo, hi, count + 1, jump);
  }
  // K-321: frames lo..hi read over `frames` output frames (jump: a jump ended them).
  static void covered(const Sample* s, double lo, double hi, int frames, bool jump);
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
  // More ranges than kept: neighbours were merged. K-321: the merged ranges are then cut back to
  // what the file's exact bitmap says was read, so no gap between them counts.
  bool overflow = false;
  // K-321
  double outSeconds = 0;              // output seconds its voices sounded while reading (each voice, each repeat)
  int64_t jumps = 0;                  // jumps inside a voice (chops)
  int64_t pitched = 0;                // notes that reported their pitch (tap::pitch)
  double semisMin = 0, semisMax = 0;  // the pitch they reported (semitones; when pitched > 0)
  double ratioMin = 0, ratioMax = 0;  // the time ratio they reported (0: none did)
  std::vector<Segment> segments;      // in output order (Pass::Options::segments)
};

class Pass {
 public:
  static constexpr int kMaxPairs = 8192;
  static constexpr int kMaxRanges = 96;
  static constexpr double kGrain = 0.001;  // seconds per bitmap bit

  struct Options {
    bool segments = false;           // K-321: log the segments (Use::segments)
    size_t segmentCapacity = 65536;  // segments a pass keeps; past it segmentsFull()
  };
  explicit Pass(std::vector<WatchFile> files);
  Pass(std::vector<WatchFile> files, const Options& o);
  ~Pass();
  Pass(const Pass&) = delete;
  Pass& operator=(const Pass&) = delete;

  // K-322: a pass is no longer made active machine-wide: renders bind it on their
  // own threads (RenderJob::tap, RenderOptions::tap, AudioEngine::setTapPass, tap::Bind).
  void setReadsOnly(bool on);  // K-319 (see tap::readsOnly)
  uint64_t marks() const;      // reads marked so far

  const std::vector<WatchFile>& files() const;
  // Exact union of everything read from file i (bitmap, kGrain resolution).
  std::vector<Range> fileRanges(int i) const;
  std::vector<Use> uses() const;
  bool pairsFull() const;
  bool segmentsFull() const;  // K-321: the segment log ran out of room (ranges and seconds stay complete)

 private:
  friend class Bind;
  std::unique_ptr<PassData> d_;
};

}  // namespace gl::tap
