// Gridline: streaming voice analysis for the Voice engine (J-144).
//
// What a pitch-synchronous engine needs to know about a voice, decided a
// little ahead of where it is played:
//   - pitch and voicing per analysis frame (5.8 ms). Each frame gives a few
//     period candidates (the dips of YIN's cumulative mean normalised
//     difference, de Cheveigne & Kawahara 2002) on an anti-aliased 4x
//     decimated mix; a fixed-lag Viterbi search over the candidates and an
//     unvoiced state picks the path (as in pYIN, Mauch & Dixon 2014): an
//     octave jump or a voicing switch has to be paid for, so the track does
//     not flicker between voiced and unvoiced or jump an octave on one frame;
//   - pitch marks, one per period: each sits on the period's excitation (the
//     burst of high-frequency energy at glottal closure) and keeps the shape
//     of the previous period (cross-correlation), with a sub-sample position.
//     Voiced runs get a lead and a tail mark so the engine can fade in and out;
//   - how close each period is to a pure tone (no formants to keep), smoothed
//     over the run.
// configure() allocates; push() and every query are realtime safe.
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace gl::pitch {

class VoiceTrack {
 public:
  struct Mark {
    double pos = 0;     // input frame of the excitation (fractional)
    double period = 0;  // frames since the previous mark (the tracked period at a run's first mark)
    float voiced = 0;   // 1 inside a voiced run, 0 for the lead / tail marks around it
    float tonal = 0;    // 0 a voice (keep its formants) .. 1 a pure tone (transpose it)
  };

  // capacity: input frames of history the queries may reach back.
  void configure(double sampleRate, int64_t capacity);
  void reset();
  // One frame of the analysis mix (mono).
  void push(float x) {
    mono_[static_cast<size_t>(written_ & mask_)] = x;
    // Excitation signal: squared first difference, mean over exLen_ frames
    // (stored at the window's centre).
    const float d = x - last_;
    last_ = x;
    const float e = d * d;
    exSum_ += static_cast<double>(e) - exHist_[static_cast<size_t>(written_ % exLen_)];
    exHist_[static_cast<size_t>(written_ % exLen_)] = e;
    if (written_ >= exLen_ / 2) ex_[static_cast<size_t>((written_ - exLen_ / 2) & mask_)] = static_cast<float>(std::max(0.0, exSum_) / exLen_);
    ++written_;
    if (written_ >= nextStep_) step();
  }
  int64_t written() const { return written_; }
  // Frames from the newest pushed frame back to where the marks are final.
  int latency() const { return latency_; }
  int maxPeriod() const { return tMax_; }
  int minPeriod() const { return tMin_; }
  double sampleRate() const { return sr_; }

  // Decided voicing at input frame pos (unvoiced past what is decided).
  bool voicedAt(double pos) const;
  // Period (input frames) of the newest decided voiced frame no more than
  // `frames` before the newest decision, 0 when there is none.
  double recentPeriod(int frames) const;
  // Frames whose voicing is decided ([0, decidedTo)).
  int64_t decidedTo() const { return decided_ * hop_ + hop_ / 2; }

  int64_t markCount() const { return marksDone_; }
  int64_t firstMark() const { return marksDone_ > markMask_ ? marksDone_ - markMask_ : 0; }
  const Mark& mark(int64_t i) const { return marks_[static_cast<size_t>(i & markMask_)]; }
  // Index of the mark nearest to pos, -1 when there is none.
  int64_t markNear(double pos) const;

  float monoAt(int64_t i) const {
    return i < 0 || i >= written_ || i <= written_ - cap_ ? 0.0f : mono_[static_cast<size_t>(i & mask_)];
  }

 private:
  struct Cand {
    float tau = 0;  // period, input frames
    float d = 1;    // CMNDF value
  };
  static constexpr int kCands = 4;
  static constexpr int kLag = 8;  // Viterbi decision lag (frames)
  struct Frame {
    Cand c[kCands];
    int n = 0;
    float cost[kCands + 1] = {};
    int8_t back[kCands + 1] = {};
  };

  void step();
  void decimate();
  void analyseFrame();
  void viterbi();
  void makeMarks();
  bool frameVoiced(int64_t k) const;
  double framePeriod(int64_t k) const;
  double periodAt(double pos) const;
  float decAt(int64_t j) const {
    return j < 0 || j >= decDone_ || j <= decDone_ - decCap_ ? 0.0f : dec_[static_cast<size_t>(j & decMask_)];
  }
  float exAt(int64_t i) const {
    return i < 0 || i >= written_ - exLen_ / 2 || i <= written_ - cap_ ? 0.0f : ex_[static_cast<size_t>(i & mask_)];
  }
  double ncc(int64_t a, int64_t b, int h, int stride) const;
  float tonalAt(int64_t pos, double period) const;
  void emit(const Mark& m);

  double sr_ = 44100;
  int D_ = 4;                   // decimation factor
  int tMin_ = 40, tMax_ = 882;  // period range, input frames (1100 Hz .. 50 Hz)
  int hop_ = 256;               // analysis hop, input frames
  int W_ = 276;                 // CMNDF window, decimated frames
  int tauMax_ = 222, tauMin_ = 10;
  int latency_ = 0;
  int exLen_ = 26;
  int64_t cap_ = 0, mask_ = 0;
  std::vector<float> mono_, ex_, exHist_;
  double exSum_ = 0;
  float last_ = 0;
  int64_t written_ = 0, nextStep_ = 0;
  // Decimation (windowed-sinc low-pass, then every D_-th frame).
  std::vector<float> fir_;
  int firHalf_ = 0;
  std::vector<float> dec_;
  int64_t decCap_ = 0, decMask_ = 0, decDone_ = 0;
  std::vector<double> dif_;  // CMNDF scratch
  // Frames: candidates and Viterbi state (ring), decisions (ring).
  std::vector<Frame> frames_;
  int64_t frameMask_ = 0, framesDone_ = 0;
  std::vector<float> decPeriod_;  // decided period per frame (0 = unvoiced)
  int64_t decidedMask_ = 0, decided_ = 0;
  // Marks.
  std::vector<Mark> marks_;
  int64_t markMask_ = 0, marksDone_ = 0;
  int state_ = 0;     // 0 between runs, 1 in a voiced run, 2 adding a run's tail marks
  int64_t scan_ = 0;  // next decided frame to look for a voiced run in
  float runTonal_ = 0;
  double runEnd_ = 0;
  int tails_ = 0;
};

}  // namespace gl::pitch
