// Gridline — K-151: swing presets and grooves.
//
// The song's swing is an amount (Song::swing, the rack's Swing knob) and a
// shape (Song::groove, see GrooveShape in Song.h): a grid (the pattern step or
// a note value), a cycle of cells, a timing offset per cell and an optional
// velocity factor per cell. The default shape is the classic step swing, so a
// project that never touched a preset plays exactly as before.
//
// A swing preset is a shape plus the amount (and accent) it sets. Factory
// presets are built in; the user's own live in the preset library as small
// JSON files:
//
//   <config>/presets/grooves/<folder…>/<name>.json
//   {"format": "gridline-groove", "version": 1, "name": "…", "grid": "1/16",
//    "cycle": 2, "amount": 0.32, "timing": [0, 0.5], "velocity": [1, 0.85],
//    "accent": 1, "source": "…"}
//
// Applying a preset sets Song::swing and Song::groove in one undo step.
// Grooves can be taken from notes (a pattern, a MIDI file, a score file) or
// from an audio clip's transients (engine/WarpOps.h).
#pragma once
#include <string>
#include <vector>
#include "Song.h"

namespace gl::groove {

enum Grid : int { kGridStep = 0, kGrid8th = 1, kGrid16th = 2, kGrid32nd = 3, kGridCount = 4 };
inline constexpr int kMaxCycle = 16;
inline constexpr int kCycles[] = {2, 3, 4, 6, 8, 12, 16};
inline constexpr int kCycleCount = 7;
inline constexpr int kFormatVersion = 1;

const char* gridKey(int grid);                 // "step", "1/8", "1/16", "1/32"
int gridFromKey(const std::string& key);       // unknown: step
const char* gridName(int grid);                // "Pattern step", "8ths", "16ths", "32nds"
bool validCycle(int cycle);

// Clamps a shape read from a file, a project or a collaborator: known grid,
// a listed cycle, timing / velocity sized to the cycle (velocity empty or full).
void normalize(GrooveShape& s);
void normalize(SongGroove& g);
// Two cells, the second half a cell late at full amount, no accents (grid any).
bool isPairSwing(const GrooveShape& s);

// Length of one cell in ticks: the pattern's step (grid 0) or a note value.
// Without a pattern, grid 0 counts as sixteenths.
double cellTicks(const Song& song, const Pattern* p, int grid);

// The groove ready to place notes: no allocation, cheap to copy.
struct Warp {
  bool active = false;     // false: positions unchanged
  bool accents = false;    // a velocity lane applies
  double cell = 0;         // ticks per cell
  double bar = 0;          // ticks per bar (cycles restart every bar)
  int cycle = 2;
  double anchor[kMaxCycle + 2] = {};  // cell starts inside a cycle, in ticks (anchor[cycle]: the cycle's end)
  double tailEnd = 0;      // anchor[cycle] when the next cycle in the bar is straight
  float vel[kMaxCycle] = {};  // velocity factor per cell (1: unchanged)
  // Warped position of pattern tick x (piecewise linear between cell starts).
  double map(double x) const;
  // Velocity factor of a note written at pattern tick x.
  float velocityAt(double x) const;
};
// `amount` is Song::swing x the channel's swing mix (0..1); `accent` the accent
// share (SongGroove::accent x the channel's swing mix). `p` null: song bars.
Warp makeWarp(const Song& song, const Pattern* p, const GrooveShape& shape, double amount, double accent);

// ---- presets ----
struct Preset {
  std::string name;
  GrooveShape shape;
  float amount = 0;
  float accent = 1;
  std::string source;   // optional note ("from the pattern 'Drums'")
  bool factory = false;
  std::string folder;   // below grooves/ ("" = top level), user presets
  std::string file;     // absolute path, user presets
};
const std::vector<Preset>& factoryPresets();
// Factory preset by name (case-insensitive; older quantize groove names too). Null: none.
const Preset* findFactory(const std::string& name);

std::string grooveFolder(const std::string& presetRoot);  // <root>/grooves
std::vector<Preset> listUserPresets(const std::string& presetRoot);
// Factory first, then the user's (whose name may repeat a factory one).
std::vector<Preset> allPresets(const std::string& presetRoot);
// Factory or user preset by name ("Folder/Name" for user presets in folders; user wins over factory).
bool findPreset(const std::string& presetRoot, const std::string& name, Preset& out);

std::string toJsonText(const Preset& p);
bool fromJsonText(const std::string& text, Preset& out, std::string* err = nullptr);
bool loadPresetFile(const std::string& file, Preset& out, std::string* err = nullptr);
bool savePresetFile(const std::string& file, const Preset& p, std::string* err = nullptr);
// Saves into the library as <root>/grooves/<relName>.json (relName may hold folders).
bool saveUserPreset(const std::string& presetRoot, const Preset& p, const std::string& relName, std::string* path = nullptr,
                    std::string* err = nullptr);
bool deleteUserPreset(const Preset& p);

// The preset made of the song's current swing (named `name`).
Preset fromSong(const Song& song, const std::string& name);
// Sets the song's swing amount and groove to the preset's (the caller wraps it in one undo step).
void apply(Song& song, const Preset& p);
// True when the song's swing still is the preset's (no edit since it was applied).
bool matches(const Song& song, const Preset& p);
// What the rack's preset chip shows: the preset's name ("Easy shuffle", "Easy shuffle (edited)"),
// or "Straight" / "Step swing" / "Custom groove" without one.
std::string chipLabel(const Song& song, const std::vector<Preset>& presets);
// Preset by name in a list (allPresets), user presets first; factory fallback. Null: none.
const Preset* findIn(const std::vector<Preset>& presets, const std::string& name);
// One line for hints: "16ths, off-beat at 58 % of the pair", "8 cells on 16ths, with accents".
std::string feelText(const GrooveShape& s, double amount);

// ---- grooves taken from what was played ----
struct Onset {
  double tick = 0;    // song or pattern tick
  float level = 1;    // velocity or peak level (any scale; normalised to the loudest)
};
std::vector<Onset> onsetsOfNotes(const std::vector<Note>& notes);
// Average timing (and level) of the onsets per cell, cycled every `cycle` cells
// from bar starts. The timing is scaled so its largest offset is half a cell and
// the amount carries the size: a played 58 % swing gives {0, 0.5} at 0.32. Cells
// with no onset stay straight; timing counts from the first cell (the downbeat).
// Velocity lane only when the levels vary (each cell at least 0.1: never silent).
Preset extract(const std::vector<Onset>& onsets, double cell, double bar, int grid, int cycle, const std::string& name);

// A groove from the notes of a MIDI file (every track), on `grid` (16ths when the step grid).
bool fromMidiFile(const std::string& file, int grid, Preset& out, std::string* err = nullptr);
// Any file a swing preset can come from: a preset (.json), a MIDI file (.mid / .midi) or a score (.fsc).
bool importFile(const std::string& file, Preset& out, std::string* err = nullptr);

// ---- FL Studio ----
// The main swing value to write to an .flp (0..1): the amount when the shape is
// a plain pair swing on the pattern step (exact), the nearest for a pair swing
// on another grid, 0 for any other groove. `exact` says whether nothing is lost.
double flSwing(const Song& song, bool* exact);
// A groove from the notes of an FL score file (.fsc): the grid is read from
// the file name ("16_…" sixteenths, "8_…" eighths; others sixteenths).
bool fromScoreFile(const std::string& file, Preset& out, std::string* err = nullptr);

}  // namespace gl::groove
