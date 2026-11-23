// Gridline — "seltmann" pack: the Ribbon editor's pure pieces (0.7.8). No UI here, so the tests and
// the editor share them, and a later Tracker-only layout can reuse them:
//   heights     Compact / Standard / Full, from the host's room and the person's choice
//   tools       Play / Cut / Grid (what a click in the waveform does)
//   layouts     preview models (the current model untouched), one letter system (slots resolved to
//               regions), the regions a layout plays on the current chop, stars and recent layouts
//   key names   per instance (the piano roll names a Seltmann channel's keys by region)
//   linked send a pattern sent to the project follows later sequence edits until its notes are edited
//   takes       a new take that keeps the chop
//   editor keys the editor's own keys, each only when it is free in every key set
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include "../../core/Keymap.h"
#include "../../core/Song.h"
#include "seltmann.h"

namespace gl::packs::seltmann {

// ------------------------------------------------------------------ heights
enum HeightClass { HeightCompact = 0, HeightStandard, HeightFull, HeightCount };
// Room (logical px) under which a host gets the smaller form when the choice is automatic.
inline constexpr float kCompactBelow = 300, kStandardBelow = 440;
// "auto" (or anything else) = -1, "compact" / "standard" / "full" = the class.
int heightChoice(const std::string& name);
const char* heightChoiceName(int choice);  // -1 = "auto"
// The form to draw: a choice wins; automatic: floating (hostH <= 0) = Full, else by the host's room.
int heightClassFor(float hostH, int choice);

// ------------------------------------------------------------------ tools
enum Tool { ToolPlay = 0, ToolCut, ToolGrid, ToolCount };
// A click in the waveform: what each tool lets the person do there.
inline bool toolCuts(int tool) { return tool == ToolCut; }
inline bool toolMovesGrid(int tool) { return tool == ToolGrid; }

// ------------------------------------------------------------------ layouts
// One letter system: every step that names a layout slot plays the region the slot maps to as a
// region step (the slots stay, so the "Letters" key map of old projects keeps working). The sound
// does not change. Returns the steps changed.
int resolveSlots(Model& m);
// The model a layout would make on the current chop (applyPreset on a copy, slots resolved);
// `m` stays as it was. `params` (optional) receives the parameter overrides the layout wants.
Model previewModel(const Model& m, const Sample* s, const Preset& p, uint32_t seed, std::vector<std::pair<int, float>>* params = nullptr);
// The region each step plays (-1 rest), at most `maxSteps`: what a layout card shows.
std::vector<int> stepRegions(const Model& m, int maxSteps);
// Stars and the last layouts used, per person (a small text file in the config folder).
struct LayoutPrefs {
  std::vector<std::string> stars;   // layout names, in the order they were starred
  std::vector<std::string> recent;  // newest first, at most kRecent
  static constexpr size_t kRecent = 3;
  bool starred(const std::string& name) const;
  void toggleStar(const std::string& name);
  void used(const std::string& name);  // moves it to the front of recent
};
std::string layoutPrefsToText(const LayoutPrefs& p);
LayoutPrefs layoutPrefsFromText(const std::string& text);
bool saveLayoutPrefs(const std::string& file, const LayoutPrefs& p);
LayoutPrefs loadLayoutPrefs(const std::string& file);

// ------------------------------------------------------------------ key names (piano roll)
// Names of a Seltmann instance's keys (index = MIDI note, "" = a plain key): from the base note one
// key per region ("A", "B", ...; with the Letters key map the slot letters' regions), the octave
// below "Pattern" and "Pattern +1" ... "Pattern +11".
std::vector<std::string> keyNamesFor(int baseNote, int regions);
// The same for a plugin state (parses the model; cached). Empty for another type or no model.
std::vector<std::string> instanceKeyNames(const PluginState& s);

// ------------------------------------------------------------------ linked send
// Hash of the notes a linked pattern holds for its channel (what the link last wrote).
uint64_t notesHash(const std::vector<Note>& notes);
// Writes the sequence as notes for `channel` into `p` (its notes for the channel and its length),
// `cycles` times: what "Send to project" writes and what a linked pattern follows.
void writePatternNotes(const Song& s, gl::Pattern& p, ObjectId channel, const Model& m, const std::vector<float>& params, int cycles = 1);
enum LinkState { LinkNone = 0, LinkFollowing, LinkEdited };
// A pattern linked to `channel`: following (its notes are what the link wrote) or edited by hand.
LinkState linkState(const gl::Pattern& p, ObjectId channel);
// Brings a following linked pattern up to the model (Relink: also an edited one). True when the
// pattern changed.
bool refreshLinked(const Song& s, gl::Pattern& p, ObjectId channel, const Model& m, const std::vector<float>& params, bool relink = false);

// ------------------------------------------------------------------ takes
// A new take keeps the chop: the regions, cuts and sequence stay where they are in time (the take was
// played to the same tempo); anything past the take's end is dropped or shortened.
Model keepChopOnTake(const Model& m, int64_t takeFrames, double takeRate);

// ------------------------------------------------------------------ editor keys
enum EditorKey {
  KeyPrevRegion = 0, KeyNextRegion, KeyPlayRegion, KeyPlayPattern, KeySplit, KeyJoin, KeyReverseStep,
  KeyChop1, KeyChop2, KeyChop3, KeyChop4, KeyChop5, KeyChop6, KeyToolPlay, KeyCount
};
keys::Chord editorChord(int key);
const char* editorKeyLabel(int key);  // English ("Previous region")
// True when no command of any key set (with and without the macOS Command key) binds the chord
// globally: the editor only takes keys nobody else uses.
bool editorKeyFree(const keys::Chord& c);

}  // namespace gl::packs::seltmann
