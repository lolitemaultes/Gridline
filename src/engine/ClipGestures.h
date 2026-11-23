// Gridline: the pointer gestures on an audio clip that the playlist and the
// audio clip editor share (one code path, so the same drag gives the same clip
// in both): trimming either edge, slipping the audio inside the clip, fade
// lengths and curves, and the volume line. Positions are song ticks; each
// returns the clip as it would be after the gesture (a copy), or the value to
// store. UI-free.
#pragma once
#include <set>
#include <vector>
#include "../core/Automation.h"
#include "../core/Song.h"

namespace gl::clipgest {

// The left edge moved by d ticks, the audio staying in place (engine/PlaylistTools trimStarts).
Clip trimStart(const Song& s, const TempoMap& tm, const Clip& c, Tick d, bool stretch, bool zeroCross);
// The right edge moved by d ticks (resizeEnds).
Clip trimEnd(const Song& s, const TempoMap& tm, const Clip& c, Tick d, bool stretch, bool zeroCross);
// The audio moved d ticks inside the clip (slipClip).
Clip slip(const Song& s, const TempoMap& tm, const Clip& c, Tick d, bool zeroCross);

// A fade handle dragged to `atSec` seconds after the clip's start (`durSec`: the clip's playing
// length; `otherSec`: the fade at the other edge). kind 1: fade in, 2: fade out. The fade's new
// length in seconds (never into the other fade).
double fadeLength(int kind, double atSec, double durSec, double otherSec);
// A fade curve handle dragged `downPx` pixels from where it was grabbed at handle value `startUi`
// (-1 .. 1): the curve's new tension (FL unit), with the detent at the plain curve.
float fadeCurve(float startUi, float downPx);
// The handle value (-1 .. 1) the drag shows (before fadeTensionFromUi).
float fadeCurveUi(float startUi, float downPx);

}  // namespace gl::clipgest
