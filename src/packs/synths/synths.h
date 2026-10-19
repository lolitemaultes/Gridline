// Gridline — "synths" pack: parameter layouts shared by info, DSP and editors.
//   gl.analog     "Analog"     three-oscillator subtractive synth with unison,
//                              multimode filter, three envelopes, two LFOs, a
//                              small modulation matrix, arpeggiator and FX.
//   gl.wavetable  "Wavetable"  two morphing wavetable oscillators (built-in
//                              procedural tables, warp, unison up to 16), two
//                              filters, four envelopes, four LFOs, an 8-slot
//                              modulation matrix, four macros and FX.
#pragma once

namespace gl::packs::synths {

// ------------------------------------------------------------- shared lists
// Oscillator waves of Analog.
enum AnWave { WaveSine, WaveTriangle, WaveSaw, WavePulse, WaveNoise, WaveCount };
// Filter modes shared by both synths.
enum FilterMode { FltLp12, FltLp24, FltBp12, FltBp24, FltHp12, FltHp24, FltNotch, FltPeak, FltModeCount };
// LFO shapes.
enum LfoShape { LfoSine, LfoTriangle, LfoSawUp, LfoSawDown, LfoSquare, LfoSampleHold, LfoSmooth, LfoShapeCount };
// Tempo divisions for LFOs, arpeggiator and delay (beats per cycle in kDivBeats).
enum Division { Div1_32, Div1_16T, Div1_16, Div1_8T, Div1_16D, Div1_8, Div1_4T, Div1_8D, Div1_4, Div1_4D, Div1_2, Div1_1, Div2_1, Div4_1, DivCount };
inline constexpr double kDivBeats[DivCount] = {0.125, 1.0 / 6.0, 0.25, 1.0 / 3.0, 0.375, 0.5, 2.0 / 3.0, 0.75, 1.0, 1.5, 2.0, 4.0, 8.0, 16.0};
// Voice modes.
enum PlayMode { PlayPoly, PlayMono, PlayLegato };

// Modulation destinations of Analog (LFO / mod envelope / matrix).
enum AnDest {
  AdNone, AdPitch, AdOsc1Pitch, AdOsc2Pitch, AdOsc3Pitch, AdPulseWidth, AdOscMod, AdCutoff, AdResonance,
  AdVolume, AdPan, AdDetune, AdLfo1Rate, AdLfo2Rate, AdNoise, AdDestCount
};
// Modulation sources of Analog's matrix.
enum AnSource { AsNone, AsLfo1, AsLfo2, AsModEnv, AsFilterEnv, AsAmpEnv, AsVelocity, AsKey, AsRandom, AsSourceCount };

// ---------------------------------------------------------------- Analog
inline constexpr int kAnOscCount = 3;
enum AnOscField { OscOn, OscWave, OscCoarse, OscFine, OscLevel, OscPw, OscMod, OscModAmt, OscStride };
enum AnOscModKind { ModNone, ModSync, ModRing, ModFm, ModKindCount };
enum AnLfoField { LfoShapeP, LfoRate, LfoSync, LfoDiv, LfoRetrig, LfoDelay, LfoDest, LfoAmt, LfoStride };
enum ArpMode { ArpUp, ArpDown, ArpUpDown, ArpRandom, ArpPlayed, ArpModeCount };
inline constexpr int kAnMatrixRows = 4;

enum AnalogParam {
  AnVolume, AnPan, AnTranspose, AnFine, AnVoices, AnPlayMode, AnGlide, AnGlideMode, AnVelAmp, AnVelFilter,
  AnUniVoices, AnUniDetune, AnUniSpread,
  AnOsc1,                                // 3 × OscStride
  AnSubLevel = AnOsc1 + kAnOscCount * OscStride, AnSubOct, AnSubWave, AnNoise,
  AnFltMode, AnCutoff, AnReso, AnDrive, AnKeyTrack, AnFltEnv,
  AnAmpA, AnAmpD, AnAmpS, AnAmpR,
  AnFenA, AnFenD, AnFenS, AnFenR,
  AnModA, AnModD, AnModS, AnModR, AnModDest, AnModAmt,
  AnLfo1,                                // 2 × LfoStride
  AnMx1Src = AnLfo1 + 2 * LfoStride,     // kAnMatrixRows × (src, dest, amount)
  AnArpOn = AnMx1Src + kAnMatrixRows * 3, AnArpMode, AnArpOct, AnArpDiv, AnArpGate,
  AnFxDrive, AnChoOn, AnChoRate, AnChoDepth, AnChoMix, AnDlyOn, AnDlyDiv, AnDlyFb, AnDlyMix,
  AnRevOn, AnRevSize, AnRevDamp, AnRevMix,
  AnCount
};
inline constexpr int anOsc(int k, int field) { return AnOsc1 + k * OscStride + field; }
inline constexpr int anLfo(int k, int field) { return AnLfo1 + k * LfoStride + field; }
inline constexpr int anMx(int row, int field) { return AnMx1Src + row * 3 + field; }  // field 0 src, 1 dest, 2 amount

// ------------------------------------------------------------- Wavetable
enum WtTable { TblAnalog, TblPulse, TblFormant, TblHarmonic, TblSync, TblFm, TblReso, TblOddEven, TblDigital, TblOrgan, TblCount };
enum WtWarp { WarpOff, WarpBendPlus, WarpBendMinus, WarpSync, WarpMirror, WarpCrush, WarpFm, WarpCount };
enum WtOscField { WoOn, WoTable, WoPos, WoWarp, WoWarpAmt, WoCoarse, WoFine, WoLevel, WoPan, WoUni, WoDetune, WoBlend, WoWidth, WoPhase, WoRandPhase, WoStride };
enum WtFilterField { WfOn, WfType, WfCutoff, WfReso, WfDrive, WfKeyTrack, WfStride };
enum WtLfoField { WlShape, WlRate, WlSync, WlDiv, WlRetrig, WlStride };
enum WtRouting { RouteSerial, RouteParallel, RouteCount };
inline constexpr int kWtMatrixRows = 8;
inline constexpr int kWtMaxUnison = 16;

enum WtDest {
  WdNone, WdPitch, WdAPitch, WdBPitch, WdAPos, WdBPos, WdAWarp, WdBWarp, WdALevel, WdBLevel, WdADetune, WdBDetune,
  WdF1Cutoff, WdF1Reso, WdF2Cutoff, WdF2Reso, WdSubLevel, WdNoiseLevel, WdVolume, WdPan, WdLfo1Rate, WdLfo2Rate, WdDestCount
};
enum WtSource {
  WsNone, WsEnv2, WsEnv3, WsEnv4, WsLfo1, WsLfo2, WsLfo3, WsLfo4, WsVelocity, WsKey, WsRandom,
  WsMacro1, WsMacro2, WsMacro3, WsMacro4, WsSourceCount
};

enum WavetableParam {
  WtVolume, WtTranspose, WtVoices, WtPlayMode, WtGlide, WtVelAmp,
  WtOscA,                                    // 2 × WoStride
  WtSubLevel = WtOscA + 2 * WoStride, WtSubOct, WtSubWave, WtNoiseLevel, WtNoiseColor,
  WtFlt1,                                    // 2 × WfStride
  WtRouting = WtFlt1 + 2 * WfStride,
  WtEnv1,                                    // 4 × (A, D, S, R)
  WtLfo1 = WtEnv1 + 16,                      // 4 × WlStride
  WtMx1Src = WtLfo1 + 4 * WlStride,          // kWtMatrixRows × (src, dest, amount)
  WtMacro1 = WtMx1Src + kWtMatrixRows * 3, WtMacro2, WtMacro3, WtMacro4,
  WtFxDrive, WtChoOn, WtChoRate, WtChoDepth, WtChoMix, WtDlyOn, WtDlyDiv, WtDlyFb, WtDlyMix,
  WtRevOn, WtRevSize, WtRevDamp, WtRevMix,
  WtCount
};
inline constexpr int wtOsc(int k, int field) { return WtOscA + k * WoStride + field; }
inline constexpr int wtFlt(int k, int field) { return WtFlt1 + k * WfStride + field; }
inline constexpr int wtEnv(int k, int stage) { return WtEnv1 + k * 4 + stage; }
inline constexpr int wtLfo(int k, int field) { return WtLfo1 + k * WlStride + field; }
inline constexpr int wtMx(int row, int field) { return WtMx1Src + row * 3 + field; }

// FX block offsets (both synths keep the 13 FX parameters in this order).
enum FxField { FxDrive, FxChoOn, FxChoRate, FxChoDepth, FxChoMix, FxDlyOn, FxDlyDiv, FxDlyFb, FxDlyMix, FxRevOn, FxRevSize, FxRevDamp, FxRevMix, FxCount };
static_assert(AnRevMix - AnFxDrive == FxRevMix, "Analog FX block layout");
static_assert(WtRevMix - WtFxDrive == FxRevMix, "Wavetable FX block layout");

// Wavetable data shared by DSP and the editor's display (built once, lazily).
inline constexpr int kWtSize = 2048;      // samples per frame
inline constexpr int kWtFrames = 8;       // morph frames per table
inline constexpr int kWtLevels = 11;      // mip levels: level L keeps 1024 >> L harmonics
const float* wavetableFrame(int table, int frame, int level);  // kWtSize samples
const char* const* wavetableNames();                            // TblCount names

}  // namespace gl::packs::synths
