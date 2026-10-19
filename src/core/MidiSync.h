// Gridline — MIDI synchronisation (J-17): MIDI clock / song position / MTC
// generation at sample-accurate positions (audio thread, allocation-free),
// and following an external MIDI clock (tempo + start/stop/continue/SPP).
#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace gl {

// A timestamped outgoing MIDI message (up to a 10-byte MTC full-frame sysex).
struct MidiOutEvent {
  int64_t sample = 0;  // absolute engine sample (AudioEngine sample counter)
  uint8_t size = 0;
  uint8_t data[10] = {};
};

// Single-producer / single-consumer ring of MidiOutEvents (audio → MIDI thread).
class MidiEventRing {
 public:
  static constexpr size_t kSize = 2048;
  bool push(const MidiOutEvent& e) {
    size_t h = head_.load(std::memory_order_relaxed);
    if (h - tail_.load(std::memory_order_acquire) >= kSize) return false;
    buf_[h % kSize] = e;
    head_.store(h + 1, std::memory_order_release);
    return true;
  }
  bool pop(MidiOutEvent& e) {
    size_t t = tail_.load(std::memory_order_relaxed);
    if (t == head_.load(std::memory_order_acquire)) return false;
    e = buf_[t % kSize];
    tail_.store(t + 1, std::memory_order_release);
    return true;
  }
  bool peek(MidiOutEvent& e) const {
    size_t t = tail_.load(std::memory_order_relaxed);
    if (t == head_.load(std::memory_order_acquire)) return false;
    e = buf_[t % kSize];
    return true;
  }

 private:
  MidiOutEvent buf_[kSize];
  std::atomic<size_t> head_{0}, tail_{0};
};

enum class SyncOut : uint8_t { Off = 0, Clock, Mtc24, Mtc25, Mtc2997Drop, Mtc30, Count };
const char* syncOutName(SyncOut s);

// SMPTE time for MTC (hours, minutes, seconds, frames) from a frame count.
struct Timecode {
  int h = 0, m = 0, s = 0, f = 0;
};
// Frames per second of an MTC mode (29.97 for drop-frame), and its nominal label rate.
double mtcFrameRate(SyncOut s);
int mtcNominalFps(SyncOut s);
int mtcRateCode(SyncOut s);  // 0 = 24, 1 = 25, 2 = 29.97 drop, 3 = 30
// Timecode of frame number `frame` (drop-frame labelling for 29.97 drop).
Timecode mtcTimecode(SyncOut s, int64_t frame);

// Generates sync messages for transport segments. Positions are ticks at
// `ppq`; clocks are sent 24 per quarter note. Call from the audio thread.
class MidiClockOut {
 public:
  void setMode(SyncOut m) { mode_ = m; }
  SyncOut mode() const { return mode_; }
  void setPpq(int ppq) { ppq_ = ppq > 0 ? ppq : 96; }
  void setSampleRate(double sr) { sr_ = sr > 0 ? sr : 44100.0; }

  // The transport started at `tick` (`seconds` = its time), at sample `at`.
  void start(double tick, double seconds, int64_t at);
  // The transport stopped / paused at sample `at`.
  void stop(int64_t at);
  bool running() const { return running_; }

  // A contiguous stretch of playback: ticks [t0, t1), seconds [sec0, sec1),
  // `len` samples starting at absolute sample `at`. `sampleOf(tick)` returns
  // the sample offset of a tick from the segment start (fractional). A jump
  // from where the previous segment ended (loop wrap, locate) re-sends the
  // position first.
  template <class SampleOf>
  void segment(double t0, double t1, double sec0, double sec1, int64_t at, int len, SampleOf sampleOf) {
    if (mode_ == SyncOut::Off || len <= 0) return;
    if (!running_) return;
    if (pendingStart_) {
      pendingStart_ = false;
      announce(t0, sec0, at, true);
    } else if (std::fabs(t0 - nextTick_) > 1e-6) {
      announce(t0, sec0, at, false);
    }
    nextTick_ = t1;
    if (mode_ == SyncOut::Clock) {
      const double q = ppq_ / 24.0;
      for (int64_t k = static_cast<int64_t>(std::ceil(t0 / q - 1e-9)); static_cast<double>(k) * q < t1 - 1e-9; ++k) {
        int64_t off = static_cast<int64_t>(std::ceil(sampleOf(static_cast<double>(k) * q) - 1e-9));
        if (off < 0 || off >= len) continue;
        emit(at + off, 0xF8);
      }
    } else {
      const double r = mtcFrameRate(mode_) * 4.0;  // quarter frames per second
      if (sec1 <= sec0) return;
      for (int64_t j = static_cast<int64_t>(std::ceil(sec0 * r - 1e-9)); static_cast<double>(j) / r < sec1 - 1e-12; ++j) {
        double off = (static_cast<double>(j) / r - sec0) / (sec1 - sec0) * len;
        int64_t o = static_cast<int64_t>(std::ceil(off - 1e-9));
        if (o < 0 || o >= len) continue;
        quarterFrame(at + o, j);
      }
    }
  }

  // Events produced so far (consumed by the caller).
  MidiEventRing* out = nullptr;
  int dropped() const { return dropped_; }

 private:
  void emit(int64_t at, uint8_t a, int size = 1, uint8_t b = 0, uint8_t c = 0);
  void announce(double tick, double sec, int64_t at, bool fresh);
  void quarterFrame(int64_t at, int64_t index);
  void fullFrame(double sec, int64_t at);
  SyncOut mode_ = SyncOut::Off;
  int ppq_ = 96;
  double sr_ = 44100.0;
  bool running_ = false, pendingStart_ = false;
  double startTick_ = 0, startSec_ = 0, nextTick_ = 0;
  int dropped_ = 0;
};

// Follows an incoming MIDI clock: tempo from the clock interval, transport
// commands from start / continue / stop / song position pointer.
class MidiClockFollower {
 public:
  struct Cmd {
    enum Kind { Start, Continue, Stop, Locate } kind = Start;
    double sixteenths = 0;  // Locate: song position in 16th notes
  };
  // A system real-time / common message received at `timeSec` (seconds, any epoch).
  void message(uint8_t status, uint8_t d1, uint8_t d2, double timeSec);
  bool hasTempo() const { return count_ >= 7; }
  double bpm() const { return bpm_; }
  bool running() const { return running_; }
  // Clocks counted since start / continue (24 per quarter note).
  int64_t clocks() const { return clocks_; }
  std::vector<Cmd> takeCommands();
  void reset();

 private:
  static constexpr int kWindow = 24;
  double times_[kWindow + 1] = {};
  int count_ = 0, head_ = 0;
  double bpm_ = 0;
  bool running_ = false;
  int64_t clocks_ = 0;
  std::vector<Cmd> cmds_;
};

}  // namespace gl
