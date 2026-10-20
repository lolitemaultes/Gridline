// Gridline — audio logger (J-31): keeps the last minutes of audio in a ring
// so a good moment can be saved after it happened ("save the last N minutes").
//
// The ring is allocated on the UI thread (constructor); the audio thread only
// writes into it (no allocation, no locks). The UI thread copies the most
// recent frames out while the audio thread keeps writing: the copy is taken
// from a snapshot of the write position and the ring keeps one extra second
// of headroom, so frames being copied are not overwritten mid-copy.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gl {

class AudioLogger {
 public:
  // What the logger hears.
  enum class Source : uint8_t {
    ArmedInputs = 0,  // the inputs of the record-armed mixer tracks, before their effects
    Master,           // the master output, after every effect
    Count
  };

  AudioLogger(double sampleRate, double seconds, Source source = Source::ArmedInputs);

  // ---- audio thread ----
  void write(const float* L, const float* R, int n);
  Source source() const { return source_; }

  // ---- UI thread ----
  double sampleRate() const { return sr_; }
  double seconds() const { return seconds_; }
  // Frames written so far (all time) / frames available for copying.
  int64_t written() const { return static_cast<int64_t>(pos_.load(std::memory_order_acquire)); }
  int64_t available() const;
  // Copies the last `frames` frames (at most available()), oldest first.
  // Returns the number of frames copied.
  int64_t copyLast(int64_t frames, std::vector<float>& L, std::vector<float>& R) const;
  // Peak level of the last `frames` frames (silence detection).
  float peakLast(int64_t frames) const;

 private:
  double sr_, seconds_;
  Source source_;
  size_t keep_ = 0;  // frames that can be copied
  std::vector<float> L_, R_;  // keep_ + one second of headroom
  std::atomic<size_t> pos_{0};
};

}  // namespace gl
