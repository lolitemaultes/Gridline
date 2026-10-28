// Gridline — offline slice processing for the beat slicer (J-41): gl.slicer
// engine 2 ("Processed"). The slicer's sample is rendered once, on the UI
// thread, into a processed copy that the voices then play as is:
//   1. reverse the slices marked reversed;
//   2. declick each slice's end (a DC-tracking fade over the last 28 ms whose
//      tracking speed rises to the full rate, the final 1 ms on a raised
//      cosine);
//   3. stretch: the whole sample becomes round(frames · 2^(fit/10000)) long,
//      slice starts scale with it, each slice running to the next. Methods
//      0/1 keep each slice's audio as it was (truncated or followed by
//      silence); methods 2+ time-stretch every slice to its new length,
//      with the pitch shift (cents) applied at the same time; a pitch shift
//      alone also uses the stretcher. The stretched slices are declicked
//      again;
//   4. fades: fade in (linear; raised cosine at 50 ms or less) and fade out
//      (exponential, tension 10; raised cosine at 50 ms or less) of every
//      slice, in milliseconds.
// Laws worked out from the reference slicer's behaviour (see analysis notes).
// J-87: the stretch runs on engine/TimeStretch. J-153: every method 2+ is the
// one stretch (the stored values stay for FL import and export).
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "Sample.h"
#include "TimeStretch.h"

namespace gl {

struct PluginState;

namespace sliceproc {

struct Settings {
  int pitchCents = 0;      // -1200..1200
  int fit = 0;             // stretch: ratio 2^(fit/10000)
  int method = 4;          // 0/1 keep the audio (no stretch), 2+ time-stretch
  int fadeInMs = 0, fadeOutMs = 0;
  bool declick = true;
  bool neutral() const { return pitchCents == 0 && fit == 0 && fadeInMs == 0 && fadeOutMs == 0 && !declick; }
};

double ratioOf(int fit);  // 2^(fit/10000), as single precision like the reference

// The processed copy of `in`. `starts` are the slice start frames (sorted,
// within the sample); `reverse` one flag per slice. `outStarts` receives the
// slice starts in the processed sample.
SamplePtr process(const Sample& in, const std::vector<int64_t>& starts, const std::vector<char>& reverse, const Settings& s,
                  std::vector<int64_t>* outStarts);

// Steps, exposed for tests (one channel, in place).
void declickEnd(float* x, int64_t len, double sampleRate);
void fadeIn(float* x, int64_t len, int64_t frames, double sampleRate);
void fadeOut(float* x, int64_t len, int64_t frames, double sampleRate);
// Time-stretch `nch` channels of `n` frames to exactly `outLen` frames with a
// pitch factor (engine/TimeStretch, start and end aligned).
std::vector<std::vector<float>> stretch(const float* const* in, int nch, int64_t n, int64_t outLen, double pitch, double sampleRate);

// Settings / slices of a gl.slicer state (engine 2) and the cached processed sample.
Settings settingsOf(const PluginState& st);
SamplePtr processedFor(const PluginState& st, const SamplePtr& in, std::vector<int64_t>* outStarts);

}  // namespace sliceproc
}  // namespace gl
