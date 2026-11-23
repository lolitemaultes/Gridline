// Gridline: the audio clip editor's "Edit audio" operations (Loupe's
// operations, packs/edison/edison.h) on a part of a sample. Each writes a new
// file (named by the hash of its audio, in the project's edited-audio folder)
// and keeps the old one, so pointing the channel at the new file is one undo
// step and undo goes back to the old file. Pure file in, file out: the UI
// makes the channel edit; collaborators fetch the new file as they fetch a
// recording.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "../core/Song.h"
#include "../core/TimeWarp.h"

namespace gl::clipedit {

enum class Op : int {
  // Selection
  Cut, Silence, InsertSilence, TrimToSelection, TrimSideSilence,
  // Amplitude
  FadeIn, FadeOut, Normalize, Amplify, Reverse, Invert, RemoveDc, Gate,
  // Repair
  Denoise, Declick,
  // Time and pitch
  StretchPitch, TuneToNote, Resample,
  // Effects
  Equalize, Reverb, Blur,
  // Channels
  Mono, Stereo, Swap,
  // Warp (Loupe, the Tracker sample editor: the markers rendered into the file)
  ApplyWarp,
  Count
};
const char* opName(Op o);       // the button's label ("Fade in", ...)
const char* opHint(Op o);       // its tooltip
bool opUsesRange(Op o);         // acts on the selection (else the whole file)
bool opHasDialog(Op o);         // takes settings first ("Amplify...")

struct Args {
  double db = 0;              // Amplify: gain in dB; Normalize: target dB; Gate: threshold dB
  int curve = 0;              // Fade in / out: packs::edison::Curve
  double factor = 1.0;        // StretchPitch: time factor (2: twice as long)
  double semitones = 0;       // StretchPitch: pitch shift
  int note = -1;              // TuneToNote: the target (-1: the nearest note)
  double rate = 48000;        // Resample: the new sample rate
  double seconds = 1.0;       // InsertSilence: how long
  double thresholdDb = -60;   // TrimSideSilence
  double mix = 0.3;           // Reverb wet, Blur mix
  double ms = 40;             // Blur length
  double eqLowDb = 0, eqMidDb = 0, eqHighDb = 0;  // Equalize: a three-band tilt
  std::vector<WarpMarker> warp;                   // ApplyWarp
};

struct Result {
  std::string path;   // the new file ("" on failure)
  std::string error;
  int64_t frames = 0;  // its length
  double rate = 0;
  int64_t selA = 0, selB = 0;  // the edited range after the edit (frames), for the selection
};

// Applies `op` on frames [a, b) of the file `src` (b <= a: the whole file), writing the result into
// folder `dir`. The source file is never changed.
Result apply(const std::string& src, int64_t a, int64_t b, Op op, const Args& args, const std::string& dir);

// The clip's selection as file frames: song ticks [t0, t1) of audio clip `clip`, through its time
// map (slip, stretch, warp, reverse) and its loop passes (clipAudibleSpans: the earliest to the
// latest source second the range hears). False when the clip has no decoded audio or the range
// hears none of it (only silence after the audio ran out).
bool clipRangeFrames(const Song& s, ObjectId clip, double t0, double t1, int64_t& a, int64_t& b);

}  // namespace gl::clipedit
