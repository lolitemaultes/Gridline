// Gridline — Kerf's slicing on the beat grid (J-190): a loop cut into bars,
// beats, 8ths, 16ths, 32nds or triplets along its detected beats (tempo,
// downbeat, meter and drift from Seltmann's beat analysis, run on its worker
// threads and cached per file), so a loop that drifts or starts with a pickup
// still cuts on its beats. The cuts are Seltmann's grid chop over the whole
// loop (optionally moved onto the nearest hits, then the nearest zero
// crossing). Without a usable analysis (still running in the app, failed, or
// switched off) the grid is the loop's length in beats (Kerf's Fit tempo /
// loop beats). UI and program builds only; never the audio thread.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "KerfMarks.h"
#include "Sample.h"

namespace gl {
struct PluginState;
}
namespace gl::packs::seltmann {
struct Analysis;
struct Job;
}  // namespace gl::packs::seltmann

namespace gl::kerfgrid {

// Cell sizes (gl.slicer SlGridDiv) and meters (SlGridMeter; Detected = the analysis').
enum Division { DivBar, DivHalfBar, DivBeat, Div8th, Div16th, Div32nd, Div4T, Div8T, Div16T, DivCount };
enum Meter { MeterDetected, Meter24, Meter34, Meter44, Meter54, Meter68, MeterCount };
const char* divisionName(int div);  // "1 bar", "1/2 bar", "1/4", ... "1/16 T" (stable, not translated)
const char* meterName(int meter);   // "Detected", "2/4", ... "6/8"
// Beats of one cell: a bar holds `beatsPerBar` beats; note values count quarter
// notes, or dotted quarters in a compound meter (6/8: 1/8 = a third of a beat).
double cellBeats(int div, int beatsPerBar, bool compound);

struct Settings {
  int division = Div16th;
  int meter = MeterDetected;
  bool hits = true;      // cuts move onto the nearest hit (within a quarter of a cell and 50 ms)
  bool zero = true;      // then onto the nearest zero crossing (1.5 ms)
  bool follow = true;    // the detected beats (else the loop-length grid)
  double loopBeats = 4;  // SlLoopBeats
  bool fitted = false;   // SlFitTempo: the loop is `loopBeats` long (else the power-of-two guess)
};
Settings settingsOf(const PluginState& st);

enum Source { SourceDetected, SourceLoop };
struct Result {
  int source = SourceLoop;
  bool analysing = false;  // the analysis still runs (the loop-length grid meanwhile)
  bool failed = false;     // it found no beat (the loop-length grid)
  double bpm = 0;          // tempo of the grid used
  int beatsPerBar = 4;
  bool compound = false;
  std::shared_ptr<const packs::seltmann::Analysis> grid;  // beats used (meter applied)
  std::shared_ptr<packs::seltmann::Job> job;               // the analysis (progress)
  std::vector<int64_t> cuts;  // slice starts, at most pp::kMaxSlices
  int found = 0;              // cuts on the grid before that cap
  float progress() const;     // 0..1 while analysing
  kerfmarks::Grid timeGrid() const;  // for the ruler, grid lines, snapping and positions
};
using ResultPtr = std::shared_ptr<const Result>;

// The grid and cuts of a loop file (decoded as `s`). `wait`: block until its beat
// analysis is done (offline renders, exports, tests); else never block: while it
// runs the result is the loop-length grid with `analysing` set, and
// packs::seltmann::analysisGeneration() changes once it is done. Cached.
ResultPtr resultFor(const std::string& path, const SamplePtr& s, const Settings& set, bool wait);
// resultFor with the state's sample path and settings (null: no sample).
ResultPtr resultFor(const PluginState& st, const SamplePtr& s, bool wait);
// Changes every time a beat analysis finishes (the app rebuilds its program then).
uint64_t analysisGeneration();

// Pure pieces (tests): the loop-length grid (`beats` straight beats over the
// sample, bar 1 at frame 0) and the cuts of a sample on a grid.
packs::seltmann::Analysis loopGrid(const Sample& s, double beats, int beatsPerBar);
std::vector<int64_t> cutsOn(const Sample& s, const packs::seltmann::Analysis& g, double cellBeats, bool hits, bool zero);

}  // namespace gl::kerfgrid
