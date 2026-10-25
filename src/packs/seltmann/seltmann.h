// Gridline — "seltmann" pack: Seltmann (gl.seltmann), the automatic sample
// chopper and loop maker. Shared by the catalogue, the analysis, the runtime,
// the editor and the tests.
//
//   analysis  tempo (multi-band spectral flux, tempogram, octave choice), beat
//             tracking (dynamic programming that follows drift), meter and
//             downbeats, sections (self-similarity + novelty), loopable phrases,
//             key; runs on worker threads with progress and a cache.
//   model     the beat grid (user-correctable), the region being chopped, the
//             slices with their settings, and the pattern (chop layout) that
//             plays them; stored as text in PluginState::data after the path.
//   presets   chop layouts in six categories (Phrase, Loop, Breaks, Micro,
//             Fills, Creative) written in a small step language.
//   runtime   instrument: slices on keys, the pattern on the octave below,
//             in sync with the project (resampled or time-stretched).
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "../../core/Types.h"
#include "../../engine/Sample.h"

namespace gl {
struct Song;
class Generator;
}

namespace gl::packs::seltmann {

inline constexpr const char* kType = "gl.seltmann";

// ------------------------------------------------------------------ parameters
enum Param {
  SmVolume = 0,   // 0..1.25 output level
  SmPan,          // -1..1
  SmBaseNote,     // 0..127 first slice key (C3 = 60)
  SmKeyMap,       // choice: Slices / Slot letters
  SmSync,         // choice: Original / Resample / Stretch
  SmStretchMode,  // choice: Auto / Transient / Mono (J-87 modes for Stretch)
  SmLock,         // toggle: pattern follows the song position
  SmMono,         // toggle: one voice at a time
  SmAttack,       // ms
  SmDecay,        // ms (0 = off: hold the sustain level from the start)
  SmSustain,      // 0..1
  SmRelease,      // ms
  SmDeclick,      // ms edge fades of every slice
  SmSwing,        // 0..1 pattern swing (odd steps later)
  SmPitch,        // -24..24 semitones, every slice
  SmVelocity,     // 0..1 velocity sensitivity
  SmReverse,      // toggle: every slice reversed
  SmGate,         // 0.05..1 pattern step length
  SmSpeed,        // choice: Half / Normal / Double pattern speed
  SmCutoff,       // Hz low-pass
  SmReso,         // 0..1
  SmCount
};
enum KeyMap { KeySlices = 0, KeySlots };
enum Sync { SyncOriginal = 0, SyncResample, SyncStretch };
enum Speed { SpeedHalf = 0, SpeedNormal, SpeedDouble };

inline constexpr int kMaxVoices = 32;
inline constexpr int kMaxSlices = 512;
inline constexpr int kMaxSteps = 512;
inline constexpr int kSlotCount = 26;  // A..Z

// ------------------------------------------------------------------ analysis
struct Section {
  int firstBeat = 0, endBeat = 0;  // beat indices [first, end)
  int label = 0;                   // 0 = A, 1 = B, ...
};
struct LoopSpot {
  int firstBeat = 0;  // a downbeat
  int bars = 1;
  float score = 0;    // 0..1
};
// What the material is (drives the automatic chop).
enum Kind { KindUnknown = 0, KindBreak, KindLoop, KindSong, KindOneShot, KindAmbient, KindCount };
const char* kindName(int kind);  // "break", "loop", ... (stable, not translated)

// What the file name says (sample-pack style "_92_", "92 BPM", Tracklib "--110.00bpm-", keys "Am", "F# min").
struct AnalyseHints {
  double bpm = 0;     // 0 = none
  bool strong = false;  // the name says "bpm" (else a bare number in a sample-pack name)
  int key = -1;       // 0..11
  bool minor = false;
};
AnalyseHints analyseHintsFor(const std::string& path);

struct Analysis {
  double sampleRate = 0;
  int64_t frames = 0;
  double bpm = 0;            // final tempo (beat regression)
  double confidence = 0;     // 0..1 tempo confidence
  double halfScore = 0, doubleScore = 0;  // relative support of bpm/2 and bpm*2 (0..1)
  std::vector<double> beats; // beat frames (fractional)
  int beatsPerBar = 4;
  bool compound = false;     // ternary subdivision (6/8 when grouped in two)
  int bar1 = 0;              // index of the first downbeat in beats
  double meterConfidence = 0;
  double downbeatConfidence = 0;
  std::vector<Section> sections;
  std::vector<LoopSpot> loops;
  int key = -1;              // 0..11 tonic (C = 0), -1 unknown
  bool minor = false;
  double keyConfidence = 0;
  int kind = KindUnknown;
  int64_t contentStart = 0;  // first frame after leading silence
  int64_t contentEnd = 0;    // end of the audible part (0 = the file end)
  bool loopFit = false;      // the grid was fitted to the loop length (whole bars)
  double nameBpm = 0;        // the file-name tempo that was used as a prior (0 none)
  double onsetRate = 0;      // onsets per second of the audible part (kind features, not stored)
  double tonality = 0;       // chroma peak-to-mean (drums ~1.5..2.5, tonal material higher)
  std::vector<float> onsetEnv;  // onset strength per analysis frame (display)
  double envRate = 100;         // frames per second of onsetEnv
};

// Runs the full analysis on a sample (any thread). `progress` (0..1) and
// `cancel` may be null. Returns false when cancelled or the sample is empty.
bool analyse(const Sample& s, Analysis& out, std::atomic<float>* progress = nullptr, const std::atomic<bool>* cancel = nullptr,
             const AnalyseHints& hints = AnalyseHints{});
// Tempo-only pieces (tests, the validation tool).
std::vector<float> onsetEnvelope(const float* mono, int64_t n, double rate, double& envRate, std::vector<float>* lowBand = nullptr,
                                 std::vector<float>* midBand = nullptr);

// Min / max peaks of a sample (both channels mixed) at kPeakBlock frames per entry.
inline constexpr int kPeakBlock = 256;
struct Peaks {
  std::vector<float> mn, mx, rms;
  int64_t frames = 0;
};
void buildPeaks(const Sample& s, Peaks& p);

// Background analysis (UI): one job per file, shared. The job decodes the file
// (sample cache), builds peaks and analyses it on its own thread; results are
// cached in memory and, with a cache directory, on disk.
struct Job {
  std::string path;
  std::atomic<float> progress{0};
  std::atomic<int> state{0};  // 0 running, 1 done, 2 failed
  std::atomic<bool> cancel{false};
  SamplePtr sample;           // valid once state == 1
  Analysis result;            // valid once state == 1
  Peaks peaks;                // valid once state == 1
  std::string error;
  bool fromCache = false;     // analysis came from the disk cache
  std::thread::id worker;     // the thread that ran it (tests)
};
using JobPtr = std::shared_ptr<Job>;
// Starts (or returns the running / finished) job for a file. Never blocks.
JobPtr requestAnalysis(const std::string& path);
// Drops a finished job (re-analysis) and the disk cache entry.
void forgetAnalysis(const std::string& path);
void setAnalysisCacheDir(const std::string& dir);
// Waits for every job (tests, shutdown).
void waitAnalysisIdle();
// Forgets the finished jobs kept in memory (the disk cache stays).
void dropAnalysisJobs();
// Analysis text (same format the model stores) for the disk cache.
std::string analysisToText(const Analysis& a);
bool analysisFromText(const std::string& text, Analysis& a);

// ------------------------------------------------------------------ model
enum ChopMode { ChopGrid = 0, ChopTransients, ChopSections, ChopEqual, ChopGrains, ChopManual, ChopModeCount };
// Grid divisions (beats per slice in 4/4: a bar = beatsPerBar beats).
enum Division { Div8Bars = 0, Div4Bars, Div2Bars, Div1Bar, DivHalf, DivBeat, Div8th, Div16th, Div32nd, Div64th, Div8T, Div16T, DivCount };
// Beats of a division (bar divisions follow the meter).
double divisionBeats(int div, int beatsPerBar);
const char* divisionName(int div);
enum Snap { SnapNone = 0, SnapBeat, SnapTransient, SnapZero };

struct Slice {
  int64_t start = 0, end = 0;  // source frames [start, end)
  float pitch = 0;             // semitones
  float gain = 0;              // dB
  bool reverse = false;
  float fadeIn = 0, fadeOut = 0;  // ms (on top of the declick)
  float offset = 0;            // 0..0.9 share of the slice skipped at the start
  float length = 1;            // 0.05..1 share of the slice played
  int choke = 0;               // 0 none, 1..8
  bool operator==(const Slice& o) const;
};
struct Chop {
  int mode = ChopGrid;
  int division = Div1Bar;
  float sensitivity = 0.5f;  // transients
  int count = 8;             // equal slices
  float grainMs = 60;        // grains
  int snap = SnapBeat;       // manual edits
  bool zeroCross = true;     // move cut points to the nearest zero crossing
  bool autoSize = false;     // the chop size follows the material (resolveAutoChop)
  bool hitSnap = false;      // grid cuts move onto the nearest hit (within a quarter of a cell and 50 ms)
  bool operator==(const Chop& o) const;
};
struct Step {
  int src = 0;        // >= 0 slot (0 = A) when slot, else slice index; rests: kRest
  bool slot = true;
  int ties = 0;       // extra units this step lasts
  bool reverse = false;
  float pitch = 0;    // semitones
  float gain = 0;     // dB
  float length = 1;   // 0.05..1 share of the step that sounds
  int stutter = 1;    // 1..16 repeats inside the step
  float offset = 0;   // 0..0.9 share of the slice skipped
  bool operator==(const Step& o) const;
};
inline constexpr int kRest = -1;
struct Pattern {
  std::string name = "Straight";
  double unitBeats = 4;      // length of one step (beats)
  std::vector<Step> steps;
  std::vector<int> slots;    // slot letter -> slice index (-1 = none)
  bool operator==(const Pattern& o) const;
  double beats() const;      // whole pattern length in beats
};
struct Model {
  int64_t srcFrames = 0;
  double srcRate = 0;
  Analysis grid;             // analysis results (beats / bars / sections / loops / key), user-corrected
  bool analysed = false;     // the grid holds a real analysis (else a flat default)
  int64_t regionStart = 0, regionEnd = 0;  // frames
  Chop chop;
  std::vector<Slice> slices;
  Pattern pattern;
  std::string preset;        // last applied layout preset
};

// "<path>\n<model>" <-> path + model. A bare path yields hasModel = false.
std::string pathOf(const std::string& data);
bool parseData(const std::string& data, std::string& path, Model& m);
std::string formatData(const std::string& path, const Model& m);
std::string modelToText(const Model& m);
bool modelFromText(const std::string& text, Model& m);

// Beat position (fractional beat index) of a frame and back, following the grid
// (linear between beats, the edge periods outside).
double beatAt(const Analysis& g, double frame);
double frameAt(const Analysis& g, double beat);
// Mean beat length in frames (from the bpm).
double beatFrames(const Analysis& g);

// A fresh model for a sample and its analysis: region = the strongest loop
// spot (or the first 4 bars from bar 1), chop by bars, the default pattern.
Model defaultModel(const Sample& s, const Analysis& a);
// A flat grid (bpm guess 120, 4/4) when there is no analysis.
Analysis flatGrid(const Sample& s, double bpm = 120);

// Re-slices the region with the model's chop settings (sample for transients /
// zero crossings, may be null). Keeps per-slice settings by index when the
// count is unchanged; resets the slots to A..Z = slices 0..25. Grid chops keep
// the region on the grid (a partial cell under half a cell at either end is
// dropped); fragments shorter than degenerateLimit() merge into a neighbour.
void rechop(Model& m, const Sample* s);
// Chop size for the material (kind and region length) when chop.autoSize is set:
// breaks 1/16 (a bar or less), 1/8 (up to 4 bars) or beats, cuts on the hits;
// melodic loops beats / half bars / bars; songs bars; one-shots at the
// transients; ambient material in 8 equal parts.
void resolveAutoChop(Model& m);
// Length of the region in bars of the grid.
double regionBars(const Model& m);
// The pattern that plays every slice once, in order (unit = the slice length on grids).
void straightPattern(Model& m);
// Steps that refer to a slice that is not there fold onto the slices that are.
void foldSteps(Model& m);
// Re-chops after a chop change and keeps the pattern meaningful: the straight pattern
// follows the new slices, a layout's steps fold onto them.
void rechopKeepPattern(Model& m, const Sample* s);
// Region helpers: snapped to downbeats (whole bars), at least one bar.
void setRegionBars(Model& m, int firstBar, int bars);
int barCount(const Model& m);           // bars from bar 1 to the end of the file
double barStartFrame(const Model& m, int bar);  // bar 0 = bar 1 of the grid
// Snapping of a frame for manual edits.
int64_t snapFrame(const Model& m, const Sample* s, int64_t frame, int snap);
int64_t nearestZeroCrossing(const Sample& s, int64_t frame, int64_t radius);
// Shortest slice a chop may make (frames): shorter fragments are merged into a neighbour.
double degenerateLimit(const Model& m);
// Manual edits (switch the chop to Manual).
int addCut(Model& m, int64_t frame);    // returns the new slice index (-1 none)
bool removeCut(Model& m, int slice);    // merges slice into the previous one
bool moveCut(Model& m, int slice, int64_t frame);  // moves the start of `slice`

// Grid corrections. All keep the grid sorted and inside the file.
void gridSetBpm(Analysis& g, double bpm, double anchorFrame);  // straight grid through the anchor
void gridDouble(Analysis& g);
void gridHalve(Analysis& g);
void gridNudge(Analysis& g, double frames);
void gridSetBar1(Analysis& g, int beat);
void gridMoveDownbeat(Analysis& g, double frame);  // bar 1 lands on frame (whole grid shifts)
void gridStretchTo(Analysis& g, int beat, double frame);  // bar 1 stays, `beat` lands on frame, later beats follow
void gridSetMeter(Analysis& g, int beatsPerBar);
double tapTempo(const std::vector<double>& tapSeconds);  // BPM of taps (0 = not enough)

// ------------------------------------------------------------------ patterns
std::string patternToText(const Pattern& p);  // steps in the DSL
bool patternFromText(const std::string& steps, Pattern& p, std::string* err = nullptr);
std::string stepToText(const Step& s);
// Slice played by a step (-1 rest / unassigned).
int stepSlice(const Pattern& p, const Step& s, int sliceCount);
// Step start (beats from the pattern start, swing applied) and length (beats).
double stepStart(const Pattern& p, int i, double swing);
double stepBeats(const Pattern& p, int i);
const char* slotName(int slot);  // "A".."Z"

enum Category { CatPhrase = 0, CatLoop, CatBreaks, CatMicro, CatFills, CatCreative, CatFlip, CatUser, CatCount };
const char* categoryName(int c);
struct Preset {
  std::string name;
  int category = CatPhrase;
  std::string hint;
  Chop chop;
  int unitDiv = Div1Bar;      // step length as a division (< 0: unitBeats)
  double unitBeats = 0;       // step length in beats (user presets)
  std::string steps;          // DSL ("" = generated by `gen`)
  int gen = 0;                // generator id (0 none)
  float swing = -1;           // < 0 = keep
  int speed = -1;             // < 0 = keep
  int regionBars = 0;         // > 0: region length the layout wants (bars)
};
const std::vector<Preset>& factoryPresets();
// The layout's steps as a short readable line for its card ("A B A C", "1 2 3 4 5 6 ...",
// "random take" for generated layouts), at most `maxSteps` steps.
std::string presetSummary(const Preset& p, int maxSteps = 8);
// Editor tabs grouping the categories: Flip, Phrase (+ Loop), Breaks (+ Fills), Micro, Creative, Mine.
enum PresetTab { TabFlip = 0, TabPhrase, TabBreaks, TabMicro, TabCreative, TabMine, TabCount };
const char* presetTabName(int tab);
int presetTabOf(int category);
// Applies a layout: chop settings, region length (when it asks for one), re-chop,
// the pattern (generated ones use the key and a seed). Material shorter than the
// layout's region keeps the layout's slice count with a finer chop (and, when the
// steps play the slices at their own length, a step scaled the same); sounds
// shorter than a bar are cut into that many equal parts. Steps never refer to a
// slice that is not there (they fold onto the slices that are). Returns the param
// overrides it wants (swing / speed) as (index, value) pairs.
std::vector<std::pair<int, float>> applyPreset(Model& m, const Sample* s, const Preset& p, uint32_t seed = 1);
// User presets: one file per preset (<dir>/<name>.seltmann).
bool saveUserPreset(const std::string& dir, const std::string& name, const Model& m, float swing, int speed);
std::vector<Preset> loadUserPresets(const std::string& dir);
std::string presetToText(const Preset& p);
bool presetFromText(const std::string& text, Preset& p);

// ------------------------------------------------------------------ rendering (UI / worker threads)
struct RenderOpts {
  double sampleRate = 44100;
  double tempo = 120;         // project tempo
  int sync = SyncStretch;
  int stretchMode = 0;        // 0 Auto, 1 Transient, 2 Mono
  std::vector<float> params;  // plugin params (empty = defaults)
};
// The pattern rendered as a seamless loop of exactly one cycle at the project tempo.
bool renderPatternLoop(const Model& m, SamplePtr src, const RenderOpts& o, std::vector<float>& L, std::vector<float>& R);
// One slice as it sounds on its key.
bool renderSlice(const Model& m, SamplePtr src, int slice, const RenderOpts& o, std::vector<float>& L, std::vector<float>& R);
// A span of the source (beats [b0, b1)) warped to the project tempo with a crossfaded loop seam.
bool renderSpan(const Model& m, SamplePtr src, double b0, double b1, const RenderOpts& o, std::vector<float>& L, std::vector<float>& R);
// The pattern as notes for one cycle: (start beat, length beat, key, velocity, modX 0..255, modY 0..255).
struct PatNote {
  double start = 0, length = 0;
  int key = 60, velocity = 100, modX = 128, modY = 128;
};
std::vector<PatNote> patternNotes(const Model& m, int baseNote, float swing, float gate);
// Writes a 24-bit WAV (true on success).
bool writeLoopWav(const std::string& path, const std::vector<float>& L, const std::vector<float>& R, double rate);
// Writes a one-track MIDI file of notes (beats at `ppq`).
bool writePatternMidi(const std::string& path, const std::vector<PatNote>& notes, double tempo);

// ------------------------------------------------------------------ project items
// A new pattern holding the steps as notes for `channel` (slice keys from the
// base note; mod X = step pitch, mod Y = reverse, stutters expanded), `cycles`
// times; with track >= 0 also a playlist clip of it at `at`. Returns the pattern id.
ObjectId sendPatternToSong(Song& s, ObjectId channel, const Model& m, const std::vector<float>& params, int cycles = 1, int track = -1,
                           Tick at = 0);
// Bars a rendered pattern loop lasts at its pattern speed (in the song's bars).
double patternBars(const Model& m, const std::vector<float>& params, int beatsPerBar);

// ------------------------------------------------------------------ live view (UI <- audio)
// Generator::displayStep() packs what is playing: -1 idle, else
// (step + 1) * 4096 + (slice + 1)  (step 0 = no pattern step).
inline int packDisplay(int step, int slice) { return (step + 1) * 4096 + (slice + 1); }
inline int displayStepOf(int v) { return v < 0 ? -1 : v / 4096 - 1; }
inline int displaySliceOf(int v) { return v < 0 ? -1 : v % 4096 - 1; }

// What an instance is playing (UI thread, read from the audio thread's atomics):
// the step and slice last started, the source frame under the newest voice
// (-1 = silent), the position in the pattern (0..1, -1 = not running) and
// whether the pattern runs (J-110: latched by its key; cleared by the key's
// release, Stop, the panic, a focus loss's release).
struct LiveView {
  int step = -1, slice = -1;
  double srcFrame = -1;
  double patternPos = -1;
  bool patternRunning = false;
};
bool liveView(const Generator* g, LiveView& out);

// ------------------------------------------------------------------ command line
// `gridline seltmann-analyse ...` (seltmann_cli_dsp.cpp).
int cliMain(int argc, char** argv);

}  // namespace gl::packs::seltmann
