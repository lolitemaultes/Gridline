// Gridline — J-164: how fast an audio clip plays its source and at what pitch.
// The one mapping the engine (Program.cpp), the clip operations (ClipOps:
// splits, trims, natural length) and the playlist's waveform share.
//
// A clip's length on the playlist and the stretch that fits its audio into it
// are the user's: pitch never changes them. In Resample mode the stretch
// resamples (a clip stretched to twice its length plays an octave lower) and
// the clip's pitch moves on top of it without changing the timing, i.e. the
// audio is resampled by the pitch and stretched back by the same ratio, so it
// still spans exactly the clip. Only `Clip::speedPitch` (FL projects, whose
// resampled pitch also sets the speed) changes the speed.
// Tempo-synced clips: the time factor scales the tempo fit; pitch is apart.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include "PluginParams.h"
#include "Song.h"

namespace gl {

struct ClipRate {
  bool resample = true;   // the channel's time mode resamples (else it keeps the pitch)
  double timeFactor = 1;  // clip stretch x the channel's time stretch (2 = twice as long)
  double speed = 1;       // source seconds per playback second (not tempo-synced)
  double pitch = 1;       // heard frequency ratio against the source (before the channel's pitch knob)
};

inline ClipRate clipRate(const Channel& ch, const Clip& c) {
  const bool sampler = ch.plugin.type == "gl.sampler";
  auto par = [&](int k, double d) {
    return sampler && k < static_cast<int>(ch.plugin.params.size()) ? static_cast<double>(ch.plugin.params[static_cast<size_t>(k)]) : d;
  };
  ClipRate r;
  r.resample = par(pp::SmpStretchMode, 0) < 0.5;
  r.timeFactor = std::max(0.01, c.stretch * par(pp::SmpTimeStretch, 1));
  const double cents = par(pp::SmpPitchShift, 0) + static_cast<double>(c.pitch);
  const double ratio = std::pow(2.0, cents / 1200.0);
  if (r.resample) {
    r.speed = std::pow(2.0, static_cast<double>(c.speedPitch) / 1200.0) / r.timeFactor;
    r.pitch = ratio / r.timeFactor;
  } else {
    r.speed = 1.0 / r.timeFactor;
    r.pitch = ratio;
  }
  return r;
}

// Source seconds per playback second of a clip that does not follow the tempo.
inline double clipPlaybackSpeed(const Channel& ch, const Clip& c) { return clipRate(ch, c).speed; }

}  // namespace gl
