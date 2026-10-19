// Gridline — piano roll and step sequencer editing tools (J-11).
//
// Pure functions on a note list / step row so the piano roll, the channel
// rack, the menus and the tests share them. Every note tool acts on the
// selected notes (`sel`, same size as `notes`) or on all notes when nothing is
// selected, and keeps `sel` aligned with `notes` (sorted by start, pitch).
// Song-level operations (clips, markers, channels) go through Document so each
// is one undo step.
#pragma once
#include <cstdint>
#include <set>
#include <string>
#include <vector>
#include "Document.h"
#include "MenuOps.h"
#include "MenuOpsExtra.h"  // createLayer, setSwingMix, markers (J-18)

namespace gl::tools {

using NoteSel = ops::NoteSel;

// ------------------------------------------------------------ note properties ----
enum class NoteProp { Velocity = 0, Pan, Release, Fine, ModX, ModY, Count };
const char* notePropName(NoteProp p);
// Property as 0..1 (0.5 = centre for pan / fine pitch).
double notePropNorm(const Note& n, NoteProp p);
void setNotePropNorm(Note& n, NoteProp p, double v);
bool notePropBipolar(NoteProp p);

// -------------------------------------------------------------- scales / keys ----
struct ScaleDef {
  const char* name;
  std::vector<int> steps;  // semitones above the root, ascending, first = 0
};
const std::vector<ScaleDef>& scales();  // index 0 = chromatic (no highlighting)
bool inScale(int pitch, int root, int scale);
// Nearest pitch in the scale (ties go down).
int snapPitchToScale(int pitch, int root, int scale);
void snapToScale(std::vector<Note>& notes, NoteSel& sel, int root, int scale);
// Selects notes inside (or outside) the scale.
void selectInScale(const std::vector<Note>& notes, NoteSel& sel, int root, int scale, bool inside);
std::string pitchName(int pitch);  // "C5", "F#3" (MIDI 60 = C5)

// --------------------------------------------------------------- chord stamps ----
struct ChordDef {
  const char* name;
  std::vector<int> steps;  // semitones above the root
};
const std::vector<ChordDef>& chords();
// Pitches of a chord on `root`; inversion k moves the k lowest notes up an octave.
std::vector<int> chordPitches(int root, int chord, int inversion = 0);
// The chord's notes with `base`'s timing and properties (base.pitch = root).
std::vector<Note> stampChord(const Note& base, int chord, int inversion = 0);

// ------------------------------------------------------------------- quantize ----
enum class DurationMode { Leave = 0, LeaveEnd, QuantizeLength, QuantizeEnd, Count };
const char* durationModeName(DurationMode m);
struct Groove {
  const char* name;
  std::vector<double> shift;     // per grid line (cycled): fraction of a grid cell, -0.5..0.5
  std::vector<double> velocity;  // per grid line (cycled): velocity factor (empty = unchanged)
};
const std::vector<Groove>& grooves();  // index 0 = straight
struct QuantizeOpts {
  Tick grid = 24;
  double startStrength = 1.0;  // 0..1 how far starts move to the target
  DurationMode duration = DurationMode::Leave;
  double durationStrength = 1.0;
  double swing = 0.0;          // 0..1 delays every second grid line by up to half a cell
  double window = 1.0;         // 0..1 only notes within window * half a cell of a line move
  int groove = 0;              // grooves() index
  double grooveVelocity = 0.0; // 0..1 how much of the groove's accents is applied
};
// Target position of grid line `line` (swing and groove applied).
Tick quantizeTarget(const QuantizeOpts& o, Tick line);
void quantizeEx(std::vector<Note>& notes, NoteSel& sel, const QuantizeOpts& o);
// A groove taken from notes: the average offset of note starts from each grid
// line (cycled over `cycle` lines), usable as QuantizeOpts' groove via
// quantizeWithGroove.
std::vector<double> extractGroove(const std::vector<Note>& notes, Tick grid, int cycle);
void quantizeWithShifts(std::vector<Note>& notes, NoteSel& sel, QuantizeOpts o, const std::vector<double>& shift);

// ------------------------------------------------------------------------ chop ----
struct ChopPattern {
  const char* name;
  std::vector<double> lengths;  // piece lengths in steps (cycled)
  std::vector<int> pitch;       // semitone offset per piece (cycled, empty = none)
};
const std::vector<ChopPattern>& chopPatterns();
// Cuts notes into pieces following the pattern (lengths × timeMul steps).
// Properties are kept on every piece; `gate` (0..1) shortens each piece.
void chopByPattern(std::vector<Note>& notes, NoteSel& sel, const ChopPattern& p, Tick stepTicks, double timeMul = 1.0,
                   double gate = 1.0);
// Fixed-size chop that keeps note properties (ops::chop drops them).
void chopEvery(std::vector<Note>& notes, NoteSel& sel, Tick len);

// ----------------------------------------------------------------------- strum ----
struct StrumOpts {
  Tick time = 24;            // offset of the last note of a chord (ticks)
  double timeTension = 0;    // -1..1 bends the offsets (0 = even)
  double velocity = 0;       // 0..1 velocity reduction towards the last note
  double velocityTension = 0;
  double end = 0;            // 0 = ends stay; 1 = ends move with the starts
  double endTension = 0;
  int direction = 0;         // 0 up (lowest first), 1 down, 2 alternate per chord
};
void strumEx(std::vector<Note>& notes, NoteSel& sel, const StrumOpts& o);

// -------------------------------------------------------------------- arpeggio ----
enum class ArpPattern { Up = 0, Down, UpDown, DownUp, Converge, Diverge, Random, Chord, Count };
const char* arpPatternName(ArpPattern p);
struct ArpOpts {
  ArpPattern pattern = ArpPattern::Up;
  int octaves = 1;           // range 1..4
  int rangeMode = 0;         // 0 normal, 1 flip direction on every octave pass, 2 alternate octaves
  Tick step = 24;            // ticks per arpeggio note
  double gate = 0.9;         // 0.05..1 note length relative to the step
  int sync = 0;              // 0 time (steps on the global grid), 1 per chord (restart at each chord)
  double velocityAccent = 0; // 0..1: notes off the beat get quieter
  uint32_t seed = 1;
};
// Turns each chord (notes with the same start; single notes too when octaves > 1)
// into an arpeggio over the chord's length.
void arpeggiateEx(std::vector<Note>& notes, NoteSel& sel, const ArpOpts& o);

// ------------------------------------------------------------------------ flam ----
struct FlamOpts {
  Tick time = 6;          // distance of grace notes (ticks)
  double velocity = 0.6;  // grace note velocity relative to the main note
  int count = 1;          // grace notes per note (1 = flam, 2 = drag)
  bool early = true;      // grace notes before the note (else the note moves later)
};
struct FlamPreset {
  const char* name;
  FlamOpts opts;  // `time` in 1/48 of a beat (scaled with the PPQ by flamPresetOpts)
};
const std::vector<FlamPreset>& flamPresets();
FlamOpts flamPresetOpts(int index, int ppq);
void flam(std::vector<Note>& notes, NoteSel& sel, const FlamOpts& o);

// ------------------------------------------------------------------ articulate ----
struct ArticulateOpts {
  double length = 1.0;     // fraction of the time to the next note (0.05..2)
  Tick gap = 0;            // ticks kept free before the next note
  double variation = 0;    // 0..1 random length variation
  bool onlyOverlaps = false;  // just trim notes that overlap the next note
  uint32_t seed = 1;
};
struct ArticulatePreset {
  const char* name;
  ArticulateOpts opts;  // `gap` in 1/48 of a beat
};
const std::vector<ArticulatePreset>& articulatePresets();
ArticulateOpts articulatePresetOpts(int index, int ppq);
void articulate(std::vector<Note>& notes, NoteSel& sel, const ArticulateOpts& o);

// -------------------------------------------------------------------- randomize ----
struct RandomOpts {
  int root = 0;             // 0..11
  int scale = 1;            // scales() index
  int octave = 4;           // lowest octave (C4 = 48 with C5 = 60)
  int octaves = 1;          // range
  int maxPoly = 1;          // simultaneous notes per step
  double probability = 0.5; // chance of a note per step
  double length = 1.0;      // note length in steps
  double lengthVariation = 0.0;
  int velocity = 100;
  int velocityRange = 20;
  Tick step = 24;
  Tick span = 384;          // generated range (ticks from `from`)
  Tick from = 0;
  uint32_t seed = 1;
};
std::vector<Note> randomNotes(const RandomOpts& o);
struct LevelRandom {
  double strength[static_cast<int>(NoteProp::Count)] = {0, 0, 0, 0, 0, 0};  // 0..1 per property
  uint32_t seed = 1;
};
void randomizeLevels(std::vector<Note>& notes, NoteSel& sel, const LevelRandom& o);

// ------------------------------------------------------------------- note LFO ----
enum class LfoWave { Sine = 0, Triangle, Square, Count };
struct NoteLfoOpts {
  NoteProp prop = NoteProp::Velocity;
  double startValue = 0.5, startRange = 0.5;  // 0..1 centre and depth at the start
  double endValue = 0.5, endRange = 0.5;      // … at the end
  Tick startPeriod = 384, endPeriod = 384;    // cycle length (ticks)
  LfoWave wave = LfoWave::Sine;
  double gate = 0.5;  // square: high part of the cycle
};
// Sets the property of each targeted note from an LFO running over the targets' time span.
void noteLfo(std::vector<Note>& notes, NoteSel& sel, const NoteLfoOpts& o);

// --------------------------------------------------------------- scale levels ----
struct ScaleLevelOpts {
  NoteProp prop = NoteProp::Velocity;
  double centre = 0.5;     // 0..1 pivot
  double scale = 1.0;      // -2..2 distance from the pivot is multiplied by this
  double offset = 0.0;     // -1..1 added afterwards
  double rampFrom = 1.0, rampTo = 1.0;  // extra factor ramped over time (fade in / out)
};
void scaleLevels(std::vector<Note>& notes, NoteSel& sel, const ScaleLevelOpts& o);

// ------------------------------------------------------------------------ limit ----
enum class LimitMode { Fold = 0, Clamp, Remove, Count };
const char* limitModeName(LimitMode m);
// Keeps notes between `lo` and `hi` (inclusive): folds by octaves, clamps, or removes.
void limitKeys(std::vector<Note>& notes, NoteSel& sel, int lo, int hi, LimitMode mode);

// -------------------------------------------------------------- riff generator ----
struct RiffOpts {
  int root = 0;         // 0..11
  int scale = 1;
  int octave = 5;       // octave of the root (C5 = 60)
  int range = 7;        // scale degrees the melody may wander up or down
  int bars = 2;
  Tick barTicks = 384;
  Tick step = 24;       // rhythm grid
  double density = 0.55;  // share of steps that start a note
  int rhythm = 0;       // 0 even, 1 syncopated, 2 long notes, 3 driving
  double repeat = 0.5;  // 0..1 how much later bars repeat the first
  double leap = 0.25;   // 0..1 chance of a leap instead of a step
  bool chords = false;  // add a chord (scale triad) on every bar's first note
  int velocity = 100;
  uint32_t seed = 1;
};
const char* riffRhythmName(int r);
std::vector<Note> generateRiff(const RiffOpts& o);

// ------------------------------------------------------------- misc note tools ----
// Sets one property on every targeted note.
void setNotesProp(std::vector<Note>& notes, NoteSel& sel, NoteProp p, double norm);
void setNotesColor(std::vector<Note>& notes, NoteSel& sel, int color);
void setNotesMuted(std::vector<Note>& notes, NoteSel& sel, int mode);  // 0 unmute, 1 mute, 2 toggle
void setNotesSlide(std::vector<Note>& notes, NoteSel& sel, int mode);  // 0 off, 1 on, 2 toggle
void setNotesPorta(std::vector<Note>& notes, NoteSel& sel, int mode);
void selectColor(const std::vector<Note>& notes, NoteSel& sel, int color);
void selectMuted(const std::vector<Note>& notes, NoteSel& sel);
// Detunes the targeted notes by `cents` (fine pitch, clamped).
void detune(std::vector<Note>& notes, NoteSel& sel, int cents);
// Notes crossing tick `t` (start <= t < end).
std::vector<size_t> notesAt(const std::vector<Note>& notes, Tick t);

// ======================================================= step sequencer rows ====
// A channel's step row (velocities, 0 = off) and its per-step parameters.
using StepRow = std::vector<uint8_t>;
using StepPropRow = std::vector<StepProps>;
// Brings the property row to the step row's size (defaults).
void alignStepProps(const StepRow& row, StepPropRow& props);
void fillEvery(StepRow& row, int every, int offset, uint8_t velocity = 100);
// Rotates (wraps) by n steps: n > 0 right.
void rotateSteps(StepRow& row, StepPropRow& props, int n);
// Shifts by n steps without wrapping (steps pushed out are lost).
void shiftSteps(StepRow& row, StepPropRow& props, int n);
void reverseSteps(StepRow& row, StepPropRow& props);
void invertSteps(StepRow& row, uint8_t velocity = 100);
// Inserts an empty step at `at` (the last step falls off) / deletes step `at` (an empty step enters at the end).
void insertStep(StepRow& row, StepPropRow& props, int at);
void deleteStep(StepRow& row, StepPropRow& props, int at);
// Random steps: each step on with `probability`, velocity in [velLo, velHi].
void randomizeSteps(StepRow& row, double probability, int velLo, int velHi, uint32_t seed);
// Evenly spread `hits` over `length` steps (repeated over the row), rotated by `rotation`.
void euclidSteps(StepRow& row, int hits, int length, int rotation, uint8_t velocity = 100);
// Step values the graph editor shows / edits (0..1).
enum class StepLane { Velocity = 0, Pan, Release, Fine, ModX, ModY, Shift, Pitch, Count };
const char* stepLaneName(StepLane l);
double stepLaneNorm(uint8_t vel, const StepProps& p, StepLane l);
void setStepLaneNorm(uint8_t& vel, StepProps& p, StepLane l, double v);
// Converts a channel's steps to piano-roll notes (pitch, properties and shift
// kept) and back. notesToSteps keeps notes that do not fit the step grid
// (returned in `rest`).
std::vector<Note> stepsToNotes(const StepRow& row, const StepPropRow& props, Tick stepTicks, int defaultPitch = 60);
void notesToSteps(const std::vector<Note>& notes, Tick stepTicks, int total, StepRow& row, StepPropRow& props,
                  std::vector<Note>* rest = nullptr, int defaultPitch = 60);

}  // namespace gl::tools

// ========================================================= song-level edits ====
namespace gl::ops {

// Channel rack: per-step parameter rows live in Pattern::stepProps.
// The channel's row and parameters sized to the pattern (missing = off / defaults).
void readStepRow(const Pattern& p, ObjectId channel, tools::StepRow& row, tools::StepPropRow& props);
bool editStepRow(Document& d, ObjectId pattern, ObjectId channel, const std::string& label,
                 const std::function<void(tools::StepRow&, tools::StepPropRow&)>& fn, uint64_t mergeKey = 0);
// Moves (or copies) the channel's steps into piano-roll notes and back.
bool convertStepsToNotes(Document& d, ObjectId pattern, ObjectId channel);
bool convertNotesToSteps(Document& d, ObjectId pattern, ObjectId channel);
// A new pattern holding the channels' data from `pattern` (moved when `move`). Returns its id.
ObjectId channelsToNewPattern(Document& d, ObjectId pattern, const std::vector<ObjectId>& channels, bool move);
bool renameChannelGroup(Document& d, int group, const std::string& name);
// Copies (or moves) notes of one channel to another in the same pattern.
bool sendNotesToChannel(Document& d, ObjectId pattern, ObjectId from, ObjectId to, const std::vector<Note>& notes, bool move,
                        const std::vector<Note>& keep);

// Playlist.
// Splits a pattern / automation clip at song tick `at`; returns the new right-hand clip.
ObjectId sliceClip(Document& d, ObjectId clip, Tick at);
// Slices every clip crossing `at` on `track` (track < 0: all tracks).
std::vector<ObjectId> sliceAt(Document& d, Tick at, int track);
// Shifts a clip's content inside it (slip edit): offset += delta, clamped at 0.
bool slipClip(Document& d, ObjectId clip, Tick delta, uint64_t mergeKey = 0);
// 0 unmute, 1 mute, 2 toggle.
bool muteClips(Document& d, const std::set<ObjectId>& clips, int mode);
bool muteTrackClips(Document& d, int track, bool muted);
// Merges every pattern clip on the track into one pattern clip.
ObjectId mergeTrackPatternClips(Document& d, int track);
// Chops each clip into pieces of `every` ticks.
std::vector<ObjectId> chopClips(Document& d, const std::set<ObjectId>& clips, Tick every);
// Splits one clip's pattern by channel (the clip gets its own pattern first).
std::vector<ObjectId> splitClipByChannel(Document& d, ObjectId clip);
// Clips that play the same source (pattern / audio channel / automation clip).
std::set<ObjectId> clipsOfSameSource(const Song& s, ObjectId clip);
bool setTrackLocked(Document& d, int track, bool locked);
bool setTrackHeights(Document& d, const std::vector<int>& tracks, float height);
// Markers (current arrangement). Positions keep the list sorted.
int addMarker(Document& d, const Marker& m);  // returns its index (overload of MenuOpsExtra's)
bool editMarker(Document& d, int index, const std::string& label, const std::function<void(Marker&)>& fn);
// Adds a marker every `every` ticks over [from, to).
int addMarkersEvery(Document& d, Tick from, Tick to, Tick every);
const char* markerActionName(MarkerAction a);
// The time range a loop marker repeats: from it to the next marker (or the song end).
std::pair<Tick, Tick> markerLoopRange(const Song& s, int index);

}  // namespace gl::ops
