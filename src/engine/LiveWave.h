// Gridline — the waveform of a recording in progress, drawn live (K-145).
//
// Shared by every view that records audio and shows it (Loupe, Seltmann's recorder):
//
//   LiveWave  the take as a min / max pyramid per channel: level 0 holds one pair per
//             kBase frames, each level above halves the count. add() appends any chunk
//             size (the result equals one add of the whole signal); truncate() drops the
//             tail back to a bucket boundary for a caller whose buffer changed after the
//             fact (a resume crossfade), which then adds again from there.
//   LiveFit   the view while recording: the take grows left to right at the view's normal
//             scale; once it reaches the right edge the span is the whole take, every
//             frame, so the whole recording is always in view. The drawn head follows the
//             recorded count at the speed frames arrive (audio arrives in blocks, frames are
//             drawn at the display rate), so it glides instead of stepping by blocks.
//   trace()   the outline to draw for a span and a width: the pyramid level with one to two
//             buckets per pixel (cost O(width), whatever the take's length), each bucket
//             blended toward its parent (value and place) by the fractional octave so a level change is
//             continuous. Buckets sit on a fixed frame grid, so a growing span squeezes the
//             shape smoothly instead of making it shimmer.
//
// UI thread only. The audio thread never touches it: the views feed it from the buffers
// their recorders already fill on the UI thread from lock-free rings.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gl {

class LiveWave {
 public:
  static constexpr size_t kBase = 128;  // frames per level-0 bucket
  static constexpr int kMaxLevels = 28;

  void reset(double rate, int channels);
  // n frames; R ignored for a mono wave (may be null).
  void add(const float* L, const float* R, size_t n);
  void addSilence(size_t n);
  // Keeps at most `frames` frames, cut back to a bucket boundary. Returns the frames kept:
  // the caller adds its audio again from there.
  size_t truncate(size_t frames);

  size_t frames() const { return frames_; }
  double rate() const { return rate_; }
  int channels() const { return channels_; }
  double seconds() const { return rate_ > 0 ? static_cast<double>(frames_) / rate_ : 0.0; }
  int levels() const { return static_cast<int>(lv_.size()); }
  size_t buckets(int level) const;
  static size_t bucketFrames(int level) { return kBase << level; }
  // Min / max of channel ch (0 or 1; a mono wave answers channel 0 for both) in bucket b
  // of a level, in -1..1.
  void bucket(int ch, int level, size_t b, float& mn, float& mx) const;
  // A bucket reaching full scale (|sample| >= 0.999) on that channel.
  bool clipped(int ch, int level, size_t b) const;
  // Bytes held (tests: an hour stays bounded).
  size_t memoryBytes() const;

 private:
  struct Level {
    std::vector<int16_t> mn[2], mx[2];
  };
  void push(float l, float r);
  void parents(size_t b0);  // recomputes the buckets above level-0 bucket b0's line
  static int16_t q(float v);
  std::vector<Level> lv_;
  size_t frames_ = 0;
  double rate_ = 44100;
  int channels_ = 2;
  size_t inBucket_ = 0;  // frames in the last level-0 bucket
};

// The view of a recording in progress.
struct LiveFit {
  double base = 0;   // frames the view shows across its width at its normal scale
  double shown = 0;  // the drawn head (frames)
  double rate = 44100;
  double vel = 0;    // frames a second arriving (smoothed)
  double last = 0;   // the recorded count at the previous step
  bool active = false;

  // A recording starts: the view's normal span, the rate, frames already there (append).
  void begin(double baseFrames, double sampleRate, double startFrames = 0);
  void end() { active = false; }
  // Each drawn frame: the frames recorded so far and the frame time (s).
  void step(double recorded, double dt);
  // Frames mapped onto `width` px, leaving `pad` px after the head once the take fills it.
  double span(float width, float pad) const;
};

// One point of a traced lane: x in px from the lane's left edge, lo / hi in -1..1.
struct LiveWavePoint {
  float x, lo, hi;
};
struct LiveWaveView {
  double start = 0;      // first frame at the left edge
  double span = 1;       // frames across the width
  double end = -1;       // draw up to this frame (the drawn head; < 0: everything)
  float width = 1;       // px
  float pxScale = 1;     // device pixels per px (framebuffer scale)
};
// ch: 0 / 1 a channel, -1 the mix (min / max over both), -2 the absolute peak of both
// (lo = -peak, hi = peak: a mirrored look). Returns the level used. `out` is reused.
int traceLiveWave(const LiveWave& w, int ch, const LiveWaveView& v, std::vector<LiveWavePoint>& out);

}  // namespace gl
