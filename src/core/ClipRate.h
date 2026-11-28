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
  double timeFactor = 1;  // clip stretch x its rate fit x the channel's time stretch (2 = twice as long)
  double speed = 1;       // source seconds per playback second (not tempo-synced)
  double pitch = 1;       // heard frequency ratio against the source (root note, fine tune, pitch shift and the
                          // clip's pitch; before the channel's pitch knob)
};

// K-444: the key a playlist audio clip plays its channel's sound at (C5).
constexpr double kClipReferenceKey = 60.0;

inline ClipRate clipRate(const Channel& ch, const Clip& c) {
  const bool sampler = ch.plugin.type == "gl.sampler";
  auto finiteOr = [](double v, double d) { return std::isfinite(v) ? v : d; };
  auto par = [&](int k, double d) {
    if (!sampler || k < 0 || k >= static_cast<int>(ch.plugin.params.size())) return d;
    return finiteOr(static_cast<double>(ch.plugin.params[static_cast<size_t>(k)]), d);
  };
  ClipRate r;
  r.resample = par(pp::SmpStretchMode, 0) < 0.5;
  const double cStretch = std::clamp(finiteOr(c.stretch, 1.0), 1.0 / 64.0, 64.0);
  const double cFit = std::clamp(finiteOr(c.rateFit, 1.0), 1.0 / 1024.0, 1024.0);
  const double chTime = std::clamp(par(pp::SmpTimeStretch, 1), 0.01, 100.0);
  r.timeFactor = std::clamp(cStretch * cFit * chTime, 0.01, 1000.0);
  // K-444: a clip plays its sound as its channel plays the reference key (C5, note 60): the root
  // note says which key plays the sound at its own pitch and the fine tune moves it, as they do
  // for the channel rack's notes (a root a whole tone up plays the clip a whole tone down).
  const double keyCents = (kClipReferenceKey - par(pp::SmpRoot, kClipReferenceKey)) * 100.0 + par(pp::SmpFine, 0);
  const double cents =
      std::clamp(par(pp::SmpPitchShift, 0) + finiteOr(static_cast<double>(c.pitch), 0.0) + keyCents, -6000.0, 6000.0);
  const double ratio = std::pow(2.0, cents / 1200.0);
  if (r.resample) {
    const double spCents = std::clamp(finiteOr(static_cast<double>(c.speedPitch), 0.0), -6000.0, 6000.0);
    r.speed = std::pow(2.0, spCents / 1200.0) / r.timeFactor;
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
