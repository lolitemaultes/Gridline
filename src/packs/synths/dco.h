// Gridline — "synths" pack, J-22 group "dco": parameter layouts of
//   gl.dcosynth  "DCO Synth"   two digitally-controlled oscillators (pulse /
//                              saw / sub each), cross-mod, AM, high-pass +
//                              4-slope ladder low-pass, two envelopes, two
//                              LFOs, an 8-slot matrix, arpeggiator, BBD chorus
//                              and an FX chain (EQ, delay, reverb, saturation,
//                              ensemble). A "Panel" switch picks the two-DCO
//                              or the classic one-DCO front panel / voicing.
//   gl.warmsynth "Warm Trio"   three oscillators (5 shapes, octave buttons,
//                              warm switch, ring 1×2), an LFO, a state-variable
//                              filter with low / band / high outputs and its
//                              own envelope, an amp envelope, chorus.
//   gl.fmduo     "FM Duo"      sine carrier with two parallel modulators, each
//                              with an init → sustain envelope, waveform
//                              shaping, modulator thru, vibrato.
// Parameter indices follow the reference instruments' control tags so that
// recorded automation maps one-to-one (see FlpJ22Dco.cpp).
#pragma once

namespace gl::packs::synths::dco {

// ================================================================ DCO Synth
// Indices 0..154 are the reference's control tags (values 0..1 like the
// reference stores them, switches as choice indices).
enum DcoParam {
  DcLfo1ToDco1, DcLfo1ToDco1Wide, DcLfo2ToDco1, DcLfo2ToDco1Wide, DcEnv2ToDco1, DcDco1Pw, DcDco1PwSrc,
  DcDco1Pulse, DcDco1Saw, DcDco1Sub, DcDco1SubLevel, DcDco1SubPhase,
  DcDco2On, DcDco2Semi, DcDco2Detune, DcDco2Saw, DcDco2Sub, DcDco2SubLevel, DcDco2SubPhase, DcDco2Pw,
  DcLfo1ToDco2, DcLfo1ToDco2Wide, DcLfo2ToDco2, DcLfo2ToDco2Wide, DcEnv2ToDco2, DcDco2PwSrc, DcDco2Pulse,
  DcSync, DcXmod12, DcXmod21, DcXmod12Shape, DcXmod21Shape, DcDco1Level, DcDco2Level, DcNoise, DcAm,
  DcHpf, DcCutoff, DcReso, DcKeyFollow, DcSlope, DcFilterFm, DcFilterFmShape, DcEnvPolarity, DcEnv1ToCut,
  DcLfo1ToCut, DcLfo2ToCut, DcVcaSrc,
  DcLfo1Rate, DcLfo1Sync, DcLfo1Delay, DcLfo1Trig, DcLfo1PhaseOn, DcLfo1Phase,
  DcLfo2Rate, DcLfo2Sync, DcLfo2Delay, DcLfo2Trig, DcLfo2Shape, DcLfo2PhaseOn, DcLfo2Phase,
  DcEnv1A, DcEnv1D, DcEnv1S, DcEnv1R, DcEnv2A, DcEnv2D, DcEnv2S, DcEnv2R,
  DcChorus, DcChorusNoise, DcChorusGate,
  DcVolume, DcVelVol, DcPan, DcMono, DcLegato, DcRetrig, DcRelease, DcPortaMode, DcPorta, DcLfoTrigger,
  DcHold, DcOctave, DcBend,
  DcArpOn, DcArpRate, DcArpSync, DcArpRange, DcArpMode, DcArpGate, DcArpVariant,
  DcMxAmt1,  // 8 matrix amounts (0.5 = none)
  DcEqOn = DcMxAmt1 + 8, DcEqQLo, DcEqQMid, DcEqQHi, DcEqGainLo, DcEqGainMid, DcEqGainHi, DcEqFreqLo, DcEqFreqMid,
  DcEqFreqHi,
  DcDlyOn, DcDlySync, DcDlyInput, DcDlyTime, DcDlyRightTap, DcDlyFb, DcDlyFbCut, DcDlyFbType, DcDlyStereo, DcDlyBlur,
  DcDlyTone,
  DcRevOn, DcRevLowCut, DcRevHighCut, DcRevPredelay, DcRevPreFb, DcRevSize, DcRevDiffusion, DcRevDecay, DcRevDamp,
  DcRevWidth, DcRevDry, DcRevEarly, DcRevWet,
  DcSatOn, DcSatAmount, DcSatPost,
  DcEnsOn, DcEnsDelay, DcEnsFb, DcEnsRate, DcEnsDepth, DcEnsMove, DcEnsMoveRate, DcEnsLines, DcEnsLowPass,
  DcEnsHighPass, DcEnsMix, DcEnsPhase,
  DcXOffset, DcYOffset,
  DcVcaSmooth, DcSatHq, DcMasterTune, DcHpfOn,  // 151..154 (not stored by the reference)
  DcMxSrc1,                                      // 8 matrix sources (choice, 0 = none)
  DcMxDst1 = DcMxSrc1 + 8,                       // 8 matrix destinations (choice, 0 = none)
  DcPanel = DcMxDst1 + 8,                        // 0 two-DCO panel, 1 classic one-DCO panel
  DcArpQuantize,                                 // classic panel option: first arpeggio step on the beat
  DcModel,                                       // J-33: 0 = J-22 engine (older states), 1 = reference laws
  DcCount
};
static_assert(DcLfo1Rate == 48 && DcEnv1A == 61 && DcVolume == 72 && DcArpOn == 85 && DcEqOn == 100 &&
                  DcDlyOn == 110 && DcRevOn == 121 && DcSatOn == 134 && DcEnsOn == 137 && DcXOffset == 149 &&
                  DcHpfOn == 154,
              "DCO Synth indices must equal the reference control tags");
inline constexpr int kDcTags = 155;
inline constexpr int kDcMatrix = 8;
inline constexpr int kDcSources = 24;  // matrix sources (choice index 1..24)
// Matrix destinations: choice 0 = none, 1..155 = parameter tag + 1, then the two pitches.
inline constexpr int kDcDestDco1Pitch = kDcTags + 1, kDcDestDco2Pitch = kDcTags + 2, kDcDestCount = kDcTags + 3;
enum DcPanelKind { PanelTwo, PanelOne };

// ================================================================ Warm Trio
// Indices 0..57 are the reference's control tags.
enum WarmParam {
  WmOsc1Level, WmOsc1Lfo, WmOsc1Env, WmOsc2Level, WmOsc2Lfo, WmOsc2Env, WmOsc3Level, WmOsc3Lfo, WmOsc3Env,
  WmLfoDelay, WmLfoRate, WmCut, WmEmph, WmSvfEnv, WmSvfLfo, WmSvfHigh, WmSvfBand,
  WmAmpA, WmAmpD, WmAmpS, WmAmpR, WmVcaVel, WmSvfA, WmSvfD, WmSvfS, WmSvfR,
  WmOsc1Pw, WmOsc1Coarse, WmOsc1Fine, WmOsc2Pw, WmOsc2Coarse, WmOsc2Fine, WmOsc3Pw, WmOsc3Coarse, WmOsc3Fine,
  WmSvfKb,
  WmChorus, WmRetrig, WmTrackAmp, WmLfoOn, WmSvfOn, WmOsc1On, WmOsc2On, WmOsc3On, WmRing,
  WmOsc1Oct1, WmOsc1Oct2, WmOsc1Warm, WmOsc2Oct1, WmOsc2Oct2, WmOsc2Warm, WmOsc3Oct1, WmOsc3Oct2, WmOsc3Warm,
  WmOsc1Shape, WmOsc2Shape, WmOsc3Shape, WmLfoShape,
  WmCount
};
static_assert(WmSvfKb == 35 && WmChorus == 36 && WmOsc1Shape == 54 && WmLfoShape == 57, "Warm Trio tags");
// Oscillator shapes in the reference's selector order.
enum WarmShape { WsPulse, WsSaw, WsTriangle, WsNoise, WsSine, WsCount };
enum WarmLfoShape { WlSquare, WlRamp, WlTriangle, WlRandom, WlCount };

// ================================================================== FM Duo
// Indices 0..21 are the reference's control tags (0..1; octave −3..3).
enum DuoParam {
  FdAttack, FdDecay, FdRelease, FdM1Coarse, FdM1Fine, FdM1Init, FdM1Time, FdM1Sustain, FdM1Release, FdM1Vel,
  FdVibrato, FdWaveform, FdModThru, FdLfoRate, FdM2Coarse, FdM2Fine, FdM2Init, FdM2Time, FdM2Sustain, FdM2Release,
  FdM2Vel, FdOctave,
  FdCount
};

}  // namespace gl::packs::synths::dco
