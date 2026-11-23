// Gridline - K-255: polyrhythmic grids and lane lengths. Every channel of a pattern is a lane:
// its step row on a grid of its own (steps per bar; Pattern::lanes, 0 = the pattern's), a length
// (Pattern::stepLoop, the J-31 step loop, in the lane's own steps) and a mode for that length:
// restart with the pattern (as 0.7.7's step loops) or run on through a longer clip (polymeter).
//
// Timing is exact: step s of a lane on g steps a bar starts at s * bar / g pattern ticks, a
// fraction of a tick where the bar does not divide (5 or 7 a bar); playback keeps the fraction
// (engine/Program.cpp compileChannelNotes), only a burn into notes rounds (and keeps the rest
// in Note::frac). A run-on lane takes its phase from the pass of the clip it plays in: pass k of
// a looping pattern clip starts the lane k x (its steps per pass) steps on, wrapped at its length.
// Lanes with no fields of their own play exactly as in 0.7.7.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "Song.h"

namespace gl {
class Document;
}

namespace gl::lanes {

// The channel's lane (a default one when it has none).
const PatternLane& of(const Pattern& p, ObjectId channel);
// Steps per bar of the channel's row: its own grid, else the pattern's.
int grid(const Pattern& p, ObjectId channel);
// True when the row has a grid of its own.
bool ownGrid(const Pattern& p, ObjectId channel);
// Steps in the row (bars x grid).
int steps(const Pattern& p, ObjectId channel);
// The lane length (the step loop) when it is shorter than the row, else 0 (no loop).
int length(const Pattern& p, ObjectId channel);
LaneMode mode(const Pattern& p, ObjectId channel);
// A lane that loops and runs on through the clip.
bool runsOn(const Pattern& p, ObjectId channel);
// The pattern has a lane with fields of its own (a grid or a mode).
bool any(const Pattern& p);
// The pattern has a lane that loops and runs on.
bool anyRunOn(const Pattern& p);

// Exact start of step `s` of the channel's row, in pattern ticks (a fraction where it falls between ticks).
double stepTick(const Song& song, const Pattern& p, ObjectId channel, int64_t s);
// One step of the row in ticks (exact).
double stepTicks(const Song& song, const Pattern& p, ObjectId channel);
// Steps of the row in one pass of the pattern (its content length, whole bars, x the grid).
int64_t stepsPerPass(const Song& song, const Pattern& p, ObjectId channel);
// Where a run-on lane starts in pass `pass` of a looping clip (0 for every other lane).
int64_t phaseOf(const Song& song, const Pattern& p, ObjectId channel, int64_t pass);
// Passes of the pattern until every run-on lane starts again with it (1 when none runs on), at most `cap`.
int64_t lineUpPasses(const Song& song, const Pattern& p, int64_t cap = 1 << 20);
// The same as bars of the pattern (passes x its bars).
int64_t lineUpBars(const Song& song, const Pattern& p, int64_t cap = 1 << 20);

// A lane that plays exactly as a 0.7.7 row does (the pattern's grid, restarting): playback keeps
// its 0.7.7 code path for these, so existing songs render bit for bit as before.
bool plain(const Pattern& p, ObjectId channel);

// ---- what a lane plays in one pass (shared by playback and the burn) ----
// The steps that play in pass `pass` of the pattern: `index` counts steps from the pass's start,
// `src` is the row step it plays (its velocity and properties), `on` its exact start (pattern ticks).
struct StepHit {
  int64_t index = 0;
  int src = 0;
  double on = 0;
};
std::vector<StepHit> playedSteps(const Song& song, const Pattern& p, ObjectId channel, int64_t pass);
// The piano roll notes that play in that pass: each note (muted ones too; the caller skips them)
// with the offset its repeat is played at (a step loop repeats the notes that start inside it;
// the offset is a fraction of a tick on an odd grid).
struct NoteRepeat {
  const Note* note = nullptr;
  double offset = 0;
};
std::vector<NoteRepeat> playedNotes(const Song& song, const Pattern& p, ObjectId channel, int64_t pass);
// Pass length in ticks the steps above cover (the row, or a run-on lane's whole pass).
double passTicks(const Song& song, const Pattern& p, ObjectId channel);

// What happens to steps already set when the grid changes.
enum class Regrid : uint8_t {
  Nearest,  // each set step moves to the new step nearest to where it sounded (two on one: the louder)
  Order     // step k stays step k (steps past the new row's end drop)
};
// Pure: a row (and its step properties) moved from grid `from` to grid `to` (bars x grid steps).
void regridRow(std::vector<uint8_t>& row, std::vector<StepProps>& props, int bars, int from, int to, Regrid how);

// ---- edits (one undo step each, through Document::editPattern) ----
// Gives the channels a grid of their own (`grid` steps a bar, 1..kMaxLaneGrid; 0 or the pattern's
// own steps per bar: back to the pattern's grid). The lane length scales with the steps (Nearest)
// or stays (Order). False when nothing changed.
bool setGrid(Document& d, ObjectId pattern, const std::vector<ObjectId>& channels, int grid, Regrid how = Regrid::Nearest);
// Every channel of the pattern with steps or notes takes the grid of `like`.
bool gridLikeAll(Document& d, ObjectId pattern, ObjectId like, Regrid how = Regrid::Nearest);
bool setMode(Document& d, ObjectId pattern, const std::vector<ObjectId>& channels, LaneMode m);
// The lane length in its own steps (0 or the whole row: no loop) and, optionally, its mode, in one step.
bool setLength(Document& d, ObjectId pattern, const std::vector<ObjectId>& channels, int steps, const LaneMode* m = nullptr,
               uint64_t mergeKey = 0);

// ---- burn (K-255 P7) ----
// Writes the lanes out as plain pattern content: a lane on its own grid becomes notes (one per
// step, exact to a fraction of a tick, with the step's properties); a lane that restarts writes
// its repeats; a lane that runs on lengthens the pattern to where every lane lines up again (at
// most kMaxBurnBars bars; past that it stops there) and writes its steps through it. The lanes'
// fields and step loops are removed. `channels` empty: every lane of the pattern. Returns the
// lanes burnt (0: nothing to burn).
inline constexpr int kMaxBurnBars = 64;
// `cut`: set when the lanes line up only after more than kMaxBurnBars bars (the burn stops there).
int burn(Document& d, ObjectId pattern, const std::vector<ObjectId>& channels = {}, bool* cut = nullptr);
// Pure: the burn on a song (clips of a lengthened pattern keep their length).
int burnPattern(Song& s, ObjectId pattern, const std::vector<ObjectId>& channels = {}, bool* cut = nullptr);
// The same on a song (no undo; the FL and MIDI exports burn a copy first: the formats have no lanes).
int burnSong(Song& s);
// The set steps of a row on its own grid as notes (one per step, exact, with the step's properties).
std::vector<Note> rowAsNotes(const Song& s, const Pattern& p, ObjectId channel);
// True when a pattern of the song has a lane to burn (a step loop, a grid of its own).
bool needsBurn(const Song& s);

}  // namespace gl::lanes
