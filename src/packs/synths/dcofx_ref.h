// Gridline — "synths" pack, J-33: DCO Synth reference-model master effects
// (DcModel = 1), the reference instrument's blocks as analysed:
//   RefEnsemble   4 / 8 lines from fixed per-line min / max tables (delay,
//                 rate, move rate), parabolic-sine LFOs, depth relative to the
//                 line's own delay, equal-power input movement between left and
//                 right, per-line self feedback, cubic Hermite reads, −5 / −11 dB
//                 line sum, RBJ low / high pass (Q 0.5, high pass off at 0 with a
//                 10 ms crossfade), equal-power mix
//   RefDelay      two read heads with a 1000-sample crossfade on time changes,
//                 mono feedback from the left tap (10 Hz high pass → feedback
//                 filter, which the input also passes → soft clip → limiter →
//                 65-sample delay), blur lines, tone tilt, stereo cross matrix
//   RefSaturation Padé tanh normalised by the drive (ADAA tanh in HQ)
// The reverb is Hex Synth's plate engine (hex_fx.h) driven with this
// instrument's laws. Every buffer is sized at configure(), never in process().
#pragma once
#include <array>
#include <cstdint>
#include <vector>

namespace gl::packs::synths::dcofx {

class RefEnsemble {
 public:
  void configure(double sr);  // allocates
  void clear();
  // 0..1 control positions.
  void set(double delay, double fb, double rate, double depth, double move, double moveRate, bool eight, double lp,
           double hp, double mix, double phase);
  void process(float* L, float* R, int n);

 private:
  struct Line {
    std::vector<float> l, r;
    double D = 0, dep = 0, inc = 0, minc = 0;
    double ph = 0, mph = 0;
    float yl = 0, yr = 0;
  };
  struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1[2] = {0, 0}, z2[2] = {0, 0};
    double run(double x, int c) {
      const double y = b0 * x + z1[c];
      z1[c] = b1 * x - a1 * y + z2[c];
      z2[c] = b2 * x - a2 * y;
      return y;
    }
  };
  double sr_ = 44100;
  std::array<Line, 8> lines_;
  int w_ = 0;
  bool eight_ = false;
  double fb_ = 0, mv_ = 0, off_ = 0, dry_ = 1, wet_ = 0;
  Biquad lp_, hp_;
  bool hpOn_ = false;
  int hpFade_ = 0, hpFadeLen_ = 441;  // counter 0..len: HP share
  double lastLp_ = -1, lastHp_ = -1;
};

class RefDelay {
 public:
  void configure(double sr);  // allocates
  void clear();
  // `seconds`: the delay time; the other values are 0..1 positions.
  void set(double input, double seconds, double rightTap, double fb, double fbCut, int fbType, double stereo,
           double blur, double tone);
  void process(float* L, float* R, int n);

 private:
  static constexpr int kLen = 262144;
  float rd(const std::vector<float>& b, double d) const;
  void updateFilters();
  double sr_ = 44100;
  std::vector<float> main_, blur_[4];
  std::array<float, 65> pre_{};
  int pi_ = 0, w_ = 0;
  // Heads.
  double T_ = 0.25, r_ = 0;
  double dl_[2] = {1, 1}, dr_[2] = {1, 1}, g_[2] = {1, 0};
  double hT_[2] = {-1, -1}, hR_[2] = {-1, -1};
  int active_ = 0, fade_ = 0, poll_ = 0;
  bool first_ = true;
  // Parameters.
  double in_ = 0, fb_ = 0, cut_ = 0, s1_ = 0.5, s2_ = 0.5, bl_ = 0, tone_ = 0.5;
  int type_ = 0;
  // Feedback chain state.
  double hpG_ = 0, hpS_ = 0, env_ = 0;
  double svfF_ = 0, svfQ_ = 2, low_ = 0, band_ = 0;
  double bb0_ = 0, bb1_ = 0, bb2_ = 0, ba1_ = 0, ba2_ = 0, bz1_ = 0, bz2_ = 0;
  bool bpReady_ = false;
  double lastCut_ = -1;
  int lastType_ = -1;
  // Tone.
  double toneG_ = 1, toneS_[2] = {0, 0};
  bool toneHp_ = false;
  double lastTone_ = -1;
};

class RefSaturation {
 public:
  void configure(double sr);
  void clear();
  void set(double amount, bool hq);
  void process(float* L, float* R, int n);

 private:
  double k_ = 0.0005, g_ = 0, target_ = 0;
  bool hq_ = false;
  double prev_[2] = {0, 0};
  bool started_ = false;
};

}  // namespace gl::packs::synths::dcofx
