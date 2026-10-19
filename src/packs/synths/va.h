// Gridline — "synths" pack, J-22 group "va": parameter layouts of the
// reference-architecture synths (shared by info, DSP, editors and the FLP
// decoder). Parameter index = the reference plugin's control tag, so FL
// automation maps 1:1; every value is linear in the reference control's
// normalized position (the DSP applies the control curves).
//   gl.twinva      "Twin VA"      two-oscillator VA: saw / pulse oscillators with
//                                 sync / ring, noise, 3-mode filter, routable
//                                 envelope and LFO, unison, 16-step trance gate,
//                                 arpeggiator, octaver, glide, delay, chorus.
//   gl.sawswarm    "Saw Swarm"    super-saw synth: one saw stack (unison 1..8)
//                                 plus a sub-saw at a free octave offset, hard
//                                 sync, noise, 4-mode filter, filter envelope,
//                                 LFO, chorus / phaser / delay / reverb,
//                                 arpeggiator, 6-slot MIDI modulation matrix.
//   gl.fingerbass  "Finger Bass"  plucked bass-guitar model with a three-band
//                                 tone stack.
#pragma once

namespace gl::packs::synths::va {

// --------------------------------------------------------------- Twin VA
enum TwinParam {
  TwAShape, TwAPw, TwSync, TwRing, TwBShape, TwBPw, TwBPitch, TwBDetune, TwBalance, TwNoise,
  TwEgAmt, TwEgA, TwEgD, TwEgS, TwEgR,
  TwGain, TwFine, TwTranspose, TwCutoff, TwReso, TwKeyTrk, TwVelTrk,
  TwAmpA, TwAmpD, TwAmpS, TwAmpR,
  TwLfoAmt, TwLfoRetrig, TwLfoRate, TwUniDetune, TwUniPan,
  TwDlyOn, TwDlyL, TwDlyR, TwDlyFb, TwDlyDepth, TwDlyLoCut, TwDlyHiCut, TwDlyWet,
  TwChoOn, TwChoRate, TwChoDepth, TwChoWet,
  TwMwAmt, TwMono, TwOctaver, TwGlide, TwGateMix, TwGateSmooth,
  TwGate1,  // 16 steps
  TwEgDest = TwGate1 + 16, TwFltMode, TwLfoDest, TwLfoShape, TwArpMode, TwArpRange, TwMwDest, TwGlideHeld, TwGlideStatic,
  TwModWheel,  // tag 74 is unused by the reference; Gridline's mod wheel position
  TwUnison, TwPoly, TwArpSync, TwBendRange, TwGateSync, TwLfoSync,
  TwCount
};
inline constexpr int kTwGateSteps = 16;
// Destinations of the envelope (first 5) and the LFO (6); the mod wheel's own list.
enum TwDest { TdCutoff, TdFreqA, TdFreqAB, TdPwA, TdPwAB, TdPan, TdCount };
enum TwWheelDest { TmCutoff, TmLfo, TmFreqA, TmPwA, TmPwAB, TmCount };
enum TwFilter { TfLow, TfBand, TfHigh };
enum TwLfoShapeE { TlSaw, TlPulse, TlTriangle, TlSine, TlRandom, TlShapeCount };
enum TwArp { TaOff, TaUp, TaDown, TaUpDown, TaRandom };
inline constexpr int kTwSyncDivs = 11;  // LFO sync 0 = free, 1..10 divisions

// ------------------------------------------------------------- Saw Swarm
enum SwarmParam {
  SwLevel, SwPan, SwAmpA, SwAmpD, SwAmpS, SwAmpR, SwTranspose, SwFine, SwMono, SwGlideStatic, SwGlideHeld, SwGlide,
  SwOctaver, SwUniRetrig, SwUnison, SwUniDetune, SwUniPan,
  SwSubPhase, SwSubOct, SwSubLevel, SwSubDetune, SwSubRetrig, SwSubInvert, SwSubSquare,
  SwSyncFreq, SwSyncOn, SwNoise,
  SwCutoff, SwReso, SwKeyTrk, SwVelTrk, SwFltMode,
  SwEgA, SwEgD, SwEgS, SwEgR, SwEgAmt, SwEgDest,
  SwLfoShape, SwLfoSpeed, SwLfoSync, SwLfoAttack, SwLfoRelease, SwLfoRetrig, SwLfoInvert, SwLfoAmt, SwLfoDest,
  SwChoOn, SwChoDepth, SwChoRate, SwChoMix,
  SwPhOn, SwPhCenter, SwPhRate, SwPhDepth, SwPhFb, SwPhMix,
  SwDlyOn, SwDlyL, SwDlyR, SwDlyFb, SwDlyDepth, SwDlyLoCut, SwDlyHiCut, SwDlyMix,
  SwRevOn, SwRevDecay, SwRevHiCut, SwRevDamp, SwRevMix,
  SwArpOn, SwArpMode, SwArpRange,
  SwModWheel,  // tag 73 is unused by the reference; Gridline's mod wheel position
  SwArpSync, SwPoly, SwBendRange, SwSpare,
  SwModAmt1,              // 6 amounts
  SwModDest1 = SwModAmt1 + 6,  // 6 destinations (parameter index)
  SwModSrc1 = SwModDest1 + 6,  // 6 sources
  SwCount = SwModSrc1 + 6
};
inline constexpr int kSwModSlots = 6;
enum SwEgDestE { SeCutoff, SeSyncFreq, SeSubPhase, SeMasterFreq, SeCount };
enum SwLfoDestE { SlCutoff, SlSyncFreq, SlSubPhase, SlMasterFreq, SlNoise, SlLevel, SlPan, SlCount };
enum SwLfoShapeE { SsTriangle, SsSaw, SsSquare, SsRandom };
enum SwFilter { SfLp24, SfLp12, SfBp, SfHp };
enum SwArp { SaUp, SaDown, SaUpDown, SaRandom };
// MIDI modulation sources: none, velocity, aftertouch, pitch wheel, then
// controllers (kSwCc lists their numbers).
inline constexpr int kSwCcCount = 50;
inline constexpr int kSwSources = 4 + kSwCcCount;
inline constexpr int kSwCc[kSwCcCount] = {0,  1,  2,  4,  5,  6,  7,  8,  10, 11, 12, 13, 16, 17, 18, 19, 32,
                                          64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80,
                                          81, 82, 83, 84, 91, 92, 93, 94, 95, 96, 97, 120, 121, 123, 126, 127};
inline constexpr int kSwPolyChoices = 6;
inline constexpr int kSwPoly[kSwPolyChoices] = {4, 8, 12, 16, 20, 24};
// Arpeggiator step (16ths) and pitch-bend range (semitones) of the combo items.
inline constexpr int kSwArpSteps = 7;
inline constexpr double kSwArpStep16[kSwArpSteps] = {0.5, 1, 2, 3, 4, 8, 12};
inline constexpr int kSwBendSteps = 12;
inline constexpr int kSwBend[kSwBendSteps] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12};

// ----------------------------------------------------------- Finger Bass
enum BassParam { BbBass, BbMid, BbTreble, BbCount };

}  // namespace gl::packs::synths::va
