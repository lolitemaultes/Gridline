// Gridline — offline rendering through the realtime engine (identical sound).
//
// renderSong: the plain song / pattern render used by tools and tests.
// renderAudio: the export engine (RN-01, RN-03, RN-04): render scope (song,
// pattern, song range), loop count, tail modes, render switches (insert /
// master effects, polyphony limits, sampler interpolation), progress and
// cancel. renderStems / renderPlaylistTracks split the mix (one buffer per
// mixer insert or playlist track) on top of it.
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <vector>
#include "../core/AudioBuffer.h"
#include "../core/Song.h"
#include "AudioEngine.h"

namespace gl {

struct RenderOptions {
  int sampleRate = 44100;
  int channels = 2;
  Tick tail = 0;         // extra ticks after the song end for release tails
  // Chunk size the renderer asks the engine for. The engine renders its own fixed
  // blocks offline (AudioEngine::setFixedBlock), so the output is identical for any size.
  int blockSize = 512;
  bool patternMode = false;
  ObjectId pattern = kNoId;  // pattern to render in pattern mode (one loop)
  int threads = 1;           // mixer graph threads (output is identical for any count)
  bool pdc = true;           // plugin delay compensation (off: paths are not aligned, nothing is skipped)
};

// Renders the song (or one pattern) from the start, without looping.
AudioBuffer renderSong(const Song& song, const RenderOptions& opt);

// ---------------------------------------------------------------- export render

// What is rendered.
enum class RenderScope : int {
  Song = 0,  // the arrangement from its start to the end of its last clip
  Pattern,   // one pattern (RenderJob::pattern), from 0 to its length
  Range,     // song ticks [RenderJob::start, RenderJob::end): time selection / marker range
  Count
};
// What happens to sound still ringing when the scope ends.
enum class TailMode : int {
  Cut = 0,  // the file ends exactly at the scope end
  Leave,    // the file runs on until the tail has died away
  Wrap,     // the tail is mixed into the start of the file (seamless loops)
  Count
};
// Sampler interpolation used while rendering (0 = the realtime default).
enum class RenderInterp : int { Linear = 0, Hermite, Sinc16, Sinc32, Sinc64, Sinc128, Sinc256, Sinc512, Count };
const char* renderInterpName(RenderInterp q);

struct RenderJob {
  int sampleRate = 44100;
  RenderScope scope = RenderScope::Song;
  ObjectId pattern = kNoId;     // Pattern scope
  Tick start = 0, end = 0;      // Range scope (end > start)
  int loops = 1;                // the scope plays this many times (1..64)
  TailMode tail = TailMode::Cut;
  double maxTailSeconds = 30;   // Leave / Wrap: the tail never runs longer
  double silenceDb = -100;      // Leave / Wrap: level under which the tail counts as silent
  // Render switches.
  bool insertEffects = true;    // effect slots of the insert tracks
  bool masterEffects = true;    // effect slots + strip EQ of the master track
  bool polyphonyLimits = true;  // false: channels' maximum polyphony is ignored ("disable max polyphony")
  RenderInterp interp = RenderInterp::Linear;
  int oversample = 1;           // 1, 2 or 4: the engine runs at sampleRate × oversample, then decimates
  int blockSize = 512;
  int threads = 1;
  bool pdc = true;
  // Progress (0..1, called from the rendering thread) and cancel flag.
  std::function<void(double)> progress;
  const std::atomic<bool>* cancel = nullptr;
};

struct RenderResult {
  AudioBuffer audio;          // stereo
  int64_t bodyFrames = 0;     // frames of the scope × loops (the rest is tail)
  int sampleRate = 44100;
  Tick start = 0, end = 0;    // scope in ticks (song ticks, or pattern ticks)
  bool cancelled = false;
  std::string error;          // non-empty: nothing rendered
};

// The song as the render switches see it (effects disabled, limits lifted).
Song prepareRenderSong(const Song& song, const RenderJob& job);
// Scope of a job in ticks; false when empty (no song, no pattern, empty range).
bool renderScopeTicks(const Song& song, const RenderJob& job, Tick& start, Tick& end);
// Frames the engine plays for [start, end) at `sampleRate` (sample-exact: the
// engine's own wrap point).
int64_t scopeFrames(const Song& song, const RenderJob& job, Tick start, Tick end, int sampleRate);
RenderResult renderAudio(const Song& song, const RenderJob& job);

// ---- stems ("split mixer tracks") and playlist tracks ----

// The song reduced to one mixer insert's output: the insert (sent straight to
// the master at unity) and every track and channel feeding it play;
// everything else is muted. The master is neutral (unity, no effects, flat
// EQ) unless `masterFx`. `track` = the master's id → the full mix.
Song stemSong(const Song& song, ObjectId track, bool masterFx);
// Inserts that carry sound: channels or sends feed them (mixer order).
std::vector<ObjectId> usedInserts(const Song& song);
// The song with only playlist track `track` audible.
Song playlistTrackSong(const Song& song, int track);
// Playlist tracks that hold unmuted clips.
std::vector<int> usedPlaylistTracks(const Song& song);

struct SplitPart {
  ObjectId mixer = kNoId;  // stems: the insert (master id = full mix)
  int track = -1;          // playlist track parts
  int index = 0;           // insert number / track number (1-based for tracks)
  std::string name;
};
// Renders each part and hands it to `sink` (return false to stop). Progress
// covers all parts. Returns the number of parts rendered.
using PartSink = std::function<bool(const SplitPart&, RenderResult&&)>;
int renderStems(const Song& song, const RenderJob& job, const std::vector<ObjectId>& tracks, bool masterFx,
                const PartSink& sink);
int renderPlaylistTracks(const Song& song, const RenderJob& job, const std::vector<int>& tracks, const PartSink& sink);
// J-31: several playlist tracks per part (track groups). Each set renders
// with only its tracks audible; part.track = the set's first track.
struct TrackSet {
  std::string name;
  std::vector<int> tracks;
};
Song playlistTracksSong(const Song& song, const std::vector<int>& tracks);
// Top-level playlist track groups (a header track and the tracks it holds)
// that hold unmuted clips, named after the header track.
std::vector<TrackSet> playlistTrackGroups(const Song& song);
int renderPlaylistTrackSets(const Song& song, const RenderJob& job, const std::vector<TrackSet>& sets, const PartSink& sink);

}  // namespace gl
