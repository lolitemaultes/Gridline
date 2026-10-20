// Gridline — offline loudness measurement and normalisation (RN-09).
//
// Integrated loudness follows ITU-R BS.1770-4 / EBU R128: K-weighting (high
// shelf + high pass, designed for the buffer's sample rate), mean square per
// channel summed with unity weights (front L / R; a mono buffer counts as
// both), 400 ms gating blocks with 75 % overlap, an absolute gate at
// -70 LUFS and a relative gate 10 LU below the ungated mean.
#pragma once
#include "../core/AudioBuffer.h"

namespace gl {

inline constexpr double kSilentLufs = -144.0;  // what silence (or a buffer shorter than one block) measures

double integratedLoudness(const AudioBuffer& buf, int sampleRate);
// Sample peak of the buffer in dBFS (-144 for silence).
double samplePeakDb(const AudioBuffer& buf);

struct LoudnessNormalization {
  double measured = kSilentLufs;  // before (LUFS)
  double gainDb = 0;              // applied gain
  double result = kSilentLufs;    // after (LUFS)
  bool ceilingLimited = false;    // the peak ceiling reduced the gain (result below the target)
};
// Scales the buffer so it measures `targetLufs`, but never so far that its
// sample peak passes `ceilingDb` (dBFS). Silence is left as it is.
LoudnessNormalization normalizeLoudness(AudioBuffer& buf, int sampleRate, double targetLufs, double ceilingDb = -1.0);

}  // namespace gl
