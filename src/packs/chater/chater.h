// Gridline - "chater" pack: Chater (gl.chater), a dreamy, nostalgic and spacey effect with
// gentle glitches and fairy sparkle.
//
// Audio in, audio out. A rolling capture (Catch) feeds the breaks (repeat, reverse, tape,
// scrub, jump, skip) that rearrange the input in time: soft-edged and washed into the space
// at the dreamy end, sharp at the glitch end. A sparkle world (grains pitched up, a pitched
// feedback shimmer, six tuned resonators, a spectral halo) sits on top. Haze (warmth, damp),
// Memory (wow, flutter, worn tape, ensemble, hiss), a long diffuse Room with shimmer and
// freeze, Echo and Tone colour the result. Eight macros drive the whole engine.
//
// Chater does not sequence: breaks fire by chance on the tempo grid (Density, Lean,
// Variation) and/or on the input's own hits (an onset detector), never from a pattern the
// user programs. Every random choice is a hash of (Seed, grid slot) or (Seed, hit number),
// so playback, offline renders and repeated plays give the same result.
//
// Parameter indices are stable (saved in projects); spare indices are reserved after each
// group so later versions can grow without moving anything.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include "../../core/PluginInfo.h"

namespace gl {
class Effect;
}

namespace gl::packs::chater {

inline constexpr const char* kType = "gl.chater";
inline constexpr int kVersion = 2;  // 2: the dreamy remap (Haze, Memory; Glitch moved to tile 6)

enum Param : int {
  // ---- macros (Mix is the Dry/Wet parameter itself)
  PSparkle = 0, PHaze, PMemory, PDrift, PShine, PGlitch, PSpace, PMix,
  // ---- Catch
  PLength = 8, PQuantise, PSmooth, PHold,
  // ---- Triggers (when a break happens)
  PTrigMode = 12, PGrid, PDensity, PVariation, PLean, PSpan, PSensitivity, PHitGap, PHitChance, PSeed,
  // 22, 23 spare
  // ---- break odds
  POddRepeat = 24, POddReverse, POddTape, POddScrub, POddJump, POddSkip,
  // 30, 31 spare
  // ---- Repeat, Tape and scrub
  PDivision = 32, PAccel, PRamp, PGate, PTapeTime, PTapeCurve, PScrubDepth, PScrubRate,
  // ---- Haze (wet path) and soft breaks
  PDamp = 40, PWarmth, PSoftness, PWash,
  // ---- Memory (wet path)
  PWow = 44, PFlutter, PAge, PEnsemble,
  // ---- Glitter
  PGrDensity = 48, PGrSize, PGrScatter, PGrReverse, PGrLevel, PGrWindow, PIntervals, PReach, PDetune, PScale, PRoot, PSnap,
  // ---- Memory, continued
  PHiss = 60, PCrackle, PWowRate,
  // 63 spare
  // ---- Shimmer
  PShFeedback = 64, PShInterval, PShTone, PShSpread,
  // ---- Crystal
  PCrChord = 68, PCrDecay, PCrBright, PCrStrike, PCrSpread, PCrMix,
  // ---- Halo
  PHaLift = 74, PHaFrom, PHaTwinkle, PHaSmear, PHaFreeze, PHaMix,
  // ---- Grit
  PGtBits = 80, PGtRate, PGtSmooth, PGtJitter, PGtDrive, PGtMix,
  // ---- Spray
  PWidth = 86, PPanScatter, PMicroDelay, POrbit,
  // ---- Room
  PRmSize = 90, PRmDecay, PRmDamping, PRmMix,
  // ---- Echo
  PEcTime = 94, PEcFeedback, PEcTone, PEcMix, PEcPingPong,
  // ---- Tone and output
  PLowCut = 99, PHighCut, PTilt, PDuck, POutput, PSafety,
  // ---- Room and Echo, continued
  PRmShimmer = 105, PRmFreeze, PRmMod, PEcDiffuse,
  // 109 .. 111 spare
  // ---- Sways (A then B) and the Follower
  PSwA = 112,  // Shape, Rate, Depth, Phase, Smooth
  PSwB = 117,
  PFoAttack = 122, PFoRelease, PFoGain, PFoFloor,
  // 126, 127 spare
  // ---- Links: 8 slots of (source, target parameter, depth)
  PLink0 = 128,
  // 152 .. 159 spare
  // ---- settings
  PQuality = 160, PHaloEngine, PCaptureUpTo, PTempoMode, PFreeBpm, PRestart, PKeepVibe,
  // 167 spare
  // ---- dice locks: 8 masks of 24 parameters each
  PLock0 = 168,
  PVersion = 176,
  PCount = 177
};

// Sway parameters (offset from PSwA / PSwB).
enum SwayParam : int { SwShape = 0, SwRate, SwDepth, SwPhase, SwSmooth, SwCount };
inline int swayParam(int sway, int k) { return (sway == 0 ? PSwA : PSwB) + k; }
// Link slots.
inline constexpr int kLinks = 8;
enum LinkParam : int { LkSource = 0, LkTarget, LkDepth, LkCount };
inline int linkParam(int slot, int k) { return PLink0 + slot * LkCount + k; }
enum LinkSource : int { SrcOff = 0, SrcSwayA, SrcSwayB, SrcFollower, SrcBreaks, SrcCount };
// Lock masks.
inline constexpr int kLockMasks = 8, kLockBits = 24;

enum TrigMode : int { TrigGrid = 0, TrigHits, TrigBoth };
enum Lean : int { LeanEven = 0, LeanDown, LeanOff, LeanBarEnd };
enum Break : int { BrNone = 0, BrRepeat, BrReverse, BrTape, BrScrub, BrJump, BrSkip, BrCount };
enum Window : int { WinSoft = 0, WinBell, WinSharp, WinClick, WinSwell };
enum GritSmooth : int { GsRaw = 0, GsSoft, GsClean };
enum Quality : int { QLight = 0, QNormal, QHigh };
enum HaloEngine : int { HaloOff = 0, HaloLow, HaloFull };
enum SwayShape : int { ShSine = 0, ShTri, ShRamp, ShSteps, ShDrift };

// Choice tables (beats are quarter notes).
inline constexpr int kGridCount = 7;            // 1/2, 1/4, 1/8, 1/16, 1/32, 1/8T, 1/16T
double gridBeats(int idx);
inline constexpr int kDivisionCount = 10;       // 1/2 .. 1/128, 1/8T, 1/16T, 1/32T
double divisionBeats(int idx);
inline constexpr int kDivisionStraight = 7;     // the first seven are straight (Shatter steps through them)
double lengthBars(int idx);                     // Catch length: 1/4 .. 8 bars
double quantiseBeats(int idx);                  // 1/4 .. 1/64, triplets
double tapeBeats(int idx);                      // 1/16 .. 2 bars (in beats of 4/4)
double scrubBeats(int idx);                     // 1/32 .. 1 bar
double echoBeats(int idx);                      // 1/32 .. 1 bar incl. dotted and triplets
double orbitBeats(int idx);                     // 0 = off
double swayBeats(int idx);                      // 1/32 .. 8 bars
int spanSteps(int idx);                         // 1, 2, 3, 4, 6, 8 grid steps
int captureBars(int idx);                       // 2, 4, 8
// Pitch chips (semitones, bit order of PIntervals) and crystal chords.
inline constexpr int kChips = 8;
inline constexpr std::array<int, kChips> kChipSemis = {-12, 0, 5, 7, 12, 19, 24, 31};
int chordSemis(int chord, int voice);           // six voices per chord
int shimmerSemis(int idx);                      // +7, +12, +19
// Scale degrees (bit mask over 12 semitones; 0 = off).
int scaleMask(int scale);

// ------------------------------------------------------------------ macros
// What a macro drives: each row moves one parameter (or an internal amount) on top of its
// page value. amount = curve(macro) (0 below `gate`); kinds:
//   Add    v + amount * span            (parameter units)
//   Mul    v * (1 + amount * span)      (span < 0 shrinks)
//   Toward v + (span - v) * amount      (span is the target value)
//   Steps  choice + round(amount * span)
//   Inner  an internal amount (no parameter), e.g. the grain pitch wobble
enum MacroKind : int { MkAdd = 0, MkMul, MkToward, MkSteps, MkInner };
enum MacroCurve : int { McLin = 0, McExp2 };
struct MacroRow {
  int macro;
  int param;       // -1 for MkInner
  int kind;
  double span;
  int curve;
  double gate;     // the macro's first `gate` does nothing to this row (0..1)
  const char* label;
};
const std::vector<MacroRow>& macroRows();
double macroAmount(const MacroRow& r, double macro01);
// States saved before version 2 (the glitch-first layout) read as that character: the old
// Glitch macro moves to its new tile, Haze and Memory start at 0, Damp off, no soft edges or wash.
// Works on any float array of PCount values (no allocation: the engine runs it on its own copy).
bool needsMigration(const float* params, size_t n);
void migrateV1(float* params, size_t n);
// The value of `param` with every macro (and link offset) applied (clamped to its range).
// `macros` holds the effective macro values 0..1 (index PSparkle..PSpace).
double applyMacros(int param, double page, const double* macros);
// Internal amounts (MkInner rows): the grain pitch wobble in cents.
double wobbleCents(const double* macros);

// ------------------------------------------------------------------ display
// Short text for a parameter value as the editor shows it ("4.2 kHz", "+35%", "1/16").
std::string display(int param, double v);
// Long name (the knob menu header and hints) and the editor's short label.
const char* shortLabel(int param);
// Parameters a link may target (continuous ones, in page order).
const std::vector<int>& linkTargets();
// Which page a parameter belongs to (0 main, 1 fracture, 2 glitter, 3 colour, 4 space,
// 5 motion, 6 settings) and its colour family (the macro tile it sits under, -1 none).
int pageOf(int param);

// ------------------------------------------------------------------ presets
inline constexpr int kBanks = 8;
const char* bankName(int bank);
struct PresetMeta {
  const char* name;
  int bank;
  const char* description;
  const char* tags;
};
// Factory presets (index into PluginInfo::presets; the first is Init).
const std::vector<PresetMeta>& presetMeta();

// ------------------------------------------------------------------ dice
enum DiceAmount : int { DiceNudge = 0, DiceShake, DiceNew };
// Breaks: the triggers, odds, Catch, Repeat, Tape and scrub.
enum DiceScope : int { ScMacros = 1, ScBreaks = 2, ScGlitter = 4, ScColour = 8, ScSpace = 16, ScMotion = 32 };
inline constexpr int kDiceScopes = 6;
const char* scopeName(int i);  // i = 0..5, the bit 1 << i
// Which scope a parameter rolls with (0: never rolled, e.g. Output, settings, locks).
int diceScope(int param);
bool locked(const std::vector<float>& params, int param);
void setLocked(std::vector<float>& params, int param, bool on);
// A new state: every unlocked parameter in `scopes` moves (Nudge within 10 % of its travel,
// Shake within 30 %, New sound anywhere in its musical range). `bank` >= 0 with keepVibe keeps
// the roll inside that bank's character. Deterministic for a seed. Only `only` (>= 0) rolls
// when given ("Roll this knob only").
std::vector<float> roll(const std::vector<float>& params, int scopes, int amount, uint32_t seed, int bank, int only = -1);

// ------------------------------------------------------------------ live view (editor)
inline constexpr int kTapeCols = 480;     // the glitch tape: four bars of the input, one column each
inline constexpr int kTapeBeats = 16;     // window length in beats (four bars of 4/4)
inline constexpr int kEventRing = 32;
inline constexpr int kViewGrains = 64;
struct FiredBreak {
  uint32_t seq = 0;       // 0 = empty
  double beat = 0;        // where it started (song beats)
  double beats = 0;       // how long it lasts
  int type = BrNone;
  int repeats = 0;        // Repeat: how many slices
  bool hit = false;       // fired by an input hit (else by the grid)
  int64_t frame = 0;      // absolute frame (tests)
};
struct LiveView {
  double beat = 0;        // clock position (beats)
  double tempo = 120;
  double barOrigin = 0, barBeats = 4;  // the tape's four bars start at barOrigin (beats)
  bool playing = false;
  float peak[kTapeCols] = {};
  uint8_t type[kTapeCols] = {};
  FiredBreak events[kEventRing];
  int activeType = BrNone;
  float follower = 0, swayA = 0, swayB = 0;
  float wow = 0;          // the Memory pitch wobble now (cents)
  float room = 0;         // the Room's output level (the Space page's freeze glow)
  float outL = 0, outR = 0;
  struct Grain {
    float pan = 0, semis = 0, life = 0, level = 0;
    bool alive = false;
  } grains[kViewGrains];
  float crystal[6] = {};
  float macros[8] = {};   // effective macro values (links included)
};
// Reads a running instance (false when fx is not Chater).
bool liveView(const Effect* fx, LiveView& out);
// Tests: the breaks fired so far (newest last), up to max.
int firedBreaks(const Effect* fx, FiredBreak* out, int max);
// Tests: last measured process time per block (seconds) and idle flags.
bool idle(const Effect* fx);

}  // namespace gl::packs::chater
