// Gridline — "spectral" pack: the reference vocoder's built-in carrier tones (J-65).
//
// The imported vocoder's built-in carrier is the FM-matrix instrument playing
// one of its own tone presets, shipped inside the FL Studio installation
// (Plugins/Fruity/Effects/Vocodex/Synthesizer/Data/Presets/<tone>.fst). The
// preset is read in place at run time (never copied into Gridline) and played
// by Gridline's Matrix FM reference model the way the vocoder drives it: every
// held key at host voice volume 0.5 (velocity 100), centre pan, pitch
// (key - 60) * 100 cents,
// at the host rate in 256-frame blocks from the first rendered frame (checked
// sample for sample against the instrument's output). Without the
// installation the vocoder keeps its approximate tone families.
#pragma once
#include <memory>
#include <string>

namespace gl::packs::spectral {

// The installation's preset file for a tone name ("" when it is not there).
std::string vxTonePath(const std::string& tone, const std::string& factoryData = "");
// True when the tone preset exists and the reference model plays it exactly.
bool vxToneExact(const std::string& tone, const std::string& factoryData = "");

class VxCarrier {
 public:
  VxCarrier();
  ~VxCarrier();
  VxCarrier(const VxCarrier&) = delete;
  VxCarrier& operator=(const VxCarrier&) = delete;
  // UI thread (Effect::prepareHost): builds the tone's engine when the tone or
  // the rate changed ("" = none).
  void prepare(const std::string& tone, int rate);
  // Audio thread: adopts a prepared engine, keeps the sounding keys equal to
  // `keys` and renders n frames into L / R (overwritten) at `tempo` (BPM).
  // False when no exact tone is ready (L / R untouched).
  bool render(const int* keys, int nk, double tempo, float* L, float* R, int n);
  void reset();  // audio thread: silences the tone (keys start again)

 private:
  struct Impl;
  std::unique_ptr<Impl> d_;
};

}  // namespace gl::packs::spectral
