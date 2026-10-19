// Gridline — "utility" pack: the Scope's reference display engine (ScEngine =
// 1). Analysis that runs on the UI thread from the scope's history ring:
// meter detectors and ballistics per meter type, the reassigned spectrum with
// a linear↔log frequency axis and pink weighting. No ImGui here (testable).
#pragma once
#include <cstdint>
#include <vector>
#include "utility_rt.h"

namespace gl::packs::utility {

// Bar position (0..1) of a linear amplitude (1 = 0 dBFS, the bar ends at +6 dB)
// for the meter curve c (0..1): k = (6001^c − 1) / 6, pos = ln(1 + k·a/2) / ln(1 + k).
double candyMeterPos(double amp, double curve);
// Inverse of candyMeterPos.
double candyMeterAmp(double pos, double curve);

// Level detector + display ballistics of one meter (two channels).
// Analysis runs in 20 ms blocks; the bar falls linearly by the full scale in
// fallMs, the hold marker keeps the maximum for holdMs (0 = no hold,
// infinite = never falls).
class CandyMeter {
 public:
  struct Config {
    int type = SmtPeak;
    double sr = 48000;
    double curve = 0.5;
    double holdMs = 2500;
    bool holdInfinite = false;
    double fallMs = 750;
  };
  void configure(const Config& c);
  void reset();
  // Feeds n new frames (any count; blocks are formed internally).
  void feed(const float* L, const float* R, size_t n);
  // Detector amplitude of the last analysis block (linear; 1 = full-scale sine
  // peak for Peak / PPM, the sine's peak equivalent for RMS / VU / Leq, and
  // 10^(LUFS/20) for loudness).
  double level(int ch) const { return level_[ch]; }
  double pos(int ch) const { return pos_[ch]; }    // displayed bar position 0..1
  double hold(int ch) const { return hold_[ch]; }  // hold marker position 0..1 (= pos when there is no hold)
  int blockFrames() const { return block_; }

 private:
  void endBlock();
  Config c_;
  int block_ = 960, fill_ = 0;
  double blockPeak_[2] = {}, env_[2] = {}, vu1_[2] = {}, vu2_[2] = {}, ms_[2] = {}, leq_[2] = {};
  double aw_[2][3][2] = {};  // A-weighting: three biquad sections (z1, z2) per channel
  double awB_[3][3] = {}, awA_[3][2] = {}, awGain_ = 1;
  double level_[2] = {}, pos_[2] = {}, hold_[2] = {}, holdLeft_[2] = {};
  LoudnessMeter lufs_;
  double designedSr_ = 0;
};

// Frequency (Hz) of the axis position u (0..1) for the frequency curve c
// (0 linear, 1 logarithmic) between 20 Hz and the Nyquist frequency.
double candyAxisHz(double u, double curve, double nyquist);

// Spectrum of the reference engine: FFT of 2N points (Hann), optional
// time-frequency reassignment of every bin ("sharpen"), pink weighting.
class CandySpectrum {
 public:
  void init(int fftSize);  // allocates (UI thread)
  int size() const { return n_; }
  // Analyses the newest fftSize frames of mono `x` (length fftSize). Results:
  // per bin dB (re a full-scale sine) and its (reassigned) frequency.
  void analyse(const float* x, double sr, bool sharpen);
  const std::vector<float>& binDb() const { return db_; }
  const std::vector<float>& binHz() const { return hz_; }
  // Display columns (dB, pink weighting and gain applied) over the curve axis.
  void columns(int cols, double curve, bool pink, double gainDb, double sr, std::vector<float>& out) const;
  // Frequency of the strongest bin (reassigned when analysed with sharpen).
  double peakHz() const;

 private:
  int n_ = 0;
  Fft fft_;
  std::vector<float> win_, dwin_, re_, im_, re2_, im2_, db_, hz_, plain_;
  float norm_ = 1;
};

}  // namespace gl::packs::utility
