// Gridline — "synths" pack: "Groove Synth" (gl.gms).
//
// A three-oscillator subtractive synth: band-limited wavetable oscillators
// (built-in single cycles or a single-cycle file per oscillator), oscillator 2
// modulating oscillator 1 (hard sync / phase FM / AM), noise, unison up to 16,
// a 4-pole ladder filter (low / band / high), an exponential amp envelope, two
// attack-decay modulation envelopes and two LFOs with routable destinations,
// velocity routing, a fixed chain of ten effect slots (each with its own
// tempo LFO on its X control) and a 5-band master EQ.
//
// Wave files live in PluginState::data:  "gms 1\nw <osc> <path>\n…"
#pragma once
#include <string>

namespace gl::packs::synths::gms {

inline constexpr int kOscs = 3;
inline constexpr int kFxSlots = 10;
inline constexpr int kEqBands = 5;

// Built-in single-cycle waves; WvFile plays the oscillator's file from the data.
enum Wave { WvSine, WvTriangle, WvSaw, WvSquare, WvPulse25, WvPulse10, WvSoftSaw, WvHollow, WvOrgan, WvBell, WvSubSaw, WvFormant, WvSpike, WvFile, WvCount };
enum FltType { GfLowPass, GfBandPass, GfHighPass };
enum ModKind { GkSync, GkFm, GkAm };
enum EgDest { EdCutoff, EdMod, EdPitch, EdLfoDepth };
enum LfoDest { LdCutoff, LdMod, LdPitch, LdVolume };
enum LfoShape { LsSine, LsTriangle, LsSaw, LsSquare };
enum LfoSync { LyFree, LyBeats, LyBeats4 };
inline constexpr int kDivs = 9;
inline constexpr double kDivBeatsG[kDivs] = {0.125, 0.25, 0.375, 0.5, 0.75, 1, 2, 4, 8};
enum FxKind { FxDistort, FxCrush, FxFlange, FxPhase, FxLowPass, FxHighPass, FxDelay, FxReverb, FxPanner, FxGate };
// Velocity destinations.
enum VelDest {
  VdNone, VdCutoff, VdReso, VdMod, VdEg1A, VdEg1D, VdEg1Amt, VdEg2A, VdEg2D, VdEg2Amt, VdLfo1Rate, VdLfo1Amt, VdLfo2Rate, VdLfo2Amt,
  VdAmpA, VdAmpD, VdCount
};

enum EgField { EgAmt, EgAtt, EgDec, EgDest, EgStride };
enum LfoField { LfAmt, LfShape, LfRate, LfSync, LfDiv, LfRetrig, LfDest, LfStride };
enum OscField { OsWave, OsSemi, OsCents, OsStride };
enum FxField { FfOn, FfX, FfY, FfDiv, FfShape, FfX4, FfLfo, FfStride };

enum Param {
  GVolume, GPan, GPitch, GOctave, GLevel, GVelVol, GBend,
  GAmpA, GAmpD, GAmpS, GAmpR,
  GSlide, GSlideTime, GMono,
  GCutoff, GReso, GKey, GFltType,
  GEg1,                                   // 2 × EgStride
  GLfo1 = GEg1 + 2 * EgStride,            // 2 × LfStride
  GUniVoices = GLfo1 + 2 * LfStride, GUniDetune, GUniStereo, GUniPhase, GUniRetrig,
  GNoise, GOsc2Mix, GOsc3Mix, GModAmt, GModKind, GOsc2Phase, GOsc2Retrig, GOsc2Invert,
  GOsc1,                                  // kOscs × OsStride
  GVelAmt = GOsc1 + kOscs * OsStride, GVelDest,
  GEqOn, GEq1,                            // kEqBands gains
  GFx1 = GEq1 + kEqBands,                 // kFxSlots × FfStride
  GCount = GFx1 + kFxSlots * FfStride
};
inline constexpr int eg(int k, int f) { return GEg1 + k * EgStride + f; }
inline constexpr int lfo(int k, int f) { return GLfo1 + k * LfStride + f; }
inline constexpr int osc(int k, int f) { return GOsc1 + k * OsStride + f; }
inline constexpr int fx(int k, int f) { return GFx1 + k * FfStride + f; }

inline const char* const kFxNames[kFxSlots] = {"DIST", "CRUSH", "FLANGE", "PHASE", "LOW PASS", "HIGH PASS", "DELAY", "REVERB", "PAN", "GATE"};
inline const char* const kFxX[kFxSlots] = {"Drive", "Crush", "Time", "Depth", "Freq", "Freq", "Feedback", "Decay", "Width", "Release"};
inline const char* const kFxY[kFxSlots] = {"Tone", "Tone", "Fdbk", "Fdbk", "Q", "Q", "Tone", "Damp", "Spread", "Depth"};
inline constexpr double kEqHz[kEqBands] = {80, 220, 1200, 6000, 12000};

// Wave-file data.
struct WaveFiles {
  std::string path[kOscs];
};
WaveFiles parseWaveFiles(const std::string& data);
std::string serializeWaveFiles(const WaveFiles& w);

}  // namespace gl::packs::synths::gms
