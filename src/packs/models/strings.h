// Gridline — "models" pack, J-22 group "strings": parameter layouts shared by
// the catalogue, runtimes, editors and the FL import.
//   gl.twinstring  "Twin String"  two damped waveguide strings excited by a
//                                 filtered noise / impulse burst, comb filter,
//                                 8-band resonator body, free envelope + LFO,
//                                 8-slot modulation matrix, delay / chorus /
//                                 reverb. Parameter index = the reference
//                                 synth's control tag (identity automation).
//   gl.plucker     "Plucker"      a lean plucked-string voice (decay, colour,
//                                 normalized decay, gate, widen); index = tag.
//   gl.stepbass    "Step Bass"    a transistor-style bass line voice with a
//                                 32-step sequencer per program (128 programs
//                                 of patches + sequences kept in the state).
#pragma once

namespace gl::packs::models::strs {

// ------------------------------------------------------------ Twin String
inline constexpr int kTsRes = 8;    // resonator bands
inline constexpr int kTsMods = 8;   // modulation slots
enum TsParam {
  TsLevel = 0, TsTune, TsAmpA, TsAmpD, TsAmpS, TsAmpR,
  TsS1Decay = 6, TsS1Inv, TsS1DampOfs, TsS1Damp, TsS1Sharp,
  TsS2Decay = 11, TsS2Inv, TsS2DampOfs, TsS2Damp, TsS2Sharp,
  TsSaturation = 16, TsMix, TsStereo,
  TsCombRatio = 19, TsCombFb, TsCombFbInv, TsCombLevel, TsCombLevelInv,
  TsLoCut = 24, TsLoRes,
  TsHiAtk = 26, TsHiDec, TsHiSus, TsHiCut, TsHiRes,
  TsNoiseLevel = 31, TsNoiseRate, TsDrive,
  TsEgA = 34, TsEgD, TsEgS, TsEgR, TsLfoRate,
  TsResFreq1 = 39,                  // 8 × frequency
  TsResFb1 = TsResFreq1 + kTsRes,   // 8 × feedback
  TsResOn1 = TsResFb1 + kTsRes,     // 8 × on
  TsDlyOn = 63, TsDlyTime, TsDlyFb, TsDlySpeed, TsDlyDepth, TsDlyBlur, TsDlyWet,
  TsChoOn = 70, TsChoRate, TsChoDepth, TsChoWet,
  TsRevOn = 74, TsRevMix, TsRevDecay, TsRevColor,
  TsSpare = 78,                     // unused slot of the reference layout (kept for the identity map)
  TsMod1 = 79,                      // 8 × (source, amount, negative, destination)
  TsS2Ratio = TsMod1 + 4 * kTsMods, TsDlySync, TsTranspose,
  TsCount,
  // Gridline-side (J-33): 0 = J-22 waveguide engine, 1 = reference engine (twinref.h).
  TsEngine = TsCount, TsParamCount
};
enum TsModField { TmSource, TmAmount, TmNegative, TmDest, TmStride };
inline constexpr int tsMod(int slot, int field) { return TsMod1 + slot * TmStride + field; }
enum TsModSource { MsNone, MsEnv, MsLfo, MsModWheel, MsPitchWheel, MsPressure, MsVelocity, MsKey, MsRelease, MsCount };
enum TsModDest {
  MdLfoDepth, MdLevel, MdPan, MdPitch, MdLoCut, MdHiCut, MdHiAtk, MdHiDec, MdS1Decay, MdS2Decay, MdBothDecay,
  MdS1Damp, MdS2Damp, MdBothDamp, MdNoiseRate, MdCount
};
inline constexpr int kTsRatios = 7;     // string 2 ratio steps (TsS2Ratio raw 0..6)
inline constexpr int kTsDlySyncs = 10;  // delay sync steps (TsDlySync raw 0..9, 0 = free)

// --------------------------------------------------------------- Plucker
enum PkParam { PkDecay = 0, PkColor, PkNormalize, PkGate, PkWiden, PkCount };

// ------------------------------------------------------------- Step Bass
// Patch parameters: index = the reference's patch float index 0..40 (= its
// control tag; 0 is the tuning wheel). Then Gridline-side globals (selected
// program, sequencer on, sequence root key, tempo source, own tempo).
enum SbParam {
  SbTune = 0, SbWave, SbCutoff, SbKeyFollow, SbReso, SbEnvMod, SbDecay, SbAccent, SbVolume, SbHighPass,
  SbMinDecay = 10, SbPulseWidth, SbLfoRate, SbLfoPw,
  SbRevOn = 14, SbRevLowCut, SbRevHiCut, SbRevPredelay, SbRevPreFb, SbRevSize, SbRevDiffusion, SbRevDecay,
  SbRevHiDamp, SbRevWidth, SbRevDry, SbRevEr, SbRevWet,
  SbDistOn = 27, SbDistHp, SbDistDrive, SbDistTone, SbDistVolume,
  SbDlyOn = 32, SbDlySync, SbDlyAmount, SbDlyTime, SbDlyRightTap, SbDlyFeedback, SbDlyTone, SbDlyStereo,
  SbClicks = 40, SbClassicPulse,
  SbPatchCount = 42,
  SbProgram = SbPatchCount, SbSeqOn, SbRoot, SbTempoMode, SbBpm,
  SbCount
};
inline constexpr int kSbPrograms = 128;
inline constexpr int kSbSteps = 32;
inline constexpr int kSbPatchFloats = 100;
inline constexpr int kSbDelayDivs = 8;  // delay sync steps (1/64 .. 3/16)

}  // namespace gl::packs::models::strs
