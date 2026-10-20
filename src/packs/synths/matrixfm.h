// Gridline — "synths" pack: "Matrix FM" (gl.matrixfm).
//
// Six operators and three filters ("units") wired by a 9 × 9 matrix: each
// row is a unit (operators 1..6, filters 1..3), columns 1..6 are the six
// operators as sources (frequency modulation into an operator row, audio
// input into a filter row), column 7 is the unit's pan, 8 its send to the
// effects section and 9 its level to the output. A second 9 × 9 layer holds
// ring-modulation amounts (operator rows only).
//
// Every operator and filter has articulators — one per modulation target —
// each with a multipoint envelope, a drawn LFO, scale controls and six
// mapping curves (key, velocity, mod X, mod Y, random, unison). The main
// unit has two (pan, volume). Scalar controls are parameters; drawn data
// (harmonics, envelope / LFO / mapping points, waveshaper curves) lives in
// PluginState::data (text, see matrixfm_data.h) and is prepared on the UI
// thread.
#pragma once
#include <cstdint>

namespace gl::packs::synths::mfm {

inline constexpr int kOps = 6, kFilters = 3, kUnits = 10;  // unit 9 = main
inline constexpr int kRows = 9, kCols = 9;                  // matrix
inline constexpr int kColPan = 6, kColFx = 7, kColOut = 8;
inline constexpr int kHarmonics = 256;
inline constexpr int kMaps = 6;  // key, velocity, mod X, mod Y, random, unison

// Articulator targets.
enum OpArt { OaPan, OaVol, OaMod, OaPitch, OaPhase, OaDamp, kOpArts };
enum FltArt { FaPan, FaVol, FaCut, FaRes, FaLow, FaBand, FaHigh, FaWsMix, kFltArts };
enum MainArt { MaPan, MaVol, kMainArts };
inline constexpr int unitArts(int unit) { return unit < kOps ? kOpArts : unit < kOps + kFilters ? kFltArts : kMainArts; }
enum MapKind { MapKey, MapVel, MapX, MapY, MapRand, MapUni };

// Articulator scalar fields.
enum ArtField {
  AfEnvOn,       // envelope active
  AfLfoOn,       // LFO active
  AfAmount,      // envelope + LFO depth (-1..1)
  AfLfoAmt,      // LFO share of the depth (0..1)
  AfAtt, AfDec, AfSus, AfRel,  // envelope time scales 0..2 (factor 2^((v-1)*8), 1 = as drawn); sustain offset -1..1
  AfLfoSpeed,    // Hz
  AfLfoSkew,     // LFO phase skew -1..1
  AfTempo,       // LFO speed follows the tempo (speed = cycles per beat)
  AfLfoGlobal,   // one LFO for all voices (no retrigger)
  kArtFields
};

// Operator fields.
enum OpField {
  OpPhase, OpVol, OpPitchRange, OpRatio, OpFreq, OpNoise,
  OpShape, OpTension, OpSkew, OpSine, OpFilter,
  OpHalf, OpEven, OpAbs, OpGlobal, OpCenter, OpDeclick, OpPluck, OpBandLimit,  // toggles
  OpArts,  // kOpArts × kArtFields
  kOpFields = OpArts + kOpArts * kArtFields
};
// Filter fields.
enum FltField {
  FlMode, FlType, FlEnv, FlCut, FlRes, FlLow, FlBand, FlHigh, FlWsAmp, FlWsMix, FlNext,
  FlHq, FlFlat, FlWsOn, FlWsBipolar,
  FlArts,
  kFltFields = FlArts + kFltArts * kArtFields
};
// Filter types (FlType).
enum FltType { FtSvf, FtLp, FtHp, FtLp2, FtBp, FtBs, FtHp2, FtAp, FtLs, FtPk, FtHs, FtLp3, FtPhaser, kFltTypes };
// Filter modes (FlMode): off, 1 / 2 / 3 stages, alternate 2 / 3.
enum FltMode { FmOff, FmX1, FmX2, FmX3, FmAlt2, FmAlt3, kFltModes };

enum MainParam {
  MVolume, MLfoLevel, MPitch,
  MVolA, MVolD, MVolS, MVolR, MFltA, MFltD, MFltS, MFltR,
  MUniOrder, MUniPan, MUniVol, MUniPitch, MUniSub, MUniPhase, MUniEnv,
  MModX, MModY,
  MPorta, MPortaTime, MVoices, MMonoKey, MGlobalPitch, MSoften, MCenter, MGibbs, MRandom, MSmooth,
  MEqTarget, MEq1Type, MEq1Freq, MEq1Bw, MEq1Gain, MEq2Type, MEq2Freq, MEq2Bw, MEq2Gain, MEq3Type, MEq3Freq, MEq3Bw, MEq3Gain,
  // effects
  FxPan, ChOrder, ChDepth, ChSpeed, ChDelay, ChSpread, ChCross, ChVol,
  DlSerial,
  Dl1On, Dl1Mode, Dl1Fb, Dl1Time, Dl1Ofs, Dl1Vol, Dl1Tempo,
  Dl2On, Dl2Mode, Dl2Fb, Dl2Time, Dl2Ofs, Dl2Vol, Dl2Tempo,
  Dl3On, Dl3Mode, Dl3Fb, Dl3Time, Dl3Ofs, Dl3Vol, Dl3Tempo,
  RvOn, RvVol, RvHighDamp, RvDecay, RvDiff, RvRoom, RvPreDelay, RvHighCut, RvLowCut, RvColor, RvTempo,
  MainArts,                                     // kMainArts × kArtFields
  MatFm = MainArts + kMainArts * kArtFields,    // kRows × kCols, -1..1
  MatRm = MatFm + kRows * kCols,                // kRows × kCols, -1..1
  OpBase = MatRm + kRows * kCols,               // kOps × kOpFields
  FltBase = OpBase + kOps * kOpFields,          // kFilters × kFltFields
  kParamCount = FltBase + kFilters * kFltFields
};
// Engine model (J-50, appended): 0 = Gridline's own DSP (legacy states),
// 1 = the reference model (mfmref.h, sample-exact imported patches).
inline constexpr int MModel = kParamCount;
inline constexpr int kParamCountAll = kParamCount + 1;
inline constexpr int kDlStride = Dl2On - Dl1On;
inline constexpr int kEqStride = MEq2Type - MEq1Type;

inline constexpr int fm(int row, int col) { return MatFm + row * kCols + col; }
inline constexpr int rm(int row, int col) { return MatRm + row * kCols + col; }
inline constexpr int op(int k, int field) { return OpBase + k * kOpFields + field; }
inline constexpr int flt(int j, int field) { return FltBase + j * kFltFields + field; }
inline constexpr int dly(int d, int field) { return Dl1On + d * kDlStride + field; }  // field relative to Dl1On
inline constexpr int eq(int b, int field) { return MEq1Type + b * kEqStride + field; }  // 0 type 1 freq 2 bw 3 gain
// Articulator field of unit u (0..5 operators, 6..8 filters, 9 main), target a.
inline constexpr int art(int unit, int a, int field) {
  return unit < kOps ? op(unit, OpArts + a * kArtFields + field)
                     : unit < kOps + kFilters ? flt(unit - kOps, FlArts + a * kArtFields + field)
                                              : MainArts + a * kArtFields + field;
}

// Articulator target names (editor tabs).
inline const char* const kOpArtNames[kOpArts] = {"PAN", "VOL", "MOD", "PITCH", "PHASE", "DAMP"};
inline const char* const kFltArtNames[kFltArts] = {"PAN", "VOL", "CUT", "RES", "LOW", "BAND", "HIGH", "WS MIX"};
inline const char* const kMainArtNames[kMainArts] = {"PAN", "VOL"};
inline const char* const kMapNames[kMaps] = {"KEY", "VEL", "MOD X", "MOD Y", "RAND", "UNISON"};
inline const char* const kFltTypeNames[kFltTypes] = {"SVF", "LP", "HP", "LP2", "BP", "BS", "HP2", "AP", "LS", "PK", "HS", "LP3", "PHS"};
inline const char* const kFltModeNames[kFltModes] = {"OFF", "x1", "x2", "x3", "ALT x2", "ALT x3"};
// Bipolar targets centre at 0.5 (pan; operator pitch / phase; filter cutoff /
// resonance); the others scale 0..1 (neutral 1).
inline constexpr bool artBipolar(int unit, int a) {
  return a == 0 || (unit < kOps && (a == OaPitch || a == OaPhase)) || (unit >= kOps && unit < kOps + kFilters && (a == FaCut || a == FaRes));
}

}  // namespace gl::packs::synths::mfm
