// Gridline — "models" pack: Twin String reference engine (TsEngine = 1), the
// reference string instrument rebuilt from its measured behaviour (J-33,
// rewritten in J-65 against the instrument run side by side: every shipped
// program renders to within float rounding of it).
//   Voices run at 48 kHz in control chunks of ≤ 48 host samples and are
//   resampled linearly; voices are rendered in groups of four that share one
//   LCG noise stream (inactive voices fill the last group and keep running
//   their exciter).
//   Exciter: noise sample-and-hold → 2-pole lo-cut → 4-pole hi-cut ladder with
//   its own envelope → drive (division on the ladder output).
//   Strings: two strings of two lanes (heard / comb-feeding), DC-tracked
//   injection, 2-pole damping with sharpness feedback and in-loop saturation,
//   comb lines; string 2 an integer ratio of string 1.
//   Envelopes: one-pole amp / hi-cut, linear free envelope; global LFO.
//   Master: 32 Hz DC high pass → 8 resonators (clocked lines, two per band) →
//   chorus (3 swept clocked lines) → plate reverb (Hex Synth's engine) →
//   delay (two serial chains of 3 swept clocked lines) → level.
//   The engine runs in 256-frame blocks (notes and controls apply from the
//   next block start), like the host the reference was measured in.
// Allocation happens in configure() only.
#pragma once
#include <cstdint>
#include <memory>
#include "strings.h"

namespace gl::packs::models::strs {

class TwinRef {
 public:
  TwinRef();
  ~TwinRef();
  // `p`: TsCount control values (raw, as stored). Changed controls are sent
  // to the engine one by one (as the reference receives them).
  void configure(const float* p, double sr, double bpm);
  // `pitch`: MIDI key + fraction; `vel` 0..1 (×128 = the host's note
  // velocity); `pan` −1..1.
  void noteOn(uint32_t id, double pitch, float vel, float pan = 0.f);
  void noteOff(uint32_t id);
  void midi(double modWheel, double bend, double pressure);
  void allOff();
  void kill();
  bool active() const;
  // Adds into L / R.
  void render(float* L, float* R, int n);

  struct Engine;

 private:
  std::unique_ptr<Engine> e_;
};

}  // namespace gl::packs::models::strs
