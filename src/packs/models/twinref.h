// Gridline — "models" pack, J-33: Twin String reference engine (TsEngine = 1),
// the reference string instrument as analysed (behaviour only):
//   voices run at 48 kHz in control chunks (≤ 48 host samples, on a fixed grid
//   of the instrument's own sample clock) and are resampled linearly;
//   exciter: shared-LCG noise sample-and-hold / DC step → non-tracking lo-cut
//   high pass → 4-pole hi-cut ladder with its own envelope → drive clip on the
//   ladder state; strings: two strings of two lanes each (lane a heard, lane b
//   feeding the comb), DC-tracked injection, 2-pole damping with sharpness
//   feedback, in-loop saturation, string 1 tuned exactly by a fractional
//   string clock, string 2 an integer ratio (string 1 shortened to a multiple);
//   one-pole amp / hi-cut envelopes, linear free envelope, global LFO, 8-slot
//   matrix; master FX: 32 Hz DC filter → 8 resonators (clocked lines, two in
//   series per band) → chorus → reverb (Hex Synth's plate engine) → delay
//   (clocked lines) → master level.
// Allocation happens in configure() only.
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <vector>
#include "strings.h"

namespace gl::packs::models::strs {

class TwinRef {
 public:
  TwinRef();
  ~TwinRef();
  // `p`: TsCount control values (raw, as stored).
  void configure(const float* p, double sr, double bpm);
  void noteOn(uint32_t id, double pitch, float vel);
  void noteOff(uint32_t id);
  void midi(double modWheel, double bend, double pressure) { mw_ = modWheel, pw_ = bend, at_ = pressure; }
  void allOff();
  void kill();
  bool active() const;
  void render(float* L, float* R, int n);

 private:
  struct Voice;
  struct Fx;
  void control(Voice& v, int n48);
  double mod(const Voice& v, int dest) const;
  void hardReset(Voice& v);
  void renderVoices(int k);
  void renderPart(float* L, float* R, int n);
  void fx(float* L, float* R, int n);
  float p_[TsCount] = {};
  double sr_ = 44100, bpm_ = 120, step_ = 48000.0 / 44100.0;
  std::vector<Voice> v_;
  std::unique_ptr<Fx> fx_;
  std::vector<double> bufL_, bufR_;
  double pos_ = 0;
  int chunkLeft_ = 0;  // host samples left in the current 48-sample control chunk
  int chunkK_ = 0;     // 48 kHz samples rendered for it
  double lfoPh_ = 0, lfo_ = 1;
  uint32_t lcg_ = 0x1234567u;
  uint64_t counter_ = 0;
  double mw_ = 0, pw_ = 0, at_ = 0;
  int tail_ = 0;
};

}  // namespace gl::packs::models::strs
