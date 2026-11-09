// Gridline — K-85: the audio an audio clip plays from its channel.
//
// A sampler channel's sample page edits the sound its clips play, not only its
// notes: the start and end handles of the whole-file view trim it (FL's
// precomputed start offset and length), the precomputed effects (J-23) and the
// processing switches (normalize, remove DC, polarity, swap stereo, fades over
// the region) shape it. Notes and clips play the same sound.
//
// Clip windows (Clip::audioStart / audioEnd) stay seconds of the file, in the
// clip's direction (a reversed clip's window is mirrored). The channel's
// handles limit them: a clip never plays audio the handles cut away, and a
// clip whose window starts before the start handle starts at it.
//
// The one place the engine, the clip operations, the playlist's waveform and
// the FL interop read a clip's sound and its limits from.
#pragma once
#include "../core/Song.h"
#include "Sample.h"

namespace gl {

struct ClipSource {
  SamplePtr sample;            // what the channel's clips play (nullptr: no sound, or still decoding)
  double start = 0, end = 0;   // the channel's start / end handles, seconds of `sample`, file direction
  bool ok() const { return sample && sample->frames() >= 2 && sample->sampleRate > 0 && end > start; }
  double seconds() const { return sample ? sample->seconds() : 0.0; }
};

// The channel's start / end handles as fractions of its sample (sampler
// channels; others play the whole file): start < end, at least 1 ms apart
// once the length is known.
void channelTrim(const Channel& ch, double& startFrac, double& endFrac);
// A start (isEnd false) or end handle dragged to `v` (a fraction), kept
// at least 1 ms of a `seconds` long sample from the `other` handle (start <
// end: the handles never cross).
double clampTrimHandle(bool isEnd, double v, double other, double seconds);
// Decodes on the calling thread when needed (engine builds, operations, renders).
ClipSource clipSource(const Channel& ch);
// Never waits for a decode (drawing): no sample while the file decodes.
ClipSource clipSourceIfReady(const Channel& ch);
// True when the clip plays its audio backwards (its own reverse against the channel's).
bool clipPlaysReversed(const Channel& ch, const Clip& c);
// The part of the sound a clip may play, in its own direction (seconds).
void clipLimits(const ClipSource& src, bool reversed, double& lo, double& hi);
// The clip's window [a, b) inside those limits (false: nothing to play).
bool clipWindow(const ClipSource& src, bool reversed, const Clip& c, double& a, double& b);
bool clipWindow(const Song& song, const Clip& c, ClipSource* src, double& a, double& b);

// K-85: the clip's warp markers in its direction (core/TimeWarp.h; empty:
// not warped, also when every section plays at the clip's own speed).
std::vector<WarpPoint> clipWarp(const ClipSource& src, bool reversed, const Clip& c);
std::vector<WarpPoint> clipWarp(const Song& song, const Clip& c);
// Warped seconds between two source positions of a clip (b - a unwarped).
double warpedSpan(const std::vector<WarpPoint>& w, double a, double b);

// K-85: a clip's reverse switched on or off keeps its own audio: the window
// is mirrored over the file (`seconds`: the last frame's position, where
// the engine mirrors a reversed read), so the same part plays backwards. A
// window that is the whole file stays as it is.
void mirrorClipWindow(Clip& c, double seconds);
// The reverse switch of a clip, keeping its audio (mirrorClipWindow).
void toggleClipReverse(const Song& song, Clip& c);

}  // namespace gl
