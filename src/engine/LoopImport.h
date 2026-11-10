// Gridline - loop files into a project (K-161). A sliced loop becomes a
// slicer channel (Kerf) cut at the file's own slices and a pattern that plays
// every slice on its beat, so the loop follows the song tempo by retriggering
// its slices without stretching them. A loop that states its tempo plays as a
// sampler or audio clip that follows the song tempo with its pitch kept.
// Pure song edits (no UI): the app wraps them in one undo step.
#pragma once
#include <string>
#include <vector>
#include "../core/Song.h"
#include "LoopFile.h"

namespace gl::loopimport {

// Most slices one slicer channel plays: the 128 keys. Files with more get
// their shortest neighbouring slices joined until they fit.
inline constexpr int kMaxSlicedKeys = 128;

struct Sliced {
  PluginState state;                   // the slicer's state with the file as its sample
  std::vector<loopfile::Slice> slices; // the slices it plays (after joining)
  std::vector<int> keys;               // the key of each slice
  int joined = 0;                      // slices joined to fit the keys
};
// The slicer for a sliced loop file (`li` from loopfile::read). False when it has no slices.
bool slicerFor(const std::string& path, const loopfile::Info& li, Sliced& out);

// The loop's length in ticks (ppq ticks per quarter note); 0 when unknown.
Tick loopTicks(const loopfile::Info& li, int ppq);
// Ticks from the loop start of a position in beats of the loop's meter.
Tick beatTicks(const loopfile::Info& li, double beat, int ppq);

// One note per slice (not the muted ones) at its beat, as long as the slice.
std::vector<Note> sliceNotes(const Sliced& sl, const loopfile::Info& li, int ppq);

struct Result {
  ObjectId channel = kNoId, pattern = kNoId, clip = kNoId;
  int slices = 0, joined = 0;
};
// A slicer channel and its pattern; with track >= 0 also a pattern clip there at `start`.
Result addSlicedLoop(Song& s, const std::string& path, const loopfile::Info& li, int track, Tick start);

// A sampler state set to follow the song tempo from the loop's own tempo, its pitch kept.
void followTempo(PluginState& sampler, double bpm);

}  // namespace gl::loopimport
