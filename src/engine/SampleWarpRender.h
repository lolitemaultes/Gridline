// Gridline: a sample played through its warp markers (core/SampleWarp.h).
// Rendered once, off the audio thread (when an instrument's aux data is
// prepared, or by a "Apply warp" edit), through the one stretch engine's
// variable time map (engine/TimeStretch.h processMapped): every voice then
// plays the warped sample as it would any other, so nothing new runs on the
// audio thread. Cached by the source sample and the markers.
#pragma once
#include "../core/SampleWarp.h"
#include "Sample.h"

namespace gl {

// The warped copy of `in` (its length is the warped length). `in` itself when the warp is not
// active (off, fewer than two markers, or every section at its own speed).
SamplePtr warpedSample(const SamplePtr& in, const SampleWarp& w);
// The same without the cache (tests, the Apply edits): always renders when the markers move anything.
SamplePtr renderWarpedSample(const Sample& in, const std::vector<WarpMarker>& m);
// Onsets of the sample as file seconds (the warp mode's "Add at transients", snapping new markers).
std::vector<double> sampleOnsetsSec(const Sample& in, double sensitivity = 0.5);

}  // namespace gl
