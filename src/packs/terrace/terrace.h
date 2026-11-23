// Gridline — "terrace" pack: Terrace (gl.terrace), a multiband rhythmic gate.
//
// The signal splits into 3 or 4 bands on a Linkwitz-Riley crossover (12, 24 or 48 dB per octave)
// whose bands add back up to the input's level at every frequency: the low bands pass all-pass
// filters that match the phase of the splits above them, so with every lane at rest the sum is
// the input through an all-pass (flat, no notch, no bump). Each band has a volume, a cutoff and a
// pan lane played by the Shaper's lane engine (engine/ShaperSteps.h: free lengths of 1 to 64
// steps, step sizes, swing, chance and repeats per step), with its own depth and curve, plus solo,
// mute and gain. Inside: two LFOs, an envelope follower, a sidechain (another track's level or a
// channel's notes) and 16 routes from those onto band depths, step sizes, swing, gain, cutoff,
// pan, the splits and the mix. 8 patterns of steps share the lane settings and play by the Pattern
// control, a chain or notes, as in the Shaper.
//
// Step data, chances, repeats and the chain live in PluginState::data (text, see Bank below);
// everything else is a parameter.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include "../../core/PluginInfo.h"
#include "../../core/PluginParams.h"
#include "../../core/Types.h"

namespace gl {
class Document;
class Effect;
}  // namespace gl

namespace gl::packs::terrace {

inline constexpr const char* kType = "gl.terrace";
inline constexpr int kBands = 4, kLanes = 3, kPatterns = 8, kSteps = 64, kChain = 16, kRoutes = 16, kLfos = 2;

enum Lane { LnVol = 0, LnCut, LnPan };
// Parameters of one lane of one band.
enum LaneParam { LpSteps = 0, LpSize, LpSwing, LpDepth, LpCurve, LpCount };
// Parameters of one band: its three lanes, then solo, mute and gain.
enum BandParam { BpSolo = kLanes * LpCount, BpMute, BpGain, BpCount };
// Parameters of one LFO.
enum LfoParam { LfSync = 0, LfRate, LfHz, LfShape, LfPhase, LfAmount, LfRetrig, LfCount };
// Parameters of one route.
enum RouteParam { RtSource = 0, RtTarget, RtAmount, RtCount };

enum Param {
  TrBands = 0,  // choice: 3 or 4 bands
  TrSplit1,     // Hz: the crossover splits (low to high)
  TrSplit2,
  TrSplit3,
  TrSlope,      // choice: 12, 24, 48 dB per octave
  TrLength,     // choice: the pattern length (Auto step size, the chain's slot, LFO restarts)
  TrSmooth,     // ms: smoothing of every lane
  TrOutput,     // dB
  TrMix,        // 0..1: the wet against the dry (the dry phase-matched to the bands)
  TrPattern,    // choice 1..8: the pattern played without the chain or notes
  TrChain,      // toggle: play the chain
  TrBand0,                                            // kBands x BpCount
  TrLfo0 = TrBand0 + kBands * BpCount,                // kLfos x LfCount
  TrFolSource = TrLfo0 + kLfos * LfCount,             // choice: the input or one band
  TrFolAttack,                                        // ms
  TrFolRelease,                                       // ms
  TrFolGain,                                          // dB
  TrFolAmount,                                        // -1..1
  TrScIn,                                             // side input route (PluginInfo::sideInParam)
  TrScListen,                                         // choice: Level, Notes, Both
  TrScThreshold,                                      // dB
  TrScRelease,                                        // ms
  TrScAmount,                                         // -1..1
  TrScRetrig,                                         // toggle: each note restarts the lanes
  TrNoteSrc,                                          // choice: NoteSource
  TrNotePort,                                         // -1 any MIDI Out port, else 0..255
  TrNotePatterns,                                     // toggle: notes from TrNoteBase up pick patterns 1..8
  TrNoteBase,                                         // note
  TrRoute0,                                           // kRoutes x RtCount
  TrCount = TrRoute0 + kRoutes * RtCount
};

inline constexpr int bandParam(int b, int k) { return TrBand0 + b * BpCount + k; }
inline constexpr int laneParam(int b, int lane, int k) { return TrBand0 + b * BpCount + lane * LpCount + k; }
inline constexpr int lfoParam(int i, int k) { return TrLfo0 + i * LfCount + k; }
inline constexpr int routeParam(int r, int k) { return TrRoute0 + r * RtCount + k; }

// Where the notes come from (as Tideline's note trigger).
enum NoteSource { NsPorts = 0, NsChannel, NsInput };
enum Listen { LsLevel = 0, LsNotes, LsBoth };
enum Slope { Slope12 = 0, Slope24, Slope48 };

// Modulation sources and targets of the routes.
enum Source { SrcOff = 0, SrcLfo1, SrcLfo2, SrcFollower, SrcSidechain, SrcCount };
enum BandTarget { TbVolDepth = 0, TbCutDepth, TbPanDepth, TbSize, TbSwing, TbGain, TbCutoff, TbPan, TbCount };
enum Target { TgNone = 0, TgBand0 = 1, TgSplit1 = TgBand0 + kBands * TbCount, TgSplit2, TgSplit3, TgMix, TgOutput, TgCount };
inline constexpr int bandTarget(int b, int k) { return TgBand0 + b * TbCount + k; }

// LFO rates (synced) in beats, and their names.
inline constexpr int kLfoRates = 15;
inline constexpr double kLfoRateBeats[kLfoRates] = {0.125, 1.0 / 6, 0.25, 1.0 / 3, 0.5, 0.75, 2.0 / 3, 1, 1.5, 2, 3, 4, 8, 16, 32};
inline const char* const kLfoRateNames[kLfoRates] = {"1/32", "1/16T", "1/16", "1/8T", "1/8", "3/16", "1/4T", "1/4",
                                                     "3/8",  "1/2 bar", "3/4 bar", "1 bar", "2 bars", "4 bars", "8 bars"};
enum LfoShape { ShSine = 0, ShTriangle, ShSawUp, ShSawDown, ShSquare, ShRandom, ShSmoothRandom, ShCount };
// The pattern lengths in beats (TrLength).
inline constexpr double kLengthBeats[4] = {2, 4, 8, 16};

// The heading a factory preset sits under in the preset list ("" for a name that is not one).
struct PresetGroupEntry {
  const char* name;
  const char* group;
};
const char* presetGroup(const std::string& name);

// Band names for 3 and 4 bands.
const char* bandName(int band, int bands);       // "Low", "Low mid", ... (translated by the caller)
const char* bandCaps(int band, int bands);       // "LOW", "LOW MID", ...
std::string targetName(int target, int bands);   // "Low mid depth", "Split 2", ... (English)

// A lane's default level, and the level a step plays when its chance skips it (or between
// repeats): silence for volume, the open filter for cutoff, the centre for pan.
inline float laneDefault(int lane) { return lane == LnPan ? 0.f : 1.f; }
inline float laneRest(int lane) { return lane == LnVol ? 0.f : lane == LnCut ? 1.f : 0.f; }

// ---- the step bank: 8 patterns x 4 bands x 3 lanes of 64 steps, chance and repeats, the chain.
// Text in PluginState::data (steps at their default are left out; "" = everything at rest):
//   TR1
//   v <pattern 0..7> <band 0..3> <lane 0..2> <first step> <level x 10000> ...
//   q <pattern> <band> <lane> <first step> <chance 0..100> ...
//   r <pattern> <band> <lane> <first step> <repeats 1..4> ...
//   n <slot pattern 1..8, 0 empty> ...        (the chain, up to 16 slots)
//   channel=<id>                              (the note source channel, as Tideline keeps it)
struct Bank {
  float val[kPatterns][kBands][kLanes][kSteps];
  uint8_t chance[kPatterns][kBands][kLanes][kSteps];  // percent
  uint8_t hits[kPatterns][kBands][kLanes][kSteps];    // 1..4
  bool anyChance[kPatterns][kBands][kLanes], anyHits[kPatterns][kBands][kLanes];
  int8_t chain[kChain];  // -1 empty, else pattern 0..7
  int chainLen;          // slots up to the last filled one (0: no chain)
};
void clear(Bank& b);
void finish(Bank& b);                   // recomputes the flags and the chain length
void parse(const char* text, Bank& b);  // clear() first, then the text (no allocation)
std::string write(const Bank& b);       // the step lines ("" when everything is at its default)
// The state text with the steps of `b` and the note source channel kept.
std::string withBank(const std::string& data, const Bank& b);
uint32_t noteChannel(const std::string& data);
std::string withNoteChannel(const std::string& data, uint32_t channel);

// ---- the crossover (shared by the DSP and the tests): 3 or 4 bands of one channel.
struct Section {
  double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
  double process(double x) {
    const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1, x1 = x, y2 = y1, y1 = y;
    if (y1 > -1e-20 && y1 < 1e-20) y1 = 0;
    return y;
  }
  void clear() { x1 = x2 = y1 = y2 = 0; }
};
struct Crossover {
  static constexpr int kMaxSec = 4, kMaxAp = 2;
  int bands = 4, slope = Slope24, secs = 2, apSecs = 1;
  double sign = 1;
  double sr = 0, hz[3] = {-1, -1, -1};
  Section lp[3][kMaxSec], hp[3][kMaxSec];
  Section apBand[3][3][kMaxAp];  // [band][split][section]: band i passes the all-passes of the splits above it
  Section apDry[3][kMaxAp];      // the dry signal through every split's all-pass
  // Sets the splits (Hz, ascending), the slope and the band count; filters keep their memory.
  void set(double sampleRate, int bandCount, int slopeChoice, const double* splits);
  void split(double x, double* out);  // out[0 .. bands-1]
  double dry(double x);               // x through the all-pass the bands add up to
  void clear();
};

// ---- telemetry for the editor (written by the audio thread, read by the UI).
struct Live {
  float play[kBands][kLanes] = {};  // where each lane plays: (step + fraction) / steps, -1 stopped
  float level[kBands] = {};         // each band's output peak (decaying)
  float lfo[kLfos] = {}, follower = 0, sidechain = 0;
  int pattern = 0, chainSlot = -1;
  bool playing = false;
};
// Reads a running instance (false when fx is not Terrace).
bool liveView(const Effect* fx, Live& out);

// ---- sources in the project (one undo step each; shared by the editor and the tests).
// Listens to mixer track `from`'s level: a sidechain-only send into the slot's track (or the
// route already there); Listen becomes Level unless it was Both.
bool linkLevelTrack(Document& d, ObjectId owner, int slot, ObjectId from);
// Listens to channel `channel`: its notes (Listen Notes), its level (its track routed in, Listen
// Level) or both, by the current Listen.
bool linkChannel(Document& d, ObjectId owner, int slot, ObjectId channel);
// Notes from MIDI Out ports (`port` -1: any) or the MIDI input; forgets the channel.
bool setNoteSource(Document& d, ObjectId owner, int slot, int src, int port = -1);
// No level source (the slot's own input is not used: Sidechain input none).
bool clearLevelSource(Document& d, ObjectId owner, int slot);

}  // namespace gl::packs::terrace
