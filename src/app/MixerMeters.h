// Gridline - the mixer tracks' meter readings, taken once per frame (UI thread). The engine keeps
// each track's peak and RMS since the interface last read them (MixerRuntime, exchange(0)): a
// second reader would steal peaks from the first, so the mixer strips and the Stage both read
// them through here.
#pragma once
#include "../core/Types.h"

namespace gl {
class App;
namespace mixmeters {

struct Reading {
  float peakL = 0, peakR = 0, rmsL = 0, rmsR = 0;
  float peak() const { return peakL > peakR ? peakL : peakR; }
};
// The track's readings for this frame (the same values for every caller in one frame).
Reading get(App& a, ObjectId track);

}  // namespace mixmeters
}  // namespace gl
