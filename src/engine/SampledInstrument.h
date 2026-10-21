// Gridline — render to sampled instrument (RN-08): plays a channel's
// instrument (built-in or hosted, which renders as itself) offline through
// the render engine, note by note, and turns the takes into a multisampled
// Wave Program (gl.waveprogram): one zone per sampled note, velocity layer
// and round robin, key ranges reaching halfway to the neighbouring samples.
// Takes are trimmed, optionally given sustain loops, written as WAV files
// next to a Gridline preset that maps them.
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <vector>
#include "../core/AudioBuffer.h"
#include "../core/Document.h"
#include "../core/Song.h"

namespace gl::sampled {

struct Options {
  int noteLo = 36, noteHi = 84;  // sampled range (MIDI, C5 = 60)
  int step = 3;                  // sample every `step` semitones from noteLo (noteHi is always sampled)
  std::vector<int> notes;        // non-empty: exactly these notes (a note selection) instead of the range
  int velocityLayers = 1;        // 1..8
  // J-80: non-empty: exactly these velocities as layers (1..127; each layer
  // reaches halfway to its neighbours) instead of velocityLayers even splits.
  std::vector<int> velocities;
  int roundRobins = 1;           // 1..4 takes per note and layer
  double noteSeconds = 1.0;      // how long each note is held
  double tailSeconds = 1.0;      // release tail recorded after the note off
  bool autoTrim = true;          // leading / trailing silence removed
  double trimDb = -60.0;         // silence threshold (dBFS)
  bool findLoops = false;        // sustain loops in the held part
  int sampleRate = 44100;
  int bits = 24;                 // 16, 24 (WAV)
  std::string name = "Sampled";  // instrument / file name
};

// A rendered note.
struct Take {
  int note = 60, velocity = 127, roundRobin = 0;
  int keyLo = 0, keyHi = 127, velLo = 1, velHi = 127;
  AudioBuffer audio;           // stereo, after trimming
  int64_t holdFrames = 0;      // where the note off falls in `audio`
  int64_t trimmedStart = 0;    // frames removed from the start
  int loopStart = 0, loopEnd = 0;  // loopEnd > loopStart: a sustain loop
  std::string file;            // written WAV (renderToSampler)
};

struct Result {
  std::vector<Take> takes;
  PluginState state;       // gl.waveprogram mapping the files
  std::string dir;         // folder the files went to
  std::string presetFile;  // the preset next to them
  std::string error;       // non-empty: failed (nothing usable)
  bool cancelled = false;
};

// Notes that get sampled (ascending, unique, 0..127).
std::vector<int> sampledNotes(const Options& o);
// Key range of root i among sorted roots: halfway to the neighbours (the
// lower half-step goes to the upper sample), the ends reach 0 / 127.
void keyRange(const std::vector<int>& roots, size_t i, int& lo, int& hi);
// Velocity layer k of n: [lo, hi], rendered at hi.
void velocityLayer(int layers, int k, int& lo, int& hi);
// Display name of a note ("C#5" with C5 = 60).
std::string noteLabel(int note);

// The song reduced to the source channel (its instrument, envelopes and
// layers; no note processors, echo or glide) at unity on a neutral master.
Song sourceSong(const Song& song, ObjectId channel);
// Renders every take (no files). Progress 0..1; returns false on error / cancel.
bool renderTakes(const Song& song, ObjectId channel, const Options& o, std::vector<Take>& out, std::string* err = nullptr,
                 const std::function<void(double)>& progress = {}, const std::atomic<bool>* cancel = nullptr);
// Removes leading silence (keeping a few frames before the first sound) and
// trailing silence (with a short fade). Returns the frames removed from the start.
int64_t trimSilence(AudioBuffer& a, double thresholdDb);
// A sustain loop in [from, to) frames: zero crossings whose surroundings match
// best. False when the material does not loop well (or is too short).
bool findLoop(const AudioBuffer& a, int64_t from, int64_t to, int sampleRate, int& start, int& end);
// The Wave Program state for the takes (zones refer to Take::file).
PluginState programState(const std::vector<Take>& takes, const Options& o);

// Renders, writes `<dir>/<name>_<note>_v<vel>[_rr<k>].wav` and `<dir>/<name>.json`
// (a Gridline preset of the Wave Program). `dir` is created; it should be new
// (sample files are cached by path).
Result renderToSampler(const Song& song, ObjectId channel, const Options& o, const std::string& dir,
                       const std::function<void(double)>& progress = {}, const std::atomic<bool>* cancel = nullptr);

// Puts a rendered instrument on the project as one undo step: the channel's
// instrument is replaced (`replace`), or a new channel named `name` with the
// same strip and mixer track is added after it. Returns the channel's id.
ObjectId applyToProject(Document& d, ObjectId channel, const PluginState& st, bool replace, const std::string& name);

// A folder under `parent` named after the instrument that does not exist yet
// ("<name>", "<name> 2", …).
std::string freshFolder(const std::string& parent, const std::string& name);

}  // namespace gl::sampled
