// Gridline — "plasma" pack: Plasma (gl.plasma), a wavetable-centred
// subtractive / hybrid synthesizer: three main oscillators (wavetable,
// multisample, sample, granular, spectral) with two warp slots and up to 16
// unison voices each, a sub and a noise oscillator, two per-voice filters, four
// envelopes, ten LFOs (drawn, path, chaos, sample & hold), eight macros, a
// 64-slot modulation matrix, three effect racks with splitters, an
// arpeggiator and a clip player, and import of Serum 2 presets.
//
// Shared by the catalogue (plasma_info.cpp), the runtime (plasma_*_dsp.cpp),
// the editor (plasma_*_ui.cpp) and the tests.
//
// Parameter layout: fixed blocks (global, oscillators, sub, noise, filters,
// routing, envelopes, LFOs, macros, LFO busses, retriggers, matrix slots,
// effect slots). Curved controls (levels, times, LFO rates) keep a 0..1
// control value (unit "%") so automation follows the same taper as the
// editor; the runtime converts with the functions below.
#pragma once
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "../../core/PluginInfo.h"

namespace gl {
struct Json;
class Generator;
}

namespace gl::packs::plasma {

inline constexpr const char* kType = "gl.plasma";

inline constexpr int kOscs = 3;       // A, B, C
inline constexpr int kFilters = 2;
inline constexpr int kEnvs = 4;
inline constexpr int kLfos = 10;
inline constexpr int kMacros = 8;
inline constexpr int kLfoBusses = 16;
inline constexpr int kModSlots = 64;
inline constexpr int kFxSlots = 24;
inline constexpr int kFxParams = 28;
inline constexpr int kRoutes = 7;     // A, B, C, Noise, Sub, Filter 1, Filter 2
inline constexpr int kMaxUnison = 16;
inline constexpr int kMaxVoices = 32;  // played notes (unison voices are inside a note)
inline constexpr int kArpSlots = 12;
inline constexpr int kClipSlots = 12;
inline constexpr int kRetrigs = 19;

// ------------------------------------------------------------------ global
enum Global : int {
  G_MasterVol = 0,  // 0..1 control (1.41 * v^2)
  G_MasterTune,     // -100..100 cents
  G_Amp,            // 0..1 voice amp (ENV 1 depth)
  G_PortaTime,      // 0..1 control (8 s * v^5)
  G_PortaCurve,     // -1..1
  G_PortaAlways,
  G_PortaScaled,
  G_BendUp,         // -48..48 st
  G_BendDown,       // -48..48 st
  G_PitchWheel,     // -1..1 (wheel position)
  G_ModWheel,       // 0..1
  G_Mono,
  G_Legato,
  G_Poly,           // 1..64 notes
  G_Priority,       // choice: Newest, Oldest, Highest, Lowest, Velocity
  G_SameNote,       // toggle: one voice per key
  G_Swing,          // 0..1 (0.5 = off)
  G_SwingDiv,       // choice
  G_Transpose,      // -48..48 st
  G_DirectVol,      // 0..1 control (4 * v^2: 0.5 = 0 dB)
  G_Bus1Vol,
  G_Bus2Vol,
  G_Bus1Dest,       // choice: Main, Direct
  G_Bus2Dest,
  G_Quality,        // choice: Draft (1x), High (2x), Ultra (4x)
  G_S1Mode,         // toggle: legacy compatibility (warp phase behaviour)
  G_NoSmooth,       // toggle: parameter smoothing off
  G_ConcertA,       // 400..480 Hz
  G_EnvScale,       // 0.1..10 x (all envelope times)
  G_LfoScale,       // 0.1..10 x (all LFO rates)
  G_LfoScaleRate,   // toggle: scale rates instead of times
  G_MpeOn,
  G_MpeBend,        // 1..96 st per-note bend range
  G_MpeYBipolar,
  G_Key,            // choice: Off, C .. B
  G_Scale,          // choice (scales)
  G_RndPan,         // 0..1 per-voice random pan
  G_RndDetune,      // 0..20 cents per-voice random detune
  G_RndCutoff,      // 0..1 per-voice random cutoff (±2 octaves at 1)
  G_RndEnv,         // 0..1 per-voice random envelope time (±50 % at 1)
  G_VcS, G_VcA, G_VcB, G_VcC, G_VcN,  // voice control applies to these oscillators
  G_VcSteps,        // 1..8 voice-control sequence length
  G_ArpOn,
  G_ClipOn,
  G_MidiOut,        // choice: Off, Clip player, Synth (clip + arp)
  G_NoteLatch,
  G_ArpRate, G_ArpShift, G_ArpRange, G_ArpOffset, G_ArpRepeats, G_ArpGate, G_ArpChance, G_ArpRetrigRate,
  G_ArpVeloDecay, G_ArpVeloTarget, G_ArpTranspose, G_ArpWrapTranspose, G_ArpWrapRange, G_ArpWrapPhantom,  // -1..1 offsets
  G_ClipTranspose, G_ClipRate, G_ClipOffset,  // -1..1 offsets (clip player)
  G_TrigOctave,     // choice: Off, C-2 .. C8 (clip / arp slot trigger octave)
  G_Count
};

// ------------------------------------------------------------------ oscillators
enum OscType : int { OscWavetable = 0, OscMultisample, OscSample, OscGranular, OscSpectral, OscTypeCount };

enum Osc : int {
  O_Enable = 0,
  O_Type,
  O_Table,       // choice: factory table (0 = custom / file in data)
  O_Level,       // 0..1 control (v^2)
  O_Pan,         // -1..1
  O_Octave,      // -4..4
  O_Semi,        // -12..12
  O_Fine,        // -100..100 ct
  O_Coarse,      // -64..64 st (continuous)
  O_Ratio,       // 0.25..64 (pitch mode Ratio)
  O_HzOffset,    // -1000..1000 Hz
  O_PitchTrack,
  O_PitchMode,   // choice: Semitones, Steps, Harmonics, Ratio
  O_WtPos,       // 0..1
  O_Phase,       // 0..1 (0..360°)
  O_RandPhase,   // 0..1
  O_PhaseMemory, // choice: Per voice, Contiguous, All (free-running)
  O_Smooth,      // toggle: frame interpolation
  O_Unison,      // 1..16
  O_UniStack,    // choice
  O_UniDetune,   // 0..1 control (v^2)
  O_UniBlend,    // 0..1
  O_UniWidth,    // -1..1
  O_UniSpan,     // -1..1 (tilts the detune spread)
  O_UniRange,    // 0..48 st (full detune range)
  O_UniRotate,   // 0..1 (rotates per-note voice order)
  O_UniRandStart,// 0..1
  O_UniWtPos,    // -1..1 (table position spread)
  O_UniWarp1,    // -1..1
  O_UniWarp2,    // -1..1
  O_DetuneMode,  // choice: Linear, Super, Exp, Inverse, Random
  O_Warp1Mode, O_Warp1, O_Warp1Var,
  O_Warp2Mode, O_Warp2, O_Warp2Var,
  // sample / granular / spectral / multisample source
  O_Start, O_End,       // 0..1
  O_Reverse,
  O_ScanRate,           // -2..2 (playback rate, 1 = original)
  O_ScanSync,           // toggle: scan follows tempo divisions
  O_ScanBpm,            // choice: divisions
  O_ScanKey,            // toggle: scan rate follows the key
  O_Position,           // 0..1 (granular / spectral scan position)
  O_LoopStart, O_LoopEnd, O_LoopXfade,  // 0..1
  O_LoopMode,           // choice: One-shot, Forward, Reverse, Ping-pong, Tailed
  O_RelLoop,            // toggle: loop points relative to start
  O_Slice,              // 1..100
  O_SliceMode,          // choice: To next marker, To end
  O_GDensity,           // 0..1 control (grains per second, 1..200 log)
  O_GLength,            // 0..1 control (grain length 2..1000 ms log)
  O_GReverse,           // 0..1 probability
  O_GWinShape,          // choice (10)
  O_GWinSkew,           // -1..1
  O_GWinParam,          // 0..1
  O_GRndOffset, O_GRndDir, O_GRndPitch, O_GRndLength, O_GRndPan, O_GRndGain, O_GRndWin, O_GRndSkew, O_GRndWarp, O_GRndWarp2,
  O_GDensMode,          // choice: Free, Tempo, Grains
  O_GLenMode,           // choice: Free, Tempo, Percent
  O_GMaxGrains,         // 1..64
  O_GTrig,              // choice: Together, Even, Exponential, Random (unison grain start)
  O_SCut,               // -1..1 spectral filter shift
  O_SMix,               // 0..1 spectral filter mix
  O_SLo, O_SHi,         // 0..1 spectral band edges
  O_SPhaseLock,
  O_STransients,
  O_MTimbre,            // -1..1 multisample timbre shift
  O_MVelTrack,          // 0..1
  O_MEnvOn,             // toggle: per-sample envelope override
  O_MDelay, O_MAttack, O_MHold, O_MDecay, O_MSustain, O_MRelease,  // 0..1 controls
  O_KeyMin, O_KeyMax, O_VelMin, O_VelMax,  // 0..127 zone
  O_MapFold,            // toggle: keys outside the zone fold into it by octaves
  O_MapWarp,            // toggle: outside the zone the oscillator still runs as a warp source (silent)
  O_Count
};

enum Sub : int { S_Enable = 0, S_Level, S_Pan, S_Octave, S_Coarse, S_PitchTrack, S_Shape, S_Phase, S_ContPhase, S_Count };
enum SubShape : int { SubSine = 0, SubRoundRect, SubTriangle, SubSaw, SubSquare, SubPulse, SubShapeCount };

enum Noise : int { N_Enable = 0, N_Level, N_Pan, N_PitchTrack, N_Pitch, N_Fine, N_Phase, N_RandPhase, N_OneShot, N_Type, N_Color, N_Count };

enum Filt : int { F_Enable = 0, F_Type, F_Freq, F_Res, F_Drive, F_Var, F_Stereo, F_Wet, F_Level, F_KeyTrack, F_X, F_Y, F_Count };

enum RouteDest : int { RouteFilter = 0, RouteMain, RouteDirect, RouteNone, RouteDestCount };
enum Route : int { R_Dest = 0, R_Balance, R_Bus1, R_Bus2, R_Count };
enum RouteSrc : int { RtA = 0, RtB, RtC, RtNoise, RtSub, RtF1, RtF2 };

enum Env : int {
  E_Attack = 0, E_Hold, E_Decay, E_Sustain, E_Release,  // times 0..1 control (32 s * v^5); sustain 0..1 (v^2 level)
  E_CurveA, E_CurveD, E_CurveR,                          // 0..100 (50 = linear)
  E_Start, E_End,                                        // 0..1 start / end level
  E_Bpm,                                                 // toggle: times in beats
  E_LegatoInv,                                           // toggle: retrigger even in legato
  E_Steal,                                               // choice: From stolen level, From zero
  E_Count
};

enum LfoMode : int { LfoFree = 0, LfoRetrig, LfoEnvelope };
enum LfoType : int { LfoNormal = 0, LfoPath, LfoLorenz, LfoRossler, LfoSH, LfoTypeCount };
enum Lfo : int {
  L_Rate = 0,   // 0..1 control (beats table or 0..100 Hz curve)
  L_Smooth,     // 0..1
  L_Rise,       // 0..1 control
  L_Delay,      // 0..1 control
  L_Phase,      // 0..1
  L_Mode,       // choice
  L_Type,       // choice
  L_Bpm,        // toggle
  L_Anchor,     // toggle: phase from the song position
  L_Dot, L_Trip,
  L_GridX, L_GridY,  // 1..32
  L_Mono,
  L_Dir,        // choice: Forward, Reverse, Ping-pong
  L_Rate10x,
  L_Swing,
  L_PhaseSnap,
  L_Shape,      // choice: factory shape (0 = custom in data)
  L_Count
};

enum ModP : int {
  M_Source = 0,  // 0..kSourceCount-1 (integer)
  M_Amount,      // -1..1
  M_Bipolar,
  M_Dest,        // 0 = none, else parameter index + 1 (integer)
  M_Aux,         // source id (integer)
  M_AuxInv,
  M_Out,         // 0..1
  M_Bypass,
  M_Curve,       // -1..1 bend of the source curve
  M_AuxCurve,    // -1..1
  M_Rise, M_Fall,  // 0..1 slew (0 = off)
  M_Count
};

// Modulation sources (stored ids).
enum Source : int {
  SrcNone = 0,
  SrcEnv1, SrcEnv2, SrcEnv3, SrcEnv4,
  SrcLfo1, SrcLfo10 = SrcLfo1 + 9,
  SrcLfo1Y, SrcLfo10Y = SrcLfo1Y + 9,
  SrcVelo, SrcNote, SrcActiveVoices, SrcAlt1, SrcAlt2, SrcRandDiscrete, SrcRand1, SrcRand2, SrcReleaseVelo,
  SrcVoiceIndex, SrcVoiceMod1, SrcVoiceMod2,
  SrcNoise, SrcOscA, SrcOscB, SrcOscC, SrcSub,
  SrcMacro1, SrcMacro8 = SrcMacro1 + 7,
  SrcExprX, SrcExprY, SrcExprZ,
  SrcFilter1, SrcFilter2,
  SrcModWheel, SrcAftertouch, SrcPolyAT, SrcPitchBend, SrcFixed,
  SourceCount
};

enum FxType : int {
  FxNone = 0, FxDistortion, FxFlanger, FxPhaser, FxChorus, FxDelay, FxComp, FxReverb, FxEq, FxFilter, FxHyper, FxBode,
  FxConvolve, FxUtility, FxSplitLH, FxSplitLMH, FxSplitMS, FxTypeCount
};
enum FxSlotP : int { X_Type = 0, X_On, X_P0, X_Count = X_P0 + kFxParams };

// ------------------------------------------------------------------ layout
inline constexpr int kOscBase = G_Count;
inline constexpr int kSubBase = kOscBase + kOscs * O_Count;
inline constexpr int kNoiseBase = kSubBase + S_Count;
inline constexpr int kFiltBase = kNoiseBase + N_Count;
inline constexpr int kRouteBase = kFiltBase + kFilters * F_Count;
inline constexpr int kEnvBase = kRouteBase + kRoutes * R_Count;
inline constexpr int kLfoBase = kEnvBase + kEnvs * E_Count;
inline constexpr int kMacroBase = kLfoBase + kLfos * L_Count;
inline constexpr int kBusBase = kMacroBase + kMacros;
inline constexpr int kRetrigBase = kBusBase + kLfoBusses;
inline constexpr int kModBase = kRetrigBase + kRetrigs;
inline constexpr int kFxBase = kModBase + kModSlots * M_Count;
// Parameters added after the first release go here, at the very end, so older states pad them
// with their defaults (J-201: noise tone, unison mix, filter drive mode).
// J-203a: filter key-track centre, multisample level law. J-203g: wavetable / warp law.
enum TailP : int { T_NoiseTone = 0, T_UniMix, T_DriveMode, T_KeyCentre, T_MsLaw, T_EnvLaw, T_OscLaw, T_ModLaw, T_Pad1, T_Pad2, T_Count };
inline constexpr int kTailBase = kFxBase + kFxSlots * X_Count;
inline constexpr int kParamCount = kTailBase + T_Count;

constexpr int oscP(int osc, int p) { return kOscBase + osc * O_Count + p; }
constexpr int subP(int p) { return kSubBase + p; }
constexpr int noiseP(int p) { return kNoiseBase + p; }
constexpr int filtP(int f, int p) { return kFiltBase + f * F_Count + p; }
constexpr int routeP(int r, int p) { return kRouteBase + r * R_Count + p; }
constexpr int envP(int e, int p) { return kEnvBase + e * E_Count + p; }
constexpr int lfoP(int l, int p) { return kLfoBase + l * L_Count + p; }
constexpr int macroP(int m) { return kMacroBase + m; }
constexpr int busP(int b) { return kBusBase + b; }
constexpr int retrigP(int r) { return kRetrigBase + r; }
constexpr int modP(int s, int p) { return kModBase + s * M_Count + p; }
constexpr int fxP(int s, int p) { return kFxBase + s * X_Count + p; }
constexpr int tailP(int p) { return kTailBase + p; }

// ------------------------------------------------------------------ names (plasma_info.cpp)
const std::vector<std::string>& warpNames();       // every warp mode (menu labels)
int warpCount();
// Warp groups: which modes an oscillator type offers (menu filtering, import).
bool warpAllowed(int oscType, int mode);
const std::vector<std::string>& filterNames();     // filter types (menu labels)
const std::vector<std::string>& filterGroups();    // filter menu group of each type ("Low", "Comb"...)
int filterVarKind(int type);                       // label id for the VAR knob (see filterVarLabel)
const char* filterVarLabel(int type);              // "FAT", "SPREAD", ...
const std::vector<std::string>& sourceNames();     // matrix source labels (id order)
const char* sourceGroup(int src);                  // "Envelopes", "LFOs", ...
const std::vector<std::string>& fxTypeNames();
const std::vector<std::string>& lfoShapeNames();   // factory LFO shapes (0 = custom)
const std::vector<std::string>& noiseNames();      // noise oscillator sources
const std::vector<std::string>& scaleNames();
const std::vector<int>& scaleSteps(int scale);     // semitone offsets in the octave
const std::vector<std::string>& divisionNames();   // tempo divisions (LFO / swing / scan)
double divisionBeats(int div);                     // length in beats
int lfoRateDivisionCount();
const char* lfoRateDivisionName(int i);
double lfoRateDivisionBeats(int i);

// Parameter display (the editor): text for value v of parameter k, using the
// real curves (dB, ms, Hz, beats) instead of the stored control value.
std::string paramText(int k, double v, const std::vector<float>* params = nullptr);
// Short label ("WT POS") and full name ("A WT Pos") for menus / matrix rows.
std::string paramShortName(int k);
std::string paramLongName(int k);
// Parameters a matrix slot may target (continuous, not the matrix itself).
bool modulatable(int k);
// Whole-number parameters (octave, semitones, unison, steps...): the editor snaps them.
bool integerParam(int k);

// ------------------------------------------------------------------ curves
inline double levelGain(double v) { return v * v; }                      // osc / sub / noise level
inline double masterGain(double v) { return 1.41253754 * v * v; }        // +3 dB at the top
inline double busGain(double v) { return 4.0 * v * v; }                  // +12 dB at the top, 0 dB at 0.5
inline double envTime(double v) { return 32.0 * std::pow(v < 0 ? 0 : v, 5.0); }   // seconds
inline double envTimeInv(double s) { return s <= 0 ? 0 : std::pow(s / 32.0, 0.2); }
inline double portaTime(double v) { return 8.0 * std::pow(v < 0 ? 0 : v, 5.0); }
inline double portaTimeInv(double s) { return s <= 0 ? 0 : std::pow(s / 8.0, 0.2); }
inline double cutoffHz(double v) { return 440.0 * std::pow(2.0, (v * 136.9282 - 69.0) / 12.0); }
inline double cutoffNorm(double hz) { return (12.0 * std::log2(hz / 440.0) + 69.0) / 136.9282; }
inline double lfoHz(double v, bool x10) { return (x10 ? 1000.0 : 100.0) * std::pow(v < 0 ? 0 : v, 4.0); }
inline double lfoHzInv(double hz, bool x10) { return hz <= 0 ? 0 : std::pow(hz / (x10 ? 1000.0 : 100.0), 0.25); }
inline double grainDensity(double v) { return std::exp(std::log(1.0) + v * (std::log(200.0) - std::log(1.0))); }  // grains / s
inline double grainLengthMs(double v) { return std::exp(std::log(2.0) + v * (std::log(1000.0) - std::log(2.0))); }

// ------------------------------------------------------------------ data model (PluginState::data, JSON)
struct CurvePt {
  float x = 0, y = 0;   // 0..1
  float c = 0;          // -1..1 bend of the segment ending at this point
};
using Curve = std::vector<CurvePt>;
// Value of a point curve at x (segments bent by each end point's c).
double curveAt(const Curve& c, double x);
Curve linearCurve();                  // (0,0) .. (1,1)
Curve lfoFactoryShape(int shape);     // shape index from lfoShapeNames() (>= 1)
double bendCurve(double t, double c); // 0..1 -> 0..1 bent by c (-1..1)

struct LfoData {
  Curve pts;           // empty = factory shape from the L_Shape parameter
  int loop = -1;       // envelope-mode loopback point (-1 = none)
  Curve path;          // path mode: x/y points (x = time-ordered position, y)
  std::vector<float> pathX;  // path mode: X output per path point (y in pts is the Y output)
  std::string name;    // shape name shown in the menu
};
struct BusLink {       // LFO point modulation: a bus moves a point of an LFO
  int lfo = 0, point = 0;
  int axis = 1;        // 0 = X (time), 1 = Y (value)
  int bus = 0;         // 0..15
  float depth = 1;     // -1..1
};
struct OscData {
  std::string table;   // "" = factory (O_Table), "file:<path>", "custom"
  std::string tableName;
  std::vector<std::vector<float>> frames;  // custom table frames (2048 samples each)
  std::string sample;  // sample / granular / spectral source file
  std::string sfz;     // multisample (.sfz) file
  std::vector<float> slices;  // slice markers 0..1
  std::array<Curve, 4> remap;  // remap warp curves 1-4
};
struct FxNode {        // one effect slot placed in a rack
  int slot = 0;        // 0..kFxSlots-1
  int rack = 0;        // 0 = Main, 1 = Bus 1, 2 = Bus 2
  int parent = -1;     // splitter slot that hosts it (-1 = rack level)
  int band = 0;        // splitter band (0..2)
};
struct FxExtra {       // per-slot data that is not a parameter
  std::string ir;      // convolve: impulse response file
  Curve shapeA, shapeB;  // distortion X-Shaper curves
};
struct ArpSlot {
  int shape = 1;                 // ArpShape
  int rate = 7;                  // division index (1/16; Hz from `hz` when !bpm)
  float hz = 4;
  bool bpm = true, trip = false, dot = false;
  int shift = 0, range = 0, rangeShape = 0;   // transpose: semitones per step, steps, shape
  bool latch = false, thru = false;
  int offset = 0, repeats = 0;   // repeats 0 = inf
  float gate = 1.0f, chance = 1.0f;
  bool chancePre = false;
  bool retrigLaunch = true, retrigRate = false, retrigNote = false, retrigFirst = false;
  int retrigDiv = 6;
  bool velo = false, veloRetrig = false;
  float veloDecay = 0.5f, veloTarget = 0.5f;
  // custom pattern (shape == Pattern)
  struct Step { float time = 0, len = 0.25f; int row = 0; float vel = 1; bool accent = false; float strum = 0; };
  std::vector<Step> steps;
  float patternBeats = 4;
  int patMode = 0, stepMode = 0, wrap = 0, wrapPitch = 12, rangeWrap = 0;
  float patTime = 1;
  std::string name;
};
struct ClipNote { float start = 0, len = 0.25f; int key = 60; float vel = 0.8f; bool mute = false; };
struct ClipLane { int target = -1; std::vector<std::pair<float, float>> pts; };  // automation (parameter index; beat, value)
struct ClipSlot {
  std::vector<ClipNote> notes;
  std::vector<ClipLane> lanes;
  float lengthBeats = 4;
  int span = 0;          // KB span: Off, Mono, Poly, Offset
  int transpose = 0;
  int mode = 0;          // Normal, Reverse, Pendulum, Random, RandomNoDup, RandomStart, RandomEnd, OneShot, Static
  float modeTime = 1;    // beats per random slice
  float rate = 1;        // speed factor
  bool bpm = true, trip = false, dot = false;
  int launchQuant = 6;   // division index
  bool retrig = true, veloTrig = false, noteGate = false;
  float startOffset = 0; // beats
  std::string name;
};
struct VoiceStep { float pan = 0, detune = 0, cutoff = 0, env = 0, mod1 = 0, mod2 = 0; };

// Audio a sound carries inside its own data (the wavetables, samples, noises,
// impulses and multisample maps a Serum preset embeds), so a project keeps
// working on a computer without the preset. Referenced as "media:<id>" from
// OscData::sample / sfz, Model::noiseFile and FxExtra::ir; extracted on demand
// into Gridline's cache folder (mediaFile) and read by the usual file loaders.
struct Media {
  std::string id;          // content hash (16 hex digits)
  std::string name;        // file name it had in the preset
  int rate = 44100, channels = 1;
  int64_t frames = 0;
  float peak = 1;          // pcm holds sample / peak
  std::string pcm;         // base64: 16-bit little-endian, interleaved
  std::string sfz;         // multisample map: its sample= values are media references
};

struct Model {
  std::array<OscData, kOscs> osc;
  std::string noiseFile;                 // user noise sample (N_Type = file)
  std::array<LfoData, kLfos> lfo;
  Curve velo, note;                      // scalar graphs (empty = linear)
  std::array<Curve, kModSlots> modCurve, auxCurve;  // editable curves (empty = from M_Curve / M_AuxCurve)
  std::vector<BusLink> busLinks;
  std::vector<FxNode> fx;                // rack order (top to bottom)
  std::array<FxExtra, kFxSlots> fxExtra;
  std::array<std::array<int, 16>, 3> fxProxy{};  // host automation slots per rack (-1 = none): slot*64 + param
  std::array<ArpSlot, kArpSlots> arp;
  int arpActive = 0, arpLaunchQuant = 6;
  std::array<ClipSlot, kClipSlots> clip;
  int clipActive = 0;
  bool clipPoly = false;
  std::array<VoiceStep, 8> voiceSeq{};
  std::array<std::string, kMacros> macroName;
  std::string tuning;                    // .tun text (empty = 12-TET)
  std::string tuningName;
  std::string presetName, author, description;
  std::vector<std::string> tags;
  // Report of the Serum preset / state this sound was opened from (shown by
  // the info mark next to the preset name; empty for Plasma's own presets).
  std::string openFormat;
  std::vector<std::string> openNotes, openMissing;
  bool mpeXYZtoMacros = false, mpeYZtoMacros = false, mpeYtoModWheel = false, mpeBendToX = false;
  bool lockFx = false;
  std::vector<Media> media;              // embedded audio (see Media)
};

// Stores audio in m.media (deduplicated by content); returns its "media:<id>" reference.
std::string addMedia(Model& m, const std::string& name, int rate, const std::vector<std::vector<float>>& channels);
// Stores a multisample map whose sample= values are media references.
std::string addMediaMap(Model& m, const std::string& name, const std::string& sfzText);
bool isMediaRef(const std::string& ref);
const Media* findMedia(const Model& m, const std::string& ref);
// Audio of a media entry (channels of frames).
bool mediaAudio(const Media& e, std::vector<std::vector<float>>& channels);
// The file a reference plays from: media entries are written to the cache
// folder once (named by their id) and that path is returned; other references
// are returned as they are; "" when a media entry is missing or cannot be written.
std::string mediaFile(const Model& m, const std::string& ref);
// Folder of the extracted media ($GRIDLINE_CACHE_DIR/plasma-media when set).
std::string mediaCacheDir();

Model defaultModel();
bool modelFromData(const std::string& data, Model& m);   // false: not Plasma data (m = defaults)
std::string modelToData(const Model& m);
// Racks with the effect order the runtime uses: every slot with a type that is
// not listed in m.fx is appended to the Main rack.
std::vector<FxNode> resolvedFx(const Model& m, const std::vector<float>& params);

// ------------------------------------------------------------------ wavetables
inline constexpr int kFrameSize = 2048;
struct Table {
  std::string name;
  int frames = 0;
  std::vector<float> data;  // frames * kFrameSize
  bool morph = false;       // the file asks for frames blended by position (its 'clm ' flags start with 1)
  const float* frame(int i) const { return data.data() + static_cast<size_t>(i) * kFrameSize; }
};
const std::vector<std::string>& factoryTableNames();  // index = O_Table - 1
std::shared_ptr<const Table> factoryTable(int index); // cached
// Loads a single-cycle / wavetable WAV (cycle length from a 'clm ' chunk or
// a .txt sidecar, else 2048); stereo files use the left channel.
std::shared_ptr<const Table> loadTableFile(const std::string& path, std::string* err = nullptr, int channel = 0);
std::shared_ptr<const Table> tableFromFrames(const std::vector<std::vector<float>>& frames, const std::string& name);

// ------------------------------------------------------------------ arp shapes
enum ArpShape : int {
  ArpPattern = 0, ArpUp, ArpDown, ArpUpDown, ArpDownUp, ArpUpAndDown, ArpDownAndUp, ArpThumbUp, ArpThumbUD, ArpPinkyUp,
  ArpPinkyUD, ArpConverge, ArpDiverge, ArpConDiv, ArpPlayed, ArpChord, ArpRand, ArpRandNoDup, ArpRandDrift, ArpRandOnce,
  ArpShapeCount
};
const std::vector<std::string>& arpShapeNames();
// Order of the held notes for one cycle of a shape (indices into `sorted`
// low..high; `played` = indices in played order). rng drives random shapes.
void arpOrder(int shape, int count, const std::vector<int>& played, uint32_t& rng, std::vector<int>& out);

// ------------------------------------------------------------------ effect definitions
struct FxParamDef {
  const char* key;    // stored name (also the Serum key it imports from)
  const char* label;  // knob label
  double min, max, def;
  int kind;           // 0 continuous, 1 toggle, 2 choice
  int curve;          // 0 linear, 1 log (min > 0), 2 dB level (0..1 control, v^2 * 4 = +12 dB), 3 integer
  const char* unit;   // "Hz", "ms", "%", "dB", "st", ""
  std::vector<std::string> choices;
};
const std::vector<FxParamDef>& fxParams(int type);
int fxParamIndex(int type, const char* key);         // -1 when absent
double fxPlain(int type, int p, double norm);         // stored 0..1 -> plain
double fxNorm(int type, int p, double plain);         // plain -> stored 0..1
std::string fxParamText(int type, int p, double norm);
float fxDefaultNorm(int type, int p);
// Serum 2's value of effect parameter p when a preset leaves it unset (plain units); false when
// Plasma's own default stands.
bool fxSerumDefault(int type, int p, double& plain);

// ------------------------------------------------------------------ live view (editor displays)
struct LiveView {
  int voices = 0;                       // sounding notes
  float oscPos[kOscs] = {};             // displayed table / sample position (lead note)
  bool oscOn[kOscs] = {};
  float env[kEnvs] = {};                // envelope levels (lead note)
  int envStage[kEnvs] = {};             // 0 idle, 1 attack, 2 hold, 3 decay, 4 sustain, 5 release
  float envTime[kEnvs] = {};            // seconds in the current stage
  float lfo[kLfos] = {}, lfoPhase[kLfos] = {};
  float macro[kMacros] = {};
  float fxMeter[kFxSlots] = {};
  float outL = 0, outR = 0;             // output peak
  float mod[kModSlots] = {};            // current output of each matrix slot (lead note)
  int arpStep = -1;
  double clipPos = -1;
  float filterCut[kFilters] = {};       // modulated cutoff (Hz, lead note)
  float source[SourceCount] = {};       // each modulation source's raw output (lead note, or global)
};
// How a source meter shows a raw source value: bipolar sources (LFOs, pitch bend,
// audio-rate sources) return -1..1 centred on zero, the rest 0..1 from zero.
float sourceMeterValue(int src, float raw, bool& bipolar);
// Reads the running instance's live data (false when g is not a Plasma instance).
bool liveView(const Generator* g, LiveView& out);
// Audition / preview from the editor (UI thread): queued for the audio thread.
void previewNote(Generator* g, int key, bool on);
// Clip recording: while on, played notes sound directly (not through the clip
// player / arp) and are reported to the editor with their timing.
struct RecordedEvent {
  int key = 60;
  float vel = 0.8f;
  bool on = true;
  double clock = 0;     // free-running beats (always advances)
  double beat = 0;      // host song position in beats
  bool playing = false; // host transport running
};
void setClipRecording(Generator* g, bool on);
// Events since `readIndex` (advanced); returns the count written to `out`.
int recordedEvents(const Generator* g, uint32_t& readIndex, RecordedEvent* out, int max);

// ------------------------------------------------------------------ presets / import
// Factory presets that need data (drawn tables, shapes, sequences): parameter
// overrides on top of the defaults plus the Model data. (Parameter-only presets
// live in PluginInfo::presets.)
struct DataPreset {
  std::string name;
  std::vector<std::pair<int, float>> values;
  std::string data;
};
const std::vector<DataPreset>& factoryDataPresets();

struct ImportReport {
  bool ok = false;
  std::string error;
  std::string format;                     // "Serum 2 preset", "Serum 1 preset (.fxp)"
  std::string presetName, author;
  std::vector<std::string> approximated;  // human-readable notes
  std::vector<std::string> missingFiles;
  std::vector<std::string> unread;        // stored values with no mapping ("Module.kParamKey")
  int mapped = 0, skipped = 0;
};
// Reads a Serum 2 (.SerumPreset) or Serum 1 (.fxp) preset; fills a Plasma state.
// `searchRoots`: folders searched for wavetables / samples / IRs referenced by
// relative path (the user's own Serum content folders). Audio the preset embeds
// is kept in the state itself (Model::media); referenced files that are not
// embedded are also looked for next to the preset (`presetDir` and the folders
// above it, as packs lay them out).
bool importSerumPreset(const std::string& path, PluginState& out, ImportReport& rep,
                       const std::vector<std::string>& searchRoots = {});
bool importSerumBytes(const std::vector<uint8_t>& bytes, PluginState& out, ImportReport& rep,
                      const std::vector<std::string>& searchRoots = {}, const std::string& presetDir = {});
std::vector<std::string> defaultSerumRoots();  // the user's Serum content folders that exist (+ extra folders)
// Serum 1 / Serum 2 preset folders looked for on this system (macOS, Windows,
// Linux Wine prefixes), existing or not.
std::vector<std::string> serumRootCandidates();
// Folders the user added (Plasma browser settings): searched like the defaults.
void setExtraSerumFolders(const std::vector<std::string>& folders);
std::vector<std::string> extraSerumFolders();

// ---- opening presets: one path for every format
// Plasma's own presets (.plasmapreset / Gridline preset JSON), Serum 2
// presets (.SerumPreset) and Serum 1 presets (.fxp), detected by content
// (the extension only breaks ties). `rep.format` names what was read;
// Serum presets also keep their report in the Model (openFormat / openNotes).
bool openPresetBytes(const std::vector<uint8_t>& bytes, const std::string& fileName, PluginState& out, ImportReport& rep,
                     const std::vector<std::string>& searchRoots = {}, const std::string& presetDir = {});
bool openPresetFile(const std::string& path, PluginState& out, ImportReport& rep, const std::vector<std::string>& searchRoots = {});
// True when the file name has an extension the open path reads.
bool isOpenableName(const std::string& fileName);
const std::vector<std::string>& openableExtensions();  // ".plasmapreset", ".serumpreset", ".fxp", ".json"

// ---- Serum stand-ins (core/PluginStandIn.h)
// The state of a hosted Serum / Serum 2 instance (VST3 component state, AU
// class info, VST2 chunk) as a Plasma state.
bool serumStateToPlasma(const PluginState& hosted, PluginState& out, ImportReport& rep);
// Automation link of parameter `index` of the hosted instance (`hosted.type`
// order) to a Plasma parameter: `curve` = Plasma normalized values at
// original values i / (curve.size() - 1). False when Plasma has no equivalent.
bool serumParamLink(const PluginState& hosted, int index, int& plasmaParam, std::vector<float>& curve, std::string& title);
// Parameter count / name of the hosted Serum type (0 / "" when unknown).
int serumParamCount(const std::string& type);
std::string serumParamName(const std::string& type, int index);
// Parameter index of a plugin parameter id (VST3 / AU id; VST2: the index) or -1.
int serumParamIndexOfId(const std::string& type, uint32_t id);

// ---- Serum preset library (preset browser; plasma_library_dsp.cpp)
struct LibraryPreset {
  std::string name, bank;  // bank: folder below the root's preset folder ("" = top)
  std::string root, file;  // content root (defaultSerumRoots) and preset file
  bool serum2 = false;     // .SerumPreset (else Serum 1 .fxp)
};
struct LibrarySnapshot {
  std::vector<std::string> roots;
  std::vector<LibraryPreset> presets;  // by root, bank, name
  bool indexing = false;               // a rescan is running (presets: the previous index)
  uint64_t generation = 0;             // changes with every new index
};
// Starts a background rescan of the library (at most every 15 s unless
// `force`); the first call lists the index kept in `cacheFile` at once.
void libraryRefresh(const std::string& cacheFile, bool force = false);
std::shared_ptr<const LibrarySnapshot> librarySnapshot();
// Keeps the user's added folders (extraSerumFolders) in the cache file.
void librarySaveFolders(const std::string& cacheFile);
// Blocking scan of preset files below the roots (tests, the background index).
std::vector<LibraryPreset> scanLibrary(const std::vector<std::string>& roots, const std::atomic<bool>* cancel = nullptr);
// The library's folders as a tree (pre-order: each folder's sub-folders right
// below it, siblings in natural name order, roots in `snap.roots` order and
// listed even without presets). `key` is the browser scope of the folder:
// "serum:<root>" or "serum:<root>\n<bank path>"; "serum:" is the whole library.
struct LibraryFolder {
  std::string label, key;
  int depth = 0, parent = -1;  // parent: index in the tree (-1 = a root)
  int count = 0;               // presets in the folder and its sub-folders
  bool hasChildren = false;
};
std::vector<LibraryFolder> libraryFolderTree(const LibrarySnapshot& snap);
// Indices (into snap.presets) of the presets of a scope: the folder's own and
// its sub-folders' presets.
std::vector<int> libraryPresetsIn(const LibrarySnapshot& snap, const std::string& key);
// The tree rows shown when the folders with open[i] != 0 are expanded.
std::vector<int> libraryVisibleFolders(const std::vector<LibraryFolder>& tree, const std::vector<char>& open);
// Preset browser rows: indices of the rows whose name or folder contains every
// word of `query` (any case), sorted by column 0 name / 1 folder / 2 source
// (natural order, any case; ties keep the list order) or in list order (-1).
struct BrowserRow {
  std::string name, folder, source;
};
std::vector<int> browserFilter(const std::vector<BrowserRow>& rows, const std::string& query, int sortColumn, bool descending);
// Natural, case-insensitive order ("Lead 2" < "lead 10"): <0, 0, >0.
int naturalCompare(const std::string& a, const std::string& b);
// Decoders used by the import (exposed for tests).
bool zstdDecompress(const uint8_t* src, size_t n, std::vector<uint8_t>& out, std::string* err = nullptr);
bool cborDecode(const uint8_t* src, size_t n, Json& out, std::string* err = nullptr);

}  // namespace gl::packs::plasma
