// Gridline - tracker modules (K-76): the tracker cells an imported note carries (Note::tracker).
//
// A note imported from a module is one voice of one tracker channel. Its stream holds what the
// Tracker Sampler needs to play that voice exactly as the tracker did:
//   - a header: format version, flags, the tracker channel ("lane"), the tick of the first row the
//     note starts on (note delay), and (for a voice that starts here) the channel state before
//     its first row: effect memories, volume, pan, vibrato positions;
//   - its rows, from the row it starts on: every row's cell as the module has it, its speed (ticks),
//     passes (pattern delay repeats) and tick rate (the tracker's BPM). When the song's tempo
//     changes on the note's first row, the row before it (its tick rate, speed and passes) too, so
//     the voice can follow a tempo change that falls inside an audio block. Empty rows are run-length coded. A row that starts the
//     next voice of the same tracker channel is not part of the note, except a delayed one: the
//     note then ends at that row's delay tick (an end mark), as the tracker cut it there.
// A continuation (`kTie`) starts on a pattern's first row and takes over the voice the same lane
// left sounding at the end of the previous pattern. `kHandOver`: the note ends at the end of its
// pattern while its voice keeps sounding, to be taken over by the next pattern's continuation.
//
// The reader works on a plain byte range with no allocation (audio thread).
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>
#include "TrackerChannel.h"

namespace gl::tracker {

inline constexpr uint8_t kStreamVersion = 1;
enum StreamFlag : uint8_t { kTie = 1, kHandOver = 2, kHasState = 4 };
inline constexpr size_t kStreamMax = 4096;  // the longest stream an instrument voice keeps

// How long a row plays, in units of 1/44000 s (the tracker counts whole such units per tick);
// 0 when the tick rate is not given.
inline int rowLength(int bpm, int speed, int passes) {
  if (bpm <= 0) return 0;
  return std::max(1, speed) * std::max(1, passes) * (110000 / std::clamp(bpm, 32, 255));
}

struct StreamHeader {
  uint8_t flags = 0;
  uint8_t lane = 0;       // tracker channel 0..31
  uint8_t startTick = 0;  // tick of row 0 the note starts on (0 unless a note delay)
  State state{};          // with kHasState
};

struct StreamRow {
  Cell cell;
  uint8_t speed = 6;   // ticks per pass
  uint8_t passes = 1;  // 1 + pattern delay
  uint8_t bpm = 0;     // the tracker's tick rate (32..255), 0 = not given
  int endTick = -1;    // >= 0: the voice ends at this tick of the row (end mark)
};

class StreamWriter {
 public:
  explicit StreamWriter(const StreamHeader& h) {
    out_.push_back(kStreamVersion);
    out_.push_back(h.flags);
    out_.push_back(h.lane);
    out_.push_back(h.startTick);
    if (h.flags & kHasState) {
      const auto* p = reinterpret_cast<const uint8_t*>(&h.state);
      out_.insert(out_.end(), p, p + sizeof(State));
    }
  }
  void row(const StreamRow& r) {
    if (r.speed != speed_ || r.passes != passes_) {
      flushEmpty();
      out_.push_back(0x01);
      out_.push_back(r.speed);
      out_.push_back(r.passes);
      speed_ = r.speed;
      passes_ = r.passes;
    }
    if (r.bpm != 0 && r.bpm != bpm_) {
      flushEmpty();
      out_.push_back(0x04);
      out_.push_back(r.bpm);
      bpm_ = r.bpm;
    }
    if (r.cell.empty()) {
      ++empty_;
      return;
    }
    flushEmpty();
    out_.push_back(0x02);
    out_.push_back(r.cell.note);
    out_.push_back(r.cell.instr);
    out_.push_back(r.cell.vol);
    out_.push_back(r.cell.efx);
    out_.push_back(r.cell.param);
  }
  // The song row before the first one (only before any row is written).
  void prior(uint8_t bpm, uint8_t speed, uint8_t passes) {
    out_.push_back(0x05);
    out_.push_back(bpm);
    out_.push_back(speed);
    out_.push_back(passes);
  }
  // The voice ends at tick `t` of the last row written (t >= its tick count: when that row ends).
  void end(uint8_t t) {
    flushEmpty();
    out_.push_back(0x03);
    out_.push_back(t);
  }
  std::vector<uint8_t> finish() {
    // Every row is there (empty ones too): a voice handed over knows where its rows end. Past
    // them a voice runs on with empty rows.
    flushEmpty();
    out_.push_back(0x00);
    return std::move(out_);
  }

 private:
  void flushEmpty() {
    while (empty_ > 0) {
      const int n = empty_ > 127 ? 127 : empty_;
      out_.push_back(static_cast<uint8_t>(0x80 | n));
      empty_ -= n;
    }
  }
  std::vector<uint8_t> out_;
  uint8_t speed_ = 0, passes_ = 0, bpm_ = 0;
  int empty_ = 0;
};

// Reads a stream row by row. Rows past the end are empty, with the last speed.
class StreamReader {
 public:
  bool open(const uint8_t* p, size_t n, StreamHeader& h) {
    p_ = p;
    n_ = n;
    pos_ = 0;
    pendingEmpty_ = 0;
    done_ = false;
    speed_ = 6;
    passes_ = 1;
    bpm_ = 0;
    prior_ = 0;
    if (!p || n < 4 || p[0] != kStreamVersion) return false;
    h.flags = p[1];
    h.lane = p[2];
    h.startTick = p[3];
    pos_ = 4;
    if (h.flags & kHasState) {
      if (n - pos_ < sizeof(State)) return false;
      std::memcpy(&h.state, p + pos_, sizeof(State));
      pos_ += sizeof(State);
    }
    return true;
  }
  StreamRow next() {
    StreamRow r = take();
    if (pendingEmpty_ == 0 && pos_ + 1 < n_ && p_[pos_] == 0x03) {
      r.endTick = p_[pos_ + 1];
      pos_ += 2;
    }
    return r;
  }
  bool done() const { return done_; }
  // The length of the song row before the first one (rowLength()), 0 = not given.
  int priorLength() const { return prior_; }

 private:
  StreamRow take() {
    StreamRow r;
    for (int guard = 0; guard < 4096; ++guard) {
      if (pendingEmpty_ > 0) {
        --pendingEmpty_;
        break;
      }
      if (pos_ >= n_) {
        done_ = true;
        break;
      }
      const uint8_t op = p_[pos_++];
      if (op == 0x00) {
        pos_ = n_;
        done_ = true;
        break;
      }
      if (op & 0x80) {
        pendingEmpty_ = op & 0x7F;
        continue;
      }
      if (op == 0x01) {
        if (n_ - pos_ < 2) {
          pos_ = n_;
          continue;
        }
        speed_ = p_[pos_] ? p_[pos_] : 1;
        passes_ = p_[pos_ + 1] ? p_[pos_ + 1] : 1;
        pos_ += 2;
        continue;
      }
      if (op == 0x04) {
        if (n_ - pos_ < 1) {
          pos_ = n_;
          continue;
        }
        bpm_ = p_[pos_++];
        continue;
      }
      if (op == 0x05) {
        if (n_ - pos_ < 3) {
          pos_ = n_;
          continue;
        }
        prior_ = rowLength(p_[pos_], p_[pos_ + 1], p_[pos_ + 2]);
        pos_ += 3;
        continue;
      }
      if (op == 0x02) {
        if (n_ - pos_ < 5) {
          pos_ = n_;
          continue;
        }
        r.cell.note = p_[pos_];
        r.cell.instr = p_[pos_ + 1];
        r.cell.vol = p_[pos_ + 2];
        r.cell.efx = p_[pos_ + 3];
        r.cell.param = p_[pos_ + 4];
        pos_ += 5;
        break;
      }
      if (op == 0x03) {  // an end mark not after a row: ignored
        pos_ = std::min(n_, pos_ + 1);
        continue;
      }
      pos_ = n_;  // unknown op: the rest is ignored
    }
    r.speed = speed_;
    r.passes = passes_;
    r.bpm = bpm_;
    return r;
  }
  const uint8_t* p_ = nullptr;
  size_t n_ = 0, pos_ = 0;
  int pendingEmpty_ = 0;
  uint8_t speed_ = 6, passes_ = 1, bpm_ = 0;
  int prior_ = 0;
  bool done_ = false;
};

}  // namespace gl::tracker
