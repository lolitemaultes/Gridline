// Gridline — "synths" pack, J-33 group "hex": Hex Synth's post-voice chain —
// drive + DC blocker, two effect slots (delay, chorus, plate reverb, flanger,
// phaser, lo-fi) and the 8-band EQ — following the reference instrument's
// algorithms (see hexfx_dsp.cpp). Buffers are sized in configure() (UI-free,
// sample-rate changes only); process() never allocates.
#pragma once
#include <cstdint>
#include <memory>
#include <vector>
#include "hex.h"

namespace gl::packs::synths::hex {

// Clocked ("bucket brigade") delay line: the write head steps through a cycle
// by s per sample, each written sample keeps its position; the output is the
// previous cycle read at the current position (delay = 1 / s samples), so
// changing s (an LFO, a new time) bends the pitch like a tape / BBD line.
class BbdLine {
 public:
  // Allocates room for lines up to n samples (sample-rate changes only).
  void reserve(size_t n);
  // Sets the line length (within the reserved room) and start position; never allocates.
  void configure(size_t n, double startPos);
  void reset();
  void setTime(double t);  // 0..0.99 of the line length
  void setFeedback(float fb) { fb_ = fb; }
  void setMix(float m) { m_ = m; }
  void setRate(double hz, double sr) { inc_ = (hz > 0 ? hz : 0) / sr; }
  // Feedback taken through a one-pole (coefficient 0.5) of the read value (resonator lines).
  void setLoopLowpass(bool on) { lpOn_ = on; }
  void setMixNow(float m) { m_ = mSmooth_ = m; }
  void setDepth(double d) { depth_ = static_cast<float>(d < 0 ? 0 : (d > 1 ? 1 : d)); }
  float tick(float in);

 private:
  struct Cycle {
    std::vector<float> s;   // samples, entry k at index k + kPre
    std::vector<double> p;  // positions
    int count = 0, cursor = 0;
  };
  static constexpr int kPre = 10;
  size_t n_ = 4;
  double start_ = 0, pos_ = 0, phi_ = 0, inc_ = 0;
  double s0_ = 0, sSmooth_ = 0, sMin_ = 0.25;
  float fb_ = 0, m_ = 1, mSmooth_ = 0, depth_ = 0, lpS_ = 0;
  bool lpOn_ = false;
  Cycle cyc_[2];
  int write_ = 0;  // index of the cycle being written; the other is read
};

// Plate reverb engine (hexreverb_dsp.cpp).
class PlateReverb {
 public:
  PlateReverb();
  ~PlateReverb();
  void configure(double sr);
  void reset();
  void setHighCut(double v);  // 0..1 engine positions (see the laws there)
  void setDecay(double v);
  void setDamping(double v);
  void setWet(double v);
  // Any engine control by its index (0 input, 1 low cut, 2 high cut, 3 predelay,
  // 4 size, 5 diffusion, 6 bass, 7 crossover, 8 decay, 9 damping, 10 dry,
  // 11 early, 12 tail, 13 width, 14..17 mutes), 0..1 position.
  void setParam(int idx, double v);
  // Predelay feedback (0..0.9): the predelay line recirculates (DCO Synth's build).
  void setPredelayFeedback(double fb);
  // The zone sampler's earlier build of the engine (J-33): continuous controls
  // recomputed per call, no early reflections / dry / width / tail smoothing,
  // all four diffusers with one gain, fixed 94.5 Hz low cut and 22 kHz input
  // high cut; process() then writes the wet tail only. room / damping /
  // diffusion / decay are the 0..1 program controls.
  void setSamplerMode(double room, double damping, double diffusion, double decay);
  // Adds early reflections + tail to the dry input in place.
  void process(float* L, float* R, int n);

 private:
  struct Impl;
  std::unique_ptr<Impl> d_;
};

// One effect slot.
class FxSlot {
 public:
  void configure(double sr);
  void setType(int type);  // rebuilds on change
  void setEnabled(bool on);
  void setParams(const float* p, int slot);  // p = effective params (HexParam), slot 0 / 1
  void reset();
  void process(float* L, float* R, int n, double bpm);
  int type() const { return type_; }

 private:
  void applySetup();
  double sr_ = 44100;
  int type_ = -1;
  bool bypass_ = true, built_ = false;
  float raw_[32] = {};  // this slot's copies of the effect parameters (see setParams)
  int sync_ = 0;
  BbdLine line_[6];
  float fade_ = 0, wet_ = 0;  // input fade f, slot gain W
  // Phaser.
  float apL_[6] = {}, apR_[6] = {}, fbL_ = 0, fbR_ = 0, phPhase_ = 0, phF_ = -1, phM_ = -1;
  // Lo-fi.
  int hold_ = 0;
  float heldL_ = 0, heldR_ = 0;
  struct Lp {
    float b = 0, a1 = 0, a2 = 0, x1 = 0, x2 = 0, y1 = 0, y2 = 0, f = -1;
    void set(float hz, float q, float sr);
    float tick(float x);
  } lpL_, lpR_;
  PlateReverb reverb_;
};

// Drive, slots, EQ and the silence sleep logic.
class PostChain {
 public:
  void configure(double sr);
  void reset();
  void setDrive(double d) { driveTarget_ = static_cast<float>(d); }
  void setEq(bool on, const float* gainDb);
  FxSlot slot[2];
  void process(float* L, float* R, int n, double bpm);
  bool awake() const { return !asleep_; }

 private:
  struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    float tick(float x);
  };
  void design(int band);
  double sr_ = 44100;
  float drive_ = 0, driveTarget_ = 0, dcL_ = 0, dcR_ = 0;
  bool eqOn_ = false;
  float eqDb_[kEqBands] = {};
  float eqDesigned_[kEqBands];
  Biquad eqL_[kEqBands], eqR_[kEqBands];
  int64_t silent_ = 0;
  bool asleep_ = false;
};

}  // namespace gl::packs::synths::hex
