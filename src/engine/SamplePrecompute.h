// Gridline — the sampler's precomputed sample effects (J-23): processing
// applied once to a channel's sample data when it is prepared (UI thread),
// never on the audio thread. Laws follow the reference sampler's "legacy FX"
// stage (see SamplePrecompute.cpp for each step and its law).
#pragma once
#include <string>
#include "Sample.h"

namespace gl {

struct PluginState;

struct SamplePrecomputeFx {
  double rampIn = 0, rampOut = 0;  // linear ramps over the whole sample, slope ×1/256 per raw unit (0..4)
  double boost = 0;                // 0..1 → gain 1 + 16·boost
  bool clip = false;               // hard clip at ±1 after the boost
  double tilt = 0;                 // -1..1: one-pole feedback coefficient (raw 0..256 around 128)
  double cutoff = 1, reso = 0;     // 0..1 (cutoff 1 = off)
  double ringMix = 0, ringHz = 1041.25;
  double stereoDelayMs = 0;        // > 0 delays the left channel, < 0 the right
  double warp = 0;                 // -1..1 time warp ("pogo")
  double reverbMix = 0;
  int reverbType = 1;
  bool fadeStereo = false;         // left fades out while the right fades in
  bool neutral() const;
  std::string key() const;         // cache key fragment
};

// Reads the sampler's precompute parameters (gl.sampler only).
SamplePrecomputeFx samplePrecomputeFx(const PluginState& st);
// The processed copy of `in` (a new sample; `in` when fx is neutral).
SamplePtr precomputeSample(const SamplePtr& in, const SamplePrecomputeFx& fx);
// Cached: the processed sample for `path` (loaded through sampleCache()).
SamplePtr precomputedSample(const std::string& path, const SamplePrecomputeFx& fx);

}  // namespace gl
